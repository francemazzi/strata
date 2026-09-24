/***************************************************************************
    testqgsaiclaudeconnectwidget.cpp
    ---------------------
    begin                : June 2026
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
#include "ai/qgsaimodelrouter.h"
#include "qgsaisecretstoretestutils.h"
#include "qgstest.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>

using namespace Qt::StringLiterals;
class TestQgsAiClaudeConnectWidget : public QObject
{
    Q_OBJECT
  private slots:
    void init() { installTestSecretBackend(); }
    void subscriptionAndApiKeyWidget()
    {
      QgsAiModelRouter router;
      QgsAiClaudeConnectWidget widget( &router );
      QVERIFY( !widget.findChild<QLabel *>( u"aiClaudeSuspensionNotice"_s ) );
      QVERIFY( widget.findChild<QLabel *>( u"aiClaudeLoginStatus"_s ) );
      QVERIFY( widget.findChild<QPushButton *>( u"aiClaudeConnectButton"_s ) );
      QVERIFY( widget.findChild<QPushButton *>( u"aiClaudeLogoutButton"_s ) );
      QVERIFY( !widget.findChild<QLineEdit *>( u"aiClaudeManualTokenLineEdit"_s ) );
      auto *input = widget.findChild<QLineEdit *>( u"aiClaudeApiKeyLineEdit"_s );
      QVERIFY( input );
      QCOMPARE( input->echoMode(), QLineEdit::Password );
      input->setText( u"sk-ant-api03-test"_s );
      QCOMPARE( widget.pendingApiKey(), u"sk-ant-api03-test"_s );
      input->setText( u"sk-ant-oat01-pasted"_s );
      QCOMPARE( widget.pendingApiKey(), u"sk-ant-oat01-pasted"_s );
      QString error;
      QVERIFY( !router.storeApiKey( QgsAiModelRouter::Provider::Claude, widget.pendingApiKey(), &error ) );
      QSignalSpy cloud( &widget, &QgsAiClaudeConnectWidget::cloudRequested );
      QSignalSpy use( &widget, &QgsAiClaudeConnectWidget::useRequested );
      widget.findChild<QPushButton *>( u"aiClaudeCloudButton"_s )->click();
      widget.findChild<QPushButton *>( u"aiClaudeUseButton"_s )->click();
      QCOMPARE( cloud.count(), 1 );
      QCOMPARE( use.count(), 1 );
    }
};
QGSTEST_MAIN( TestQgsAiClaudeConnectWidget )
#include "testqgsaiclaudeconnectwidget.moc"
