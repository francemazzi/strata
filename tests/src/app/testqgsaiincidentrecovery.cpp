// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgstest.h"
#include "qgsapplication.h"
#include "qgsrasterlayer.h"
#include "qgsproject.h"
#include "qgsvectorlayer.h"
#include "qgsvectordataprovider.h"
#include "ai/qgsaiagentsessionmanager.h"
#include "ai/qgsaichathistorystore.h"
#include "ai/qgsaifilecontextprovider.h"
#include "ai/qgsaireviewpatchengine.h"
#include "ai/tools/qgsailayertools.h"
#include "ai/tools/qgsaitoolregistry.h"
#include "ai/qgsaimodelrouter.h"
#include "ai/tools/qgsaiarcgissnapshot.h"
#include "ai/tools/qgsaigeographicverification.h"
#include "qgsaisecretstoretestutils.h"
#include "qgsaitestloopbackserver.h"
#include <QBuffer>
#include <memory>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QSettings>
using namespace Qt::StringLiterals;

class TestQgsAiIncidentRecovery : public QObject
{
    Q_OBJECT
  private:
    void configure( QgsAiModelRouter &router, quint16 port )
    {
      QVERIFY( router.setPlanSessionToken( u"strata_synthetic_opaque"_s ) );
      auto settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
      settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( port );
      settings.model = u"managed-plan"_s;
      settings.enabled = true;
      router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
    }
    QString start( QgsAiModelRouter &router )
    {
      QgsAiChatMessage message;
      message.content = u"Synthetic recovery request"_s;
      return router.startChatRequest( QgsAiModelRouter::Provider::Plan, { message }, true );
    }
    QgsAiTestLoopbackServer::ScriptedResponse busy( int seconds )
    {
      return QgsAiTestLoopbackServer::jsonResponse(
        503,
        "Unavailable",
        QJsonDocument( QJsonObject { { u"error"_s, u"managed_provider_busy"_s }, { u"message"_s, u"Provider busy"_s }, { u"recoverable"_s, true }, { u"retry_after"_s, seconds } } ).toJson()
      );
    }
  private slots:
    void initTestCase()
    {
      QCoreApplication::setOrganizationName( u"StrataIncidentSynthetic"_s );
      QCoreApplication::setApplicationName( QUuid::createUuid().toString() );
      QgsApplication::initQgis();
    }
    void init()
    {
      installTestSecretBackend();
      QSettings().clear();
    }
    void cleanupTestCase()
    {
      QSettings().clear();
      QgsApplication::exitQgis();
    }
    void respectsWaitAndCancel()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << busy( 120 );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      QSignalSpy waiting( &router, &QgsAiModelRouter::retryWaiting );
      const auto id = start( router );
      QTRY_VERIFY( !waiting.isEmpty() );
      QCOMPARE( waiting.first()[2].toInt(), 120 );
      QTest::qWait( 350 );
      QCOMPARE( server.requestCount, 1 );
      router.cancelRequest( id );
      QTest::qWait( 350 );
      QCOMPARE( server.requestCount, 1 );
      QVERIFY( !router.hasActiveRequest( id ) );
    }
    void retriesSameRequestAfterWait()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << busy( 1 ) << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", R"({"content":"Recovered"})" );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      QSignalSpy finished( &router, &QgsAiModelRouter::requestFinished );
      QElapsedTimer timer;
      timer.start();
      start( router );
      QTRY_COMPARE_WITH_TIMEOUT( finished.size(), 1, 5000 );
      QVERIFY( timer.elapsed() >= 1000 );
      QVERIFY( finished.first()[1].toBool() );
      QCOMPARE( server.requestCount, 2 );
      QCOMPARE( server.requestBodies[0], server.requestBodies[1] );
    }
    void limitsRetries()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << busy( 0 );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      QSignalSpy finished( &router, &QgsAiModelRouter::requestFinished );
      start( router );
      QTRY_COMPARE( finished.size(), 1 );
      QCOMPARE( server.requestCount, 4 );
      QVERIFY( !finished.first()[1].toBool() );
    }
    void cancelsOnModelChange()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << busy( 1 );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      QSignalSpy waiting( &router, &QgsAiModelRouter::retryWaiting );
      QSignalSpy finished( &router, &QgsAiModelRouter::requestFinished );
      start( router );
      QTRY_VERIFY( !waiting.isEmpty() );
      auto settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
      settings.model = u"different-model"_s;
      router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
      QTRY_COMPARE( finished.size(), 1 );
      QCOMPARE( server.requestCount, 1 );
      QVERIFY( !finished.first()[1].toBool() );
    }
    void rejectsWaitBeyondTenMinutes()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << busy( 601 );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      QSignalSpy finished( &router, &QgsAiModelRouter::requestFinished );
      start( router );
      QTRY_COMPARE( finished.size(), 1 );
      QCOMPARE( server.requestCount, 1 );
      QVERIFY( !finished.first()[1].toBool() );
    }
    void restoresWaitWithoutAutomaticRequest()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << busy( 120 );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      router.setActiveProvider( QgsAiModelRouter::Provider::Plan );
      QTemporaryDir root;
      QgsAiFileContextProvider files( root.path() );
      QgsAiReviewPatchEngine review;
      QgsAiChatHistoryStore store( &files );
      {
        QgsAiAgentSessionManager manager( &router, &files, &review );
        manager.setHistoryStore( &store );
        manager.sendUserMessage( u"Synthetic interrupted request"_s );
        QTRY_VERIFY( !manager.history().isEmpty() && manager.history().last().metadata.contains( u"retry_at_ms"_s ) );
        QCOMPARE( server.requestCount, 1 );
      }
      QgsAiAgentSessionManager restored( &router, &files, &review );
      restored.setHistoryStore( &store );
      QTRY_VERIFY( !restored.history().isEmpty() );
      QVERIFY( !restored.hasActiveRequest() );
      QCOMPARE( restored.history().last().metadata.value( u"ui_kind"_s ).toString(), u"request_error"_s );
      QString error;
      QVERIFY( !restored.resumeLastInterruptedTurn( &error ) );
      QVERIFY2( error.contains( u"wait ends"_s ), qPrintable( error ) );
      QTest::qWait( 300 );
      QCOMPARE( server.requestCount, 1 );
      restored.clearHistory();
      QgsAiAgentSessionManager cleared( &router, &files, &review );
      cleared.setHistoryStore( &store );
      QTest::qWait( 100 );
      QVERIFY( cleared.history().isEmpty() );
    }
    void rejectsPartialOutputRetry()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << QgsAiTestLoopbackServer::sseResponse(
        { "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"Partial\"}}\n\n",
          "data: {\"error\":{\"code\":503,\"message\":\"busy\",\"error_code\":\"managed_provider_busy\",\"recoverable\":true,\"retry_after\":0}}\n\n" }
      );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      QSignalSpy finished( &router, &QgsAiModelRouter::requestFinished );
      start( router );
      QTRY_COMPARE( finished.size(), 1 );
      QCOMPARE( server.requestCount, 1 );
      QVERIFY( !finished.first()[1].toBool() );
    }
    void verifiesOpaqueIdentityAndRevocation()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", R"({"id":"user-synthetic"})" ) << QgsAiTestLoopbackServer::jsonResponse( 401, "Unauthorized", R"({"error":"revoked"})" );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      QCOMPARE( router.verifiedPlanAccountId(), u"user-synthetic"_s );
      QVERIFY( server.lastRawRequest().startsWith( "GET /v1/auth/me" ) );
      QCOMPARE( router.verifiedPlanAccountId(), u"user-synthetic"_s );
      QCOMPARE( server.requestCount, 1 );
      QVERIFY( router.verifiedPlanAccountId( true ).isEmpty() );
      QCOMPARE( server.requestCount, 2 );
    }
    void verifiesJwtThroughServerAndInvalidatesAccountCache()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", R"({"id":"verified-one"})" ) << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", R"({"id":"verified-two"})" );
      QgsAiModelRouter router;
      configure( router, server.serverPort() );
      const QString jwt = u"header."_s + QString::fromLatin1( QByteArrayLiteral( "{\"sub\":\"untrusted-claim\"}" ).toBase64( QByteArray::Base64UrlEncoding ) ) + u".signature"_s;
      QVERIFY( router.setPlanSessionToken( jwt ) );
      QCOMPARE( router.verifiedPlanAccountId(), u"verified-one"_s );
      QVERIFY( router.setPlanSessionToken( u"strata_changed_account"_s ) );
      QVERIFY( router.cachedPlanAccountId().isEmpty() );
      QCOMPARE( router.verifiedPlanAccountId(), u"verified-two"_s );
      QCOMPARE( server.requestCount, 2 );
    }
    void projectCrsAssignmentDoesNotRepairRasterFile()
    {
      QTemporaryDir root;
      const QString path = root.filePath( u"unreferenced.png"_s );
      QImage image( 12, 12, QImage::Format_RGBA8888 );
      image.fill( Qt::transparent );
      QVERIFY( image.save( path ) );
      QgsRasterLayer layer( path, u"Unreferenced raster"_s );
      QVERIFY( layer.isValid() );
      layer.setCrs( QgsCoordinateReferenceSystem( u"EPSG:3857"_s ) );
      QVERIFY( layer.crs().isValid() );
      const auto checks = qgsAiVerifyRasterSource( &layer );
      QVERIFY( !checks.value( u"raster_source_crs_valid"_s ).toBool() );
      QVERIFY( !checks.value( u"raster_transform_valid"_s ).toBool() );
    }
    void snapshotCannotEscapeWorkspaceThroughSymlink()
    {
      QTemporaryDir root, outside;
      QVERIFY( QFile::link( outside.path(), root.filePath( u"escape"_s ) ) );
      QgsAiFileContextProvider files( root.path() );
      QgsAiAddLayerFromServiceTool tool( QgsProject::instance(), &files );
      const auto result = tool.execute(
        QJsonObject { { u"provider"_s, u"arcgis_mapserver"_s }, { u"mode"_s, u"snapshot"_s }, { u"destination"_s, u"escape/output.tif"_s }, { u"uri"_s, u"https://example.test/MapServer"_s } }
      );
      QVERIFY( !result.success );
      QVERIFY( result.errorMessage.contains( u"outside the workspace"_s ) );
      QVERIFY( !QFileInfo::exists( outside.filePath( u"output.tif"_s ) ) );
    }
    void correctedVerificationResumesWithoutToolReplay()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", R"({"choices":[{"message":{"role":"assistant","content":"Verified recovery"},"finish_reason":"stop"}]})" );
      QgsAiModelRouter router;
      QVERIFY( router.storeApiKey( QgsAiModelRouter::Provider::OpenRouter, u"synthetic-key"_s ) );
      auto settings = router.providerSettings( QgsAiModelRouter::Provider::OpenRouter );
      settings.endpoint = u"http://127.0.0.1:%1/chat"_s.arg( server.serverPort() );
      settings.model = u"test/model"_s;
      settings.enabled = true;
      router.setProviderSettings( QgsAiModelRouter::Provider::OpenRouter, settings );
      router.setActiveProvider( QgsAiModelRouter::Provider::OpenRouter );
      QTemporaryDir root;
      QgsAiFileContextProvider files( root.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( &router, &files, &review );
      QgsAiToolRegistry tools;
      tools.registerTool( std::make_unique<QgsAiAddLayerFromFileTool>( &files, QgsProject::instance() ) );
      manager.setToolRegistry( &tools );
      auto *layer = new QgsVectorLayer( u"Point"_s, u"Synthetic resource"_s, u"memory"_s );
      QgsFeature feature;
      feature.setGeometry( QgsGeometry::fromWkt( u"POINT (1 1)"_s ) );
      QgsFeatureList features { feature };
      layer->dataProvider()->addFeatures( features );
      layer->updateExtents();
      // A point extent is empty; use a second distinct feature for a non-empty bounding box.
      QgsFeature second;
      second.setGeometry( QgsGeometry::fromWkt( u"POINT (2 2)"_s ) );
      features = { second };
      layer->dataProvider()->addFeatures( features );
      layer->updateExtents();
      layer->setCrs( QgsCoordinateReferenceSystem() );
      QgsProject::instance()->addMapLayer( layer );
      QgsAiChatMessage user;
      user.id = u"user"_s;
      user.content = u"Load synthetic resource"_s;
      manager.appendHistoryMessage( user );
      QgsAiChatMessage call;
      call.id = u"call"_s;
      call.role = QgsAiChatRole::Assistant;
      call.metadata.insert( u"tool_calls"_s, QVariantList { QVariantMap { { u"id"_s, u"original-call"_s }, { u"name"_s, u"add_layer_from_file"_s } } } );
      manager.appendHistoryMessage( call );
      QgsAiChatMessage result;
      result.id = u"result"_s;
      result.role = QgsAiChatRole::Tool;
      result.content = QString::fromUtf8(
        QJsonDocument( QJsonObject { { u"layer_id"_s, layer->id() }, { u"quality_checks"_s, QJsonObject { { u"crs_valid"_s, false }, { u"passed"_s, false } } } } ).toJson()
      );
      result.metadata = { { u"tool_call_id"_s, u"original-call"_s }, { u"tool_name"_s, u"add_layer_from_file"_s }, { u"is_error"_s, true } };
      manager.appendHistoryMessage( result );
      auto checkpoint = manager.recoveryCheckpoint();
      checkpoint.insert( u"provider"_s, static_cast<int>( QgsAiModelRouter::Provider::OpenRouter ) );
      checkpoint.insert( u"model"_s, settings.model );
      checkpoint.insert( u"version"_s, 1 );
      QgsAiChatMessage failed;
      failed.id = u"failure"_s;
      failed.role = QgsAiChatRole::Assistant;
      failed.metadata = { { u"ui_kind"_s, u"request_error"_s }, { u"recovery"_s, checkpoint } };
      manager.appendHistoryMessage( failed );
      QString error;
      QVERIFY( !manager.resumeLastInterruptedTurn( &error ) );
      QVERIFY2( error.contains( u"crs_valid"_s ), qPrintable( error ) );
      QCOMPARE( server.requestCount, 0 );
      layer->setCrs( QgsCoordinateReferenceSystem( u"EPSG:3857"_s ) );
      QVERIFY2( manager.resumeLastInterruptedTurn( &error ), qPrintable( error ) );
      QTRY_VERIFY_WITH_TIMEOUT( !manager.hasActiveRequest(), 5000 );
      QCOMPARE( server.requestCount, 1 );
      int results = 0, evidence = 0;
      for ( const auto &message : manager.history() )
      {
        if ( message.role == QgsAiChatRole::Tool )
          ++results;
        if ( message.metadata.value( u"original_tool_call_id"_s ) == u"original-call"_s )
          ++evidence;
      }
      QCOMPARE( results, 1 );
      QCOMPARE( evidence, 1 );
      QgsProject::instance()->removeMapLayer( layer );
    }
    void writesReturnedExtentAndDimensions()
    {
      // Synthetic response to a 4000x4400 request: both dimensions and bounds changed.
      QImage image( 3723, 4096, QImage::Format_RGBA8888 );
      image.fill( Qt::transparent );
      QByteArray png;
      QBuffer buffer( &png );
      buffer.open( QIODevice::WriteOnly );
      QVERIFY( image.save( &buffer, "PNG" ) );
      const QJsonObject
        response { { u"width"_s, 3723 }, { u"height"_s, 4096 }, { u"extent"_s, QJsonObject { { u"xmin"_s, 1000 }, { u"ymin"_s, 2000 }, { u"xmax"_s, 8446 }, { u"ymax"_s, 10192 }, { u"spatialReference"_s, QJsonObject { { u"wkid"_s, 3857 } } } } } };
      QTemporaryDir dir;
      const QString path = dir.filePath( u"snapshot.tif"_s );
      QString error;
      QVERIFY2( qgsAiWriteArcGisGeoTiff( png, response, path, error ), qPrintable( error ) );
      QgsRasterLayer layer( path, u"snapshot"_s );
      QVERIFY( layer.isValid() );
      QCOMPARE( layer.width(), 3723 );
      QCOMPARE( layer.height(), 4096 );
      QCOMPARE( layer.crs().authid(), u"EPSG:3857"_s );
      QCOMPARE( layer.extent().xMinimum(), 1000.0 );
      QCOMPARE( layer.extent().yMinimum(), 2000.0 );
      QCOMPARE( layer.extent().xMaximum(), 8446.0 );
      QCOMPARE( layer.extent().yMaximum(), 10192.0 );
      QCOMPARE( layer.bandCount(), 4 );
      const auto checks = qgsAiVerifyRasterSource( &layer );
      for ( auto it = checks.begin(); it != checks.end(); ++it )
        QVERIFY2( it.value().toBool(), qPrintable( it.key() ) );
      QVERIFY( !qgsAiWriteArcGisGeoTiff( png, response, path, error ) );
      auto mismatch = response;
      mismatch.insert( u"width"_s, 4000 );
      QVERIFY( !qgsAiWriteArcGisGeoTiff( png, mismatch, dir.filePath( u"invalid.tif"_s ), error ) );
    }
};
QGSTEST_MAIN( TestQgsAiIncidentRecovery )
#include "testqgsaiincidentrecovery.moc"
