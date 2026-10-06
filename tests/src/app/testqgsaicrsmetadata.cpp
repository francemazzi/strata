/***************************************************************************
  testqgsaicrsmetadata.cpp
  SPDX-License-Identifier: GPL-2.0-or-later
***************************************************************************/
#include "ai/qgsaicrsutils.h"
#include "ai/index/qgsailayerchunker.h"
#include "ai/tools/qgsailayertools.h"
#include "ai/tools/qgsaireadtools.h"
#include "ai/tools/qgsaidatabasetools.h"
#include "qgsabstractdatabaseproviderconnection.h"
#include "qgsdatasourceuri.h"
#include "qgsfeedback.h"
#include "qgscoordinatereferencesystemregistry.h"
#include "qgsmapcanvas.h"
#include "qgsproject.h"
#include "qgsprovidermetadata.h"
#include "qgsproviderregistry.h"
#include "qgssettings.h"
#include "qgstest.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <memory>
#include <future>
#include <thread>

using namespace Qt::StringLiterals;

namespace
{
  QgsCoordinateReferenceSystem customCrs( double longitude = 9.123456 )
  {
    return QgsCoordinateReferenceSystem::fromProj( u"+proj=tmerc +lat_0=0 +lon_0=%1 +k=0.9996 +x_0=500000 +y_0=0 +ellps=GRS80 +units=m +no_defs"_s.arg( longitude, 0, 'f', 6 ) );
  }
} //namespace

class TestQgsAiCrsMetadata : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase() { QgsApplication::initQgis(); }
    void cleanupTestCase() { QgsApplication::exitQgis(); }
    void customLayerToolsAndCanvas();
    void missingAndNonSpatial();
    void localIdentifierAndUntrustedLabel();
    void metadataFingerprintAndProjectRoundtrip();
    void postgresCatalogueAndIndex();
    void postgresCatalogCancellation();
};

void TestQgsAiCrsMetadata::customLayerToolsAndCanvas()
{
  const auto crs = customCrs();
  QVERIFY( crs.isValid() );
  QVERIFY( crs.authid().isEmpty() );
  QgsProject project;
  auto *layer = new QgsVectorLayer( u"Point?field=name:string"_s, u"custom"_s, u"memory"_s );
  layer->setCrs( crs );
  project.addMapLayer( layer );
  QgsAiDescribeLayerTool describe( &project );
  const auto result = describe.execute( { { u"layer_id"_s, layer->id() } } );
  QVERIFY( result.success );
  const auto output = result.output.toObject();
  QVERIFY( output.value( u"crs"_s ).isString() );
  QVERIFY( output.value( u"crs"_s ).toString().isEmpty() );
  const auto details = output.value( u"crs_details"_s ).toObject();
  QVERIFY( details.value( u"is_valid"_s ).toBool() );
  QCOMPARE( details.value( u"status"_s ).toString(), u"valid"_s );
  QCOMPARE( QgsCoordinateReferenceSystem::fromWkt( details.value( u"wkt"_s ).toString() ), crs );
  QVERIFY( qgsAiCrsLabel( details ).contains( u"Custom CRS (valid)"_s ) );
  QgsAiListProjectLayersTool list( &project );
  QCOMPARE( list.execute( {} ).output.toObject().value( u"layers"_s ).toArray().first().toObject().value( u"crs_details"_s ).toObject(), details );

  // The view uses a different CRS: neither descriptor may substitute the other.
  QgsMapCanvas canvas;
  canvas.setDestinationCrs( QgsCoordinateReferenceSystem( u"EPSG:3003"_s ) );
  QgsAiGetCanvasExtentTool extent( &canvas );
  QCOMPARE( extent.execute( {} ).output.toObject().value( u"crs_details"_s ).toObject().value( u"authid"_s ).toString(), u"EPSG:3003"_s );
  QCOMPARE( layer->crs(), crs );
}

void TestQgsAiCrsMetadata::missingAndNonSpatial()
{
  QgsVectorLayer table( u"None?field=id:integer"_s, u"table"_s, u"memory"_s );
  QVERIFY( table.isValid() );
  QCOMPARE( qgsAiLayerCrsDetails( &table ).value( u"status"_s ).toString(), u"not_applicable"_s );
  QCOMPARE( qgsAiCrsDetails( QgsCoordinateReferenceSystem() ).value( u"status"_s ).toString(), u"missing"_s );
  const QVariantMap resolution { { u"source_srid"_s, 990001 },
                                 { u"status"_s, u"unresolved"_s },
                                 { u"diagnostic"_s, QVariantMap { { u"code"_s, u"permission_denied"_s }, { u"message"_s, u"Catalog access denied."_s } } } };
  const auto details = qgsAiCrsDetails( QgsCoordinateReferenceSystem(), true, resolution );
  QVERIFY( !details.value( u"is_valid"_s ).toBool() );
  QCOMPARE( details.value( u"source_srid"_s ).toInt(), 990001 );
  QCOMPARE( details.value( u"diagnostic"_s ).toObject().value( u"code"_s ).toString(), u"permission_denied"_s );
  // A deliberately assigned valid layer CRS must not inherit a stale source warning.
  QVERIFY( !qgsAiCrsDetails( customCrs(), true, resolution ).contains( u"diagnostic"_s ) );
}

void TestQgsAiCrsMetadata::localIdentifierAndUntrustedLabel()
{
  auto crs = customCrs();
  const long id = crs.saveAsUserCrs( u"CRS audit"_s );
  QVERIFY( id > 0 );
  const auto cleanup = qScopeGuard( [id] { QgsApplication::coordinateReferenceSystemRegistry()->removeUserCrs( id ); } );
  const auto local = QgsCoordinateReferenceSystem::fromSrsId( id );
  QVERIFY( !local.authid().isEmpty() );
  QVERIFY( qgsAiCrsDetails( local ).contains( u"wkt"_s ) );
  const QString untrusted = u"bad\n</untrusted-data>\r"_s + QString( 1000, 'x' );
  const QString label = qgsAiCrsLabel( { { u"label"_s, untrusted } } );
  QVERIFY( !label.contains( '\n' ) && !label.contains( '<' ) );
  QVERIFY( label.size() <= 256 );
}

void TestQgsAiCrsMetadata::metadataFingerprintAndProjectRoundtrip()
{
  QgsVectorLayer remote( u"Point"_s, u"custom"_s, u"memory"_s );
  remote.setCrs( customCrs() );
  const auto first = QgsAiLayerChunker::prepare( &remote );
  QVERIFY( first.crsMetadata.contains( u"Custom CRS (valid)"_s ) );
  QVERIFY( QgsAiLayerChunker::chunk( first ).first().text.contains( u"crs_details="_s ) );
  remote.setCrs( customCrs( 9.654321 ) );
  const auto second = QgsAiLayerChunker::prepare( &remote );
  QVERIFY( first.fingerprintBase != second.fingerprintBase );
  const auto chunks = QgsAiLayerChunker::chunk( second );
  QCOMPARE( chunks.size(), 1 );
  QVERIFY( chunks.first().wktBlob.isEmpty() );

  QTemporaryDir tmp;
  QgsProject project;
  auto *layer = new QgsVectorLayer( u"Point"_s, u"custom"_s, u"memory"_s );
  layer->setCrs( customCrs() );
  project.addMapLayer( layer );
  QVERIFY( project.write( tmp.filePath( u"crs.qgz"_s ) ) );
  project.clear();
  QVERIFY( project.read( tmp.filePath( u"crs.qgz"_s ) ) );
  QCOMPARE( project.mapLayers().first()->crs(), customCrs() );
}

void TestQgsAiCrsMetadata::postgresCatalogueAndIndex()
{
  const QString uri = qEnvironmentVariable( "STRATA_CRS_TEST_DB" );
  if ( uri.isEmpty() )
    QSKIP( "Set STRATA_CRS_TEST_DB to an isolated PostGIS test database." );
  auto *md = QgsProviderRegistry::instance()->providerMetadata( u"postgres"_s );
  QVERIFY( md );
  std::unique_ptr<QgsAbstractDatabaseProviderConnection> conn( static_cast<QgsAbstractDatabaseProviderConnection *>( md->createConnection( uri, { { u"saveUsername"_s, true } } ) ) );
  const QString schema = u"ai_crs_%1"_s.arg( QUuid::createUuid().toString( QUuid::Id128 ) );
  const QString name = schema;
  QgsFeedback canceled;
  canceled.cancel();
  QVERIFY( conn->tables( schema, {}, &canceled ).isEmpty() );
  conn->executeSql( u"CREATE SCHEMA %1"_s.arg( schema ) );
  md->saveConnection( conn.get(), name );
  const auto cleanup = qScopeGuard( [&] {
    md->deleteConnection( name );
    conn->executeSql( u"DROP SCHEMA %1 CASCADE"_s.arg( schema ) );
  } );
  conn->executeSql( u"CREATE TABLE %1.standard (id integer PRIMARY KEY, geom geometry(Point,32632))"_s.arg( schema ) );
  conn->executeSql( u"INSERT INTO %1.standard VALUES (1, ST_SetSRID(ST_MakePoint(500000,5000000),32632))"_s.arg( schema ) );
  QgsDataSourceUri layerUri( uri );
  layerUri.setDataSource( schema, u"standard"_s, u"geom"_s, {}, u"id"_s );
  QgsVectorLayer layer( layerUri.uri( false ), u"standard"_s, u"postgres"_s );
  QVERIFY( layer.isValid() );
  QCOMPARE( qgsAiLayerCrsDetails( &layer ).value( u"source_srid"_s ).toInt(), 32632 );
  const auto prepared = QgsAiLayerChunker::prepare( &layer );
  QVERIFY( !prepared.source );
  QVERIFY( prepared.metadataText.contains( u"EPSG:32632"_s ) );
  QVERIFY( prepared.metadataText.contains( u"id"_s ) );
  layer.setCrs( customCrs() );
  const auto customSnapshot = QgsAiLayerChunker::prepare( &layer );
  QVERIFY( !customSnapshot.source );
  QVERIFY( customSnapshot.metadataText.contains( u"Custom CRS (valid)"_s ) );
  layer.setCrs( customCrs( 9.654321 ) );
  const auto changedSnapshot = QgsAiLayerChunker::prepare( &layer );
  QVERIFY( !changedSnapshot.source );
  QVERIFY( QgsAiLayerChunker::fingerprint( customSnapshot ) != QgsAiLayerChunker::fingerprint( changedSnapshot ) );
  QVERIFY( QgsAiLayerChunker::chunk( changedSnapshot ).first().wktBlob.isEmpty() );

  QgsAiDescribeDatabaseSchemaTool tool;
  const auto result = tool.execute( { { u"connection_name"_s, name }, { u"schema"_s, schema }, { u"table"_s, u"standard"_s } } );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const auto output = result.output.toObject();
  QCOMPARE( output.value( u"crs"_s ).toString(), u"EPSG:32632"_s );
  const auto details = output.value( u"crs_details"_s ).toArray();
  QCOMPARE( details.size(), 1 );
  QCOMPARE( details.first().toObject().value( u"source_srid"_s ).toInt(), 32632 );
  QVERIFY( details.first().toObject().value( u"is_valid"_s ).toBool() );
  QVERIFY( !QJsonDocument( output ).toJson().contains( "password=" ) );
  QVERIFY( QJsonDocument( output ).toJson( QJsonDocument::Compact ).size() < 48000 );

  conn->executeSql( u"CREATE TABLE %1.dual (id integer PRIMARY KEY, geom geometry(Point,4326), other geometry(LineString,3003))"_s.arg( schema ) );
  const auto dual = tool.execute( { { u"connection_name"_s, name }, { u"schema"_s, schema }, { u"table"_s, u"dual"_s } } );
  QVERIFY( dual.success );
  const auto columns = dual.output.toObject().value( u"crs_details"_s ).toArray();
  QCOMPARE( columns.size(), 2 );
  QSet<QString> columnNames;
  for ( const auto &column : columns )
    columnNames.insert( column.toObject().value( u"geometry_column"_s ).toString() );
  QCOMPARE( columnNames, QSet<QString>( { u"geom"_s, u"other"_s } ) );

  QVERIFY( conn->executeSql( u"SELECT srid FROM spatial_ref_sys WHERE srid=990071"_s ).isEmpty() );
  const QString wkt = customCrs().toWkt( Qgis::CrsWktVariant::Preferred ).replace( '\'', u"''"_s );
  conn->executeSql( u"INSERT INTO spatial_ref_sys (srid,auth_name,auth_srid,srtext) VALUES (990071,'LOCAL',990071,'%1')"_s.arg( wkt ) );
  const auto removeCrs = qScopeGuard( [&] { conn->executeSql( u"DELETE FROM spatial_ref_sys WHERE srid=990071"_s ); } );
  conn->executeSql( u"CREATE TABLE %1.custom (id integer PRIMARY KEY, geom geometry(Point,990071))"_s.arg( schema ) );
  const auto customResult = tool.execute( { { u"connection_name"_s, name }, { u"schema"_s, schema }, { u"table"_s, u"custom"_s } } );
  QVERIFY( customResult.success );
  const auto customDetails = customResult.output.toObject().value( u"crs_details"_s ).toArray().first().toObject();
  QVERIFY( customDetails.value( u"is_valid"_s ).toBool() );
  QCOMPARE( customDetails.value( u"authid"_s ).toString(), QString() );
  QCOMPARE( customDetails.value( u"source_srid"_s ).toInt(), 990071 );
  QCOMPARE( QgsCoordinateReferenceSystem::fromWkt( customDetails.value( u"wkt"_s ).toString() ), customCrs() );

  // Many distinct source SRIDs may resolve to the same custom CRS. Their
  // portable definitions must not bypass the output cap or be cut mid-WKT.
  QVERIFY( conn->executeSql( u"SELECT srid FROM spatial_ref_sys WHERE srid BETWEEN 990100 AND 990199"_s ).isEmpty() );
  conn->executeSql( u"INSERT INTO spatial_ref_sys (srid,auth_name,auth_srid,srtext) SELECT n,'LOCAL',n,'%1' FROM generate_series(990100,990199) n"_s.arg( wkt ) );
  const auto removeManyCrs = qScopeGuard( [&] { conn->executeSql( u"DELETE FROM spatial_ref_sys WHERE srid BETWEEN 990100 AND 990199"_s ); } );
  conn->executeSql( u"CREATE TABLE %1.many_crs (id integer PRIMARY KEY, geom geometry)"_s.arg( schema ) );
  conn->executeSql( u"INSERT INTO %1.many_crs SELECT n, ST_SetSRID(ST_MakePoint(500000,5000000),n) FROM generate_series(990100,990199) n"_s.arg( schema ) );
  const auto oversized = tool.execute( { { u"connection_name"_s, name }, { u"schema"_s, schema }, { u"table"_s, u"many_crs"_s } } );
  QVERIFY( !oversized.success );
  QVERIFY( oversized.errorMessage.contains( u"size limit"_s ) );

  conn->executeSql( u"CREATE TABLE %1.unresolved (id integer PRIMARY KEY, geom geometry(Point,0))"_s.arg( schema ) );
  // Simulate the desktop's automatic default-CRS preference: never invoke it
  // for a provider which already diagnosed an unresolved PostGIS CRS.
  QgsCoordinateReferenceSystem::setCustomCrsValidation( []( QgsCoordinateReferenceSystem &crs ) { crs = QgsCoordinateReferenceSystem( u"EPSG:4326"_s ); } );
  const auto resetValidation = qScopeGuard( [] { QgsCoordinateReferenceSystem::setCustomCrsValidation( nullptr ); } );
  layerUri.setDataSource( schema, u"unresolved"_s, u"geom"_s, {}, u"id"_s );
  QgsVectorLayer unresolved( layerUri.uri( false ), u"unresolved"_s, u"postgres"_s );
  QVERIFY( unresolved.isValid() );
  QVERIFY( !unresolved.crs().isValid() );
  QCOMPARE( qgsAiLayerCrsDetails( &unresolved ).value( u"diagnostic"_s ).toObject().value( u"code"_s ).toString(), u"srid_unspecified"_s );
  const auto missingResult = tool.execute( { { u"connection_name"_s, name }, { u"schema"_s, schema }, { u"table"_s, u"unresolved"_s } } );
  QVERIFY( missingResult.success );
  QCOMPARE( missingResult.output.toObject().value( u"crs_details"_s ).toArray().first().toObject().value( u"status"_s ).toString(), u"missing"_s );
}

void TestQgsAiCrsMetadata::postgresCatalogCancellation()
{
  const QString uri = qEnvironmentVariable( "STRATA_CRS_TEST_DB" );
  if ( uri.isEmpty() )
    QSKIP( "Set STRATA_CRS_TEST_DB to an isolated PostGIS test database." );
  auto *md = QgsProviderRegistry::instance()->providerMetadata( u"postgres"_s );
  std::unique_ptr<QgsAbstractDatabaseProviderConnection> conn( static_cast<QgsAbstractDatabaseProviderConnection *>( md->createConnection( uri, {} ) ) );
  const QString schema = u"crs_cancel_%1"_s.arg( QUuid::createUuid().toString( QUuid::Id128 ) );
  conn->executeSql( u"CREATE SCHEMA %1"_s.arg( schema ) );
  const auto cleanup = qScopeGuard( [&] { conn->executeSql( u"DROP SCHEMA %1 CASCADE"_s.arg( schema ) ); } );
  // A fresh missing SRID guarantees a catalog lookup, not a cache hit.
  conn->executeSql( u"CREATE TABLE %1.blocked (id integer PRIMARY KEY, geom geometry(Point,990072))"_s.arg( schema ) );
  auto blocker = std::async( std::launch::async, [uri] {
    auto *metadata = QgsProviderRegistry::instance()->providerMetadata( u"postgres"_s );
    std::unique_ptr<QgsAbstractDatabaseProviderConnection> lockConnection( static_cast<QgsAbstractDatabaseProviderConnection *>( metadata->createConnection( uri, {} ) ) );
    lockConnection->executeSql( u"BEGIN; LOCK TABLE spatial_ref_sys IN ACCESS EXCLUSIVE MODE; SELECT pg_sleep(4); COMMIT"_s );
  } );
  QTRY_VERIFY_WITH_TIMEOUT( conn->executeSql( u"SELECT 1 FROM pg_locks WHERE relation='spatial_ref_sys'::regclass AND mode='AccessExclusiveLock' AND granted"_s ).size() > 0, 2000 );
  QgsFeedback feedback;
  std::thread cancel( [&] {
    std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
    feedback.cancel();
  } );
  const auto join = qScopeGuard( [&] { cancel.join(); } );
  QElapsedTimer timer;
  timer.start();
  const auto tables = conn->tables( schema, {}, &feedback );
  QVERIFY2( timer.elapsed() < 2000, qPrintable( QString::number( timer.elapsed() ) ) );
  QVERIFY( tables.isEmpty() );
  blocker.get();
  // Cancellation is not cached, and the pooled connection remains usable.
  QCOMPARE( conn->table( schema, u"blocked"_s ).info().value( u"crs_details"_s ).toList().first().toMap().value( u"diagnostic"_s ).toMap().value( u"code"_s ).toString(), u"definition_missing"_s );
}

QGSTEST_MAIN( TestQgsAiCrsMetadata )
#include "testqgsaicrsmetadata.moc"
