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

#include "ai/qgsaiclaudecoderuntime.h"
#ifndef STRATA_CLAUDE_TRANSPORT_TESTS
#include "ai/qgsaimodelrouter.h"
#include "ai/qgsaiagentsessionmanager.h"
#include "ai/tools/qgsaitoolregistry.h"
#include "qgsaisecretstoretestutils.h"
#endif
#include "qgsaiclaudecodetestutils.h"
#include "qgssettings.h"
#include "qgstest.h"
#include <QSignalSpy>

#ifndef STRATA_CLAUDE_TRANSPORT_TESTS
class ClaudeSyntheticTool final : public QgsAiTool
{
  public:
    explicit ClaudeSyntheticTool( int &executions )
      : mExecutions( executions )
    {}
    QString name() const override { return u"list_layers"_s; }
    QString description() const override { return u"Synthetic mutation to verify approval routing"_s; }
    QJsonObject schema() const override { return { { u"type"_s, u"object"_s } }; }
    bool requiresApproval() const override { return true; }
    QgsAiToolResult execute( const QJsonObject & ) override
    {
      ++mExecutions;
      return QgsAiToolResult::ok( QJsonObject { { u"verified"_s, true } } );
    }

  private:
    int &mExecutions;
};

#endif

class TestQgsAiClaudeCodeRuntime : public QObject
{
    Q_OBJECT
  private:
    QJsonObject payload() const
    {
      return {
        { u"model"_s, u"sonnet"_s },
        { u"messages"_s, QJsonArray { QJsonObject { { u"role"_s, u"user"_s }, { u"content"_s, QJsonArray { QJsonObject { { u"type"_s, u"text"_s }, { u"text"_s, u"Synthetic question"_s } } } } } } },
        { u"tools"_s, QJsonArray { QJsonObject { { u"name"_s, u"list_layers"_s }, { u"description"_s, u"Synthetic test tool"_s }, { u"input_schema"_s, QJsonObject { { u"type"_s, u"object"_s } } } } } }
      };
    }
  private slots:
    void init()
    {
#ifndef STRATA_CLAUDE_TRANSPORT_TESTS
      installTestSecretBackend();
#endif
      QgsSettings().remove( u"ai/provider/claude"_s );
      QgsSettings().setValue( u"ai/provider/claude/code/executable"_s, QCoreApplication::applicationFilePath() );
      qunsetenv( "STRATA_TEST_CLAUDE_CASE" );
    }
    void cleanup() { qunsetenv( "STRATA_TEST_CLAUDE_CASE" ); }
    void streamingAndToolsUseOfficialProcess()
    {
      QgsAiClaudeCodeRuntime runtime;
      QSignalSpy done( &runtime, &QgsAiClaudeCodeRuntime::finished );
      QSignalSpy chunks( &runtime, &QgsAiClaudeCodeRuntime::progress );
      QSignalSpy usage( &runtime, &QgsAiClaudeCodeRuntime::usageReported );
      int executed = 0;
      runtime.start( payload(), [&executed]( const QString &name, const QJsonObject &, const QString &id ) {
        ++executed;
        return QgsAiClaudeMcpBridge::toolResult( name + id );
      } );
      QVERIFY( done.wait( 15000 ) );
      QVERIFY2( done[0][0].toBool(), qPrintable( done[0][2].toString() ) );
      QCOMPARE( done[0][1].toString(), u"Fixture response"_s );
      QCOMPARE( executed, 1 );
      QCOMPARE( chunks.size(), 1 );
      QCOMPARE( usage.size(), 1 );
      QCOMPARE( usage[0][0].toString(), u"claude-test-model"_s );
      QVERIFY( QgsAiClaudeCodeAuth::isConfigured() );
    }
    void subscriptionNeverUsesInheritedApiKey()
    {
      qputenv( "ANTHROPIC_API_KEY", "synthetic-api-key" );
      const auto restore = qScopeGuard( []() { qunsetenv( "ANTHROPIC_API_KEY" ); } );
      QVERIFY( !QgsAiClaudeCodeAuth::environment().contains( u"ANTHROPIC_API_KEY"_s ) );
      QCOMPARE( QgsAiClaudeCodeAuth::environment().value( u"CLAUDE_CONFIG_DIR"_s ), QgsAiClaudeCodeAuth::configDirectory() );
    }
    void rateLimitNeverLooksSuccessful()
    {
      qputenv( "STRATA_TEST_CLAUDE_CASE", "rate_limit" );
      QgsAiClaudeCodeRuntime runtime;
      QSignalSpy done( &runtime, &QgsAiClaudeCodeRuntime::finished );
      runtime.start( payload(), {} );
      QVERIFY( done.wait( 15000 ) );
      QVERIFY( !done[0][0].toBool() );
      QCOMPARE( done[0][3].toInt(), 429 );
      QVERIFY( !done[0][2].toString().contains( "sensitive"_L1 ) );
    }
    void cooldownPreventsAnotherAttempt()
    {
      QgsAiClaudeCodeAuth::setCooldown( 37 );
      QgsAiClaudeCodeRuntime runtime;
      QSignalSpy done( &runtime, &QgsAiClaudeCodeRuntime::finished );
      runtime.start( payload(), {} );
      QCOMPARE( done.count(), 1 );
      QCOMPARE( done[0][3].toInt(), 429 );
      QVERIFY( QgsAiClaudeCodeAuth::cooldownSeconds() >= 36 );
    }
    void historicalImagesStayMultimodal()
    {
      const QJsonObject image { { u"type"_s, u"image"_s }, { u"source"_s, QJsonObject { { u"type"_s, u"base64"_s }, { u"data"_s, u"synthetic-image-data"_s }, { u"media_type"_s, u"image/png"_s } } } };
      const QJsonArray messages { QJsonObject { { u"role"_s, u"user"_s }, { u"content"_s, QJsonArray { image } } }, QJsonObject { { u"role"_s, u"assistant"_s }, { u"content"_s, QJsonArray() } } };
      const QJsonArray content = QJsonDocument::fromJson( QgsAiClaudeCodeRuntime::inputMessage( messages ) ).object().value( u"message"_s ).toObject().value( u"content"_s ).toArray();
      QVERIFY( !content.first().toObject().value( u"text"_s ).toString().contains( "synthetic-image-data"_L1 ) );
      QCOMPARE( content[1].toObject().value( u"type"_s ).toString(), u"image"_s );
    }
    void apiLoginAndOldCliAreRejected_data()
    {
      QTest::addColumn<QByteArray>( "scenario" );
      QTest::newRow( "API credentials" ) << QByteArray( "api_key" );
      QTest::newRow( "signed out" ) << QByteArray( "signed_out" );
      QTest::newRow( "old version" ) << QByteArray( "old" );
    }
    void apiLoginAndOldCliAreRejected()
    {
      QFETCH( QByteArray, scenario );
      qputenv( "STRATA_TEST_CLAUDE_CASE", scenario );
      QgsAiClaudeCodeRuntime runtime;
      QSignalSpy done( &runtime, &QgsAiClaudeCodeRuntime::finished );
      runtime.start( payload(), {} );
      QVERIFY( done.wait( 15000 ) );
      QVERIFY( !done[0][0].toBool() );
    }
    void cancelStopsProcessWithoutCompletion()
    {
      qputenv( "STRATA_TEST_CLAUDE_CASE", "hang" );
      QgsAiClaudeCodeRuntime runtime;
      QSignalSpy done( &runtime, &QgsAiClaudeCodeRuntime::finished );
      runtime.start( payload(), {} );
      QTest::qWait( 500 );
      runtime.cancel();
      QTest::qWait( 100 );
      QCOMPARE( done.count(), 0 );
    }
#ifndef STRATA_CLAUDE_TRANSPORT_TESTS
    void agentToolsKeepStrataApprovals_data()
    {
      QTest::addColumn<bool>( "approve" );
      QTest::newRow( "approved" ) << true;
      QTest::newRow( "rejected" ) << false;
    }
    void agentToolsKeepStrataApprovals()
    {
      QFETCH( bool, approve );
      QgsSettings().setValue( u"ai/provider/claude/code/connected"_s, true );
      QgsAiModelRouter router;
      QVERIFY( router.setCredentialMode( QgsAiModelRouter::Provider::Claude, QgsAiModelRouter::CredentialMode::ClaudeCode ) );
      router.setActiveProvider( QgsAiModelRouter::Provider::Claude );
      int executions = 0;
      QgsAiToolRegistry tools;
      QVERIFY( tools.registerTool( std::make_unique<ClaudeSyntheticTool>( executions ) ) );
      QgsAiAgentSessionManager session( &router, nullptr, nullptr );
      session.setToolRegistry( &tools );
      session.setActiveAgent( u"ask_before_edits"_s );
      int approvals = 0;
      connect( &session, &QgsAiAgentSessionManager::toolApprovalRequested, &session, [&]( const QString &id, const QString &, const QVariantMap &, const QString & ) {
        ++approvals;
        QTimer::singleShot( 0, &session, [&session, id, approve]() { session.resolveToolApproval( id, approve ); } );
      } );
      QSignalSpy finished( &router, &QgsAiModelRouter::requestFinished );
      session.sendUserMessage( u"Run the synthetic test tool"_s );
      QVERIFY( finished.wait( 15000 ) );
      QCOMPARE( approvals, 1 );
      QCOMPARE( executions, approve ? 1 : 0 );
      int results = 0;
      for ( const auto &message : session.history() )
        if ( message.role == QgsAiChatRole::Tool )
          ++results;
      QCOMPARE( results, 1 );
      QVERIFY( !session.hasActiveRequest() );
    }
    void toolBudgetStopsTheOfficialAgent()
    {
      qputenv( "STRATA_TEST_CLAUDE_CASE", "budget" );
      QgsSettings().setValue( u"ai/provider/claude/code/connected"_s, true );
      QgsAiModelRouter router;
      QVERIFY( router.setCredentialMode( QgsAiModelRouter::Provider::Claude, QgsAiModelRouter::CredentialMode::ClaudeCode ) );
      router.setActiveProvider( QgsAiModelRouter::Provider::Claude );
      int executions = 0;
      QgsAiToolRegistry tools;
      QVERIFY( tools.registerTool( std::make_unique<ClaudeSyntheticTool>( executions ) ) );
      QgsAiAgentSessionManager session( &router, nullptr, nullptr );
      session.setToolRegistry( &tools );
      session.setActiveAgent( u"editor"_s );
      auto behavior = session.agentBehaviorSettings();
      behavior.maxToolIterationsPerTurn = 1;
      behavior.autoContinueToolBlocks = false;
      session.setAgentBehaviorSettings( behavior );
      session.sendUserMessage( u"Run the synthetic test tool"_s );
      QTRY_VERIFY_WITH_TIMEOUT( !session.hasActiveRequest(), 15000 );
      QCOMPARE( executions, 1 );
      bool paused = false;
      for ( const auto &message : session.history() )
        paused |= message.metadata.value( u"ui_kind"_s ).toString() == "tool_limit"_L1;
      QVERIFY( paused );
    }
#endif
    void recoveryHistoryIsData()
    {
      QJsonArray history { QJsonObject { { u"role"_s, u"user"_s }, { u"content"_s, QJsonArray { QJsonObject { { u"type"_s, u"tool_result"_s }, { u"content"_s, u"already completed"_s } } } } } };
      const QJsonObject message = QJsonDocument::fromJson( QgsAiClaudeCodeRuntime::inputMessage( history ) ).object();
      QVERIFY( message.value( u"message"_s ).toObject().value( u"content"_s ).toArray().first().toObject().value( u"text"_s ).toString().contains( "already completed"_L1 ) );
    }
};
#define main strataTestsMain
QGSTEST_MAIN( TestQgsAiClaudeCodeRuntime )
#undef main
int main( int argc, char **argv )
{
  return isFakeClaudeInvocation( argc, argv ) ? fakeClaudeMain( argc, argv ) : strataTestsMain( argc, argv );
}
#include "testqgsaiclaudecoderuntime.moc"
