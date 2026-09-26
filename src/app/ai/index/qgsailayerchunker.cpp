/***************************************************************************
    qgsailayerchunker.cpp
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

#include "qgsailayerchunker.h"

#include <algorithm>

#include "qgsfeature.h"
#include "qgsfeatureiterator.h"
#include "qgsfeaturerequest.h"
#include "qgsfeedback.h"
#include "qgsfields.h"
#include "qgsgeometry.h"
#include "qgsmaplayer.h"
#include "qgsproviderregistry.h"
#include "qgsrasterdataprovider.h"
#include "qgsrasterlayer.h"
#include "qgsrectangle.h"
#include "qgssettings.h"
#include "qgsvectorlayer.h"
#include "qgsvectorlayerfeatureiterator.h"
#include "qgswkbtypes.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace
{
  constexpr int MAX_VECTOR_FEATURE_SAMPLE = 200;
  //! Bump when the chunk text changes, so every layer is chunked again.
  constexpr const char *LAYER_CHUNK_FORMAT_VERSION = "layer-chunks-2";
  constexpr int MAX_VECTOR_CHUNKS = 20;

  QString fieldsSummary( const QgsFields &fields )
  {
    QStringList out;
    out.reserve( fields.size() );
    for ( const QgsField &f : fields )
      out << u"%1:%2"_s.arg( f.name(), f.typeName() );
    return out.join( ", "_L1 );
  }

  QString serializeFeatureLine( const QgsFeature &feature, const QgsFields &fields, const QString &geometryType )
  {
    QStringList kv;
    kv.reserve( fields.size() );
    for ( const QgsField &f : fields )
    {
      const QVariant val = feature.attribute( f.name() );
      const QString rendered = val.isValid() && !val.isNull() ? val.toString() : QString();
      kv << u"%1=%2"_s.arg( f.name(), rendered );
    }

    QString bbox = u"NULL"_s;
    const QgsGeometry geom = feature.geometry();
    if ( !geom.isNull() )
    {
      const QgsRectangle r = geom.boundingBox();
      bbox = u"(%1,%2,%3,%4)"_s.arg( r.xMinimum() ).arg( r.yMinimum() ).arg( r.xMaximum() ).arg( r.yMaximum() );
    }

    return u"[%1] %2; bbox=%3; type=%4"_s.arg( feature.id() ).arg( kv.join( "; "_L1 ), bbox, geometryType );
  }

  QString rasterMetadataText( QgsRasterLayer *layer )
  {
    QString out;
    out += u"Raster layer '%1' (id=%2)\n"_s.arg( layer->name(), layer->id() );
    out += u"crs=%1; size=%2x%3; bands=%4\n"_s.arg( layer->crs().authid() ).arg( layer->width() ).arg( layer->height() ).arg( layer->bandCount() );

    const QgsRectangle ext = layer->extent();
    out += u"extent=(%1,%2,%3,%4)\n"_s.arg( ext.xMinimum() ).arg( ext.yMinimum() ).arg( ext.xMaximum() ).arg( ext.yMaximum() );

    QgsRasterDataProvider *dp = layer->dataProvider();
    if ( !dp )
      return out + "(no data provider)\n"_L1;

    out += "(band statistics skipped during fast layer snapshot)\n"_L1;
    return out;
  }

  QString sourceWithoutLayerOptions( QString source )
  {
    const int pipeIndex = source.indexOf( '|' );
    if ( pipeIndex >= 0 )
      source.truncate( pipeIndex );
    return source.trimmed();
  }

  bool isOfficeSpreadsheetSource( const QString &source )
  {
    const QString path = sourceWithoutLayerOptions( source ).toLower();
    static const QStringList extensions {
      u".ods"_s,
      u".fods"_s,
      u".xls"_s,
      u".xlsx"_s,
      u".xlsm"_s,
      u".xlsb"_s,
    };

    for ( const QString &extension : extensions )
    {
      if ( path.endsWith( extension ) )
        return true;
    }
    return false;
  }

  bool isOfficeSpreadsheetVectorLayer( QgsVectorLayer *layer )
  {
    return layer && layer->providerType().compare( u"ogr"_s, Qt::CaseInsensitive ) == 0 && isOfficeSpreadsheetSource( layer->source() );
  }

  QString metadataOnlyVectorText( QgsVectorLayer *layer, const QString &reason )
  {
    return u"Vector layer '%1' (id=%2, provider=%3)\n"
           u"feature_count=unknown; sampled_feature_limit=0; chunk_limit=1\n"
           u"feature sampling skipped: %4\n"_s.arg( layer->name(), layer->id(), layer->providerType(), reason );
  }

  QgsAiWorkspaceIndex::Chunk layerChunk( const QgsAiPreparedLayer &prepared, int chunkIndex, const QString &text )
  {
    QgsAiWorkspaceIndex::Chunk c;
    c.sourceType = QString::fromLatin1( QgsAiWorkspaceIndex::SOURCE_TYPE_LAYER );
    c.relativePath = prepared.name;
    c.layerId = prepared.layerId;
    c.chunkIndex = chunkIndex;
    c.text = text;
    return c;
  }
} // namespace

bool QgsAiLayerChunker::isRemoteLayer( const QgsMapLayer *layer )
{
  if ( !layer )
    return false;

  static const QSet<QString> remoteProviders {
    u"postgres"_s,
    u"wfs"_s,
    u"oapif"_s,
    u"arcgisfeatureserver"_s,
    u"arcgismapserver"_s,
    u"afs"_s,
    u"mssql"_s,
    u"oracle"_s,
    u"hana"_s,
    u"wms"_s,
    u"wcs"_s,
    u"xyzvectortiles"_s,
    u"vectortile"_s,
    u"sensorthings"_s,
  };
  if ( remoteProviders.contains( layer->providerType().toLower() ) )
    return true;

  const QString source = layer->source().trimmed().toLower();
  return source.startsWith( "http://"_L1 )
         || source.startsWith( "https://"_L1 )
         || source.contains( "/vsicurl"_L1 )
         || source.contains( "/vsis3"_L1 )
         || source.contains( "/vsigs"_L1 )
         || source.contains( "/vsiaz"_L1 )
         || source.contains( "/vsiadls"_L1 );
}

QgsAiPreparedLayer QgsAiLayerChunker::prepare( QgsMapLayer *layer )
{
  QgsAiPreparedLayer prepared = prepareUnfingerprinted( layer );
  if ( prepared.layerId.isEmpty() )
    return prepared;

  // The data source is hashed, never stored: it can hold database credentials.
  const QByteArray sourceHash = QCryptographicHash::hash( layer->source().toUtf8(), QCryptographicHash::Sha1 ).toHex();
  QStringList parts { QString::fromLatin1( LAYER_CHUNK_FORMAT_VERSION ), prepared.name, prepared.providerType, prepared.crsAuthId, QString::fromLatin1( sourceHash ), prepared.metadataText };
  if ( prepared.source )
  {
    const QgsVectorLayer *vector = qobject_cast<const QgsVectorLayer *>( layer );
    parts << prepared.geometryType << fieldsSummary( prepared.fields ) << prepared.extent.toString( 6 ) << ( prepared.includeWkt ? u"wkt"_s : u"nowkt"_s );
    if ( vector )
      parts << vector->subsetString() << QString::number( vector->featureCount() );
    const QVariantMap uriParts = QgsProviderRegistry::instance()->decodeUri( prepared.providerType, layer->source() );
    const QString path = uriParts.value( u"path"_s ).toString();
    // Only the path string is checked here: the file itself is read by fingerprint(), off the interface thread.
    if ( !path.isEmpty() && QFileInfo( path ).isAbsolute() )
      prepared.sourceFilePath = path;
  }
  prepared.fingerprintBase = parts.join( QChar( 0x1f ) );
  return prepared;
}

QString QgsAiLayerChunker::fingerprint( const QgsAiPreparedLayer &prepared )
{
  if ( prepared.fingerprintBase.isEmpty() )
    return QString();
  // A feature source without a local file (memory, database) can change without notice.
  if ( prepared.source && prepared.sourceFilePath.isEmpty() )
    return QString();

  QCryptographicHash hash( QCryptographicHash::Sha1 );
  hash.addData( prepared.fingerprintBase.toUtf8() );
  if ( prepared.source )
  {
    const QFileInfo main( prepared.sourceFilePath );
    if ( !main.exists() )
      return QString();
    // Editing attributes of a shapefile writes only its .dbf, and a GeoPackage writes its -wal first.
    QStringList files { main.absoluteFilePath(), main.absoluteFilePath() + u"-wal"_s };
    const QString suffix = main.suffix().toLower();
    if ( suffix == "shp"_L1 )
    {
      for ( const QString &sidecar : { u"dbf"_s, u"shx"_s, u"prj"_s, u"cpg"_s } )
        files << main.absoluteDir().filePath( main.completeBaseName() + '.' + sidecar );
    }
    for ( const QString &file : std::as_const( files ) )
    {
      const QFileInfo info( file );
      if ( info.exists() )
        hash.addData( u"%1|%2|%3;"_s.arg( info.fileName() ).arg( info.size() ).arg( info.lastModified().toMSecsSinceEpoch() ).toUtf8() );
    }
  }
  return QString::fromLatin1( hash.result().toHex() );
}

QgsAiPreparedLayer QgsAiLayerChunker::prepareUnfingerprinted( QgsMapLayer *layer )
{
  QgsAiPreparedLayer prepared;
  if ( !layer )
    return prepared;

  prepared.layerId = layer->id();
  prepared.name = layer->name();
  prepared.providerType = layer->providerType();
  prepared.crsAuthId = layer->crs().authid();

  if ( QgsRasterLayer *raster = qobject_cast<QgsRasterLayer *>( layer ) )
  {
    // Raster metadata comes from the provider's capabilities: no pixel is read.
    prepared.metadataText = rasterMetadataText( raster );
    return prepared;
  }

  QgsVectorLayer *vector = qobject_cast<QgsVectorLayer *>( layer );
  if ( !vector )
  {
    prepared.metadataText = u"Layer '%1' (id=%2, provider=%3)\n"_s.arg( layer->name(), layer->id(), layer->providerType() );
    return prepared;
  }

  if ( isOfficeSpreadsheetVectorLayer( vector ) )
  {
    prepared.metadataText = metadataOnlyVectorText( vector, u"Office spreadsheet layers can require GDAL to parse large repeated-cell ranges."_s );
    return prepared;
  }

  const QgsSettings settings;
  if ( isRemoteLayer( vector ) && !settings.value( u"strata/index/include_remote_layers"_s, false ).toBool() )
  {
    // Reading a remote service would mean network traffic for every re-index, and its extent
    // can trigger a request too. Enable strata/index/include_remote_layers to sample it anyway.
    prepared.metadataText = metadataOnlyVectorText( vector, u"remote layer; enable strata/index/include_remote_layers to sample its features."_s );
    return prepared;
  }

  prepared.geometryType = QgsWkbTypes::geometryDisplayString( vector->geometryType() );
  prepared.fields = vector->fields();
  // Local providers know their extent from the file header or keep it cached.
  prepared.extent = vector->extent();
  prepared.extentKnown = true;
  prepared.includeWkt = settings.value( u"strata/privacy/include_layer_wkt_in_model_context"_s, false ).toBool();
  prepared.source = std::make_shared<QgsVectorLayerFeatureSource>( vector );
  return prepared;
}

QList<QgsAiWorkspaceIndex::Chunk> QgsAiLayerChunker::chunk( const QgsAiPreparedLayer &prepared, QgsFeedback *feedback, const QgsAiWorkspaceIndex::TokenCounter &tokenCount, int maxTokens )
{
  QList<QgsAiWorkspaceIndex::Chunk> chunks;
  if ( prepared.layerId.isEmpty() )
    return chunks;

  const bool byTokens = tokenCount && maxTokens > 0;
  if ( !prepared.metadataText.isEmpty() || !prepared.source )
  {
    chunks.append( layerChunk( prepared, 0, byTokens ? QgsAiWorkspaceIndex::truncateToTokens( prepared.metadataText, tokenCount, maxTokens ) : prepared.metadataText ) );
    return chunks;
  }

  const QString extentText = prepared.extentKnown
                               ? u"(%1,%2,%3,%4)"_s.arg( prepared.extent.xMinimum() ).arg( prepared.extent.yMinimum() ).arg( prepared.extent.xMaximum() ).arg( prepared.extent.yMaximum() )
                               : u"unknown"_s;
  const QString header = u"Vector layer '%1' (id=%2, crs=%3, geometry=%4)\nfeature_count=unknown; sampled_feature_limit=%5; chunk_limit=%6\nextent=%7\nfields=%8\n"_s
                           .arg( prepared.name, prepared.layerId, prepared.crsAuthId, prepared.geometryType )
                           .arg( MAX_VECTOR_FEATURE_SAMPLE )
                           .arg( MAX_VECTOR_CHUNKS )
                           .arg( extentText, fieldsSummary( prepared.fields ) );

  const auto countTokens = [&tokenCount]( const QString &text ) {
    const int count = tokenCount( text );
    return count < 0 ? static_cast<int>( text.size() ) : count;
  };
  const int headerTokens = byTokens ? countTokens( header ) : 0;
  QString currentText = header;
  int currentTokens = headerTokens;
  QByteArray currentWkts;
  qint64 firstFid = -1;
  qint64 lastFid = -1;
  int chunkIndex = 0;

  auto flush = [&]() {
    if ( firstFid < 0 )
      return;
    if ( chunks.size() >= MAX_VECTOR_CHUNKS )
      return;
    QgsAiWorkspaceIndex::Chunk c = layerChunk( prepared, chunkIndex++, currentText );
    c.firstFeatureId = firstFid;
    c.lastFeatureId = lastFid;
    if ( !currentWkts.isEmpty() )
      c.wktBlob = qCompress( currentWkts );
    chunks.append( c );

    currentText = header;
    currentTokens = headerTokens;
    currentWkts.clear();
    firstFid = -1;
    lastFid = -1;
  };

  QgsFeatureRequest request;
  request.setLimit( MAX_VECTOR_FEATURE_SAMPLE );
  if ( feedback )
    request.setFeedback( feedback );
  QgsFeatureIterator it = prepared.source->getFeatures( request );
  QgsFeature feature;
  int sampledFeatures = 0;
  while ( sampledFeatures < MAX_VECTOR_FEATURE_SAMPLE && chunks.size() < MAX_VECTOR_CHUNKS && it.nextFeature( feature ) )
  {
    if ( feedback && feedback->isCanceled() )
      return chunks;

    QString line = serializeFeatureLine( feature, prepared.fields, prepared.geometryType );
    int lineTokens = 0;
    if ( byTokens )
    {
      // A feature with many attributes may not fit a chunk even alone: keep its start.
      line = QgsAiWorkspaceIndex::truncateToTokens( line, tokenCount, std::max( 1, maxTokens - headerTokens - 1 ) );
      lineTokens = countTokens( line ) + 1;
    }

    // Flush before adding if appending would exceed the target *and* the chunk
    // already has at least one feature (avoid empty/header-only chunks).
    const bool full = byTokens ? currentTokens + lineTokens > maxTokens : currentText.size() + line.size() + 1 > QgsAiWorkspaceIndex::CHUNK_TARGET_CHARS;
    if ( firstFid >= 0 && full )
    {
      flush();
      if ( chunks.size() >= MAX_VECTOR_CHUNKS )
        break;
    }

    if ( firstFid < 0 )
      firstFid = feature.id();
    lastFid = feature.id();

    currentText += line;
    currentText += '\n';
    currentTokens += lineTokens;

    if ( prepared.includeWkt )
    {
      const QgsGeometry geom = feature.geometry();
      if ( !currentWkts.isEmpty() )
        currentWkts.append( '\n' );
      currentWkts.append( geom.isNull() ? QByteArray( "NULL" ) : geom.asWkt().toUtf8() );
    }
    ++sampledFeatures;
  }
  flush();

  if ( chunks.isEmpty() )
    chunks.append( layerChunk( prepared, 0, header + u"(no sampled features)\n"_s ) );

  return chunks;
}

QList<QgsAiWorkspaceIndex::Chunk> QgsAiLayerChunker::chunkVector( QgsVectorLayer *layer )
{
  if ( !layer )
    return {};
  return chunk( prepare( layer ) );
}

QList<QgsAiWorkspaceIndex::Chunk> QgsAiLayerChunker::chunkRaster( QgsRasterLayer *layer )
{
  if ( !layer )
    return {};
  return chunk( prepare( layer ) );
}
