// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaiagentsessionmanager.h"
#include "qgsaimodelrouter.h"
#include "qgsproject.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
using namespace Qt::StringLiterals;

QString QgsAiAgentSessionManager::recoveryJournalPath() const
{
  if ( !mRouter || workspaceRoot().isEmpty() )
    return {};
  const QString binding = workspaceRoot() + '\n' + QgsProject::instance()->fileName() + '\n' + mRouter->planCredentialScope();
  const QString digest = QString::fromLatin1( QCryptographicHash::hash( binding.toUtf8(), QCryptographicHash::Sha256 ).toHex() );
  return QStandardPaths::writableLocation( QStandardPaths::AppLocalDataLocation ) + u"/ai-recovery/"_s + digest + u".json"_s;
}

void QgsAiAgentSessionManager::persistRecoveryJournal() const
{
  // Kept outside the indexed workspace: this local recovery journal is never retrieval input.
  if ( mActiveProvider != QgsAiModelRouter::Provider::Plan || !mHistoryStore || mHistory.isEmpty() || mHistory.last().metadata.value( u"ui_kind"_s ) != u"request_error"_s )
    return;
  const QString path = recoveryJournalPath();
  if ( path.isEmpty() )
    return;
  QJsonArray messages;
  for ( const auto &message : mHistory )
    messages.append( message.toJson() );
  const QByteArray bytes = QJsonDocument(
                             QJsonObject { { u"version"_s, 1 }, { u"messages"_s, messages }, { u"scope"_s, chatHistoryScopeKey() }, { u"session_id"_s, mActiveSessionId }, { u"mode"_s, mActiveAgent } }
  ).toJson( QJsonDocument::Compact );
  if ( bytes.size() > 8 * 1024 * 1024 )
    return;
  QDir().mkpath( QFileInfo( path ).absolutePath() );
  QSaveFile file( path );
  if ( file.open( QIODevice::WriteOnly ) )
  {
    file.setPermissions( QFileDevice::ReadOwner | QFileDevice::WriteOwner );
    if ( file.write( bytes ) == bytes.size() )
      file.commit();
  }
}

void QgsAiAgentSessionManager::restoreRecoveryJournal()
{
  if ( hasActiveRequest() || !mHistory.isEmpty() || !mHistoryStore )
    return;
  QFile file( recoveryJournalPath() );
  if ( !file.exists() || file.size() > 8 * 1024 * 1024 || !file.open( QIODevice::ReadOnly ) )
    return;
  const auto document = QJsonDocument::fromJson( file.readAll() ).object();
  if ( document.value( u"version"_s ).toInt() != 1 || document.value( u"scope"_s ).toString() != chatHistoryScopeKey() )
    return;
  const auto messages = document.value( u"messages"_s ).toArray();
  if ( messages.isEmpty() )
    return;
  const auto last = QgsAiChatMessage::fromJson( messages.last().toObject() );
  if ( last.metadata.value( u"ui_kind"_s ) != u"request_error"_s )
    return;
  mActiveSessionId = document.value( u"session_id"_s ).toString();
  const QString mode = document.value( u"mode"_s ).toString();
  if ( availableAgents().contains( mode ) )
    mActiveAgent = mode;
  for ( const auto &message : messages )
    mHistory.append( QgsAiChatMessage::fromJson( message.toObject() ) );
  mNextMessageOrdering = mHistory.size();
  emit historyReplaced();
  emit requestStateChanged( u"interrupted"_s, tr( "Saved interruption restored. Review the original project and choose Resume; nothing will run automatically." ) );
}

void QgsAiAgentSessionManager::setHistoryStore( QgsAiChatHistoryStore *store )
{
  mHistoryStore = store;
  QTimer::singleShot( 0, this, &QgsAiAgentSessionManager::restoreRecoveryJournal );
}
