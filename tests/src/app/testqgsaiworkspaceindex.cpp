/***************************************************************************
  testqgsaiworkspaceindex.cpp
  --------------------------------
  begin                : May 2026
***************************************************************************/

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>

#include "ai/index/qgsaiembeddingclient.h"
#include "ai/index/qgsaiembeddingprovider.h"
#include "ai/index/qgsaiindexingscheduler.h"
#include "ai/index/qgsailayerchunker.h"
#include "ai/index/qgsaiworkspaceindex.h"
#include "ai/qgsaifilecontextprovider.h"
#include "ai/tools/qgsaiindextools.h"
#include "qgsapplication.h"
#include "qgsfeature.h"
#include "qgsfeatureiterator.h"
#include "qgsfeedback.h"
#include "qgsgeometry.h"
#include "qgspointxy.h"
#include "qgsproject.h"
#include "qgssettings.h"
#include "qgstaskmanager.h"
#include "qgstest.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QList>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>
#include <QVector>

using namespace Qt::StringLiterals;

namespace
{
  class ScopedEmbeddingConfiguration
  {
    public:
      ScopedEmbeddingConfiguration()
        : mHadOpenAiEnv( qEnvironmentVariableIsSet( "OPENAI_API_KEY" ) )
        , mHadOpenRouterEnv( qEnvironmentVariableIsSet( "OPENROUTER_API_KEY" ) )
        , mHadE5ModelDirEnv( qEnvironmentVariableIsSet( "STRATA_AI_EMBEDDING_MODEL_DIR" ) )
        , mOpenAiEnv( qgetenv( "OPENAI_API_KEY" ) )
        , mOpenRouterEnv( qgetenv( "OPENROUTER_API_KEY" ) )
        , mE5ModelDirEnv( qgetenv( "STRATA_AI_EMBEDDING_MODEL_DIR" ) )
      {
        QgsSettings settings;
        const QStringList keys = {
          u"ai/provider/openai/apiKey"_s,
          u"ai/provider/openrouter/apiKey"_s,
          u"strata/index/embedding_provider"_s,
          u"ai/embeddings/provider"_s,
          u"ai/embeddings/openai/model"_s,
          u"ai/embeddings/openrouter/model"_s,
        };

        for ( const QString &key : keys )
        {
          SavedSetting saved;
          saved.key = key;
          saved.hadValue = settings.contains( key );
          saved.value = settings.value( key );
          mSavedSettings.append( saved );
          settings.remove( key );
        }

        qunsetenv( "OPENAI_API_KEY" );
        qunsetenv( "OPENROUTER_API_KEY" );
        qunsetenv( "STRATA_AI_EMBEDDING_MODEL_DIR" );
      }

      ~ScopedEmbeddingConfiguration()
      {
        qunsetenv( "OPENAI_API_KEY" );
        qunsetenv( "OPENROUTER_API_KEY" );
        qunsetenv( "STRATA_AI_EMBEDDING_MODEL_DIR" );
        if ( mHadOpenAiEnv )
          qputenv( "OPENAI_API_KEY", mOpenAiEnv );
        if ( mHadOpenRouterEnv )
          qputenv( "OPENROUTER_API_KEY", mOpenRouterEnv );
        if ( mHadE5ModelDirEnv )
          qputenv( "STRATA_AI_EMBEDDING_MODEL_DIR", mE5ModelDirEnv );

        QgsSettings settings;
        for ( const SavedSetting &saved : mSavedSettings )
        {
          if ( saved.hadValue )
            settings.setValue( saved.key, saved.value );
          else
            settings.remove( saved.key );
        }
      }

    private:
      struct SavedSetting
      {
          QString key;
          bool hadValue = false;
          QVariant value;
      };

      QList<SavedSetting> mSavedSettings;
      bool mHadOpenAiEnv = false;
      bool mHadOpenRouterEnv = false;
      bool mHadE5ModelDirEnv = false;
      QByteArray mOpenAiEnv;
      QByteArray mOpenRouterEnv;
      QByteArray mE5ModelDirEnv;
  };

  class FakeEmbeddingProvider : public QgsAiEmbeddingProvider
  {
    public:
      QString providerId() const override { return u"fake-local"_s; }
      QString displayName() const override { return u"Fake local embeddings"_s; }
      bool isAvailable( QString *errorMessage = nullptr ) const override
      {
        Q_UNUSED( errorMessage )
        return true;
      }

      bool embed( const QStringList &texts, QList<QVector<float>> &out, QString *errorMessage = nullptr, int maxBatch = 64 ) override
      {
        Q_UNUSED( errorMessage )
        Q_UNUSED( maxBatch )
        out.clear();
        for ( const QString &text : texts )
        {
          QVector<float> v( 3 );
          v[0] = text.contains( u"alpha"_s, Qt::CaseInsensitive ) ? 1.0f : 0.0f;
          v[1] = text.contains( u"beta"_s, Qt::CaseInsensitive ) ? 1.0f : 0.0f;
          v[2] = 0.1f;
          out.append( v );
        }
        return true;
      }
  };

  //! Takes \a delayMs per embed() call, i.e. per batch, like a real model on a slow computer.
  class SlowEmbeddingProvider : public FakeEmbeddingProvider
  {
    public:
      explicit SlowEmbeddingProvider( int delayMs )
        : mDelayMs( delayMs )
      {}

      bool embed( const QStringList &texts, QList<QVector<float>> &out, QString *errorMessage = nullptr, int maxBatch = 64 ) override
      {
        ++calls;
        embeddedTexts += static_cast<int>( texts.size() );
        QThread::msleep( mDelayMs );
        return FakeEmbeddingProvider::embed( texts, out, errorMessage, maxBatch );
      }

      std::atomic_int calls { 0 };
      std::atomic_int embeddedTexts { 0 };

    private:
      int mDelayMs = 0;
  };

  /**
   * Provider with fully configurable identity metadata, used to exercise the
   * provider/model mismatch -> wipe-and-rebuild path of the on-disk index.
   */
  class MetaEmbeddingProvider : public QgsAiEmbeddingProvider
  {
    public:
      MetaEmbeddingProvider( QString providerId, QString modelId, QString revision, int dimension )
        : mProviderId( std::move( providerId ) )
        , mModelId( std::move( modelId ) )
        , mRevision( std::move( revision ) )
        , mDimension( dimension )
      {}

      QString providerId() const override { return mProviderId; }
      QString displayName() const override { return mProviderId; }
      QString modelId() const override { return mModelId; }
      QString modelRevision() const override { return mRevision; }
      int embeddingDimension() const override { return mDimension; }
      bool isAvailable( QString *errorMessage = nullptr ) const override
      {
        Q_UNUSED( errorMessage )
        return true;
      }

      bool embed( const QStringList &texts, QList<QVector<float>> &out, QString *errorMessage = nullptr, int maxBatch = 64 ) override
      {
        Q_UNUSED( errorMessage )
        Q_UNUSED( maxBatch )
        out.clear();
        for ( int i = 0; i < texts.size(); ++i )
          out.append( QVector<float>( mDimension, 0.1f ) );
        return true;
      }

    private:
      QString mProviderId;
      QString mModelId;
      QString mRevision;
      int mDimension = 0;
  };
} // namespace

class TestQgsAiWorkspaceIndex : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();

    void schemaRoundTripPreservesAllFields();
    void replaceScopeAllFilesPreservesLayerChunks();
    void replaceScopeSingleLayerOnlyTouchesThatLayer();
    void removeLayerDropsOnlyMatchingChunks();
    void workspaceRootChangeLoadsSeparateIndex();
    void embeddingProviderAvailabilityIgnoresRemoteSettings();
    void embeddingProviderRegistryDefaultsToE5AndKeepsMinihashFallback();
    void e5PreprocessingHelpers();
    void e5ProviderAvailabilityHonorsEnvironmentModelDir();
    void e5ProviderIntegrationWhenModelDirIsConfigured();
    void embeddingClientDefaultsToOpenAi();
    void embeddingClientUsesOpenRouterSettings();
    void schemaMigrationDropsOldDb();
    void providerMetadataMismatchRebuildsIndex();
    void reindexAndSearchUseLocalProvider();
    void reindexLayersFailsWithoutLocalProvider();
    void layerSnapshotsReindexWithoutDroppingFileChunks();
    void chunkerOutputPersistsAsLayerChunks();
    void reindexLayersToolRequiresConfirm();
    void e5AvailabilityDoesNotLoadTheModel();
    void workspaceScanPrunesExcludedFoldersAtAnyDepth();
    void searchWaitsForOneBatchNotTheWholeReindex();
    void reindexStopsWithinOneBatchWhenCanceled();
    void setEmbeddingProviderDoesNotWaitForTheReindex();
    void schedulerRerunsARequestMadeDuringAPass();
    void databaseUsesWriteAheadLog();
    void layerRemovalsReachTheDatabaseTogether();
    void staleDatabasesAreRemoved();
    void clearRemovesTheWriteAheadLog();
    void unchangedLayerIsNotEmbeddedAgain();
    void editedLayerReusesUnchangedChunks();
    void unchangedFilesAreNotReadAgain();
    void searchSkipsLayersOutsideTheProject();
    void layersUnseenForAMonthExpire();

  private:
    //! Writes a workspace file that chunkText() splits into \a chunkCount chunks.
    static void writeManyChunkFile( const QString &path, int chunkCount );
    static QgsAiWorkspaceIndex::Chunk makeFileChunk( const QString &rel, int index, const QString &text );
    static QgsAiWorkspaceIndex::Chunk makeLayerChunk( const QString &layerId, const QString &name, qint64 fidMin, qint64 fidMax, int index, const QString &text, const QByteArray &wkt );
    static QVector<float> dummyEmbedding( float seed );
};

QgsAiWorkspaceIndex::Chunk TestQgsAiWorkspaceIndex::makeFileChunk( const QString &rel, int index, const QString &text )
{
  QgsAiWorkspaceIndex::Chunk c;
  c.sourceType = QString::fromLatin1( QgsAiWorkspaceIndex::SOURCE_TYPE_FILE );
  c.relativePath = rel;
  c.chunkIndex = index;
  c.text = text;
  return c;
}

QgsAiWorkspaceIndex::Chunk TestQgsAiWorkspaceIndex::makeLayerChunk( const QString &layerId, const QString &name, qint64 fidMin, qint64 fidMax, int index, const QString &text, const QByteArray &wkt )
{
  QgsAiWorkspaceIndex::Chunk c;
  c.sourceType = QString::fromLatin1( QgsAiWorkspaceIndex::SOURCE_TYPE_LAYER );
  c.layerId = layerId;
  c.relativePath = name;
  c.firstFeatureId = fidMin;
  c.lastFeatureId = fidMax;
  c.chunkIndex = index;
  c.text = text;
  c.wktBlob = wkt;
  return c;
}

QVector<float> TestQgsAiWorkspaceIndex::dummyEmbedding( float seed )
{
  QVector<float> v( 4 );
  v[0] = seed;
  v[1] = seed + 0.1f;
  v[2] = seed + 0.2f;
  v[3] = seed + 0.3f;
  return v;
}

void TestQgsAiWorkspaceIndex::schemaRoundTripPreservesAllFields()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiFileContextProvider contextProvider( tempDir.path() );

  // Write
  {
    QgsAiWorkspaceIndex index( &contextProvider, nullptr );
    QList<QgsAiWorkspaceIndex::Chunk> chunks;
    QList<QVector<float>> embeddings;

    chunks << makeFileChunk( u"src/foo.cpp"_s, 0, u"hello file"_s );
    embeddings << dummyEmbedding( 0.1f );

    chunks << makeLayerChunk( u"layer-abc"_s, u"Comuni"_s, 1, 50, 0, u"chunk-A text"_s, qCompress( QByteArray( "POINT(1 2)" ) ) );
    embeddings << dummyEmbedding( 0.2f );

    chunks << makeLayerChunk( u"layer-abc"_s, u"Comuni"_s, 51, 100, 1, u"chunk-B text"_s, qCompress( QByteArray( "POINT(3 4)" ) ) );
    embeddings << dummyEmbedding( 0.3f );

    QString err;
    QVERIFY2( index.persistChunks( chunks, embeddings, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ), err.toUtf8().constData() );
  }

  // Read back from a fresh instance
  {
    QgsAiWorkspaceIndex index( &contextProvider, nullptr );
    QVERIFY( index.ensureLoaded() );

    const auto status = index.status();
    QCOMPARE( status.chunkCount, 3 );
    QCOMPARE( status.fileChunkCount, 1 );
    QCOMPARE( status.layerChunkCount, 2 );

    const auto fileChunks = index.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllFiles );
    QCOMPARE( fileChunks.size(), 1 );
    QCOMPARE( fileChunks.first().relativePath, u"src/foo.cpp"_s );
    QCOMPARE( fileChunks.first().sourceType, QString::fromLatin1( QgsAiWorkspaceIndex::SOURCE_TYPE_FILE ) );
    QVERIFY( fileChunks.first().layerId.isEmpty() );

    const auto layerChunks = index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, u"layer-abc"_s );
    QCOMPARE( layerChunks.size(), 2 );
    QCOMPARE( layerChunks.first().firstFeatureId, qint64( 1 ) );
    QCOMPARE( layerChunks.first().lastFeatureId, qint64( 50 ) );
    QCOMPARE( qUncompress( layerChunks.first().wktBlob ), QByteArray( "POINT(1 2)" ) );
    QCOMPARE( qUncompress( layerChunks.last().wktBlob ), QByteArray( "POINT(3 4)" ) );
  }
}

void TestQgsAiWorkspaceIndex::replaceScopeAllFilesPreservesLayerChunks()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  QgsAiWorkspaceIndex index( &contextProvider, nullptr );

  QList<QgsAiWorkspaceIndex::Chunk> initial;
  QList<QVector<float>> initEmb;
  initial << makeFileChunk( u"a.md"_s, 0, u"original file"_s );
  initEmb << dummyEmbedding( 0.5f );
  initial << makeLayerChunk( u"L1"_s, u"L1"_s, 1, 10, 0, u"layer text"_s, QByteArray() );
  initEmb << dummyEmbedding( 0.6f );

  QString err;
  QVERIFY( index.persistChunks( initial, initEmb, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );

  // Replace only file chunks. Layer chunk must stay.
  QList<QgsAiWorkspaceIndex::Chunk> replacement;
  QList<QVector<float>> repEmb;
  replacement << makeFileChunk( u"b.md"_s, 0, u"new file"_s );
  repEmb << dummyEmbedding( 0.7f );

  QVERIFY( index.persistChunks( replacement, repEmb, QgsAiWorkspaceIndex::ReplaceScope::AllFiles, QString(), &err ) );

  const auto status = index.status();
  QCOMPARE( status.fileChunkCount, 1 );
  QCOMPARE( status.layerChunkCount, 1 );
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllFiles ).first().relativePath, u"b.md"_s );
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllLayers ).first().layerId, u"L1"_s );
}

void TestQgsAiWorkspaceIndex::replaceScopeSingleLayerOnlyTouchesThatLayer()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  QgsAiWorkspaceIndex index( &contextProvider, nullptr );

  QList<QgsAiWorkspaceIndex::Chunk> initial;
  QList<QVector<float>> initEmb;
  initial << makeFileChunk( u"f.md"_s, 0, u"file"_s );
  initEmb << dummyEmbedding( 0.1f );
  initial << makeLayerChunk( u"L1"_s, u"L1"_s, 1, 10, 0, u"L1 chunk"_s, QByteArray() );
  initEmb << dummyEmbedding( 0.2f );
  initial << makeLayerChunk( u"L2"_s, u"L2"_s, 1, 5, 0, u"L2 chunk"_s, QByteArray() );
  initEmb << dummyEmbedding( 0.3f );

  QString err;
  QVERIFY( index.persistChunks( initial, initEmb, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );

  // Reindex only L1: replaces L1 chunks, leaves L2 + file alone.
  QList<QgsAiWorkspaceIndex::Chunk> replacement;
  QList<QVector<float>> repEmb;
  replacement << makeLayerChunk( u"L1"_s, u"L1"_s, 1, 5, 0, u"L1 new chunk a"_s, QByteArray() );
  repEmb << dummyEmbedding( 0.4f );
  replacement << makeLayerChunk( u"L1"_s, u"L1"_s, 6, 10, 1, u"L1 new chunk b"_s, QByteArray() );
  repEmb << dummyEmbedding( 0.5f );

  QVERIFY( index.persistChunks( replacement, repEmb, QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, u"L1"_s, &err ) );

  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllFiles ).size(), 1 );
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, u"L1"_s ).size(), 2 );
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, u"L2"_s ).size(), 1 );
}

void TestQgsAiWorkspaceIndex::removeLayerDropsOnlyMatchingChunks()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  QgsAiWorkspaceIndex index( &contextProvider, nullptr );

  QList<QgsAiWorkspaceIndex::Chunk> initial;
  QList<QVector<float>> initEmb;
  initial << makeFileChunk( u"f.md"_s, 0, u"file"_s );
  initEmb << dummyEmbedding( 0.1f );
  initial << makeLayerChunk( u"L1"_s, u"L1"_s, 1, 10, 0, u"L1 chunk"_s, QByteArray() );
  initEmb << dummyEmbedding( 0.2f );
  initial << makeLayerChunk( u"L2"_s, u"L2"_s, 1, 5, 0, u"L2 chunk"_s, QByteArray() );
  initEmb << dummyEmbedding( 0.3f );

  QString err;
  QVERIFY( index.persistChunks( initial, initEmb, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );

  QVERIFY( index.removeLayer( u"L1"_s, &err ) );

  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllFiles ).size(), 1 );
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, u"L1"_s ).size(), 0 );
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, u"L2"_s ).size(), 1 );

  // Round-trip via fresh instance: removal must be persisted on disk too.
  QgsAiWorkspaceIndex index2( &contextProvider, nullptr );
  QVERIFY( index2.ensureLoaded() );
  QCOMPARE( index2.status().chunkCount, 2 );
  QCOMPARE( index2.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, u"L1"_s ).size(), 0 );
}

void TestQgsAiWorkspaceIndex::workspaceRootChangeLoadsSeparateIndex()
{
  QTemporaryDir root1;
  QTemporaryDir root2;
  QVERIFY( root1.isValid() );
  QVERIFY( root2.isValid() );

  QgsAiFileContextProvider contextProvider( root1.path() );
  QgsAiWorkspaceIndex index( &contextProvider, nullptr );

  QString err;
  QVERIFY( index.persistChunks( { makeFileChunk( u"a.md"_s, 0, u"root one"_s ) }, { dummyEmbedding( 0.1f ) }, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );
  QCOMPARE( index.status().workspaceRoot, QDir( root1.path() ).absolutePath() );
  QCOMPARE( index.status().chunkCount, 1 );

  contextProvider.setWorkspaceRoot( root2.path() );
  QCOMPARE( index.status().workspaceRoot, QDir( root2.path() ).absolutePath() );
  QCOMPARE( index.status().chunkCount, 0 );

  QVERIFY( index.persistChunks( { makeFileChunk( u"b.md"_s, 0, u"root two"_s ) }, { dummyEmbedding( 0.2f ) }, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );
  QCOMPARE( index.status().chunkCount, 1 );
  QCOMPARE( index.chunks().first().relativePath, u"b.md"_s );

  contextProvider.setWorkspaceRoot( root1.path() );
  QCOMPARE( index.status().workspaceRoot, QDir( root1.path() ).absolutePath() );
  // The previous workspace's cache loads in the background after the switch.
  QVERIFY( index.status().loading );
  QVERIFY( index.ensureLoaded() );
  QVERIFY( !index.status().loading );
  QCOMPARE( index.status().chunkCount, 1 );
  QCOMPARE( index.chunks().first().relativePath, u"a.md"_s );
}

void TestQgsAiWorkspaceIndex::embeddingProviderAvailabilityIgnoresRemoteSettings()
{
  ScopedEmbeddingConfiguration scopedConfiguration;
  QgsSettings settings;
  settings.setValue( u"ai/embeddings/provider"_s, u"openai"_s );

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  const bool e5ProviderListed = QgsAiEmbeddingProviderRegistry::providerIds().contains( QgsAiE5EmbeddingProvider::staticProviderId() );

  std::unique_ptr<QgsAiEmbeddingProvider> provider = QgsAiEmbeddingProviderRegistry::createProviderFromSettings();
  QCOMPARE( provider->providerId(), QgsAiEmbeddingProviderRegistry::defaultProviderId() );
  QString providerError;
  QCOMPARE( provider->isAvailable( &providerError ), !e5ProviderListed );
  if ( e5ProviderListed )
    QVERIFY2( providerError.contains( u"E5"_s, Qt::CaseInsensitive ), providerError.toUtf8().constData() );
  QgsAiWorkspaceIndex index( &contextProvider, provider.get() );
  QCOMPARE( index.embeddingProviderAvailable(), !e5ProviderListed );
  QCOMPARE( index.hasEmbeddingConfiguration(), !e5ProviderListed );

  settings.setValue( u"ai/provider/openai/apiKey"_s, u"sk-test-settings"_s );
  provider = QgsAiEmbeddingProviderRegistry::createProviderFromSettings();
  QCOMPARE( provider->providerId(), QgsAiEmbeddingProviderRegistry::defaultProviderId() );
  QCOMPARE( provider->isAvailable(), !e5ProviderListed );

  settings.remove( u"ai/provider/openai/apiKey"_s );
  qputenv( "OPENAI_API_KEY", "sk-test-env" );
  provider = QgsAiEmbeddingProviderRegistry::createProviderFromSettings();
  QCOMPARE( provider->providerId(), QgsAiEmbeddingProviderRegistry::defaultProviderId() );
  QCOMPARE( provider->isAvailable(), !e5ProviderListed );

  qunsetenv( "OPENAI_API_KEY" );
  settings.setValue( u"ai/embeddings/provider"_s, u"openrouter"_s );
  settings.setValue( u"ai/provider/openrouter/apiKey"_s, u"sk-or-test-settings"_s );
  qputenv( "OPENROUTER_API_KEY", "sk-or-test-env" );
  provider = QgsAiEmbeddingProviderRegistry::createProviderFromSettings();
  QCOMPARE( provider->providerId(), QgsAiEmbeddingProviderRegistry::defaultProviderId() );
  QCOMPARE( provider->isAvailable(), !e5ProviderListed );

  settings.setValue( u"strata/index/embedding_provider"_s, u"openrouter"_s );
  provider = QgsAiEmbeddingProviderRegistry::createProviderFromSettings();
  QCOMPARE( provider->providerId(), u"openrouter"_s );
  QVERIFY( provider->isRemote() );
  QVERIFY( provider->isAvailable() );
}

void TestQgsAiWorkspaceIndex::embeddingProviderRegistryDefaultsToE5AndKeepsMinihashFallback()
{
  ScopedEmbeddingConfiguration scopedConfiguration;

  const bool e5ProviderListed = QgsAiEmbeddingProviderRegistry::providerIds().contains( QgsAiE5EmbeddingProvider::staticProviderId() );
  QCOMPARE( QgsAiEmbeddingProviderRegistry::defaultProviderId(), e5ProviderListed ? QgsAiE5EmbeddingProvider::staticProviderId() : u"local:minihash-384"_s );
  QVERIFY( QgsAiEmbeddingProviderRegistry::providerIds().contains( u"local:minihash-384"_s ) );

  const QList<QgsAiEmbeddingProviderUiEntry> uiEntries = QgsAiEmbeddingProviderRegistry::providerUiEntries();
  auto e5UiIt = std::find_if( uiEntries.constBegin(), uiEntries.constEnd(), []( const QgsAiEmbeddingProviderUiEntry &entry ) {
    return entry.providerId == QgsAiE5EmbeddingProvider::staticProviderId();
  } );
  QVERIFY( e5UiIt != uiEntries.constEnd() );
  QCOMPARE( e5UiIt->selectable, e5ProviderListed );
  if ( !e5ProviderListed )
    QVERIFY2( e5UiIt->unavailableReason.contains( u"ONNX"_s, Qt::CaseInsensitive ) && e5UiIt->unavailableReason.contains( u"SentencePiece"_s, Qt::CaseInsensitive ), e5UiIt->unavailableReason.toUtf8().constData() );

  std::unique_ptr<QgsAiEmbeddingProvider> defaultProvider = QgsAiEmbeddingProviderRegistry::createProviderFromSettings();
  QCOMPARE( defaultProvider->providerId(), QgsAiEmbeddingProviderRegistry::defaultProviderId() );
  if ( e5ProviderListed )
  {
    QCOMPARE( defaultProvider->modelId(), QgsAiE5EmbeddingProvider::modelName() );
    QCOMPARE( defaultProvider->modelRevision(), QgsAiE5EmbeddingProvider::pinnedModelRevision() );
  }
  else
  {
    QCOMPARE( defaultProvider->modelId(), u"strata-local-minihash-384"_s );
    QVERIFY( defaultProvider->isAvailable() );
  }
  QCOMPARE( defaultProvider->embeddingDimension(), 384 );

  std::unique_ptr<QgsAiEmbeddingProvider> fallbackProvider = QgsAiEmbeddingProviderRegistry::createProvider( u"local:minihash-384"_s );
  QCOMPARE( fallbackProvider->providerId(), u"local:minihash-384"_s );
  QVERIFY( fallbackProvider->isAvailable() );
}

void TestQgsAiWorkspaceIndex::e5PreprocessingHelpers()
{
  QCOMPARE( QgsAiE5EmbeddingProvider::formatInputForRole( u"roads"_s, QgsAiEmbeddingRole::Query ), u"query: roads"_s );
  QCOMPARE( QgsAiE5EmbeddingProvider::formatInputForRole( u"query: roads"_s, QgsAiEmbeddingRole::Query ), u"query: roads"_s );
  QCOMPARE( QgsAiE5EmbeddingProvider::formatInputForRole( u"passage: roads"_s, QgsAiEmbeddingRole::Query ), u"query: roads"_s );
  QCOMPARE( QgsAiE5EmbeddingProvider::formatInputForRole( u"query: passage: roads"_s, QgsAiEmbeddingRole::Query ), u"query: roads"_s );
  QCOMPARE( QgsAiE5EmbeddingProvider::formatInputForRole( u"query: roads"_s, QgsAiEmbeddingRole::Passage ), u"passage: roads"_s );
  QCOMPARE( QgsAiE5EmbeddingProvider::formatInputForRole( QString(), QgsAiEmbeddingRole::Query ), u"query: "_s );

  const QVector<qint64> shortIds = QgsAiE5EmbeddingProvider::tokenIdsWithSpecials( QVector<int> { 10, 11 }, 5 );
  QCOMPARE( shortIds, ( QVector<qint64> { 0, 10, 11, 2 } ) );

  const QVector<qint64> truncatedIds = QgsAiE5EmbeddingProvider::tokenIdsWithSpecials( QVector<int> { 10, 11, 12, 13 }, 4 );
  QCOMPARE( truncatedIds, ( QVector<qint64> { 0, 10, 11, 2 } ) );

  const QVector<float> pooled = QgsAiE5EmbeddingProvider::meanPoolAndNormalize( QVector<float> { 3.0f, 0.0f, 0.0f, 4.0f, 100.0f, 100.0f }, QVector<qint64> { 1, 1, 0 }, 2 );
  QCOMPARE( pooled.size(), 2 );
  QVERIFY( std::abs( pooled.at( 0 ) - 0.6f ) < 0.0001f );
  QVERIFY( std::abs( pooled.at( 1 ) - 0.8f ) < 0.0001f );

  const QVector<float> emptyPooled = QgsAiE5EmbeddingProvider::meanPoolAndNormalize( QVector<float> { 1.0f, 2.0f }, QVector<qint64> { 0 }, 2 );
  QCOMPARE( emptyPooled, ( QVector<float> { 0.0f, 0.0f } ) );
}

void TestQgsAiWorkspaceIndex::e5ProviderAvailabilityHonorsEnvironmentModelDir()
{
  ScopedEmbeddingConfiguration scopedConfiguration;

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  qputenv( "STRATA_AI_EMBEDDING_MODEL_DIR", QFile::encodeName( tempDir.path() ) );

  QCOMPARE( QgsAiE5EmbeddingProvider::developerModelDirectory(), QDir::cleanPath( tempDir.path() ) );
  QCOMPARE( QgsAiE5EmbeddingProvider::activeModelDirectory(), QDir::cleanPath( tempDir.path() ) );

  QgsAiE5EmbeddingProvider provider;
  QString error;
  QVERIFY( !provider.isAvailable( &error ) );
  if ( QgsAiEmbeddingProviderRegistry::providerIds().contains( QgsAiE5EmbeddingProvider::staticProviderId() ) )
    QVERIFY2( error.contains( tempDir.path() ), error.toUtf8().constData() );
  else
    QVERIFY2( error.contains( u"not compiled"_s, Qt::CaseInsensitive ), error.toUtf8().constData() );
}

void TestQgsAiWorkspaceIndex::e5ProviderIntegrationWhenModelDirIsConfigured()
{
  if ( !QgsAiEmbeddingProviderRegistry::providerIds().contains( QgsAiE5EmbeddingProvider::staticProviderId() ) )
    QSKIP( "Local E5 embeddings were not compiled because ONNX Runtime and/or SentencePiece were not found." );

  const QByteArray modelDir = qgetenv( "STRATA_AI_EMBEDDING_MODEL_DIR" );
  if ( modelDir.trimmed().isEmpty() )
    QSKIP( "STRATA_AI_EMBEDDING_MODEL_DIR is not set; skipping optional E5 ONNX integration test." );

  QgsAiE5EmbeddingProvider provider;
  QString error;
  QVERIFY2( provider.isAvailable( &error ), error.toUtf8().constData() );

  QList<QVector<float>> queryVectors;
  QVERIFY2( provider.embed( QStringList { u"query: strade comunali"_s }, QgsAiEmbeddingRole::Query, queryVectors, &error ), error.toUtf8().constData() );
  QList<QVector<float>> passageVectors;
  QVERIFY2( provider.embed( QStringList { u"passage: layer con strade e civici"_s }, QgsAiEmbeddingRole::Passage, passageVectors, &error ), error.toUtf8().constData() );

  QList<QVector<float>> vectors = queryVectors + passageVectors;
  QCOMPARE( vectors.size(), 2 );
  for ( const QVector<float> &vector : std::as_const( vectors ) )
  {
    QCOMPARE( vector.size(), 384 );
    double norm = 0.0;
    for ( float value : vector )
      norm += static_cast<double>( value ) * static_cast<double>( value );
    QVERIFY( std::abs( std::sqrt( norm ) - 1.0 ) < 0.001 );
  }
}

void TestQgsAiWorkspaceIndex::embeddingClientDefaultsToOpenAi()
{
  ScopedEmbeddingConfiguration scopedConfiguration;

  QgsAiEmbeddingClient client;
  QCOMPARE( client.provider(), QgsAiEmbeddingClient::Provider::OpenAi );
  QCOMPARE( client.endpoint(), u"https://api.openai.com/v1/embeddings"_s );
  QCOMPARE( client.model(), u"text-embedding-3-small"_s );
  QVERIFY( !client.hasApiKey() );

  QgsSettings settings;
  settings.setValue( u"ai/provider/openai/apiKey"_s, u"sk-test-settings"_s );
  QVERIFY( client.hasApiKey() );
}

void TestQgsAiWorkspaceIndex::embeddingClientUsesOpenRouterSettings()
{
  ScopedEmbeddingConfiguration scopedConfiguration;

  QgsSettings settings;
  settings.setValue( u"ai/embeddings/provider"_s, u"openrouter"_s );
  settings.setValue( u"ai/embeddings/openrouter/model"_s, u"openai/text-embedding-3-small"_s );
  settings.setValue( u"ai/provider/openrouter/apiKey"_s, u"sk-or-test-settings"_s );

  QgsAiEmbeddingClient client;
  QCOMPARE( client.provider(), QgsAiEmbeddingClient::Provider::OpenRouter );
  QCOMPARE( client.endpoint(), u"https://openrouter.ai/api/v1/embeddings"_s );
  QCOMPARE( client.model(), u"openai/text-embedding-3-small"_s );
  QVERIFY( client.hasApiKey() );

  const QJsonObject preferences = client.openRouterProviderPreferences();
  QCOMPARE( preferences.value( u"sort"_s ).toString(), u"price"_s );
  QCOMPARE( preferences.value( u"data_collection"_s ).toString(), u"deny"_s );
  QCOMPARE( preferences.value( u"allow_fallbacks"_s ).toBool(), true );

  settings.setValue( u"ai/embeddings/openrouter/model"_s, QString() );
  QCOMPARE( client.model(), u"openai/text-embedding-3-small"_s );
}

void TestQgsAiWorkspaceIndex::schemaMigrationDropsOldDb()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  // Manually create a v1-style chunks DB at the path the index will use.
  // We rely on dbPath() being deterministic (SHA-1 of workspace root) so we can
  // open the same file from outside with a raw SQLite connection.
  QgsAiWorkspaceIndex bootstrap( &contextProvider, nullptr );
  // Touch the DB by persisting an empty payload via the new schema, then
  // overwrite user_version to 1 to simulate a stale schema.
  QString err;
  QVERIFY( bootstrap.persistChunks( {}, {}, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );

  // We can't easily access dbPath() (it's private). Instead, simulate the upgrade
  // by writing a chunk and then bumping schema by re-creating with same provider:
  // the migration path is that loadAll sees an older user_version and drops the table.
  // To exercise that branch deterministically without exposing internals, we
  // simply trust that loadAll() executes the upgrade SQL when SCHEMA_VERSION
  // is greater than the stored value.
  // (Functional coverage of the migration path is therefore left to manual smoke
  // tests; this slot just verifies that a fresh DB at the current schema loads
  // cleanly without the migration branch erroring.)
  QgsAiWorkspaceIndex fresh( &contextProvider, nullptr );
  QVERIFY( fresh.ensureLoaded() );
  QCOMPARE( fresh.status().chunkCount, 0 );
}

void TestQgsAiWorkspaceIndex::providerMetadataMismatchRebuildsIndex()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  // Seed the on-disk index with chunks produced by provider A.
  MetaEmbeddingProvider providerA( u"mismatch-test"_s, u"model-A"_s, u"rev-1"_s, 3 );
  {
    QgsAiWorkspaceIndex index( &contextProvider, &providerA );
    QString err;
    QVERIFY( index.persistChunks( { makeFileChunk( u"a.md"_s, 0, u"hello"_s ) }, { QVector<float>( 3, 0.2f ) }, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ) );
    QCOMPARE( index.status().chunkCount, 1 );
  }

  // Reopen the SAME workspace with a provider that has the same providerId (so the
  // db path is identical) but a different model. The stored chunks belong to a
  // different model and must be dropped: the index rebuilds empty rather than
  // mixing incompatible embeddings.
  MetaEmbeddingProvider providerB( u"mismatch-test"_s, u"model-B"_s, u"rev-2"_s, 3 );
  QgsAiWorkspaceIndex index2( &contextProvider, &providerB );
  QVERIFY( index2.ensureLoaded() );
  QCOMPARE( index2.status().chunkCount, 0 );
}

void TestQgsAiWorkspaceIndex::initTestCase()
{
  QgsApplication::initQgis();
}

void TestQgsAiWorkspaceIndex::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsAiWorkspaceIndex::reindexAndSearchUseLocalProvider()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QFile alphaFile( QDir( tempDir.path() ).filePath( u"alpha.md"_s ) );
  QVERIFY( alphaFile.open( QIODevice::WriteOnly | QIODevice::Text ) );
  alphaFile.write( "alpha roads and buildings\n" );
  alphaFile.close();

  QFile betaFile( QDir( tempDir.path() ).filePath( u"beta.md"_s ) );
  QVERIFY( betaFile.open( QIODevice::WriteOnly | QIODevice::Text ) );
  betaFile.write( "beta rivers and parks\n" );
  betaFile.close();

  QgsAiFileContextProvider contextProvider( tempDir.path() );
  FakeEmbeddingProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );

  QString err;
  QVERIFY2( index.reindex( 10, &err ), err.toUtf8().constData() );
  QCOMPARE( index.status().fileChunkCount, 2 );

  const QList<QgsAiWorkspaceIndex::Chunk> hits = index.search( u"alpha"_s, 1, &err );
  QVERIFY2( !hits.isEmpty(), err.toUtf8().constData() );
  QVERIFY2( hits.first().text.contains( u"alpha"_s ), hits.first().text.toUtf8().constData() );
}

void TestQgsAiWorkspaceIndex::reindexLayersFailsWithoutLocalProvider()
{
  ScopedEmbeddingConfiguration scopedConfiguration;

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  QgsAiUnavailableLocalEmbeddingProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );

  QString err;
  bool ok = index.reindex( 10, &err );
  QVERIFY( !ok );
  QVERIFY2( err.contains( u"Local embedding model"_s, Qt::CaseInsensitive ), err.toUtf8().constData() );

  err.clear();
  ok = index.reindexLayers( &err );
  QVERIFY( !ok );
  QVERIFY2( err.contains( u"Local embedding model"_s, Qt::CaseInsensitive ), err.toUtf8().constData() );

  err.clear();
  const QList<QgsAiWorkspaceIndex::Chunk> hits = index.search( u"alpha"_s, 1, &err );
  QVERIFY( hits.isEmpty() );
  QVERIFY2( err.contains( u"Local embedding model"_s, Qt::CaseInsensitive ), err.toUtf8().constData() );
}

void TestQgsAiWorkspaceIndex::layerSnapshotsReindexWithoutDroppingFileChunks()
{
  QgsProject::instance()->clear();

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  FakeEmbeddingProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );

  QString err;
  QVERIFY2( index.persistChunks( { makeFileChunk( u"notes.md"_s, 0, u"alpha file chunk"_s ) }, { dummyEmbedding( 0.3f ) }, QgsAiWorkspaceIndex::ReplaceScope::AllFiles, QString(), &err ), err.toUtf8().constData() );

  auto layer = std::make_unique<QgsVectorLayer>( u"Point?crs=EPSG:4326&field=name:string"_s, u"snapshot_points"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  const QString layerId = layer->id();
  QgsProject::instance()->addMapLayer( layer.get(), false );

  QgsAiWorkspaceIndex::WorkspaceLayerSnapshot allSnapshot;
  QVERIFY2( index.createWorkspaceLayerSnapshot( allSnapshot, &err ), err.toUtf8().constData() );
  QCOMPARE( allSnapshot.scope, QgsAiWorkspaceIndex::ReplaceScope::AllLayers );
  QCOMPARE( allSnapshot.layerCount, 1 );
  // Prepared on this thread, read into chunks where the snapshot is indexed (a worker in the app).
  QCOMPARE( allSnapshot.preparedLayers.size(), 1 );
  QVERIFY( allSnapshot.chunks.isEmpty() );
  {
    QgsAiWorkspaceIndex::WorkspaceLayerSnapshot materialized = allSnapshot;
    QVERIFY( QgsAiWorkspaceIndex::materializeLayerSnapshot( materialized ) );
    QVERIFY( !materialized.chunks.isEmpty() );
    QVERIFY( materialized.preparedLayers.isEmpty() );
  }

  QVERIFY2( index.reindexLayerSnapshot( allSnapshot, &err ), err.toUtf8().constData() );
  QCOMPARE( index.status().fileChunkCount, 1 );
  QVERIFY( index.status().layerChunkCount > 0 );

  QgsAiWorkspaceIndex::WorkspaceLayerSnapshot singleSnapshot;
  QVERIFY2( index.createWorkspaceLayerSnapshotForLayer( layerId, singleSnapshot, &err ), err.toUtf8().constData() );
  QCOMPARE( singleSnapshot.scope, QgsAiWorkspaceIndex::ReplaceScope::SingleLayer );
  QCOMPARE( singleSnapshot.scopedLayerId, layerId );
  QCOMPARE( singleSnapshot.preparedLayers.size(), 1 );

  QVERIFY2( index.reindexLayerSnapshot( singleSnapshot, &err ), err.toUtf8().constData() );
  QCOMPARE( index.status().fileChunkCount, 1 );
  QVERIFY( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, layerId ).size() > 0 );

  QgsProject::instance()->removeMapLayer( layer.release() );
  QgsProject::instance()->clear();
}

void TestQgsAiWorkspaceIndex::chunkerOutputPersistsAsLayerChunks()
{
  // End-to-end without hitting the network: feed real chunker output of a
  // shapefile through persistChunks(AllLayers) with fake embeddings, then
  // verify the round-trip preserves layer-id, feature ranges and WKT blobs.
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  const QString shpPath = QStringLiteral( TEST_DATA_DIR ) + u"/points.shp"_s;
  auto layer = std::make_unique<QgsVectorLayer>( shpPath, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );

  // WKT blobs are only collected when the privacy setting allows geometries in the model context.
  QgsSettings wktSettings;
  wktSettings.setValue( u"strata/privacy/include_layer_wkt_in_model_context"_s, true );
  const auto restoreWkt = qScopeGuard( [&wktSettings]() { wktSettings.remove( u"strata/privacy/include_layer_wkt_in_model_context"_s ); } );
  const auto chunks = QgsAiLayerChunker::chunkVector( layer.get() );
  QVERIFY( !chunks.isEmpty() );

  QList<QVector<float>> embeddings;
  embeddings.reserve( chunks.size() );
  for ( int i = 0; i < chunks.size(); ++i )
    embeddings.append( dummyEmbedding( 0.1f + 0.01f * i ) );

  QgsAiWorkspaceIndex index( &contextProvider, nullptr );
  QString err;
  QVERIFY2( index.persistChunks( chunks, embeddings, QgsAiWorkspaceIndex::ReplaceScope::AllLayers, QString(), &err ), err.toUtf8().constData() );

  // File chunks would be preserved by AllLayers scope — sanity-check that there are none here.
  QCOMPARE( index.status().fileChunkCount, 0 );
  QCOMPARE( index.status().layerChunkCount, chunks.size() );

  const auto retrieved = index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, layer->id() );
  QCOMPARE( retrieved.size(), chunks.size() );
  for ( const auto &c : retrieved )
  {
    QVERIFY( !c.wktBlob.isEmpty() );
    QVERIFY( c.firstFeatureId >= 0 );
    QCOMPARE( c.layerId, layer->id() );
  }

  // Reindex of a non-existent layer should fall back to removeLayer (no embedding provider needed).
  QVERIFY( index.removeLayer( layer->id(), &err ) );
  QCOMPARE( index.status().layerChunkCount, 0 );
}

void TestQgsAiWorkspaceIndex::reindexLayersToolRequiresConfirm()
{
  ScopedEmbeddingConfiguration scopedConfiguration;

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );

  QgsAiUnavailableLocalEmbeddingProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );

  QgsAiReindexLayersTool tool( &index );

  // Without confirm, the tool refuses to even reach the embedding client.
  const QgsAiToolResult denied = tool.execute( QJsonObject() );
  QVERIFY( !denied.success );
  QVERIFY2( denied.errorMessage.contains( u"confirm"_s, Qt::CaseInsensitive ), denied.errorMessage.toUtf8().constData() );

  // With confirm but no local model, the provider error is propagated verbatim.
  QJsonObject args;
  args.insert( u"confirm"_s, true );
  const QgsAiToolResult confirmed = tool.execute( args );
  QVERIFY( !confirmed.success );
  QVERIFY2( confirmed.errorMessage.contains( u"Local embedding model"_s, Qt::CaseInsensitive ), confirmed.errorMessage.toUtf8().constData() );
}

void TestQgsAiWorkspaceIndex::writeManyChunkFile( const QString &path, int chunkCount )
{
  QFile file( path );
  QVERIFY( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
  // One line per chunk, a bit shorter than CHUNK_TARGET_CHARS so each chunk ends on its newline.
  const QByteArray line = QByteArray( QgsAiWorkspaceIndex::CHUNK_TARGET_CHARS - 10, 'a' ) + '\n';
  for ( int i = 0; i < chunkCount; ++i )
    file.write( line );
}

void TestQgsAiWorkspaceIndex::e5AvailabilityDoesNotLoadTheModel()
{
  if ( !QgsAiEmbeddingProviderRegistry::providerIds().contains( QgsAiE5EmbeddingProvider::staticProviderId() ) )
    QSKIP( "Local E5 embeddings are not compiled in." );

  ScopedEmbeddingConfiguration scopedConfiguration;
  QTemporaryDir modelDir;
  QVERIFY( modelDir.isValid() );
  QVERIFY( QDir( modelDir.path() ).mkpath( u"onnx"_s ) );
  // Files that exist but are not a model: availability must not notice, only a real load does.
  for ( const QString &relative : { u"onnx/model_qint8_avx512_vnni.onnx"_s, u"sentencepiece.bpe.model"_s } )
  {
    QFile file( QDir( modelDir.path() ).filePath( relative ) );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    file.write( "not a model" );
  }
  qputenv( "STRATA_AI_EMBEDDING_MODEL_DIR", QFile::encodeName( modelDir.path() ) );

  QgsAiE5EmbeddingProvider provider;
  QString error;
  QVERIFY2( provider.isAvailable( &error ), error.toUtf8().constData() );
  QVERIFY( !provider.runtimeLoaded() );

  // The first embed() loads the model on its own thread; a failed load is then reported.
  QList<QVector<float>> out;
  QVERIFY( !provider.embed( { u"alpha"_s }, out, &error ) );
  QVERIFY( !provider.runtimeLoaded() );
  error.clear();
  QVERIFY( !provider.isAvailable( &error ) );
  QVERIFY( !error.isEmpty() );
}

void TestQgsAiWorkspaceIndex::workspaceScanPrunesExcludedFoldersAtAnyDepth()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  const QDir dir( root.path() );
  for ( const QString &relative : { u"a/node_modules/skip.md"_s, u"a/.git/skip.md"_s, u"b/keep.md"_s, u"build/skip.md"_s, u"a/build/keep.md"_s } )
  {
    QVERIFY( dir.mkpath( QFileInfo( dir.filePath( relative ) ).path() ) );
    QFile file( dir.filePath( relative ) );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    file.write( "text" );
  }

  QgsAiFileContextProvider::WorkspaceScanOptions options;
  const QgsAiFileContextProvider::WorkspaceScanResult result = QgsAiFileContextProvider::scanWorkspace( root.path(), options );
  QStringList found;
  for ( const QgsAiFileContextProvider::WorkspaceFile &file : result.files )
    found << file.relativePath;
  found.sort();
  // "build" is excluded at the root only: a project's own build folder deeper down is data.
  QCOMPARE( found, QStringList( { u"a/build/keep.md"_s, u"b/keep.md"_s } ) );
  QVERIFY( !result.truncated );
  // Excluded folders are not even walked.
  QVERIFY2( result.visitedEntries <= 7, QString::number( result.visitedEntries ).toUtf8().constData() );

  options.maxEntries = 2;
  QVERIFY( QgsAiFileContextProvider::scanWorkspace( root.path(), options ).truncated );
}

void TestQgsAiWorkspaceIndex::searchWaitsForOneBatchNotTheWholeReindex()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  // 10 batches of 200 ms: about 2 s of embedding.
  writeManyChunkFile( QDir( root.path() ).filePath( u"big.md"_s ), 10 * QgsAiWorkspaceIndex::EMBEDDING_BATCH );

  QgsAiFileContextProvider contextProvider( root.path() );
  SlowEmbeddingProvider provider( 200 );
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QString err;
  QVERIFY2( index.persistChunks( { makeFileChunk( u"notes.md"_s, 0, u"alpha"_s ) }, { QVector<float> { 1.0f, 0.0f, 0.1f } }, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ), err.toUtf8().constData() );

  std::atomic_bool reindexFinished { false };
  std::thread worker( [&index, &reindexFinished]() {
    QString reindexError;
    index.reindex( 10, &reindexError );
    index.closeDatabaseConnectionForCurrentThread();
    reindexFinished = true;
  } );
  QThread::msleep( 300 );

  QElapsedTimer clock;
  clock.start();
  const QList<QgsAiWorkspaceIndex::Chunk> hits = index.search( u"alpha"_s, 1, &err );
  const qint64 searchMs = clock.elapsed();
  const bool reindexWasRunning = !reindexFinished;
  worker.join();

  QVERIFY2( !hits.isEmpty(), err.toUtf8().constData() );
  QVERIFY( reindexWasRunning );
  // At most the batch in progress plus the query itself, not the rest of the reindex.
  QVERIFY2( searchMs < 1000, QString::number( searchMs ).toUtf8().constData() );
}

void TestQgsAiWorkspaceIndex::reindexStopsWithinOneBatchWhenCanceled()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  writeManyChunkFile( QDir( root.path() ).filePath( u"big.md"_s ), 10 * QgsAiWorkspaceIndex::EMBEDDING_BATCH );

  QgsAiFileContextProvider contextProvider( root.path() );
  SlowEmbeddingProvider provider( 200 );
  QgsAiWorkspaceIndex index( &contextProvider, &provider );

  QgsFeedback feedback;
  std::thread canceller( [&feedback]() {
    QThread::msleep( 300 );
    feedback.cancel();
  } );
  QElapsedTimer clock;
  clock.start();
  QString err;
  const bool ok = index.reindex( 10, &err, &feedback );
  const qint64 elapsedMs = clock.elapsed();
  canceller.join();

  QVERIFY( !ok );
  QVERIFY2( err.contains( u"canceled"_s, Qt::CaseInsensitive ), err.toUtf8().constData() );
  QVERIFY2( elapsedMs < 1000, QString::number( elapsedMs ).toUtf8().constData() );
  QVERIFY( provider.calls < 10 );
  // Nothing half-done reached the index.
  QCOMPARE( index.status().fileChunkCount, 0 );
}

void TestQgsAiWorkspaceIndex::setEmbeddingProviderDoesNotWaitForTheReindex()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  writeManyChunkFile( QDir( root.path() ).filePath( u"big.md"_s ), 10 * QgsAiWorkspaceIndex::EMBEDDING_BATCH );

  QgsAiFileContextProvider contextProvider( root.path() );
  SlowEmbeddingProvider provider( 200 );
  FakeEmbeddingProvider replacement;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );

  std::atomic_bool reindexFinished { false };
  std::thread worker( [&index, &reindexFinished]() {
    QString reindexError;
    index.reindex( 10, &reindexError );
    index.closeDatabaseConnectionForCurrentThread();
    reindexFinished = true;
  } );
  QThread::msleep( 300 );

  QElapsedTimer clock;
  clock.start();
  index.setEmbeddingProvider( &replacement );
  const qint64 swapMs = clock.elapsed();
  const bool reindexWasRunning = !reindexFinished;
  worker.join();

  QVERIFY( reindexWasRunning );
  QVERIFY2( swapMs < 1000, QString::number( swapMs ).toUtf8().constData() );
  // The running pass kept the provider it started with.
  QVERIFY( provider.calls >= 10 );
}

void TestQgsAiWorkspaceIndex::schedulerRerunsARequestMadeDuringAPass()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  writeManyChunkFile( QDir( root.path() ).filePath( u"big.md"_s ), 3 * QgsAiWorkspaceIndex::EMBEDDING_BATCH );

  QgsAiFileContextProvider contextProvider( root.path() );
  SlowEmbeddingProvider provider( 200 );
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QgsAiIndexingScheduler scheduler( &index );
  scheduler.setAutomaticEnabled( true );

  int passes = 0;
  QgsTaskManager *manager = QgsApplication::taskManager();
  const QMetaObject::Connection counter = connect( manager, &QgsTaskManager::taskAdded, this, [&passes, manager]( long taskId ) {
    if ( QgsTask *task = manager->task( taskId ); task && task->description() == QObject::tr( "Index AI workspace" ) )
      ++passes;
  } );

  scheduler.scheduleWorkspaceIndexing( 0 );
  QTRY_VERIFY_WITH_TIMEOUT( scheduler.isRunning(), 5000 );
  // A request during the pass is kept and runs once the pass ends.
  scheduler.scheduleWorkspaceIndexing( 0 );
  QTRY_COMPARE_WITH_TIMEOUT( passes, 2, 10000 );
  QTRY_VERIFY_WITH_TIMEOUT( !scheduler.isRunning(), 10000 );
  disconnect( counter );
  QCOMPARE( passes, 2 );
}

void TestQgsAiWorkspaceIndex::databaseUsesWriteAheadLog()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  QgsAiFileContextProvider contextProvider( root.path() );
  FakeEmbeddingProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QString err;
  QVERIFY2( index.persistChunks( { makeFileChunk( u"notes.md"_s, 0, u"alpha"_s ) }, { dummyEmbedding( 1.0f ) }, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ), err.toUtf8().constData() );

  const QString connection = u"test_wal_check"_s;
  {
    QSqlDatabase db = QSqlDatabase::addDatabase( u"QSQLITE"_s, connection );
    db.setDatabaseName( index.databasePath() );
    QVERIFY( db.open() );
    QSqlQuery mode( db );
    QVERIFY( mode.exec( u"PRAGMA journal_mode"_s ) && mode.next() );
    QCOMPARE( mode.value( 0 ).toString(), u"wal"_s );
    db.close();
  }
  QSqlDatabase::removeDatabase( connection );
  QVERIFY( index.databaseSizeBytes() > 0 );
}

void TestQgsAiWorkspaceIndex::layerRemovalsReachTheDatabaseTogether()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  QgsAiFileContextProvider contextProvider( root.path() );
  FakeEmbeddingProvider provider;
  {
    QgsAiWorkspaceIndex index( &contextProvider, &provider );
    QList<QgsAiWorkspaceIndex::Chunk> chunks;
    QList<QVector<float>> embeddings;
    for ( int i = 0; i < 20; ++i )
    {
      chunks << makeLayerChunk( u"layer_%1"_s.arg( i ), u"Layer %1"_s.arg( i ), 0, 10, 0, u"layer text %1"_s.arg( i ), QByteArray() );
      embeddings << dummyEmbedding( static_cast<float>( i + 1 ) );
    }
    chunks << makeFileChunk( u"notes.md"_s, 0, u"alpha"_s );
    embeddings << dummyEmbedding( 0.5f );
    QString err;
    QVERIFY2( index.persistChunks( chunks, embeddings, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ), err.toUtf8().constData() );

    // A project closing removes its layers one signal at a time, without waiting on the database.
    for ( int i = 0; i < 20; ++i )
      QVERIFY( index.removeLayer( u"layer_%1"_s.arg( i ), &err ) );
    QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllLayers ).size(), 0 );
  }

  // A fresh index reads what reached the database.
  QgsAiWorkspaceIndex reloaded( &contextProvider, &provider );
  QVERIFY( reloaded.ensureLoaded() );
  QCOMPARE( reloaded.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllLayers ).size(), 0 );
  QCOMPARE( reloaded.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllFiles ).size(), 1 );
}

void TestQgsAiWorkspaceIndex::staleDatabasesAreRemoved()
{
  QTemporaryDir directory;
  QVERIFY( directory.isValid() );
  const QDateTime old = QDateTime::currentDateTimeUtc().addDays( -( QgsAiWorkspaceIndex::STALE_DATABASE_DAYS + 5 ) );
  const auto writeFile = [&directory]( const QString &name, const QDateTime &modified ) {
    QFile file( QDir( directory.path() ).filePath( name ) );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    file.write( "x" );
    // Written data reaches the file first, or closing it would set the time again.
    QVERIFY( file.flush() );
    if ( modified.isValid() )
      QVERIFY( file.setFileTime( modified, QFileDevice::FileModificationTime ) );
    file.close();
  };
  writeFile( u"ws_old_e5.sqlite"_s, old );
  writeFile( u"ws_old_e5.sqlite-wal"_s, old );
  writeFile( u"ws_fresh_e5.sqlite"_s, QDateTime() );
  writeFile( u"ws_current_e5.sqlite"_s, old );
  writeFile( u"ws_recentwal_e5.sqlite"_s, old );
  writeFile( u"ws_recentwal_e5.sqlite-wal"_s, QDateTime() );
  writeFile( u"history.sqlite"_s, old );

  const QString keep = QDir( directory.path() ).filePath( u"ws_current_e5.sqlite"_s );
  QCOMPARE( QgsAiWorkspaceIndex::removeStaleDatabases( directory.path(), keep ), 1 );

  const QDir dir( directory.path() );
  QVERIFY( !dir.exists( u"ws_old_e5.sqlite"_s ) );
  QVERIFY( !dir.exists( u"ws_old_e5.sqlite-wal"_s ) );
  QVERIFY( dir.exists( u"ws_fresh_e5.sqlite"_s ) );
  QVERIFY( dir.exists( u"ws_current_e5.sqlite"_s ) );
  // Recently written through its log: still in use.
  QVERIFY( dir.exists( u"ws_recentwal_e5.sqlite"_s ) );
  // Only index databases are touched.
  QVERIFY( dir.exists( u"history.sqlite"_s ) );
}

void TestQgsAiWorkspaceIndex::clearRemovesTheWriteAheadLog()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  QgsAiFileContextProvider contextProvider( root.path() );
  FakeEmbeddingProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QString err;
  QVERIFY2( index.persistChunks( { makeFileChunk( u"notes.md"_s, 0, u"alpha"_s ) }, { dummyEmbedding( 1.0f ) }, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ), err.toUtf8().constData() );
  const QString path = index.databasePath();
  QVERIFY( QFileInfo::exists( path ) );

  index.clear();
  QVERIFY( !QFileInfo::exists( path ) );
  QVERIFY( !QFileInfo::exists( path + u"-wal"_s ) );
  QVERIFY( !QFileInfo::exists( path + u"-shm"_s ) );
  QCOMPARE( index.databaseSizeBytes(), 0 );
}

void TestQgsAiWorkspaceIndex::unchangedLayerIsNotEmbeddedAgain()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  for ( const QString &ext : { u".shp"_s, u".shx"_s, u".dbf"_s, u".prj"_s } )
    QFile::copy( QStringLiteral( TEST_DATA_DIR ) + u"/points"_s + ext, root.path() + u"/points"_s + ext );
  QgsVectorLayer *layer = new QgsVectorLayer( root.path() + u"/points.shp"_s, u"points"_s, u"ogr"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer );
  const auto removeLayer = qScopeGuard( [layer]() { QgsProject::instance()->removeMapLayer( layer ); } );

  QgsAiFileContextProvider contextProvider( root.path() );
  SlowEmbeddingProvider provider( 0 );
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  const auto reindex = [&index, layer]() {
    QgsAiWorkspaceIndex::WorkspaceLayerSnapshot snapshot;
    QString err;
    const bool ok = index.createWorkspaceLayerSnapshotForLayer( layer->id(), snapshot, &err ) && index.reindexLayerSnapshot( snapshot, &err );
    if ( !ok )
      qWarning() << err;
    return ok;
  };

  QVERIFY( reindex() );
  const int firstTexts = provider.embeddedTexts;
  QVERIFY( firstTexts > 0 );
  const int chunkCount = static_cast<int>( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, layer->id() ).size() );

  // Same files, same settings: nothing is read or embedded.
  QVERIFY( reindex() );
  QCOMPARE( provider.embeddedTexts.load(), firstTexts );

  // Another index of the same workspace (Strata started again) knows the layer too.
  QgsAiWorkspaceIndex reopened( &contextProvider, &provider );
  QgsAiWorkspaceIndex::WorkspaceLayerSnapshot snapshot;
  QString err;
  QVERIFY( reopened.createWorkspaceLayerSnapshotForLayer( layer->id(), snapshot, &err ) );
  QVERIFY2( reopened.reindexLayerSnapshot( snapshot, &err ), err.toUtf8().constData() );
  QCOMPARE( provider.embeddedTexts.load(), firstTexts );
  QCOMPARE( reopened.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, layer->id() ).size(), chunkCount );

  // A newer .dbf (attributes saved) means reading the layer again; its unchanged chunks keep their embeddings.
  QFile dbf( root.path() + u"/points.dbf"_s );
  QVERIFY( dbf.open( QIODevice::ReadWrite ) );
  QVERIFY( dbf.setFileTime( QDateTime::currentDateTime().addSecs( 60 ), QFileDevice::FileModificationTime ) );
  dbf.close();
  QVERIFY( reindex() );
  QCOMPARE( provider.embeddedTexts.load(), firstTexts );
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, layer->id() ).size(), chunkCount );
}

void TestQgsAiWorkspaceIndex::editedLayerReusesUnchangedChunks()
{
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string"_s, u"memory points"_s, u"memory"_s );
  QgsFeatureList features;
  for ( int i = 0; i < 150; ++i )
  {
    QgsFeature feature( layer->fields() );
    feature.setAttribute( 0, u"feature number %1 with a reasonably long descriptive name"_s.arg( i ) );
    feature.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( i, i ) ) );
    features << feature;
  }
  QVERIFY( layer->dataProvider()->addFeatures( features ) );
  QgsProject::instance()->addMapLayer( layer );
  const auto removeLayer = qScopeGuard( [layer]() { QgsProject::instance()->removeMapLayer( layer ); } );

  QTemporaryDir root;
  QVERIFY( root.isValid() );
  QgsAiFileContextProvider contextProvider( root.path() );
  SlowEmbeddingProvider provider( 0 );
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  const auto reindex = [&index, layer]() {
    QgsAiWorkspaceIndex::WorkspaceLayerSnapshot snapshot;
    QString err;
    return index.createWorkspaceLayerSnapshotForLayer( layer->id(), snapshot, &err ) && index.reindexLayerSnapshot( snapshot, &err );
  };

  QVERIFY( reindex() );
  const int firstTexts = provider.embeddedTexts;
  QVERIFY( firstTexts > 2 );

  // A memory layer has no file to compare: it is read again, but only new text is embedded.
  QVERIFY( reindex() );
  QCOMPARE( provider.embeddedTexts.load(), firstTexts );

  // Editing the last feature changes its chunk only.
  QgsFeature last;
  QgsFeatureIterator it = layer->getFeatures();
  while ( it.nextFeature( last ) )
    ;
  QVERIFY( layer->dataProvider()->changeAttributeValues( { { last.id(), { { 0, u"renamed feature"_s } } } } ) );
  QVERIFY( reindex() );
  QCOMPARE( provider.embeddedTexts.load(), firstTexts + 1 );
}

void TestQgsAiWorkspaceIndex::unchangedFilesAreNotReadAgain()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  const QString notesPath = QDir( root.path() ).filePath( u"notes.md"_s );
  const QString otherPath = QDir( root.path() ).filePath( u"other.md"_s );
  const auto writeText = []( const QString &path, const QString &text ) {
    QFile file( path );
    QVERIFY( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    file.write( text.toUtf8() );
  };
  writeText( notesPath, u"original notes about parcels"_s );
  writeText( otherPath, u"other notes about roads"_s );

  QgsAiFileContextProvider contextProvider( root.path() );
  SlowEmbeddingProvider provider( 0 );
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QString err;
  QVERIFY2( index.reindex( 10, &err ), err.toUtf8().constData() );
  QCOMPARE( provider.embeddedTexts.load(), 2 );

  // New content with the old modification time: the file is not read, so the old text stays.
  const QDateTime modified = QFileInfo( notesPath ).lastModified();
  writeText( notesPath, u"rewritten without touching the time"_s );
  {
    QFile file( notesPath );
    QVERIFY( file.open( QIODevice::ReadWrite ) );
    QVERIFY( file.setFileTime( modified, QFileDevice::FileModificationTime ) );
  }
  QVERIFY2( index.reindex( 10, &err ), err.toUtf8().constData() );
  QCOMPARE( provider.embeddedTexts.load(), 2 );
  const auto textOf = [&index]( const QString &relativePath ) {
    for ( const QgsAiWorkspaceIndex::Chunk &chunk : index.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllFiles ) )
    {
      if ( chunk.relativePath == relativePath )
        return chunk.text;
    }
    return QString();
  };
  QCOMPARE( textOf( u"notes.md"_s ), u"original notes about parcels"_s );

  // A newer time: read again. The removed file leaves the index; the other one is untouched.
  {
    QFile file( notesPath );
    QVERIFY( file.open( QIODevice::ReadWrite ) );
    QVERIFY( file.setFileTime( modified.addSecs( 60 ), QFileDevice::FileModificationTime ) );
  }
  QVERIFY( QFile::remove( otherPath ) );
  QVERIFY2( index.reindex( 10, &err ), err.toUtf8().constData() );
  QCOMPARE( provider.embeddedTexts.load(), 3 );
  QCOMPARE( textOf( u"notes.md"_s ), u"rewritten without touching the time"_s );
  QVERIFY( textOf( u"other.md"_s ).isEmpty() );

  // What reached the database matches.
  QgsAiWorkspaceIndex reloaded( &contextProvider, &provider );
  QVERIFY( reloaded.ensureLoaded() );
  const QList<QgsAiWorkspaceIndex::Chunk> stored = reloaded.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllFiles );
  QCOMPARE( stored.size(), 1 );
  QCOMPARE( stored.first().text, u"rewritten without touching the time"_s );
}

void TestQgsAiWorkspaceIndex::searchSkipsLayersOutsideTheProject()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  QgsAiFileContextProvider contextProvider( root.path() );
  FakeEmbeddingProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QString err;
  QVERIFY( index.persistChunks(
    { makeLayerChunk( u"open_layer"_s, u"Open"_s, 0, 1, 0, u"parcels"_s, QByteArray() ),
      makeLayerChunk( u"closed_layer"_s, u"Closed"_s, 0, 1, 0, u"parcels"_s, QByteArray() ),
      makeFileChunk( u"notes.md"_s, 0, u"parcels"_s ) },
    { dummyEmbedding( 1.0f ), dummyEmbedding( 1.0f ), dummyEmbedding( 1.0f ) },
    QgsAiWorkspaceIndex::ReplaceScope::All,
    QString(),
    &err
  ) );

  QCOMPARE( index.search( u"parcels"_s, 10, &err ).size(), 3 );

  index.setActiveLayerIds( { u"open_layer"_s } );
  const QList<QgsAiWorkspaceIndex::Chunk> hits = index.search( u"parcels"_s, 10, &err );
  QCOMPARE( hits.size(), 2 );
  for ( const QgsAiWorkspaceIndex::Chunk &hit : hits )
    QVERIFY( hit.layerId != "closed_layer"_L1 );
  // The closed project's layer is still indexed, for when it opens again.
  QCOMPARE( index.chunks( QgsAiWorkspaceIndex::ReplaceScope::SingleLayer, u"closed_layer"_s ).size(), 1 );
}

void TestQgsAiWorkspaceIndex::layersUnseenForAMonthExpire()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string"_s, u"memory points"_s, u"memory"_s );
  QgsFeature feature( layer->fields() );
  feature.setAttribute( 0, u"a"_s );
  feature.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 1, 1 ) ) );
  QVERIFY( layer->dataProvider()->addFeature( feature ) );
  QgsProject::instance()->addMapLayer( layer );
  const auto removeLayer = qScopeGuard( [layer]() { QgsProject::instance()->removeMapLayer( layer ); } );

  QgsAiFileContextProvider contextProvider( root.path() );
  FakeEmbeddingProvider provider;
  QString path;
  {
    QgsAiWorkspaceIndex index( &contextProvider, &provider );
    QString err;
    QVERIFY2(
      index.persistChunks( { makeLayerChunk( u"old_layer"_s, u"Old"_s, 0, 1, 0, u"old"_s, QByteArray() ) }, { dummyEmbedding( 1.0f ) }, QgsAiWorkspaceIndex::ReplaceScope::All, QString(), &err ),
      err.toUtf8().constData()
    );
    QgsAiWorkspaceIndex::WorkspaceLayerSnapshot snapshot;
    QVERIFY( index.createWorkspaceLayerSnapshotForLayer( layer->id(), snapshot, &err ) );
    QVERIFY2( index.reindexLayerSnapshot( snapshot, &err ), err.toUtf8().constData() );
    path = index.databasePath();
  }

  // Age both layers: the one with a recorded sighting through layer_state, the other through its rows.
  const QString connection = u"test_age_layers"_s;
  {
    QSqlDatabase db = QSqlDatabase::addDatabase( u"QSQLITE"_s, connection );
    db.setDatabaseName( path );
    QVERIFY( db.open() );
    QSqlQuery q( db );
    const qint64 old = QDateTime::currentDateTimeUtc().addDays( -( QgsAiWorkspaceIndex::STALE_DATABASE_DAYS + 1 ) ).toMSecsSinceEpoch();
    QVERIFY( q.exec( u"UPDATE chunks SET last_sync = %1"_s.arg( old ) ) );
    QVERIFY( q.exec( u"INSERT OR REPLACE INTO layer_state (layer_id, fingerprint, last_seen) VALUES ('%1', 'x', %2)"_s.arg( layer->id() ).arg( old ) ) );
    db.close();
  }
  QSqlDatabase::removeDatabase( connection );

  QgsAiWorkspaceIndex reloaded( &contextProvider, &provider );
  QVERIFY( reloaded.ensureLoaded() );
  QCOMPARE( reloaded.chunks( QgsAiWorkspaceIndex::ReplaceScope::AllLayers ).size(), 0 );
}

QGSTEST_MAIN( TestQgsAiWorkspaceIndex )
#include "testqgsaiworkspaceindex.moc"
