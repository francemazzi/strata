/***************************************************************************
  testqgsaidatahubextracttool.cpp
  -------------------------------
  begin                : August 2026
***************************************************************************/

#include "qgsaidatahubextracttool.h"
#include "qgsaimodelrouter.h"
#include "qgsaisecretstore.h"
#include "qgsaitestloopbackserver.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>

using namespace Qt::StringLiterals;

namespace
{
  bool configurePlanForLoopback( QgsAiModelRouter &router, quint16 port )
  {
    QString error;
    if ( !router.setPlanSessionToken( u"strata-plan-datahub-test-token"_s, &error ) )
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
      { u"endpointId"_s, u"endpoint-42"_s },
      { u"catalogRecordId"_s, u"record-7"_s },
      { u"typeName"_s, u"CP:CadastralParcel"_s },
      { u"bbox"_s, QJsonArray { 9.0, 45.0, 10.0, 46.0 } },
      { u"format"_s, u"geojson"_s },
    };
  }

  void clearPlanSettings()
  {
    QgsSettings().remove( u"ai/provider/plan"_s );
    QgsAiSecretStore::removeSecret( u"ai/provider/plan/token"_s );
  }
} // namespace

class TestQgsAiDataHubExtractTool : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void validatesArgumentsWithoutNetwork();
    void pollsAndReturnsVerifiedArtifact();
    void pollsAndReturnsVerifiedZipArtifact();
    void includesAgentContextWhenSetOnRouter();
    void rejectsMalformedArtifact();
    void stopsAfterBoundedPolls();
};

void TestQgsAiDataHubExtractTool::init()
{
  clearPlanSettings();
}

void TestQgsAiDataHubExtractTool::cleanup()
{
  clearPlanSettings();
}

void TestQgsAiDataHubExtractTool::validatesArgumentsWithoutNetwork()
{
  QgsAiTestLoopbackServer server;
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );
  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiDataHubExtractTool tool( &router, 0, 2 );

  QJsonObject args = validArgs();
  args.remove( u"endpointId"_s );
  const QgsAiToolResult missingEndpoint = tool.execute( args );
  QVERIFY( !missingEndpoint.success );
  QVERIFY( missingEndpoint.errorMessage.contains( u"endpointId"_s ) );

  args = validArgs();
  args.insert( u"bbox"_s, QJsonArray { 10.0, 45.0, 9.0, 46.0 } );
  const QgsAiToolResult invertedBounds = tool.execute( args );
  QVERIFY( !invertedBounds.success );
  QVERIFY( invertedBounds.errorMessage.contains( u"minX < maxX"_s ) );
  QCOMPARE( server.requestCount, 0 );
}

void TestQgsAiDataHubExtractTool::pollsAndReturnsVerifiedArtifact()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job/42","status":"QUEUED","format":"geojson","createdAt":"2026-08-05T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"status":"running"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse(
         200,
         "OK",
         QByteArrayLiteral(
           R"({"status":"SUCCEEDED","artifact":{"downloadUrl":"http://127.0.0.1:9876/artifacts/job-42.geojson?signature=test","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","sizeBytes":"1234","format":"GEOJSON","expiresAt":"2030-01-02T03:04:05Z"},"provenance":{"sourceId":"source-42","endpointId":"endpoint-42","sourceName":"Official cadastre","serviceUri":"https://example.test/wfs","license":"CC-BY-4.0","attribution":"Official source","trustLevel":"OFFICIAL"}})"
         )
       );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiDataHubExtractTool tool( &router, 0, 4 );
  const QgsAiToolResult result = tool.execute( validArgs() );

  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonObject output = result.output.toObject();
  QCOMPARE( output.value( u"jobId"_s ).toString(), u"job/42"_s );
  QCOMPARE( output.value( u"status"_s ).toString(), u"completed"_s );
  QVERIFY( output.value( u"artifactMetadataVerified"_s ).toBool() );
  QCOMPARE( output.value( u"provenance"_s ).toObject().value( u"license"_s ).toString(), u"CC-BY-4.0"_s );
  QCOMPARE( output.value( u"artifact"_s ).toObject().value( u"sha256"_s ).toString(), QString( 64, 'a' ) );
  QCOMPARE( server.requestCount, 3 );

  QVERIFY( server.rawRequests.at( 0 ).startsWith( "POST /v1/datahub/extract HTTP/1.1\r\n" ) );
  QVERIFY( server.rawRequests.at( 1 ).startsWith( "GET /v1/datahub/jobs/job%2F42 HTTP/1.1\r\n" ) );
  QVERIFY( server.rawRequests.at( 0 ).contains( "Authorization: Bearer strata-plan-datahub-test-token\r\n" ) );

  const QJsonObject submitted = QJsonDocument::fromJson( server.requestBodies.at( 0 ) ).object();
  QCOMPARE( submitted.value( u"endpointId"_s ).toString(), u"endpoint-42"_s );
  QCOMPARE( submitted.value( u"catalogRecordId"_s ).toString(), u"record-7"_s );
  QCOMPARE( submitted.value( u"typeName"_s ).toString(), u"CP:CadastralParcel"_s );
  QVERIFY( !submitted.contains( u"agent_mode"_s ) );
  QVERIFY( !submitted.contains( u"agentRunId"_s ) );
  QVERIFY( !submitted.contains( u"agentClientSessionId"_s ) );
}

void TestQgsAiDataHubExtractTool::pollsAndReturnsVerifiedZipArtifact()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job/zip","status":"QUEUED","format":"zip","createdAt":"2026-08-22T10:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse(
         200,
         "OK",
         QByteArrayLiteral(
           R"({"status":"SUCCEEDED","artifact":{"downloadUrl":"http://127.0.0.1:9876/artifacts/job-zip.zip?signature=test","sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","sizeBytes":"2048","format":"ZIP","expiresAt":"2030-01-02T03:04:05Z"},"provenance":{"sourceId":"source-zip","endpointId":"endpoint-zip","sourceName":"AdE Catasto Lombardia","serviceUri":"https://example.test/GetDataset.php?dataset=LOMBARDIA.zip","license":"CC-BY-4.0","attribution":"Agenzia delle Entrate","trustLevel":"OFFICIAL"}})"
         )
       );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiDataHubExtractTool tool( &router, 0, 2 );
  QJsonObject args = validArgs();
  args.insert( u"format"_s, u"zip"_s );
  args.insert( u"typeName"_s, u"LOMBARDIA.zip"_s );
  const QgsAiToolResult result = tool.execute( args );

  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( result.output.toObject().value( u"artifact"_s ).toObject().value( u"format"_s ).toString(), u"zip"_s );
  const QJsonObject submitted = QJsonDocument::fromJson( server.requestBodies.at( 0 ) ).object();
  QCOMPARE( submitted.value( u"format"_s ).toString(), u"zip"_s );
}

void TestQgsAiDataHubExtractTool::includesAgentContextWhenSetOnRouter()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job/ctx","status":"QUEUED","format":"geojson","createdAt":"2026-08-05T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse(
         200,
         "OK",
         QByteArrayLiteral(
           R"({"status":"SUCCEEDED","artifact":{"downloadUrl":"http://127.0.0.1:9876/artifacts/job-ctx.geojson?signature=test","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","sizeBytes":"1234","format":"GEOJSON","expiresAt":"2030-01-02T03:04:05Z"},"provenance":{"sourceId":"source-42","endpointId":"endpoint-42","sourceName":"Official cadastre","serviceUri":"https://example.test/wfs","license":"CC-BY-4.0","attribution":"Official source","trustLevel":"OFFICIAL"}})"
         )
       );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  router.setAgentMode( u"ask_before_edits"_s );
  router.setPlanAgentRunId( u"run_extract_1"_s );
  router.setPlanClientSessionId( u"desktop_extract_1"_s );
  QgsAiDataHubExtractTool tool( &router, 0, 2 );
  const QgsAiToolResult result = tool.execute( validArgs() );

  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( server.requestCount, 2 );
  QVERIFY( server.rawRequests.at( 0 ).startsWith( "POST /v1/datahub/extract HTTP/1.1\r\n" ) );

  const QJsonObject submitted = QJsonDocument::fromJson( server.requestBodies.at( 0 ) ).object();
  QCOMPARE( submitted.value( u"agent_mode"_s ).toString(), u"ask_before_edits"_s );
  QCOMPARE( submitted.value( u"agentRunId"_s ).toString(), u"run_extract_1"_s );
  QCOMPARE( submitted.value( u"agentClientSessionId"_s ).toString(), u"desktop_extract_1"_s );
  QCOMPARE( submitted.value( u"endpointId"_s ).toString(), u"endpoint-42"_s );
}

void TestQgsAiDataHubExtractTool::rejectsMalformedArtifact()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job-bad","status":"QUEUED","format":"geojson","createdAt":"2026-08-05T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse(
         200,
         "OK",
         QByteArrayLiteral(
           "{\"status\":\"completed\",\"artifact\":{\"downloadUrl\":\"https://example.test/data.geojson\","
           "\"sha256\":\"not-a-hash\",\"sizeBytes\":12,\"format\":\"geojson\",\"expiresAt\":\"2030-01-02T03:04:05Z\"}}"
         )
       );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiDataHubExtractTool tool( &router, 0, 2 );
  const QgsAiToolResult result = tool.execute( validArgs() );

  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"SHA-256"_s ) );
}

void TestQgsAiDataHubExtractTool::stopsAfterBoundedPolls()
{
  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::jsonResponse( 202, "Accepted", QByteArrayLiteral( R"({"id":"job-slow","status":"QUEUED","format":"geojson","createdAt":"2026-08-05T16:00:00Z"})" ) )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"status":"running"})" ) );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiDataHubExtractTool tool( &router, 0, 2 );
  const QgsAiToolResult result = tool.execute( validArgs() );

  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"2 polling attempts"_s ) );
  QCOMPARE( server.requestCount, 3 );
}

QGSTEST_MAIN( TestQgsAiDataHubExtractTool )
#include "testqgsaidatahubextracttool.moc"
