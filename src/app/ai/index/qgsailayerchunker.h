/***************************************************************************
    qgsailayerchunker.h
    ---------------------
    begin                : May 2026
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

#ifndef QGSAILAYERCHUNKER_H
#define QGSAILAYERCHUNKER_H

#include <memory>

#include "qgis_app.h"
#include "qgsaiworkspaceindex.h"
#include "qgsfields.h"
#include "qgsrectangle.h"

#include <QList>
#include <QString>

class QgsAbstractFeatureSource;
class QgsFeedback;
class QgsMapLayer;
class QgsRasterLayer;
class QgsVectorLayer;

/**
 * Everything the chunker needs from a map layer, captured on the interface thread.
 *
 * Only cheap metadata and a feature source snapshot are read there. The features themselves
 * are read by QgsAiLayerChunker::chunk() on a worker thread.
 */
struct APP_EXPORT QgsAiPreparedLayer
{
    QString layerId;
    QString name;
    QString providerType;
    //! When not empty, the layer is indexed from this text alone and no feature is read.
    QString metadataText;
    QString crsAuthId;
    QString geometryType;
    QgsFields fields;
    bool extentKnown = false;
    QgsRectangle extent;
    //! Collect the WKT of sampled features (only when the privacy setting allows it).
    bool includeWkt = false;
    //! Feature source snapshot, null for metadata-only layers.
    std::shared_ptr<QgsAbstractFeatureSource> source;
    //! What prepare() read that shapes the chunks; QgsAiLayerChunker::fingerprint() adds the files.
    QString fingerprintBase;
    //! Local file the features come from. Empty for memory, database and service layers.
    QString sourceFilePath;
};

/**
 * Builds RAG chunks from QGIS map layers, ready to be embedded and stored
 * by QgsAiWorkspaceIndex.
 *
 * Vector layers: features are packed into auto-sized chunks driven by
 * QgsAiWorkspaceIndex::CHUNK_TARGET_CHARS. The text fed to the embedding
 * model is **semantic** — attribute key/value pairs plus the bounding box
 * and geometry type of each feature. When the privacy setting
 * `strata/privacy/include_layer_wkt_in_model_context` is on, the WKT of every
 * feature in a chunk is collected separately and saved (gzipped) in `wktBlob`
 * so that retrieval can return the precise geometries to the LLM.
 *
 * Layers from remote providers (PostGIS, WFS, OGC API, ArcGIS, …) are indexed
 * from their metadata only, unless `strata/index/include_remote_layers` is on.
 *
 * Raster layers: a single chunk per layer carrying metadata. Pixel sampling is
 * intentionally skipped (low semantic value for similarity search).
 */
class APP_EXPORT QgsAiLayerChunker
{
  public:
    //! Captures what chunk() needs from \a layer. Cheap; call it on the interface thread.
    static QgsAiPreparedLayer prepare( QgsMapLayer *layer );

    /**
     * Builds the chunks of a prepared layer. Safe on any thread: it only reads the prepared
     * data and its feature source. Returns what was built so far when \a feedback is canceled.
     * With \a tokenCount, each chunk holds at most \a maxTokens tokens; otherwise chunks are
     * sized in characters (QgsAiWorkspaceIndex::CHUNK_TARGET_CHARS).
     */
    static QList<QgsAiWorkspaceIndex::Chunk> chunk( const QgsAiPreparedLayer &prepared, QgsFeedback *feedback = nullptr, const QgsAiWorkspaceIndex::TokenCounter &tokenCount = {}, int maxTokens = 0 );

    //! True if \a layer reads its data from a remote service or database.
    static bool isRemoteLayer( const QgsMapLayer *layer );

    /**
     * Fingerprint of a prepared layer: while it stays the same, chunk() builds the same chunks.
     * It covers the modification time and size of the layer's files, sidecars included (.dbf,
     * .gpkg-wal…), so it reads the file system: call it on a worker thread. Empty when changes
     * cannot be detected without reading the features (memory and database layers).
     */
    static QString fingerprint( const QgsAiPreparedLayer &prepared );

    //! Convenience for prepare() followed by chunk(), on the calling thread.
    static QList<QgsAiWorkspaceIndex::Chunk> chunkVector( QgsVectorLayer *layer );
    //! Convenience for prepare() followed by chunk(), on the calling thread.
    static QList<QgsAiWorkspaceIndex::Chunk> chunkRaster( QgsRasterLayer *layer );

  private:
    //! prepare() without the fingerprint.
    static QgsAiPreparedLayer prepareUnfingerprinted( QgsMapLayer *layer );
};

#endif // QGSAILAYERCHUNKER_H
