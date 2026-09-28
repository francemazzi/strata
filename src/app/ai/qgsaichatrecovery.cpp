/***************************************************************************
  qgsaichatrecovery.cpp — resumable provider rounds (not tool replay)
  SPDX-License-Identifier: GPL-2.0-or-later
 ***************************************************************************/
#include "qgsaiagentsessionmanager.h"
#include "qgsaifilecontextprovider.h"
#include "qgsaimodelrouter.h"
#include "qgsproject.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
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
  return {
    { u"version"_s, 1 },
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
  const QgsAiChatMessage &failure = mHistory.last();
  const QVariantMap checkpoint = failure.metadata.value( u"recovery"_s ).toMap();
  const auto provider = mRouter->resolveProvider();
  if ( !mRouter->isProviderUsable( provider ) )
    return fail( tr( "Sign in or configure the selected provider before resuming." ) );
  if ( !checkpoint.isEmpty() )
  {
    if (
      checkpoint.value( u"version"_s ).toInt() != 1
      || checkpoint.value( u"provider"_s ).toInt() != static_cast<int>( provider )
      || checkpoint.value( u"model"_s ).toString() != mRouter->providerSettings( provider ).model
      || checkpoint.value( u"mode"_s ).toString() != mActiveAgent
      || checkpoint.value( u"scope"_s ).toString() != chatHistoryScopeKey()
      || checkpoint.value( u"workspace"_s ).toString() != workspaceRoot()
      || checkpoint.value( u"project"_s ).toString() != QgsProject::instance()->fileName()
      || ( provider == QgsAiModelRouter::Provider::Plan && checkpoint.value( u"account"_s ).toString() != accountIdentity( mRouter ) )
    )
      return fail( tr( "Restore this chat's original account, project, mode and model before resuming." ) );
    if ( mHistory.size() < 2 || checkpoint.value( u"last_message_id"_s ).toString() != mHistory.at( mHistory.size() - 2 ).id )
      return fail( tr( "The saved recovery checkpoint is incomplete. Review the last operation before continuing." ) );
  }
  else if ( failure.metadata.value( u"error_code"_s ).toString() != "remote_content_not_allowed"_L1 || failure.metadata.value( u"error_provider"_s ).toString() != mRouter->providerDisplayName( provider ) )
    return fail( tr( "This older interruption has no recovery checkpoint." ) );

  // Every tool call in this turn must have exactly one completed result.
  QSet<QString> pending;
  QSet<QString> completed;
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
    }
    if ( message.role == QgsAiChatRole::Tool )
    {
      const QString id = message.metadata.value( u"tool_call_id"_s ).toString();
      if ( !pending.remove( id ) || message.metadata.value( u"undo_status"_s ).toString() == "undone"_L1 || message.metadata.value( u"is_error"_s ).toBool() )
        return fail( tr( "A tool result is missing or was undone. Review it before continuing." ) );
      completed.insert( id );
    }
  }
  if ( !pending.isEmpty() )
    return fail( tr( "A tool has no confirmed result. Verify its outcome before continuing." ) );
  mTotalToolIterations = std::max( rounds, checkpoint.value( u"tool_rounds"_s ).toInt() );
  mToolIterations = mTotalToolIterations;
  mToolRunCanceled = false;
  mEmptyErrorRecoveryAttempted = false;
  mPendingProviders.clear();
  mActiveProvider = provider;
  emit requestRunningChanged( true );
  beginRetrievalThenDispatch( provider );
  return true;
}
