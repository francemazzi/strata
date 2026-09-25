/***************************************************************************
    qgsaiworkspaceindex.cpp
    ---------------------
    begin                : April 2026
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

#include "qgsaiworkspaceindex.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "ai/qgsaisecretstore.h"
#include "ai/tools/qgsaitaskrunner.h"
#include "qgsaiembeddingprovider.h"
#include "qgsaifilecontextprovider.h"
#include "qgsaifilesummarizer.h"
#include "qgsaiindexingthrottle.h"
#include "qgsailayerchunker.h"
#include "qgsapplication.h"
#include "qgsfeedback.h"
#include "qgsmaplayer.h"
#include "qgsmessagelog.h"
#include "qgsproject.h"
#include "qgsrasterlayer.h"
#include "qgssettings.h"
#include "qgstaskmanager.h"
#include "qgsvectorlayer.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QScopeGuard>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QUuid>
#include <QVariant>

#include "moc_qgsaiworkspaceindex.cpp"

using namespace Qt::StringLiterals;

namespace
{
  struct ProviderInfo
  {
      QString providerId;
      QString modelId;
      QString modelRevision;
      int embeddingDimension = 0;
  };

  const QStringList TEXT_EXTENSIONS = {
    u"h"_s,  u"hpp"_s,  u"cpp"_s, u"cc"_s,   u"c"_s,   u"py"_s,  u"md"_s,   u"txt"_s, u"json"_s, u"geojson"_s, u"csv"_s, u"tsv"_s,   u"qgs"_s,  u"qml"_s, u"xml"_s, u"ts"_s,
    u"js"_s, u"html"_s, u"css"_s, u"yaml"_s, u"yml"_s, u"ini"_s, u"toml"_s, u"cfg"_s, u"sh"_s,   u"bat"_s,     u"ps1"_s, u"cmake"_s, u"make"_s, u"mk"_s,  u"sql"_s, u"rst"_s,
  };

  QByteArray vectorToBytes( const QVector<float> &v )
  {
    QByteArray bytes;
    bytes.resize( static_cast<int>( v.size() * sizeof( float ) ) );
    if ( !v.isEmpty() )
      std::memcpy( bytes.data(), v.constData(), bytes.size() );
    return bytes;
  }

  QVector<float> bytesToVector( const QByteArray &bytes )
  {
    QVector<float> v;
    if ( bytes.size() % static_cast<int>( sizeof( float ) ) != 0 )
      return v;
    const int count = bytes.size() / static_cast<int>( sizeof( float ) );
    v.resize( count );
    if ( count > 0 )
      std::memcpy( v.data(), bytes.constData(), bytes.size() );
    return v;
  }

  //! Why workspace content may not go to \a provider, or an empty string if it may.
  QString indexRemoteConsentMissing( const QgsAiEmbeddingProvider *provider )
  {
    if ( !provider || !provider->isRemote() || QgsAiEmbeddingProviderRegistry::remoteEmbeddingConsented( provider->providerId() ) )
      return QString();
    return u"Indexing with %1 sends file text, layer attributes and coordinates to that service. Agree to it in the AI settings (Indexing) first."_s.arg( provider->displayName() );
  }

  bool ensureEmbeddingProviderAvailable( QgsAiEmbeddingProvider *provider, QString *errorMessage )
  {
    // Nothing leaves the computer for a remote embedding service without the user's consent.
    if ( const QString consentMissing = indexRemoteConsentMissing( provider ); !consentMissing.isEmpty() )
    {
      if ( errorMessage )
        *errorMessage = consentMissing;
      return false;
    }
    QString providerError;
    if ( provider && provider->isAvailable( &providerError ) )
      return true;

    if ( errorMessage )
      *errorMessage = providerError.isEmpty() ? u"Local embedding model is not installed."_s : providerError;
    return false;
  }

  QString storageProviderId( QgsAiEmbeddingProvider *provider )
  {
    return provider ? provider->providerId() : u"test"_s;
  }

  QString storageModelId( QgsAiEmbeddingProvider *provider )
  {
    return provider ? provider->modelId() : u"test"_s;
  }

  QString storageModelRevision( QgsAiEmbeddingProvider *provider )
  {
    if ( !provider )
      return u"test"_s;
    // A provider may report a null/empty revision (the base default returns a
    // null QString). The model_revision column is NOT NULL, so coalesce to a
    // non-null empty string to bind '' rather than SQL NULL.
    const QString revision = provider->modelRevision();
    return revision.isNull() ? u""_s : revision;
  }

  ProviderInfo providerInfo( QgsAiEmbeddingProvider *provider )
  {
    ProviderInfo info;
    info.providerId = storageProviderId( provider );
    info.modelId = storageModelId( provider );
    info.modelRevision = storageModelRevision( provider );
    info.embeddingDimension = provider ? provider->embeddingDimension() : 0;
    return info;
  }

  int storageDimension( const ProviderInfo &provider, const QVector<float> &embedding )
  {
    return provider.embeddingDimension > 0 ? provider.embeddingDimension : embedding.size();
  }

  bool metadataMatchesProvider( const QString &providerId, const QString &modelId, const QString &modelRevision, int dimension, const ProviderInfo &provider )
  {
    if ( provider.providerId.isEmpty() )
      return true;
    if ( providerId != provider.providerId )
      return false;
    if ( modelId != provider.modelId )
      return false;
    if ( modelRevision != provider.modelRevision )
      return false;
    return provider.embeddingDimension <= 0 || dimension == provider.embeddingDimension;
  }

  bool bindEncryptedIndexValue( QSqlQuery &query, const QString &plain, bool encryptionAvailable, bool requireEncryption, QString *errorMessage )
  {
    if ( !encryptionAvailable )
    {
      query.addBindValue( plain );
      return true;
    }

    const QgsAiSecretStore::EncryptionResult encrypted = QgsAiSecretStore::tryEncryptValue( plain );
    if ( encrypted.ok )
    {
      query.addBindValue( encrypted.value );
      return true;
    }

    if ( requireEncryption )
    {
      if ( errorMessage )
        *errorMessage = encrypted.errorMessage.isEmpty() ? u"Workspace index encryption failed."_s : encrypted.errorMessage;
      return false;
    }

    QgsAiSecretStore::warnPlaintextStorageOnce();
    query.addBindValue( plain );
    return true;
  }

  bool bindEncryptedIndexBlob( QSqlQuery &query, const QByteArray &blob, bool encryptionAvailable, bool requireEncryption, QString *errorMessage )
  {
    if ( !encryptionAvailable )
    {
      query.addBindValue( blob );
      return true;
    }

    const QgsAiSecretStore::BlobEncryptionResult encrypted = QgsAiSecretStore::tryEncryptBlob( blob );
    if ( encrypted.ok )
    {
      query.addBindValue( encrypted.value );
      return true;
    }

    if ( requireEncryption )
    {
      if ( errorMessage )
        *errorMessage = encrypted.errorMessage.isEmpty() ? u"Workspace index encryption failed."_s : encrypted.errorMessage;
      return false;
    }

    QgsAiSecretStore::warnPlaintextStorageOnce();
    query.addBindValue( blob );
    return true;
  }

  QString textHash( const QString &text, const QByteArray &extra = QByteArray() )
  {
    QCryptographicHash hash( QCryptographicHash::Sha1 );
    hash.addData( text.toUtf8() );
    if ( !extra.isEmpty() )
      hash.addData( extra );
    return QString::fromLatin1( hash.result().toHex() );
  }

  QString chunkReuseKey( const QString &sourceType, const QString &relativePath, const QString &layerId, int chunkIndex )
  {
    return sourceType + QChar( 0x1f ) + relativePath + QChar( 0x1f ) + layerId + QChar( 0x1f ) + QString::number( chunkIndex );
  }

  QString readSnapshotTextFile( const QString &absolutePath, int maxBytes )
  {
    QFile file( absolutePath );
    if ( !file.open( QIODevice::ReadOnly ) )
      return QString();

    QByteArray content = file.read( std::max( 0, maxBytes ) + 1 );
    if ( content.size() > maxBytes )
      content.truncate( maxBytes );
    if ( content.contains( '\0' ) )
      return QString();
    return QString::fromUtf8( content );
  }

  QString indexProviderSlug( const QString &providerId )
  {
    // Separate index files per embedding provider so switching providers (for example
    // local <-> remote) keeps each index intact instead of rebuilding every time.
    QString slug;
    for ( const QChar ch : providerId.toLower() )
      slug.append( ch.isLetterOrNumber() ? ch : QChar( u'_' ) );
    return slug.isEmpty() ? u"none"_s : slug;
  }

  //! Provider id that names the database file (no provider: "none", as before).
  QString indexDatabaseProviderId( QgsAiEmbeddingProvider *provider )
  {
    return provider ? provider->providerId() : QString();
  }

  QString indexCanceledMessage()
  {
    return u"Indexing was canceled."_s;
  }

  /**
   * Embeds \a texts one EMBEDDING_BATCH at a time, holding \a providerUseMutex only for the
   * batch being computed, so a search can run between two batches of a long reindex.
   */
  bool indexEmbedInBatches(
    QMutex &providerUseMutex,
    const std::atomic_int &searchesWaiting,
    QgsAiEmbeddingProvider *provider,
    const QStringList &texts,
    QgsAiEmbeddingRole role,
    QList<QVector<float>> &out,
    QString *errorMessage,
    QgsFeedback *feedback,
    const std::function<void( double percent )> &progress = {}
  )
  {
    out.clear();
    out.reserve( texts.size() );
    // Automatic indexing leaves the computer to the user: pauses between batches, and waits
    // while the map is in use or the computer runs on battery.
    const bool throttled = QgsAiIndexingThrottle::inBackgroundIndexing();
    const QgsAiIndexingThrottle::Speed speed = throttled ? QgsAiIndexingThrottle::speed() : QgsAiIndexingThrottle::Speed::High;
    qint64 lastBatchMs = 0;
    for ( int start = 0; start < texts.size(); start += QgsAiWorkspaceIndex::EMBEDDING_BATCH )
    {
      if ( throttled && !QgsAiIndexingThrottle::waitBeforeNextBatch( start > 0 ? QgsAiIndexingThrottle::pauseAfterBatchMs( speed, lastBatchMs ) : 0, feedback ) )
      {
        if ( errorMessage )
          *errorMessage = indexCanceledMessage();
        return false;
      }
      if ( feedback && feedback->isCanceled() )
      {
        if ( errorMessage )
          *errorMessage = indexCanceledMessage();
        return false;
      }
      const QStringList batch = texts.mid( start, QgsAiWorkspaceIndex::EMBEDDING_BATCH );
      QgsAiEmbeddingOptions options;
      options.maxBatch = static_cast<int>( batch.size() );
      options.feedback = feedback;
      QList<QVector<float>> vectors;
      QElapsedTimer batchTimer;
      batchTimer.start();
      {
        const QMutexLocker locker( &providerUseMutex );
        if ( !provider->embed( batch, role, vectors, errorMessage, options ) )
          return false;
      }
      lastBatchMs = batchTimer.elapsed();
      // QMutex is not fair: relocking right away would starve a search waiting for this batch.
      for ( int waitedMs = 0; searchesWaiting.load() > 0 && waitedMs < QgsAiWorkspaceIndex::SEARCH_LOCK_TIMEOUT_MS; ++waitedMs )
        QThread::msleep( 1 );
      if ( vectors.size() != batch.size() )
      {
        if ( errorMessage )
          *errorMessage = u"Embedding count mismatch: expected %1 got %2"_s.arg( batch.size() ).arg( vectors.size() );
        return false;
      }
      out.append( vectors );
      if ( progress )
        progress( 100.0 * static_cast<double>( out.size() ) / static_cast<double>( texts.size() ) );
    }
    return true;
  }

  /**
   * Opens \a path on the connection named \a connection (created if needed) in write-ahead log
   * mode: readers never wait for a writer and a commit costs one fsync instead of three.
   */
  QSqlDatabase indexOpenDatabase( const QString &connection, const QString &path, QString *errorMessage )
  {
    QSqlDatabase db = QSqlDatabase::contains( connection ) ? QSqlDatabase::database( connection, false ) : QSqlDatabase::addDatabase( u"QSQLITE"_s, connection );
    if ( db.isOpen() && db.databaseName() == path )
      return db;
    db.close();
    db.setDatabaseName( path );
    db.setConnectOptions( u"QSQLITE_BUSY_TIMEOUT=5000"_s );
    if ( !db.open() )
    {
      if ( errorMessage )
        *errorMessage = db.lastError().text();
      return db;
    }
    QSqlQuery pragma( db );
    pragma.exec( u"PRAGMA journal_mode=WAL"_s );
    pragma.exec( u"PRAGMA synchronous=NORMAL"_s );
    return db;
  }

  //! Layers stay indexed this long after they were last seen in a project.
  constexpr qint64 INDEX_LAYER_RETENTION_MS = static_cast<qint64>( QgsAiWorkspaceIndex::STALE_DATABASE_DAYS ) * 24 * 3600 * 1000;

  //! Fingerprint and last sighting of each indexed layer, so an unchanged layer is not read again.
  bool indexEnsureLayerStateTable( QSqlDatabase &db )
  {
    QSqlQuery q( db );
    return q.exec( u"CREATE TABLE IF NOT EXISTS layer_state (layer_id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, last_seen INTEGER NOT NULL)"_s );
  }

  //! Deletes the database file at \a path together with its write-ahead log files.
  void indexRemoveDatabaseFiles( const QString &path )
  {
    for ( const QString &suffix : { QString(), u"-wal"_s, u"-shm"_s, u"-journal"_s } )
      QFile::remove( path + suffix );
  }

  /**
   * How chunks are sized for \a provider: its token counter and the tokens a chunk may hold, or no
   * counter when the provider has no input limit or cannot count (chunks are then sized in characters).
   */
  std::pair<QgsAiWorkspaceIndex::TokenCounter, int> indexTokenBudget( QgsAiEmbeddingProvider *provider )
  {
    if ( !provider || provider->maxInputTokens() <= QgsAiWorkspaceIndex::CHUNK_TOKEN_MARGIN || provider->tokenCount( u"probe"_s ) < 0 )
      return { {}, 0 };
    return { [provider]( const QString &text ) { return provider->tokenCount( text ); }, provider->maxInputTokens() - QgsAiWorkspaceIndex::CHUNK_TOKEN_MARGIN };
  }

  //! Serial pool for index database work that must not block the interface thread.
  QThreadPool *indexDatabaseWorkPool()
  {
    static QThreadPool *pool = []() {
      auto *p = new QThreadPool();
      p->setMaxThreadCount( 1 );
      return p;
    }();
    return pool;
  }

  //! Loads the index cache in the background for QgsAiWorkspaceIndex::requestLoad().
  class QgsAiIndexLoadTask final : public QgsTask
  {
    public:
      explicit QgsAiIndexLoadTask( QgsAiWorkspaceIndex *index )
        : QgsTask( QObject::tr( "Load AI workspace index" ), QgsTask::CanCancel | QgsTask::CancelWithoutPrompt | QgsTask::Silent | QgsTask::Hidden )
        , mIndex( index )
      {}

    protected:
      bool run() override
      {
        if ( !mIndex )
          return false;
        mIndex->ensureLoaded();
        mIndex->closeDatabaseConnectionForCurrentThread();
        return true;
      }

    private:
      QPointer<QgsAiWorkspaceIndex> mIndex;
  };
} //namespace

QgsAiWorkspaceIndex::QgsAiWorkspaceIndex( QgsAiFileContextProvider *contextProvider, QgsAiEmbeddingProvider *embeddingProvider, QObject *parent )
  : QObject( parent )
  , mContextProvider( contextProvider )
  , mEmbeddingProvider( embeddingProvider, []( QgsAiEmbeddingProvider * ) {} )
{
  if ( mContextProvider )
  {
    mCurrentRoot = mContextProvider->workspaceRoot();
    connect( mContextProvider, &QgsAiFileContextProvider::workspaceRootChanged, this, &QgsAiWorkspaceIndex::onWorkspaceRootChanged );
  }
  updateStatusSnapshot();

  // A loaded local model holds about 300 MB: give it back when indexing and search are idle.
  const int idleSeconds = QgsSettings().value( u"strata/index/model_idle_unload_s"_s, DEFAULT_MODEL_IDLE_UNLOAD_S ).toInt();
  if ( idleSeconds > 0 )
  {
    connect( &mIdleReleaseTimer, &QTimer::timeout, this, &QgsAiWorkspaceIndex::releaseIdleEmbeddingModel );
    mIdleReleaseTimer.start( std::clamp( idleSeconds * 250, 1000, 30000 ) );
  }
}

void QgsAiWorkspaceIndex::releaseIdleEmbeddingModel()
{
  const int idleSeconds = QgsSettings().value( u"strata/index/model_idle_unload_s"_s, DEFAULT_MODEL_IDLE_UNLOAD_S ).toInt();
  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  if ( idleSeconds <= 0 || !provider )
    return;
  // Freeing a model takes a moment: never on the interface thread.
  indexDatabaseWorkPool()->start( [provider, idleMs = static_cast<qint64>( idleSeconds ) * 1000]() { provider->releaseIdleResources( idleMs ); } );
}

QgsAiWorkspaceIndex::~QgsAiWorkspaceIndex()
{
  if ( mLoadTask )
  {
    mLoadTask->cancel();
    mLoadTask->waitForFinished( 5000 );
  }
  indexDatabaseWorkPool()->waitForDone( 5000 );
  closeDatabaseConnectionForCurrentThread();
}

void QgsAiWorkspaceIndex::closeDatabaseConnectionForCurrentThread() const
{
  const QString name = connectionName();
  if ( QSqlDatabase::contains( name ) )
  {
    {
      QSqlDatabase db = QSqlDatabase::database( name );
      db.close();
    }
    QSqlDatabase::removeDatabase( name );
  }
}

QString QgsAiWorkspaceIndex::connectionName() const
{
  // One named SQL connection per index instance and thread — avoid clashing with other
  // QGIS components that already use unnamed default connections.
  return u"qgsai_index_%1_%2"_s.arg( reinterpret_cast<quintptr>( this ) ).arg( reinterpret_cast<quintptr>( QThread::currentThreadId() ) );
}

std::shared_ptr<QgsAiEmbeddingProvider> QgsAiWorkspaceIndex::providerSnapshot() const
{
  const QMutexLocker locker( &mProviderPointerMutex );
  return mEmbeddingProvider;
}

QString QgsAiWorkspaceIndex::workspaceRoot() const
{
  const QMutexLocker locker( &mRootMutex );
  return mCurrentRoot;
}

QString QgsAiWorkspaceIndex::dbPath() const
{
  return dbPathForRoot( workspaceRoot() );
}

QString QgsAiWorkspaceIndex::dbPathForRoot( const QString &workspaceRoot ) const
{
  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  return dbPathForRoot( workspaceRoot, indexDatabaseProviderId( provider.get() ) );
}

QString QgsAiWorkspaceIndex::dbPathForRoot( const QString &workspaceRoot, const QString &providerId )
{
  const QString root = workspaceRoot.trimmed().isEmpty() ? QString() : QDir( workspaceRoot ).absolutePath();
  if ( root.isEmpty() )
    return QString();

  const QByteArray hash = QCryptographicHash::hash( root.toUtf8(), QCryptographicHash::Sha1 ).toHex().left( 16 );
  const QString dir = QgsApplication::qgisSettingsDirPath() + u"ai_index"_s;
  QDir().mkpath( dir );
  return QDir( dir ).filePath( u"ws_%1_%2.sqlite"_s.arg( QString::fromLatin1( hash ), indexProviderSlug( providerId ) ) );
}

bool QgsAiWorkspaceIndex::isTextFile( const QString &relativePath )
{
  const QString ext = QFileInfo( relativePath ).suffix().toLower();
  if ( ext.isEmpty() )
    return false;
  return TEXT_EXTENSIONS.contains( ext );
}

bool QgsAiWorkspaceIndex::hasEmbeddingConfiguration() const
{
  return embeddingProviderAvailable();
}

void QgsAiWorkspaceIndex::setEmbeddingProvider( QgsAiEmbeddingProvider *embeddingProvider )
{
  // Not owned: the caller keeps it alive.
  setEmbeddingProvider( std::shared_ptr<QgsAiEmbeddingProvider>( embeddingProvider, []( QgsAiEmbeddingProvider * ) {} ) );
}

void QgsAiWorkspaceIndex::setEmbeddingProvider( std::shared_ptr<QgsAiEmbeddingProvider> embeddingProvider )
{
  const QgsAiPerfScope perf( u"index"_s, u"set_provider"_s );
  {
    // Tasks already running keep their own reference to the previous provider.
    const QMutexLocker locker( &mProviderPointerMutex );
    if ( mEmbeddingProvider.get() == embeddingProvider.get() )
      return;
    mEmbeddingProvider = std::move( embeddingProvider );
  }

  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    mCache.clear();
    mLayerFingerprints.clear();
    mCacheRoot.clear();
    mLastSync = QDateTime();
    mLoaded = false;
    updateStatusSnapshot();
  }
  requestLoad();
}

bool QgsAiWorkspaceIndex::embeddingProviderAvailable() const
{
  // Cheap and never blocking: timers and the interface thread call this. Providers must
  // answer without loading models or waiting on the network (see QgsAiE5EmbeddingProvider).
  // A remote provider the user did not agree to send content to counts as unavailable.
  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  const QgsAiPerfScope perf( u"index"_s, u"provider_available"_s, 5 );
  QString ignored;
  return provider && indexRemoteConsentMissing( provider.get() ).isEmpty() && provider->isAvailable( &ignored );
}

QString QgsAiWorkspaceIndex::unavailableReason() const
{
  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  if ( !provider )
    return tr( "No embedding provider is configured." );
  if ( const QString consentMissing = indexRemoteConsentMissing( provider.get() ); !consentMissing.isEmpty() )
    return consentMissing;
  QString error;
  if ( !provider->isAvailable( &error ) )
    return error.isEmpty() ? tr( "The embedding provider is not available." ) : error;
  return QString();
}

void QgsAiWorkspaceIndex::onWorkspaceRootChanged()
{
  const QString root = mContextProvider ? mContextProvider->workspaceRoot() : QString();
  {
    const QMutexLocker locker( &mRootMutex );
    mCurrentRoot = root;
  }
  closeDatabaseConnectionForCurrentThread();
  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    mCache.clear();
    mLayerFingerprints.clear();
    mCacheRoot.clear();
    mLastSync = QDateTime();
    mLoaded = false;
    updateStatusSnapshot();
  }
  // The new workspace's cache loads in the background: opening a project never waits on it.
  requestLoad();
}

void QgsAiWorkspaceIndex::requestLoad()
{
  // Once per session, drop the indexes of workspaces not opened for a month.
  static bool sStaleDatabasesChecked = false;
  if ( !std::exchange( sStaleDatabasesChecked, true ) )
  {
    const QString directory = QgsApplication::qgisSettingsDirPath() + u"ai_index"_s;
    indexDatabaseWorkPool()->start( [directory, keep = dbPath()]() {
      if ( const int removed = removeStaleDatabases( directory, keep ) )
        QgsMessageLog::logMessage( u"Removed %1 AI index databases unused for %2 days."_s.arg( removed ).arg( STALE_DATABASE_DAYS ), u"AI/Index"_s, Qgis::MessageLevel::Info, false );
    } );
  }
  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    if ( mLoaded )
      return;
  }
  if ( mLoadTask && mLoadTask->isActive() )
    return;
  QgsTaskManager *manager = QgsApplication::taskManager();
  if ( !manager )
    return;
  auto *task = new QgsAiIndexLoadTask( this );
  mLoadTask = task;
  connect( task, &QgsTask::taskCompleted, this, [this]() {
    updateStatusSnapshot();
    emit loaded();
  } );
  manager->addTask( task );
}

QStringList QgsAiWorkspaceIndex::chunkText( const QString &content )
{
  QStringList chunks;
  if ( content.isEmpty() )
    return chunks;

  // Greedy chunker: walk forward CHUNK_TARGET_CHARS at a time, snap the
  // boundary back to the previous newline if there is one within the last 30%
  // of the chunk so we don't split mid-line.
  int pos = 0;
  while ( pos < content.size() )
  {
    int end = std::min( pos + CHUNK_TARGET_CHARS, static_cast<int>( content.size() ) );
    if ( end < content.size() )
    {
      const int searchFrom = end - CHUNK_TARGET_CHARS / 3;
      const int newline = content.lastIndexOf( '\n', end );
      if ( newline > pos && newline >= searchFrom )
        end = newline + 1;
    }
    QString slice = content.mid( pos, end - pos ).trimmed();
    if ( !slice.isEmpty() )
      chunks.append( slice );
    pos = end;
  }
  return chunks;
}

QString QgsAiWorkspaceIndex::truncateToTokens( const QString &text, const TokenCounter &tokenCount, int maxTokens )
{
  if ( !tokenCount || maxTokens <= 0 || tokenCount( text ) <= maxTokens )
    return text;
  // Binary search on the length: tokens grow with characters.
  int low = 0;
  int high = static_cast<int>( text.size() );
  while ( low < high )
  {
    const int middle = ( low + high + 1 ) / 2;
    if ( tokenCount( text.left( middle ) ) <= maxTokens )
      low = middle;
    else
      high = middle - 1;
  }
  return text.left( low );
}

QStringList QgsAiWorkspaceIndex::chunkTextByTokens( const QString &content, const TokenCounter &tokenCount, int maxTokens )
{
  if ( !tokenCount || maxTokens <= 0 )
    return chunkText( content );

  QStringList chunks;
  QString current;
  int currentTokens = 0;
  const auto flush = [&]() {
    const QString chunk = current.trimmed();
    if ( !chunk.isEmpty() )
      chunks.append( chunk );
    current.clear();
    currentTokens = 0;
  };

  const QStringList lines = content.split( '\n' );
  for ( const QString &line : lines )
  {
    const int count = tokenCount( line );
    const int lineTokens = ( count < 0 ? static_cast<int>( line.size() ) : count ) + 1;
    if ( lineTokens > maxTokens )
    {
      // A line longer than a chunk becomes pieces of its own.
      flush();
      QString rest = line;
      while ( !rest.trimmed().isEmpty() )
      {
        QString piece = truncateToTokens( rest, tokenCount, maxTokens - 1 );
        if ( piece.isEmpty() )
          piece = rest.left( 1 );
        chunks.append( piece.trimmed() );
        rest = rest.mid( piece.size() );
      }
      continue;
    }
    if ( currentTokens + lineTokens > maxTokens )
      flush();
    current += line;
    current += '\n';
    currentTokens += lineTokens;
  }
  flush();
  chunks.removeAll( QString() );
  return chunks;
}

float QgsAiWorkspaceIndex::cosineSimilarity( const QVector<float> &a, const QVector<float> &b )
{
  if ( a.size() != b.size() || a.isEmpty() )
    return 0.0f;

  double dot = 0.0;
  double na = 0.0;
  double nb = 0.0;
  for ( int i = 0; i < a.size(); ++i )
  {
    dot += static_cast<double>( a[i] ) * b[i];
    na += static_cast<double>( a[i] ) * a[i];
    nb += static_cast<double>( b[i] ) * b[i];
  }
  if ( na == 0.0 || nb == 0.0 )
    return 0.0f;
  return static_cast<float>( dot / ( std::sqrt( na ) * std::sqrt( nb ) ) );
}

bool QgsAiWorkspaceIndex::ensureLoaded()
{
  // Serializes loads without holding mMutex during the database read, so the interface
  // thread, which only takes mMutex for short cache updates, never waits on it.
  const QMutexLocker loadLocker( &mLoadMutex );
  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    if ( mLoaded )
      return true;
  }
  QString err;
  if ( !loadAll( &err ) )
  {
    QgsMessageLog::logMessage( u"Workspace index load failed: %1"_s.arg( err ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );
    // Treat missing/corrupt DB as empty rather than fatal.
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    mCache.clear();
    mLayerFingerprints.clear();
    mCacheRoot = workspaceRoot();
    mLastSync = QDateTime();
    mLoaded = true;
    updateStatusSnapshot();
  }
  return true;
}

void QgsAiWorkspaceIndex::updateStatusSnapshot()
{
  const QMutexLocker<QRecursiveMutex> locker( &mMutex );
  Status s;
  s.chunkCount = static_cast<int>( mCache.size() );
  QSet<QString> uniqueFiles;
  for ( const CachedChunk &c : std::as_const( mCache ) )
  {
    if ( c.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_LAYER ) )
      ++s.layerChunkCount;
    else
    {
      ++s.fileChunkCount;
      uniqueFiles.insert( c.chunk.relativePath );
    }
  }
  s.fileCount = static_cast<int>( uniqueFiles.size() );
  s.lastSync = mLastSync;
  s.indexed = !mCache.isEmpty();
  s.loading = !mLoaded;

  const QMutexLocker statusLocker( &mStatusMutex );
  mStatus = s;
}

QgsAiWorkspaceIndex::Status QgsAiWorkspaceIndex::status() const
{
  Status s;
  {
    const QMutexLocker locker( &mStatusMutex );
    s = mStatus;
  }
  s.workspaceRoot = workspaceRoot();
  if ( const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot() )
  {
    s.embeddingProviderId = provider->providerId();
    s.embeddingModelId = provider->modelId();
  }
  return s;
}

bool QgsAiWorkspaceIndex::cacheHoldsRoot( const QString &workspaceRoot ) const
{
  return QDir::cleanPath( mCacheRoot ) == QDir::cleanPath( workspaceRoot );
}

QList<QgsAiWorkspaceIndex::Chunk> QgsAiWorkspaceIndex::chunks( ReplaceScope scope, const QString &layerId ) const
{
  const QMutexLocker<QRecursiveMutex> locker( &mMutex );
  QList<Chunk> out;
  for ( const CachedChunk &c : mCache )
  {
    const bool isLayer = c.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_LAYER );
    switch ( scope )
    {
      case ReplaceScope::All:
        out.append( c.chunk );
        break;
      case ReplaceScope::AllFiles:
        if ( !isLayer )
          out.append( c.chunk );
        break;
      case ReplaceScope::AllLayers:
        if ( isLayer )
          out.append( c.chunk );
        break;
      case ReplaceScope::SingleLayer:
        if ( isLayer && c.chunk.layerId == layerId )
          out.append( c.chunk );
        break;
    }
  }
  return out;
}

bool QgsAiWorkspaceIndex::loadAll( QString *errorMessage )
{
  const QgsAiPerfScope perf( u"index"_s, u"load_cache"_s );
  // A pending background removal must be on disk before we read it back.
  indexDatabaseWorkPool()->waitForDone();

  const QString root = workspaceRoot();
  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  const ProviderInfo activeProvider = providerInfo( provider.get() );
  const QString path = dbPathForRoot( root, indexDatabaseProviderId( provider.get() ) );

  QList<CachedChunk> loadedChunks;
  QHash<QString, QString> loadedFingerprints;
  qint64 maxSync = 0;
  const auto commit = [&]() {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    if ( QDir::cleanPath( root ) != QDir::cleanPath( workspaceRoot() ) )
      return; // The workspace changed while loading: the next load reads the new one.
    mCache = loadedChunks;
    mLayerFingerprints = loadedFingerprints;
    mCacheRoot = root;
    mLastSync = maxSync > 0 ? QDateTime::fromMSecsSinceEpoch( maxSync ) : QDateTime();
    mLoaded = true;
    updateStatusSnapshot();
  };

  if ( path.isEmpty() || !QFileInfo::exists( path ) )
  {
    commit(); // Nothing to load yet — that's not an error.
    return true;
  }

  QSqlDatabase db = indexOpenDatabase( connectionName(), path, errorMessage );
  if ( !db.isOpen() )
    return false;

  // Schema migration: read PRAGMA user_version. If older than SCHEMA_VERSION,
  // drop the chunks table — callers will rebuild it on the next reindex.
  {
    QSqlQuery v( db );
    int currentVersion = 0;
    if ( v.exec( u"PRAGMA user_version"_s ) && v.next() )
      currentVersion = v.value( 0 ).toInt();
    if ( currentVersion < SCHEMA_VERSION )
    {
      QgsMessageLog::logMessage( u"Workspace index schema upgrade: %1 → %2 (dropping old chunks)"_s.arg( currentVersion ).arg( SCHEMA_VERSION ), u"AI/Index"_s, Qgis::MessageLevel::Info, false );
      QSqlQuery drop( db );
      drop.exec( u"DROP TABLE IF EXISTS chunks"_s );
      drop.exec( u"PRAGMA user_version = %1"_s.arg( SCHEMA_VERSION ) );
      drop.exec( u"VACUUM"_s );
      commit(); // Empty — caller will reindex.
      return true;
    }
  }

  {
    QSqlQuery tableCheck( db );
    if ( tableCheck.exec( u"SELECT name FROM sqlite_master WHERE type='table' AND name='chunks'"_s ) && !tableCheck.next() )
    {
      commit();
      return true;
    }
  }

  {
    QSqlQuery metadata( db );
    if ( metadata.exec( u"SELECT provider_id, model_id, model_revision, embedding_dimension FROM chunks LIMIT 1"_s ) && metadata.next() )
    {
      const QString providerId = metadata.value( 0 ).toString();
      const QString modelId = metadata.value( 1 ).toString();
      const QString modelRevision = metadata.value( 2 ).toString();
      const int dimension = metadata.value( 3 ).toInt();
      if ( provider && !metadataMatchesProvider( providerId, modelId, modelRevision, dimension, activeProvider ) )
      {
        QgsMessageLog::
          logMessage( u"Workspace index provider changed (%1/%2 -> %3/%4); dropping old chunks"_s.arg( providerId, modelId, activeProvider.providerId, activeProvider.modelId ), u"AI/Index"_s, Qgis::MessageLevel::Info, false );
        QSqlQuery drop( db );
        drop.exec( u"DROP TABLE IF EXISTS chunks"_s );
        drop.exec( u"PRAGMA user_version = %1"_s.arg( SCHEMA_VERSION ) );
        drop.exec( u"VACUUM"_s );
        commit();
        return true;
      }
    }
  }

  // Layers stay indexed after their project closes, so reopening it costs nothing; those not
  // seen in any project for a month are dropped.
  indexEnsureLayerStateTable( db );
  {
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - INDEX_LAYER_RETENTION_MS;
    QSqlQuery expire( db );
    db.transaction();
    expire.prepare(
      u"DELETE FROM chunks WHERE source_type = 'layer' AND (layer_id IN (SELECT layer_id FROM layer_state WHERE last_seen < ?) OR (last_sync < ? AND layer_id NOT IN (SELECT layer_id FROM layer_state)))"_s
    );
    expire.addBindValue( cutoff );
    expire.addBindValue( cutoff );
    expire.exec();
    const int expiredRows = expire.numRowsAffected();
    QSqlQuery expireState( db );
    expireState.prepare( u"DELETE FROM layer_state WHERE last_seen < ?"_s );
    expireState.addBindValue( cutoff );
    expireState.exec();
    db.commit();
    if ( expiredRows > 0 )
      QgsMessageLog::logMessage( u"Workspace index: dropped %1 chunks of layers not seen for %2 days."_s.arg( expiredRows ).arg( STALE_DATABASE_DAYS ), u"AI/Index"_s, Qgis::MessageLevel::Info, false );
  }
  {
    QSqlQuery states( db );
    if ( states.exec( u"SELECT layer_id, fingerprint FROM layer_state"_s ) )
    {
      while ( states.next() )
        loadedFingerprints.insert( states.value( 0 ).toString(), states.value( 1 ).toString() );
    }
  }

  QSqlQuery q( db );
  if ( !q.exec(
         u"SELECT source_type, relative_path, layer_id, feature_id_min, feature_id_max, chunk_index, text, wkt_blob, embedding, last_sync, provider_id, model_id, model_revision, embedding_dimension, content_hash, source_mtime FROM chunks ORDER BY id"_s
       ) )
  {
    if ( errorMessage )
      *errorMessage = q.lastError().text();
    return false;
  }
  while ( q.next() )
  {
    CachedChunk c;
    c.chunk.sourceType = q.value( 0 ).toString();
    if ( c.chunk.sourceType.isEmpty() )
      c.chunk.sourceType = QString::fromLatin1( SOURCE_TYPE_FILE );
    c.chunk.relativePath = q.value( 1 ).toString();
    c.chunk.layerId = q.value( 2 ).toString();
    c.chunk.firstFeatureId = q.value( 3 ).isNull() ? -1 : q.value( 3 ).toLongLong();
    c.chunk.lastFeatureId = q.value( 4 ).isNull() ? -1 : q.value( 4 ).toLongLong();
    c.chunk.chunkIndex = q.value( 5 ).toInt();
    // Per-value `enc1:` detection keeps legacy plaintext rows readable (mixed mode).
    c.chunk.text = QgsAiSecretStore::decryptValue( q.value( 6 ).toString() );
    c.chunk.wktBlob = QgsAiSecretStore::decryptBlob( q.value( 7 ).toByteArray() );
    c.embedding = bytesToVector( QgsAiSecretStore::decryptBlob( q.value( 8 ).toByteArray() ) );
    const qint64 syncMs = q.value( 9 ).toLongLong();
    c.providerId = q.value( 10 ).toString();
    c.modelId = q.value( 11 ).toString();
    c.modelRevision = q.value( 12 ).toString();
    c.embeddingDimension = q.value( 13 ).toInt();
    c.contentHash = q.value( 14 ).toString();
    c.sourceMTime = q.value( 15 ).toLongLong();
    if ( syncMs > maxSync )
      maxSync = syncMs;
    if ( !c.embedding.isEmpty() )
      loadedChunks.append( c );
  }
  commit();
  return true;
}

bool QgsAiWorkspaceIndex::persistAll(
  const QList<CachedChunk> &chunks,
  ReplaceScope scope,
  const QString &scopedLayerId,
  const QString &workspaceRoot,
  const QString &databaseProviderId,
  QString *errorMessage,
  const QHash<QString, QString> &layerFingerprints,
  const QStringList *replacedFilePaths
)
{
  // Runs without mMutex: the database write can take a while, and each thread has its own
  // SQLite connection. Callers update the cache under mMutex afterwards.
  const QString path = dbPathForRoot( workspaceRoot, databaseProviderId );
  if ( path.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"Workspace root is unset; cannot persist index."_s;
    return false;
  }

  QSqlDatabase db = indexOpenDatabase( connectionName(), path, errorMessage );
  if ( !db.isOpen() )
    return false;

  QSqlQuery q( db );
  q.exec( QStringLiteral(
    "CREATE TABLE IF NOT EXISTS chunks ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "source_type TEXT NOT NULL DEFAULT 'file', "
    "relative_path TEXT NOT NULL, "
    "layer_id TEXT, "
    "feature_id_min INTEGER, "
    "feature_id_max INTEGER, "
    "chunk_index INTEGER NOT NULL, "
    "text TEXT NOT NULL, "
    "wkt_blob BLOB, "
    "embedding BLOB NOT NULL, "
    "last_sync INTEGER NOT NULL, "
    "provider_id TEXT NOT NULL DEFAULT 'unknown', "
    "model_id TEXT NOT NULL DEFAULT 'unknown', "
    "model_revision TEXT NOT NULL DEFAULT '', "
    "embedding_dimension INTEGER NOT NULL DEFAULT 0, "
    "content_hash TEXT NOT NULL DEFAULT '', "
    "source_mtime INTEGER NOT NULL DEFAULT 0"
    ")"
  ) );
  q.exec( u"CREATE INDEX IF NOT EXISTS idx_chunks_layer ON chunks(layer_id) WHERE layer_id IS NOT NULL"_s );
  q.exec( u"CREATE INDEX IF NOT EXISTS idx_chunks_source_hash ON chunks(source_type, relative_path, chunk_index, content_hash)"_s );
  q.exec( u"PRAGMA user_version = %1"_s.arg( SCHEMA_VERSION ) );
  indexEnsureLayerStateTable( db );

  // Encrypt-at-rest when the data key is available; otherwise warn once and
  // persist plaintext (or refuse when the user requires encryption).
  const bool encryptionAvailable = QgsAiSecretStore::storageEncryptionAvailable();
  const bool requireEncryption = QgsSettings().value( u"ai/storage/requireEncryption"_s, false ).toBool();
  if ( !encryptionAvailable )
  {
    if ( requireEncryption )
    {
      if ( errorMessage )
        *errorMessage = u"Encryption is required (ai/storage/requireEncryption) but the authentication vault is unavailable."_s;
      return false;
    }
    QgsAiSecretStore::warnPlaintextStorageOnce();
  }

  // The replaced rows are deleted in the same transaction as the new rows are written,
  // so a concurrent reader never sees the scope empty.
  if ( !db.transaction() )
  {
    if ( errorMessage )
      *errorMessage = u"Cannot start SQLite transaction: %1"_s.arg( db.lastError().text() );
    return false;
  }

  switch ( scope )
  {
    case ReplaceScope::All:
      q.exec( u"DELETE FROM chunks"_s );
      q.exec( u"DELETE FROM layer_state"_s );
      break;
    case ReplaceScope::AllFiles:
      if ( replacedFilePaths )
      {
        QSqlQuery del( db );
        del.prepare( u"DELETE FROM chunks WHERE source_type = 'file' AND relative_path = ?"_s );
        for ( const QString &path : *replacedFilePaths )
        {
          del.addBindValue( path );
          del.exec();
        }
      }
      else
      {
        q.exec( u"DELETE FROM chunks WHERE source_type = 'file'"_s );
      }
      break;
    case ReplaceScope::AllLayers:
      q.exec( u"DELETE FROM chunks WHERE source_type = 'layer'"_s );
      q.exec( u"DELETE FROM layer_state"_s );
      break;
    case ReplaceScope::SingleLayer:
    {
      QSqlQuery del( db );
      del.prepare( u"DELETE FROM chunks WHERE source_type = 'layer' AND layer_id = ?"_s );
      del.addBindValue( scopedLayerId );
      del.exec();
      QSqlQuery delState( db );
      delState.prepare( u"DELETE FROM layer_state WHERE layer_id = ?"_s );
      delState.addBindValue( scopedLayerId );
      delState.exec();
      break;
    }
  }

  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  q.prepare( QStringLiteral(
    "INSERT INTO chunks (source_type, relative_path, layer_id, feature_id_min, feature_id_max, chunk_index, text, wkt_blob, embedding, last_sync, provider_id, model_id, model_revision, "
    "embedding_dimension, content_hash, source_mtime) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
  ) );
  for ( const CachedChunk &c : chunks )
  {
    const QString rowProviderId = c.providerId.isEmpty() ? u"test"_s : c.providerId;
    const QString modelId = c.modelId.isEmpty() ? u"test"_s : c.modelId;
    const QString modelRevision = c.modelRevision.isNull() ? u""_s : c.modelRevision;
    const int dimension = c.embeddingDimension > 0 ? c.embeddingDimension : static_cast<int>( c.embedding.size() );
    // The content hash stays computed over the PLAINTEXT so chunk-reuse keys
    // remain stable across encrypted and plaintext sessions.
    const QString hash = c.contentHash.isEmpty() ? textHash( c.chunk.text, c.chunk.wktBlob ) : c.contentHash;
    q.addBindValue( c.chunk.sourceType.isEmpty() ? QString::fromLatin1( SOURCE_TYPE_FILE ) : c.chunk.sourceType );
    q.addBindValue( c.chunk.relativePath );
    q.addBindValue( c.chunk.layerId.isEmpty() ? QVariant( QMetaType::fromType<QString>() ) : QVariant( c.chunk.layerId ) );
    q.addBindValue( c.chunk.firstFeatureId < 0 ? QVariant( QMetaType::fromType<qint64>() ) : QVariant( c.chunk.firstFeatureId ) );
    q.addBindValue( c.chunk.lastFeatureId < 0 ? QVariant( QMetaType::fromType<qint64>() ) : QVariant( c.chunk.lastFeatureId ) );
    q.addBindValue( c.chunk.chunkIndex );
    if ( !bindEncryptedIndexValue( q, c.chunk.text, encryptionAvailable, requireEncryption, errorMessage ) )
    {
      db.rollback();
      return false;
    }
    if ( c.chunk.wktBlob.isEmpty() )
    {
      q.addBindValue( QVariant( QMetaType::fromType<QByteArray>() ) );
    }
    else if ( !bindEncryptedIndexBlob( q, c.chunk.wktBlob, encryptionAvailable, requireEncryption, errorMessage ) )
    {
      db.rollback();
      return false;
    }
    if ( !bindEncryptedIndexBlob( q, vectorToBytes( c.embedding ), encryptionAvailable, requireEncryption, errorMessage ) )
    {
      db.rollback();
      return false;
    }
    q.addBindValue( nowMs );
    q.addBindValue( rowProviderId );
    q.addBindValue( modelId );
    q.addBindValue( modelRevision );
    q.addBindValue( dimension );
    q.addBindValue( hash );
    q.addBindValue( c.sourceMTime );
    if ( !q.exec() )
    {
      if ( errorMessage )
        *errorMessage = q.lastError().text();
      db.rollback();
      return false;
    }
  }
  if ( !layerFingerprints.isEmpty() )
  {
    QSqlQuery state( db );
    state.prepare( u"INSERT OR REPLACE INTO layer_state (layer_id, fingerprint, last_seen) VALUES (?, ?, ?)"_s );
    for ( auto it = layerFingerprints.constBegin(); it != layerFingerprints.constEnd(); ++it )
    {
      if ( it.value().isEmpty() )
        continue;
      state.addBindValue( it.key() );
      state.addBindValue( it.value() );
      state.addBindValue( nowMs );
      state.exec();
    }
  }
  if ( !db.commit() )
  {
    if ( errorMessage )
      *errorMessage = db.lastError().text();
    return false;
  }
  return true;
}

bool QgsAiWorkspaceIndex::persistChunks(
  const QList<Chunk> &chunks, const QList<QVector<float>> &embeddings, ReplaceScope scope, const QString &scopedLayerId, QString *errorMessage, const QString &workspaceRoot
)
{
  if ( chunks.size() != embeddings.size() )
  {
    if ( errorMessage )
      *errorMessage = u"persistChunks: chunks/embeddings size mismatch (%1 vs %2)"_s.arg( chunks.size() ).arg( embeddings.size() );
    return false;
  }

  const QString root = workspaceRoot.trimmed().isEmpty() ? this->workspaceRoot() : workspaceRoot;
  ensureLoaded();

  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  const ProviderInfo activeProvider = providerInfo( provider.get() );
  QList<CachedChunk> built;
  built.reserve( chunks.size() );
  for ( int i = 0; i < chunks.size(); ++i )
  {
    CachedChunk cc;
    cc.chunk = chunks.at( i );
    if ( cc.chunk.sourceType.isEmpty() )
      cc.chunk.sourceType = QString::fromLatin1( SOURCE_TYPE_FILE );
    cc.embedding = embeddings.at( i );
    cc.providerId = activeProvider.providerId;
    cc.modelId = activeProvider.modelId;
    cc.modelRevision = activeProvider.modelRevision;
    cc.embeddingDimension = storageDimension( activeProvider, cc.embedding );
    cc.contentHash = textHash( cc.chunk.text, cc.chunk.wktBlob );
    built.append( cc );
  }

  return storeChunks( built, scope, scopedLayerId, root, indexDatabaseProviderId( provider.get() ), {}, errorMessage );
}

bool QgsAiWorkspaceIndex::storeChunks(
  const QList<CachedChunk> &built,
  ReplaceScope scope,
  const QString &scopedLayerId,
  const QString &workspaceRoot,
  const QString &databaseProviderId,
  const QHash<QString, QString> &layerFingerprints,
  QString *errorMessage
)
{
  {
    const QgsAiPerfScope persistPerf( u"index_task"_s, u"persist_chunks"_s );
    if ( !persistAll( built, scope, scopedLayerId, workspaceRoot, databaseProviderId, errorMessage, layerFingerprints ) )
      return false;
  }

  // Reflect the change in the in-memory cache without re-reading the DB, unless the cache
  // now holds another workspace (the project changed while the chunks were computed).
  const QMutexLocker<QRecursiveMutex> locker( &mMutex );
  if ( !cacheHoldsRoot( workspaceRoot ) )
    return true;
  switch ( scope )
  {
    case ReplaceScope::All:
      mCache.clear();
      mLayerFingerprints.clear();
      break;
    case ReplaceScope::AllFiles:
      mCache.erase( std::remove_if( mCache.begin(), mCache.end(), []( const CachedChunk &c ) { return c.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_FILE ); } ), mCache.end() );
      break;
    case ReplaceScope::AllLayers:
      mCache.erase( std::remove_if( mCache.begin(), mCache.end(), []( const CachedChunk &c ) { return c.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_LAYER ); } ), mCache.end() );
      mLayerFingerprints.clear();
      break;
    case ReplaceScope::SingleLayer:
      mCache.erase(
        std::remove_if(
          mCache.begin(), mCache.end(), [&scopedLayerId]( const CachedChunk &c ) { return c.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_LAYER ) && c.chunk.layerId == scopedLayerId; }
        ),
        mCache.end()
      );
      mLayerFingerprints.remove( scopedLayerId );
      break;
  }
  mCache.append( built );
  for ( auto it = layerFingerprints.constBegin(); it != layerFingerprints.constEnd(); ++it )
  {
    if ( !it.value().isEmpty() )
      mLayerFingerprints.insert( it.key(), it.value() );
  }
  mLastSync = QDateTime::currentDateTimeUtc();
  mLoaded = true;
  updateStatusSnapshot();
  return true;
}

void QgsAiWorkspaceIndex::touchLayers( const QStringList &layerIds, const QString &workspaceRoot, const QString &databaseProviderId )
{
  const QString path = dbPathForRoot( workspaceRoot, databaseProviderId );
  if ( path.isEmpty() || layerIds.isEmpty() )
    return;
  QString error;
  QSqlDatabase db = indexOpenDatabase( connectionName(), path, &error );
  if ( !db.isOpen() )
    return;
  indexEnsureLayerStateTable( db );
  db.transaction();
  QSqlQuery touch( db );
  touch.prepare( u"UPDATE layer_state SET last_seen = ? WHERE layer_id = ?"_s );
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  for ( const QString &layerId : layerIds )
  {
    touch.addBindValue( nowMs );
    touch.addBindValue( layerId );
    touch.exec();
  }
  db.commit();
}

void QgsAiWorkspaceIndex::setActiveLayerIds( const QSet<QString> &layerIds )
{
  const QMutexLocker locker( &mActiveLayersMutex );
  mActiveLayerIds = layerIds;
}

bool QgsAiWorkspaceIndex::removeLayer( const QString &layerId, QString *errorMessage )
{
  if ( layerId.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"removeLayer: empty layerId."_s;
    return false;
  }

  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    mCache.erase(
      std::remove_if( mCache.begin(), mCache.end(), [&layerId]( const CachedChunk &c ) { return c.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_LAYER ) && c.chunk.layerId == layerId; } ),
      mCache.end()
    );
    mLayerFingerprints.remove( layerId );
    updateStatusSnapshot();
  }

  // The interface thread calls this when a layer is removed: never wait on the database,
  // which a background reindex may be writing. Removals queued meanwhile, such as a batch of
  // layers removed together, are deleted in one transaction, in order with later loads.
  const QString path = dbPath();
  if ( path.isEmpty() || !QFileInfo::exists( path ) )
    return true;
  {
    const QMutexLocker locker( &mPendingRemovalsMutex );
    mPendingLayerRemovals[path].insert( layerId );
    if ( mRemovalScheduled )
      return true;
    mRemovalScheduled = true;
  }
  indexDatabaseWorkPool()->start( [this]() { flushLayerRemovals(); } );
  return true;
}

void QgsAiWorkspaceIndex::flushLayerRemovals()
{
  QHash<QString, QSet<QString>> pending;
  {
    const QMutexLocker locker( &mPendingRemovalsMutex );
    pending.swap( mPendingLayerRemovals );
    mRemovalScheduled = false;
  }
  const QString connection = u"qgsai_index_remove_%1"_s.arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) );
  for ( auto it = pending.constBegin(); it != pending.constEnd(); ++it )
  {
    QString error;
    QSqlDatabase db = indexOpenDatabase( connection, it.key(), &error );
    if ( !db.isOpen() )
    {
      QgsMessageLog::logMessage( u"Layer index: cannot open %1 to remove layers: %2"_s.arg( it.key(), error ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );
      continue;
    }
    indexEnsureLayerStateTable( db );
    db.transaction();
    QSqlQuery del( db );
    del.prepare( u"DELETE FROM chunks WHERE source_type = 'layer' AND layer_id = ?"_s );
    QSqlQuery delState( db );
    delState.prepare( u"DELETE FROM layer_state WHERE layer_id = ?"_s );
    bool ok = true;
    for ( const QString &layerId : it.value() )
    {
      del.addBindValue( layerId );
      ok = del.exec() && ok;
      delState.addBindValue( layerId );
      ok = delState.exec() && ok;
    }
    if ( !ok || !db.commit() )
    {
      QgsMessageLog::logMessage( u"Layer index: removing %1 layers from the database failed: %2"_s.arg( it.value().size() ).arg( del.lastError().text() ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );
      db.rollback();
    }
    db.close();
  }
  QSqlDatabase::removeDatabase( connection );
}

qint64 QgsAiWorkspaceIndex::databaseSizeBytes() const
{
  const QString path = dbPath();
  if ( path.isEmpty() )
    return 0;
  qint64 bytes = 0;
  for ( const QString &suffix : { QString(), u"-wal"_s, u"-shm"_s } )
    bytes += QFileInfo( path + suffix ).size();
  return bytes;
}

int QgsAiWorkspaceIndex::removeStaleDatabases( const QString &directory, const QString &keepPath, int maxAgeDays )
{
  const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays( -maxAgeDays );
  const QString keep = QFileInfo( keepPath ).absoluteFilePath();
  int removed = 0;
  const QFileInfoList databases = QDir( directory ).entryInfoList( { u"ws_*.sqlite"_s }, QDir::Files );
  for ( const QFileInfo &database : databases )
  {
    if ( database.absoluteFilePath() == keep )
      continue;
    // The write-ahead log is touched on every write: it tells when the index was last used.
    const QFileInfo wal( database.absoluteFilePath() + u"-wal"_s );
    const QDateTime lastUsed = std::max( database.lastModified().toUTC(), wal.exists() ? wal.lastModified().toUTC() : QDateTime() );
    if ( lastUsed >= cutoff )
      continue;
    indexRemoveDatabaseFiles( database.absoluteFilePath() );
    ++removed;
  }
  return removed;
}

bool QgsAiWorkspaceIndex::createWorkspaceFileSnapshot( int maxFiles, QString &workspaceRoot, QList<WorkspaceFileSnapshot> &snapshot, QString *errorMessage ) const
{
  snapshot.clear();
  workspaceRoot = this->workspaceRoot();
  if ( !mContextProvider )
  {
    if ( errorMessage )
      *errorMessage = u"Workspace context provider is unavailable."_s;
    return false;
  }
  return scanWorkspaceFileSnapshot( workspaceRoot, maxFiles, snapshot, errorMessage );
}

bool QgsAiWorkspaceIndex::scanWorkspaceFileSnapshot( const QString &workspaceRoot, int maxFiles, QList<WorkspaceFileSnapshot> &snapshot, QString *errorMessage, QgsFeedback *feedback )
{
  snapshot.clear();
  if ( workspaceRoot.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"AI workspace root is unset. Save the QGIS project or configure the AI workspace root in provider settings."_s;
    return false;
  }

  if ( QgsAiFileContextProvider::isNetworkPath( workspaceRoot ) && !QgsSettings().value( u"strata/index/allow_network_workspace"_s, false ).toBool() )
  {
    // Walking and reading a network share can take minutes and loads the network.
    if ( errorMessage )
      *errorMessage = u"The AI workspace %1 is on a network share; file indexing is skipped. Enable strata/index/allow_network_workspace to index it anyway."_s.arg( workspaceRoot );
    return false;
  }

  const QgsAiPerfScope perf( u"index_task"_s, u"scan_files"_s );
  QgsAiFileContextProvider::WorkspaceScanOptions options;
  options.timeBudgetMs = FILE_SCAN_TIME_BUDGET_MS;
  options.excludedFolders = QgsSettings().value( u"strata/index/excluded_folders"_s ).toStringList();
  const QgsAiFileContextProvider::WorkspaceScanResult scan = QgsAiFileContextProvider::scanWorkspace( workspaceRoot, options, feedback );
  if ( feedback && feedback->isCanceled() )
  {
    if ( errorMessage )
      *errorMessage = indexCanceledMessage();
    return false;
  }
  if ( scan.truncated )
    QgsMessageLog::
      logMessage( u"Workspace scan of %1 stopped early after %2 entries (%3)."_s.arg( workspaceRoot ).arg( scan.visitedEntries ).arg( scan.timedOut ? u"time limit"_s : u"entry limit"_s ), u"AI/Index"_s, Qgis::MessageLevel::Info, false );

  const int cap = maxFiles > 0 ? maxFiles : DEFAULT_MAX_FILES;
  for ( const QgsAiFileContextProvider::WorkspaceFile &file : scan.files )
  {
    if ( !isTextFile( file.relativePath ) )
      continue;
    // A large table is still worth a summary of its first rows; other large files are skipped.
    const QString suffix = QFileInfo( file.relativePath ).suffix().toLower();
    const bool table = suffix == "csv"_L1 || suffix == "tsv"_L1;
    if ( file.size > ( table ? MAX_SUMMARIZED_FILE_BYTES : MAX_FILE_BYTES ) )
      continue;
    snapshot.append( { file.relativePath, file.absolutePath, file.lastModifiedMs, file.size } );
    if ( snapshot.size() >= cap )
      break;
  }
  return true;
}

bool QgsAiWorkspaceIndex::reindex( int maxFiles, QString *errorMessage, QgsFeedback *feedback )
{
  QString root;
  QList<WorkspaceFileSnapshot> snapshot;
  if ( !createWorkspaceFileSnapshot( maxFiles, root, snapshot, errorMessage ) )
    return false;
  return reindex( snapshot, root, errorMessage, feedback );
}

bool QgsAiWorkspaceIndex::reindex( const QList<WorkspaceFileSnapshot> &snapshot, const QString &workspaceRoot, QString *errorMessage, QgsFeedback *feedback )
{
  if ( workspaceRoot.trimmed().isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"AI workspace root is unset. Save the QGIS project or configure the AI workspace root in provider settings."_s;
    return false;
  }

  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  if ( !ensureEmbeddingProviderAvailable( provider.get(), errorMessage ) )
    return false;
  const ProviderInfo activeProvider = providerInfo( provider.get() );

  ensureLoaded();

  // The index already holds these file chunks for this provider, by file and by chunk position.
  QHash<QString, QList<CachedChunk>> cachedFileChunks;
  QHash<QString, CachedChunk> reusableFileChunks;
  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    if ( cacheHoldsRoot( workspaceRoot ) )
    {
      for ( const CachedChunk &cached : std::as_const( mCache ) )
      {
        if ( ( cached.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_FILE ) || cached.chunk.sourceType.isEmpty() )
             && metadataMatchesProvider( cached.providerId, cached.modelId, cached.modelRevision, cached.embeddingDimension, activeProvider ) )
        {
          cachedFileChunks[cached.chunk.relativePath].append( cached );
          reusableFileChunks.insert( chunkReuseKey( QString::fromLatin1( SOURCE_TYPE_FILE ), cached.chunk.relativePath, QString(), cached.chunk.chunkIndex ), cached );
        }
      }
    }
  }

  // Progress: reading files is the first 20 %, embeddings the next 75 %, writing the rest.
  const auto reportProgress = [feedback]( double percent ) {
    if ( feedback )
      feedback->setProgress( percent );
  };

  const auto [tokenCounter, chunkTokens] = indexTokenBudget( provider.get() );

  // Only files that changed since they were indexed are read and written again.
  QList<CachedChunk> built;
  QList<CachedChunk> unchangedChunks;
  QStringList changedFiles;
  QStringList textsToEmbed;
  QList<int> backRefIndex;
  int unchangedFileCount = 0;
  {
    const QgsAiPerfScope readPerf( u"index_task"_s, u"read_files files=%1"_s.arg( snapshot.size() ) );
    for ( int fileIdx = 0; fileIdx < snapshot.size(); ++fileIdx )
    {
      if ( feedback && feedback->isCanceled() )
      {
        if ( errorMessage )
          *errorMessage = indexCanceledMessage();
        return false;
      }
      const WorkspaceFileSnapshot &file = snapshot.at( fileIdx );
      emit progress( fileIdx + 1, snapshot.size(), file.relativePath );
      reportProgress( 20.0 * ( fileIdx + 1 ) / snapshot.size() );

      const QList<CachedChunk> cached = cachedFileChunks.value( file.relativePath );
      const bool unchanged = !cached.isEmpty() && std::all_of( cached.cbegin(), cached.cend(), [&file]( const CachedChunk &c ) { return c.sourceMTime == file.sourceMTime && !c.embedding.isEmpty(); } );
      if ( unchanged )
      {
        ++unchangedFileCount;
        unchangedChunks.append( cached );
        continue;
      }
      changedFiles << file.relativePath;

      const QString fileText = readSnapshotTextFile( file.absolutePath, MAX_FILE_BYTES );
      if ( fileText.isEmpty() )
        continue;

      // Structured files are indexed from a summary, chunks are sized for the model's input.
      const QString summary = QgsAiFileSummarizer::summarize( file.relativePath, fileText, file.size > MAX_FILE_BYTES );
      const QStringList chunks = chunkTextByTokens( summary.isEmpty() ? fileText : summary, tokenCounter, chunkTokens );
      for ( int ci = 0; ci < chunks.size(); ++ci )
      {
        CachedChunk c;
        c.chunk.relativePath = file.relativePath;
        c.chunk.chunkIndex = ci;
        c.chunk.text = chunks.at( ci );
        c.chunk.sourceType = QString::fromLatin1( SOURCE_TYPE_FILE );
        c.providerId = activeProvider.providerId;
        c.modelId = activeProvider.modelId;
        c.modelRevision = activeProvider.modelRevision;
        c.contentHash = textHash( c.chunk.text );
        c.sourceMTime = file.sourceMTime;

        const QString reuseKey = chunkReuseKey( QString::fromLatin1( SOURCE_TYPE_FILE ), file.relativePath, QString(), ci );
        const auto reusableIt = reusableFileChunks.constFind( reuseKey );
        if ( reusableIt != reusableFileChunks.constEnd() && reusableIt->contentHash == c.contentHash && !reusableIt->embedding.isEmpty() )
        {
          c.embedding = reusableIt->embedding;
          c.embeddingDimension = reusableIt->embeddingDimension;
          built.append( c );
          continue;
        }

        built.append( c );
        backRefIndex.append( built.size() - 1 );
        textsToEmbed.append( c.chunk.text );
      }
    }
  }

  // Files indexed before but no longer in the workspace.
  QSet<QString> snapshotPaths;
  for ( const WorkspaceFileSnapshot &file : snapshot )
    snapshotPaths.insert( file.relativePath );
  QStringList replacedFiles = changedFiles;
  for ( auto it = cachedFileChunks.constBegin(); it != cachedFileChunks.constEnd(); ++it )
  {
    if ( !snapshotPaths.contains( it.key() ) )
      replacedFiles << it.key();
  }

  if ( !textsToEmbed.isEmpty() )
  {
    const QgsAiPerfScope embedPerf( u"index_task"_s, u"embed_files chunks=%1"_s.arg( textsToEmbed.size() ) );
    QList<QVector<float>> vectors;
    const auto embedProgress = [&reportProgress]( double percent ) { reportProgress( 20.0 + 0.75 * percent ); };
    if ( !indexEmbedInBatches( mProviderUseMutex, mSearchesWaiting, provider.get(), textsToEmbed, QgsAiEmbeddingRole::Passage, vectors, errorMessage, feedback, embedProgress ) )
      return false;

    for ( int i = 0; i < vectors.size(); ++i )
    {
      CachedChunk &chunk = built[backRefIndex.at( i )];
      chunk.embedding = vectors.at( i );
      chunk.embeddingDimension = storageDimension( activeProvider, chunk.embedding );
    }
  }

  if ( feedback && feedback->isCanceled() )
  {
    if ( errorMessage )
      *errorMessage = indexCanceledMessage();
    return false;
  }

  // When the cache mirrors this workspace's database, the rows of unchanged files stay as they
  // are. Otherwise (another workspace or embedding model in the cache) every file row is rewritten.
  bool partial = false;
  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    partial = cacheHoldsRoot( workspaceRoot ) && std::none_of( mCache.cbegin(), mCache.cend(), [&activeProvider]( const CachedChunk &c ) {
                return ( c.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_FILE ) || c.chunk.sourceType.isEmpty() )
                       && !metadataMatchesProvider( c.providerId, c.modelId, c.modelRevision, c.embeddingDimension, activeProvider );
              } );
  }
  if ( !partial )
    built.append( unchangedChunks );

  if ( !partial || !replacedFiles.isEmpty() )
  {
    const QgsAiPerfScope persistPerf( u"index_task"_s, u"persist_files"_s );
    if ( !persistAll( built, ReplaceScope::AllFiles, QString(), workspaceRoot, indexDatabaseProviderId( provider.get() ), errorMessage, {}, partial ? &replacedFiles : nullptr ) )
      return false;
  }
  reportProgress( 100.0 );

  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    if ( cacheHoldsRoot( workspaceRoot ) )
    {
      const QSet<QString> replaced( replacedFiles.cbegin(), replacedFiles.cend() );
      mCache.erase(
        std::remove_if(
          mCache.begin(),
          mCache.end(),
          [partial, &replaced]( const CachedChunk &c ) {
            const bool isFile = c.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_FILE ) || c.chunk.sourceType.isEmpty();
            return isFile && ( !partial || replaced.contains( c.chunk.relativePath ) );
          }
        ),
        mCache.end()
      );
      mCache.append( built );
      mLastSync = QDateTime::currentDateTimeUtc();
      mLoaded = true;
      updateStatusSnapshot();
    }
  }
  QgsMessageLog::logMessage(
    u"Workspace index built: files=%1 unchanged=%2 changed=%3 removed=%4 embedded=%5"_s.arg( snapshot.size() )
      .arg( unchangedFileCount )
      .arg( changedFiles.size() )
      .arg( replacedFiles.size() - changedFiles.size() )
      .arg( textsToEmbed.size() ),
    u"AI/Index"_s,
    Qgis::MessageLevel::Info,
    false
  );
  return true;
}

QList<QgsAiWorkspaceIndex::Chunk> QgsAiWorkspaceIndex::search( const QString &query, int k, QString *errorMessage, QgsFeedback *feedback )
{
  QList<Chunk> results;
  if ( query.trimmed().isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"Empty query."_s;
    return results;
  }

  const QgsAiPerfScope perf( u"index"_s, u"search"_s );
  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  if ( !ensureEmbeddingProviderAvailable( provider.get(), errorMessage ) )
    return results;

  ensureLoaded();
  std::optional<QSet<QString>> activeLayers;
  {
    const QMutexLocker locker( &mActiveLayersMutex );
    activeLayers = mActiveLayerIds;
  }
  QList<CachedChunk> cache;
  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    cache.reserve( mCache.size() );
    for ( const CachedChunk &cached : std::as_const( mCache ) )
    {
      // Layers of projects that are not open stay indexed for when they open again.
      if ( activeLayers && cached.chunk.sourceType == QString::fromLatin1( SOURCE_TYPE_LAYER ) && !activeLayers->contains( cached.chunk.layerId ) )
        continue;
      cache.append( cached );
    }
  }

  if ( cache.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"Workspace index is empty. Run reindex_workspace first."_s;
    return results;
  }

  // A reindex holds the provider for one batch at a time: wait for that batch, not the whole run.
  ++mSearchesWaiting;
  const bool providerLocked = mProviderUseMutex.tryLock( SEARCH_LOCK_TIMEOUT_MS );
  --mSearchesWaiting;
  if ( !providerLocked )
  {
    if ( errorMessage )
      *errorMessage = u"The workspace index is busy updating; try the search again in a moment."_s;
    return results;
  }
  const auto unlockProvider = qScopeGuard( [this]() { mProviderUseMutex.unlock(); } );
  if ( feedback && feedback->isCanceled() )
  {
    if ( errorMessage )
      *errorMessage = u"Search cancelled."_s;
    return results;
  }

  QStringList qList { query };
  QList<QVector<float>> qEmb;
  QgsAiEmbeddingOptions options;
  options.maxBatch = 1;
  options.feedback = feedback;
  if ( !provider->embed( qList, QgsAiEmbeddingRole::Query, qEmb, errorMessage, options ) || qEmb.isEmpty() )
    return results;

  if ( feedback && feedback->isCanceled() )
    return results;

  const QVector<float> &qVec = qEmb.first();

  // Linear cosine scan; small enough for the MVP.
  QList<QPair<float, int>> scored;
  scored.reserve( cache.size() );
  for ( int i = 0; i < cache.size(); ++i )
    scored.append( { cosineSimilarity( qVec, cache.at( i ).embedding ), i } );

  std::sort( scored.begin(), scored.end(), []( const auto &a, const auto &b ) { return a.first > b.first; } );

  const int topK = std::clamp( k, 1, std::min( 20, static_cast<int>( cache.size() ) ) );
  for ( int i = 0; i < topK; ++i )
  {
    Chunk c = cache.at( scored.at( i ).second ).chunk;
    c.score = scored.at( i ).first;
    results.append( c );
  }
  return results;
}

bool QgsAiWorkspaceIndex::createWorkspaceLayerSnapshot( WorkspaceLayerSnapshot &snapshot, QString *errorMessage ) const
{
  snapshot = WorkspaceLayerSnapshot();
  snapshot.scope = ReplaceScope::AllLayers;
  snapshot.workspaceRoot = workspaceRoot();

  QgsProject *project = QgsProject::instance();
  if ( !project )
  {
    if ( errorMessage )
      *errorMessage = u"No active QgsProject available."_s;
    return false;
  }

  const QMap<QString, QgsMapLayer *> layers = project->mapLayers();
  snapshot.layerCount = layers.size();
  for ( auto it = layers.constBegin(); it != layers.constEnd(); ++it )
    snapshot.preparedLayers.append( std::make_shared<const QgsAiPreparedLayer>( QgsAiLayerChunker::prepare( it.value() ) ) );
  return true;
}

bool QgsAiWorkspaceIndex::createWorkspaceLayerSnapshotForLayer( const QString &layerId, WorkspaceLayerSnapshot &snapshot, QString *errorMessage ) const
{
  snapshot = WorkspaceLayerSnapshot();
  snapshot.scope = ReplaceScope::SingleLayer;
  snapshot.scopedLayerId = layerId;
  snapshot.workspaceRoot = workspaceRoot();
  if ( layerId.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"createWorkspaceLayerSnapshotForLayer: empty layerId."_s;
    return false;
  }

  QgsProject *project = QgsProject::instance();
  QgsMapLayer *layer = project ? project->mapLayer( layerId ) : nullptr;
  if ( !layer )
  {
    snapshot.layerCount = 0;
    return true;
  }

  snapshot.layerCount = 1;
  snapshot.preparedLayers.append( std::make_shared<const QgsAiPreparedLayer>( QgsAiLayerChunker::prepare( layer ) ) );
  return true;
}

bool QgsAiWorkspaceIndex::materializeLayerSnapshot( WorkspaceLayerSnapshot &snapshot, QgsFeedback *feedback, const TokenCounter &tokenCount, int maxTokens )
{
  for ( const std::shared_ptr<const QgsAiPreparedLayer> &prepared : std::as_const( snapshot.preparedLayers ) )
  {
    if ( feedback && feedback->isCanceled() )
      return false;
    if ( prepared )
      snapshot.chunks.append( QgsAiLayerChunker::chunk( *prepared, feedback, tokenCount, maxTokens ) );
  }
  snapshot.preparedLayers.clear();
  return !( feedback && feedback->isCanceled() );
}

bool QgsAiWorkspaceIndex::reindexLayerSnapshot( const WorkspaceLayerSnapshot &preparedSnapshot, QString *errorMessage, QgsFeedback *feedback )
{
  if ( preparedSnapshot.scope != ReplaceScope::AllLayers && preparedSnapshot.scope != ReplaceScope::SingleLayer )
  {
    if ( errorMessage )
      *errorMessage = u"Layer snapshots must use AllLayers or SingleLayer replace scope."_s;
    return false;
  }
  if ( preparedSnapshot.scope == ReplaceScope::SingleLayer && preparedSnapshot.scopedLayerId.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"Layer snapshot is missing scoped layer id."_s;
    return false;
  }

  const std::shared_ptr<QgsAiEmbeddingProvider> provider = providerSnapshot();
  if ( !ensureEmbeddingProviderAvailable( provider.get(), errorMessage ) )
    return false;
  const ProviderInfo activeProvider = providerInfo( provider.get() );
  const QString databaseProviderId = indexDatabaseProviderId( provider.get() );
  const QString root = preparedSnapshot.workspaceRoot.trimmed().isEmpty() ? workspaceRoot() : preparedSnapshot.workspaceRoot;
  ensureLoaded();

  // What the index already holds for this provider: layer chunks by layer and embeddings by text.
  QHash<QString, QList<CachedChunk>> cachedLayerChunks;
  QHash<QString, CachedChunk> embeddingsByHash;
  QHash<QString, QString> storedFingerprints;
  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    if ( cacheHoldsRoot( root ) )
    {
      for ( const CachedChunk &cached : std::as_const( mCache ) )
      {
        if ( cached.chunk.sourceType != QString::fromLatin1( SOURCE_TYPE_LAYER )
             || cached.embedding.isEmpty()
             || !metadataMatchesProvider( cached.providerId, cached.modelId, cached.modelRevision, cached.embeddingDimension, activeProvider ) )
          continue;
        cachedLayerChunks[cached.chunk.layerId].append( cached );
        embeddingsByHash.insert( cached.contentHash, cached );
      }
      storedFingerprints = mLayerFingerprints;
    }
  }

  // Layers whose settings and files did not change since they were indexed are not read again.
  WorkspaceLayerSnapshot snapshot = preparedSnapshot;
  QHash<QString, QString> fingerprints;
  QStringList unchangedLayerIds;
  QList<CachedChunk> unchangedChunks;
  if ( !snapshot.preparedLayers.isEmpty() )
  {
    const QgsAiPerfScope checkPerf( u"index_task"_s, u"check_layers"_s, 20 );
    QList<std::shared_ptr<const QgsAiPreparedLayer>> changed;
    for ( const std::shared_ptr<const QgsAiPreparedLayer> &prepared : std::as_const( snapshot.preparedLayers ) )
    {
      if ( !prepared )
        continue;
      const QString fingerprint = QgsAiLayerChunker::fingerprint( *prepared );
      fingerprints.insert( prepared->layerId, fingerprint );
      if ( !fingerprint.isEmpty() && storedFingerprints.value( prepared->layerId ) == fingerprint && cachedLayerChunks.contains( prepared->layerId ) )
      {
        unchangedLayerIds << prepared->layerId;
        unchangedChunks.append( cachedLayerChunks.value( prepared->layerId ) );
        continue;
      }
      changed.append( prepared );
    }
    snapshot.preparedLayers = changed;
  }

  if ( snapshot.scope == ReplaceScope::SingleLayer && !unchangedLayerIds.isEmpty() )
  {
    touchLayers( unchangedLayerIds, root, databaseProviderId );
    qgsAiLogPerf( u"index_task"_s, u"layer_unchanged"_s, 0 );
    return true;
  }

  if ( !snapshot.preparedLayers.isEmpty() )
  {
    const QgsAiPerfScope chunkPerf( u"index_task"_s, u"read_layer"_s );
    const auto [tokenCounter, chunkTokens] = indexTokenBudget( provider.get() );
    if ( !materializeLayerSnapshot( snapshot, feedback, tokenCounter, chunkTokens ) )
    {
      if ( errorMessage )
        *errorMessage = indexCanceledMessage();
      return false;
    }
  }

  // Chunks whose text was embedded before, for this layer or another, reuse that embedding.
  QList<CachedChunk> built;
  built.reserve( snapshot.chunks.size() + unchangedChunks.size() );
  QStringList textsToEmbed;
  QList<int> backRefIndex;
  for ( const Chunk &chunk : std::as_const( snapshot.chunks ) )
  {
    CachedChunk c;
    c.chunk = chunk;
    c.chunk.sourceType = QString::fromLatin1( SOURCE_TYPE_LAYER );
    c.providerId = activeProvider.providerId;
    c.modelId = activeProvider.modelId;
    c.modelRevision = activeProvider.modelRevision;
    c.contentHash = textHash( c.chunk.text, c.chunk.wktBlob );
    const auto reusable = embeddingsByHash.constFind( c.contentHash );
    if ( reusable != embeddingsByHash.constEnd() )
    {
      c.embedding = reusable->embedding;
      c.embeddingDimension = reusable->embeddingDimension;
    }
    else
    {
      backRefIndex.append( static_cast<int>( built.size() ) );
      textsToEmbed.append( c.chunk.text );
    }
    built.append( c );
  }

  if ( !textsToEmbed.isEmpty() )
  {
    QList<QVector<float>> vectors;
    const QgsAiPerfScope embedPerf( u"index_task"_s, u"embed_layer chunks=%1 reused=%2"_s.arg( textsToEmbed.size() ).arg( built.size() - textsToEmbed.size() ) );
    if ( !indexEmbedInBatches( mProviderUseMutex, mSearchesWaiting, provider.get(), textsToEmbed, QgsAiEmbeddingRole::Passage, vectors, errorMessage, feedback ) )
      return false;
    for ( int i = 0; i < vectors.size(); ++i )
    {
      CachedChunk &chunk = built[backRefIndex.at( i )];
      chunk.embedding = vectors.at( i );
      chunk.embeddingDimension = storageDimension( activeProvider, chunk.embedding );
    }
  }
  if ( feedback && feedback->isCanceled() )
  {
    if ( errorMessage )
      *errorMessage = indexCanceledMessage();
    return false;
  }

  // AllLayers replaces every layer row: unchanged layers keep theirs by being written back.
  built.append( unchangedChunks );
  if ( !storeChunks( built, snapshot.scope, snapshot.scopedLayerId, root, databaseProviderId, fingerprints, errorMessage ) )
    return false;

  QgsMessageLog::logMessage(
    snapshot.scope == ReplaceScope::AllLayers
      ? u"Workspace index: layer reindex done — layers=%1 unchanged=%2 chunks=%3 embedded=%4"_s.arg( snapshot.layerCount ).arg( unchangedLayerIds.size() ).arg( built.size() ).arg( textsToEmbed.size() )
      : u"Workspace index: layer reindex done — layer=%1 chunks=%2 embedded=%3"_s.arg( snapshot.scopedLayerId ).arg( built.size() ).arg( textsToEmbed.size() ),
    u"AI/Index"_s,
    Qgis::MessageLevel::Info,
    false
  );
  return true;
}

bool QgsAiWorkspaceIndex::reindexLayers( QString *errorMessage )
{
  if ( !ensureEmbeddingProviderAvailable( providerSnapshot().get(), errorMessage ) )
    return false;

  WorkspaceLayerSnapshot snapshot;
  if ( !createWorkspaceLayerSnapshot( snapshot, errorMessage ) )
    return false;
  return reindexLayerSnapshot( snapshot, errorMessage );
}

bool QgsAiWorkspaceIndex::reindexLayer( const QString &layerId, QString *errorMessage )
{
  if ( !ensureEmbeddingProviderAvailable( providerSnapshot().get(), errorMessage ) )
    return false;

  WorkspaceLayerSnapshot snapshot;
  if ( !createWorkspaceLayerSnapshotForLayer( layerId, snapshot, errorMessage ) )
    return false;
  return reindexLayerSnapshot( snapshot, errorMessage );
}

void QgsAiWorkspaceIndex::clear()
{
  const QString path = dbPath();
  indexDatabaseWorkPool()->waitForDone();
  {
    const QMutexLocker<QRecursiveMutex> locker( &mMutex );
    mCache.clear();
    mLayerFingerprints.clear();
    mLastSync = QDateTime();
    updateStatusSnapshot();
  }
  closeDatabaseConnectionForCurrentThread();
  if ( !path.isEmpty() )
    indexRemoveDatabaseFiles( path );
}
