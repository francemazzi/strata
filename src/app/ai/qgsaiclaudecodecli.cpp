/***************************************************************************
    qgsaiclaudecodecli.cpp
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

#include "qgsaiclaudecodecli.h"

#include "qgsaiptysession.h"
#include "qgsmessagelog.h"
#include "qgssettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>

#if defined( Q_OS_WIN )
#include <qt_windows.h>
#endif

#include "moc_qgsaiclaudecodecli.cpp"

using namespace Qt::StringLiterals;

namespace
{
  constexpr int PROBE_STEP_TIMEOUT_MS = 10000;
  constexpr int TOKEN_SETTLE_MS = 400;
  constexpr int FALLBACK_POLL_MS = 500;
  constexpr int FALLBACK_TIMEOUT_MS = 10 * 60 * 1000;
  constexpr int PTY_COLUMNS = 1000;
  constexpr int PTY_ROWS = 50;
  constexpr int MIN_TOKEN_BODY_LENGTH = 40;

  const QRegularExpression &tokenStartPattern()
  {
    static const QRegularExpression pattern( u"sk-ant-oat01-[A-Za-z0-9_-]*"_s );
    return pattern;
  }

  const QRegularExpression &fullTokenPattern()
  {
    static const QRegularExpression pattern( u"^sk-ant-oat01-[A-Za-z0-9_-]{%1,}$"_s.arg( MIN_TOKEN_BODY_LENGTH ) );
    return pattern;
  }

  bool isExecutableFile( const QString &path )
  {
    if ( path.trimmed().isEmpty() )
      return false;
    const QFileInfo info( path );
    return info.exists() && info.isFile() && info.isExecutable();
  }

  QStringList nvmNodeBinDirectories()
  {
    QStringList directories;
    const QDir nvmDir( QDir::homePath() + u"/.nvm/versions/node"_s );
    if ( !nvmDir.exists() )
      return directories;
    // Newest version first (lexicographic is good enough for vNN.x.y).
    const QStringList versions = nvmDir.entryList( QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed );
    for ( const QString &version : versions )
      directories.append( nvmDir.filePath( version + u"/bin"_s ) );
    return directories;
  }

  QString lastLines( const QString &text, int lineCount )
  {
    QStringList lines = text.split( u'\n', Qt::SkipEmptyParts );
    for ( QString &line : lines )
      line = line.trimmed();
    lines.removeAll( QString() );
    if ( lines.size() > lineCount )
      lines = lines.mid( lines.size() - lineCount );
    return lines.join( u'\n' );
  }
} // namespace

QgsAiClaudeCodeCli::QgsAiClaudeCodeCli( QObject *parent )
  : QObject( parent )
{
  mTimeoutTimer = new QTimer( this );
  mTimeoutTimer->setSingleShot( true );
  connect( mTimeoutTimer, &QTimer::timeout, this, &QgsAiClaudeCodeCli::onTimeout );

  mTokenSettleTimer = new QTimer( this );
  mTokenSettleTimer->setSingleShot( true );
  mTokenSettleTimer->setInterval( TOKEN_SETTLE_MS );
  connect( mTokenSettleTimer, &QTimer::timeout, this, &QgsAiClaudeCodeCli::finalizeToken );
}

QgsAiClaudeCodeCli::~QgsAiClaudeCodeCli()
{
  if ( mPty && mPty->isRunning() )
    mPty->kill();
  stopFallback( true );
}

// ---------------------------------------------------------------- discovery

QString QgsAiClaudeCodeCli::cliPathSettingKey()
{
  return u"ai/provider/claude/cliPath"_s;
}

QStringList QgsAiClaudeCodeCli::extraSearchDirectories()
{
  const QString home = QDir::homePath();
  QStringList directories;
#if defined( Q_OS_WIN )
  directories << home + u"/.local/bin"_s
              << qEnvironmentVariable( "APPDATA" ) + u"/npm"_s
              << qEnvironmentVariable( "LOCALAPPDATA" ) + u"/Programs/claude"_s;
#else
  directories << home + u"/.local/bin"_s
              << home + u"/.claude/local"_s
              << u"/opt/homebrew/bin"_s
              << u"/usr/local/bin"_s
              << home + u"/.npm-global/bin"_s
              << home + u"/.volta/bin"_s
              << home + u"/.bun/bin"_s;
  directories << nvmNodeBinDirectories();
#endif
  directories.removeAll( QString() );
  return directories;
}

QStringList QgsAiClaudeCodeCli::candidatePaths()
{
  QStringList paths;
  const QStringList directories = extraSearchDirectories();
  for ( const QString &directory : directories )
  {
#if defined( Q_OS_WIN )
    paths << directory + u"/claude.exe"_s << directory + u"/claude.cmd"_s;
#else
    paths << directory + u"/claude"_s;
#endif
  }
#if !defined( Q_OS_WIN )
  paths << u"/usr/bin/claude"_s;
#endif
  return paths;
}

QString QgsAiClaudeCodeCli::locate( QString *reason )
{
  const QString envPath = qEnvironmentVariable( "STRATA_CLAUDE_CLI" ).trimmed();
  if ( !envPath.isEmpty() )
  {
    if ( isExecutableFile( envPath ) )
      return QFileInfo( envPath ).absoluteFilePath();
    if ( reason )
      *reason = tr( "STRATA_CLAUDE_CLI points to a file that is not executable: %1" ).arg( envPath );
    return QString();
  }

  QgsSettings settings;
  const QString override = settings.value( cliPathSettingKey() ).toString().trimmed();
  if ( !override.isEmpty() )
  {
    if ( isExecutableFile( override ) )
      return QFileInfo( override ).absoluteFilePath();
    if ( reason )
      *reason = tr( "The configured Claude Code executable is missing or not executable: %1" ).arg( override );
    return QString();
  }

  const QStringList candidates = candidatePaths();
  for ( const QString &candidate : candidates )
  {
    if ( isExecutableFile( candidate ) )
      return QFileInfo( candidate ).absoluteFilePath();
  }

  QString onPath = QStandardPaths::findExecutable( u"claude"_s );
  if ( onPath.isEmpty() )
    onPath = QStandardPaths::findExecutable( u"claude"_s, extraSearchDirectories() );
  if ( !onPath.isEmpty() )
    return onPath;

  if ( reason )
    *reason = tr( "Claude Code CLI not found. Install it, or choose the executable manually." );
  return QString();
}

QProcessEnvironment QgsAiClaudeCodeCli::childEnvironment( const QString &cliPath )
{
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

  QStringList pathDirectories;
  if ( !cliPath.trimmed().isEmpty() )
  {
    const QFileInfo info( cliPath );
    pathDirectories << info.absolutePath();
    // npm shims are `#!/usr/bin/env node` scripts: node lives next to the symlink target.
    const QString canonical = info.canonicalFilePath();
    if ( !canonical.isEmpty() )
      pathDirectories << QFileInfo( canonical ).absolutePath();
  }
  pathDirectories << extraSearchDirectories();
  pathDirectories << environment.value( u"PATH"_s ).split( QDir::listSeparator(), Qt::SkipEmptyParts );
  pathDirectories.removeDuplicates();
  environment.insert( u"PATH"_s, pathDirectories.join( QDir::listSeparator() ) );

  environment.insert( u"TERM"_s, u"xterm-256color"_s );
  environment.insert( u"COLUMNS"_s, QString::number( PTY_COLUMNS ) );
  environment.insert( u"LINES"_s, QString::number( PTY_ROWS ) );
  environment.insert( u"NO_COLOR"_s, u"1"_s );
  // Always run a fresh browser login: ambient credentials would short-circuit the
  // flow or make `auth status` report an identity that is not the CLI's own login.
  environment.remove( u"CLAUDE_CODE_OAUTH_TOKEN"_s );
  environment.remove( u"ANTHROPIC_API_KEY"_s );
  environment.remove( u"ANTHROPIC_AUTH_TOKEN"_s );
  // CI=1 flips Claude Code into non-interactive mode; never inherit it.
  environment.remove( u"CI"_s );
  return environment;
}

bool QgsAiClaudeCodeCli::interactiveSessionSupported()
{
  return QgsAiPtySession::isSupported();
}

QUrl QgsAiClaudeCodeCli::installDocsUrl()
{
  return QUrl( u"https://code.claude.com/docs/en/setup"_s );
}

// -------------------------------------------------------------------- probe

void QgsAiClaudeCodeCli::probe( const QString &cliPath )
{
  if ( mProbeProcess )
    return;

  mProbeInfo = CliInfo();
  QString reason;
  mProbeInfo.path = cliPath.trimmed().isEmpty() ? locate( &reason ) : cliPath.trimmed();
  if ( mProbeInfo.path.isEmpty() || !isExecutableFile( mProbeInfo.path ) )
  {
    mProbeInfo.path.clear();
    mProbeInfo.error = reason.isEmpty() ? tr( "Claude Code CLI not found." ) : reason;
    // Deliver asynchronously so callers can always connect after calling probe().
    QTimer::singleShot( 0, this, &QgsAiClaudeCodeCli::finishProbe );
    return;
  }

  runProbeStep( { u"--version"_s }, [this]( int exitCode, const QByteArray &output ) {
    if ( exitCode != 0 && output.trimmed().isEmpty() )
    {
      mProbeInfo.error = tr( "Claude Code did not start (exit code %1)." ).arg( exitCode );
      finishProbe();
      return;
    }
    mProbeInfo.version = parseVersion( output );
    runProbeStep( { u"auth"_s, u"status"_s, u"--json"_s }, [this]( int, const QByteArray &statusOutput ) {
      // A non-zero exit code just means "not logged in"; only unparsable output is worth noting.
      if ( !parseAuthStatus( statusOutput, mProbeInfo ) )
        mProbeInfo.loggedIn = false;
      finishProbe();
    } );
  } );
}

void QgsAiClaudeCodeCli::runProbeStep( const QStringList &arguments, std::function<void( int, const QByteArray & )> onDone )
{
  QProcess *process = new QProcess( this );
  mProbeProcess = process;
  process->setProcessEnvironment( childEnvironment( mProbeInfo.path ) );
  process->setProcessChannelMode( QProcess::MergedChannels );
  process->setWorkingDirectory( QDir::homePath() );

  QTimer *timeout = new QTimer( process );
  timeout->setSingleShot( true );
  timeout->setInterval( PROBE_STEP_TIMEOUT_MS );
  connect( timeout, &QTimer::timeout, process, [process]() {
    if ( process->state() != QProcess::NotRunning )
      process->kill();
  } );

  connect( process, &QProcess::finished, this, [this, process, onDone]( int exitCode, QProcess::ExitStatus ) {
    const QByteArray output = process->readAllStandardOutput();
    process->deleteLater();
    if ( mProbeProcess == process )
      mProbeProcess = nullptr;
    onDone( exitCode, output );
  } );
  connect( process, &QProcess::errorOccurred, this, [this, process, onDone]( QProcess::ProcessError error ) {
    if ( error != QProcess::FailedToStart )
      return;
    process->deleteLater();
    if ( mProbeProcess == process )
      mProbeProcess = nullptr;
    mProbeInfo.error = tr( "Unable to start Claude Code: %1" ).arg( process->errorString() );
    onDone( -1, QByteArray() );
  } );

  timeout->start();
  process->start( mProbeInfo.path, arguments );
}

void QgsAiClaudeCodeCli::finishProbe()
{
  mProbeProcess = nullptr;
  emit probeFinished( mProbeInfo );
}

// -------------------------------------------------------------- setup-token

bool QgsAiClaudeCodeCli::isRunning() const
{
  return ( mPty && mPty->isRunning() ) || mFallbackActive;
}

bool QgsAiClaudeCodeCli::supportsInteractiveInput() const
{
  return mPty != nullptr;
}

bool QgsAiClaudeCodeCli::startSetupToken( const QString &cliPath, QString *errorMessage )
{
  if ( isRunning() )
  {
    if ( errorMessage )
      *errorMessage = tr( "A Claude Code login is already in progress." );
    return false;
  }

  QString reason;
  const QString path = cliPath.trimmed().isEmpty() ? locate( &reason ) : cliPath.trimmed();
  if ( path.isEmpty() || !isExecutableFile( path ) )
  {
    if ( errorMessage )
      *errorMessage = reason.isEmpty() ? tr( "Claude Code CLI not found." ) : reason;
    return false;
  }

  resetSession();
  mCliPath = path;

#if defined( Q_OS_WIN )
  if ( !startConsoleFallback( path, errorMessage ) )
    return false;
  setState( State::WaitingForBrowser );
  mTimeoutTimer->start( std::max( mTimeoutMs, FALLBACK_TIMEOUT_MS ) );
  return true;
#else
  mPty = new QgsAiPtySession( this );
  connect( mPty, &QgsAiPtySession::outputReceived, this, &QgsAiClaudeCodeCli::appendRawOutput );
  connect( mPty, &QgsAiPtySession::finished, this, &QgsAiClaudeCodeCli::handleSessionFinished );
  if ( !mPty->start( path, { u"setup-token"_s }, childEnvironment( path ), PTY_COLUMNS, PTY_ROWS, errorMessage ) )
  {
    mPty->deleteLater();
    mPty = nullptr;
    return false;
  }
  setState( State::Starting );
  mTimeoutTimer->start( mTimeoutMs );
  return true;
#endif
}

void QgsAiClaudeCodeCli::submitAuthorizationCode( const QString &code )
{
  const QString trimmed = code.trimmed();
  if ( trimmed.isEmpty() || !mPty || !mPty->isRunning() )
    return;
  mPty->write( trimmed.toUtf8() + "\r" );
}

void QgsAiClaudeCodeCli::cancel()
{
  if ( !isRunning() )
    return;
  mCancelled = true;
  mTimeoutTimer->stop();
  mTokenSettleTimer->stop();
  if ( mPty && mPty->isRunning() )
  {
    mPty->kill();
    return; // handleSessionFinished() completes the cancellation.
  }
  if ( mFallbackActive )
  {
    stopFallback( true );
    setState( State::Idle );
    emit finished( -1 );
  }
}

QString QgsAiClaudeCodeCli::transcript() const
{
  return redactTokens( mTranscript );
}

QString QgsAiClaudeCodeCli::transcriptTail( int lines ) const
{
  return redactTokens( lastLines( mTranscript, lines ) );
}

void QgsAiClaudeCodeCli::resetSession()
{
  mRawOutput.clear();
  mTranscript.clear();
  mToken.clear();
  mAuthorizationUrl = QUrl();
  mTokenEmitted = false;
  mCodePromptEmitted = false;
  mCancelled = false;
  mPendingFailure.clear();
  mTokenSettleTimer->stop();
  if ( mPty )
  {
    mPty->deleteLater();
    mPty = nullptr;
  }
}

void QgsAiClaudeCodeCli::setState( State state )
{
  if ( mState == state )
    return;
  mState = state;
  emit stateChanged( state );
}

void QgsAiClaudeCodeCli::appendRawOutput( const QByteArray &bytes )
{
  if ( bytes.isEmpty() )
    return;
  mRawOutput.append( bytes );
  // Re-derive the transcript from the raw bytes so escape sequences and UTF-8
  // characters split across chunks are handled once they are complete.
  const int previousLength = mTranscript.size();
  mTranscript = stripAnsi( QString::fromUtf8( mRawOutput ) );
  if ( mTranscript.size() > previousLength )
    emit outputReceived( redactTokens( mTranscript.mid( previousLength ) ) );

  if ( mAuthorizationUrl.isEmpty() )
  {
    const QUrl url = extractAuthorizeUrl( mTranscript );
    if ( url.isValid() )
    {
      mAuthorizationUrl = url;
      if ( mState == State::Starting )
        setState( State::WaitingForBrowser );
      emit browserUrlDetected( url );
    }
  }

  if ( !mCodePromptEmitted && !mTokenEmitted && detectsCodePrompt( mTranscript ) )
  {
    mCodePromptEmitted = true;
    setState( State::WaitingForCode );
    emit authorizationCodeRequested();
  }

  if ( !mTokenEmitted && !extractSetupToken( mTranscript ).isEmpty() )
  {
    setState( State::Finishing );
    scheduleTokenSettle();
  }
}

void QgsAiClaudeCodeCli::scheduleTokenSettle()
{
  // Wait for the output to settle so a token split across chunks is captured whole.
  mTokenSettleTimer->start();
}

void QgsAiClaudeCodeCli::finalizeToken()
{
  if ( mTokenEmitted )
    return;
  const QString token = extractSetupToken( mTranscript );
  if ( token.isEmpty() )
    return;
  mTokenEmitted = true;
  mToken = token;
  mTimeoutTimer->stop();
  setState( State::Finishing );
  emit tokenReceived( token );

  if ( mFallbackActive )
  {
    stopFallback( true );
    mFallbackActive = false;
    setState( State::Idle );
    emit finished( 0 );
    return;
  }
  // The CLI normally exits right after printing the token; nudge it if it lingers.
  QTimer::singleShot( 1500, this, [this]() {
    if ( mPty && mPty->isRunning() )
      mPty->terminate();
  } );
}

void QgsAiClaudeCodeCli::handleSessionFinished( int exitCode, bool crashed )
{
  mTimeoutTimer->stop();
  mTokenSettleTimer->stop();
  if ( mPty )
  {
    mPty->deleteLater();
    mPty = nullptr;
  }

  if ( mCancelled )
  {
    setState( State::Idle );
    emit finished( exitCode );
    return;
  }

  if ( !mTokenEmitted )
    finalizeToken();

  if ( !mTokenEmitted )
  {
    QString message = mPendingFailure;
    if ( message.isEmpty() )
    {
      message = crashed ? tr( "Claude Code stopped unexpectedly (signal %1) before printing a token." ).arg( exitCode - 128 )
                        : tr( "Claude Code exited (code %1) without printing a token." ).arg( exitCode );
    }
    const QString tail = transcriptTail();
    if ( !tail.isEmpty() )
      message += u"\n\n"_s + tail;
    QgsMessageLog::logMessage( u"Claude Code setup-token failed: %1"_s.arg( message ), u"AI"_s, Qgis::MessageLevel::Warning );
    setState( State::Idle );
    emit failed( message );
    emit finished( exitCode );
    return;
  }

  setState( State::Idle );
  emit finished( exitCode );
}

void QgsAiClaudeCodeCli::onTimeout()
{
  if ( !isRunning() )
    return;
  mPendingFailure = tr( "Timed out waiting for the browser approval." );
  if ( mPty && mPty->isRunning() )
  {
    mPty->kill();
    return;
  }
  if ( mFallbackActive )
  {
    stopFallback( true );
    setState( State::Idle );
    emit failed( mPendingFailure );
    emit finished( -1 );
  }
}

// ------------------------------------------------------ Windows console path

bool QgsAiClaudeCodeCli::startConsoleFallback( const QString &cliPath, QString *errorMessage )
{
#if defined( Q_OS_WIN )
  const QString tempDir = QStandardPaths::writableLocation( QStandardPaths::TempLocation );
  const QString id = QUuid::createUuid().toString( QUuid::WithoutBraces );
  mFallbackLogPath = QDir( tempDir ).filePath( u"strata-claude-setup-token-%1.log"_s.arg( id ) );
  const QString scriptPath = QDir( tempDir ).filePath( u"strata-claude-setup-token-%1.ps1"_s.arg( id ) );

  // PowerShell tees the CLI output into the log Strata polls; stdin stays the
  // console so the CLI keeps its interactive prompt.
  QString escapedCli = cliPath;
  escapedCli.replace( QChar( u'\'' ), u"''"_s );
  QString escapedLog = mFallbackLogPath;
  escapedLog.replace( QChar( u'\'' ), u"''"_s );
  QFile script( scriptPath );
  if ( !script.open( QIODevice::WriteOnly | QIODevice::Text ) )
  {
    if ( errorMessage )
      *errorMessage = tr( "Unable to write the helper script: %1" ).arg( scriptPath );
    return false;
  }
  script.write( u"$host.UI.RawUI.WindowTitle = 'Connect Claude Code to Strata'\n"_s.toUtf8() );
  script.write( u"Write-Host 'Strata is waiting for Claude Code. Approve the login in your browser; this window closes on its own when done.'\n"_s.toUtf8() );
  script.write( u"& '%1' setup-token 2>&1 | Tee-Object -FilePath '%2'\n"_s.arg( escapedCli, escapedLog ).toUtf8() );
  script.close();

  QProcess process;
  process.setProgram( u"powershell.exe"_s );
  process.setArguments( { u"-NoProfile"_s, u"-ExecutionPolicy"_s, u"Bypass"_s, u"-File"_s, QDir::toNativeSeparators( scriptPath ) } );
  process.setProcessEnvironment( childEnvironment( cliPath ) );
  process.setWorkingDirectory( QDir::homePath() );
  process.setCreateProcessArgumentsModifier( []( QProcess::CreateProcessArguments *args ) {
    args->flags &= ~static_cast<DWORD>( CREATE_NO_WINDOW );
    args->flags |= CREATE_NEW_CONSOLE;
    args->startupInfo->dwFlags &= ~static_cast<DWORD>( STARTF_USESTDHANDLES );
  } );
  if ( !process.startDetached() )
  {
    if ( errorMessage )
      *errorMessage = tr( "Unable to open a console for Claude Code: %1" ).arg( process.errorString() );
    return false;
  }

  mFallbackActive = true;
  mFallbackReadOffset = 0;
  if ( !mFallbackPollTimer )
  {
    mFallbackPollTimer = new QTimer( this );
    mFallbackPollTimer->setInterval( FALLBACK_POLL_MS );
    connect( mFallbackPollTimer, &QTimer::timeout, this, &QgsAiClaudeCodeCli::pollFallbackLog );
  }
  mFallbackPollTimer->start();
  return true;
#else
  Q_UNUSED( cliPath )
  if ( errorMessage )
    *errorMessage = tr( "The console fallback is only available on Windows." );
  return false;
#endif
}

void QgsAiClaudeCodeCli::pollFallbackLog()
{
  if ( !mFallbackActive || mFallbackLogPath.isEmpty() )
    return;
  QFile log( mFallbackLogPath );
  if ( !log.exists() || !log.open( QIODevice::ReadOnly ) )
    return;
  if ( log.size() <= mFallbackReadOffset )
    return;
  log.seek( mFallbackReadOffset );
  const QByteArray delta = log.readAll();
  mFallbackReadOffset = log.pos();
  log.close();
  // PowerShell writes the log as UTF-16LE with a BOM; normalise to UTF-8 bytes.
  QByteArray bytes = delta;
  if ( mFallbackReadOffset == delta.size() && bytes.startsWith( "\xFF\xFE" ) )
    bytes = QString::fromUtf16( reinterpret_cast<const char16_t *>( bytes.constData() + 2 ), ( bytes.size() - 2 ) / 2 ).toUtf8();
  else if ( bytes.size() >= 2 && bytes.at( 1 ) == '\0' )
    bytes = QString::fromUtf16( reinterpret_cast<const char16_t *>( bytes.constData() ), bytes.size() / 2 ).toUtf8();
  appendRawOutput( bytes );
}

void QgsAiClaudeCodeCli::stopFallback( bool deleteLog )
{
  if ( mFallbackPollTimer )
    mFallbackPollTimer->stop();
  mFallbackActive = false;
  if ( deleteLog && !mFallbackLogPath.isEmpty() )
  {
    // The log holds the token: scrub it before removing the file.
    QFile log( mFallbackLogPath );
    if ( log.exists() && log.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
      log.write( QByteArray( 256, ' ' ) );
      log.close();
    }
    QFile::remove( mFallbackLogPath );
    QString scriptPath = mFallbackLogPath;
    scriptPath.replace( u".log"_s, u".ps1"_s );
    QFile::remove( scriptPath );
    mFallbackLogPath.clear();
  }
}

// ------------------------------------------------------------ pure helpers

QString QgsAiClaudeCodeCli::stripAnsi( const QString &raw )
{
  const QString esc( QChar( 0x1B ) );
  static const QRegularExpression oscSequence( esc + u"\\][^\\x07\\x1B]*(?:\\x07|"_s + esc + u"\\\\)"_s );
  static const QRegularExpression csiSequence( esc + u"\\[[0-?]*[ -/]*[@-~]"_s );
  // Fe escapes (ESC @ … ESC _) plus the common private ones (ESC 7 / ESC 8 / ESC =).
  static const QRegularExpression singleEscape( esc + u"[@-_0-9=<>]"_s );
  static const QRegularExpression controlChars( u"[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1A\\x1C-\\x1F\\x7F]"_s );
  static const QString boxDrawing = QString::fromUtf8( "│╭╮╰╯─┃┌┐└┘━┏┓┗┛" );

  QString cleaned = raw;
  cleaned.remove( oscSequence );
  cleaned.remove( csiSequence );
  cleaned.remove( singleEscape );
  cleaned.remove( u'\r' );
  cleaned.remove( controlChars );
  for ( const QChar &character : boxDrawing )
    cleaned.remove( character );
  return cleaned;
}

QString QgsAiClaudeCodeCli::extractSetupToken( const QString &text )
{
  // A wrapped fragment is a long run of token characters; short single words
  // ("Done") on the next line are ordinary output, not part of the token.
  static const QRegularExpression continuationLine( u"^[A-Za-z0-9_-]{10,}$"_s );
  QString best;
  QRegularExpressionMatchIterator it = tokenStartPattern().globalMatch( text );
  while ( it.hasNext() )
  {
    const QRegularExpressionMatch match = it.next();
    QString candidate = match.captured( 0 );
    int position = match.capturedEnd( 0 );
    // Terminals hard-wrap long lines: glue following lines while they look like
    // token fragments (only token characters, no spaces).
    for ( ;; )
    {
      int cursor = position;
      while ( cursor < text.size() && ( text.at( cursor ) == u' ' || text.at( cursor ) == u'\t' ) )
        ++cursor;
      if ( cursor >= text.size() || text.at( cursor ) != u'\n' )
        break;
      int lineEnd = text.indexOf( u'\n', cursor + 1 );
      if ( lineEnd < 0 )
        lineEnd = text.size();
      const QString nextLine = text.mid( cursor + 1, lineEnd - cursor - 1 ).trimmed();
      if ( nextLine.isEmpty() || !continuationLine.match( nextLine ).hasMatch() )
        break;
      candidate += nextLine;
      position = lineEnd;
    }
    if ( candidate.size() > best.size() )
      best = candidate;
  }
  return fullTokenPattern().match( best ).hasMatch() ? best : QString();
}

QUrl QgsAiClaudeCodeCli::extractAuthorizeUrl( const QString &text )
{
  static const QRegularExpression urlPattern( u"https://[^\\s\"'<>]+"_s );
  QRegularExpressionMatchIterator it = urlPattern.globalMatch( text );
  while ( it.hasNext() )
  {
    QString candidate = it.next().captured( 0 );
    while ( !candidate.isEmpty() && ( candidate.endsWith( u'.' ) || candidate.endsWith( u',' ) || candidate.endsWith( u')' ) ) )
      candidate.chop( 1 );
    const QUrl url( candidate );
    if ( !url.isValid() )
      continue;
    const QString host = url.host().toLower();
    const QString path = url.path().toLower();
    const bool anthropicHost = host.endsWith( "claude.ai"_L1 ) || host.endsWith( "claude.com"_L1 ) || host.endsWith( "anthropic.com"_L1 );
    const bool loginPath = path.contains( "oauth"_L1 ) || path.contains( "authorize"_L1 ) || path.contains( "login"_L1 );
    if ( anthropicHost && loginPath )
      return url;
  }
  return QUrl();
}

bool QgsAiClaudeCodeCli::detectsCodePrompt( const QString &text )
{
  static const QRegularExpression pattern( u"paste\\s+(?:the\\s+)?code\\s+here|authorization\\s+code|paste\\s+the\\s+code"_s, QRegularExpression::CaseInsensitiveOption );
  return pattern.match( text ).hasMatch();
}

QString QgsAiClaudeCodeCli::parseVersion( const QByteArray &output )
{
  static const QRegularExpression pattern( u"(\\d+\\.\\d+\\.\\d+(?:[-+][A-Za-z0-9.]+)?)"_s );
  const QRegularExpressionMatch match = pattern.match( QString::fromUtf8( output ) );
  return match.hasMatch() ? match.captured( 1 ) : QString();
}

bool QgsAiClaudeCodeCli::parseAuthStatus( const QByteArray &json, CliInfo &info )
{
  const QByteArray trimmed = json.trimmed();
  const int start = trimmed.indexOf( '{' );
  if ( start < 0 )
    return false;
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson( trimmed.mid( start ), &parseError );
  if ( parseError.error != QJsonParseError::NoError || !document.isObject() )
    return false;
  const QJsonObject object = document.object();
  info.loggedIn = object.value( u"loggedIn"_s ).toBool( false );
  info.email = object.value( u"email"_s ).toString().trimmed();
  info.orgName = object.value( u"orgName"_s ).toString().trimmed();
  if ( info.orgName.isEmpty() )
    info.orgName = object.value( u"organization"_s ).toString().trimmed();
  info.subscriptionType = object.value( u"subscriptionType"_s ).toString().trimmed();
  info.authMethod = object.value( u"authMethod"_s ).toString().trimmed();
  if ( info.authMethod.isEmpty() )
    info.authMethod = object.value( u"loginMethod"_s ).toString().trimmed();
  return true;
}

QString QgsAiClaudeCodeCli::redactTokens( const QString &text )
{
  static const QRegularExpression secrets( u"sk-ant-(?:oat01|ort01|api03)-[A-Za-z0-9_-]+"_s );
  QString redacted = text;
  redacted.replace( secrets, u"sk-ant-…[redacted]"_s );
  return redacted;
}
