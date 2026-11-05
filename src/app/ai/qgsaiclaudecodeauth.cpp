/***************************************************************************
    qgsaiclaudecodeauth.cpp
    ---------------------
    begin                : September 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsaiclaudecodeauth.h"
#include "qgsapplication.h"
#include "qgssettings.h"
#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVersionNumber>
#include "moc_qgsaiclaudecodeauth.cpp"
using namespace Qt::StringLiterals;

QString QgsAiClaudeCodeAuth::executablePath()
{
  const QString configured = QgsSettings().value( u"ai/provider/claude/code/executable"_s ).toString();
  if ( !configured.isEmpty() )
    return QFileInfo( configured ).isExecutable() ? QFileInfo( configured ).absoluteFilePath() : QString();
  QString found = QStandardPaths::findExecutable( u"claude"_s );
  if ( found.isEmpty() )
    found = QStandardPaths::findExecutable( u"claude"_s, { QDir::homePath() + u"/.local/bin"_s, u"/opt/homebrew/bin"_s, u"/usr/local/bin"_s } );
  return found;
}

QString QgsAiClaudeCodeAuth::configDirectory()
{
  return QDir( QgsApplication::qgisSettingsDirPath() ).absoluteFilePath( u"claude-code"_s );
}

QProcessEnvironment QgsAiClaudeCodeAuth::environment()
{
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  // Never let inherited API/gateway credentials change this subscription route.
  for ( const QString &key : env.keys() )
  {
    if ( key.startsWith( "ANTHROPIC_"_L1 ) || key.startsWith( "CLAUDE_CODE_"_L1 ) || key == "CLAUDE_CONFIG_DIR"_L1 || key == "CLAUDECODE"_L1 )
      env.remove( key );
  }
  env.insert( u"CLAUDE_CONFIG_DIR"_s, configDirectory() );
  env.insert( u"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC"_s, u"1"_s );
  env.insert( u"CLAUDE_CODE_SKIP_PROMPT_HISTORY"_s, u"1"_s );
  return env;
}

bool QgsAiClaudeCodeAuth::isConfigured()
{
  return !executablePath().isEmpty() && QgsSettings().value( u"ai/provider/claude/code/connected"_s, false ).toBool();
}

int QgsAiClaudeCodeAuth::cooldownSeconds()
{
  const qint64 remaining = QgsSettings().value( u"ai/provider/claude/code/retryAt"_s, 0 ).toLongLong() - QDateTime::currentMSecsSinceEpoch();
  return static_cast<int>( std::clamp<qint64>( ( remaining + 999 ) / 1000, 0, 86400 ) );
}
void QgsAiClaudeCodeAuth::setCooldown( int seconds )
{
  QgsSettings().setValue( u"ai/provider/claude/code/retryAt"_s, QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>( std::max( 1, seconds ) ) * 1000 );
}

QgsAiClaudeCodeAuth::QgsAiClaudeCodeAuth( QObject *parent )
  : QObject( parent )
{
  mTimer.setSingleShot( true );
  connect( &mTimer, &QTimer::timeout, this, [this]() { fail( tr( "Claude Code did not finish sign-in in time. Try connecting again." ) ); } );
  connect( &mProcess, &QProcess::readyReadStandardOutput, this, [this]() {
    const QByteArray bytes = mProcess.readAllStandardOutput();
    if ( mOperation == "login"_L1 )
      mRateLimited |= bytes.contains( "429" ) || bytes.toLower().contains( "rate limit" );
    // Login output can contain an authorization URL: never retain or log it.
    if ( mCheckingVersion || mOperation == "status"_L1 )
    {
      mOutput += bytes;
      if ( mOutput.size() > 32768 )
        fail( tr( "Claude Code returned an invalid status response." ) );
    }
  } );
  connect( &mProcess, &QProcess::readyReadStandardError, this, [this]() {
    const QByteArray bytes = mProcess.readAllStandardError();
    if ( mOperation == "login"_L1 )
      mRateLimited |= bytes.contains( "429" ) || bytes.toLower().contains( "rate limit" );
  } );
  connect( &mProcess, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this, [this]( int code, QProcess::ExitStatus ) { finished( code ); } );
  connect( &mProcess, &QProcess::errorOccurred, this, [this]( QProcess::ProcessError error ) {
    if ( error == QProcess::FailedToStart )
      fail( tr( "Claude Code could not start. Check its installation." ) );
  } );
}
QgsAiClaudeCodeAuth::~QgsAiClaudeCodeAuth()
{
  cancel();
}
void QgsAiClaudeCodeAuth::cancel()
{
  mTimer.stop();
  mOperation.clear();
  mOutput.clear();
  if ( mProcess.state() != QProcess::NotRunning )
  {
    mProcess.kill();
    mProcess.waitForFinished( 1000 );
  }
}
void QgsAiClaudeCodeAuth::fail( const QString &message )
{
  cancel();
  emit statusChanged( false, message );
}
void QgsAiClaudeCodeAuth::checkStatus()
{
  begin( u"status"_s );
}
void QgsAiClaudeCodeAuth::login()
{
  begin( u"login"_s );
}
void QgsAiClaudeCodeAuth::logout()
{
  begin( u"logout"_s );
}
void QgsAiClaudeCodeAuth::begin( const QString &operation )
{
  cancel();
  mRateLimited = false;
  if ( operation == "login"_L1 && cooldownSeconds() > 0 )
  {
    emit statusChanged( false, tr( "Wait %1 seconds before trying Claude sign-in again." ).arg( cooldownSeconds() ) );
    return;
  }
  if ( executablePath().isEmpty() )
  {
    emit statusChanged( false, tr( "Install the official Claude Code application, then connect your Claude subscription." ) );
    return;
  }
  const QString directory = configDirectory();
  if ( !QDir().mkpath( directory ) )
  {
    emit statusChanged( false, tr( "Strata could not create its Claude Code profile." ) );
    return;
  }
  QFile::setPermissions( directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner );
  mProcess.setProcessEnvironment( environment() );
  mProcess.setWorkingDirectory( directory );
  mOperation = operation;
  mCheckingVersion = true;
  run( { u"--version"_s } );
}
void QgsAiClaudeCodeAuth::run( const QStringList &arguments )
{
  mOutput.clear();
  mTimer.start( mOperation == "login"_L1 ? 180000 : 15000 );
  mProcess.start( executablePath(), arguments );
}
void QgsAiClaudeCodeAuth::finished( int exitCode )
{
  if ( mOperation.isEmpty() )
    return;
  mTimer.stop();
  if ( mCheckingVersion || mOperation == "status"_L1 )
    mOutput += mProcess.readAllStandardOutput();
  else
    mProcess.readAllStandardOutput();
  if ( mCheckingVersion )
  {
    mCheckingVersion = false;
    const auto match = QRegularExpression( u"^(\\d+\\.\\d+\\.\\d+) \\(Claude Code\\)"_s ).match( QString::fromUtf8( mOutput ).trimmed() );
    if ( exitCode != 0 || !match.hasMatch() || QVersionNumber::fromString( match.captured( 1 ) ) < QVersionNumber( 2, 1, 283 ) )
    {
      fail( tr( "Update the official Claude Code application to version 2.1.283 or later." ) );
      return;
    }
    run( mOperation == "login"_L1 ? QStringList { u"auth"_s, u"login"_s, u"--claudeai"_s } : QStringList { u"auth"_s, mOperation } );
    return;
  }
  if ( mOperation == "login"_L1 )
  {
    if ( exitCode != 0 )
    {
      if ( mRateLimited )
        setCooldown( 60 );
      fail( tr( "Claude Code sign-in was not completed. Return to the official sign-in flow and try again later." ) );
      return;
    }
    mOperation = u"status"_s;
    run( { u"auth"_s, u"status"_s } );
    return;
  }
  bool connected = false;
  if ( mOperation == "status"_L1 )
  {
    const QJsonObject status = QJsonDocument::fromJson( mOutput ).object();
    connected = exitCode == 0 && status.value( u"loggedIn"_s ).toBool() && status.value( u"authMethod"_s ).toString() == "claude.ai"_L1;
  }
  else if ( exitCode != 0 )
  {
    fail( tr( "Claude Code could not sign out. Try again." ) );
    return;
  }
  QgsSettings().setValue( u"ai/provider/claude/code/connected"_s, connected );
  mOperation.clear();
  mOutput.clear();
  emit statusChanged( connected, connected ? tr( "Claude subscription connected. Choose a model and use it in the chat." ) : tr( "Connect your Claude subscription using the official sign-in flow." ) );
}
