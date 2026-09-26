/***************************************************************************
  testqgsaiwebsearchtool.cpp
  --------------------------
  begin                : September 2026
***************************************************************************/

#include "qgsaiagentpolicy.h"
#include "qgsaimcpcalltool.h"
#include "qgsaimodelrouter.h"
#include "qgsaisecretstore.h"
#include "qgsaisecretstoretestutils.h"
#include "qgsaitaskrunner.h"
#include "qgsaitestloopbackserver.h"
#include "qgsaitoolregistry.h"
#include "qgsaiwebsearchtool.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace
{
  bool configurePlanForLoopback( QgsAiModelRouter &router, quint16 port )
  {
    QString error;
    if ( !router.setPlanSessionToken( u"strata-plan-web-search-test-token"_s, &error ) )
      return false;

    QgsAiModelRouter::ProviderSettings settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
    settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( port );
    settings.model = u"managed-plan"_s;
    settings.enabled = true;
    router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
    return true;
  }

  //! Value of a request header, case-insensitive.
  QByteArray headerValue( const QByteArray &rawRequest, const QByteArray &name )
  {
    const QList<QByteArray> lines = rawRequest.left( rawRequest.indexOf( "\r\n\r\n" ) ).split( '\n' );
    for ( const QByteArray &line : lines )
    {
      const int colon = line.indexOf( ':' );
      if ( colon > 0 && line.left( colon ).trimmed().toLower() == name.toLower() )
        return line.mid( colon + 1 ).trimmed();
    }
    return QByteArray();
  }

  void clearPlanSettings()
  {
    QgsSettings().remove( u"ai/provider/plan"_s );
    QgsAiSecretStore::removeSecret( u"ai/provider/plan/token"_s );
  }
} // namespace

class TestQgsAiWebSearchTool : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void returnsSearchResults();
    void stopDuringPendingSearchReturnsCanceled();
    void mcpWriteWithUnknownOutcomeIsNotRunTwice();
    void mcpReadsCarryNoIdempotencyKey();
};

void TestQgsAiWebSearchTool::init()
{
  installTestSecretBackend();
  clearPlanSettings();
}

void TestQgsAiWebSearchTool::cleanup()
{
  clearPlanSettings();
}

void TestQgsAiWebSearchTool::returnsSearchResults()
{
  QgsAiTestLoopbackServer server;
  server.responses << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"results\":[{\"title\":\"Roads\"}]}" ) );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiWebSearchTool tool( &router );
  const QgsAiToolResult result = tool.execute( QJsonObject { { u"query"_s, u"roads padova"_s } } );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QVERIFY( !result.canceled );
  QVERIFY( server.rawRequests.at( 0 ).startsWith( "POST /v1/tools/web-search HTTP/1.1\r\n" ) );
}

void TestQgsAiWebSearchTool::stopDuringPendingSearchReturnsCanceled()
{
  QgsAiTestLoopbackServer server;
  QgsAiTestLoopbackServer::ScriptedResponse pending = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"results\":[]}" ) );
  pending.responseDelayMs = 60000;
  server.responses << pending;
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiWebSearchTool tool( &router );
  QTimer::singleShot( 200, []() { qgsAiCancelActiveBackgroundTool(); } );
  QElapsedTimer elapsed;
  elapsed.start();
  const QgsAiToolResult result = tool.execute( QJsonObject { { u"query"_s, u"roads padova"_s } } );
  QVERIFY( !result.success );
  QVERIFY( result.canceled );
  // Well under the search timeout: Stop aborted the pending request.
  QVERIFY2( elapsed.elapsed() < 10000, "Stop did not interrupt the pending search" );
  QVERIFY( !qgsAiHasActiveBackgroundTool() );
}

void TestQgsAiWebSearchTool::mcpWriteWithUnknownOutcomeIsNotRunTwice()
{
  QgsAiTestLoopbackServer server;
  // The gateway answers after the client gave up: the note may exist anyway.
  QgsAiTestLoopbackServer::ScriptedResponse late = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"tool":"mcp__notes__create","output":"created","creditsCharged":1})" ) );
  late.responseDelayMs = 3000;
  server.responses << late << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"tool":"mcp__notes__create","output":"created","creditsCharged":0})" ) );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  auto proxy = std::make_unique<QgsAiMcpCallTool>( &router );
  proxy->setRequestTimeoutMs( 300 );
  QgsAiMcpCallTool *tool = proxy.get();
  QgsAiToolRegistry registry;
  registry.setMcpCallProxy( std::move( proxy ) );
  QgsAiManagedMcpTool definition;
  definition.name = u"mcp__notes__create"_s;
  definition.mutating = true;
  registry.setManagedMcpTools( { definition } );

  const QJsonObject args { { u"title"_s, u"Survey"_s } };
  const QgsAiToolResult first = registry.execute( u"mcp__notes__create"_s, args, u"call_1"_s );
  QVERIFY( !first.success );
  // Not "timed out", which a model reads as "try again".
  QVERIFY2( first.errorMessage.contains( u"Outcome uncertain"_s ), qPrintable( first.errorMessage ) );
  QCOMPARE( first.output.toObject().value( u"retryable"_s ).toBool( true ), false );
  const QByteArray firstKey = headerValue( server.rawRequests.at( 0 ), "Idempotency-Key" );
  QVERIFY( firstKey.startsWith( "strata-call_1-" ) );
  QCOMPARE( QJsonDocument::fromJson( server.requestBodies.at( 0 ) ).object().value( u"idempotencyKey"_s ).toString().toUtf8(), firstKey );

  // The model repeats it anyway, under a new call id: the same key lets the gateway replay the first outcome.
  tool->setRequestTimeoutMs( 20000 );
  const QgsAiToolResult retry = registry.execute( u"mcp__notes__create"_s, args, u"call_2"_s );
  QVERIFY2( retry.success, qPrintable( retry.errorMessage ) );
  QCOMPARE( headerValue( server.rawRequests.at( 1 ), "Idempotency-Key" ), firstKey );

  // Once the outcome is known, the same request is a new call with its own key.
  const QgsAiToolResult next = registry.execute( u"mcp__notes__create"_s, args, u"call_3"_s );
  QVERIFY( next.success );
  QVERIFY( headerValue( server.rawRequests.at( 2 ), "Idempotency-Key" ).startsWith( "strata-call_3-" ) );
}

void TestQgsAiWebSearchTool::mcpReadsCarryNoIdempotencyKey()
{
  QgsAiTestLoopbackServer server;
  QgsAiTestLoopbackServer::ScriptedResponse late = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"tool":"mcp__nominatim__geocode","output":"[]","creditsCharged":1})" ) );
  late.responseDelayMs = 3000;
  server.responses << late;
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiMcpCallTool tool( &router );
  tool.setRequestTimeoutMs( 300 );
  const QgsAiToolResult result = tool.executeNamed( u"mcp__nominatim__geocode"_s, QJsonObject { { u"q"_s, u"Padova"_s } }, false, u"call_1"_s );
  QVERIFY( !result.success );
  // A read can simply be repeated.
  QVERIFY( !result.errorMessage.contains( u"Outcome uncertain"_s ) );
  QVERIFY( headerValue( server.rawRequests.at( 0 ), "Idempotency-Key" ).isEmpty() );
}

QGSTEST_MAIN( TestQgsAiWebSearchTool )
#include "testqgsaiwebsearchtool.moc"
