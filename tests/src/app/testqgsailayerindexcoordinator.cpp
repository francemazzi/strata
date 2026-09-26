/***************************************************************************
  testqgsailayerindexcoordinator.cpp
  --------------------------------
  begin                : May 2026
***************************************************************************/

#include <atomic>
#include <memory>

#include "ai/index/qgsaiembeddingprovider.h"
#include "ai/index/qgsailayerchunker.h"
#include "ai/index/qgsailayerindexcoordinator.h"
#include "ai/index/qgsaiworkspaceindex.h"
#include "ai/qgsaifilecontextprovider.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgsproject.h"
#include "qgstest.h"
#include "qgsvectorlayer.h"

#include <QFile>
#include <QMetaObject>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>

using namespace Qt::StringLiterals;

class TestQgsAiLayerIndexCoordinator : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void enablingSchedulesExistingLayers();
    void layerChangeSignalsAreDebounced();
    void layerWillBeRemovedDropsChunksImmediately();
    void disabledCoordinatorIgnoresProjectSignals();
    void unavailableEmbeddingProviderPreventsScheduling();
    void providerUnavailableMidBatchStopsBatch();
    void multipleLayersAreIndexedOneAtATime();
    void bulkReindexUsesLongerDebounceWindow();
    void bulkOperationPausesFlushWithoutClearingDirtySet();
    void nestedBulkOperationsResumeOnlyAtDepthZero();
    void closingTheProjectKeepsLayerChunks();
    void editsAreIndexedOnceSaved();
    void layerRemovedDuringItsTaskLeavesNoChunks();

  private:
    /**
     * Copies the test points.shp (+ sidecars) into \a destDir so editingStopped
     * exercises a writable copy and never mutates the in-tree fixture.
     */
    static QString copyPointsShapefile( const QString &destDir );
};

class CountingWorkspaceIndex : public QgsAiWorkspaceIndex // clazy:exclude=missing-qobject-macro
{
  public:
    explicit CountingWorkspaceIndex( QgsAiFileContextProvider *contextProvider )
      : QgsAiWorkspaceIndex( contextProvider, nullptr )
    {}

    bool embeddingProviderAvailable() const override { return true; }

    bool reindexLayer( const QString &layerId, QString *errorMessage = nullptr ) override
    {
      Q_UNUSED( layerId )
      Q_UNUSED( errorMessage )
      ++directReindexLayerCalls;
      return false;
    }

    bool reindexLayerSnapshot( const QgsAiWorkspaceIndex::WorkspaceLayerSnapshot &snapshot, QString *errorMessage = nullptr, QgsFeedback *feedback = nullptr ) override
    {
      Q_UNUSED( errorMessage )
      Q_UNUSED( feedback )
      reindexedLayerIds.append( snapshot.scopedLayerId );
      return true;
    }

    int directReindexLayerCalls = 0;
    QStringList reindexedLayerIds;
};

/**
 * Index whose first reindexLayerSnapshot() fails and trips the embedding provider to
 * unavailable, simulating a remote 401/403 circuit breaker firing mid-batch.
 */
class BreakerTrippingIndex : public QgsAiWorkspaceIndex // clazy:exclude=missing-qobject-macro
{
  public:
    explicit BreakerTrippingIndex( QgsAiFileContextProvider *contextProvider )
      : QgsAiWorkspaceIndex( contextProvider, nullptr )
    {}

    bool embeddingProviderAvailable() const override { return mAvailable; }

    bool reindexLayer( const QString &layerId, QString *errorMessage = nullptr ) override
    {
      Q_UNUSED( layerId )
      Q_UNUSED( errorMessage )
      ++directReindexLayerCalls;
      return false;
    }

    bool reindexLayerSnapshot( const QgsAiWorkspaceIndex::WorkspaceLayerSnapshot &snapshot, QString *errorMessage = nullptr, QgsFeedback *feedback = nullptr ) override
    {
      Q_UNUSED( feedback )
      reindexedLayerIds.append( snapshot.scopedLayerId );
      mAvailable = false; // the breaker trips on the first failed embed
      if ( errorMessage )
        *errorMessage = u"authentication failed"_s;
      return false;
    }

    int directReindexLayerCalls = 0;
    bool mAvailable = true;
    QStringList reindexedLayerIds;
};

QString TestQgsAiLayerIndexCoordinator::copyPointsShapefile( const QString &destDir )
{
  const QString src = QStringLiteral( TEST_DATA_DIR ) + u"/points"_s;
  const QString dst = destDir + u"/points"_s;
  for ( const QString &ext : { u".shp"_s, u".shx"_s, u".dbf"_s, u".prj"_s } )
  {
    if ( QFile::exists( src + ext ) )
      QFile::copy( src + ext, dst + ext );
  }
  return dst + u".shp"_s;
}

void TestQgsAiLayerIndexCoordinator::initTestCase()
{
  QgsApplication::initQgis();
}

void TestQgsAiLayerIndexCoordinator::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsAiLayerIndexCoordinator::cleanup()
{
  QgsProject::instance()->clear();
}

void TestQgsAiLayerIndexCoordinator::enablingSchedulesExistingLayers()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  CountingWorkspaceIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );

  // Add a layer to the project BEFORE enabling the coordinator: the coordinator
  // must wire signals on existing layers too via connectProjectSignals().
  const QString shpPath = copyPointsShapefile( tempDir.path() );
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer.get(), false );

  QSignalSpy startSpy( &coord, &QgsAiLayerIndexCoordinator::reindexStarted );
  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );

  coord.setEnabled( true );

  QVERIFY( doneSpy.wait( 5000 ) );
  QCOMPARE( startSpy.count(), 1 );
  QCOMPARE( startSpy.first().at( 0 ).toString(), layer->id() );
  QCOMPARE( doneSpy.count(), 1 );
  QCOMPARE( doneSpy.first().at( 0 ).toString(), layer->id() );
  QCOMPARE( doneSpy.first().at( 1 ).toBool(), true );
  QCOMPARE( index.reindexedLayerIds, QStringList { layer->id() } );
  QCOMPARE( index.directReindexLayerCalls, 0 );

  QgsProject::instance()->removeMapLayer( layer.release() );
}

void TestQgsAiLayerIndexCoordinator::layerChangeSignalsAreDebounced()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  CountingWorkspaceIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 200 );
  coord.setBulkDebounceMs( 200 );

  const QString shpPath = copyPointsShapefile( tempDir.path() );
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer.get(), false );

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );
  coord.setEnabled( true );
  QVERIFY( doneSpy.wait( 5000 ) );
  doneSpy.clear();
  index.reindexedLayerIds.clear();

  QVERIFY( QMetaObject::invokeMethod( layer.get(), "layerModified", Qt::DirectConnection ) );
  QVERIFY( QMetaObject::invokeMethod( layer.get(), "dataChanged", Qt::DirectConnection ) );

  QVERIFY( layer->startEditing() );
  QgsFeature feature( layer->fields() );
  feature.setGeometry( QgsGeometry::fromWkt( u"Point(0 0)"_s ) );
  QVERIFY( layer->addFeature( feature ) );
  QVERIFY( layer->commitChanges() );

  QVERIFY( doneSpy.wait( 5000 ) );
  QCOMPARE( doneSpy.count(), 1 );
  QCOMPARE( doneSpy.first().at( 0 ).toString(), layer->id() );
  QCOMPARE( doneSpy.first().at( 1 ).toBool(), true );
  QCOMPARE( index.reindexedLayerIds, QStringList { layer->id() } );
  QCOMPARE( index.directReindexLayerCalls, 0 );

  QgsProject::instance()->removeMapLayer( layer.release() );
}

void TestQgsAiLayerIndexCoordinator::layerWillBeRemovedDropsChunksImmediately()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  QgsAiWorkspaceIndex index( &contextProvider, nullptr );

  // Pre-populate the index with chunker output for a real shapefile,
  // bypassing the embedding step with dummy vectors.
  const QString shpPath = copyPointsShapefile( tempDir.path() );
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );

  const auto chunks = QgsAiLayerChunker::chunkVector( layer.get() );
  QVERIFY( !chunks.isEmpty() );
  QList<QVector<float>> embeddings;
  embeddings.reserve( chunks.size() );
  for ( int i = 0; i < chunks.size(); ++i )
  {
    QVector<float> v( 4 );
    v[0] = 0.1f * i;
    embeddings.append( v );
  }
  QString err;
  QVERIFY( index.persistChunks( chunks, embeddings, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );
  QVERIFY( index.status().layerChunkCount > 0 );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );

  QgsProject::instance()->addMapLayer( layer.get(), false );
  coord.setEnabled( true );

  // Removing the layer must purge its chunks synchronously (no debounce).
  const QString layerId = layer->id();
  QgsProject::instance()->removeMapLayer( layer.release() );
  QCOMPARE( index.status().layerChunkCount, 0 );
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, layerId ).size(), 0 );
}

void TestQgsAiLayerIndexCoordinator::disabledCoordinatorIgnoresProjectSignals()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  CountingWorkspaceIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );
  // intentionally not enabling the coordinator

  const QString shpPath = copyPointsShapefile( tempDir.path() );
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer.get(), false );

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );
  QVERIFY( layer->startEditing() );
  QVERIFY( layer->commitChanges() );

  // Wait twice the debounce window — nothing should have fired.
  QTest::qWait( 200 );
  QCOMPARE( doneSpy.count(), 0 );
  QCOMPARE( index.directReindexLayerCalls, 0 );

  QgsProject::instance()->removeMapLayer( layer.release() );
}

void TestQgsAiLayerIndexCoordinator::unavailableEmbeddingProviderPreventsScheduling()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiUnavailableLocalEmbeddingProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );

  const QString shpPath = copyPointsShapefile( tempDir.path() );
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer.get(), false );

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );
  coord.setEnabled( true );

  QTest::qWait( 200 );
  QCOMPARE( doneSpy.count(), 0 );

  QgsProject::instance()->removeMapLayer( layer.release() );
}

void TestQgsAiLayerIndexCoordinator::providerUnavailableMidBatchStopsBatch()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  BreakerTrippingIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );

  // Several layers all become dirty in the same flush pass.
  QList<QgsVectorLayer *> layers;
  for ( int i = 0; i < 4; ++i )
  {
    auto *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=id:integer"_s, u"mem%1"_s.arg( i ), u"memory"_s );
    QVERIFY( layer->isValid() );
    QgsProject::instance()->addMapLayer( layer, false );
    layers.append( layer );
  }

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );
  coord.setEnabled( true );

  // The first reindex fails and trips the provider to unavailable. The coordinator
  // must stop the batch immediately instead of attempting (and logging a warning
  // for) every remaining layer.
  QVERIFY( doneSpy.wait( 5000 ) );
  QTest::qWait( 300 ); // give any erroneous extra iterations a chance to fire

  QCOMPARE( index.reindexedLayerIds.size(), 1 );
  QCOMPARE( index.directReindexLayerCalls, 0 );
  QCOMPARE( doneSpy.count(), 1 );
  QCOMPARE( doneSpy.first().at( 1 ).toBool(), false );

  coord.setEnabled( false );
  for ( int i = 0; i < layers.size(); ++i )
    QgsProject::instance()->removeMapLayer( layers.at( i ) );
}


void TestQgsAiLayerIndexCoordinator::multipleLayersAreIndexedOneAtATime()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  CountingWorkspaceIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );

  QList<QgsVectorLayer *> layers;
  for ( int i = 0; i < 3; ++i )
  {
    auto *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=id:integer"_s, u"mem%1"_s.arg( i ), u"memory"_s );
    QVERIFY( layer->isValid() );
    QgsProject::instance()->addMapLayer( layer, false );
    layers.append( layer );
  }

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );
  coord.setEnabled( true );

  for ( int i = 0; i < 3; ++i )
  {
    QVERIFY( doneSpy.wait( 5000 ) );
  }

  QCOMPARE( doneSpy.count(), 3 );
  QCOMPARE( index.reindexedLayerIds.size(), 3 );

  coord.setEnabled( false );
  for ( QgsVectorLayer *layer : layers )
    QgsProject::instance()->removeMapLayer( layer );
}

void TestQgsAiLayerIndexCoordinator::bulkReindexUsesLongerDebounceWindow()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  CountingWorkspaceIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 400 );

  for ( int i = 0; i < 2; ++i )
  {
    auto *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=id:integer"_s, u"mem%1"_s.arg( i ), u"memory"_s );
    QVERIFY( layer->isValid() );
    QgsProject::instance()->addMapLayer( layer, false );
  }

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );
  coord.setEnabled( true );

  QTest::qWait( 150 );
  QCOMPARE( doneSpy.count(), 0 );

  QVERIFY( doneSpy.wait( 5000 ) );
  QVERIFY( doneSpy.count() >= 1 );

  coord.setEnabled( false );
  QgsProject::instance()->clear();
}


void TestQgsAiLayerIndexCoordinator::bulkOperationPausesFlushWithoutClearingDirtySet()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  CountingWorkspaceIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );
  coord.setInterFlushDelayMs( 0 );
  coord.setEnabled( true );

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );

  coord.beginBulkOperation();
  QVERIFY( coord.isBulkOperationActive() );

  // layers added while the bulk operation is active are queued, not flushed
  const QString shpPath = copyPointsShapefile( tempDir.path() );
  QList<QString> layerIds;
  for ( int i = 0; i < 3; i++ )
  {
    QgsVectorLayer *layer = new QgsVectorLayer( shpPath, u"points_%1"_s.arg( i ), u"ogr"_s );
    QVERIFY( layer->isValid() );
    QgsProject::instance()->addMapLayer( layer, false );
    layerIds.append( layer->id() );
  }

  QTest::qWait( 300 );
  QCOMPARE( doneSpy.count(), 0 );

  // ending the bulk operation flushes the preserved dirty set, one layer at a time
  coord.endBulkOperation();
  QVERIFY( !coord.isBulkOperationActive() );

  while ( doneSpy.count() < 3 )
  {
    if ( !doneSpy.wait( 5000 ) )
      break;
  }
  QCOMPARE( doneSpy.count(), 3 );
  QCOMPARE( index.reindexedLayerIds.size(), 3 );
  for ( const QString &layerId : layerIds )
    QVERIFY( index.reindexedLayerIds.contains( layerId ) );

  coord.setEnabled( false );
}

void TestQgsAiLayerIndexCoordinator::nestedBulkOperationsResumeOnlyAtDepthZero()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  CountingWorkspaceIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );
  coord.setInterFlushDelayMs( 0 );
  coord.setEnabled( true );

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );

  coord.beginBulkOperation();
  coord.beginBulkOperation();

  const QString shpPath = copyPointsShapefile( tempDir.path() );
  QgsVectorLayer *layer = new QgsVectorLayer( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer, false );

  // ending only one of two nested operations must not resume flushing
  coord.endBulkOperation();
  QVERIFY( coord.isBulkOperationActive() );
  QTest::qWait( 300 );
  QCOMPARE( doneSpy.count(), 0 );

  coord.endBulkOperation();
  QVERIFY( !coord.isBulkOperationActive() );
  QVERIFY( doneSpy.wait( 5000 ) );
  QCOMPARE( doneSpy.count(), 1 );

  // an unbalanced extra end is ignored
  coord.endBulkOperation();
  QVERIFY( !coord.isBulkOperationActive() );

  coord.setEnabled( false );
}

void TestQgsAiLayerIndexCoordinator::closingTheProjectKeepsLayerChunks()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiWorkspaceIndex index( &contextProvider, nullptr );

  const QString shpPath = copyPointsShapefile( tempDir.path() );
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );
  const auto chunks = QgsAiLayerChunker::chunkVector( layer.get() );
  QList<QVector<float>> embeddings;
  for ( int i = 0; i < chunks.size(); ++i )
    embeddings.append( QVector<float> { 0.1f * i, 1.0f, 0.0f, 0.0f } );
  QString err;
  QVERIFY( index.persistChunks( chunks, embeddings, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );
  const QString layerId = layer->id();
  QgsProject::instance()->addMapLayer( layer.release(), false );
  coord.setEnabled( true );

  // Closing the project (or opening another) keeps the chunks: reopening it reuses them.
  QgsProject::instance()->clear();
  QVERIFY( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, layerId ).size() > 0 );
}

void TestQgsAiLayerIndexCoordinator::editsAreIndexedOnceSaved()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  CountingWorkspaceIndex index( &contextProvider );

  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 100 );
  coord.setBulkDebounceMs( 100 );

  const QString shpPath = copyPointsShapefile( tempDir.path() );
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer.get(), false );

  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );
  coord.setEnabled( true );
  QVERIFY( doneSpy.wait( 5000 ) );
  doneSpy.clear();
  index.reindexedLayerIds.clear();

  // Edits in progress are not indexed…
  QVERIFY( layer->startEditing() );
  QgsFeature feature( layer->fields() );
  feature.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  QVERIFY( layer->addFeature( feature ) );
  QVERIFY( layer->changeAttributeValue( feature.id(), 0, u"edited"_s ) );
  QVERIFY( !doneSpy.wait( 500 ) );
  QVERIFY( index.reindexedLayerIds.isEmpty() );

  // …the saved result is, once.
  QVERIFY( layer->commitChanges() );
  QVERIFY( doneSpy.wait( 5000 ) );
  QVERIFY( !doneSpy.wait( 400 ) );
  QCOMPARE( index.reindexedLayerIds, QStringList { layer->id() } );

  QgsProject::instance()->removeMapLayer( layer.release() );
}

void TestQgsAiLayerIndexCoordinator::layerRemovedDuringItsTaskLeavesNoChunks()
{
  //! Takes a while to index a layer, and counts removals.
  class SlowIndex : public QgsAiWorkspaceIndex // clazy:exclude=missing-qobject-macro
  {
    public:
      explicit SlowIndex( QgsAiFileContextProvider *contextProvider )
        : QgsAiWorkspaceIndex( contextProvider, nullptr )
      {}
      bool embeddingProviderAvailable() const override { return true; }
      bool reindexLayerSnapshot( const QgsAiWorkspaceIndex::WorkspaceLayerSnapshot &, QString * = nullptr, QgsFeedback * = nullptr ) override
      {
        started = true;
        QThread::msleep( 600 );
        return true;
      }
      bool removeLayer( const QString &layerId, QString * = nullptr ) override
      {
        removedIds.append( layerId );
        return true;
      }
      std::atomic_bool started { false };
      QStringList removedIds;
  };

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  SlowIndex index( &contextProvider );
  QgsAiLayerIndexCoordinator coord( &index );
  coord.setDebounceMs( 50 );
  coord.setBulkDebounceMs( 50 );

  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326"_s, u"points"_s, u"memory"_s );
  const QString layerId = layer->id();
  QgsProject::instance()->addMapLayer( layer, false );
  QSignalSpy doneSpy( &coord, &QgsAiLayerIndexCoordinator::reindexFinished );
  coord.setEnabled( true );
  QTRY_VERIFY_WITH_TIMEOUT( index.started, 5000 );

  // Removed while its chunks are being written: removed at once, and again once the task ends,
  // because the task may still write them.
  QgsProject::instance()->removeMapLayer( layer );
  QCOMPARE( index.removedIds, QStringList { layerId } );
  QVERIFY( doneSpy.wait( 5000 ) );
  QCOMPARE( index.removedIds, QStringList( { layerId, layerId } ) );
}

QGSTEST_MAIN( TestQgsAiLayerIndexCoordinator )
#include "testqgsailayerindexcoordinator.moc"
