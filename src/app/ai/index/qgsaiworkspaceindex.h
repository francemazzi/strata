/***************************************************************************
    qgsaiworkspaceindex.h
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

#ifndef QGSAIWORKSPACEINDEX_H
#define QGSAIWORKSPACEINDEX_H

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "qgis_app.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QRecursiveMutex>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

class QgsAiEmbeddingProvider;
class QgsAiFileContextProvider;
class QgsFeedback;
class QgsTask;
struct QgsAiPreparedLayer;

/**
 * Lightweight retrieval index over the user's workspace. The model uses this
 * via the search_workspace / index_status / reindex_workspace tools to ground
 * its answers on actual project content (RAG).
 *
 * MVP design (intentionally simple):
 *
 * - Plain SQLite database stored under qgisSettingsDirPath()/ai_index/, one
 *   file per workspace (hashed root path).
 * - One row per text chunk: { file_path, chunk_index, text, embedding BLOB }.
 * - Embeddings come from the configured local embeddings provider.
 * - Retrieval is a linear cosine-similarity scan in C++ (fast enough for
 *   tens of thousands of chunks; we cap reindex at 500 files for the MVP).
 *
 * Threading: indexing and search run on worker threads. The interface thread only
 * prepares snapshots (cheap), reads status() (never blocks) and asks for the cache to
 * be loaded in the background (requestLoad()). The embedding provider is shared with
 * the tasks using it, and its lock is held for one embedding batch at a time, so a
 * search waits at most one batch for a running reindex.
 *
 * Privacy: the default product path is local/on-device embeddings. Remote
 * embedding providers must be explicitly selected by future code before use.
 */
class APP_EXPORT QgsAiWorkspaceIndex : public QObject
{
    Q_OBJECT

  public:
    static constexpr int CHUNK_TARGET_CHARS = 1200;
    static constexpr int MAX_FILE_BYTES = 256 * 1024;
    static constexpr int DEFAULT_MAX_FILES = 500;
    static constexpr int EMBEDDING_BATCH = 16;
    //! How long search() waits for a running reindex batch before reporting the index as busy.
    static constexpr int SEARCH_LOCK_TIMEOUT_MS = 2000;
    //! Time budget of a workspace file scan.
    static constexpr int FILE_SCAN_TIME_BUDGET_MS = 5000;

    /**
     * Bumped when the on-disk SQLite schema or the chunk text changes; older DBs are dropped on first load.
     * 5: chunks sized in tokens, structured files summarized.
     */
    static constexpr int SCHEMA_VERSION = 5;
    //! Tokens kept free below the model's input limit for the "passage: " prefix and special tokens.
    static constexpr int CHUNK_TOKEN_MARGIN = 62;
    //! Tables (CSV, TSV) larger than MAX_FILE_BYTES are indexed from a summary of their start, up to this size.
    static constexpr qint64 MAX_SUMMARIZED_FILE_BYTES = 512LL * 1024 * 1024;
    //! Index databases of other workspaces unused for this many days are deleted.
    static constexpr int STALE_DATABASE_DAYS = 30;
    //! The local model is unloaded after this many seconds without embedding (strata/index/model_idle_unload_s).
    static constexpr int DEFAULT_MODEL_IDLE_UNLOAD_S = 180;

    //! Discriminates between workspace-file chunks and layer-data chunks.
    static constexpr const char *SOURCE_TYPE_FILE = "file";
    static constexpr const char *SOURCE_TYPE_LAYER = "layer";

    /**
     * Scope of a persistChunks() call. Determines which existing rows are replaced.
     *
     * - All: drop everything and write the new chunks.
     * - AllFiles: replace every chunk with source_type='file' (layer chunks preserved).
     * - AllLayers: replace every chunk with source_type='layer' (file chunks preserved).
     * - SingleLayer: replace only the chunks of the given layer id (file + other layer chunks preserved).
     */
    enum class ReplaceScope
    {
      All,
      AllFiles,
      AllLayers,
      SingleLayer,
    };

    struct Chunk
    {
        //! "file" or "layer" (see SOURCE_TYPE_*).
        QString sourceType = QString::fromLatin1( SOURCE_TYPE_FILE );
        //! For files: workspace-relative path. For layers: layer name (cosmetic).
        QString relativePath;
        //! Layer id (QgsMapLayer::id()). Empty for file chunks.
        QString layerId;
        //! Range of QgsFeature ids covered by this chunk. -1 when not applicable (file/raster).
        qint64 firstFeatureId = -1;
        qint64 lastFeatureId = -1;
        int chunkIndex = 0;
        QString text;
        //! Compressed (qCompress) join of the WKT of the features in this chunk. Empty for files/raster.
        QByteArray wktBlob;
        float score = 0.0f; // populated by search()
    };

    struct Status
    {
        bool indexed = false;
        //! The cache of the current workspace is still being loaded in the background.
        bool loading = false;
        int fileCount = 0;
        int chunkCount = 0;
        int fileChunkCount = 0;
        int layerChunkCount = 0;
        QDateTime lastSync;
        QString workspaceRoot;
        QString embeddingProviderId;
        QString embeddingModelId;
    };

    struct WorkspaceFileSnapshot
    {
        QString relativePath;
        QString absolutePath;
        qint64 sourceMTime = 0;
        qint64 size = 0;
    };

    //! Counts the tokens of a text for the embedding model, or returns -1 when it cannot.
    using TokenCounter = std::function<int( const QString &text )>;

    /**
     * Splits \a content into chunks of at most \a maxTokens tokens, at line ends where possible.
     * A line longer than that is cut into pieces.
     */
    static QStringList chunkTextByTokens( const QString &content, const TokenCounter &tokenCount, int maxTokens );

    //! The longest start of \a text within \a maxTokens tokens.
    static QString truncateToTokens( const QString &text, const TokenCounter &tokenCount, int maxTokens );

    struct WorkspaceLayerSnapshot
    {
        ReplaceScope scope = ReplaceScope::AllLayers;
        QString scopedLayerId;
        int layerCount = 0;
        //! Workspace whose database receives the chunks, captured when the snapshot was prepared.
        QString workspaceRoot;
        //! Layers prepared on the interface thread; materializeLayerSnapshot() turns them into chunks.
        QList<std::shared_ptr<const QgsAiPreparedLayer>> preparedLayers;
        QList<Chunk> chunks;
    };

    QgsAiWorkspaceIndex( QgsAiFileContextProvider *contextProvider, QgsAiEmbeddingProvider *embeddingProvider, QObject *parent = nullptr );
    ~QgsAiWorkspaceIndex() override;

    //! Uses \a embeddingProvider without owning it. The caller keeps it alive while the index may use it.
    void setEmbeddingProvider( QgsAiEmbeddingProvider *embeddingProvider );
    //! Shares ownership of \a embeddingProvider with the tasks using it, so it can be replaced while they run.
    void setEmbeddingProvider( std::shared_ptr<QgsAiEmbeddingProvider> embeddingProvider );
    virtual bool embeddingProviderAvailable() const;
    //! Why embeddingProviderAvailable() is false, worded for the user; empty when it is available.
    QString unavailableReason() const;
    //! Temporary compatibility wrapper for older call sites.
    virtual bool hasEmbeddingConfiguration() const;

    /**
     * Counts of the cached chunks. Never blocks: while a background task holds the index it
     * returns the last known counts, and while the cache loads it reports Status::loading.
     */
    Status status() const;

    //! Root of the current workspace. Call on the interface thread.
    QString workspaceRoot() const;

    /**
     * Walks the workspace, chunks every eligible text file, embeds the chunks
     * via the configured local embeddings provider, and stores them in the local SQLite
     * database, replacing any previous content. Returns false on failure with
     * \a errorMessage filled.
     *
     * \param maxFiles    Cap on the number of files indexed in one run (default 500).
     */
    bool reindex( int maxFiles, QString *errorMessage = nullptr, QgsFeedback *feedback = nullptr );

    //! Creates an immutable file snapshot of the current workspace, on the calling thread.
    bool createWorkspaceFileSnapshot( int maxFiles, QString &workspaceRoot, QList<WorkspaceFileSnapshot> &snapshot, QString *errorMessage = nullptr ) const;

    /**
     * Scans \a workspaceRoot for indexable text files. Safe on any thread: it does not read
     * the index or the context provider. Excluded folders are skipped at any depth and the
     * walk stops after FILE_SCAN_TIME_BUDGET_MS.
     */
    static bool scanWorkspaceFileSnapshot( const QString &workspaceRoot, int maxFiles, QList<WorkspaceFileSnapshot> &snapshot, QString *errorMessage = nullptr, QgsFeedback *feedback = nullptr );

    //! Reindexes file chunks from a prebuilt snapshot without reading the context provider.
    bool reindex( const QList<WorkspaceFileSnapshot> &snapshot, const QString &workspaceRoot, QString *errorMessage = nullptr, QgsFeedback *feedback = nullptr );

    //! Prepares every layer of the active project on the calling (interface) thread. Cheap.
    bool createWorkspaceLayerSnapshot( WorkspaceLayerSnapshot &snapshot, QString *errorMessage = nullptr ) const;

    //! Prepares one layer on the calling (interface) thread. Cheap.
    bool createWorkspaceLayerSnapshotForLayer( const QString &layerId, WorkspaceLayerSnapshot &snapshot, QString *errorMessage = nullptr ) const;

    /**
     * Reads the prepared layers of \a snapshot into chunks, of at most \a maxTokens tokens when
     * \a tokenCount is set. Safe on any thread. Returns false if canceled.
     */
    static bool materializeLayerSnapshot( WorkspaceLayerSnapshot &snapshot, QgsFeedback *feedback = nullptr, const TokenCounter &tokenCount = {}, int maxTokens = 0 );

    //! Chunks (if still needed), embeds and persists a prepared layer snapshot. Meant for a worker thread.
    virtual bool reindexLayerSnapshot( const WorkspaceLayerSnapshot &snapshot, QString *errorMessage = nullptr, QgsFeedback *feedback = nullptr );

    /**
     * Runs a cosine similarity search for \a query, returning the best \a k chunks.
     *
     * Cancelling \a feedback aborts the (possibly remote) query embedding. If a reindex holds
     * the embedding provider for longer than SEARCH_LOCK_TIMEOUT_MS, the search gives up and
     * reports the index as busy instead of waiting for the whole reindex.
     */
    virtual QList<Chunk> search( const QString &query, int k, QString *errorMessage = nullptr, QgsFeedback *feedback = nullptr );

    /**
     * Builds chunks for every layer in the active QgsProject (vectors + rasters),
     * embeds them and persists them with ReplaceScope::AllLayers (file chunks
     * are preserved). Returns false on failure with \a errorMessage filled.
     */
    bool reindexLayers( QString *errorMessage = nullptr );

    /**
     * Re-embeds and persists the chunks for a single layer identified by
     * \a layerId. Existing chunks for the same layer are replaced; other
     * layers and file chunks are preserved.
     */
    virtual bool reindexLayer( const QString &layerId, QString *errorMessage = nullptr );

    /**
     * Drops the index file from disk and clears the in-memory cache. Useful
     * when the user wants to revoke the embedding cache.
     */
    void clear();

    //! Size on disk of the current workspace's index, write-ahead log included.
    qint64 databaseSizeBytes() const;

    //! SQLite file of the current workspace's index with the active embedding provider.
    QString databasePath() const { return dbPath(); }

    /**
     * Layers of the open project. search() leaves out the chunks of other layers, which are kept
     * so that reopening their project reuses them. Until this is called, search() uses them all.
     */
    void setActiveLayerIds( const QSet<QString> &layerIds );

    /**
     * Deletes the index databases in \a directory unused for more than \a maxAgeDays, except
     * \a keepPath. Returns how many were deleted. Safe on any thread.
     */
    static int removeStaleDatabases( const QString &directory, const QString &keepPath, int maxAgeDays = STALE_DATABASE_DAYS );

    /**
     * Writes \a chunks (with their precomputed \a embeddings) to the SQLite store
     * and updates the in-memory cache. Existing rows are replaced according to
     * \a scope (and \a scopedLayerId for SingleLayer). Used by reindex(),
     * reindexLayers(), reindexLayer() and by tests. An empty \a workspaceRoot means the
     * current workspace.
     */
    bool persistChunks(
      const QList<Chunk> &chunks, const QList<QVector<float>> &embeddings, ReplaceScope scope, const QString &scopedLayerId, QString *errorMessage = nullptr, const QString &workspaceRoot = QString()
    );

    /**
     * Removes every chunk belonging to \a layerId from the cache at once, and from the
     * SQLite store in the background.
     */
    virtual bool removeLayer( const QString &layerId, QString *errorMessage = nullptr );

    /**
     * Returns the cached chunks (without embeddings), optionally filtered.
     *
     * - scope=All: every chunk regardless of source.
     * - scope=AllFiles: only source_type='file'.
     * - scope=AllLayers: only source_type='layer'.
     * - scope=SingleLayer: only source_type='layer' matching \a layerId.
     */
    QList<Chunk> chunks( ReplaceScope scope = ReplaceScope::All, const QString &layerId = QString() ) const;

    /**
     * Loads the on-disk SQLite store into the in-memory cache the first time
     * it is called, on the calling thread. Subsequent calls are no-ops. Prefer
     * requestLoad() on the interface thread.
     */
    bool ensureLoaded();

    //! Loads the cache in a background task if it is not loaded yet. Never blocks.
    void requestLoad();

    //! Closes the SQLite connection owned by the current thread, if one exists.
    void closeDatabaseConnectionForCurrentThread() const;

  signals:
    void progress( int current, int total, const QString &filePath );
    //! The cache finished loading in the background.
    void loaded();

  private slots:
    void onWorkspaceRootChanged();
    //! Asks the embedding provider, off the interface thread, to free a model left unused.
    void releaseIdleEmbeddingModel();

  private:
    struct CachedChunk
    {
        Chunk chunk;
        QVector<float> embedding;
        QString providerId;
        QString modelId;
        QString modelRevision;
        int embeddingDimension = 0;
        QString contentHash;
        qint64 sourceMTime = 0;
    };

    //! The provider in use, kept alive by the returned pointer even if it is replaced meanwhile.
    std::shared_ptr<QgsAiEmbeddingProvider> providerSnapshot() const;
    /**
     * Writes \a chunks in one transaction, replacing the rows of \a scope. With \a replacedFilePaths
     * and ReplaceScope::AllFiles, only the rows of those files are replaced. \a layerFingerprints
     * records the fingerprint of the layers written (see QgsAiLayerChunker::fingerprint()).
     */
    bool persistAll(
      const QList<CachedChunk> &chunks,
      ReplaceScope scope,
      const QString &scopedLayerId,
      const QString &workspaceRoot,
      const QString &databaseProviderId,
      QString *errorMessage,
      const QHash<QString, QString> &layerFingerprints = {},
      const QStringList *replacedFilePaths = nullptr
    );
    //! persistAll() and the matching cache update.
    bool storeChunks(
      const QList<CachedChunk> &built,
      ReplaceScope scope,
      const QString &scopedLayerId,
      const QString &workspaceRoot,
      const QString &databaseProviderId,
      const QHash<QString, QString> &layerFingerprints,
      QString *errorMessage
    );
    //! Records that \a layerIds were seen unchanged, which keeps their chunks from expiring.
    void touchLayers( const QStringList &layerIds, const QString &workspaceRoot, const QString &databaseProviderId );
    bool loadAll( QString *errorMessage );
    QString dbPath() const;
    QString dbPathForRoot( const QString &workspaceRoot ) const;
    static QString dbPathForRoot( const QString &workspaceRoot, const QString &providerId );
    QString connectionName() const;
    //! Deletes the queued layer removals, one transaction per database. Runs in the database pool.
    void flushLayerRemovals();
    //! Refreshes the snapshot returned by status(). Call with mMutex held after changing mCache.
    void updateStatusSnapshot();
    //! True if the cache holds \a workspaceRoot (or nothing loaded yet). Call with mMutex held.
    bool cacheHoldsRoot( const QString &workspaceRoot ) const;
    static QStringList chunkText( const QString &content );
    static bool isTextFile( const QString &relativePath );
    static float cosineSimilarity( const QVector<float> &a, const QVector<float> &b );

    QgsAiFileContextProvider *mContextProvider = nullptr;
    //! Copy of the context provider's root, readable from worker threads.
    QString mCurrentRoot;
    mutable QMutex mRootMutex;
    std::shared_ptr<QgsAiEmbeddingProvider> mEmbeddingProvider;
    mutable QMutex mProviderPointerMutex;
    //! Serializes cache loads; never taken by the interface thread.
    QMutex mLoadMutex;
    QList<CachedChunk> mCache;
    //! Fingerprint of each layer whose chunks are in mCache, from the layer_state table.
    QHash<QString, QString> mLayerFingerprints;
    //! See setActiveLayerIds().
    std::optional<QSet<QString>> mActiveLayerIds;
    mutable QMutex mActiveLayersMutex;
    //! Workspace whose chunks are in mCache.
    QString mCacheRoot;
    QDateTime mLastSync;
    bool mLoaded = false;
    //! Serializes embedding calls; held for one batch at a time.
    mutable QMutex mProviderUseMutex;
    //! Searches waiting for mProviderUseMutex: a reindex lets them in between batches.
    mutable std::atomic_int mSearchesWaiting { 0 };
    mutable QRecursiveMutex mMutex;
    mutable QMutex mStatusMutex;
    Status mStatus;
    QPointer<QgsTask> mLoadTask;
    QTimer mIdleReleaseTimer;
    //! Layers waiting to be deleted from each database by flushLayerRemovals().
    QHash<QString, QSet<QString>> mPendingLayerRemovals;
    bool mRemovalScheduled = false;
    QMutex mPendingRemovalsMutex;
};

#endif // QGSAIWORKSPACEINDEX_H
