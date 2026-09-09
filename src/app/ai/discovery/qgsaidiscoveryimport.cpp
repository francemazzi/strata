// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaidiscoveryimport.h"

#include <cmath>

#include "qgsdatasourceuri.h"
#include "qgsfeatureiterator.h"
#include "qgsproviderregistry.h"
#include "qgsprovidersublayerdetails.h"
#include "qgsrasterblock.h"
#include "qgsrasterdataprovider.h"
#include "qgsrasterlayer.h"
#include "qgsvectorlayer.h"

#include <QDir>
#include <QJsonArray>
#include <QString>

using namespace Qt::StringLiterals;
QgsAiDiscoveryImport::Result QgsAiDiscoveryImport::open( const QJsonObject &manifest, const QString &directory, const QgsCoordinateTransformContext &context, QThread *destinationThread )
{
  Result result;
  QJsonArray checks;
  int rejected = 0;
  for ( const auto &value : manifest.value( u"layers"_s ).toArray() )
  {
    const auto entry = value.toObject();
    const QString title = entry.value( u"title"_s ).toString();
    QList<QgsMapLayer *> layers;
    if ( entry.value( u"mode"_s ) == "file"_L1 )
    {
      const auto path = QDir( directory ).filePath( entry.value( u"path"_s ).toString() );
      auto options = QgsProviderSublayerDetails::LayerOptions( context );
      options.loadDefaultStyle = false;
      options.loadAllStoredStyle = false;
      for ( const auto &details : QgsProviderRegistry::instance()->querySublayers( path ) )
      {
        const auto names = entry.value( u"sublayers"_s ).toArray();
        if ( !names.isEmpty() && !names.contains( details.name() ) )
          continue;
        if ( details.providerKey() != "ogr"_L1 && details.providerKey() != "gdal"_L1 )
          continue;
        if ( auto layer = details.toLayer( options ) )
        {
          layer->setName( title + u" · "_s + details.name() );
          layers << layer;
        }
      }
    }
    else
    {
      QgsDataSourceUri uri;
      uri.setParam( u"url"_s, entry.value( u"url"_s ).toString() );
      const bool wfs = entry.value( u"protocol"_s ) == "WFS"_L1;
      if ( wfs )
      {
        uri.setParam( u"typename"_s, entry.value( u"typeName"_s ).toString() );
        uri.setParam( u"version"_s, u"auto"_s );
        QgsVectorLayer::LayerOptions options( context, false );
        options.skipCrsValidation = true;
        layers << new QgsVectorLayer( QString::fromUtf8( uri.encodedUri() ), title, u"WFS"_s, options );
      }
      else if ( entry.value( u"protocol"_s ) == "WMS"_L1 )
      {
        uri.setParam( u"layers"_s, entry.value( u"typeName"_s ).toString() );
        uri.setParam( u"styles"_s, QString() );
        uri.setParam( u"format"_s, u"image/png"_s );
        if ( !entry.value( u"nativeCrs"_s ).toString().isEmpty() )
          uri.setParam( u"crs"_s, entry.value( u"nativeCrs"_s ).toString() );
        QgsRasterLayer::LayerOptions options( false, context );
        options.skipCrsValidation = true;
        layers << new QgsRasterLayer( QString::fromUtf8( uri.encodedUri() ), title, u"wms"_s, options );
      }
    }
    if ( layers.isEmpty() )
    {
      ++rejected;
      checks.append( QJsonObject { { u"id"_s, entry.value( u"id"_s ) }, { u"passed"_s, false }, { u"reason"_s, u"Provider GIS non disponibile"_s } } );
    }
    for ( auto layer : layers )
    {
      const auto extent = layer->extent();
      const QString native = entry.value( u"nativeCrs"_s ).toString();
      bool valid = layer->isValid() && layer->crs().isValid() && extent.isFinite();
      QStringList issues;
      if ( !native.isEmpty() && QgsCoordinateReferenceSystem( native ).isValid() && layer->crs() != QgsCoordinateReferenceSystem( native ) )
      {
        valid = false;
        issues << u"CRS diverso dal manifest"_s;
      }
      qint64 features = -1;
      if ( auto vector = qobject_cast<QgsVectorLayer *>( layer ) )
      {
        // Remote counts may require an unbounded service query; retain unknown explicitly.
        if ( entry.value( u"mode"_s ) == "file"_L1 )
        {
          features = vector->featureCount();
          QgsFeature feature;
          auto iterator = vector->getFeatures( QgsFeatureRequest().setLimit( 1 ) );
          if ( features > 0 && !iterator.nextFeature( feature ) )
            valid = false;
          if ( layers.size() == 1 && entry.contains( u"featureCount"_s ) && features != entry.value( u"featureCount"_s ).toInteger() )
          {
            valid = false;
            issues << u"Conteggio diverso dal manifest"_s;
          }
        }
      }
      if ( auto raster = qobject_cast<QgsRasterLayer *>( layer ) )
      {
        valid = valid && raster->bandCount() > 0;
        if ( valid )
        {
          std::unique_ptr<QgsRasterBlock> sample( raster->dataProvider()->block( 1, extent, 1, 1 ) );
          valid = sample && sample->isValid();
        }
      }
      checks.append(
        QJsonObject {
          { u"id"_s, entry.value( u"id"_s ) },
          { u"passed"_s, valid },
          { u"nativeCrs"_s, layer->crs().authid() },
          { u"extent"_s, QJsonArray { extent.xMinimum(), extent.yMinimum(), extent.xMaximum(), extent.yMaximum() } },
          { u"featureCount"_s, features },
          { u"featureCountKnown"_s, features >= 0 },
          { u"issues"_s, QJsonArray::fromStringList( issues ) }
        }
      );
      if ( !valid )
      {
        ++rejected;
        delete layer;
        continue;
      }
      layer->setCustomProperty( u"strata/discovery/layerId"_s, entry.value( u"id"_s ).toString() );
      layer->moveToThread( destinationThread );
      result.layers << layer;
    }
  }
  result.outcome = {
    { u"status"_s,
      result.layers.isEmpty() ? u"FAILED"_s
      : rejected              ? u"PARTIAL"_s
                              : u"SUCCEEDED"_s },
    { u"checks"_s, checks },
    { u"importedLayers"_s, result.layers.size() },
    { u"rejectedLayers"_s, rejected },
    { u"reprojectionPerformed"_s, false },
    { u"rasterClipPerformed"_s, false },
    { u"coverage"_s, manifest.value( u"coverage"_s ) }
  };
  return result;
}
