/***************************************************************************
    qgsaiembeddingprovider.h
    ------------------------
    begin                : June 2026
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

#ifndef QGSAIEMBEDDINGPROVIDER_H
#define QGSAIEMBEDDINGPROVIDER_H

#include <atomic>
#include <memory>

#include "qgis_app.h"

#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

using namespace Qt::StringLiterals;

class QObject;

enum class QgsAiEmbeddingRole
{
  Query,
  Passage,
};

class QgsFeedback;

struct QgsAiEmbeddingOptions
{
    int maxBatch = 64;
    //! Optional cancellation hook: providers doing network requests abort when it is cancelled.
    QgsFeedback *feedback = nullptr;
};

struct APP_EXPORT QgsAiEmbeddingModelDownloadFile
{
    QString relativePath;
    QString url;
    QString sha256;
    qint64 size = 0;
};

struct APP_EXPORT QgsAiEmbeddingProviderUiEntry
{
    QString providerId;
    QString displayName;
    bool selectable = true;
    QString unavailableReason;
};

/**
 * Local embedding provider interface used by the workspace index.
 *
 * Implementations must not call external APIs unless the user has explicitly
 * selected a remote provider. The default product path is local/on-device.
 */
class APP_EXPORT QgsAiEmbeddingProvider
{
  public:
    virtual ~QgsAiEmbeddingProvider() = default;

    virtual QString providerId() const = 0;
    virtual QString displayName() const = 0;
    virtual QString modelId() const { return providerId(); }
    virtual QString modelRevision() const { return QString(); }
    virtual int embeddingDimension() const { return 0; }
    virtual bool isRemote() const { return false; }
    virtual bool isAvailable( QString *errorMessage = nullptr ) const = 0;
    virtual bool embed( const QStringList &texts, QList<QVector<float>> &out, QString *errorMessage = nullptr, int maxBatch = 64 ) = 0;
    virtual bool embed( const QStringList &texts, QgsAiEmbeddingRole role, QList<QVector<float>> &out, QString *errorMessage = nullptr, const QgsAiEmbeddingOptions &options = QgsAiEmbeddingOptions() )
    {
      Q_UNUSED( role )
      return embed( texts, out, errorMessage, options.maxBatch );
    }

    /**
     * Frees what the provider keeps loaded (a local model) if it has not embedded anything for
     * \a idleMs. The next embed() loads it again. Never waits: skipped while embedding.
     */
    virtual void releaseIdleResources( qint64 idleMs ) { Q_UNUSED( idleMs ) }

    //! Longest input the model reads, in tokens: the rest of a longer text is lost. 0 when there is no practical limit.
    virtual int maxInputTokens() const { return 0; }

    //! Tokens of \a text for this model, or -1 when unknown. Safe on any thread; never loads the model.
    virtual int tokenCount( const QString &text ) const
    {
      Q_UNUSED( text )
      return -1;
    }
};

/**
 * Placeholder provider used until a local embedding model is packaged.
 */
class APP_EXPORT QgsAiUnavailableLocalEmbeddingProvider final : public QgsAiEmbeddingProvider
{
  public:
    QString providerId() const override { return u"local"_s; }
    QString displayName() const override { return u"Local embedding model"_s; }
    QString modelId() const override { return u"unavailable"_s; }

    bool isAvailable( QString *errorMessage = nullptr ) const override
    {
      if ( errorMessage )
        *errorMessage = u"Local embedding model is not installed."_s;
      return false;
    }

    bool embed( const QStringList &texts, QList<QVector<float>> &out, QString *errorMessage = nullptr, int maxBatch = 64 ) override
    {
      Q_UNUSED( texts )
      Q_UNUSED( maxBatch )
      out.clear();
      if ( errorMessage )
        *errorMessage = u"Local embedding model is not installed."_s;
      return false;
    }
};

/**
 * Zero-setup local embedding provider.
 *
 * This is intentionally small and CPU-only. It is NOT an E5 model: it provides
 * deterministic 384 dimensional embeddings from lexical tokens/ngrams (a MinHash
 * style embedder). It remains available as an explicit fallback when the ONNX E5
 * model is not installed or cannot run on a machine.
 */
class APP_EXPORT QgsAiLocalEmbeddingProvider final : public QgsAiEmbeddingProvider
{
  public:
    QString providerId() const override { return u"local:minihash-384"_s; }
    QString displayName() const override { return u"Local MinHash fallback"_s; }
    QString modelId() const override { return u"strata-local-minihash-384"_s; }
    QString modelRevision() const override { return u"2026-06-09"_s; }
    int embeddingDimension() const override { return 384; }
    bool isAvailable( QString *errorMessage = nullptr ) const override;
    bool embed( const QStringList &texts, QList<QVector<float>> &out, QString *errorMessage = nullptr, int maxBatch = 64 ) override;
    bool embed( const QStringList &texts, QgsAiEmbeddingRole role, QList<QVector<float>> &out, QString *errorMessage = nullptr, const QgsAiEmbeddingOptions &options = QgsAiEmbeddingOptions() ) override;

  private:
    QVector<float> embedOne( const QString &text, QgsAiEmbeddingRole role ) const;
};

class APP_EXPORT QgsAiE5EmbeddingProvider final : public QgsAiEmbeddingProvider
{
  public:
    QgsAiE5EmbeddingProvider();
    ~QgsAiE5EmbeddingProvider() override;

    static QString staticProviderId();
    static QString modelName();
    static QString pinnedModelRevision();
    static QString developerModelDirectory();
    static QString userModelDirectory();
    static QString packagedModelDirectory();
    static QString activeModelDirectory();
    static QString modelPath( const QString &modelDirectory );
    static QString tokenizerPath( const QString &modelDirectory );
    static bool modelFilesAvailable( const QString &modelDirectory, QString *errorMessage = nullptr );
    static QList<QgsAiEmbeddingModelDownloadFile> downloadFiles();
    static qint64 downloadSize();
    static QString formatInputForRole( const QString &text, QgsAiEmbeddingRole role );
    static QVector<qint64> tokenIdsWithSpecials( const QVector<int> &pieceIds, int maxSequenceLength = 512 );
    static QVector<float> meanPoolAndNormalize( const QVector<float> &lastHiddenStates, const QVector<qint64> &attentionMask, int hiddenSize );
    static QString fileSha256( const QString &path, QString *errorMessage = nullptr );
    static bool fileMatchesSha256( const QString &path, const QString &expectedSha256, QString *errorMessage = nullptr );

    QString providerId() const override { return staticProviderId(); }
    QString displayName() const override { return u"Local multilingual E5 small (recommended)"_s; }
    QString modelId() const override { return modelName(); }
    QString modelRevision() const override { return pinnedModelRevision(); }
    int embeddingDimension() const override { return 384; }

    /**
     * True when the model files are installed and a previous load did not fail.
     *
     * Never loads the model: that happens on the first embed(), on the thread that calls it
     * (a background task), so checking availability from the interface stays instant.
     */
    bool isAvailable( QString *errorMessage = nullptr ) const override;
    bool embed( const QStringList &texts, QList<QVector<float>> &out, QString *errorMessage = nullptr, int maxBatch = 64 ) override;
    bool embed( const QStringList &texts, QgsAiEmbeddingRole role, QList<QVector<float>> &out, QString *errorMessage = nullptr, const QgsAiEmbeddingOptions &options = QgsAiEmbeddingOptions() ) override;

    //! True once the ONNX session is loaded.
    bool runtimeLoaded() const { return mRuntimeReady; }

    //! Unloads the ONNX session and its roughly 300 MB when unused for \a idleMs.
    void releaseIdleResources( qint64 idleMs ) override;

    int maxInputTokens() const override;
    //! Counts with the model's SentencePiece tokenizer, loaded apart from the model (5 MB).
    int tokenCount( const QString &text ) const override;

    //! Tokens a batch may hold, padding included: eight texts of the maximum length.
    static constexpr int BATCH_TOKEN_BUDGET = 4096;

    /**
     * Groups texts of \a tokenCounts tokens into batches of at most \a maxBatch texts, shortest
     * first, so that a batch padded to its longest text stays within \a tokenBudget tokens
     * (a single longer text makes its own batch). Returns the indexes of each batch.
     */
    static QList<QList<int>> planBatches( const QVector<int> &tokenCounts, int maxBatch, int tokenBudget = BATCH_TOKEN_BUDGET );

  private:
    struct Runtime;
    struct CountingTokenizer;

    bool ensureRuntime( QString *errorMessage = nullptr ) const;
    //! ensureRuntime() with mRuntimeMutex already held.
    bool ensureRuntimeLocked( QString *errorMessage ) const;
    //! Records a load failure so isAvailable() reports it without touching the runtime lock.
    void setLoadFailure( const QString &error ) const;

    mutable QMutex mRuntimeMutex;
    mutable std::unique_ptr<Runtime> mRuntime;
    mutable QString mRuntimeError;
    mutable bool mRuntimeLoadAttempted = false;
    //! Read by isAvailable() without mRuntimeMutex, which embed() holds for a whole batch.
    mutable std::atomic_bool mRuntimeReady { false };
    mutable std::atomic_bool mRuntimeFailed { false };
    //! When the runtime last embedded something, in milliseconds since the epoch.
    std::atomic<qint64> mLastUseMs { 0 };
    //! Tokenizer for tokenCount(), apart from mRuntime so counting never waits for an embedding batch.
    mutable QMutex mCountingTokenizerMutex;
    mutable std::unique_ptr<CountingTokenizer> mCountingTokenizer;
    mutable bool mCountingTokenizerFailed = false;
    mutable QMutex mLoadFailureMutex;
    mutable QString mLoadFailure;
};

class APP_EXPORT QgsAiEmbeddingProviderRegistry
{
  public:
    static QString defaultProviderId();
    static QString configuredProviderId();
    static void setConfiguredProviderId( const QString &providerId );
    static QList<QgsAiEmbeddingProviderUiEntry> providerUiEntries();
    static QStringList providerIds();
    static QString displayNameForProviderId( const QString &providerId );
    static bool isRemoteProviderId( const QString &providerId );
    static std::unique_ptr<QgsAiEmbeddingProvider> createProviderFromSettings( QObject *parent = nullptr );
    static std::unique_ptr<QgsAiEmbeddingProvider> createProvider( const QString &providerId, QObject *parent = nullptr );
};

#endif // QGSAIEMBEDDINGPROVIDER_H
