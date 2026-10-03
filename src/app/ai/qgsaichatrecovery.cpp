/***************************************************************************
  qgsaichatrecovery.cpp — resumable provider rounds (not tool replay)
  SPDX-License-Identifier: GPL-2.0-or-later
 ***************************************************************************/
#include "qgsaiagentsessionmanager.h"
#include "qgsaifilecontextprovider.h"
#include "qgsaimodelrouter.h"
#include "qgsproject.h"
#include "qgsmaplayer.h"
#include "qgsaitoolregistry.h"
#include "tools/qgsaigeographicverification.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
using namespace Qt::StringLiterals;

namespace
{
  QString accountIdentity( QgsAiModelRouter *router )
  {
    const QString token = router->planSessionToken();
    const QList<QByteArray> parts = token.toUtf8().split( '.' );
    const QString subject = parts.size() == 3 ? QJsonDocument::fromJson( QByteArray::fromBase64( parts[1], QByteArray::Base64UrlEncoding ) ).object().value( u"sub"_s ).toString() : QString();
    return QString::fromLatin1( QCryptographicHash::hash( ( subject.isEmpty() ? token : subject ).toUtf8(), QCryptographicHash::Sha256 ).toHex() );
  }
} //namespace

QVariantMap QgsAiAgentSessionManager::recoveryCheckpoint() const
{
  QString turnId;
  for ( auto it = mHistory.crbegin(); it != mHistory.crend(); ++it )
    if ( it->role == QgsAiChatRole::User )
    {
      turnId = it->id;
      break;
    }
  QVariantList resources;
  for ( auto *layer : QgsProject::instance()->mapLayers() )
    resources << QVariantMap { { u"id"_s, layer->id() }, { u"source_hash"_s, QString::fromLatin1( QCryptographicHash::hash( layer->source().toUtf8(), QCryptographicHash::Sha256 ).toHex() ) } };
  return {
    { u"resources"_s, resources },
    { u"version"_s, 2 },
    { u"credential_scope"_s, mRouter->planCredentialScope() },
    { u"verified_account"_s, mActiveProvider == QgsAiModelRouter::Provider::Plan ? mRouter->cachedPlanAccountId() : QString() },
    { u"turn_id"_s, turnId },
    { u"last_message_id"_s, mHistory.isEmpty() ? QString() : mHistory.last().id },
    { u"provider"_s, static_cast<int>( mActiveProvider ) },
    { u"model"_s, mRouter->providerSettings( mActiveProvider ).model },
    { u"scope"_s, chatHistoryScopeKey() },
    { u"workspace"_s, workspaceRoot() },
    { u"project"_s, QgsProject::instance()->fileName() },
    { u"account"_s, mActiveProvider == QgsAiModelRouter::Provider::Plan ? accountIdentity( mRouter ) : QString() },
    { u"tool_rounds"_s, mTotalToolIterations },
    { u"mode"_s, mActiveAgent }
  };
}

bool QgsAiAgentSessionManager::resumeLastInterruptedTurn( QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return false;
  };
  if ( !mRouter || hasActiveRequest() )
    return fail( tr( "Wait for the active request to finish." ) );
  if ( mHistory.isEmpty() || mHistory.last().metadata.value( u"ui_kind"_s ).toString() != "request_error"_L1 )
    return fail( tr( "There is no interrupted response to resume." ) );
  const QgsAiChatMessage failure = mHistory.last();
  const qint64 retryAt = failure.metadata.value( u"retry_at_ms"_s ).toLongLong();
  if ( retryAt > QDateTime::currentMSecsSinceEpoch() )
    return fail( tr( "The provider wait ends in %1 seconds." ).arg( ( retryAt - QDateTime::currentMSecsSinceEpoch() + 999 ) / 1000 ) );
  const QVariantMap checkpoint = failure.metadata.value( u"recovery"_s ).toMap();
  const auto provider = mRouter->resolveProvider();
  if ( !mRouter->isProviderUsable( provider ) )
    return fail( tr( "Sign in or configure the selected provider before resuming." ) );
  if ( !checkpoint.isEmpty() )
  {
    if (
      ( checkpoint.value( u"version"_s ).toInt() != 1 && checkpoint.value( u"version"_s ).toInt() != 2 )
      || checkpoint.value( u"provider"_s ).toInt() != static_cast<int>( provider )
      || checkpoint.value( u"model"_s ).toString() != mRouter->providerSettings( provider ).model
      || checkpoint.value( u"mode"_s ).toString() != mActiveAgent
      || checkpoint.value( u"scope"_s ).toString() != chatHistoryScopeKey()
      || checkpoint.value( u"workspace"_s ).toString() != workspaceRoot()
      || checkpoint.value( u"project"_s ).toString() != QgsProject::instance()->fileName()
      || ( provider == QgsAiModelRouter::Provider::Plan && checkpoint.value( u"account"_s ).toString() != accountIdentity( mRouter ) )
    )
      return fail( tr( "Restore this chat's original account, project, mode and model before resuming." ) );
    if ( provider == QgsAiModelRouter::Provider::Plan && checkpoint.value( u"version"_s ).toInt() >= 2 )
    {
      const QString account = mRouter->verifiedPlanAccountId( true );
      if (
        account.isEmpty()
        || ( !checkpoint.value( u"verified_account"_s ).toString().isEmpty() && account != checkpoint.value( u"verified_account"_s ).toString() )
        || mRouter->planCredentialScope() != checkpoint.value( u"credential_scope"_s ).toString()
      )
        return fail( tr( "The original Strata identity could not be verified. Sign in to the same account before resuming." ) );
    }
    if ( mHistory.size() < 2 || checkpoint.value( u"last_message_id"_s ).toString() != mHistory.at( mHistory.size() - 2 ).id )
      return fail( tr( "The saved recovery checkpoint is incomplete. Review the last operation before continuing." ) );
  }
  else if ( failure.metadata.value( u"error_code"_s ).toString() != "remote_content_not_allowed"_L1 || failure.metadata.value( u"error_provider"_s ).toString() != mRouter->providerDisplayName( provider ) )
    return fail( tr( "This older interruption has no recovery checkpoint." ) );

  for ( const auto &value : checkpoint.value( u"resources"_s ).toList() )
  {
    const auto resource = value.toMap();
    const QString id = resource.value( u"id"_s ).toString();
    auto *layer = QgsProject::instance()->mapLayer( id );
    if ( !layer )
      return fail( tr( "Original resource %1 is missing. Restore the original project before resuming." ).arg( id ) );
    const QString current = QString::fromLatin1( QCryptographicHash::hash( layer->source().toUtf8(), QCryptographicHash::Sha256 ).toHex() );
    if ( current != resource.value( u"source_hash"_s ).toString() && !qgsAiVerifyLayerQuality( layer, QgsProject::instance() ).value( u"passed"_s ).toBool() )
      return fail( tr( "Resource %1 has a changed source that fails geographic verification." ).arg( layer->name() ) );
  }

  // Every tool call in this turn must have exactly one completed result.
  QSet<QString> pending;
  QSet<QString> completed;
  QMap<QString, QString> names;
  QList<QgsAiChatMessage> evidence;
  int start = mHistory.size() - 2;
  while ( start >= 0 && mHistory.at( start ).role != QgsAiChatRole::User )
    --start;
  if ( start < 0 )
    return fail( tr( "The original user message is missing." ) );
  int rounds = 0;
  for ( int i = start + 1; i < mHistory.size() - 1; ++i )
  {
    const auto &message = mHistory.at( i );
    const QVariantList calls = message.metadata.value( u"tool_calls"_s ).toList();
    if ( !calls.isEmpty() )
      ++rounds;
    for ( const QVariant &call : calls )
    {
      const QString id = call.toMap().value( u"id"_s ).toString();
      if ( id.isEmpty() || pending.contains( id ) || completed.contains( id ) )
        return fail( tr( "Tool history is ambiguous; verify the operation first." ) );
      pending.insert( id );
      names.insert( id, call.toMap().value( u"name"_s ).toString() );
    }
    if ( message.role == QgsAiChatRole::Tool )
    {
      const QString id = message.metadata.value( u"tool_call_id"_s ).toString();
      const QString toolName = message.metadata.value( u"tool_name"_s ).toString();
      const QJsonObject result = QJsonDocument::fromJson( message.content.toUtf8() ).object();
      const QString layerId = message.metadata.value( u"resource_layer_id"_s, result.value( u"layer_id"_s ).toString() ).toString();
      const QString resource = layerId.isEmpty() ? tr( "unknown resource" ) : layerId;
      if ( !pending.remove( id ) || message.metadata.value( u"undo_status"_s ).toString() == "undone"_L1 )
        return fail( tr( "%1 — %2: result missing, duplicated or undone; verify effects before continuing." ).arg( toolName, resource ) );
      if ( message.metadata.value( u"is_error"_s ).toBool() )
      {
        const auto *tool = mToolRegistry ? mToolRegistry->find( toolName ) : nullptr;
        const bool readOnly = message.metadata.value( u"effects_state"_s ).toString() == "read_only"_L1 || ( tool && !tool->requiresApproval() && tool->riskLevel() == QgsAiToolRiskLevel::Low );
        const bool canceled = message.metadata.value( u"execution_state"_s ).toString() == "canceled"_L1;
        if ( canceled )
          return fail( tr( "%1 — %2: operation canceled; inspect its effects." ).arg( toolName, resource ) );
        if ( !readOnly )
        {
          QJsonArray resources = result.value( u"geographic_verifications"_s ).toArray();
          if ( resources.isEmpty() )
            resources.append( QJsonObject { { u"layer_id"_s, layerId }, { u"quality_checks"_s, result.value( u"quality_checks"_s ) } } );
          for ( const auto &resourceValue : resources )
          {
            const auto geographicResource = resourceValue.toObject();
            const QString checkedLayerId = geographicResource.value( u"layer_id"_s ).toString();
            const QString checkedResource = checkedLayerId.isEmpty() ? resource : checkedLayerId;
            const QJsonObject original = geographicResource.value( u"quality_checks"_s ).toObject();
            auto *layer = QgsProject::instance()->mapLayer( checkedLayerId );
            if ( !layer || original.isEmpty() )
              return fail( tr( "%1 — %2: effects are not confirmed; a completed Python call is not geographic verification." ).arg( toolName, checkedResource ) );
            bool canceledCheck = false;
            const QJsonObject checks = qgsAiVerifyLayerQuality( layer, QgsProject::instance(), &canceledCheck );
            QStringList failed;
            for ( auto it = checks.begin(); it != checks.end(); ++it )
              if ( it.key() != "passed"_L1 && ( it.value().isNull() || ( it.value().isBool() && !it.value().toBool() ) ) )
                failed << it.key();
            for ( auto it = original.begin(); it != original.end(); ++it )
              if ( it.key() != "passed"_L1 && it.value().isBool() && !it.value().toBool() && !checks.contains( it.key() ) )
                failed << it.key();
            if ( canceledCheck || !checks.value( u"passed"_s ).toBool() || !failed.isEmpty() )
              return fail( tr( "%1 — %2: verification still requires attention (%3)." ).arg( toolName, checkedResource, failed.join( ", "_L1 ) ) );
            auto note = buildAssistantMessage( tr( "Verified after correction: %1 — %2. Original tool call: %3. No operation was replayed." ).arg( toolName, layer->name(), id ) );
            note.metadata = { { u"ui_kind"_s, u"verification_evidence"_s }, { u"original_tool_call_id"_s, id }, { u"layer_id"_s, checkedLayerId }, { u"quality_checks"_s, checks.toVariantMap() } };
            evidence << note;
          }
        }
      }
      completed.insert( id );
    }
  }
  if ( !pending.isEmpty() )
    return fail( tr( "%1: no confirmed result; verify the resource and effects before continuing." ).arg( names.value( *pending.begin() ) ) );
  for ( const auto &note : evidence )
    recordHistoryMessage( note );
  mTotalToolIterations = std::max( rounds, checkpoint.value( u"tool_rounds"_s ).toInt() );
  mToolIterations = mTotalToolIterations;
  mToolRunCanceled = false;
  mEmptyErrorRecoveryAttempted = false;
  mPendingProviders.clear();
  mActiveProvider = provider;
  emit requestRunningChanged( true );
  startProviderAttempt( provider );
  return true;
}

QString QgsAiAgentSessionManager::interruptedToolSummary() const
{
  QStringList applied, unverified;
  for ( auto it = mHistory.crbegin(); it != mHistory.crend() && it->role != QgsAiChatRole::User; ++it )
  {
    if ( it->role != QgsAiChatRole::Tool )
      continue;
    const QString name = it->metadata.value( u"tool_name"_s ).toString();
    if ( it->metadata.value( u"effects_state"_s ).toString() == "applied"_L1 )
      applied.prepend( name );
    if ( it->metadata.value( u"verification_state"_s ).toString() != "passed"_L1 )
      unverified.prepend( name );
  }
  if ( applied.isEmpty() && unverified.isEmpty() )
    return {};
  return tr( "\n\nOperations applied: %1.\nChecks pending or failed: %2.\nAI response interrupted; the complete request has not been verified." )
    .arg( applied.isEmpty() ? tr( "none confirmed" ) : applied.join( ", "_L1 ), unverified.isEmpty() ? tr( "none recorded" ) : unverified.join( ", "_L1 ) );
}
