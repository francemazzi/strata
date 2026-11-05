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

#include "ai/qgsaiclaudeconnectwidget.h"
#include "ai/qgsaiclaudecodeauth.h"
#include "ai/qgsaimodelrouter.h"
#include "qgsaiclaudecodetestutils.h"
#include "qgsaisecretstoretestutils.h"
#include "qgssettings.h"
#include "qgstest.h"
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QSignalSpy>
class TestQgsAiClaudeConnectWidget : public QObject
{
    Q_OBJECT
  private slots:
    void init()
    {
      installTestSecretBackend();
      QgsSettings().remove( u"ai/provider/claude"_s );
      QgsSettings().setValue( u"ai/provider/claude/code/executable"_s, QCoreApplication::applicationFilePath() );
      qunsetenv( "STRATA_TEST_CLAUDE_CASE" );
    }
    void subscriptionAndApiAreExplicit()
    {
      QgsAiModelRouter router;
      const auto active = router.activeProvider();
      QgsAiClaudeConnectWidget widget( &router );
      auto *mode = widget.findChild<QComboBox *>( u"aiClaudeCredentialMode"_s );
      auto *input = widget.findChild<QLineEdit *>( u"aiClaudeApiKeyLineEdit"_s );
      QVERIFY( mode && input );
      QCOMPARE( input->echoMode(), QLineEdit::Password );
      input->setText( u"sk-ant-api03-test"_s );
      QVERIFY( widget.usesSubscription() );
      QVERIFY( widget.pendingApiKey().isEmpty() );
      mode->setCurrentIndex( 1 );
      QCOMPARE( widget.pendingApiKey(), u"sk-ant-api03-test"_s );
      auto *auth = widget.findChild<QgsAiClaudeCodeAuth *>();
      QVERIFY( auth );
      QTRY_VERIFY( !auth->isBusy() );
      QCOMPARE( router.activeProvider(), active );
      QCOMPARE( router.providerSettings( QgsAiModelRouter::Provider::Claude ).credentialMode, QgsAiModelRouter::CredentialMode::ApiKey );
      QVERIFY( !widget.findChild<QLineEdit *>( u"aiClaudeManualTokenLineEdit"_s ) );
    }
    void existingApiModelIsPreserved()
    {
      QgsAiModelRouter router;
      QVERIFY( router.storeApiKey( QgsAiModelRouter::Provider::Claude, u"sk-ant-api03-synthetic"_s ) );
      auto settings = router.providerSettings( QgsAiModelRouter::Provider::Claude );
      settings.model = u"custom-claude-model"_s;
      router.setProviderSettings( QgsAiModelRouter::Provider::Claude, settings );
      QgsSettings().remove( u"ai/provider/claude/apiModel"_s );
      QgsAiClaudeConnectWidget widget( &router );
      QVERIFY( !widget.usesSubscription() );
      QCOMPARE( widget.modelText(), u"custom-claude-model"_s );
    }
    void connectionDoesNotSelectTheProvider()
    {
      qputenv( "STRATA_TEST_CLAUDE_CASE", "signed_out" );
      QgsAiModelRouter router;
      const auto active = router.activeProvider();
      QgsAiClaudeConnectWidget widget( &router );
      auto *auth = widget.findChild<QgsAiClaudeCodeAuth *>();
      QTRY_VERIFY( !auth->isBusy() );
      qunsetenv( "STRATA_TEST_CLAUDE_CASE" );
      widget.findChild<QPushButton *>( u"aiClaudeConnectButton"_s )->click();
      QTRY_VERIFY( QgsAiClaudeCodeAuth::isConfigured() );
      QCOMPARE( router.providerSettings( QgsAiModelRouter::Provider::Claude ).credentialMode, QgsAiModelRouter::CredentialMode::ApiKey );
      QCOMPARE( router.activeProvider(), active );
      QCOMPARE( widget.modelText(), u"sonnet"_s );
      QSignalSpy use( &widget, &QgsAiClaudeConnectWidget::useRequested );
      widget.findChild<QPushButton *>( u"aiClaudeUseButton"_s )->click();
      QCOMPARE( use.count(), 1 );
    }
};
#define main strataTestsMain
QGSTEST_MAIN( TestQgsAiClaudeConnectWidget )
#undef main
int main( int argc, char **argv )
{
  return isFakeClaudeInvocation( argc, argv ) ? fakeClaudeMain( argc, argv ) : strataTestsMain( argc, argv );
}
#include "testqgsaiclaudeconnectwidget.moc"
