/***************************************************************************
  testqgsaiclaudeconnectwidget.cpp
  --------------------------------
  begin                : September 2026
***************************************************************************/

#include "ai/qgsaiclaudecodecli.h"
#include "ai/qgsaiclaudeconnectwidget.h"
#include "ai/qgsaimodelrouter.h"
#include "ai/qgsaiptysession.h"
#include "ai/qgsaisecretstore.h"
#include "qgsapplication.h"
#include "qgscollapsiblegroupbox.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QString>
#include <QTemporaryDir>

using namespace Qt::StringLiterals;

namespace
{
  const QString TOKEN_BODY = u"AbCdEfGhIjKlMnOpQrStUvWxYz0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJ"_s;

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

  //! A CLI that answers the probe and mints a token without asking for a code.
  QString happyCliScript()
  {
    return uR"sh(if [ "$1" = "--version" ]; then echo "9.9.9 (Claude Code)"; exit 0; fi
if [ "$1" = "auth" ]; then printf '%s\n' '{"loggedIn":true,"authMethod":"claude.ai","email":"tester@example.com","orgName":"Test Org","subscriptionType":"max"}'; exit 0; fi
if [ "$1" = "setup-token" ]; then
  echo 'https://claude.ai/oauth/authorize?code=true&state=abc'
  echo 'Login successful.'
  echo 'sk-ant-oat01-%1'
  exit 0
fi
exit 2
)sh"_s.arg( TOKEN_BODY );
  }

  QString failingCliScript()
  {
    return uR"sh(if [ "$1" = "--version" ]; then echo "9.9.9 (Claude Code)"; exit 0; fi
if [ "$1" = "auth" ]; then echo '{"loggedIn":false}'; exit 1; fi
echo 'Something went wrong.'
exit 4
)sh"_s;
  }

  [[nodiscard]] auto isolateState()
  {
    const QByteArray savedOAuthToken = qgetenv( "CLAUDE_CODE_OAUTH_TOKEN" );
    const QByteArray savedApiKey = qgetenv( "ANTHROPIC_API_KEY" );
    const QByteArray savedClaudeKey = qgetenv( "CLAUDE_API_KEY" );
    qunsetenv( "CLAUDE_CODE_OAUTH_TOKEN" );
    qunsetenv( "ANTHROPIC_API_KEY" );
    qunsetenv( "CLAUDE_API_KEY" );
    auto reset = []() {
      QgsSettings settings;
      settings.remove( u"ai/provider/claude"_s );
      settings.remove( u"ai/provider/openrouter"_s );
      settings.remove( u"ai/activeProvider"_s );
      QgsAiSecretStore::removeSecret( QgsAiModelRouter::claudeSubscriptionTokenSettingKey() );
      QgsAiSecretStore::removeSecret( u"ai/provider/claude/apiKey"_s );
      QgsAiSecretStore::removeSecret( u"ai/provider/openrouter/apiKey"_s );
    };
    reset();
    return qScopeGuard( [reset, savedOAuthToken, savedApiKey, savedClaudeKey]() {
      reset();
      if ( !savedOAuthToken.isEmpty() )
        qputenv( "CLAUDE_CODE_OAUTH_TOKEN", savedOAuthToken );
      if ( !savedApiKey.isEmpty() )
        qputenv( "ANTHROPIC_API_KEY", savedApiKey );
      if ( !savedClaudeKey.isEmpty() )
        qputenv( "CLAUDE_API_KEY", savedClaudeKey );
    } );
  }

  bool waitForProbe( QgsAiClaudeConnectWidget &widget )
  {
    QPushButton *connectButton = widget.findChild<QPushButton *>( u"aiClaudeConnectButton"_s );
    QLabel *cliStatus = widget.findChild<QLabel *>( u"aiClaudeCliStatusLabel"_s );
    if ( !connectButton || !cliStatus )
      return false;
    QElapsedTimer timer;
    timer.start();
    while ( timer.elapsed() < 15000 )
    {
      QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
      if ( !cliStatus->text().contains( u"Looking for"_s ) )
        return true;
    }
    return false;
  }
} // namespace

class TestQgsAiClaudeConnectWidget : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();

    void defaultsToSubscriptionModeAndDetectsCli();
    void apiKeyModeWhenOnlyApiKeyStored();
    void missingCliDisablesConnect();
    void connectFlowStoresTokenAndActivatesClaude();
    void disconnectClearsTokenAndReturnsToApiKeyMode();
    void failedConnectShowsErrorAndAdvanced();
    void manualTokenIsExposedScrubbed();
};

void TestQgsAiClaudeConnectWidget::initTestCase()
{
  // No provider registry needed (and initQgis() would load every data provider plugin).
}

void TestQgsAiClaudeConnectWidget::cleanupTestCase()
{}

void TestQgsAiClaudeConnectWidget::defaultsToSubscriptionModeAndDetectsCli()
{
#if defined( Q_OS_WIN )
  QSKIP( "Fake CLI scripts are POSIX shell scripts." );
#endif
  const auto guard = isolateState();
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, happyCliScript() );
  QVERIFY( !cli.isEmpty() );
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), cli );

  QgsAiModelRouter router;
  QgsAiClaudeConnectWidget widget( &router );
  widget.show();

  // Fresh install: nothing stored → subscription mode, not connected.
  QCOMPARE( widget.selectedCredentialMode(), QgsAiModelRouter::CredentialMode::OAuth );
  QVERIFY( !widget.isConnected() );
  QVERIFY( !widget.isBusy() );
  QStackedWidget *modeStack = widget.findChild<QStackedWidget *>( u"aiClaudeModeStack"_s );
  QStackedWidget *subscriptionStack = widget.findChild<QStackedWidget *>( u"aiClaudeSubscriptionStack"_s );
  QVERIFY( modeStack && subscriptionStack );
  QCOMPARE( modeStack->currentIndex(), 0 );
  QCOMPARE( subscriptionStack->currentIndex(), 0 );
  QCOMPARE( widget.modelText(), QgsAiModelRouter::defaultClaudeModel() );

  QVERIFY( waitForProbe( widget ) );
  QLabel *cliStatus = widget.findChild<QLabel *>( u"aiClaudeCliStatusLabel"_s );
  QVERIFY( cliStatus->text().contains( u"9.9.9"_s ) );
  QVERIFY( cliStatus->text().contains( u"tester@example.com"_s ) );
  QPushButton *connectButton = widget.findChild<QPushButton *>( u"aiClaudeConnectButton"_s );
  QVERIFY( connectButton->isEnabled() );
  QLabel *installLink = widget.findChild<QLabel *>( u"aiClaudeInstallLink"_s );
  QVERIFY( !installLink->isVisible() );

  // The toggle switches the stacked pane.
  QPushButton *apiKeyMode = widget.findChild<QPushButton *>( u"aiClaudeModeApiKeyButton"_s );
  apiKeyMode->click();
  QCOMPARE( widget.selectedCredentialMode(), QgsAiModelRouter::CredentialMode::ApiKey );
  QCOMPARE( modeStack->currentIndex(), 1 );
}

void TestQgsAiClaudeConnectWidget::apiKeyModeWhenOnlyApiKeyStored()
{
  const auto guard = isolateState();
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), u"/nonexistent/claude"_s );

  QgsAiModelRouter router;
  QVERIFY( router.storeApiKey( QgsAiModelRouter::Provider::Claude, u"sk-ant-api03-test"_s ) );
  QgsAiModelRouter::ProviderSettings s = router.providerSettings( QgsAiModelRouter::Provider::Claude );
  s.credentialMode = QgsAiModelRouter::CredentialMode::ApiKey;
  router.setProviderSettings( QgsAiModelRouter::Provider::Claude, s );

  QgsAiClaudeConnectWidget widget( &router );
  QCOMPARE( widget.selectedCredentialMode(), QgsAiModelRouter::CredentialMode::ApiKey );
  QLineEdit *apiKeyEdit = widget.findChild<QLineEdit *>( u"aiClaudeApiKeyLineEdit"_s );
  QVERIFY( apiKeyEdit );
  QVERIFY( apiKeyEdit->placeholderText().contains( u"Saved locally"_s ) );
  apiKeyEdit->setText( u"  sk-ant-api03-new  "_s );
  QCOMPARE( widget.pendingApiKey(), u"sk-ant-api03-new"_s );
}

void TestQgsAiClaudeConnectWidget::missingCliDisablesConnect()
{
  const auto guard = isolateState();
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), u"/nonexistent/claude"_s );

  QgsAiModelRouter router;
  QgsAiClaudeConnectWidget widget( &router );
  widget.show();
  QVERIFY( waitForProbe( widget ) );

  QPushButton *connectButton = widget.findChild<QPushButton *>( u"aiClaudeConnectButton"_s );
  QLabel *installLink = widget.findChild<QLabel *>( u"aiClaudeInstallLink"_s );
  QVERIFY( !connectButton->isEnabled() );
  QVERIFY( installLink->isVisible() );

  // Starting programmatically (dock shortcut) reports the problem instead of hanging.
  widget.startConnect();
  QVERIFY( !widget.isBusy() );
  QLabel *status = widget.findChild<QLabel *>( u"aiClaudeStatusLabel"_s );
  QVERIFY( status->text().contains( u"not found"_s ) );
}

void TestQgsAiClaudeConnectWidget::connectFlowStoresTokenAndActivatesClaude()
{
  if ( !QgsAiPtySession::isSupported() )
    QSKIP( "Pseudo-terminal sessions are not supported on this platform." );
  const auto guard = isolateState();
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, happyCliScript() );
  QVERIFY( !cli.isEmpty() );
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), cli );

  QgsAiModelRouter router;
  QgsAiClaudeConnectWidget widget( &router );
  widget.show();
  QVERIFY( waitForProbe( widget ) );
  QSignalSpy stateSpy( &widget, &QgsAiClaudeConnectWidget::connectionStateChanged );
  QSignalSpy busySpy( &widget, &QgsAiClaudeConnectWidget::busyChanged );

  QPushButton *connectButton = widget.findChild<QPushButton *>( u"aiClaudeConnectButton"_s );
  QStackedWidget *subscriptionStack = widget.findChild<QStackedWidget *>( u"aiClaudeSubscriptionStack"_s );
  connectButton->click();
  QVERIFY( widget.isBusy() );
  QCOMPARE( subscriptionStack->currentIndex(), 1 );

  QTRY_COMPARE_WITH_TIMEOUT( subscriptionStack->currentIndex(), 2, 15000 );
  QTRY_VERIFY_WITH_TIMEOUT( !widget.isBusy(), 15000 );
  QVERIFY( widget.isConnected() );
  QCOMPARE( stateSpy.count(), 1 );
  QVERIFY( busySpy.count() >= 2 );

  QCOMPARE( QgsAiSecretStore::readSecret( QgsAiModelRouter::claudeSubscriptionTokenSettingKey() ), u"sk-ant-oat01-"_s + TOKEN_BODY );
  const QgsAiModelRouter::ProviderSettings claudeSettings = router.providerSettings( QgsAiModelRouter::Provider::Claude );
  QCOMPARE( claudeSettings.credentialMode, QgsAiModelRouter::CredentialMode::OAuth );
  QVERIFY( claudeSettings.enabled );
  QVERIFY( router.isProviderUsable( QgsAiModelRouter::Provider::Claude ) );
  QCOMPARE( router.activeProvider(), QgsAiModelRouter::Provider::Claude );

  // Account details come from the CLI probe that follows the login.
  QTRY_VERIFY_WITH_TIMEOUT( router.claudeSubscriptionInfo().email == u"tester@example.com"_s, 15000 );
  const QgsAiModelRouter::ClaudeSubscriptionInfo info = router.claudeSubscriptionInfo();
  QCOMPARE( info.source, u"claude-code-cli"_s );
  QCOMPARE( info.cliVersion, u"9.9.9"_s );
  QCOMPARE( info.subscriptionType, u"max"_s );
  QLabel *detail = widget.findChild<QLabel *>( u"aiClaudeConnectedDetailLabel"_s );
  QTRY_VERIFY_WITH_TIMEOUT( detail->text().contains( u"tester@example.com"_s ), 15000 );
  QVERIFY( detail->text().contains( u"9.9.9"_s ) );
  QLabel *expiry = widget.findChild<QLabel *>( u"aiClaudeExpiryLabel"_s );
  QVERIFY( expiry->text().contains( u"expires"_s ) );
  QLabel *status = widget.findChild<QLabel *>( u"aiClaudeStatusLabel"_s );
  QVERIFY( status->text().contains( u"Connected"_s ) );

  // A widget opened later starts on the Connected pane.
  QgsAiClaudeConnectWidget reopened( &router );
  QCOMPARE( reopened.findChild<QStackedWidget *>( u"aiClaudeSubscriptionStack"_s )->currentIndex(), 2 );
  QVERIFY( reopened.isConnected() );
}

void TestQgsAiClaudeConnectWidget::disconnectClearsTokenAndReturnsToApiKeyMode()
{
  const auto guard = isolateState();
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), u"/nonexistent/claude"_s );

  QgsAiModelRouter router;
  QgsAiModelRouter::ClaudeSubscriptionInfo info;
  info.source = u"claude-code-cli"_s;
  QVERIFY( router.connectClaudeSubscription( u"sk-ant-oat01-"_s + TOKEN_BODY, info ) );

  QgsAiClaudeConnectWidget widget( &router );
  widget.show();
  QSignalSpy stateSpy( &widget, &QgsAiClaudeConnectWidget::connectionStateChanged );
  QStackedWidget *subscriptionStack = widget.findChild<QStackedWidget *>( u"aiClaudeSubscriptionStack"_s );
  QCOMPARE( subscriptionStack->currentIndex(), 2 );

  QPushButton *disconnectButton = widget.findChild<QPushButton *>( u"aiClaudeDisconnectButton"_s );
  QVERIFY( disconnectButton->isEnabled() );
  disconnectButton->click();

  QCOMPARE( stateSpy.count(), 1 );
  QCOMPARE( subscriptionStack->currentIndex(), 0 );
  QVERIFY( !widget.isConnected() );
  QVERIFY( !QgsAiSecretStore::hasSecret( QgsAiModelRouter::claudeSubscriptionTokenSettingKey() ) );
  QCOMPARE( router.providerSettings( QgsAiModelRouter::Provider::Claude ).credentialMode, QgsAiModelRouter::CredentialMode::ApiKey );
  QVERIFY( !router.isProviderUsable( QgsAiModelRouter::Provider::Claude ) );
}

void TestQgsAiClaudeConnectWidget::failedConnectShowsErrorAndAdvanced()
{
  if ( !QgsAiPtySession::isSupported() )
    QSKIP( "Pseudo-terminal sessions are not supported on this platform." );
  const auto guard = isolateState();
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString cli = writeFakeCli( dir, failingCliScript() );
  QVERIFY( !cli.isEmpty() );
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), cli );

  QgsAiModelRouter router;
  QgsAiClaudeConnectWidget widget( &router );
  widget.show();
  QVERIFY( waitForProbe( widget ) );
  QSignalSpy stateSpy( &widget, &QgsAiClaudeConnectWidget::connectionStateChanged );

  QPushButton *connectButton = widget.findChild<QPushButton *>( u"aiClaudeConnectButton"_s );
  QStackedWidget *subscriptionStack = widget.findChild<QStackedWidget *>( u"aiClaudeSubscriptionStack"_s );
  QgsCollapsibleGroupBox *advanced = widget.findChild<QgsCollapsibleGroupBox *>( u"aiClaudeAdvancedGroupBox"_s );
  QVERIFY( advanced->isCollapsed() );
  connectButton->click();
  QVERIFY( widget.isBusy() );

  QTRY_VERIFY_WITH_TIMEOUT( !widget.isBusy(), 15000 );
  QCOMPARE( subscriptionStack->currentIndex(), 0 );
  QCOMPARE( stateSpy.count(), 0 );
  QVERIFY( !widget.isConnected() );
  QLabel *status = widget.findChild<QLabel *>( u"aiClaudeStatusLabel"_s );
  QVERIFY2( status->text().contains( u"code 4"_s ), qPrintable( status->text() ) );
  QVERIFY( status->text().contains( u"Something went wrong"_s ) );
  QCOMPARE( status->property( "status" ).toString(), u"error"_s );
  // The manual escape hatch is revealed.
  QVERIFY( !advanced->isCollapsed() );
}

void TestQgsAiClaudeConnectWidget::manualTokenIsExposedScrubbed()
{
  const auto guard = isolateState();
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), u"/nonexistent/claude"_s );

  QgsAiModelRouter router;
  QgsAiClaudeConnectWidget widget( &router );
  QLineEdit *manualToken = widget.findChild<QLineEdit *>( u"aiClaudeManualTokenLineEdit"_s );
  QVERIFY( manualToken );
  QVERIFY( widget.pendingManualToken().isEmpty() );
  manualToken->setText( u" sk-ant-oat01-abc \n def "_s );
  QCOMPARE( widget.pendingManualToken(), u"sk-ant-oat01-abcdef"_s );

  QComboBox *model = widget.findChild<QComboBox *>( u"aiClaudeModelComboBox"_s );
  model->setCurrentText( u"  claude-opus-4-8 "_s );
  QCOMPARE( widget.modelText(), u"claude-opus-4-8"_s );
}

QGSTEST_MAIN( TestQgsAiClaudeConnectWidget )
#include "testqgsaiclaudeconnectwidget.moc"
