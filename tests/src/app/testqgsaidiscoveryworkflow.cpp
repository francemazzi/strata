// SPDX-License-Identifier: GPL-2.0-or-later
#include <memory>
#include <zip.h>

#include "ai/discovery/qgsaidiscoverycontroller.h"
#include "ai/discovery/qgsaidiscoveryimport.h"
#include "ai/discovery/qgsaidiscoverypreview.h"
#include "ai/tools/qgsaidiscoverytool.h"
#include "qgsaifilecontextprovider.h"
#include "qgsaimodelrouter.h"
#include "qgsaitestloopbackserver.h"
#include "qgsaiworkspacetrust.h"
#include "qgsapplication.h"
#include "qgslayertree.h"
#include "qgsproject.h"
#include "qgstest.h"
#include "qgsvectorlayer.h"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

using namespace Qt::StringLiterals;
class TestQgsAiDiscoveryWorkflow : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
      QCoreApplication::setOrganizationName( u"StrataDiscoverySynthetic"_s );
      QCoreApplication::setApplicationName( QUuid::createUuid().toString() );
      QgsApplication::initQgis();
    }
    void cleanupTestCase()
    {
      QSettings().clear();
      QgsApplication::exitQgis();
    }
    void previewSelectionDownloadImport_data();
    void previewSelectionDownloadImport();
    void missingProviderIsAnImportFailure();
    void nativeCrsMismatchProducesPartialImport();
};
void TestQgsAiDiscoveryWorkflow::previewSelectionDownloadImport_data()
{
  QTest::addColumn<bool>( "unverified" );
  QTest::newRow( "identified" ) << false;
  QTest::newRow( "explicit-content-consent" ) << true;
}
void TestQgsAiDiscoveryWorkflow::previewSelectionDownloadImport()
{
  QFETCH( bool, unverified );
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  const QString path = root.filePath( u"source.zip"_s );
  const QByteArray data = R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"name":"Synthetic"},"geometry":{"type":"Point","coordinates":[1.5,1.5]}}]})";
  const QString fileHash = QString::fromLatin1( QCryptographicHash::hash( data, QCryptographicHash::Sha256 ).toHex() );
  const QJsonObject coverage { { u"complete"_s, false }, { u"required"_s, QJsonArray { u"boundary"_s, u"trees"_s } }, { u"missing"_s, QJsonArray { u"trees"_s } } };
  const QJsonObject manifest {
    { u"schemaVersion"_s, u"strata-discovery/1"_s },
    { u"coverage"_s, coverage },
    { u"files"_s, QJsonArray { QJsonObject { { u"path"_s, u"layers/key/layer.geojson"_s }, { u"sizeBytes"_s, data.size() }, { u"sha256"_s, fileHash } } } },
    { u"layers"_s,
      QJsonArray {
        QJsonObject { { u"id"_s, u"key:0"_s }, { u"title"_s, u"Synthetic boundary"_s }, { u"mode"_s, u"file"_s }, { u"path"_s, u"layers/key/layer.geojson"_s }, { u"nativeCrs"_s, u"EPSG:4326"_s }, { u"featureCount"_s, 1 } }
      } }
  };
  const auto metadata = QJsonDocument( manifest ).toJson();
  int error;
  auto zip = zip_open( QFile::encodeName( path ).constData(), ZIP_CREATE, &error );
  QVERIFY( zip );
  zip_file_add( zip, "manifest.json", zip_source_buffer( zip, metadata.constData(), metadata.size(), 0 ), 0 );
  zip_file_add( zip, "README.txt", zip_source_buffer( zip, "test", 4, 0 ), 0 );
  zip_file_add( zip, "layers/key/layer.geojson", zip_source_buffer( zip, data.constData(), data.size(), 0 ), 0 );
  QCOMPARE( zip_close( zip ), 0 );
  QFile archive( path );
  QVERIFY( archive.open( QIODevice::ReadOnly ) );
  const QByteArray kit = archive.readAll();
  archive.close();
  QgsAiTestLoopbackServer server;
  QVERIFY( server.listen( QHostAddress::LocalHost ) );
  QJsonObject
    candidate { { u"id"_s, u"candidate"_s }, { u"key"_s, u"key"_s }, { u"title"_s, u"Synthetic boundary"_s }, { u"modes"_s, QJsonArray { u"file"_s } }, { u"requirements"_s, QJsonArray { u"boundary"_s } } };
  candidate.insert( u"content"_s, QJsonObject { { u"kind"_s, unverified ? u"unknown"_s : u"vector"_s }, { u"status"_s, unverified ? u"unverified"_s : u"identified"_s } } );
  candidate.insert( u"eligibleRequirementsByMode"_s, QJsonObject { { u"file"_s, unverified ? QJsonArray {} : QJsonArray { u"boundary"_s } } } );
  QJsonObject selected { { u"candidateId"_s, u"candidate"_s }, { u"mode"_s, u"file"_s } };
  if ( unverified )
  {
    selected.insert( u"allowUnverifiedContent"_s, true );
    selected.insert( u"maxBytes"_s, 16777216 );
  }
  const QJsonArray selection { selected };
  const QJsonObject
    plan { { u"id"_s, u"plan"_s }, { u"version"_s, 1 }, { u"status"_s, u"SUCCEEDED"_s }, { u"coverage"_s, coverage }, { u"candidates"_s, QJsonArray { candidate } }, { u"expiresAt"_s, u"2099-01-01T00:00:00.000Z"_s } };
  const QJsonObject run {
    { u"id"_s, u"run"_s },
    { u"planId"_s, u"plan"_s },
    { u"planVersion"_s, 1 },
    { u"status"_s, u"SUCCEEDED"_s },
    { u"selection"_s, selection },
    { u"budget"_s, QJsonObject { { u"maxCredits"_s, 20 } } },
    { u"artifact"_s,
      QJsonObject {
        { u"downloadUrl"_s, u"http://127.0.0.1:%1/kit.zip"_s.arg( server.serverPort() ) },
        { u"sizeBytes"_s, QString::number( kit.size() ) },
        { u"sha256"_s, QString::fromLatin1( QCryptographicHash::hash( kit, QCryptographicHash::Sha256 ).toHex() ) }
      } }
  };
  auto response = []( const QJsonObject &value ) { return QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QJsonDocument( value ).toJson() ); };
  server.responses
    << response( { { u"status"_s, u"ready"_s }, { u"brief"_s, QJsonObject { { u"workflow"_s, u"inquadramento"_s } } } } )
    << response( plan )
    << response( { { u"id"_s, u"run"_s }, { u"status"_s, u"QUEUED"_s } } )
    << response( run )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", kit )
    << response( { { u"id"_s, u"review"_s } } );
  QgsAiModelRouter router;
  const auto claims = QJsonDocument( QJsonObject { { u"sub"_s, u"synthetic-discovery-user"_s } } ).toJson( QJsonDocument::Compact ).toBase64( QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals );
  QVERIFY( router.setPlanSessionToken( u"e30.%1.synthetic"_s.arg( QString::fromLatin1( claims ) ) ) );
  auto settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
  settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( server.serverPort() );
  settings.enabled = true;
  router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
  QgsProject project;
  project.setCrs( QgsCoordinateReferenceSystem( u"EPSG:3857"_s ) );
  QgsAiFileContextProvider files( root.path() );
  QgsAiWorkspaceTrust::setState( root.path(), QgsAiWorkspaceTrust::State::Trusted );
  QgsAiDiscoveryController controller( &router, &files, nullptr, &project );
  std::unique_ptr<QgsAiDiscoveryPreview> preview;
  connect( &controller, &QgsAiDiscoveryController::previewReady, this, [&]( QgsAiDiscoveryPreview *card ) { preview.reset( card ); } );
  connect( &controller, &QgsAiDiscoveryController::controlsReady, this, []( QWidget *widget ) { widget->deleteLater(); } );
  QgsAiDiscoveryTool resolve( u"discovery_resolve"_s, &controller );
  const auto started = resolve.execute( { { u"intent"_s, u"Synthetic request"_s } } );
  QVERIFY( started.success );
  const auto id = started.output.toObject().value( u"requestId"_s ).toString();
  QTRY_COMPARE( controller.execute( u"discovery_status"_s, { { u"id"_s, id }, { u"kind"_s, u"resolve"_s } } ).value( u"status"_s ).toString(), u"ready"_s );
  controller.execute( u"discovery_search"_s, { { u"resolutionId"_s, id }, { u"maxCredits"_s, 1 } } );
  QTRY_VERIFY( preview );
  QCOMPARE( server.requestCount, 2 );
  QVERIFY( project.mapLayers().isEmpty() );
  preview->findChild<QCheckBox *>()->setChecked( true );
  if ( unverified )
  {
    QVERIFY( !preview->findChild<QPushButton *>( u"discoveryConfirm"_s )->isEnabled() );
    QCOMPARE( server.requestCount, 2 );
    preview->findChild<QCheckBox *>( u"discoveryContentConsent"_s )->setChecked( true );
  }
  preview->findChild<QPushButton *>( u"discoveryConfirm"_s )->click();
  preview->findChild<QPushButton *>( u"discoveryConfirm"_s )->click();
  QTRY_COMPARE_WITH_TIMEOUT( project.mapLayers().size(), 1, 30000 );
  QTRY_COMPARE_WITH_TIMEOUT( server.requestCount, 6, 10000 );
  QCOMPARE( project.crs().authid(), u"EPSG:3857"_s );
  auto layer = qobject_cast<QgsVectorLayer *>( project.mapLayers().first() );
  QVERIFY( layer );
  QCOMPARE( layer->crs().authid(), u"EPSG:4326"_s );
  QCOMPARE( layer->featureCount(), 1 );
  QVERIFY( project.layerTreeRoot()->findGroup( u"Discovery · run"_s ) );
  const auto submitted = QJsonDocument::fromJson( server.requestBodies[2] ).object();
  QCOMPARE( submitted.value( u"selection"_s ).toArray(), selection );
  QVERIFY( submitted.contains( u"workspaceId"_s ) );
  const auto review = QJsonDocument::fromJson( server.requestBodies[5] ).object();
  QCOMPARE( review.value( u"outcome"_s ).toObject().value( u"status"_s ).toString(), u"SUCCEEDED"_s );
  QVERIFY( !review.value( u"outcome"_s ).toObject().value( u"coverage"_s ).toObject().value( u"complete"_s ).toBool() );
  controller.resume();
  QTest::qWait( 50 );
  QCOMPARE( server.requestCount, 6 );
  server.responses
    << response( { { u"id"_s, u"cancel-plan"_s }, { u"status"_s, u"QUEUED"_s } } )
    << response( { { u"id"_s, u"cancel-plan"_s }, { u"status"_s, u"CANCELLED"_s }, { u"cancelRequested"_s, true } } ); // # spellok: API protocol status.
  const auto pending = controller.execute( u"discovery_search"_s, { { u"resolutionId"_s, id }, { u"maxCredits"_s, 1 } } );
  controller.execute( u"discovery_cancel"_s, { { u"id"_s, pending.value( u"requestId"_s ) }, { u"kind"_s, u"plans"_s } } );
  QTRY_COMPARE( server.requestCount, 8 );
  QCOMPARE( controller.execute( u"discovery_status"_s, { { u"id"_s, u"cancel-plan"_s }, { u"kind"_s, u"plans"_s } } ).value( u"status"_s ).toString(), u"CANCELLED"_s ); // # spellok: API protocol status.
  QCOMPARE( project.mapLayers().size(), 1 );
}
void TestQgsAiDiscoveryWorkflow::missingProviderIsAnImportFailure()
{
  const auto result = QgsAiDiscoveryImport::
    open( { { u"layers"_s, QJsonArray { QJsonObject { { u"id"_s, u"missing"_s }, { u"mode"_s, u"service"_s }, { u"protocol"_s, u"UNKNOWN"_s } } } } }, {}, {}, QThread::currentThread() );
  QVERIFY( result.layers.isEmpty() );
  QCOMPARE( result.outcome.value( u"status"_s ).toString(), u"FAILED"_s );
}
void TestQgsAiDiscoveryWorkflow::nativeCrsMismatchProducesPartialImport()
{
  QTemporaryDir root;
  QFile data( root.filePath( u"point.geojson"_s ) );
  QVERIFY( data.open( QIODevice::WriteOnly ) );
  data.write( R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[1.5,1.5]}}]})" );
  data.close();
  const QJsonObject good { { u"id"_s, u"good"_s }, { u"mode"_s, u"file"_s }, { u"path"_s, u"point.geojson"_s }, { u"nativeCrs"_s, u"EPSG:4326"_s } };
  auto wrong = good;
  wrong.insert( u"id"_s, u"wrong"_s );
  wrong.insert( u"nativeCrs"_s, u"EPSG:3857"_s );
  const auto result = QgsAiDiscoveryImport::open( { { u"layers"_s, QJsonArray { good, wrong } } }, root.path(), {}, QThread::currentThread() );
  QCOMPARE( result.layers.size(), 1 );
  QCOMPARE( result.outcome.value( u"status"_s ).toString(), u"PARTIAL"_s );
  QCOMPARE( result.outcome.value( u"rejectedLayers"_s ).toInt(), 1 );
  QCOMPARE( result.layers[0]->crs().authid(), u"EPSG:4326"_s );
  qDeleteAll( result.layers );
}
QGSTEST_MAIN( TestQgsAiDiscoveryWorkflow )
#include "testqgsaidiscoveryworkflow.moc"
