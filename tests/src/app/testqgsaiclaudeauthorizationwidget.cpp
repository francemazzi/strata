/***************************************************************************
    testqgsaiclaudeauthorizationwidget.cpp
    ------------------------------------
    begin                : November 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************/
/*                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "ai/qgsaiclaudeauthorizationwidget.h"
#include "ai/qgsaiclaudecodeauth.h"
#include "qgsaiclaudecodetestutils.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>

class TestQgsAiClaudeAuthorizationWidget : public QObject
{
    Q_OBJECT

  private slots:
    void init()
    {
      QgsSettings().remove( u"ai/provider/claude/code"_s );
      QgsSettings().setValue( u"ai/provider/claude/code/executable"_s, QCoreApplication::applicationFilePath() );
      QFile::remove( QDir( QgsAiClaudeCodeAuth::configDirectory() ).filePath( u"claude-test-manual-login"_s ) );
    }

    void cleanup()
    {
      qunsetenv( "STRATA_TEST_CLAUDE_CASE" );
      QgsSettings().remove( u"ai/provider/claude/code"_s );
    }

    void manualSignIn_data()
    {
      QTest::addColumn<QByteArray>( "scenario" );
      QTest::newRow( "split URL from stdout" ) << QByteArray( "manual_auth_stdout" );
      QTest::newRow( "split URL from stderr" ) << QByteArray( "manual_auth_stderr" );
    }

    void manualSignIn()
    {
      QFETCH( QByteArray, scenario );
      qputenv( "STRATA_TEST_CLAUDE_CASE", scenario );

      QgsAiClaudeCodeAuth auth;
      QUrl openedUrl;
      QgsAiClaudeAuthorizationWidget widget( &auth, nullptr, [&openedUrl]( const QUrl &url ) {
        openedUrl = url;
        return false;
      } );
      auto *hint = widget.findChild<QLabel *>( u"aiClaudeAuthorizationHint"_s );
      auto *link = widget.findChild<QLineEdit *>( u"aiClaudeAuthorizationLink"_s );
      auto *open = widget.findChild<QPushButton *>( u"aiClaudeOpenAuthorizationButton"_s );
      auto *copy = widget.findChild<QPushButton *>( u"aiClaudeCopyAuthorizationButton"_s );
      auto *code = widget.findChild<QLineEdit *>( u"aiClaudeAuthorizationCode"_s );
      auto *submit = widget.findChild<QPushButton *>( u"aiClaudeSubmitAuthorizationButton"_s );
      QVERIFY( hint && link && open && copy && code && submit );
      QVERIFY( widget.isHidden() );

      QClipboard *clipboard = QApplication::clipboard();
      const QString originalClipboard = clipboard->text();
      const auto restoreClipboard = qScopeGuard( [clipboard, originalClipboard]() { clipboard->setText( originalClipboard ); } );

      auth.login();
      QTRY_VERIFY( widget.isVisible() );
      const QString expected = u"https://claude.com/cai/oauth/authorize?state=synthetic&redirect_uri=https%3A%2F%2Fplatform.claude.com%2Foauth%2Fcode%2Fcallback"_s;
      QCOMPARE( link->text(), expected );
      QVERIFY( link->isReadOnly() );
      QCOMPARE( code->echoMode(), QLineEdit::Password );

      open->click();
      QCOMPARE( openedUrl, QUrl( expected ) );
      QVERIFY( hint->text().contains( u"Could not open the browser"_s ) );
      copy->click();
      QCOMPARE( clipboard->text(), expected );
      QVERIFY( !auth.submitAuthorizationCode( u"first\nsecond"_s ) );

      code->setText( u"synthetic-code#state"_s );
      submit->click();
      QVERIFY( code->text().isEmpty() );
      QTRY_VERIFY( QgsAiClaudeCodeAuth::isConfigured() );
      QVERIFY( widget.isHidden() );
      QVERIFY( link->text().isEmpty() );

      auth.login();
      QTRY_VERIFY( widget.isVisible() );
      code->setText( u"another-synthetic-code"_s );
      auth.cancel();
      QVERIFY( widget.isHidden() );
      QVERIFY( link->text().isEmpty() );
      QVERIFY( code->text().isEmpty() );
    }

    void officialCliProvidesManualLink()
    {
      const QString executable = qEnvironmentVariable( "STRATA_TEST_CLAUDE_REAL_EXECUTABLE" );
      if ( executable.isEmpty() )
        QSKIP( "Set STRATA_TEST_CLAUDE_REAL_EXECUTABLE to verify the official CLI without signing in." );

      QgsSettings().setValue( u"ai/provider/claude/code/executable"_s, executable );
      QgsAiClaudeCodeAuth auth;
      QgsAiClaudeAuthorizationWidget widget( &auth );
      auth.login();
      QTRY_VERIFY_WITH_TIMEOUT( widget.isVisible(), 20000 );
      auto *link = widget.findChild<QLineEdit *>( u"aiClaudeAuthorizationLink"_s );
      QVERIFY( link && QUrl( link->text() ).isValid() );
      auth.cancel();
      QVERIFY( widget.isHidden() );
      QVERIFY( link->text().isEmpty() );
    }
};

#define main strataTestsMain
QGSTEST_MAIN( TestQgsAiClaudeAuthorizationWidget )
#undef main
int main( int argc, char **argv )
{
  return isFakeClaudeInvocation( argc, argv ) ? fakeClaudeMain( argc, argv ) : strataTestsMain( argc, argv );
}
#include "testqgsaiclaudeauthorizationwidget.moc"
