/***************************************************************************
  testqgsaitreesdetecttool.cpp
  ----------------------------
  begin                : August 2026
***************************************************************************/

#include "qgsaimodelrouter.h"
#include "qgsaisecretstore.h"
#include "qgsaitestloopbackserver.h"
#include "qgsaitreesdetecttool.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

using namespace Qt::StringLiterals;

namespace
{
  bool configurePlanForLoopback( QgsAiModelRouter &router, quint16 port )
  {
    QString error;
    if ( !router.setPlanSessionToken( u"strata-plan-trees-test-token"_s, &error ) )
      return false;

    QgsAiModelRouter::ProviderSettings settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
    settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( port );
    settings.model = u"managed-plan"_s;
    settings.enabled = true;
    router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
    return true;
  }

  QJsonObject validArgs()
  {
    return {
      { u"bbox"_s, QJsonArray { 9.15, 45.44, 9.22, 45.50 } },
      { u"region"_s, u"lombardia"_s },
      { u"format"_s, u"geojson"_s },
    };
  }

  QByteArray succeededJobJson()
  {
    return QByteArrayLiteral(
      R"({"status":"SUCCEEDED","artifact":{"downloadUrl":"http://127.0.0.1:9876/artifacts/job-trees.geojson?signature=test","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","sizeBytes":"1234","format":"GEOJSON","expiresAt":"2030-01-02T03:04:05Z"},"provenance":{"sourceId":"source-ade","endpointId":"endpoint-ade","sourceName":"AdE Catasto Lombardia","serviceUri":"https://example.test/GetDataset.php?dataset=LOMBARDIA.zip","license":"CC-BY-4.0","attribution":"Agenzia delle Entrate","trustLevel":"OFFICIAL"},"quality":{"precisionClass":"method_declared","confidenceKind":"placeholder","imageryClass":"synthetic","counts":{"detected":2,"privateExcluded":0,"closedCanopyExcluded":0,"belowThreshold":0,"kept":2}}})"
    );
  }

  void clearPlanSettings()
  {
    QgsSettings().remove( u"ai/provider/plan"_s );
    QgsAiSecretStore::removeSecret( u"ai/provider/plan/token"_s );
  }
} // namespace

class TestQgsAiTreesDetectTool : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void validatesArgumentsWithoutNetwork();
    void pollsAndReturnsVerifiedArtifact();
    void acceptsOmittedRegion();
    void includesAgentContextWhenSetOnRouter();
    void rejectsMalformedArtifact();
    void stopsAfterBoundedPolls();
};

void TestQgsAiTreesDetectTool::init()
{
  clearPlanSettings();
}

void TestQgsAiTreesDetectTool::cleanup()
{
  clearPlanSettings();
}

void TestQgsAiTreesDetectTool::validatesArgumentsWithoutNetwork()
{
  QgsAiTestLoopbackServer server;
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );
  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiTreesDetectTool tool( &router, 0, 2 );

  QJsonObject args = validArgs();
  args.insert( u"region"_s, u"narnia"_s );
  const QgsAiToolResult outsideItaly = tool.execute( args );
  QVERIFY( !outsideItaly.success );
  QVERIFY( outsideItaly.errorMessage.contains( u"region"_s ) );

  args = validArgs();
  args.insert( u"bbox"_s, QJsonArray { 10.5, 44.9, 10.4, 45.0 } );
  const QgsAiToolResult invertedBounds = tool.execute( args );
  QVERIFY( !invertedBounds.success );
  QVERIFY( invertedBounds.errorMessage.contains( u"minX < maxX"_s ) );
  QCOMPARE( server.requestCount, 0 );
}

void TestQgsAiTreesDetectTool::pollsAndReturnsVerifiedArtifact()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job/trees","status":"QUEUED","format":"geojson","createdAt":"2026-08-22T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"status":"running"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", succeededJobJson() );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiTreesDetectTool tool( &router, 0, 4 );
  const QgsAiToolResult result = tool.execute( validArgs() );

  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonObject output = result.output.toObject();
  QCOMPARE( output.value( u"jobId"_s ).toString(), u"job/trees"_s );
  QCOMPARE( output.value( u"status"_s ).toString(), u"completed"_s );
  QVERIFY( output.value( u"artifactMetadataVerified"_s ).toBool() );
  QCOMPARE( output.value( u"provenance"_s ).toObject().value( u"license"_s ).toString(), u"CC-BY-4.0"_s );
  QCOMPARE( output.value( u"artifact"_s ).toObject().value( u"sha256"_s ).toString(), QString( 64, 'a' ) );
  QCOMPARE( output.value( u"quality"_s ).toObject().value( u"imageryClass"_s ).toString(), u"synthetic"_s );
  QVERIFY( output.value( u"quality_checks"_s ).toObject().value( u"quality_reported"_s ).toBool() );
  QCOMPARE( server.requestCount, 3 );

  QVERIFY( server.rawRequests.at( 0 ).startsWith( "POST /v1/trees/detect HTTP/1.1\r\n" ) );
  QVERIFY( server.rawRequests.at( 1 ).startsWith( "GET /v1/trees/jobs/job%2Ftrees HTTP/1.1\r\n" ) );
  QVERIFY( server.rawRequests.at( 0 ).contains( "Authorization: Bearer strata-plan-trees-test-token\r\n" ) );

  const QJsonObject submitted = QJsonDocument::fromJson( server.requestBodies.at( 0 ) ).object();
  QCOMPARE( submitted.value( u"region"_s ).toString(), u"lombardia"_s );
  QCOMPARE( submitted.value( u"format"_s ).toString(), u"geojson"_s );
  QVERIFY( !submitted.contains( u"agent_mode"_s ) );
}

void TestQgsAiTreesDetectTool::acceptsOmittedRegion()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job/infer","status":"QUEUED","format":"geojson","region":"lombardia","createdAt":"2026-08-22T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", succeededJobJson() );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiTreesDetectTool tool( &router, 0, 2 );
  QJsonObject args = validArgs();
  args.remove( u"region"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonObject submitted = QJsonDocument::fromJson( server.requestBodies.at( 0 ) ).object();
  QVERIFY( !submitted.contains( u"region"_s ) );
  QCOMPARE( submitted.value( u"format"_s ).toString(), u"geojson"_s );
}

void TestQgsAiTreesDetectTool::includesAgentContextWhenSetOnRouter()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job/ctx","status":"QUEUED","format":"geojson","createdAt":"2026-08-22T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", succeededJobJson() );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  router.setAgentMode( u"ask_before_edits"_s );
  router.setPlanAgentRunId( u"run_trees_1"_s );
  router.setPlanClientSessionId( u"desktop_trees_1"_s );
  QgsAiTreesDetectTool tool( &router, 0, 2 );
  const QgsAiToolResult result = tool.execute( validArgs() );

  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonObject submitted = QJsonDocument::fromJson( server.requestBodies.at( 0 ) ).object();
  QCOMPARE( submitted.value( u"agent_mode"_s ).toString(), u"ask_before_edits"_s );
  QCOMPARE( submitted.value( u"agentRunId"_s ).toString(), u"run_trees_1"_s );
  QCOMPARE( submitted.value( u"agentClientSessionId"_s ).toString(), u"desktop_trees_1"_s );
}

void TestQgsAiTreesDetectTool::rejectsMalformedArtifact()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job-bad","status":"QUEUED","format":"geojson","createdAt":"2026-08-22T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse(
         200,
         "OK",
         QByteArrayLiteral(
           "{\"status\":\"completed\",\"artifact\":{\"downloadUrl\":\"https://example.test/trees.geojson\","
           "\"sha256\":\"not-a-hash\",\"sizeBytes\":12,\"format\":\"geojson\",\"expiresAt\":\"2030-01-02T03:04:05Z\"}}"
         )
       );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiTreesDetectTool tool( &router, 0, 2 );
  const QgsAiToolResult result = tool.execute( validArgs() );

  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"SHA-256"_s ) );
}

void TestQgsAiTreesDetectTool::stopsAfterBoundedPolls()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job-slow","status":"QUEUED","format":"geojson","createdAt":"2026-08-22T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"status":"running"})" ) );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiTreesDetectTool tool( &router, 0, 2 );
  const QgsAiToolResult result = tool.execute( validArgs() );

  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"2 polling attempts"_s ) );
  QCOMPARE( server.requestCount, 3 );
}

QGSTEST_MAIN( TestQgsAiTreesDetectTool )
#include "testqgsaitreesdetecttool.moc"
