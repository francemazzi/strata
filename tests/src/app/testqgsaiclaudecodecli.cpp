/***************************************************************************
  testqgsaiclaudecodecli.cpp
  --------------------------
  begin                : September 2026
***************************************************************************/

#include "ai/qgsaiclaudecodecli.h"
#include "ai/qgsaiptysession.h"
#include "qgsapplication.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace
{
  const QString TOKEN_PART_1 = u"AbCdEfGhIjKlMnOpQrStUvWxYz0123456789abcdefghijklmnopqrstuvwxyz"_s;
  const QString TOKEN_PART_2 = u"ABCDEFGHIJKLMNOPQRSTUVWXYZ-_9876543210zyxwvutsrqponmlkjihgfedcbaAA"_s;

  QString fullToken()
  {
    return u"sk-ant-oat01-"_s + TOKEN_PART_1 + TOKEN_PART_2;
  }

  //! Writes an executable shell script into \a dir and returns its path (POSIX only).
  QString writeFakeCli( const QTemporaryDir &dir, const QString &body )
  {
    const QString path = dir.filePath( u"claude"_s );
    QFile file( path );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Text ) )
      return QString();
    file.write( "#!/bin/sh\n" );
    file.write( body.toUtf8() );
    file.close();
    file.setPermissions( QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther );
    return path;
  }

  QString setupTokenScript()
  {
    // Mimics `claude setup-token`: ANSI-decorated banner, the authorize URL, a
    // code prompt read from the terminal, then the token hard-wrapped over two
    // lines inside an Ink-style box.
    return uR"sh(if [ "$1" != "setup-token" ]; then exit 2; fi
printf '\033[1m\033[38;5;208mClaude Code\033[0m setup-token\n'
printf "Browser did not open? Use the url below to sign in:\n"
printf 'https://claude.ai/oauth/authorize?code=true&client_id=test&response_type=code&state=xyz\n'
printf '\033[2mPaste code here if prompted > \033[0m'
read code
printf '\n\033[32mLogin successful.\033[0m\n'
printf '\342\224\202 sk-ant-oat01-%s \342\224\202\n' "%1"
printf '\342\224\202 %s \342\224\202\n' "%2"
printf '\nStore this token securely.\n'
exit 0
)sh"_s.arg( TOKEN_PART_1, TOKEN_PART_2 );
  }
} // namespace

class TestQgsAiClaudeCodeCli : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void stripAnsiRemovesEscapesAndBoxes();
    void extractSetupTokenPlain();
    void extractSetupTokenWrappedInsideBox();
    void extractSetupTokenPicksLongestAcrossFrames();
    void extractSetupTokenRejectsShortAndUnrelated();
    void extractAuthorizeUrlFiltersHosts();
    void detectsCodePromptCaseInsensitive();
    void parseVersionFromCliOutput();
    void parseAuthStatusJson();
    void redactTokensHidesSecrets();
    void childEnvironmentPrependsCliDirAndStripsCredentials();
    void locateHonoursSettingsOverride();
    void locateIgnoresNonExecutableOverride();
    void probeFakeCli();
    void probeReportsMissingCli();
    void setupTokenFlowThroughPty();
    void setupTokenFailureReportsExitCode();
    void cancelKillsChild();
    void timeoutFails();
    void probeInstalledCli();
};

void TestQgsAiClaudeCodeCli::initTestCase()
{
  // No provider registry needed (and initQgis() would load every data provider plugin).
}

void TestQgsAiClaudeCodeCli::cleanupTestCase()
{}

void TestQgsAiClaudeCodeCli::cleanup()
{
  QgsSettings settings;
  settings.remove( QgsAiClaudeCodeCli::cliPathSettingKey() );
  qunsetenv( "STRATA_CLAUDE_CLI" );
}

void TestQgsAiClaudeCodeCli::stripAnsiRemovesEscapesAndBoxes()
{
  const QString esc( QChar( 0x1B ) );
  const QString raw = esc + u"[1m"_s + esc + u"[38;5;208mHello"_s + esc + u"[0m "_s + esc + u"]0;title"_s + QChar( 0x07 ) + u"world\r\n"_s + QString::fromUtf8( "╭──╮\n│ x │\n" ) + esc + u"7"_s;
  const QString cleaned = QgsAiClaudeCodeCli::stripAnsi( raw );
  QCOMPARE( cleaned, u"Hello world\n\n x \n"_s );
}

void TestQgsAiClaudeCodeCli::extractSetupTokenPlain()
{
  const QString text = u"Login successful.\nYour token:\n"_s + fullToken() + u"\nStore it securely.\n"_s;
  QCOMPARE( QgsAiClaudeCodeCli::extractSetupToken( text ), fullToken() );
}

void TestQgsAiClaudeCodeCli::extractSetupTokenWrappedInsideBox()
{
  // 80-column hard wrap inside an Ink box, with trailing padding and box borders.
  const QString boxed = QString::fromUtf8( "│ sk-ant-oat01-" ) + TOKEN_PART_1 + QString::fromUtf8( "   │\n│ " ) + TOKEN_PART_2 + QString::fromUtf8( " │\n│ Store this token securely. │\n" );
  const QString cleaned = QgsAiClaudeCodeCli::stripAnsi( boxed );
  QCOMPARE( QgsAiClaudeCodeCli::extractSetupToken( cleaned ), fullToken() );

  // A wrapped continuation followed by unrelated text must stop at the text.
  const QString withTrailer = u"sk-ant-oat01-"_s + TOKEN_PART_1 + u"\n"_s + TOKEN_PART_2 + u"\nDone\n"_s;
  QCOMPARE( QgsAiClaudeCodeCli::extractSetupToken( withTrailer ), fullToken() );
}

void TestQgsAiClaudeCodeCli::extractSetupTokenPicksLongestAcrossFrames()
{
  // Ink re-renders frames: a truncated early frame must not win over the full token.
  const QString partial = u"sk-ant-oat01-"_s + TOKEN_PART_1.left( 45 );
  const QString text = partial + u"\n\n"_s + fullToken() + u"\n"_s;
  QCOMPARE( QgsAiClaudeCodeCli::extractSetupToken( text ), fullToken() );
}

void TestQgsAiClaudeCodeCli::extractSetupTokenRejectsShortAndUnrelated()
{
  QVERIFY( QgsAiClaudeCodeCli::extractSetupToken( u"sk-ant-oat01-tooshort\n"_s ).isEmpty() );
  QVERIFY( QgsAiClaudeCodeCli::extractSetupToken( u"sk-ant-api03-"_s + TOKEN_PART_1 + TOKEN_PART_2 ).isEmpty() );
  QVERIFY( QgsAiClaudeCodeCli::extractSetupToken( QString() ).isEmpty() );
}

void TestQgsAiClaudeCodeCli::extractAuthorizeUrlFiltersHosts()
{
  // No apostrophes in this file's literals: moc treats a lone quote as a char literal and stops parsing.
  const QString text = u"See https://docs.example.com/oauth first.\nBrowser did not open? https://claude.ai/oauth/authorize?code=true&state=abc.\n"_s;
  const QUrl url = QgsAiClaudeCodeCli::extractAuthorizeUrl( text );
  QVERIFY( url.isValid() );
  QCOMPARE( url.host(), u"claude.ai"_s );
  QCOMPARE( url.path(), u"/oauth/authorize"_s );
  QVERIFY( !url.toString().endsWith( u'.' ) );

  QVERIFY( !QgsAiClaudeCodeCli::extractAuthorizeUrl( u"https://claude.ai/docs/getting-started"_s ).isValid() );
  QVERIFY( !QgsAiClaudeCodeCli::extractAuthorizeUrl( u"https://evil.example.com/oauth/authorize"_s ).isValid() );
  QVERIFY( QgsAiClaudeCodeCli::extractAuthorizeUrl( u"https://platform.claude.com/oauth/authorize?x=1"_s ).isValid() );
}

void TestQgsAiClaudeCodeCli::detectsCodePromptCaseInsensitive()
{
  QVERIFY( QgsAiClaudeCodeCli::detectsCodePrompt( u"Paste code here if prompted >"_s ) );
  QVERIFY( QgsAiClaudeCodeCli::detectsCodePrompt( u"Enter the AUTHORIZATION CODE:"_s ) );
  QVERIFY( !QgsAiClaudeCodeCli::detectsCodePrompt( u"Opening your browser…"_s ) );
}

void TestQgsAiClaudeCodeCli::parseVersionFromCliOutput()
{
  QCOMPARE( QgsAiClaudeCodeCli::parseVersion( "2.1.197 (Claude Code)\n" ), u"2.1.197"_s );
  QCOMPARE( QgsAiClaudeCodeCli::parseVersion( "claude 3.0.0-beta.2\n" ), u"3.0.0-beta.2"_s );
  QVERIFY( QgsAiClaudeCodeCli::parseVersion( "garbage" ).isEmpty() );
}

void TestQgsAiClaudeCodeCli::parseAuthStatusJson()
{
  QgsAiClaudeCodeCli::CliInfo info;
  QVERIFY( QgsAiClaudeCodeCli::parseAuthStatus( R"({"loggedIn":true,"authMethod":"claude.ai","email":"tester@example.com","orgName":"Test Org","subscriptionType":"max"})", info ) );
  QVERIFY( info.loggedIn );
  QCOMPARE( info.email, u"tester@example.com"_s );
  QCOMPARE( info.orgName, u"Test Org"_s );
  QCOMPARE( info.subscriptionType, u"max"_s );
  QCOMPARE( info.authMethod, u"claude.ai"_s );

  // Leading noise (update notices) before the JSON object is tolerated.
  QgsAiClaudeCodeCli::CliInfo noisy;
  QVERIFY( QgsAiClaudeCodeCli::parseAuthStatus( "Update available\n{\"loggedIn\":false}", noisy ) );
  QVERIFY( !noisy.loggedIn );

  QgsAiClaudeCodeCli::CliInfo malformed;
  QVERIFY( !QgsAiClaudeCodeCli::parseAuthStatus( "not json", malformed ) );
  QVERIFY( !QgsAiClaudeCodeCli::parseAuthStatus( "{\"loggedIn\":", malformed ) );
}

void TestQgsAiClaudeCodeCli::redactTokensHidesSecrets()
{
  const QString redacted = QgsAiClaudeCodeCli::redactTokens( u"token "_s + fullToken() + u" and refresh sk-ant-ort01-abcdef done"_s );
  QVERIFY( !redacted.contains( TOKEN_PART_1 ) );
  QVERIFY( !redacted.contains( u"abcdef"_s ) );
  QVERIFY( redacted.contains( u"done"_s ) );
}

void TestQgsAiClaudeCodeCli::childEnvironmentPrependsCliDirAndStripsCredentials()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, u"exit 0\n"_s );
  QVERIFY( !cli.isEmpty() );

  qputenv( "CLAUDE_CODE_OAUTH_TOKEN", "sk-ant-oat01-should-not-leak" );
  qputenv( "ANTHROPIC_API_KEY", "sk-ant-api03-should-not-leak" );
  qputenv( "CI", "1" );
  const auto restore = qScopeGuard( []() {
    qunsetenv( "CLAUDE_CODE_OAUTH_TOKEN" );
    qunsetenv( "ANTHROPIC_API_KEY" );
    qunsetenv( "CI" );
  } );

  const QProcessEnvironment env = QgsAiClaudeCodeCli::childEnvironment( cli );
  const QStringList pathDirs = env.value( u"PATH"_s ).split( QDir::listSeparator(), Qt::SkipEmptyParts );
  QVERIFY( !pathDirs.isEmpty() );
  QCOMPARE( QDir( pathDirs.first() ).canonicalPath(), QDir( dir.path() ).canonicalPath() );
  QVERIFY( !env.contains( u"CLAUDE_CODE_OAUTH_TOKEN"_s ) );
  QVERIFY( !env.contains( u"ANTHROPIC_API_KEY"_s ) );
  QVERIFY( !env.contains( u"CI"_s ) );
  QCOMPARE( env.value( u"TERM"_s ), u"xterm-256color"_s );
}

void TestQgsAiClaudeCodeCli::locateHonoursSettingsOverride()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, u"exit 0\n"_s );
  QVERIFY( !cli.isEmpty() );

  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), cli );
  QString reason;
  QCOMPARE( QgsAiClaudeCodeCli::locate( &reason ), QFileInfo( cli ).absoluteFilePath() );
  QVERIFY( reason.isEmpty() );

  // The environment override wins over the settings override.
  qputenv( "STRATA_CLAUDE_CLI", cli.toUtf8() );
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), u"/nonexistent/claude"_s );
  QCOMPARE( QgsAiClaudeCodeCli::locate( &reason ), QFileInfo( cli ).absoluteFilePath() );
}

void TestQgsAiClaudeCodeCli::locateIgnoresNonExecutableOverride()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString notExecutable = dir.filePath( u"claude"_s );
  QFile file( notExecutable );
  QVERIFY( file.open( QIODevice::WriteOnly ) );
  file.write( "#!/bin/sh\nexit 0\n" );
  file.close();
  file.setPermissions( QFile::ReadOwner | QFile::WriteOwner );

  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), notExecutable );
  QString reason;
  // An explicit but broken override must not silently fall through to another install.
  QVERIFY( QgsAiClaudeCodeCli::locate( &reason ).isEmpty() );
  QVERIFY( reason.contains( notExecutable ) );
}

void TestQgsAiClaudeCodeCli::probeFakeCli()
{
#if defined( Q_OS_WIN )
  QSKIP( "Fake CLI scripts are POSIX shell scripts." );
#endif
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, uR"sh(if [ "$1" = "--version" ]; then echo "9.9.9 (Claude Code)"; exit 0; fi
if [ "$1" = "auth" ] && [ "$2" = "status" ]; then printf '%s\n' '{"loggedIn":true,"authMethod":"claude.ai","email":"tester@example.com","orgName":"Test Org","subscriptionType":"max"}'; exit 0; fi
exit 2
)sh"_s );
  QVERIFY( !cli.isEmpty() );

  QgsAiClaudeCodeCli client;
  QSignalSpy spy( &client, &QgsAiClaudeCodeCli::probeFinished );
  client.probe( cli );
  QVERIFY( client.isProbing() );
  QVERIFY( spy.wait( 15000 ) );
  QVERIFY( !client.isProbing() );

  const QgsAiClaudeCodeCli::CliInfo info = spy.first().first().value<QgsAiClaudeCodeCli::CliInfo>();
  QVERIFY( info.found() );
  QCOMPARE( info.path, cli );
  QCOMPARE( info.version, u"9.9.9"_s );
  QVERIFY( info.loggedIn );
  QCOMPARE( info.email, u"tester@example.com"_s );
  QCOMPARE( info.subscriptionType, u"max"_s );
  QVERIFY2( info.error.isEmpty(), qPrintable( info.error ) );
}

void TestQgsAiClaudeCodeCli::probeReportsMissingCli()
{
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), u"/nonexistent/claude-cli"_s );

  QgsAiClaudeCodeCli client;
  QSignalSpy spy( &client, &QgsAiClaudeCodeCli::probeFinished );
  client.probe();
  QVERIFY( spy.wait( 5000 ) );
  const QgsAiClaudeCodeCli::CliInfo info = spy.first().first().value<QgsAiClaudeCodeCli::CliInfo>();
  QVERIFY( !info.found() );
  QVERIFY( !info.error.isEmpty() );
}

void TestQgsAiClaudeCodeCli::setupTokenFlowThroughPty()
{
  if ( !QgsAiPtySession::isSupported() )
    QSKIP( "Pseudo-terminal sessions are not supported on this platform." );

  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, setupTokenScript() );
  QVERIFY( !cli.isEmpty() );

  QgsAiClaudeCodeCli client;
  QSignalSpy urlSpy( &client, &QgsAiClaudeCodeCli::browserUrlDetected );
  QSignalSpy codeSpy( &client, &QgsAiClaudeCodeCli::authorizationCodeRequested );
  QSignalSpy tokenSpy( &client, &QgsAiClaudeCodeCli::tokenReceived );
  QSignalSpy failedSpy( &client, &QgsAiClaudeCodeCli::failed );
  QSignalSpy finishedSpy( &client, &QgsAiClaudeCodeCli::finished );

  QString error;
  QVERIFY2( client.startSetupToken( cli, &error ), qPrintable( error ) );
  QVERIFY( client.isRunning() );
  QVERIFY( client.supportsInteractiveInput() );

  QVERIFY( codeSpy.wait( 10000 ) );
  QCOMPARE( urlSpy.count(), 1 );
  const QUrl url = urlSpy.first().first().toUrl();
  QCOMPARE( url.host(), u"claude.ai"_s );
  QCOMPARE( client.authorizationUrl(), url );
  QCOMPARE( client.state(), QgsAiClaudeCodeCli::State::WaitingForCode );

  client.submitAuthorizationCode( u"abc-123"_s );
  QVERIFY( tokenSpy.wait( 10000 ) );
  QCOMPARE( tokenSpy.first().first().toString(), fullToken() );
  if ( finishedSpy.isEmpty() )
    QVERIFY( finishedSpy.wait( 10000 ) );
  QCOMPARE( failedSpy.count(), 0 );
  QCOMPARE( finishedSpy.first().first().toInt(), 0 );
  QVERIFY( !client.isRunning() );
  QCOMPARE( client.state(), QgsAiClaudeCodeCli::State::Idle );

  // The transcript never exposes the token.
  QVERIFY( !client.transcript().contains( TOKEN_PART_1 ) );
  QVERIFY( client.transcript().contains( u"Login successful."_s ) );
}

void TestQgsAiClaudeCodeCli::setupTokenFailureReportsExitCode()
{
  if ( !QgsAiPtySession::isSupported() )
    QSKIP( "Pseudo-terminal sessions are not supported on this platform." );

  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, u"echo 'Not logged in. Please run /login.'\nexit 3\n"_s );
  QVERIFY( !cli.isEmpty() );

  QgsAiClaudeCodeCli client;
  QSignalSpy tokenSpy( &client, &QgsAiClaudeCodeCli::tokenReceived );
  QSignalSpy failedSpy( &client, &QgsAiClaudeCodeCli::failed );
  QSignalSpy finishedSpy( &client, &QgsAiClaudeCodeCli::finished );

  QString error;
  QVERIFY2( client.startSetupToken( cli, &error ), qPrintable( error ) );
  QVERIFY( finishedSpy.wait( 10000 ) );
  QCOMPARE( tokenSpy.count(), 0 );
  QCOMPARE( failedSpy.count(), 1 );
  const QString message = failedSpy.first().first().toString();
  QVERIFY2( message.contains( u"code 3"_s ), qPrintable( message ) );
  QVERIFY2( message.contains( u"Not logged in"_s ), qPrintable( message ) );
  QCOMPARE( finishedSpy.first().first().toInt(), 3 );
}

void TestQgsAiClaudeCodeCli::cancelKillsChild()
{
  if ( !QgsAiPtySession::isSupported() )
    QSKIP( "Pseudo-terminal sessions are not supported on this platform." );

  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, u"echo 'https://claude.ai/oauth/authorize?state=1'\nsleep 30\nexit 0\n"_s );
  QVERIFY( !cli.isEmpty() );

  QgsAiClaudeCodeCli client;
  QSignalSpy urlSpy( &client, &QgsAiClaudeCodeCli::browserUrlDetected );
  QSignalSpy failedSpy( &client, &QgsAiClaudeCodeCli::failed );
  QSignalSpy finishedSpy( &client, &QgsAiClaudeCodeCli::finished );

  QString error;
  QVERIFY2( client.startSetupToken( cli, &error ), qPrintable( error ) );
  QVERIFY( urlSpy.wait( 10000 ) );
  client.cancel();
  QVERIFY( finishedSpy.wait( 5000 ) );
  // A user cancellation is not an error.
  QCOMPARE( failedSpy.count(), 0 );
  QVERIFY( !client.isRunning() );
}

void TestQgsAiClaudeCodeCli::timeoutFails()
{
  if ( !QgsAiPtySession::isSupported() )
    QSKIP( "Pseudo-terminal sessions are not supported on this platform." );

  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, u"sleep 30\nexit 0\n"_s );
  QVERIFY( !cli.isEmpty() );

  QgsAiClaudeCodeCli client;
  client.setTimeoutMs( 500 );
  QSignalSpy failedSpy( &client, &QgsAiClaudeCodeCli::failed );
  QSignalSpy finishedSpy( &client, &QgsAiClaudeCodeCli::finished );

  QString error;
  QVERIFY2( client.startSetupToken( cli, &error ), qPrintable( error ) );
  QVERIFY( finishedSpy.wait( 5000 ) );
  QCOMPARE( failedSpy.count(), 1 );
  QVERIFY( failedSpy.first().first().toString().contains( u"Timed out"_s ) );
  QVERIFY( !client.isRunning() );
}

void TestQgsAiClaudeCodeCli::probeInstalledCli()
{
  // Opt-in smoke test against the real Claude Code CLI on this machine: version
  // and auth status only (no browser flow, nothing is written).
  if ( qEnvironmentVariable( "STRATA_RUN_LIVE_AI_TESTS" ) != "1"_L1 )
    QSKIP( "Set STRATA_RUN_LIVE_AI_TESTS=1 to probe the installed Claude Code CLI." );
  QString reason;
  const QString cli = QgsAiClaudeCodeCli::locate( &reason );
  if ( cli.isEmpty() )
    QSKIP( qPrintable( u"Claude Code CLI not installed: "_s + reason ) );

  QgsAiClaudeCodeCli client;
  QSignalSpy spy( &client, &QgsAiClaudeCodeCli::probeFinished );
  client.probe( cli );
  QVERIFY( spy.wait( 30000 ) );
  const QgsAiClaudeCodeCli::CliInfo info = spy.first().first().value<QgsAiClaudeCodeCli::CliInfo>();
  QVERIFY2( info.error.isEmpty(), qPrintable( info.error ) );
  QVERIFY2( !info.version.isEmpty(), "version not parsed from the real CLI" );
  qInfo() << "Claude Code" << info.version << "loggedIn:" << info.loggedIn << "subscription:" << info.subscriptionType;

  // The real binary also runs under the pty session (TTY-attached, wide window).
  if ( !QgsAiPtySession::isSupported() )
    return;
  QgsAiPtySession session;
  QByteArray output;
  QSignalSpy finishedSpy( &session, &QgsAiPtySession::finished );
  connect( &session, &QgsAiPtySession::outputReceived, &session, [&output]( const QByteArray &chunk ) { output.append( chunk ); } );
  QString error;
  QVERIFY2( session.start( cli, { u"--version"_s }, QgsAiClaudeCodeCli::childEnvironment( cli ), 1000, 50, &error ), qPrintable( error ) );
  QVERIFY( finishedSpy.wait( 30000 ) );
  QCOMPARE( finishedSpy.first().first().toInt(), 0 );
  QVERIFY2( QgsAiClaudeCodeCli::stripAnsi( QString::fromUtf8( output ) ).contains( info.version ), output.constData() );
}

QGSTEST_MAIN( TestQgsAiClaudeCodeCli )
#include "testqgsaiclaudecodecli.moc"
