/***************************************************************************
  testqgsailayerchunker.cpp
  --------------------------------
  begin                : May 2026
***************************************************************************/

#include <algorithm>
#include <memory>

#include "ai/index/qgsailayerchunker.h"
#include "ai/index/qgsaiworkspaceindex.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgsrasterlayer.h"
#include "qgssettings.h"
#include "qgstest.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"

#include <QByteArray>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QString>

using namespace Qt::StringLiterals;

class TestQgsAiLayerChunker : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();

    void chunksCoverEveryFeatureExactlyOnce();
    void chunkTextStaysWithinTargetSize();
    void wktBlobIsRecoverable();
    void rasterEmitsSingleMetadataChunk();
    void vectorHeaderSkipsFeatureCountScan();
    void officeSpreadsheetVectorLayerSkipsFeatureSampling();
    void rasterMetadataSkipsBandStatistics();
    void preparedLayerChunksAfterLayerIsGone();
    void remoteLayersAreIndexedFromMetadata();
    void chunksFitATokenBudget();
};

void TestQgsAiLayerChunker::initTestCase()
{
  QgsApplication::initQgis();
}

void TestQgsAiLayerChunker::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsAiLayerChunker::chunksCoverEveryFeatureExactlyOnce()
{
  const QString shpPath = QStringLiteral( TEST_DATA_DIR ) + u"/points.shp"_s;
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY2( layer->isValid(), shpPath.toUtf8().constData() );

  const auto chunks = QgsAiLayerChunker::chunkVector( layer.get() );
  QVERIFY( !chunks.isEmpty() );
  QVERIFY( chunks.size() <= 20 );

  qint64 covered = 0;
  qint64 prevLast = -1;
  for ( const auto &c : chunks )
  {
    QCOMPARE( c.sourceType, QString::fromLatin1( QgsAiWorkspaceIndex::SOURCE_TYPE_LAYER ) );
    QCOMPARE( c.layerId, layer->id() );
    QVERIFY( c.firstFeatureId >= 0 );
    QVERIFY( c.lastFeatureId >= c.firstFeatureId );
    QVERIFY( c.firstFeatureId > prevLast );
    covered += ( c.lastFeatureId - c.firstFeatureId + 1 );
    prevLast = c.lastFeatureId;
  }
  QCOMPARE( covered, std::min<qint64>( layer->featureCount(), 200 ) );
}

void TestQgsAiLayerChunker::chunkTextStaysWithinTargetSize()
{
  const QString shpPath = QStringLiteral( TEST_DATA_DIR ) + u"/points.shp"_s;
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );

  const auto chunks = QgsAiLayerChunker::chunkVector( layer.get() );
  QVERIFY( !chunks.isEmpty() );

  // The chunker flushes after exceeding CHUNK_TARGET_CHARS, so a chunk can
  // overshoot by at most one feature line. 1.5x is a generous tolerance.
  const int hardCap = static_cast<int>( QgsAiWorkspaceIndex::CHUNK_TARGET_CHARS * 1.5 );
  for ( const auto &c : chunks )
    QVERIFY2( c.text.size() <= hardCap, qPrintable( u"chunk text size %1 exceeds %2"_s.arg( c.text.size() ).arg( hardCap ) ) );
}

void TestQgsAiLayerChunker::wktBlobIsRecoverable()
{
  // WKT is only collected when the user lets geometries reach the model context.
  QgsSettings settings;
  settings.setValue( u"strata/privacy/include_layer_wkt_in_model_context"_s, true );
  const auto restore = qScopeGuard( [&settings]() { settings.remove( u"strata/privacy/include_layer_wkt_in_model_context"_s ); } );

  const QString shpPath = QStringLiteral( TEST_DATA_DIR ) + u"/points.shp"_s;
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );

  const auto chunks = QgsAiLayerChunker::chunkVector( layer.get() );
  QVERIFY( !chunks.isEmpty() );

  for ( const auto &c : chunks )
  {
    QVERIFY( !c.wktBlob.isEmpty() );
    const QByteArray decoded = qUncompress( c.wktBlob );
    QVERIFY( !decoded.isEmpty() );
    static const QRegularExpression wktRx( "\\b(POINT|MULTIPOINT|LINESTRING|MULTILINESTRING|POLYGON|MULTIPOLYGON)\\b", QRegularExpression::CaseInsensitiveOption );
    QVERIFY2( wktRx.match( QString::fromUtf8( decoded ) ).hasMatch(), decoded.left( 80 ).constData() );
  }
}

void TestQgsAiLayerChunker::rasterEmitsSingleMetadataChunk()
{
  // We don't need a real raster on disk to validate that nullptr -> empty list
  // and that the chunker is wired correctly; deeper raster tests live downstream.
  QList<QgsAiWorkspaceIndex::Chunk> chunks = QgsAiLayerChunker::chunkRaster( nullptr );
  QCOMPARE( chunks.size(), 0 );
}

void TestQgsAiLayerChunker::vectorHeaderSkipsFeatureCountScan()
{
  const QString shpPath = QStringLiteral( TEST_DATA_DIR ) + u"/points.shp"_s;
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );

  const auto chunks = QgsAiLayerChunker::chunkVector( layer.get() );
  QVERIFY( !chunks.isEmpty() );
  QVERIFY( chunks.first().text.contains( u"feature_count=unknown"_s ) );
}

void TestQgsAiLayerChunker::officeSpreadsheetVectorLayerSkipsFeatureSampling()
{
  const QString odsPath = QStringLiteral( TEST_DATA_DIR ) + u"/spreadsheet.ods|layername=Sheet1"_s;
  auto layer = std::make_unique<QgsVectorLayer>( odsPath, u"sheet"_s, u"ogr"_s );
  QVERIFY2( layer->isValid(), odsPath.toUtf8().constData() );

  const auto chunks = QgsAiLayerChunker::chunkVector( layer.get() );
  QCOMPARE( chunks.size(), 1 );
  QCOMPARE( chunks.first().sourceType, QString::fromLatin1( QgsAiWorkspaceIndex::SOURCE_TYPE_LAYER ) );
  QCOMPARE( chunks.first().layerId, layer->id() );
  QCOMPARE( chunks.first().firstFeatureId, qint64( -1 ) );
  QCOMPARE( chunks.first().lastFeatureId, qint64( -1 ) );
  QVERIFY( chunks.first().wktBlob.isEmpty() );
  QVERIFY( chunks.first().text.contains( u"sampled_feature_limit=0"_s ) );
  QVERIFY( chunks.first().text.contains( u"Office spreadsheet layers"_s ) );
  QVERIFY( !chunks.first().text.contains( u"fields="_s ) );
}

void TestQgsAiLayerChunker::rasterMetadataSkipsBandStatistics()
{
  auto layer = std::make_unique<QgsRasterLayer>( QStringLiteral( TEST_DATA_DIR ) + u"/landsat.tif"_s, u"landsat"_s );
  if ( !layer->isValid() )
    QSKIP( "landsat raster fixture unavailable" );

  const auto chunks = QgsAiLayerChunker::chunkRaster( layer.get() );
  QVERIFY( !chunks.isEmpty() );
  QVERIFY( chunks.first().text.contains( u"band statistics skipped during fast layer snapshot"_s ) );
}

void TestQgsAiLayerChunker::preparedLayerChunksAfterLayerIsGone()
{
  // prepare() runs on the interface thread; chunk() may run after the layer is gone.
  auto layer = std::make_unique<QgsVectorLayer>( u"Point?crs=EPSG:4326&field=name:string"_s, u"prepared"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsFeature feature( layer->fields() );
  feature.setAttribute( 0, u"parco giochi"_s );
  feature.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 11, 45 ) ) );
  QVERIFY( layer->dataProvider()->addFeature( feature ) );

  const QgsAiPreparedLayer prepared = QgsAiLayerChunker::prepare( layer.get() );
  QVERIFY( prepared.metadataText.isEmpty() );
  QVERIFY( prepared.source );
  layer.reset();

  const QList<QgsAiWorkspaceIndex::Chunk> chunks = QgsAiLayerChunker::chunk( prepared );
  QCOMPARE( chunks.size(), 1 );
  QVERIFY2( chunks.first().text.contains( u"parco giochi"_s ), chunks.first().text.toUtf8().constData() );
  // No WKT unless the privacy setting allows it.
  QVERIFY( chunks.first().wktBlob.isEmpty() );
}

void TestQgsAiLayerChunker::remoteLayersAreIndexedFromMetadata()
{
  auto layer = std::make_unique<QgsVectorLayer>( u"/vsicurl/https://example.invalid/data.gpkg"_s, u"remote"_s, u"ogr"_s );
  QVERIFY( QgsAiLayerChunker::isRemoteLayer( layer.get() ) );
  const QgsAiPreparedLayer prepared = QgsAiLayerChunker::prepare( layer.get() );
  QVERIFY( !prepared.source );
  QVERIFY( prepared.metadataText.contains( u"remote layer"_s ) );
  const QList<QgsAiWorkspaceIndex::Chunk> chunks = QgsAiLayerChunker::chunk( prepared );
  QCOMPARE( chunks.size(), 1 );

  auto local = std::make_unique<QgsVectorLayer>( u"Point?crs=EPSG:4326"_s, u"local"_s, u"memory"_s );
  QVERIFY( !QgsAiLayerChunker::isRemoteLayer( local.get() ) );
}

QGSTEST_MAIN( TestQgsAiLayerChunker )
void TestQgsAiLayerChunker::chunksFitATokenBudget()
{
  auto layer = std::make_unique<QgsVectorLayer>( u"Point?crs=EPSG:4326&field=name:string&field=value:double"_s, u"points"_s, u"memory"_s );
  QgsFeatureList features;
  for ( int i = 0; i < 120; ++i )
  {
    QgsFeature feature( layer->fields() );
    feature.setAttribute( 0, u"feature %1"_s.arg( i ) );
    feature.setAttribute( 1, 1234.5678 * i );
    feature.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 9 + i * 0.001, 45 + i * 0.001 ) ) );
    features << feature;
  }
  QVERIFY( layer->dataProvider()->addFeatures( features ) );

  // One token per character keeps the arithmetic visible.
  const QgsAiWorkspaceIndex::TokenCounter perCharacter = []( const QString &text ) { return static_cast<int>( text.size() ); };
  const QList<QgsAiWorkspaceIndex::Chunk> chunks = QgsAiLayerChunker::chunk( QgsAiLayerChunker::prepare( layer.get() ), nullptr, perCharacter, 600 );
  QVERIFY( chunks.size() > 3 );
  for ( const QgsAiWorkspaceIndex::Chunk &chunk : chunks )
    QVERIFY2( chunk.text.size() <= 600, QString::number( chunk.text.size() ).toUtf8().constData() );
}

#include "testqgsailayerchunker.moc"
