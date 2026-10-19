/***************************************************************************
    qgsailayertools.cpp
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

#include "qgsaicrsutils.h"
#include "qgsailayertools.h"
#include "qgsaigeographicverification.h"
#include "qgsaiarcgissnapshot.h"
#include "qgsdatasourceuri.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

#include "qgis.h"
#include "qgsaifilecontextprovider.h"
#include "qgsaitaskrunner.h"
#include "qgsaitoolschemautil.h"
#include "qgsaivisualcontextutils.h"
#include "qgsapplication.h"
#include "qgscategorizedsymbolrenderer.h"
#include "qgsclassificationcustom.h"
#include "qgsclassificationmethod.h"
#include "qgsclassificationmethodregistry.h"
#include "qgscolorrampimpl.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsexception.h"
#include "qgsfeature.h"
#include "qgsfeatureiterator.h"
#include "qgsfeaturerequest.h"
#include "qgsfeedback.h"
#include "qgsfields.h"
#include "qgsfileutils.h"
#include "qgsgraduatedsymbolrenderer.h"
#include "qgslayertree.h"
#include "qgslayertreelayer.h"
#include "qgslayertreeregistrybridge.h"
#include "qgslayoutexporter.h"
#include "qgslayoutitemlabel.h"
#include "qgslayoutitemgroup.h"
#include "qgslayoutitemlegend.h"
#include "qgslayoutitemmap.h"
#include "qgslayoutitempage.h"
#include "qgslayoutitempicture.h"
#include "qgslayoutitemscalebar.h"
#include "qgslayoutmanager.h"
#include "qgslayoutpagecollection.h"
#include "qgslayoutpoint.h"
#include "qgslayoutsize.h"
#include "qgsmapcanvas.h"
#include "qgsmaplayer.h"
#include "qgsmaplayerfactory.h"
#include "qgsmaplayerstyle.h"
#include "qgsmaprendererparalleljob.h"
#include "qgsmapsettings.h"
#include "qgsmasterlayoutinterface.h"
#include "qgsmessagelog.h"
#include "qgspallabeling.h"
#include "qgsprintlayout.h"
#include "qgsprocessing.h"
#include "qgsprocessingalgorithm.h"
#include "qgsprocessingcontext.h"
#include "qgsprocessingfeedback.h"
#include "qgsprocessingoutputs.h"
#include "qgsprocessingparameters.h"
#include "qgsprocessingregistry.h"
#include "qgsprocessingutils.h"
#include "qgsproject.h"
#include "qgsprojectstylesettings.h"
#include "qgsproperty.h"
#include "qgsprovidermetadata.h"
#include "qgsproviderregistry.h"
#include "qgsrasterlayer.h"
#include "qgsrasterrenderer.h"
#include "qgsrectangle.h"
#include "qgsrenderer.h"
#include "qgssinglesymbolrenderer.h"
#include "qgsstyle.h"
#include "qgssymbol.h"
#include "qgstaskmanager.h"
#include "qgsvectorlayer.h"
#include "qgsvectorlayerfeatureiterator.h"
#include "qgstextformat.h"
#include "qgsvectorlayerlabeling.h"
#include "qgsvariantutils.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVariant>

using namespace Qt::StringLiterals;

namespace
{
  enum class RollbackType
  {
    RemoveLayer,
    RemoveLoadedLayers,
    RestoreLayerStyle,
    RemoveLayout,
    RestoreLayoutEdit,
    DeleteFile
  };

  struct RollbackEntry
  {
      RollbackType type = RollbackType::DeleteFile;
      QString layerId;
      QString styleXml;
      bool hasVisibility = false;
      bool visible = true;
      QString layoutName;
      QStringList itemUuids;
      int pageCountBefore = 0;
      //! Map item resized by edit_print_layout to make room for legend/scale bar/north arrow.
      QString mapUuid;
      QRectF mapRect;
      QgsRectangle mapExtent;
      //! Note and legend that edit_print_layout changed in place.
      QString noteUuid;
      QString noteText;
      QRectF noteRect;
      QString legendUuid;
      Qgis::LegendSyncMode legendModeBefore = Qgis::LegendSyncMode::Manual;
      QVariantMap legendLabelsBefore;
      //! Earlier items moved up to make room for a new footer note, by uuid.
      QHash<QString, QPointF> movedItems;
      QString filePath;
      //! Copy of the file export_map replaced, put back by rollback.
      QString backupPath;
      QString description;
      //! Layers a Processing run loaded into the project.
      QStringList layerIds;
  };

  QHash<QString, RollbackEntry> &rollbackStore()
  {
    static QHash<QString, RollbackEntry> store;
    return store;
  }

  QString storeRollback( const RollbackEntry &entry )
  {
    const QString token = QUuid::createUuid().toString( QUuid::WithoutBraces );
    rollbackStore().insert( token, entry );
    return token;
  }

  QJsonObject rollbackJson( const QString &token, const QString &action )
  {
    QJsonObject rollback;
    rollback.insert( u"token"_s, token );
    rollback.insert( u"action"_s, action );
    rollback.insert( u"volatile"_s, true );
    return rollback;
  }

  // Vector and raster file extensions we auto-detect. Lowercase, no leading dot.
  const QSet<QString> &vectorExts()
  {
    static const QSet<QString> set { u"shp"_s, u"geojson"_s, u"json"_s, u"gpkg"_s, u"kml"_s, u"kmz"_s, u"tab"_s, u"mif"_s, u"gml"_s, u"gpx"_s, u"csv"_s, u"fgb"_s, u"sqlite"_s, u"db"_s };
    return set;
  }
  const QSet<QString> &rasterExts()
  {
    static const QSet<QString> set { u"tif"_s, u"tiff"_s, u"asc"_s, u"img"_s, u"jp2"_s, u"vrt"_s, u"nc"_s, u"hdf"_s, u"hgt"_s, u"ecw"_s, u"mbtiles"_s, u"dem"_s, u"bil"_s, u"png"_s, u"jpg"_s, u"jpeg"_s };
    return set;
  }

  QString detectKind( const QString &path )
  {
    const QString ext = QFileInfo( path ).suffix().toLower();
    if ( vectorExts().contains( ext ) )
      return u"vector"_s;
    if ( rasterExts().contains( ext ) )
      return u"raster"_s;
    return QString();
  }

  QString siblingDatasetPath( const QString &sidecarPath )
  {
    const QFileInfo info( sidecarPath );
    const QDir dir = info.absoluteDir();
    QStringList bases { info.completeBaseName() };
    if ( info.baseName() != info.completeBaseName() )
      bases << info.baseName();

    QStringList suffixes;
    suffixes.reserve( vectorExts().size() + rasterExts().size() );
    for ( const QString &ext : vectorExts() )
      suffixes << ext;
    for ( const QString &ext : rasterExts() )
      suffixes << ext;
    suffixes.removeDuplicates();

    // Suggest the most likely primary dataset: a .shp or .gpkg rather than a same-named
    // .csv export or .png preview.
    static const QStringList preferredSuffixes { u"shp"_s, u"gpkg"_s, u"geojson"_s, u"fgb"_s, u"tif"_s, u"tiff"_s, u"vrt"_s };
    static const QStringList sideProductSuffixes { u"csv"_s, u"png"_s, u"jpg"_s, u"jpeg"_s };
    const auto rank = []( const QString &suffix ) -> qsizetype {
      if ( preferredSuffixes.contains( suffix ) )
        return preferredSuffixes.indexOf( suffix );
      if ( sideProductSuffixes.contains( suffix ) )
        return 1000 + sideProductSuffixes.indexOf( suffix );
      return 100;
    };
    std::sort( suffixes.begin(), suffixes.end(), [&rank]( const QString &a, const QString &b ) {
      const qsizetype rankA = rank( a );
      const qsizetype rankB = rank( b );
      return rankA != rankB ? rankA < rankB : a < b;
    } );

    for ( const QString &base : std::as_const( bases ) )
    {
      for ( const QString &suffix : std::as_const( suffixes ) )
      {
        const QString candidate = dir.filePath( base + '.' + suffix );
        if ( QFileInfo::exists( candidate ) )
          return candidate;
      }
    }
    return QString();
  }

  QString sidecarRefusalMessage( const QString &path )
  {
    const QString suffix = QFileInfo( path ).suffix().toLower();
    QString message;
    if ( suffix == "qml"_L1 )
    {
      message = u"Refusing to open style file '%1' as a map layer. A .qml file is a QGIS style, not a dataset."_s.arg( path );
    }
    else
    {
      message = u"Refusing to open sidecar file '%1' as a map layer. Use the dataset file instead of metadata or style sidecars (.qmd, .qml, .prj, .cpg)."_s.arg( path );
    }
    const QString sibling = siblingDatasetPath( path );
    if ( !sibling.isEmpty() )
      message += u" Open '%1' instead."_s.arg( QFileInfo( sibling ).fileName() );
    return message;
  }

  // Resolve a user-supplied layer path inside the AI workspace only.
  QString resolvePath( QgsAiFileContextProvider *provider, const QString &path )
  {
    if ( provider )
    {
      const QString resolved = provider->resolveWorkspaceFile( path );
      if ( !resolved.isEmpty() && QFileInfo::exists( resolved ) )
        return resolved;
    }
    return QString();
  }

  QString serviceLayerProviderKey( const QString &provider )
  {
    const QString normalized = provider.toLower().trimmed();
    if ( normalized == "wms"_L1 || normalized == "xyz"_L1 )
      return u"wms"_s;
    if ( normalized == "wfs"_L1 )
      return u"WFS"_s;
    if ( normalized == "postgres"_L1 || normalized == "postgis"_L1 )
      return u"postgres"_s;
    return QString();
  }

  QString serviceLayerType( const QString &provider )
  {
    const QString normalized = provider.toLower().trimmed();
    if ( normalized == "wms"_L1 || normalized == "xyz"_L1 )
      return u"raster"_s;
    if ( normalized == "wfs"_L1 || normalized == "postgres"_L1 || normalized == "postgis"_L1 )
      return u"vector"_s;
    return QString();
  }

  QString normalizeServiceUri( const QString &provider, const QString &rawUri )
  {
    const QString normalizedProvider = provider.toLower().trimmed();
    QString uri = rawUri.trimmed();
    if ( normalizedProvider == "xyz"_L1 && !uri.contains( u"type=xyz"_s, Qt::CaseInsensitive ) )
    {
      QUrlQuery query;
      query.addQueryItem( u"type"_s, u"xyz"_s );
      query.addQueryItem( u"url"_s, uri );
      query.addQueryItem( u"zmin"_s, u"0"_s );
      query.addQueryItem( u"zmax"_s, u"19"_s );
      uri = query.toString( QUrl::FullyEncoded );
    }
    return uri;
  }

  QString applyServiceLayerOptions( const QString &providerKey, const QString &uri, const QJsonObject &options, QString &error )
  {
    if ( options.isEmpty() )
      return uri;

    QgsProviderRegistry *registry = QgsProviderRegistry::instance();
    if ( !registry )
    {
      error = u"QGIS provider registry is not available."_s;
      return QString();
    }

    QgsProviderMetadata *metadata = registry->providerMetadata( providerKey );
    if ( !metadata && providerKey == "WFS"_L1 )
      metadata = registry->providerMetadata( u"wfs"_s );
    if ( !metadata )
    {
      error = u"Layer options are not supported for provider key: %1"_s.arg( providerKey );
      return QString();
    }

    QVariantMap parts = metadata->decodeUri( uri );
    for ( auto it = options.constBegin(); it != options.constEnd(); ++it )
      parts.insert( it.key(), it.value().toVariant() );
    return metadata->encodeUri( parts );
  }

  QJsonObject extentJson( const QgsRectangle &extent )
  {
    QJsonObject e;
    e.insert( u"xmin"_s, extent.xMinimum() );
    e.insert( u"ymin"_s, extent.yMinimum() );
    e.insert( u"xmax"_s, extent.xMaximum() );
    e.insert( u"ymax"_s, extent.yMaximum() );
    return e;
  }

  /**
   * Summarizes whether a freshly added layer looks usable.
   * \a canceled is set when the user pressed Stop during the checks that scan the layer.
   */
  QJsonObject layerQualityChecks( QgsMapLayer *layer, QgsProject *project, bool *canceled = nullptr )
  {
    const bool layerInProject = layer && project && project->mapLayer( layer->id() ) == layer;
    const bool extentValid = layer && ( !layer->isSpatial() || ( layer->extent().isFinite() && !layer->extent().isEmpty() ) );
    const bool crsValid = layer && ( !layer->isSpatial() || layer->crs().isValid() );
    bool featureCountValid = true;
    bool geometryPresent = true;
    bool treesSemanticsPassed = true;
    QJsonObject checks;
    if ( QgsVectorLayer *vector = qobject_cast<QgsVectorLayer *>( layer ) )
    {
      featureCountValid = vector->featureCount() > 0;
      geometryPresent = !vector->isSpatial() || vector->geometryType() != Qgis::GeometryType::Unknown;
      const int estimateIdx = vector->fields().indexOf( "estimate"_L1 );
      const int contextIdx = vector->fields().indexOf( "context"_L1 );
      if ( estimateIdx >= 0 || contextIdx >= 0 )
      {
        const bool estimateFieldsPresent = estimateIdx >= 0;
        bool contextValuesValid = contextIdx < 0;
        bool contextValuesChecked = true;
        if ( contextIdx >= 0 )
        {
          // Shared with the worker, which may outlive this call if Strata quits mid-run.
          struct ContextScanJob
          {
              std::unique_ptr<QgsVectorLayerFeatureSource> source;
              QgsFeatureRequest request;
              int contextIdx = -1;
              long long total = 0;
              std::atomic_bool invalid { false };
          };
          auto job = std::make_shared<ContextScanJob>();
          job->source = std::make_unique<QgsVectorLayerFeatureSource>( vector );
          job->request.setSubsetOfAttributes( QList<int> { contextIdx } );
          job->request.setFlags( Qgis::FeatureRequestFlag::NoGeometry );
          job->contextIdx = contextIdx;
          job->total = std::max( 0LL, vector->featureCount() );
          QgsAiBackgroundRunOptions options;
          options.forceGuiThread = qgsAiProviderUsesTransaction( vector );
          options.dependentLayers = { vector };
          QElapsedTimer contextTimer;
          contextTimer.start();
          const QgsAiTaskWaitResult wait = qgsAiRunFunction(
            u"Checking layer context values"_s,
            [job]( QgsFeedback *feedback ) {
              QgsFeatureRequest request = job->request;
              request.setFeedback( feedback );
              QgsFeatureIterator it = job->source->getFeatures( request );
              QgsFeature feature;
              int n = 0;
              const QStringList allowed { u"street_row"_s, u"park"_s, u"public"_s };
              while ( it.nextFeature( feature ) )
              {
                if ( feedback->isCanceled() )
                  return false;
                const QString context = feature.attribute( job->contextIdx ).toString().trimmed();
                if ( !context.isEmpty() && !allowed.contains( context ) )
                {
                  job->invalid = true;
                  return true;
                }
                ++n;
                if ( job->total > 0 )
                  feedback->setProgress( std::min( 100.0, 100.0 * static_cast<double>( n ) / static_cast<double>( job->total ) ) );
              }
              return true;
            },
            options
          );
          qgsAiLogPerf( u"layer_quality_checks"_s, u"context_scan"_s, contextTimer.elapsed() );
          if ( wait.succeeded )
          {
            contextValuesValid = !job->invalid;
          }
          else
          {
            // Unknown, not valid: the scan was stopped or failed before it could decide.
            contextValuesChecked = false;
            if ( wait.canceled && canceled )
              *canceled = true;
          }
        }
        checks.insert( u"estimate_fields_present"_s, estimateFieldsPresent );
        checks.insert( u"context_values_valid"_s, contextValuesChecked ? QJsonValue( contextValuesValid ) : QJsonValue( QJsonValue::Null ) );
        treesSemanticsPassed = estimateFieldsPresent && contextValuesChecked && contextValuesValid;
      }
    }

    checks.insert( u"layer_in_project"_s, layerInProject );
    checks.insert( u"feature_count_valid"_s, featureCountValid );
    checks.insert( u"extent_valid"_s, extentValid );
    checks.insert( u"geometry_present"_s, geometryPresent );
    checks.insert( u"crs_valid"_s, crsValid );
    checks.insert( u"passed"_s, layerInProject && featureCountValid && extentValid && geometryPresent && crsValid && treesSemanticsPassed );
    return checks;
  }

  bool fileMayBeNonSpatialTable( const QString &path )
  {
    const QString extension = QFileInfo( path ).suffix().toLower();
    return extension == "csv"_L1 || extension == "gpkg"_L1 || extension == "sqlite"_L1 || extension == "db"_L1;
  }

  //! Deletes a layer handed over to the interface thread from that thread's event loop.
  struct LayerToolsLaterDeleter
  {
      void operator()( QgsMapLayer *layer ) const
      {
        if ( layer )
          layer->deleteLater();
      }
  };

  //! A layer opened and checked on a worker thread, owned by the interface thread once handed over.
  struct LayerToolsLoad
  {
      std::unique_ptr<QgsMapLayer, LayerToolsLaterDeleter> layer;
      QString error;
      QJsonObject facts;
  };

  /**
   * Runs \a open on a worker thread with Stop, and hands the layer it returns over to the calling
   * (interface) thread. \a open reports a refusal through LayerToolsLoad::error and returns null.
   */
  QgsAiTaskWaitResult layerToolsOpenInBackground(
    const QString &label, const std::function<std::unique_ptr<QgsMapLayer>( LayerToolsLoad &load, QgsFeedback *feedback )> &open, const std::shared_ptr<LayerToolsLoad> &load
  )
  {
    QThread *interfaceThread = QThread::currentThread();
    return qgsAiRunFunction( label, [open, load, interfaceThread]( QgsFeedback *feedback ) {
      std::unique_ptr<QgsMapLayer> layer = open( *load, feedback );
      if ( !layer || feedback->isCanceled() )
        return !feedback->isCanceled();
      // The extent may mean a scan of the data: learn it here, not on the interface thread.
      if ( layer->isSpatial() && layer->extent().isFinite() )
        load->facts.insert( u"extent"_s, extentJson( layer->extent() ) );
      layer->moveToThread( interfaceThread );
      load->layer.reset( layer.release() );
      return true;
    } );
  }

  QString validateUsableVectorLayer( QgsVectorLayer *layer, bool allowNonSpatialTable )
  {
    if ( !layer )
      return u"Vector layer was not created."_s;

    const qint64 featureCount = layer->featureCount();
    if ( featureCount == 0 )
      return u"Vector layer contains zero features."_s;

    if ( !layer->isSpatial() )
    {
      if ( allowNonSpatialTable )
        return QString();
      return u"Vector layer has no geometry, but this source is expected to contain spatial features."_s;
    }

    bool hasGeometry = false;
    QgsFeatureRequest request;
    request.setNoAttributes();
    QgsFeatureIterator iterator = layer->getFeatures( request );
    QgsFeature feature;
    while ( iterator.nextFeature( feature ) )
    {
      if ( feature.hasGeometry() && !feature.geometry().isEmpty() )
      {
        hasGeometry = true;
        break;
      }
    }
    if ( !hasGeometry )
      return u"Vector layer declares a spatial geometry type but no feature has geometry."_s;

    const QgsRectangle extent = layer->extent();
    if ( !extent.isFinite() )
      return u"Vector layer extent contains non-finite coordinates."_s;
    return QString();
  }

  QString layerTreePath( QgsLayerTreeLayer *node )
  {
    if ( !node )
      return QString();

    QStringList parts;
    QgsLayerTreeNode *current = node;
    while ( current )
    {
      if ( current->parent() && !current->name().isEmpty() )
        parts.prepend( current->name() );
      current = current->parent();
    }
    return u"/%1"_s.arg( parts.join( "/"_L1 ) );
  }

  QJsonObject layerVisualSummary( QgsMapLayer *layer, QgsLayerTreeLayer *node )
  {
    QJsonObject summary;
    if ( !layer )
      return summary;

    summary.insert( u"visible"_s, node ? node->isVisible() : true );
    summary.insert( u"checked"_s, node ? node->itemVisibilityChecked() : true );
    if ( node )
      summary.insert( u"tree_path"_s, layerTreePath( node ) );
    summary.insert( u"opacity"_s, layer->opacity() );
    summary.insert( u"scale_based_visibility"_s, layer->hasScaleBasedVisibility() );
    summary.insert( u"minimum_scale"_s, layer->minimumScale() );
    summary.insert( u"maximum_scale"_s, layer->maximumScale() );

    if ( QgsVectorLayer *vector = qobject_cast<QgsVectorLayer *>( layer ) )
    {
      summary.insert( u"renderer_type"_s, vector->renderer() ? vector->renderer()->type() : QString() );
      summary.insert( u"labels_enabled"_s, vector->labelsEnabled() && vector->labeling() );
    }
    else if ( QgsRasterLayer *raster = qobject_cast<QgsRasterLayer *>( layer ) )
    {
      summary.insert( u"renderer_type"_s, raster->renderer() ? raster->renderer()->type() : QString() );
      summary.insert( u"labels_enabled"_s, raster->labelsEnabled() && raster->labeling() );
    }
    else
    {
      summary.insert( u"labels_enabled"_s, false );
    }
    return summary;
  }

  QString uniqueLayoutName( QgsProject *project, const QString &requestedName )
  {
    const QString base = requestedName.trimmed().isEmpty() ? u"Strata Layout"_s : requestedName.trimmed();
    QgsLayoutManager *manager = project ? project->layoutManager() : nullptr;
    if ( !manager || !manager->layoutByName( base ) )
      return base;

    for ( int i = 2; i < 1000; ++i )
    {
      const QString candidate = u"%1 %2"_s.arg( base ).arg( i );
      if ( !manager->layoutByName( candidate ) )
        return candidate;
    }
    return u"%1 %2"_s.arg( base, QString::number( QDateTime::currentMSecsSinceEpoch() ) );
  }

  QVariantMap jsonObjectToVariantMap( const QJsonObject &object )
  {
    QVariantMap map;
    for ( auto it = object.constBegin(); it != object.constEnd(); ++it )
      map.insert( it.key(), it.value().toVariant() );
    return map;
  }

  // Single-select enums store their default as a one-element list. parameterAsInt
  // cannot read that list (it falls back to 0), so unwrap it before validation.
  QVariantMap normalizeSingleEnumParameters( const QgsProcessingAlgorithm *algorithm, const QVariantMap &parameters )
  {
    QVariantMap normalized = parameters;
    if ( !algorithm )
      return normalized;

    const QgsProcessingParameterDefinitions definitions = algorithm->parameterDefinitions();
    for ( const QgsProcessingParameterDefinition *definition : definitions )
    {
      const QgsProcessingParameterEnum *enumDefinition = dynamic_cast<const QgsProcessingParameterEnum *>( definition );
      if ( !enumDefinition || enumDefinition->allowMultiple() )
        continue;

      const QVariant value = normalized.value( enumDefinition->name() );
      if ( value.userType() != QMetaType::Type::QVariantList )
        continue;

      const QVariantList values = value.toList();
      if ( values.size() != 1 )
        continue;

      bool ok = false;
      const int index = values.constFirst().toInt( &ok );
      if ( !ok )
        continue;
      normalized.insert( enumDefinition->name(), index );
    }
    return normalized;
  }

  // A feature source given as {"source", "selected_only", "filter", "feature_limit"} becomes the
  // definition the Toolbox builds for "Selected features only" and its advanced options.
  bool resolveFeatureSourceDefinitions( const QgsProcessingAlgorithm *algorithm, QVariantMap &parameters, QgsProcessingContext &context, QString &error )
  {
    const QgsProcessingParameterDefinitions definitions = algorithm->parameterDefinitions();
    for ( const QgsProcessingParameterDefinition *definition : definitions )
    {
      const QVariant value = parameters.value( definition->name() );
      if ( definition->type() != QgsProcessingParameterFeatureSource::typeName() || value.userType() != QMetaType::Type::QVariantMap )
        continue;

      const QVariantMap map = value.toMap();
      const QString source = map.value( u"source"_s ).toString().trimmed();
      if ( source.isEmpty() )
      {
        error = u"Parameter %1: 'source' (a layer id) is required when the input is an object."_s.arg( definition->name() );
        return false;
      }
      const bool selectedOnly = map.value( u"selected_only"_s ).toBool();
      if ( selectedOnly )
      {
        // QGIS would silently process zero features
        QgsVectorLayer *layer = qobject_cast<QgsVectorLayer *>( QgsProcessingUtils::mapLayerFromString( source, context, false ) );
        if ( layer && layer->selectedFeatureCount() == 0 )
        {
          error = u"Parameter %1: layer %2 has no selected features; select them first or drop selected_only."_s.arg( definition->name(), source );
          return false;
        }
      }
      parameters.insert(
        definition->name(),
        QVariant::fromValue( QgsProcessingFeatureSourceDefinition(
          source, selectedOnly, map.value( u"feature_limit"_s, -1 ).toLongLong(), Qgis::ProcessingFeatureSourceDefinitionFlags(), Qgis::InvalidGeometryCheck::AbortOnInvalid, map.value( u"filter"_s ).toString()
        ) )
      );
    }
    return true;
  }

  bool isLoadableProcessingDestination( const QgsProcessingParameterDefinition *definition )
  {
    return dynamic_cast<const QgsProcessingParameterFeatureSink *>( definition )
           || dynamic_cast<const QgsProcessingParameterVectorDestination *>( definition )
           || dynamic_cast<const QgsProcessingParameterRasterDestination *>( definition )
           || dynamic_cast<const QgsProcessingParameterPointCloudDestination *>( definition )
           || dynamic_cast<const QgsProcessingParameterVectorTileDestination *>( definition );
  }

  // Match processing.runAndLoadResults: wrap layer destinations so TEMPORARY_OUTPUT
  // (and file sinks) stay attached to the project instead of dying with the context.
  void bindProcessingDestinationsToProject( const QgsProcessingAlgorithm *algorithm, QVariantMap &parameters, QgsProject *project )
  {
    if ( !algorithm || !project )
      return;

    const QgsProcessingParameterDefinitions definitions = algorithm->parameterDefinitions();
    for ( const QgsProcessingParameterDefinition *definition : definitions )
    {
      if ( !isLoadableProcessingDestination( definition ) )
        continue;
      if ( definition->flags().testFlag( Qgis::ProcessingParameterFlag::Hidden ) )
        continue;

      const auto *destination = dynamic_cast<const QgsProcessingDestinationParameter *>( definition );
      if ( !destination )
        continue;

      if ( !parameters.contains( definition->name() ) )
      {
        if ( !destination->createByDefault() )
          continue;
        parameters.insert( definition->name(), QgsProcessing::TEMPORARY_OUTPUT );
      }

      const QVariant value = parameters.value( definition->name() );
      if ( value.userType() == qMetaTypeId<QgsProcessingOutputLayerDefinition>() )
      {
        QgsProcessingOutputLayerDefinition layerDefinition = value.value<QgsProcessingOutputLayerDefinition>();
        if ( !layerDefinition.destinationProject )
          layerDefinition.destinationProject = project;
        parameters.insert( definition->name(), QVariant::fromValue( layerDefinition ) );
        continue;
      }

      const QString destinationString = value.toString();
      if ( destinationString.isEmpty() )
        continue;

      parameters.insert( definition->name(), QVariant::fromValue( QgsProcessingOutputLayerDefinition( destinationString, project ) ) );
    }
  }

  QJsonArray loadProcessingResultsIntoProject( QgsProcessingContext &context, QgsProject *project )
  {
    QJsonArray loaded;
    if ( !project )
      return loaded;

    QgsLayerTreeGroup *root = project->layerTreeRoot();
    QgsLayerTreeRegistryBridge *bridge = project->layerTreeRegistryBridge();
    std::optional<QgsLayerTreeRegistryBridge::InsertionPoint> previousPoint;
    if ( bridge )
      previousPoint.emplace( bridge->layerInsertionPoint() );

    const QMap<QString, QgsProcessingContext::LayerDetails> pending = context.layersToLoadOnCompletion();
    int insertIndex = 0;
    for ( auto it = pending.constBegin(); it != pending.constEnd(); ++it )
    {
      const QgsProcessingContext::LayerDetails &details = it.value();
      QgsMapLayer *layer = QgsProcessingUtils::mapLayerFromString( it.key(), context, true, details.layerTypeHint );
      if ( !layer )
        continue;

      QJsonObject info;
      info.insert( u"id"_s, layer->id() );
      info.insert( u"name"_s, layer->name() );
      info.insert( u"output"_s, details.outputName );

      if ( project->mapLayer( layer->id() ) )
      {
        loaded.append( info );
        continue;
      }

      details.setOutputLayerName( layer );
      QgsMapLayer *owned = context.temporaryLayerStore()->takeMapLayer( layer );
      if ( !owned )
        continue;

      if ( bridge && root )
        bridge->setLayerInsertionPoint( QgsLayerTreeRegistryBridge::InsertionPoint( root, insertIndex ) );
      project->addMapLayer( owned );
      ++insertIndex;

      info.insert( u"id"_s, owned->id() );
      info.insert( u"name"_s, owned->name() );
      info.insert( u"temporary"_s, owned->providerType() == "memory"_L1 );
      loaded.append( info );
    }

    if ( bridge && previousPoint.has_value() )
      bridge->setLayerInsertionPoint( *previousPoint );

    return loaded;
  }

  QJsonArray processingParametersJson( const QgsProcessingAlgorithm *algorithm )
  {
    QJsonArray array;
    if ( !algorithm )
      return array;

    const QgsProcessingParameterDefinitions definitions = algorithm->parameterDefinitions();
    for ( const QgsProcessingParameterDefinition *definition : definitions )
    {
      if ( !definition )
        continue;

      QJsonObject entry;
      entry.insert( u"name"_s, definition->name() );
      entry.insert( u"description"_s, definition->description() );
      entry.insert( u"type"_s, definition->type() );
      entry.insert( u"optional"_s, definition->flags().testFlag( Qgis::ProcessingParameterFlag::Optional ) );
      entry.insert( u"destination"_s, definition->isDestination() );
      if ( const auto *destination = dynamic_cast<const QgsProcessingDestinationParameter *>( definition ) )
        entry.insert( u"create_by_default"_s, destination->createByDefault() );
      if ( const QgsProcessingParameterEnum *enumDefinition = dynamic_cast<const QgsProcessingParameterEnum *>( definition ) )
      {
        QJsonArray options;
        const QStringList labels = enumDefinition->options();
        for ( const QString &label : labels )
          options.append( label );
        entry.insert( u"options"_s, options );
      }
      if ( definition->defaultValue().isValid() )
      {
        QVariant defaultValue = definition->defaultValue();
        if ( const QgsProcessingParameterEnum *enumDefinition = dynamic_cast<const QgsProcessingParameterEnum *>( definition ) )
        {
          if ( !enumDefinition->allowMultiple() && defaultValue.userType() == QMetaType::Type::QVariantList )
          {
            const QVariantList values = defaultValue.toList();
            if ( values.size() == 1 )
              defaultValue = values.constFirst();
          }
        }
        entry.insert( u"default"_s, QJsonValue::fromVariant( defaultValue ) );
      }
      array.push_back( entry );
    }
    return array;
  }

  QJsonArray processingOutputsJson( const QgsProcessingAlgorithm *algorithm )
  {
    QJsonArray array;
    if ( !algorithm )
      return array;

    const QgsProcessingOutputDefinitions definitions = algorithm->outputDefinitions();
    for ( const QgsProcessingOutputDefinition *definition : definitions )
    {
      if ( !definition )
        continue;
      QJsonObject entry;
      entry.insert( u"name"_s, definition->name() );
      entry.insert( u"description"_s, definition->description() );
      entry.insert( u"type"_s, definition->type() );
      array.push_back( entry );
    }
    return array;
  }

  QJsonObject processingAlgorithmMetadataJson( const QgsProcessingAlgorithm *algorithm )
  {
    QJsonObject output;
    if ( !algorithm )
      return output;
    output.insert( u"algorithm_id"_s, algorithm->id() );
    output.insert( u"name"_s, algorithm->name() );
    output.insert( u"display_name"_s, algorithm->displayName() );
    output.insert( u"group"_s, algorithm->group() );
    output.insert( u"group_id"_s, algorithm->groupId() );
    output.insert( u"parameters"_s, processingParametersJson( algorithm ) );
    output.insert( u"outputs"_s, processingOutputsJson( algorithm ) );
    return output;
  }

  QString exportResultMessage( QgsLayoutExporter::ExportResult result )
  {
    switch ( result )
    {
      case QgsLayoutExporter::Success:
        return u"success"_s;
      case QgsLayoutExporter::Canceled:
        return u"canceled"_s;
      case QgsLayoutExporter::MemoryError:
        return u"memory_error"_s;
      case QgsLayoutExporter::FileError:
        return u"file_error"_s;
      case QgsLayoutExporter::PrintError:
        return u"print_error"_s;
      case QgsLayoutExporter::SvgLayerError:
        return u"svg_layer_error"_s;
      case QgsLayoutExporter::IteratorError:
        return u"iterator_error"_s;
    }
    return u"unknown_error"_s;
  }

  QgsPrintLayout *printLayoutByName( QgsProject *project, const QString &name )
  {
    if ( !project || name.trimmed().isEmpty() )
      return nullptr;
    QgsMasterLayoutInterface *layout = project->layoutManager() ? project->layoutManager()->layoutByName( name.trimmed() ) : nullptr;
    return dynamic_cast<QgsPrintLayout *>( layout );
  }

  QgsLayoutItemMap *firstLayoutMap( QgsPrintLayout *layout )
  {
    if ( !layout )
      return nullptr;
    QList<QgsLayoutItemMap *> maps;
    layout->layoutItems<QgsLayoutItemMap>( maps );
    return maps.empty() ? nullptr : maps.constFirst();
  }

  QRectF sceneRect( const QgsLayoutItem *item )
  {
    return item->mapRectToScene( item->rect() );
  }

  QRectF pageRectAt( QgsLayout *layout, QPointF point )
  {
    QgsLayoutPageCollection *pages = layout->pageCollection();
    const QgsLayoutItemPage *page = pages->page( std::max( 0, pages->pageNumberForPoint( point ) ) );
    return page ? QRectF( page->pos(), page->rect().size() ) : QRectF();
  }

  // What the layout looks like, in mm relative to each page, plus the problems a person would see
  // on paper: items cut by the page edge or covering each other. The model cannot see the layout.
  QJsonObject layoutSummary( QgsPrintLayout *layout )
  {
    QJsonObject summary;
    if ( !layout || !layout->pageCollection() || layout->pageCollection()->pageCount() == 0 )
      return summary;

    const QgsLayoutItemPage *firstPage = layout->pageCollection()->page( 0 );
    const QSizeF pageSize = layout->convertToLayoutUnits( firstPage->pageSize() );
    QJsonObject page;
    page.insert( u"width_mm"_s, std::round( pageSize.width() ) );
    page.insert( u"height_mm"_s, std::round( pageSize.height() ) );
    page.insert( u"orientation"_s, firstPage->orientation() == QgsLayoutItemPage::Landscape ? u"landscape"_s : u"portrait"_s );
    page.insert( u"page_count"_s, layout->pageCollection()->pageCount() );
    summary.insert( u"page"_s, page );

    QList<QgsLayoutItem *> items;
    layout->layoutItems( items );
    items.erase( std::remove_if( items.begin(), items.end(), []( QgsLayoutItem *item ) { return qobject_cast<QgsLayoutItemPage *>( item ) || qobject_cast<QgsLayoutItemGroup *>( item ); } ), items.end() );

    QJsonArray itemArray;
    QJsonArray issues;
    for ( int i = 0; i < items.size(); ++i )
    {
      const QRectF rect = sceneRect( items.at( i ) );
      const QRectF pageRect = pageRectAt( layout, rect.center() );
      const QString name = items.at( i )->displayName();
      QJsonObject item;
      item.insert( u"name"_s, name );
      item.insert( u"x_mm"_s, std::round( rect.x() - pageRect.x() ) );
      item.insert( u"y_mm"_s, std::round( rect.y() - pageRect.y() ) );
      item.insert( u"width_mm"_s, std::round( rect.width() ) );
      item.insert( u"height_mm"_s, std::round( rect.height() ) );
      itemArray.append( item );

      // half a millimetre of tolerance for frame strokes and rounding
      if ( !pageRect.adjusted( -0.5, -0.5, 0.5, 0.5 ).contains( rect ) )
        issues.append( u"'%1' extends outside the page."_s.arg( name ) );
      for ( int j = i + 1; j < items.size(); ++j )
      {
        const QRectF overlap = rect.intersected( sceneRect( items.at( j ) ) );
        if ( overlap.width() > 0.5 && overlap.height() > 0.5 )
          issues.append( u"'%1' overlaps '%2'."_s.arg( name, items.at( j )->displayName() ) );
      }
    }
    summary.insert( u"items"_s, itemArray );
    summary.insert( u"issues"_s, issues );
    return summary;
  }

  QSize exportRenderSize( const QSize &requested, const QSize &fallback )
  {
    QSize size = requested.isValid() ? requested : fallback;
    if ( !size.isValid() || size.width() <= 0 || size.height() <= 0 )
      size = QSize( 1280, 720 );
    return QSize( std::clamp( size.width(), 1, 4096 ), std::clamp( size.height(), 1, 4096 ) );
  }

  QgsSymbol *defaultSymbolWithColor( Qgis::GeometryType geometryType, int index )
  {
    std::unique_ptr<QgsSymbol> symbol( QgsSymbol::defaultSymbol( geometryType ) );
    if ( !symbol )
      return nullptr;
    symbol->setColor( QColor::fromHsv( ( index * 67 ) % 360, 180, 220 ) );
    return symbol.release();
  }

  bool applyBasicLabels( QgsVectorLayer *layer, const QJsonValue &labelsValue, const QString &fallbackField, QJsonArray &changes, QString &error )
  {
    if ( !labelsValue.isObject() && !labelsValue.isBool() )
      return true;

    bool enabled = labelsValue.isBool() ? labelsValue.toBool() : labelsValue.toObject().value( u"enabled"_s ).toBool( true );
    if ( !enabled )
    {
      layer->setLabelsEnabled( false );
      changes.append( u"labels"_s );
      return true;
    }

    QString labelField = fallbackField;
    if ( labelsValue.isObject() )
      labelField = labelsValue.toObject().value( u"field"_s ).toString( fallbackField ).trimmed();
    if ( labelField.isEmpty() )
    {
      error = u"Label field is required when labels are enabled."_s;
      return false;
    }
    if ( layer->fields().lookupField( labelField ) < 0 )
    {
      error = u"Label field does not exist: %1"_s.arg( labelField );
      return false;
    }

    QgsPalLayerSettings settings = QgsAbstractVectorLayerLabeling::defaultSettingsForLayer( layer );
    settings.fieldName = labelField;
    settings.isExpression = false;
    layer->setLabeling( new QgsVectorLayerSimpleLabeling( settings ) );
    layer->setLabelsEnabled( true );
    changes.append( u"labels"_s );
    return true;
  }

  void refreshLayoutLegends( QgsProject *project, const QString &layerId )
  {
    if ( !project || !project->layoutManager() || layerId.isEmpty() )
      return;

    const QList<QgsPrintLayout *> layouts = project->layoutManager()->printLayouts();
    for ( QgsPrintLayout *layout : layouts )
    {
      if ( !layout )
        continue;

      QList<QgsLayoutItemLegend *> legends;
      layout->layoutItems( legends );
      for ( QgsLayoutItemLegend *legend : legends )
      {
        if ( !legend || !legend->model() )
          continue;

        // Manual legends own a cloned tree, so find the layer node in each
        // legend model instead of assuming it is the project's node.
        QgsLayerTree *legendRoot = legend->model()->rootGroup();
        if ( QgsLayerTreeLayer *legendNode = legendRoot ? legendRoot->findLayer( layerId ) : nullptr )
          legend->model()->refreshLayerLegend( legendNode );
        legend->updateLegend();
        legend->invalidateCache();
        legend->update();
      }
    }
  }

  QgsAiToolResult rollbackLayerAddition( QgsProject *project, const QString &token )
  {
    if ( !rollbackStore().contains( token ) )
      return QgsAiToolResult::error( u"Unknown or expired rollback token."_s );
    RollbackEntry entry = rollbackStore().take( token );
    if ( entry.type != RollbackType::RemoveLayer )
      return QgsAiToolResult::error( u"Rollback token does not belong to a layer-add operation."_s );
    if ( !project )
      return QgsAiToolResult::error( u"No active QgsProject available."_s );
    if ( !project->mapLayer( entry.layerId ) )
    {
      QJsonObject output;
      output.insert( u"status"_s, u"already_removed"_s );
      output.insert( u"rollback_token"_s, token );
      return QgsAiToolResult::ok( output );
    }

    project->removeMapLayer( entry.layerId );
    QJsonObject diff;
    diff.insert( u"summary"_s, u"Removed layer added by AI tool."_s );
    diff.insert( u"layer_id"_s, entry.layerId );

    QJsonObject output;
    output.insert( u"status"_s, u"rolled_back"_s );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"diff"_s, diff );
    return QgsAiToolResult::ok( output );
  }

  QgsAiToolResult rollbackLoadedLayers( QgsProject *project, const QString &token )
  {
    if ( !rollbackStore().contains( token ) || rollbackStore().value( token ).type != RollbackType::RemoveLoadedLayers )
      return QgsAiToolResult::error( u"Unknown or expired rollback token."_s );
    const RollbackEntry entry = rollbackStore().take( token );
    if ( !project )
      return QgsAiToolResult::error( u"No active QgsProject available."_s );
    QStringList removed;
    for ( const QString &layerId : entry.layerIds )
    {
      if ( project->mapLayer( layerId ) )
      {
        project->removeMapLayer( layerId );
        removed << layerId;
      }
    }
    QJsonObject diff;
    diff.insert( u"summary"_s, u"Removed the %1 output layers of a Processing run from the project."_s.arg( removed.size() ) );
    diff.insert( u"layer_ids"_s, QJsonArray::fromStringList( removed ) );
    QJsonObject output;
    output.insert( u"status"_s, u"rolled_back"_s );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"diff"_s, diff );
    return QgsAiToolResult::ok( output );
  }

  QgsAiToolResult rollbackLayerStyle( QgsProject *project, const QString &token )
  {
    if ( !rollbackStore().contains( token ) )
      return QgsAiToolResult::error( u"Unknown or expired rollback token."_s );
    RollbackEntry entry = rollbackStore().take( token );
    if ( entry.type != RollbackType::RestoreLayerStyle )
      return QgsAiToolResult::error( u"Rollback token does not belong to a layer-style operation."_s );
    if ( !project )
      return QgsAiToolResult::error( u"No active QgsProject available."_s );

    QgsMapLayer *layer = project->mapLayer( entry.layerId );
    if ( !layer )
      return QgsAiToolResult::error( u"Cannot rollback style: layer no longer exists: %1"_s.arg( entry.layerId ) );

    QgsLayerTreeLayer *treeNode = project->layerTreeRoot() ? project->layerTreeRoot()->findLayer( entry.layerId ) : nullptr;
    const QJsonObject before = layerVisualSummary( layer, treeNode );

    QgsMapLayerStyle style( entry.styleXml );
    style.writeToLayer( layer );
    if ( treeNode && entry.hasVisibility )
      treeNode->setItemVisibilityChecked( entry.visible );
    layer->triggerRepaint();
    if ( QgsVectorLayer *vector = qobject_cast<QgsVectorLayer *>( layer ) )
      vector->emitStyleChanged();
    refreshLayoutLegends( project, layer->id() );

    const QJsonObject after = layerVisualSummary( layer, treeNode );
    QJsonObject diff;
    diff.insert( u"summary"_s, u"Restored previous layer style."_s );
    diff.insert( u"before"_s, before );
    diff.insert( u"after"_s, after );

    QJsonObject output;
    output.insert( u"status"_s, u"rolled_back"_s );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"layer_id"_s, entry.layerId );
    output.insert( u"diff"_s, diff );
    return QgsAiToolResult::ok( output );
  }

  QgsAiToolResult rollbackLayoutCreation( QgsProject *project, const QString &token )
  {
    if ( !rollbackStore().contains( token ) )
      return QgsAiToolResult::error( u"Unknown or expired rollback token."_s );
    RollbackEntry entry = rollbackStore().take( token );
    if ( entry.type != RollbackType::RemoveLayout )
      return QgsAiToolResult::error( u"Rollback token does not belong to a layout operation."_s );
    if ( !project || !project->layoutManager() )
      return QgsAiToolResult::error( u"No layout manager available."_s );

    QgsMasterLayoutInterface *layout = project->layoutManager()->layoutByName( entry.layoutName );
    if ( layout )
      project->layoutManager()->removeLayout( layout );

    QJsonObject diff;
    diff.insert( u"summary"_s, u"Removed print layout created by AI tool."_s );
    diff.insert( u"layout_name"_s, entry.layoutName );

    QJsonObject output;
    output.insert( u"status"_s, layout ? u"rolled_back"_s : u"already_removed"_s );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"diff"_s, diff );
    return QgsAiToolResult::ok( output );
  }

  // Manual mode owns a copy of the tree, so labels set on it leave the project's layer names alone.
  // The copy starts from the current tree: start from all project layers, keep the legend ones.
  void makeLegendManual( QgsLayoutItemLegend *legend, const QStringList &keepIds )
  {
    legend->setSyncMode( Qgis::LegendSyncMode::AllProjectLayers );
    legend->setSyncMode( Qgis::LegendSyncMode::Manual );
    QgsLayerTreeGroup *root = legend->model()->rootGroup();
    const QList<QgsLayerTreeLayer *> nodes = root->findLayers();
    for ( QgsLayerTreeLayer *node : nodes )
    {
      if ( !keepIds.contains( node->layerId() ) )
        QgsLayerTree::toGroup( node->parent() )->removeChildNode( node );
    }
    root->removeChildrenGroupWithoutLayers();
  }

  void setLegendLabels( QgsLayoutItemLegend *legend, const QVariantMap &labels, QVariantMap *before = nullptr )
  {
    QgsLayerTreeGroup *root = legend->model()->rootGroup();
    for ( auto it = labels.constBegin(); it != labels.constEnd(); ++it )
    {
      QgsLayerTreeLayer *node = root->findLayer( it.key() );
      if ( !node )
        continue;
      if ( before )
        before->insert( it.key(), node->customProperty( u"legend/title-label"_s ) );
      if ( it.value().isValid() )
        node->setCustomProperty( u"legend/title-label"_s, it.value().toString() );
      else
        node->removeCustomProperty( u"legend/title-label"_s );
      legend->model()->clearCachedData( node );
    }
  }

  // a legend measures itself on its first paint: paint once so its position and the summary use the real size
  void fitLegendToContent( QgsLayoutItemLegend *legend )
  {
    QImage scratch( 1, 1, QImage::Format_ARGB32 );
    QPainter painter( &scratch );
    legend->paint( &painter, nullptr, nullptr );
    painter.end();
    legend->adjustBoxSize();
  }

  // the label wraps long text to its width: one text height per wrapped line
  double wrappedTextHeight( const QgsLayoutItemLabel *label, double width )
  {
    const QSizeF natural = label->sizeForText();
    return natural.height() * std::max( 1.0, std::ceil( natural.width() / width ) );
  }

  QgsAiToolResult rollbackLayoutEdit( QgsProject *project, const QString &token )
  {
    if ( !rollbackStore().contains( token ) )
      return QgsAiToolResult::error( u"Unknown or expired rollback token."_s );
    RollbackEntry entry = rollbackStore().take( token );
    if ( entry.type != RollbackType::RestoreLayoutEdit )
      return QgsAiToolResult::error( u"Rollback token does not belong to a layout edit operation."_s );
    if ( !project || !project->layoutManager() )
      return QgsAiToolResult::error( u"No layout manager available."_s );

    QgsPrintLayout *layout = printLayoutByName( project, entry.layoutName );
    if ( !layout )
      return QgsAiToolResult::error( u"Cannot rollback layout edit: layout no longer exists: %1"_s.arg( entry.layoutName ) );

    int removedItems = 0;
    for ( const QString &uuid : entry.itemUuids )
    {
      if ( QgsLayoutItem *item = layout->itemByUuid( uuid ) )
      {
        layout->removeLayoutItem( item );
        ++removedItems;
      }
    }

    if ( QgsLayoutItemMap *map = qobject_cast<QgsLayoutItemMap *>( layout->itemByUuid( entry.mapUuid ) ) )
    {
      map->attemptMove( QgsLayoutPoint( entry.mapRect.topLeft() ) );
      map->attemptResize( QgsLayoutSize( entry.mapRect.size() ) );
      map->zoomToExtent( entry.mapExtent );
    }

    if ( QgsLayoutItemLegend *legend = qobject_cast<QgsLayoutItemLegend *>( layout->itemByUuid( entry.legendUuid ) ) )
    {
      if ( entry.legendModeBefore != Qgis::LegendSyncMode::Manual )
        legend->setSyncMode( entry.legendModeBefore );
      else
        setLegendLabels( legend, entry.legendLabelsBefore );
      fitLegendToContent( legend );
    }

    for ( auto it = entry.movedItems.constBegin(); it != entry.movedItems.constEnd(); ++it )
    {
      if ( QgsLayoutItem *item = layout->itemByUuid( it.key() ) )
        item->attemptMove( QgsLayoutPoint( it.value() ) );
    }

    if ( QgsLayoutItemLabel *note = qobject_cast<QgsLayoutItemLabel *>( layout->itemByUuid( entry.noteUuid ) ) )
    {
      note->setText( entry.noteText );
      note->attemptMove( QgsLayoutPoint( entry.noteRect.topLeft() ) );
      note->attemptResize( QgsLayoutSize( entry.noteRect.size() ) );
    }

    int removedPages = 0;
    if ( layout->pageCollection() )
    {
      while ( layout->pageCollection()->pageCount() > entry.pageCountBefore )
      {
        layout->pageCollection()->deletePage( layout->pageCollection()->pageCount() - 1 );
        ++removedPages;
      }
    }

    QJsonObject diff;
    diff.insert( u"summary"_s, u"Removed layout items and pages added by AI tool."_s );
    diff.insert( u"layout_name"_s, entry.layoutName );
    diff.insert( u"removed_items"_s, removedItems );
    diff.insert( u"removed_pages"_s, removedPages );

    QJsonObject output;
    output.insert( u"status"_s, u"rolled_back"_s );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"diff"_s, diff );
    return QgsAiToolResult::ok( output );
  }

  QgsAiToolResult rollbackFileExport( const QString &token )
  {
    if ( !rollbackStore().contains( token ) )
      return QgsAiToolResult::error( u"Unknown or expired rollback token."_s );
    RollbackEntry entry = rollbackStore().take( token );
    if ( entry.type != RollbackType::DeleteFile )
      return QgsAiToolResult::error( u"Rollback token does not belong to an export-file operation."_s );

    if ( !entry.backupPath.isEmpty() && !QFileInfo::exists( entry.backupPath ) )
      return QgsAiToolResult::error( u"The copy of the replaced file is gone (%1); %2 was left as exported."_s.arg( entry.backupPath, entry.filePath ) );
    const bool existed = QFileInfo::exists( entry.filePath );
    bool removed = true;
    if ( existed )
      removed = QFile::remove( entry.filePath );
    if ( !removed )
      return QgsAiToolResult::error( u"Could not remove exported file: %1"_s.arg( entry.filePath ) );
    const bool restored = !entry.backupPath.isEmpty() && QFile::rename( entry.backupPath, entry.filePath );
    if ( !entry.backupPath.isEmpty() && !restored )
      return QgsAiToolResult::error( u"Removed the exported file but could not restore the previous one from %1"_s.arg( entry.backupPath ) );

    QJsonObject diff;
    diff.insert( u"summary"_s, restored ? u"Restored the file that the AI export replaced."_s : u"Removed exported file created by AI tool."_s );
    diff.insert( u"path"_s, entry.filePath );

    QJsonObject output;
    output.insert( u"status"_s, restored ? u"restored_previous"_s : existed ? u"rolled_back"_s : u"already_removed"_s );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"diff"_s, diff );
    return QgsAiToolResult::ok( output );
  }
} //namespace

QJsonObject qgsAiVerifyLayerQuality( QgsMapLayer *layer, QgsProject *project, bool *canceled )
{
  QJsonObject checks = layerQualityChecks( layer, project, canceled );
  const auto rasterChecks = qgsAiVerifyRasterSource( layer );
  bool passed = checks.value( u"passed"_s ).toBool();
  for ( auto it = rasterChecks.begin(); it != rasterChecks.end(); ++it )
  {
    checks.insert( it.key(), it.value() );
    passed = passed && it.value().toBool();
  }
  checks.insert( u"passed"_s, passed );
  return checks;
}

// ---------------------------------------------------------------------------
// add_layer_from_file
// ---------------------------------------------------------------------------

QgsAiAddLayerFromFileTool::QgsAiAddLayerFromFileTool( QgsAiFileContextProvider *contextProvider, QgsProject *project )
  : mContextProvider( contextProvider )
  , mProject( project )
{}

QString QgsAiAddLayerFromFileTool::description() const
{
  return QStringLiteral(
    "Loads a vector or raster file as a new map layer in the active QGIS project. "
    "Path may be workspace-relative or an absolute path to a file on disk. "
    "The layer kind is auto-detected from the file extension unless 'kind' is set explicitly. "
    "Returns the created layer's id, feature_count (vector) or band info (raster), CRS authid and extent. "
    "Use this instead of writing run_python with QgsVectorLayer/QgsRasterLayer when the user wants to "
    "load a single file — it's a single-shot, typed call with no Python quoting concerns."
  );
}

QJsonObject QgsAiAddLayerFromFileTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"path"_s, prop( u"string"_s, u"Workspace-relative or absolute path to the source file."_s ) );
  properties.insert( u"name"_s, prop( u"string"_s, u"Optional display name. Defaults to the file stem."_s ) );
  properties.insert( u"kind"_s, prop( u"string"_s, u"Optional. One of: 'vector', 'raster', 'auto'. Default 'auto' (detect by extension)."_s ) );
  properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token returned by a previous add_layer_from_file call. If set, removes that added layer."_s ) );
  return schemaObject( properties );
}

QgsAiToolResult QgsAiAddLayerFromFileTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !project )
    return QgsAiToolResult::error( u"No active QgsProject available."_s );

  const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
  if ( !rollbackToken.isEmpty() )
    return rollbackLayerAddition( project, rollbackToken );

  const QString rawPath = args.value( u"path"_s ).toString().trimmed();
  if ( rawPath.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'path' is required."_s );

  const QString path = resolvePath( mContextProvider, rawPath );
  if ( path.isEmpty() )
    return QgsAiToolResult::error( u"Cannot resolve path to an existing file: %1"_s.arg( rawPath ) );

  if ( QgsFileUtils::pathIsSidecarFile( path ) )
    return QgsAiToolResult::error( sidecarRefusalMessage( path ) );

  const int beforeLayerCount = project->mapLayers().size();

  QString kind = args.value( u"kind"_s ).toString().toLower().trimmed();
  if ( kind.isEmpty() || kind == "auto"_L1 )
    kind = detectKind( path );
  if ( kind.isEmpty() )
    return QgsAiToolResult::error( u"Cannot auto-detect layer kind from extension. Set 'kind' to 'vector' or 'raster'. Path: %1"_s.arg( path ) );

  const QString name = args.value( u"name"_s ).toString().trimmed().isEmpty() ? QFileInfo( path ).completeBaseName() : args.value( u"name"_s ).toString().trimmed();

  if ( kind != "vector"_L1 && kind != "raster"_L1 )
    return QgsAiToolResult::error( u"Unknown 'kind': %1 (expected 'vector' or 'raster')."_s.arg( kind ) );

  QJsonObject output;
  output.insert( u"kind"_s, kind );

  // The file is opened, checked and counted in the background: a large file or a slow disk
  // never freezes Strata, and Stop leaves no layer behind.
  QElapsedTimer phaseTimer;
  phaseTimer.start();
  auto load = std::make_shared<LayerToolsLoad>();
  const bool mayBeTable = fileMayBeNonSpatialTable( path );
  const QgsAiTaskWaitResult wait = layerToolsOpenInBackground(
    u"Loading %1"_s.arg( name ),
    [path, name, kind, mayBeTable]( LayerToolsLoad &load, QgsFeedback * ) -> std::unique_ptr<QgsMapLayer> {
      if ( kind == "vector"_L1 )
      {
        auto layer = std::make_unique<QgsVectorLayer>( path, name, u"ogr"_s );
        if ( !layer->isValid() )
        {
          load.error = u"Vector layer is invalid: %1 (provider error: %2)"_s.arg( path, layer->error().summary() );
          return nullptr;
        }
        const QString validationError = validateUsableVectorLayer( layer.get(), mayBeTable );
        if ( !validationError.isEmpty() )
        {
          load.error = u"Refusing to add unusable vector layer '%1': %2 No project layer was added."_s.arg( name, validationError );
          return nullptr;
        }
        load.facts.insert( u"feature_count"_s, static_cast<qint64>( layer->featureCount() ) );
        load.facts.insert( u"geometry_type"_s, QgsWkbTypes::geometryDisplayString( layer->geometryType() ) );
        return layer;
      }
      auto layer = std::make_unique<QgsRasterLayer>( path, name, u"gdal"_s );
      if ( !layer->isValid() )
      {
        load.error = u"Raster layer is invalid: %1 (provider error: %2)"_s.arg( path, layer->error().summary() );
        return nullptr;
      }
      load.facts.insert( u"width"_s, layer->width() );
      load.facts.insert( u"height"_s, layer->height() );
      load.facts.insert( u"bands"_s, layer->bandCount() );
      return layer;
    },
    load
  );
  qgsAiLogPerf( u"add_layer_from_file"_s, u"open_in_background"_s, phaseTimer.elapsed() );
  if ( wait.canceled || wait.abandoned )
    return QgsAiToolResult::canceledResult( u"Adding the layer was canceled."_s );
  if ( !load->error.isEmpty() )
    return QgsAiToolResult::error( load->error );
  if ( !wait.succeeded || !load->layer )
    return QgsAiToolResult::error( wait.error.isEmpty() ? u"The layer could not be opened: %1"_s.arg( path ) : wait.error );
  for ( auto it = load->facts.constBegin(); it != load->facts.constEnd(); ++it )
  {
    if ( it.key() != "extent"_L1 )
      output.insert( it.key(), it.value() );
  }
  QgsMapLayer *added = load->layer.release();

  phaseTimer.restart();
  project->addMapLayer( added );
  qgsAiLogPerf( u"add_layer_from_file"_s, u"addMapLayer"_s, phaseTimer.elapsed() );

  RollbackEntry rollback;
  rollback.type = RollbackType::RemoveLayer;
  rollback.layerId = added->id();
  rollback.description = u"Remove layer added from file."_s;
  const QString token = storeRollback( rollback );

  QJsonObject diff;
  diff.insert( u"summary"_s, u"Added a map layer to the project."_s );
  diff.insert( u"before_layer_count"_s, beforeLayerCount );
  diff.insert( u"after_layer_count"_s, project->mapLayers().size() );
  diff.insert( u"layer_id"_s, added->id() );

  output.insert( u"layer_id"_s, added->id() );
  output.insert( u"name"_s, added->name() );
  output.insert( u"crs"_s, added->crs().authid() );
  output.insert( u"crs_details"_s, qgsAiLayerCrsDetails( added ) );
  output.insert( u"source"_s, added->publicSource() );
  output.insert( u"spatial"_s, added->isSpatial() );
  output.insert( u"extent"_s, load->facts.contains( u"extent"_s ) ? load->facts.value( u"extent"_s ) : QJsonValue( QJsonValue::Null ) );
  output.insert( u"diff"_s, diff );
  output.insert( u"rollback_token"_s, token );
  output.insert( u"rollback"_s, rollbackJson( token, u"remove_added_layer"_s ) );
  phaseTimer.restart();
  const QPointer<QgsMapLayer> addedGuard( added );
  bool qualityChecksCanceled = false;
  const QJsonObject qualityChecks = qgsAiVerifyLayerQuality( added, project, &qualityChecksCanceled );
  qgsAiLogPerf( u"add_layer_from_file"_s, u"quality_check"_s, phaseTimer.elapsed() );
  if ( qualityChecksCanceled )
  {
    // Stop pressed while checking the new layer: undo the addition so the canceled call leaves no trace.
    rollbackStore().remove( token );
    if ( addedGuard )
      project->removeMapLayer( addedGuard->id() );
    return QgsAiToolResult::canceledResult( u"Adding the layer was canceled."_s );
  }
  if ( !addedGuard )
  {
    // Removed by the user while its values were checked: the checks above describe a layer that is gone.
    rollbackStore().remove( token );
    return QgsAiToolResult::error( u"The added layer was removed while it was being checked."_s );
  }
  output.insert( u"quality_checks"_s, qualityChecks );
  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// add_layer_from_service
// ---------------------------------------------------------------------------

QgsAiAddLayerFromServiceTool::QgsAiAddLayerFromServiceTool( QgsProject *project, QgsAiFileContextProvider *contextProvider )
  : mProject( project )
  , mContextProvider( contextProvider )
{}

QString QgsAiAddLayerFromServiceTool::description() const
{
  return QStringLiteral(
    "Loads a service-backed layer into the active QGIS project using native providers. "
    "Supported providers are wms, wfs, xyz, postgres and arcgis_mapserver (live by default; snapshot creates a verified static GeoTIFF). The tool validates the resulting "
    "layer, adds it to the project and returns a rollback token that removes the layer."
  );
}

QJsonObject QgsAiAddLayerFromServiceTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"provider"_s, prop( u"string"_s, u"One of: wms, wfs, xyz, postgres, arcgis_mapserver."_s ) );
  properties.insert( u"uri"_s, prop( u"string"_s, u"Provider URI. XYZ may be a full QGIS URI or a tile URL template."_s ) );
  properties.insert( u"name"_s, prop( u"string"_s, u"Optional display name for the new layer."_s ) );
  properties.insert( u"mode"_s, prop( u"string"_s, u"ArcGIS: live (default) or snapshot (static image)."_s ) );
  properties.insert( u"bbox"_s, prop( u"array"_s, u"Snapshot area [xmin,ymin,xmax,ymax] in the supplied CRS."_s ) );
  properties.insert( u"crs"_s, prop( u"string"_s, u"Area CRS such as EPSG:32632; required for snapshot."_s ) );
  properties.insert( u"layers"_s, prop( u"array"_s, u"ArcGIS numeric layer IDs; required for snapshot. Live accepts at most one layer ID."_s ) );
  properties.insert( u"destination"_s, prop( u"string"_s, u"New .tif path inside the workspace; required for snapshot. Never overwritten."_s ) );
  properties.insert( u"width"_s, prop( u"integer"_s, u"Requested snapshot width; both server image limits are respected."_s ) );
  properties.insert( u"height"_s, prop( u"integer"_s, u"Requested snapshot height."_s ) );
  properties.insert( u"layer_options"_s, prop( u"object"_s, u"Optional provider-specific URI parts to merge through QgsProviderMetadata."_s ) );
  properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token returned by a previous add_layer_from_service call. If set, removes that added layer."_s ) );
  return schemaObject( properties );
}

QgsAiToolResult QgsAiAddLayerFromServiceTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !project )
    return QgsAiToolResult::error( u"No active QgsProject available."_s );

  const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
  if ( !rollbackToken.isEmpty() )
    return rollbackLayerAddition( project, rollbackToken );

  const QString provider = args.value( u"provider"_s ).toString().toLower().trimmed();
  if ( provider.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'provider' is required."_s );

  const bool arcgis = provider == "arcgis_mapserver"_L1;
  const QString mode = args.value( u"mode"_s ).toString( u"live"_s );
  if ( arcgis && mode != "live"_L1 && mode != "snapshot"_L1 )
    return QgsAiToolResult::error( u"ArcGIS mode must be live or snapshot."_s );
  const bool snapshot = arcgis && mode == "snapshot"_L1;
  QString destination;
  if ( snapshot )
  {
    destination = mContextProvider ? mContextProvider->normalizePath( args.value( u"destination"_s ).toString() ) : QString();
    if ( destination.isEmpty() || QFileInfo::exists( destination ) || QFileInfo( destination ).suffix().compare( "tif"_L1, Qt::CaseInsensitive ) != 0 )
      return QgsAiToolResult::error( u"Snapshot destination must be a new .tif file inside the workspace."_s );
    const QString root = QFileInfo( mContextProvider->workspaceRoot() ).canonicalFilePath();
    QFileInfo ancestor( QFileInfo( destination ).absolutePath() );
    while ( !ancestor.exists() && ancestor.absoluteFilePath() != ancestor.absolutePath() )
      ancestor = QFileInfo( ancestor.absolutePath() );
    const QString resolved = ancestor.canonicalFilePath();
    if ( root.isEmpty() || resolved.isEmpty() || ( resolved != root && !resolved.startsWith( root + '/' ) ) )
      return QgsAiToolResult::error( u"Snapshot destination resolves outside the workspace."_s );
    if ( !QDir().mkpath( QFileInfo( destination ).absolutePath() ) )
      return QgsAiToolResult::error( u"Cannot create destination directory."_s );
  }
  const QString providerKey = arcgis ? ( snapshot ? u"gdal"_s : u"arcgismapserver"_s ) : serviceLayerProviderKey( provider );
  const QString layerType = arcgis ? u"raster"_s : serviceLayerType( provider );
  if ( providerKey.isEmpty() || layerType.isEmpty() )
    return QgsAiToolResult::error( u"Unsupported provider '%1'. Use one of: wms, wfs, xyz, postgres."_s.arg( provider ) );

  if ( provider == "wfs"_L1 )
  {
    QgsProviderRegistry *registry = QgsProviderRegistry::instance();
    if ( !registry || ( !registry->providerMetadata( u"WFS"_s ) && !registry->providerMetadata( u"wfs"_s ) ) )
    {
      return QgsAiToolResult::error(
        u"WFS provider is unavailable in this QGIS installation. Install or enable the QGIS WFS provider, verify that the providers plugin path is configured, then retry with a valid WFS endpoint and typename."_s
      );
    }
  }

  const QString rawUri = args.value( u"uri"_s ).toString().trimmed();
  if ( rawUri.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'uri' is required."_s );

  QString uri = normalizeServiceUri( provider, rawUri );
  if ( arcgis )
  {
    if ( snapshot )
      uri = destination;
    else
    {
      const auto layers = args.value( u"layers"_s ).toArray();
      if ( layers.size() > 1 )
        return QgsAiToolResult::error( u"A live MapServer layer accepts one layer ID, or no ID for the full map."_s );
      if ( !layers.isEmpty() && ( !layers.first().isDouble() || layers.first().toDouble() != layers.first().toInt() || layers.first().toInt() < 0 ) )
        return QgsAiToolResult::error( u"Layer IDs must be non-negative integers."_s );
      QgsDataSourceUri source;
      source.setParam( u"url"_s, rawUri );
      source.setParam( u"format"_s, u"png32"_s );
      if ( !layers.isEmpty() )
        source.setParam( u"layer"_s, QString::number( layers.first().toInt() ) );
      if ( args.contains( u"crs"_s ) )
        source.setParam( u"crs"_s, args.value( u"crs"_s ).toString() );
      uri = QString::fromUtf8( source.encodedUri() );
    }
  }
  if ( args.value( u"layer_options"_s ).isObject() )
  {
    QString optionsError;
    uri = applyServiceLayerOptions( providerKey, uri, args.value( u"layer_options"_s ).toObject(), optionsError );
    if ( uri.isEmpty() )
      return QgsAiToolResult::error( optionsError );
  }

  const QString name = ( args.value( u"name"_s ).toString().trimmed().isEmpty() ? provider.toUpper() : args.value( u"name"_s ).toString().trimmed() )
                       + ( snapshot ? u" (static snapshot)"_s : QString() );
  const int beforeLayerCount = project->mapLayers().size();

  QJsonObject output;
  output.insert( u"provider"_s, provider );
  output.insert( u"provider_key"_s, providerKey );
  output.insert( u"layer_type"_s, layerType );

  // Capabilities requests, database queries and the checks run in the background: a slow
  // service never freezes Strata, and Stop leaves no layer behind.
  QElapsedTimer phaseTimer;
  phaseTimer.start();
  auto load = std::make_shared<LayerToolsLoad>();
  const QgsAiTaskWaitResult wait = layerToolsOpenInBackground(
    u"Loading %1"_s.arg( name ),
    [uri, name, provider, providerKey, layerType, snapshot, destination, args]( LayerToolsLoad &load, QgsFeedback *feedback ) -> std::unique_ptr<QgsMapLayer> {
      if ( snapshot )
      {
        load.error = qgsAiCreateArcGisSnapshot( args, destination, feedback );
        if ( !load.error.isEmpty() )
          return nullptr;
        load.facts.insert( u"static_snapshot"_s, true );
      }
      if ( layerType == "raster"_L1 )
      {
        auto layer = std::make_unique<QgsRasterLayer>( uri, name, providerKey );
        if ( !layer->isValid() )
        {
          load.error = u"Service raster layer is invalid for provider '%1': %2"_s.arg( provider, layer->error().summary() );
          return nullptr;
        }
        load.facts.insert( u"width"_s, layer->width() );
        load.facts.insert( u"height"_s, layer->height() );
        load.facts.insert( u"bands"_s, layer->bandCount() );
        return layer;
      }
      auto layer = std::make_unique<QgsVectorLayer>( uri, name, providerKey );
      if ( !layer->isValid() )
      {
        if ( provider == "wfs"_L1 )
        {
          load.error = u"WFS layer could not be loaded. Verify that the endpoint is reachable, the URI includes a valid typename, the server advertises that layer in GetCapabilities, and authentication is configured. Provider detail: %1"_s
                         .arg( layer->error().summary() );
        }
        else
        {
          load.error = u"Service vector layer is invalid for provider '%1': %2"_s.arg( provider, layer->error().summary() );
        }
        return nullptr;
      }
      const bool allowNonSpatialTable = provider == "postgres"_L1 || provider == "postgis"_L1;
      const QString validationError = validateUsableVectorLayer( layer.get(), allowNonSpatialTable );
      if ( !validationError.isEmpty() )
      {
        const QString guidance = provider == "wfs"_L1 ? u" Verify the WFS typename, filters, server capabilities, and response payload."_s : QString();
        load.error = u"Refusing to add unusable service vector layer '%1': %2%3 No project layer was added."_s.arg( name, validationError, guidance );
        return nullptr;
      }
      load.facts.insert( u"feature_count"_s, static_cast<qint64>( layer->featureCount() ) );
      load.facts.insert( u"geometry_type"_s, QgsWkbTypes::geometryDisplayString( layer->geometryType() ) );
      return layer;
    },
    load
  );
  qgsAiLogPerf( u"add_layer_from_service"_s, u"open_in_background"_s, phaseTimer.elapsed() );
  if ( wait.canceled || wait.abandoned )
    return QgsAiToolResult::canceledResult( u"Adding the service layer was canceled."_s );
  if ( !load->error.isEmpty() )
    return QgsAiToolResult::error( load->error );
  if ( !wait.succeeded || !load->layer )
    return QgsAiToolResult::error( wait.error.isEmpty() ? u"The service layer could not be opened."_s : wait.error );
  for ( auto it = load->facts.constBegin(); it != load->facts.constEnd(); ++it )
  {
    if ( it.key() != "extent"_L1 )
      output.insert( it.key(), it.value() );
  }
  QgsMapLayer *added = load->layer.release();

  phaseTimer.restart();
  project->addMapLayer( added );
  qgsAiLogPerf( u"add_layer_from_service"_s, u"addMapLayer"_s, phaseTimer.elapsed() );

  RollbackEntry rollback;
  rollback.type = RollbackType::RemoveLayer;
  rollback.layerId = added->id();
  rollback.description = u"Remove layer added from service."_s;
  const QString token = storeRollback( rollback );

  QJsonObject diff;
  diff.insert( u"summary"_s, u"Added a service layer to the project."_s );
  diff.insert( u"before_layer_count"_s, beforeLayerCount );
  diff.insert( u"after_layer_count"_s, project->mapLayers().size() );
  diff.insert( u"layer_id"_s, added->id() );

  output.insert( u"layer_id"_s, added->id() );
  output.insert( u"name"_s, added->name() );
  output.insert( u"crs"_s, added->crs().authid() );
  output.insert( u"crs_details"_s, qgsAiLayerCrsDetails( added ) );
  output.insert( u"source"_s, added->publicSource() );
  output.insert( u"spatial"_s, added->isSpatial() );
  output.insert( u"extent"_s, load->facts.contains( u"extent"_s ) ? load->facts.value( u"extent"_s ) : QJsonValue( QJsonValue::Null ) );
  output.insert( u"diff"_s, diff );
  output.insert( u"rollback_token"_s, token );
  output.insert( u"rollback"_s, rollbackJson( token, u"remove_added_layer"_s ) );
  phaseTimer.restart();
  const QPointer<QgsMapLayer> addedGuard( added );
  bool qualityChecksCanceled = false;
  const QJsonObject qualityChecks = qgsAiVerifyLayerQuality( added, project, &qualityChecksCanceled );
  qgsAiLogPerf( u"add_layer_from_service"_s, u"quality_check"_s, phaseTimer.elapsed() );
  if ( qualityChecksCanceled )
  {
    // Stop pressed while checking the new layer: undo the addition so the canceled call leaves no trace.
    rollbackStore().remove( token );
    if ( addedGuard )
      project->removeMapLayer( addedGuard->id() );
    return QgsAiToolResult::canceledResult( u"Adding the layer was canceled."_s );
  }
  if ( !addedGuard )
  {
    // Removed by the user while its values were checked: the checks above describe a layer that is gone.
    rollbackStore().remove( token );
    return QgsAiToolResult::error( u"The added layer was removed while it was being checked."_s );
  }
  output.insert( u"quality_checks"_s, qualityChecks );
  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// reorder_layers
// ---------------------------------------------------------------------------

QgsAiReorderLayersTool::QgsAiReorderLayersTool( QgsProject *project )
  : mProject( project )
{}

QString QgsAiReorderLayersTool::description() const
{
  return u"Reorders direct root-level layers using QGIS's native reorder API. It never removes or reinserts layer-tree nodes, so registered map layers remain intact."_s;
}

QJsonObject QgsAiReorderLayersTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"layer_ids"_s, prop( u"array"_s, u"Ordered layer ids to move to the top of the root layer tree. All ids must be direct root-level layers."_s ) );
  return schemaObject( properties, QJsonArray { u"layer_ids"_s } );
}

QgsAiToolResult QgsAiReorderLayersTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  QgsLayerTreeGroup *root = project ? project->layerTreeRoot() : nullptr;
  if ( !root )
    return QgsAiToolResult::error( u"No active QGIS layer tree is available."_s );
  if ( !args.value( u"layer_ids"_s ).isArray() )
    return QgsAiToolResult::error( u"Argument 'layer_ids' must be an array."_s );

  QList<QgsMapLayer *> order;
  QSet<QString> seen;
  for ( const QJsonValue &value : args.value( u"layer_ids"_s ).toArray() )
  {
    const QString id = value.toString().trimmed();
    if ( id.isEmpty() || seen.contains( id ) )
      return QgsAiToolResult::error( u"layer_ids must contain unique non-empty layer ids."_s );
    QgsMapLayer *layer = project->mapLayer( id );
    QgsLayerTreeLayer *node = root->findLayer( id );
    if ( !layer || !node || node->parent() != root )
      return QgsAiToolResult::error( u"Layer '%1' is not a direct root-level layer."_s.arg( id ) );
    seen.insert( id );
    order << layer;
  }
  if ( order.isEmpty() )
    return QgsAiToolResult::error( u"layer_ids must contain at least one layer."_s );

  root->reorderGroupLayers( order );
  QJsonArray appliedOrder;
  for ( QgsLayerTreeNode *child : root->children() )
  {
    if ( QgsLayerTreeLayer *node = QgsLayerTree::toLayer( child ) )
      appliedOrder.append( node->layerId() );
  }
  QJsonObject output;
  output.insert( u"status"_s, u"reordered"_s );
  output.insert( u"layer_order"_s, appliedOrder );
  output.insert( u"note"_s, u"Used QgsLayerTreeGroup::reorderGroupLayers; no layer-tree node was removed or recreated."_s );
  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// describe_layer
// ---------------------------------------------------------------------------

QgsAiDescribeLayerTool::QgsAiDescribeLayerTool( QgsProject *project )
  : mProject( project )
{}

QString QgsAiDescribeLayerTool::description() const
{
  return QStringLiteral(
    "Describes a layer already loaded in the project. For vectors: id, name, geometry_type, "
    "feature_count, CRS, extent, fields[{name,type}], and (optionally) a small sample of "
    "feature attributes. For rasters: dimensions and band count. "
    "Use this BEFORE writing run_python that filters or aggregates by attribute — it lets you "
    "discover field names and types in one read-only call instead of two run_python rounds."
  );
}

QJsonObject QgsAiDescribeLayerTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"layer_id"_s, prop( u"string"_s, u"Id of the layer (as returned by list_project_layers / add_layer_from_file)."_s ) );
  properties.insert( u"sample_features"_s, prop( u"integer"_s, u"Optional number of feature attribute samples to include (default 0, hard cap 10)."_s ) );
  return schemaObject( properties, QJsonArray { u"layer_id"_s } );
}

QgsAiToolResult QgsAiDescribeLayerTool::execute( const QJsonObject &args )
{
  const QString layerId = args.value( u"layer_id"_s ).toString().trimmed();
  if ( layerId.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'layer_id' is required."_s );

  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !project )
    return QgsAiToolResult::error( u"No active QgsProject available."_s );

  QgsMapLayer *layer = project->mapLayer( layerId );
  if ( !layer )
    return QgsAiToolResult::error( u"No layer with id: %1"_s.arg( layerId ) );

  QgsLayerTree *root = project->layerTreeRoot();
  QgsLayerTreeLayer *treeNode = root ? root->findLayer( layerId ) : nullptr;

  const int requestedSamples = args.value( u"sample_features"_s ).toInt( 0 );
  const int maxSamples = std::clamp( requestedSamples, 0, 10 );

  QJsonObject output;
  output.insert( u"id"_s, layer->id() );
  output.insert( u"name"_s, layer->name() );
  output.insert( u"type"_s, QgsMapLayerFactory::typeToString( layer->type() ) );
  output.insert( u"crs"_s, layer->crs().authid() );
  output.insert( u"crs_details"_s, qgsAiLayerCrsDetails( layer ) );
  output.insert( u"source"_s, layer->publicSource() );
  output.insert( u"extent"_s, extentJson( layer->extent() ) );
  output.insert( u"visual"_s, layerVisualSummary( layer, treeNode ) );

  if ( QgsVectorLayer *vector = qobject_cast<QgsVectorLayer *>( layer ) )
  {
    output.insert( u"geometry_type"_s, QgsWkbTypes::geometryDisplayString( vector->geometryType() ) );
    output.insert( u"feature_count"_s, static_cast<qint64>( vector->featureCount() ) );

    QJsonArray fields;
    const QgsFields layerFields = vector->fields();
    for ( const QgsField &field : layerFields )
    {
      QJsonObject f;
      f.insert( u"name"_s, field.name() );
      f.insert( u"type"_s, field.typeName() );
      fields.push_back( f );
    }
    output.insert( u"fields"_s, fields );

    if ( maxSamples > 0 )
    {
      QJsonArray samples;
      QgsFeatureRequest request;
      request.setLimit( maxSamples );
      request.setFlags( Qgis::FeatureRequestFlag::NoGeometry );
      QgsFeatureIterator it = vector->getFeatures( request );
      QgsFeature feature;
      while ( it.nextFeature( feature ) && samples.size() < maxSamples )
      {
        QJsonObject row;
        for ( const QgsField &field : layerFields )
        {
          const QVariant value = feature.attribute( field.name() );
          row.insert( field.name(), QJsonValue::fromVariant( value ) );
        }
        samples.push_back( row );
      }
      output.insert( u"sample_features"_s, samples );
    }
  }
  else if ( QgsRasterLayer *raster = qobject_cast<QgsRasterLayer *>( layer ) )
  {
    output.insert( u"width"_s, raster->width() );
    output.insert( u"height"_s, raster->height() );
    output.insert( u"bands"_s, raster->bandCount() );
  }

  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// run_processing_algorithm
// ---------------------------------------------------------------------------

QgsAiRunProcessingAlgorithmTool::QgsAiRunProcessingAlgorithmTool( QgsProject *project )
  : mProject( project )
{}

QString QgsAiRunProcessingAlgorithmTool::description() const
{
  return QStringLiteral(
    "Runs a QGIS Processing algorithm by id with JSON parameters, or returns a dry-run schema for the algorithm. "
    "Use this native tool for common GIS transformations (buffers, dissolve, reprojection, statistics, raster tools) "
    "instead of generating run_python code. Output layers (including TEMPORARY_OUTPUT memory layers) are added to the "
    "current project like the QGIS Toolbox; they are not discarded when the tool returns. Mutating or output-producing "
    "runs require approval."
  );
}

QJsonObject QgsAiRunProcessingAlgorithmTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"algorithm_id"_s, prop( u"string"_s, u"QGIS Processing algorithm id, e.g. 'native:buffer'."_s ) );
  properties.insert(
    u"parameters"_s,
    prop(
      u"object"_s,
      u"Algorithm parameters as a JSON object. Use dry_run first if unsure. Layer destinations such as OUTPUT or OUTPUT_LINES default to TEMPORARY_OUTPUT and are loaded into the project. "
      u"A vector input (feature source) can be an object to process part of a layer: {\"source\": \"<layer_id>\", \"selected_only\": true} for the selected features, "
      u"{\"source\": \"<layer_id>\", \"filter\": \"<QGIS expression>\"} for the matching ones, optionally with \"feature_limit\"."_s
    )
  );
  properties.insert( u"configuration"_s, prop( u"object"_s, u"Optional Processing algorithm configuration map."_s ) );
  properties.insert( u"dry_run"_s, prop( u"boolean"_s, u"If true, returns algorithm parameter/output metadata without executing it."_s ) );
  properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token returned by a previous run. If set, removes the output layers that run loaded into the project (files it wrote stay)."_s ) );
  return schemaObject( properties, QJsonArray { u"algorithm_id"_s } );
}

bool QgsAiRunProcessingAlgorithmTool::isAvailable() const
{
  return QgsApplication::processingRegistry();
}

QString QgsAiRunProcessingAlgorithmTool::availabilityReason() const
{
  return u"QGIS Processing registry is not available."_s;
}

namespace
{
  /**
   * Project layers read by the algorithm's input parameters. Declared as the task's dependent
   * layers, they make QGIS refuse to remove them or close the project until the task ends.
   * Never loads a layer: only references already resolvable in the context are returned.
   */
  QList<QgsMapLayer *> processingToolInputProjectLayers( const QgsProcessingAlgorithm *algorithm, const QVariantMap &parameters, QgsProcessingContext &context, QgsProject *project )
  {
    QList<QgsMapLayer *> layers;
    if ( !algorithm || !project )
      return layers;

    static const QSet<QString> layerParameterTypes {
      QgsProcessingParameterFeatureSource::typeName(),
      QgsProcessingParameterVectorLayer::typeName(),
      QgsProcessingParameterRasterLayer::typeName(),
      QgsProcessingParameterMapLayer::typeName(),
      QgsProcessingParameterMultipleLayers::typeName(),
      QgsProcessingParameterMeshLayer::typeName(),
      QgsProcessingParameterPointCloudLayer::typeName(),
      QgsProcessingParameterAnnotationLayer::typeName(),
    };

    std::function<void( const QVariant & )> collect;
    collect = [&collect, &layers, &context, project]( const QVariant &value ) {
      if ( value.userType() == QMetaType::Type::QVariantList || value.userType() == QMetaType::Type::QStringList )
      {
        const QVariantList values = value.toList();
        for ( const QVariant &item : values )
          collect( item );
        return;
      }
      if ( value.userType() == qMetaTypeId<QgsProcessingFeatureSourceDefinition>() )
      {
        collect( value.value<QgsProcessingFeatureSourceDefinition>().source.staticValue() );
        return;
      }
      if ( value.userType() == qMetaTypeId<QgsProperty>() )
      {
        const QgsProperty property = value.value<QgsProperty>();
        if ( property.propertyType() == Qgis::PropertyType::Static )
          collect( property.staticValue() );
        return;
      }

      QgsMapLayer *layer = qobject_cast<QgsMapLayer *>( qvariant_cast<QObject *>( value ) );
      if ( !layer && value.userType() == QMetaType::Type::QString )
        layer = QgsProcessingUtils::mapLayerFromString( value.toString(), context, false );
      if ( layer && project->mapLayer( layer->id() ) == layer && !layers.contains( layer ) )
        layers << layer;
    };

    const QgsProcessingParameterDefinitions definitions = algorithm->parameterDefinitions();
    for ( const QgsProcessingParameterDefinition *definition : definitions )
    {
      if ( definition->isDestination() || !layerParameterTypes.contains( definition->type() ) )
        continue;
      collect( parameters.value( definition->name() ) );
    }
    return layers;
  }

  // Same thread split as QgsProcessingAlgRunnerTask (prepare on the GUI thread,
  // runPrepared on the worker, postProcess back on the GUI thread) and keeps the
  // optional algorithm configuration map, which the stock task drops.
  // It shares ownership of the context and feedback so it outlives an abandoned wait.
  class AiProcessingRunnerTask : public QgsAiBackgroundTask
  {
    public:
      AiProcessingRunnerTask(
        const QgsProcessingAlgorithm *algorithm, const QVariantMap &parameters, const QVariantMap &configuration, std::shared_ptr<QgsProcessingContext> context, std::shared_ptr<QgsProcessingFeedback> feedback
      )
        : QgsAiBackgroundTask( QObject::tr( "Executing “%1”" ).arg( algorithm ? algorithm->displayName() : QString() ), feedback )
        , mParameters( parameters )
        , mContext( std::move( context ) )
        , mProcessingFeedback( std::move( feedback ) )
      {
        if ( !algorithm || !mContext || !mProcessingFeedback )
        {
          cancel();
          return;
        }
        try
        {
          mAlgorithm.reset( algorithm->create( configuration ) );
          if ( !( mAlgorithm && mAlgorithm->prepare( mParameters, *mContext, mProcessingFeedback.get() ) ) )
            cancel();
        }
        catch ( QgsProcessingException &e )
        {
          QgsMessageLog::logMessage( e.what(), QObject::tr( "Processing" ), Qgis::MessageLevel::Critical );
          mProcessingFeedback->reportError( e.what() );
          cancel();
        }
      }

      QVariantMap results() const { return mPublishedResults; }
      bool succeeded() const { return mSucceeded; }

    protected:
      bool run() override
      {
        if ( isCanceled() || !mAlgorithm )
          return false;

        connect( mProcessingFeedback.get(), &QgsFeedback::progressChanged, this, &AiProcessingRunnerTask::setProgress );
        try
        {
          mRunResults = mAlgorithm->runPrepared( mParameters, *mContext, mProcessingFeedback.get() );
          return !mProcessingFeedback->isCanceled();
        }
        catch ( QgsProcessingException &e )
        {
          QgsMessageLog::logMessage( e.what(), QObject::tr( "Processing" ), Qgis::MessageLevel::Critical );
          mProcessingFeedback->reportError( e.what() );
          return false;
        }
      }

      void finished( bool result ) override
      {
        QVariantMap postProcessed;
        if ( mAlgorithm )
          postProcessed = mAlgorithm->postProcess( *mContext, mProcessingFeedback.get(), result );
        mSucceeded = result && !mProcessingFeedback->isCanceled();
        mPublishedResults = !postProcessed.isEmpty() ? postProcessed : mRunResults;
      }

    private:
      QVariantMap mParameters;
      QVariantMap mRunResults;
      QVariantMap mPublishedResults;
      std::shared_ptr<QgsProcessingContext> mContext;
      std::shared_ptr<QgsProcessingFeedback> mProcessingFeedback;
      std::unique_ptr<QgsProcessingAlgorithm> mAlgorithm;
      bool mSucceeded = false;
  };
} // namespace

QgsAiToolResult QgsAiRunProcessingAlgorithmTool::execute( const QJsonObject &args )
{
  const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
  if ( !rollbackToken.isEmpty() )
    return rollbackLoadedLayers( mProject ? mProject : QgsProject::instance(), rollbackToken );

  QgsProcessingRegistry *registry = QgsApplication::processingRegistry();
  if ( !registry )
    return QgsAiToolResult::error( availabilityReason() );

  const QString algorithmId = args.value( u"algorithm_id"_s ).toString().trimmed();
  if ( algorithmId.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'algorithm_id' is required."_s );

  const QgsProcessingAlgorithm *algorithm = registry->algorithmById( algorithmId );
  if ( !algorithm )
    return QgsAiToolResult::error( u"Unknown Processing algorithm: %1"_s.arg( algorithmId ) );

  QString canExecuteMessage;
  if ( !algorithm->canExecute( &canExecuteMessage ) )
    return QgsAiToolResult::error( canExecuteMessage.isEmpty() ? u"Processing algorithm cannot execute."_s : canExecuteMessage );

  if ( args.value( u"dry_run"_s ).toBool( false ) )
  {
    QJsonObject output = processingAlgorithmMetadataJson( algorithm );
    output.insert( u"dry_run"_s, true );
    return QgsAiToolResult::ok( output );
  }

  if ( !args.value( u"parameters"_s ).isObject() )
    return QgsAiToolResult::error( u"Argument 'parameters' must be an object. Call with dry_run=true to inspect required parameters."_s );

  QgsProject *project = mProject ? mProject : QgsProject::instance();
  // Shared with the task: it may outlive this call if Strata quits mid-run.
  auto context = std::make_shared<QgsProcessingContext>();
  if ( project )
    context->setProject( project );

  QVariantMap parameters = normalizeSingleEnumParameters( algorithm, jsonObjectToVariantMap( args.value( u"parameters"_s ).toObject() ) );
  QString sourceError;
  if ( !resolveFeatureSourceDefinitions( algorithm, parameters, *context, sourceError ) )
    return QgsAiToolResult::error( sourceError );
  bindProcessingDestinationsToProject( algorithm, parameters, project );
  const QVariantMap configuration = args.value( u"configuration"_s ).isObject() ? jsonObjectToVariantMap( args.value( u"configuration"_s ).toObject() ) : QVariantMap();

  auto feedback = std::make_shared<QgsProcessingFeedback>();
  context->setFeedback( feedback.get() );

  QString checkMessage;
  if ( !algorithm->checkParameterValues( parameters, *context, &checkMessage ) )
    return QgsAiToolResult::error( checkMessage.isEmpty() ? u"Processing parameter validation failed."_s : checkMessage );

  if ( !QgsApplication::taskManager() )
    return QgsAiToolResult::error( u"Processing task manager is not available."_s );

  const auto failedResult = [&feedback]() {
    const QString log = feedback->textLog().trimmed();
    return QgsAiToolResult::error( u"Processing algorithm failed: %1"_s.arg( log.isEmpty() ? u"unknown error"_s : log ) );
  };

  // Read everything needed from the registry's algorithm now: reloading scripts or providers
  // during the background wait below would delete it.
  QJsonObject output = processingAlgorithmMetadataJson( algorithm );
  const QString resolvedAlgorithmId = algorithm->id();

  QVariantMap results;
  if ( algorithm->flags() & Qgis::ProcessingAlgorithmFlag::NoThreading )
  {
    // Like the Toolbox: algorithms that touch the GUI or the project run on this thread.
    const QgsAiActiveFeedbackScope activeFeedback( feedback.get(), algorithm->displayName() );
    bool ok = false;
    try
    {
      results = algorithm->run( parameters, *context, feedback.get(), &ok, configuration );
    }
    catch ( QgsProcessingException &e )
    {
      QgsMessageLog::logMessage( e.what(), QObject::tr( "Processing" ), Qgis::MessageLevel::Critical );
      return QgsAiToolResult::error( e.what() );
    }
    if ( activeFeedback.canceledByUser() )
      return QgsAiToolResult::canceledResult( u"Processing algorithm was canceled."_s );
    if ( !ok )
      return failedResult();
  }
  else
  {
    // prepare() runs on this thread; runPrepared() runs on the task thread.
    // The nested event loop keeps the window painting and lets Stop cancel the feedback.
    auto *task = new AiProcessingRunnerTask( algorithm, parameters, configuration, context, feedback );
    task->setDependentLayers( processingToolInputProjectLayers( algorithm, parameters, *context, project ) );

    // Inputs that are files, not project layers, do not block closing the project: notice it,
    // so the outputs never land in the project opened meanwhile.
    bool projectCleared = false;
    QMetaObject::Connection clearedConnection;
    if ( project )
      clearedConnection = QObject::connect( project, &QgsProject::cleared, [&projectCleared]() { projectCleared = true; } );

    bool successful = false;
    const QgsAiTaskWaitResult wait = qgsAiRunTaskWithEventLoop( task, algorithm->displayName(), [task, &successful, &results]() {
      successful = task->succeeded();
      results = task->results();
    } );
    QObject::disconnect( clearedConnection );

    if ( wait.abandoned )
      return QgsAiToolResult::canceledResult( wait.error );
    if ( wait.layerRemoved )
      return QgsAiToolResult::error( u"An input layer was removed while the algorithm was running."_s );
    // A failed prepare() also cancels the feedback, but only a user cancel stops the turn.
    if ( wait.canceled )
      return QgsAiToolResult::canceledResult( u"Processing algorithm was canceled."_s );
    if ( !successful )
      return failedResult();
    if ( projectCleared )
      return QgsAiToolResult::error( u"The project was closed while the algorithm was running; its outputs were not loaded."_s );
  }

  const QJsonArray loadedLayers = loadProcessingResultsIntoProject( *context, project );
  QJsonObject diff;
  diff.insert( u"summary"_s, loadedLayers.isEmpty() ? u"Executed a QGIS Processing algorithm."_s : u"Executed a QGIS Processing algorithm and loaded the output layers into the project."_s );
  diff.insert( u"algorithm_id"_s, resolvedAlgorithmId );
  // Undo takes the loaded outputs out of the project; files the algorithm wrote stay on disk.
  diff.insert( u"rollback_supported"_s, !loadedLayers.isEmpty() );
  if ( !loadedLayers.isEmpty() )
  {
    RollbackEntry rollback;
    rollback.type = RollbackType::RemoveLoadedLayers;
    for ( const QJsonValue &layer : loadedLayers )
      rollback.layerIds << layer.toObject().value( u"id"_s ).toString();
    rollback.description = u"Remove the output layers loaded by a Processing run."_s;
    const QString token = storeRollback( rollback );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"rollback"_s, rollbackJson( token, u"remove_loaded_layers"_s ) );
    // "Show on map" goes to the first output.
    output.insert( u"layer_id"_s, loadedLayers.at( 0 ).toObject().value( u"id"_s ).toString() );
  }
  output.insert( u"dry_run"_s, false );
  output.insert( u"result"_s, QJsonObject::fromVariantMap( results ) );
  output.insert( u"loaded_layers"_s, loadedLayers );
  output.insert( u"log"_s, feedback->textLog() );
  output.insert( u"diff"_s, diff );
  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// style_layer
// ---------------------------------------------------------------------------

QgsAiStyleLayerTool::QgsAiStyleLayerTool( QgsProject *project )
  : mProject( project )
{}

QString QgsAiStyleLayerTool::description() const
{
  return QStringLiteral(
    "Applies common native styling changes to a loaded layer: opacity, visibility and single-symbol vector color. "
    "Use this instead of run_python for simple layer styling. It requires approval because it mutates the project."
  );
}

QJsonObject QgsAiStyleLayerTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"layer_id"_s, prop( u"string"_s, u"Target layer id."_s ) );
  properties.insert( u"opacity"_s, prop( u"number"_s, u"Optional opacity between 0 and 1."_s ) );
  properties.insert( u"visible"_s, prop( u"boolean"_s, u"Optional layer tree visibility."_s ) );
  properties.insert( u"color"_s, prop( u"string"_s, u"Optional vector single-symbol color, e.g. '#2F80ED'."_s ) );
  properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token returned by a previous style_layer call. If set, restores the previous style."_s ) );
  return schemaObject( properties );
}

QgsAiToolResult QgsAiStyleLayerTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !project )
    return QgsAiToolResult::error( u"No active QgsProject available."_s );

  const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
  if ( !rollbackToken.isEmpty() )
    return rollbackLayerStyle( project, rollbackToken );

  const QString layerId = args.value( u"layer_id"_s ).toString().trimmed();
  if ( layerId.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'layer_id' is required."_s );

  QgsMapLayer *layer = project->mapLayer( layerId );
  if ( !layer )
    return QgsAiToolResult::error( u"No layer with id: %1"_s.arg( layerId ) );

  QgsLayerTreeLayer *treeNode = project->layerTreeRoot() ? project->layerTreeRoot()->findLayer( layerId ) : nullptr;
  const QJsonObject beforeVisual = layerVisualSummary( layer, treeNode );
  QgsMapLayerStyle beforeStyle;
  beforeStyle.readFromLayer( layer );
  RollbackEntry rollback;
  rollback.type = RollbackType::RestoreLayerStyle;
  rollback.layerId = layerId;
  rollback.styleXml = beforeStyle.xmlData();
  rollback.hasVisibility = treeNode != nullptr;
  rollback.visible = treeNode ? treeNode->itemVisibilityChecked() : true;
  rollback.description = u"Restore layer style before AI styling."_s;

  QJsonArray changes;
  if ( args.contains( u"opacity"_s ) )
  {
    const double opacity = std::clamp( args.value( u"opacity"_s ).toDouble( layer->opacity() ), 0.0, 1.0 );
    layer->setOpacity( opacity );
    changes.append( u"opacity"_s );
  }

  if ( treeNode && args.contains( u"visible"_s ) )
  {
    treeNode->setItemVisibilityChecked( args.value( u"visible"_s ).toBool() );
    changes.append( u"visibility"_s );
  }

  if ( args.contains( u"color"_s ) )
  {
    QgsVectorLayer *vector = qobject_cast<QgsVectorLayer *>( layer );
    if ( !vector )
      return QgsAiToolResult::error( u"Color styling is currently supported for vector layers only."_s );

    const QColor color( args.value( u"color"_s ).toString().trimmed() );
    if ( !color.isValid() )
      return QgsAiToolResult::error( u"Invalid color. Use a CSS-style color such as '#2F80ED'."_s );

    std::unique_ptr<QgsSymbol> symbol( QgsSymbol::defaultSymbol( vector->geometryType() ) );
    if ( !symbol )
      return QgsAiToolResult::error( u"Could not create a default symbol for the layer geometry."_s );

    symbol->setColor( color );
    vector->setRenderer( new QgsSingleSymbolRenderer( symbol.release() ) );
    changes.append( u"color"_s );
  }

  layer->triggerRepaint();
  if ( QgsVectorLayer *vector = qobject_cast<QgsVectorLayer *>( layer ) )
    vector->emitStyleChanged();
  refreshLayoutLegends( project, layer->id() );

  QJsonObject output;
  const QString token = storeRollback( rollback );
  QJsonObject diff;
  diff.insert( u"summary"_s, changes.isEmpty() ? u"No style changes requested."_s : u"Applied native layer styling changes."_s );
  diff.insert( u"changes"_s, changes );
  diff.insert( u"before"_s, beforeVisual );
  diff.insert( u"after"_s, layerVisualSummary( layer, treeNode ) );

  output.insert( u"layer_id"_s, layer->id() );
  output.insert( u"name"_s, layer->name() );
  output.insert( u"visual"_s, layerVisualSummary( layer, treeNode ) );
  output.insert( u"diff"_s, diff );
  output.insert( u"rollback_token"_s, token );
  output.insert( u"rollback"_s, rollbackJson( token, u"restore_layer_style"_s ) );
  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// style_layer_advanced
// ---------------------------------------------------------------------------

QgsAiAdvancedStyleTool::QgsAiAdvancedStyleTool( QgsProject *project )
  : mProject( project )
{}

QString QgsAiAdvancedStyleTool::description() const
{
  return QStringLiteral(
    "Applies advanced native vector styling: categorized and graduated renderers, "
    "plus basic field labeling. Graduated classes come from a QGIS classification mode (default jenks, "
    "natural breaks, which suits skewed values; equal_interval suits evenly spread values) and are colored "
    "with a color ramp, light to dark by default. Class labels are rounded to readable precision. "
    "The result lists each graduated class with its label, color and feature_count: check the counts, "
    "since a class holding almost every feature makes the map hard to read. "
    "Returns a rollback token that restores the previous renderer, labeling and layer visual settings."
  );
}

QJsonObject QgsAiAdvancedStyleTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"layer_id"_s, prop( u"string"_s, u"Target vector layer id."_s ) );
  properties.insert( u"renderer"_s, prop( u"string"_s, u"Renderer type: categorized or graduated."_s ) );
  properties.insert( u"field"_s, prop( u"string"_s, u"Attribute field used by the renderer."_s ) );
  properties.insert( u"class_count"_s, prop( u"integer"_s, u"Optional number of graduated classes, 1 to 32. Default 5."_s ) );
  properties.insert( u"mode"_s, prop( u"string"_s, u"Optional graduated classification mode: jenks (default), quantile, equal_interval, pretty, stddev or logarithmic."_s ) );
  properties.insert( u"classes"_s, propArray( prop( u"object"_s, u"Graduated: {lower, upper, label?, color?}. Categorized: {value, label?, color?}. Color as #RRGGBB."_s ), u"Optional explicit classes, instead of computing them from mode and class_count."_s ) );
  properties.insert( u"color_ramp"_s, prop( u"string"_s, u"Optional QGIS color ramp name, e.g. Reds, Blues, YlOrRd, Viridis, RdYlGn. Graduated default: the project default ramp, else white to red. Categorized default: distinct colors."_s ) );
  properties.insert( u"invert_ramp"_s, prop( u"boolean"_s, u"Optional: reverse the color ramp."_s ) );
  properties.insert( u"label_precision"_s, prop( u"integer"_s, u"Optional decimal places in graduated class labels. Default: chosen from the class widths, trailing zeros trimmed."_s ) );
  properties.insert( u"labels"_s, prop( u"object"_s, u"Optional labeling config, e.g. {\"enabled\":true,\"field\":\"name\"}."_s ) );
  properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token returned by a previous style_layer_advanced call. If set, restores the previous style."_s ) );
  return schemaObject( properties );
}

QgsAiToolResult QgsAiAdvancedStyleTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !project )
    return QgsAiToolResult::error( u"No active QgsProject available."_s );

  const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
  if ( !rollbackToken.isEmpty() )
    return rollbackLayerStyle( project, rollbackToken );

  const QString layerId = args.value( u"layer_id"_s ).toString().trimmed();
  if ( layerId.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'layer_id' is required."_s );

  QgsVectorLayer *layer = qobject_cast<QgsVectorLayer *>( project->mapLayer( layerId ) );
  if ( !layer )
    return QgsAiToolResult::error( u"No vector layer with id: %1"_s.arg( layerId ) );

  const QString renderer = args.value( u"renderer"_s ).toString().toLower().trimmed();
  const bool hasLabelRequest = args.contains( u"labels"_s );
  if ( renderer.isEmpty() && !hasLabelRequest )
    return QgsAiToolResult::error( u"Argument 'renderer' or 'labels' is required."_s );
  if ( !renderer.isEmpty() && renderer != "categorized"_L1 && renderer != "graduated"_L1 )
    return QgsAiToolResult::error( u"Unsupported advanced renderer '%1'. Supported renderers: categorized, graduated."_s.arg( renderer ) );

  const QString field = args.value( u"field"_s ).toString().trimmed();
  int fieldIndex = -1;
  if ( !renderer.isEmpty() )
  {
    if ( field.isEmpty() )
      return QgsAiToolResult::error( u"Argument 'field' is required when renderer is set."_s );
    fieldIndex = layer->fields().lookupField( field );
    if ( fieldIndex < 0 )
      return QgsAiToolResult::error( u"Renderer field does not exist: %1"_s.arg( field ) );
  }

  if ( hasLabelRequest )
  {
    const QJsonValue labelsValue = args.value( u"labels"_s );
    if ( labelsValue.isObject() ? labelsValue.toObject().value( u"enabled"_s ).toBool( true ) : labelsValue.toBool( false ) )
    {
      const QString labelField = labelsValue.isObject() ? labelsValue.toObject().value( u"field"_s ).toString( field ).trimmed() : field;
      if ( labelField.isEmpty() )
        return QgsAiToolResult::error( u"Label field is required when labels are enabled."_s );
      if ( layer->fields().lookupField( labelField ) < 0 )
        return QgsAiToolResult::error( u"Label field does not exist: %1"_s.arg( labelField ) );
    }
  }

  const QJsonValue classesValue = args.value( u"classes"_s );
  const QJsonArray classArray = classesValue.toArray();
  // older callers pass the class count as classes, as a number or {"count": n}
  const int legacyClassCount = classesValue.isDouble() ? classesValue.toInt() : classesValue.toObject().value( u"count"_s ).toInt( 5 );
  const int classCount = std::clamp( args.value( u"class_count"_s ).toInt( legacyClassCount ), 1, 32 );
  const QJsonValue precisionValue = args.value( u"label_precision"_s );

  static const QMap<QString, QString>
    modeIds { { u"jenks"_s, u"Jenks"_s }, { u"quantile"_s, u"Quantile"_s }, { u"equal_interval"_s, u"EqualInterval"_s }, { u"pretty"_s, u"Pretty"_s }, { u"stddev"_s, u"StdDev"_s }, { u"logarithmic"_s, u"Logarithmic"_s } };
  QString mode = args.value( u"mode"_s ).toString().trimmed().toLower();
  if ( mode.isEmpty() )
    mode = u"jenks"_s;
  if ( renderer == "graduated"_L1 && !modeIds.contains( mode ) )
    return QgsAiToolResult::error( u"Unsupported mode '%1'. Use one of: %2."_s.arg( mode, QStringList( modeIds.keys() ).join( ", "_L1 ) ) );

  // graduated default as in the QGIS layer styling panel: the project default ramp, else white to red
  std::unique_ptr<QgsColorRamp> ramp;
  const QString rampName = args.value( u"color_ramp"_s ).toString().trimmed();
  if ( !rampName.isEmpty() )
  {
    ramp = QgsStyle::defaultStyle()->colorRamp( rampName );
    if ( !ramp )
      return QgsAiToolResult::error( u"Unknown color ramp '%1'. Available ramps: %2."_s.arg( rampName, QgsStyle::defaultStyle()->colorRampNames().join( ", "_L1 ) ) );
  }
  else if ( renderer == "graduated"_L1 )
  {
    ramp.reset( project->styleSettings()->defaultColorRamp() );
    if ( !ramp )
      ramp = std::make_unique<QgsGradientColorRamp>( QColor( 255, 255, 255 ), QColor( 255, 0, 0 ) );
  }
  if ( ramp && args.value( u"invert_ramp"_s ).toBool( false ) )
    ramp->invert();
  const auto explicitColor = [&classArray]( int index ) { return QColor( classArray.at( index ).toObject().value( u"color"_s ).toString() ); };

  QgsLayerTreeLayer *treeNode = project->layerTreeRoot() ? project->layerTreeRoot()->findLayer( layerId ) : nullptr;
  const QJsonObject beforeVisual = layerVisualSummary( layer, treeNode );
  QgsMapLayerStyle beforeStyle;
  beforeStyle.readFromLayer( layer );
  RollbackEntry rollback;
  rollback.type = RollbackType::RestoreLayerStyle;
  rollback.layerId = layerId;
  rollback.styleXml = beforeStyle.xmlData();
  rollback.hasVisibility = treeNode != nullptr;
  rollback.visible = treeNode ? treeNode->itemVisibilityChecked() : true;
  rollback.description = u"Restore layer style before AI advanced styling."_s;

  QJsonArray changes;
  QJsonArray classesJson;
  long long unclassifiedCount = 0;
  if ( renderer == "categorized"_L1 )
  {
    QgsCategoryList categories;
    if ( !classArray.isEmpty() )
    {
      for ( int i = 0; i < classArray.size(); ++i )
      {
        const QJsonObject classObject = classArray.at( i ).toObject();
        std::unique_ptr<QgsSymbol> symbol( defaultSymbolWithColor( layer->geometryType(), i ) );
        if ( !symbol )
          return QgsAiToolResult::error( u"Could not create a default symbol for categorized renderer."_s );
        const QVariant value = classObject.value( u"value"_s ).toVariant();
        const QString label = classObject.value( u"label"_s ).toString( value.toString() );
        categories.append( QgsRendererCategory( value, symbol.release(), label ) );
      }
    }
    else
    {
      QList<QVariant> values = layer->uniqueValues( fieldIndex, 256 ).values();
      std::sort( values.begin(), values.end(), []( const QVariant &a, const QVariant &b ) { return a.toString() < b.toString(); } );
      for ( int i = 0; i < values.size(); ++i )
      {
        std::unique_ptr<QgsSymbol> symbol( defaultSymbolWithColor( layer->geometryType(), i ) );
        if ( !symbol )
          return QgsAiToolResult::error( u"Could not create a default symbol for categorized renderer."_s );
        categories.append( QgsRendererCategory( values.at( i ), symbol.release(), values.at( i ).toString() ) );
      }
    }

    auto categorized = std::make_unique<QgsCategorizedSymbolRenderer>( field, categories );
    if ( ramp )
      categorized->updateColorRamp( ramp.release() );
    for ( int i = 0; i < classArray.size(); ++i )
    {
      if ( !explicitColor( i ).isValid() )
        continue;
      std::unique_ptr<QgsSymbol> symbol( categorized->categories().at( i ).symbol()->clone() );
      symbol->setColor( explicitColor( i ) );
      categorized->updateCategorySymbol( i, symbol.release() );
    }
    layer->setRenderer( categorized.release() );
    changes.append( u"categorized_renderer"_s );
  }
  else if ( renderer == "graduated"_L1 )
  {
    std::unique_ptr<QgsSymbol> baseSymbol( QgsSymbol::defaultSymbol( layer->geometryType() ) );
    if ( !baseSymbol )
      return QgsAiToolResult::error( u"Could not create a default symbol for graduated renderer."_s );
    auto graduated = std::make_unique<QgsGraduatedSymbolRenderer>( field );
    graduated->setSourceSymbol( baseSymbol->clone() );
    graduated->setSourceColorRamp( ramp.release() );
    std::unique_ptr<QgsClassificationMethod> method = QgsApplication::classificationMethodRegistry()->method( classArray.isEmpty() ? modeIds.value( mode ) : QgsClassificationCustom::METHOD_ID );
    method->setLabelTrimTrailingZeroes( true );
    graduated->setClassificationMethod( method.release() );

    if ( classArray.isEmpty() )
    {
      QString classError;
      graduated->updateClasses( layer, classCount, classError );
      if ( graduated->ranges().isEmpty() )
        return QgsAiToolResult::error( u"Could not compute graduated classes for field %1%2"_s.arg( field, classError.isEmpty() ? u": it has no numeric values."_s : u": "_s + classError ) );
    }
    else
    {
      for ( int i = 0; i < classArray.size(); ++i )
      {
        const QJsonObject classObject = classArray.at( i ).toObject();
        const double lower = classObject.value( u"lower"_s ).toDouble();
        const double upper = classObject.value( u"upper"_s ).toDouble();
        if ( upper < lower )
          return QgsAiToolResult::error( u"Graduated class upper value cannot be less than lower value."_s );
        graduated->addClass( QgsRendererRange( lower, upper, baseSymbol->clone(), QString() ) );
      }
      graduated->updateColorRamp();
    }

    if ( precisionValue.isDouble() )
      graduated->classificationMethod()->setLabelPrecision( std::clamp( precisionValue.toInt(), 0, 10 ) );
    else
      graduated->calculateLabelPrecision( false );
    graduated->updateRangeLabels();
    for ( int i = 0; i < classArray.size(); ++i )
    {
      const QString label = classArray.at( i ).toObject().value( u"label"_s ).toString();
      if ( !label.isEmpty() )
        graduated->updateRangeLabel( i, label );
      if ( explicitColor( i ).isValid() )
      {
        std::unique_ptr<QgsSymbol> symbol( graduated->ranges().at( i ).symbol()->clone() );
        symbol->setColor( explicitColor( i ) );
        graduated->updateRangeSymbol( i, symbol.release() );
      }
    }

    // count features per class with the renderer's own lookup, so the counts match what the map draws
    const QgsRangeList &ranges = graduated->ranges();
    QVector<long long> counts( ranges.size(), 0 );
    QgsFeatureRequest request;
    request.setFlags( Qgis::FeatureRequestFlag::NoGeometry ).setSubsetOfAttributes( QgsAttributeList() << fieldIndex );
    QgsFeatureIterator featureIt = layer->getFeatures( request );
    QgsFeature feature;
    while ( featureIt.nextFeature( feature ) )
    {
      const QVariant value = feature.attribute( fieldIndex );
      bool ok = false;
      const double number = QgsVariantUtils::isNull( value ) ? 0.0 : value.toDouble( &ok );
      const QgsRendererRange *range = ok ? graduated->rangeForValue( number ) : nullptr;
      if ( range )
        ++counts[static_cast<int>( range - ranges.constData() )];
      else
        ++unclassifiedCount;
    }
    for ( int i = 0; i < ranges.size(); ++i )
    {
      QJsonObject classJson;
      classJson.insert( u"lower"_s, ranges.at( i ).lowerValue() );
      classJson.insert( u"upper"_s, ranges.at( i ).upperValue() );
      classJson.insert( u"label"_s, ranges.at( i ).label() );
      classJson.insert( u"color"_s, ranges.at( i ).symbol()->color().name() );
      classJson.insert( u"feature_count"_s, counts.at( i ) );
      classesJson.append( classJson );
    }

    layer->setRenderer( graduated.release() );
    changes.append( u"graduated_renderer"_s );
  }

  if ( hasLabelRequest )
  {
    QString labelError;
    if ( !applyBasicLabels( layer, args.value( u"labels"_s ), field, changes, labelError ) )
      return QgsAiToolResult::error( labelError );
  }

  layer->triggerRepaint();
  layer->emitStyleChanged();
  refreshLayoutLegends( project, layer->id() );

  const QString token = storeRollback( rollback );
  QJsonObject diff;
  diff.insert( u"summary"_s, u"Applied advanced layer styling changes."_s );
  diff.insert( u"changes"_s, changes );
  diff.insert( u"before"_s, beforeVisual );
  diff.insert( u"after"_s, layerVisualSummary( layer, treeNode ) );

  QJsonObject output;
  output.insert( u"layer_id"_s, layer->id() );
  output.insert( u"name"_s, layer->name() );
  output.insert( u"visual"_s, layerVisualSummary( layer, treeNode ) );
  output.insert( u"diff"_s, diff );
  output.insert( u"rollback_token"_s, token );
  output.insert( u"rollback"_s, rollbackJson( token, u"restore_layer_style"_s ) );
  if ( renderer == "graduated"_L1 )
  {
    output.insert( u"mode"_s, classArray.isEmpty() ? mode : u"explicit"_s );
    output.insert( u"classes"_s, classesJson );
    output.insert( u"unclassified_feature_count"_s, unclassifiedCount );
  }
  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// create_print_layout
// ---------------------------------------------------------------------------

QgsAiCreatePrintLayoutTool::QgsAiCreatePrintLayoutTool( QgsProject *project, QgsMapCanvas *canvas )
  : mProject( project )
  , mCanvas( canvas )
{}

QString QgsAiCreatePrintLayoutTool::description() const
{
  return QStringLiteral(
    "Creates a native QGIS print layout with a map frame and optional title. "
    "The page is A4 landscape unless page_size/orientation say otherwise; the map fills the page inside the margins "
    "and shows the whole current canvas extent with its rendered layers. "
    "The result describes the page and item positions in mm, with an issues list (items outside the page or overlapping). "
    "Use this before export_map when the user asks for a report/map layout."
  );
}

QJsonObject QgsAiCreatePrintLayoutTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"name"_s, prop( u"string"_s, u"Optional layout name. Defaults to 'Strata Layout' and is made unique."_s ) );
  properties.insert( u"title"_s, prop( u"string"_s, u"Optional title label to place above the map."_s ) );
  properties.insert( u"page_size"_s, prop( u"string"_s, u"Optional page size known to QGIS, e.g. A4, A3, A0, Letter. Defaults to A4."_s ) );
  properties.insert( u"orientation"_s, prop( u"string"_s, u"Optional page orientation: landscape or portrait. Defaults to landscape."_s ) );
  properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token returned by a previous create_print_layout call. If set, removes that layout."_s ) );
  return schemaObject( properties );
}

QgsAiToolResult QgsAiCreatePrintLayoutTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !project )
    return QgsAiToolResult::error( u"No active QgsProject available."_s );
  if ( !project->layoutManager() )
    return QgsAiToolResult::error( u"No layout manager available."_s );

  const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
  if ( !rollbackToken.isEmpty() )
    return rollbackLayoutCreation( project, rollbackToken );

  const int beforeLayoutCount = project->layoutManager()->layouts().size();
  const QString layoutName = uniqueLayoutName( project, args.value( u"name"_s ).toString() );
  const QString title = args.value( u"title"_s ).toString().trimmed();
  const QString pageSizeName = args.value( u"page_size"_s ).toString( u"A4"_s ).trimmed();
  const QString orientationName = args.value( u"orientation"_s ).toString( u"landscape"_s ).trimmed().toLower();
  if ( orientationName != "landscape"_L1 && orientationName != "portrait"_L1 )
    return QgsAiToolResult::error( u"Unsupported orientation '%1'. Use landscape or portrait."_s.arg( orientationName ) );

  auto layout = std::make_unique<QgsPrintLayout>( project );
  layout->initializeDefaults();
  layout->setName( layoutName );
  QgsLayoutItemPage *page = layout->pageCollection()->page( 0 );
  if ( !page->setPageSize( pageSizeName, orientationName == "portrait"_L1 ? QgsLayoutItemPage::Portrait : QgsLayoutItemPage::Landscape ) )
    return QgsAiToolResult::error( u"Unknown page size '%1'. Use a size known to QGIS, e.g. A4, A3 or Letter."_s.arg( pageSizeName ) );
  const QSizeF pageSize = layout->convertToLayoutUnits( page->pageSize() );

  const double margin = 10.0;
  const double titleHeight = title.isEmpty() ? 0.0 : 15.0;
  QgsLayoutItemMap *map = new QgsLayoutItemMap( layout.get() );
  map->setId( u"AI Map"_s );
  map->attemptMove( QgsLayoutPoint( margin, margin + titleHeight ) );
  map->attemptResize( QgsLayoutSize( pageSize.width() - 2 * margin, pageSize.height() - 2 * margin - titleHeight ) );
  if ( mCanvas )
  {
    // zoomToExtent keeps the frame size; setExtent would stretch the frame to the extent ratio, off the page
    map->zoomToExtent( mCanvas->extent() );
    map->setLayers( mCanvas->mapSettings().layers( true ) );
    map->setKeepLayerSet( true );
  }
  else if ( project->layerTreeRoot() )
  {
    map->setLayers( project->layerTreeRoot()->layerOrder() );
    map->setKeepLayerSet( true );
  }
  layout->addLayoutItem( map );
  layout->setReferenceMap( map );

  if ( !title.isEmpty() )
  {
    QgsLayoutItemLabel *label = new QgsLayoutItemLabel( layout.get() );
    label->setId( u"AI Title"_s );
    label->setText( title );
    // a title, not body text: 20 pt bold on A4 landscape, scaled with the page width
    QgsTextFormat format = label->textFormat();
    QFont font = format.font();
    font.setBold( true );
    format.setFont( font );
    format.setSize( std::round( 20.0 * pageSize.width() / 297.0 ) );
    format.setSizeUnit( Qgis::RenderUnit::Points );
    label->setTextFormat( format );
    label->attemptMove( QgsLayoutPoint( margin, 8 ) );
    label->attemptResize( QgsLayoutSize( pageSize.width() - 2 * margin, 12 ) );
    label->adjustSizeToText();
    layout->addLayoutItem( label );
  }

  QgsPrintLayout *createdLayout = layout.get();
  if ( !project->layoutManager()->addLayout( layout.release() ) )
    return QgsAiToolResult::error( u"Could not add print layout to the project."_s );

  RollbackEntry rollback;
  rollback.type = RollbackType::RemoveLayout;
  rollback.layoutName = layoutName;
  rollback.description = u"Remove print layout created by AI tool."_s;
  const QString token = storeRollback( rollback );

  QJsonObject diff;
  diff.insert( u"summary"_s, u"Created a print layout."_s );
  diff.insert( u"before_layout_count"_s, beforeLayoutCount );
  diff.insert( u"after_layout_count"_s, project->layoutManager()->layouts().size() );
  diff.insert( u"layout_name"_s, layoutName );

  QJsonObject output;
  output.insert( u"layout_name"_s, layoutName );
  output.insert( u"title"_s, title );
  output.insert( u"has_map"_s, true );
  output.insert( u"uses_canvas_extent"_s, mCanvas != nullptr );
  output.insert( u"layout"_s, layoutSummary( createdLayout ) );
  output.insert( u"diff"_s, diff );
  output.insert( u"rollback_token"_s, token );
  output.insert( u"rollback"_s, rollbackJson( token, u"remove_created_layout"_s ) );
  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// edit_print_layout
// ---------------------------------------------------------------------------

QgsAiEditPrintLayoutTool::QgsAiEditPrintLayoutTool( QgsProject *project )
  : mProject( project )
{}

QString QgsAiEditPrintLayoutTool::description() const
{
  return QStringLiteral(
    "Edits an existing QGIS print layout by adding common composer items: legend, "
    "scale bar, north arrow and an additional page. Legend, scale bar and north arrow are placed "
    "in a panel beside the map (right on landscape pages, bottom on portrait pages, or as set by placement); "
    "the map shrinks to make room and keeps its extent. The scale bar uses QGIS defaults for units and length. "
    "The legend shows the layers visible in the map unless legend_layers is given, e.g. to leave out a basemap; "
    "legend_labels replaces technical layer names with readable ones. note adds a footer line with text the user gave, "
    "e.g. data source and date: never make up a source; add it in the same call as the other items so they are laid out together. "
    "Called again with note or legend_labels and without add_legend, it updates the existing note and legend instead of adding new ones. "
    "The result describes item positions in mm with an issues list (items outside the page or overlapping). "
    "The tool returns a rollback token which removes the newly added items/pages and restores the map size."
  );
}

QJsonObject QgsAiEditPrintLayoutTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"layout_name"_s, prop( u"string"_s, u"Existing print layout name."_s ) );
  properties.insert( u"add_legend"_s, prop( u"boolean"_s, u"Add a legend linked to the first map item."_s ) );
  properties.insert( u"add_scalebar"_s, prop( u"boolean"_s, u"Add a scale bar linked to the first map item."_s ) );
  properties.insert( u"add_north_arrow"_s, prop( u"boolean"_s, u"Add a north arrow picture linked to the first map item."_s ) );
  properties.insert( u"add_page"_s, prop( u"boolean"_s, u"Append a new page to the layout."_s ) );
  properties.insert( u"placement"_s, prop( u"string"_s, u"Optional side of the map for legend, scale bar and north arrow: right or bottom. Defaults to right on landscape pages, bottom on portrait pages."_s ) );
  properties.insert( u"legend_layers"_s, propArray( prop( u"string"_s, u"Layer id."_s ), u"Optional layer ids to show in the legend, in project order. Leave it out to show the layers visible in the map, which is usually right; pass it only to leave some of those out, e.g. a basemap. Layers the map does not show are left out and listed in legend_layers_not_in_map."_s ) );
  properties.insert( u"legend_labels"_s, prop( u"object"_s, u"Optional legend names by layer id, e.g. {\"<layer_id>\": \"Flood-prone area (%)\"}. Applies to the legend added by this call, or without add_legend to the layout's existing legend. The project layer names do not change."_s ) );
  properties.insert( u"note"_s, prop( u"string"_s, u"Optional footer text at the bottom of the page, only when the user asks for it, e.g. the data source and date they give. Replaces the text of a note added earlier."_s ) );
  properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token returned by a previous edit_print_layout call. If set, removes added items/pages."_s ) );
  return schemaObject( properties );
}

QgsAiToolResult QgsAiEditPrintLayoutTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !project )
    return QgsAiToolResult::error( u"No active QgsProject available."_s );

  const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
  if ( !rollbackToken.isEmpty() )
    return rollbackLayoutEdit( project, rollbackToken );

  const QString layoutName = args.value( u"layout_name"_s ).toString().trimmed();
  if ( layoutName.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'layout_name' is required."_s );

  QgsPrintLayout *layout = printLayoutByName( project, layoutName );
  if ( !layout )
    return QgsAiToolResult::error( u"No print layout named: %1"_s.arg( layoutName ) );
  if ( !layout->pageCollection() )
    return QgsAiToolResult::error( u"Layout has no page collection: %1"_s.arg( layoutName ) );

  const bool addLegend = args.value( u"add_legend"_s ).toBool( false );
  const bool addScaleBar = args.value( u"add_scalebar"_s ).toBool( false );
  const bool addNorthArrow = args.value( u"add_north_arrow"_s ).toBool( false );
  const bool addPage = args.value( u"add_page"_s ).toBool( false );
  const QString note = args.value( u"note"_s ).toString().trimmed();
  QJsonObject legendLabels = args.value( u"legend_labels"_s ).toObject();
  if ( !addLegend && !addScaleBar && !addNorthArrow && !addPage && note.isEmpty() && legendLabels.isEmpty() )
    return QgsAiToolResult::error( u"No layout edit requested."_s );

  QgsLayoutItemMap *map = firstLayoutMap( layout );
  if ( ( addLegend || addScaleBar || addNorthArrow ) && !map )
    return QgsAiToolResult::error( u"Layout has no map item to link legend/scale/north arrow: %1"_s.arg( layoutName ) );

  // without add_legend, legend_labels renames the existing legend: the one linked to the map, else the first
  QgsLayoutItemLegend *existingLegend = nullptr;
  if ( !legendLabels.isEmpty() && !addLegend )
  {
    QList<QgsLayoutItemLegend *> legends;
    layout->layoutItems( legends );
    for ( QgsLayoutItemLegend *legend : std::as_const( legends ) )
    {
      if ( !existingLegend || ( legend->linkedMap() == map && existingLegend->linkedMap() != map ) )
        existingLegend = legend;
    }
    if ( !existingLegend )
      return QgsAiToolResult::error( u"The layout has no legend to rename: pass add_legend=true to add one."_s );
  }

  const QString placement = args.value( u"placement"_s ).toString().trimmed().toLower();
  if ( !placement.isEmpty() && placement != "right"_L1 && placement != "bottom"_L1 )
    return QgsAiToolResult::error( u"Unsupported placement '%1'. Use right or bottom."_s.arg( placement ) );

  // A legend explains what the map draws: layers the map does not show are left out and reported.
  QStringList legendLayerIds;
  QJsonArray skippedLegendLayers;
  QStringList shownIds;
  if ( map )
  {
    const QList<QgsMapLayer *> shown = map->layersToRender();
    for ( const QgsMapLayer *layer : shown )
      shownIds.append( layer->id() );
  }
  const QJsonArray legendLayers = args.value( u"legend_layers"_s ).toArray();
  if ( !legendLayers.isEmpty() && map )
  {
    for ( const QJsonValue &value : legendLayers )
    {
      const QString layerId = value.toString().trimmed();
      if ( !project->mapLayer( layerId ) )
        return QgsAiToolResult::error( u"No layer with id: %1"_s.arg( layerId ) );
      if ( shownIds.contains( layerId ) )
        legendLayerIds.append( layerId );
      else
        skippedLegendLayers.append( layerId );
    }
    if ( legendLayerIds.isEmpty() )
      return QgsAiToolResult::error( u"None of legend_layers is visible in the map; the legend would be empty."_s );
  }
  const QStringList legendIds = existingLegend && existingLegend->syncMode() == Qgis::LegendSyncMode::Manual ? existingLegend->model()->rootGroup()->findLayerIds()
                                : legendLayerIds.isEmpty()                                                   ? shownIds
                                                                                                             : legendLayerIds;
  // like legend_layers: names for layers the legend does not show are left out and reported
  QJsonArray skippedLegendLabels;
  for ( const QString &layerId : legendLabels.keys() )
  {
    if ( !legendIds.contains( layerId ) )
    {
      legendLabels.remove( layerId );
      skippedLegendLabels.append( layerId );
    }
  }

  RollbackEntry rollback;
  rollback.type = RollbackType::RestoreLayoutEdit;
  rollback.layoutName = layoutName;
  rollback.pageCountBefore = layout->pageCollection()->pageCount();
  rollback.description = u"Remove layout items and pages added by AI layout edit."_s;
  QJsonArray changes;

  // Footer note and the panel for legend, scale bar and north arrow are sized from the page,
  // so any page size or orientation works. The map shrinks to make room, keeping its extent.
  const double margin = 10.0;
  const double gap = 5.0;
  const double northArrowSize = 15.0;
  const QRectF mapRect = map ? sceneRect( map ) : QRectF();
  const QRectF pageRect = pageRectAt( layout, map ? mapRect.center() : QPointF() );
  QRectF newMapRect = mapRect;
  double contentBottom = pageRect.bottom() - margin;

  QgsLayoutItemLabel *existingNote = qobject_cast<QgsLayoutItemLabel *>( layout->itemById( u"AI Note"_s ) );
  if ( !note.isEmpty() && existingNote )
  {
    const QRectF before = sceneRect( existingNote );
    rollback.noteUuid = existingNote->uuid();
    rollback.noteText = existingNote->text();
    rollback.noteRect = before;
    existingNote->setText( note );
    const double height = wrappedTextHeight( existingNote, before.width() );
    existingNote->attemptMove( QgsLayoutPoint( before.left(), before.bottom() - height ) );
    existingNote->attemptResize( QgsLayoutSize( before.width(), height ) );
    changes.append( u"note_updated"_s );
  }
  if ( existingNote )
    contentBottom = std::min( contentBottom, sceneRect( existingNote ).top() - gap );
  else if ( !note.isEmpty() )
  {
    QgsLayoutItemLabel *noteLabel = new QgsLayoutItemLabel( layout );
    noteLabel->setId( u"AI Note"_s );
    noteLabel->setText( note );
    // footnote size: 8 pt on A4, scaled with the page
    QgsTextFormat format = noteLabel->textFormat();
    format.setSize( std::max( 8.0, std::round( 8.0 * std::max( pageRect.width(), pageRect.height() ) / 297.0 ) ) );
    format.setSizeUnit( Qgis::RenderUnit::Points );
    noteLabel->setTextFormat( format );
    layout->addLayoutItem( noteLabel );
    const double width = pageRect.width() - 2 * margin;
    const double height = wrappedTextHeight( noteLabel, width );
    noteLabel->attemptMove( QgsLayoutPoint( pageRect.left() + margin, contentBottom - height ) );
    noteLabel->attemptResize( QgsLayoutSize( width, height ) );
    contentBottom -= height + gap;
    if ( map )
      newMapRect.setBottom( std::min( newMapRect.bottom(), contentBottom ) );
    // items an earlier call put at the bottom of the page (scale bar, a bottom panel) move up above the note
    const QList<QGraphicsItem *> sceneItems = layout->items();
    for ( QGraphicsItem *sceneItem : sceneItems )
    {
      QgsLayoutItem *item = dynamic_cast<QgsLayoutItem *>( sceneItem );
      if ( !item || item == noteLabel || item == map || qobject_cast<QgsLayoutItemPage *>( item ) || qobject_cast<QgsLayoutItemGroup *>( item ) )
        continue;
      const QRectF r = sceneRect( item );
      if ( r.bottom() <= contentBottom + 0.01 || !pageRect.intersects( r ) )
        continue;
      rollback.movedItems.insert( item->uuid(), r.topLeft() );
      const double top = r.top() - ( r.bottom() - contentBottom );
      item->attemptMove( QgsLayoutPoint( r.left(), top ) );
      if ( map && r.left() < newMapRect.right() && r.right() > newMapRect.left() )
        newMapRect.setBottom( std::min( newMapRect.bottom(), top - gap ) );
    }
    rollback.itemUuids.append( noteLabel->uuid() );
    changes.append( u"note"_s );
  }

  QRectF panel;
  bool panelOnRight = true;
  QgsLayoutItem *panelItem = layout->itemById( u"AI Legend"_s );
  if ( !panelItem )
    panelItem = layout->itemById( u"AI Scale Bar"_s );
  if ( !panelItem )
    panelItem = layout->itemById( u"AI North Arrow"_s );
  if ( ( addLegend || addScaleBar || addNorthArrow ) && panelItem && map )
  {
    // a panel from an earlier call: the new items go in it and the map keeps its size
    panelOnRight = sceneRect( panelItem ).left() >= newMapRect.right();
    panel = panelOnRight ? QRectF( newMapRect.right() + gap, newMapRect.top(), pageRect.right() - margin - newMapRect.right() - gap, newMapRect.height() )
                         : QRectF( newMapRect.left(), newMapRect.bottom() + gap, newMapRect.width(), contentBottom - newMapRect.bottom() - gap );
  }
  else if ( addLegend || addScaleBar || addNorthArrow )
  {
    panelOnRight = placement.isEmpty() ? pageRect.width() >= pageRect.height() : placement == "right"_L1;
    if ( panelOnRight )
    {
      const double width = std::clamp( pageRect.width() * 0.25, 45.0, 80.0 );
      panel = QRectF( pageRect.right() - margin - width, newMapRect.top(), width, newMapRect.height() );
      newMapRect.setRight( std::min( newMapRect.right(), panel.left() - gap ) );
    }
    else
    {
      const double height = std::clamp( pageRect.height() * 0.2, 35.0, 70.0 );
      panel = QRectF( newMapRect.left(), contentBottom - height, newMapRect.width(), height );
      newMapRect.setBottom( std::min( newMapRect.bottom(), panel.top() - gap ) );
    }
  }

  if ( map && newMapRect != mapRect )
  {
    rollback.mapUuid = map->uuid();
    rollback.mapRect = mapRect;
    rollback.mapExtent = map->extent();
    map->attemptResize( QgsLayoutSize( newMapRect.size() ) );
    map->zoomToExtent( rollback.mapExtent );
    changes.append( u"map_resized"_s );
  }

  if ( addLegend )
  {
    QgsLayoutItemLegend *legend = new QgsLayoutItemLegend( layout );
    legend->setId( u"AI Legend"_s );
    legend->setLinkedMap( map );
    layout->addLayoutItem( legend );
    if ( legendLayerIds.isEmpty() && legendLabels.isEmpty() )
    {
      legend->setSyncMode( Qgis::LegendSyncMode::VisibleLayers );
    }
    else
    {
      makeLegendManual( legend, legendIds );
      setLegendLabels( legend, legendLabels.toVariantMap() );
    }
    fitLegendToContent( legend );
    legend->attemptMove( QgsLayoutPoint( panel.topLeft() ) );
    rollback.itemUuids.append( legend->uuid() );
    changes.append( u"legend"_s );
  }

  if ( existingLegend && !legendLabels.isEmpty() )
  {
    rollback.legendUuid = existingLegend->uuid();
    rollback.legendModeBefore = existingLegend->syncMode();
    if ( existingLegend->syncMode() != Qgis::LegendSyncMode::Manual )
      makeLegendManual( existingLegend, legendIds );
    setLegendLabels( existingLegend, legendLabels.toVariantMap(), &rollback.legendLabelsBefore );
    fitLegendToContent( existingLegend );
    changes.append( u"legend_labels"_s );
  }

  if ( addScaleBar )
  {
    QgsLayoutItemScaleBar *scaleBar = new QgsLayoutItemScaleBar( layout );
    scaleBar->setId( u"AI Scale Bar"_s );
    scaleBar->setLinkedMap( map );
    layout->addLayoutItem( scaleBar );
    // same defaults as the QGIS layout designer: units and segment length chosen from the map
    scaleBar->setMethod( project->scaleMethod() );
    scaleBar->applyDefaultSize( scaleBar->guessUnits() );
    const QRectF bar = sceneRect( scaleBar );
    const double x = panelOnRight ? panel.left() : panel.right() - northArrowSize - gap - bar.width();
    scaleBar->attemptMove( QgsLayoutPoint( x, panel.bottom() - bar.height() ) );
    rollback.itemUuids.append( scaleBar->uuid() );
    changes.append( u"scalebar"_s );
  }

  if ( addNorthArrow )
  {
    QgsLayoutItemPicture *northArrow = new QgsLayoutItemPicture( layout );
    northArrow->setId( u"AI North Arrow"_s );
    northArrow->setPicturePath( u":/images/north_arrows/layout_default_north_arrow.svg"_s, Qgis::PictureFormat::SVG );
    northArrow->setLinkedMap( map );
    northArrow->setNorthMode( QgsLayoutItemPicture::GridNorth );
    // on the right panel it sits above the scale bar slot, on the bottom panel in the top-right corner
    const QPointF position = panelOnRight ? QPointF( panel.left(), panel.bottom() - 3 * northArrowSize ) : QPointF( panel.right() - northArrowSize, panel.top() );
    northArrow->attemptMove( QgsLayoutPoint( position ) );
    northArrow->attemptResize( QgsLayoutSize( northArrowSize, northArrowSize ) );
    layout->addLayoutItem( northArrow );
    rollback.itemUuids.append( northArrow->uuid() );
    changes.append( u"north_arrow"_s );
  }

  if ( addPage )
  {
    layout->pageCollection()->extendByNewPage();
    changes.append( u"page"_s );
  }

  const QString token = storeRollback( rollback );
  QJsonObject diff;
  diff.insert( u"summary"_s, u"Edited an existing print layout."_s );
  diff.insert( u"layout_name"_s, layoutName );
  diff.insert( u"changes"_s, changes );
  diff.insert( u"before_page_count"_s, rollback.pageCountBefore );
  diff.insert( u"after_page_count"_s, layout->pageCollection()->pageCount() );
  diff.insert( u"added_item_count"_s, rollback.itemUuids.size() );

  QJsonObject output;
  output.insert( u"layout_name"_s, layoutName );
  output.insert( u"page_count"_s, layout->pageCollection()->pageCount() );
  output.insert( u"layout"_s, layoutSummary( layout ) );
  if ( !skippedLegendLayers.isEmpty() )
    output.insert( u"legend_layers_not_in_map"_s, skippedLegendLayers );
  if ( !skippedLegendLabels.isEmpty() )
    output.insert( u"legend_labels_not_in_legend"_s, skippedLegendLabels );
  output.insert( u"added_item_count"_s, rollback.itemUuids.size() );
  output.insert( u"diff"_s, diff );
  output.insert( u"rollback_token"_s, token );
  output.insert( u"rollback"_s, rollbackJson( token, u"restore_layout_edit"_s ) );
  return QgsAiToolResult::ok( output );
}

// ---------------------------------------------------------------------------
// export_map
// ---------------------------------------------------------------------------

QgsAiExportMapTool::QgsAiExportMapTool( QgsAiFileContextProvider *contextProvider, QgsProject *project, QgsMapCanvas *canvas )
  : mContextProvider( contextProvider )
  , mProject( project )
  , mCanvas( canvas )
{}

QString QgsAiExportMapTool::description() const
{
  return QStringLiteral(
    "Exports a named print layout to PDF/PNG, or the current map canvas to PNG, writing inside the workspace. "
    "Use create_print_layout first when a layout does not already exist. An existing file is replaced only with overwrite=true; "
    "rollback then restores it. For a layout the result includes an image of its first page, so you can check the sheet "
    "before reporting it done (the image is shared only if the user allowed sending images to AI). "
    "This requires approval because it writes files."
  );
}

QJsonObject QgsAiExportMapTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"path"_s, prop( u"string"_s, u"Workspace-relative output path, e.g. 'exports/map.pdf' or 'exports/canvas.png'."_s ) );
  properties.insert( u"format"_s, prop( u"string"_s, u"Optional output format: 'pdf' or 'png'. Defaults from path extension, then png."_s ) );
  properties.insert( u"layout_name"_s, prop( u"string"_s, u"Optional print layout name. If omitted, exports the current canvas to PNG."_s ) );
  properties.insert( u"width"_s, prop( u"integer"_s, u"Optional canvas export width in pixels, max 4096."_s ) );
  properties.insert( u"height"_s, prop( u"integer"_s, u"Optional canvas export height in pixels, max 4096."_s ) );
  properties.insert( u"dpi"_s, prop( u"integer"_s, u"Optional layout export DPI. Default 300."_s ) );
  properties.insert( u"overwrite"_s, prop( u"boolean"_s, u"Replace an existing file at path. Default false. Rollback restores the previous file."_s ) );
  properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token returned by a previous export_map call. If set, deletes that exported file."_s ) );
  return schemaObject( properties );
}

QgsAiToolResult QgsAiExportMapTool::execute( const QJsonObject &args )
{
  const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
  if ( !rollbackToken.isEmpty() )
    return rollbackFileExport( rollbackToken );

  if ( !mContextProvider )
    return QgsAiToolResult::error( u"File context provider not available."_s );

  const QString rawPath = args.value( u"path"_s ).toString().trimmed();
  if ( rawPath.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'path' is required."_s );

  const QString outputPath = mContextProvider->normalizePath( rawPath, /*allowExternal=*/false );
  if ( outputPath.isEmpty() )
    return QgsAiToolResult::error( u"Output path must be inside the workspace: %1"_s.arg( rawPath ) );

  QString format = args.value( u"format"_s ).toString().trimmed().toLower();
  if ( format.isEmpty() )
    format = QFileInfo( outputPath ).suffix().toLower();
  if ( format.isEmpty() )
    format = u"png"_s;
  if ( format != "png"_L1 && format != "pdf"_L1 )
    return QgsAiToolResult::error( u"Unsupported export format '%1'. Use 'png' or 'pdf'."_s.arg( format ) );
  const bool existed = QFileInfo::exists( outputPath );
  if ( existed && !args.value( u"overwrite"_s ).toBool( false ) )
    return QgsAiToolResult::error( u"Output file already exists: %1. Pass overwrite=true to replace it (rollback restores it), or choose another path."_s.arg( outputPath ) );

  QgsProject *project = mProject ? mProject : QgsProject::instance();
  const QString layoutName = args.value( u"layout_name"_s ).toString().trimmed();
  QgsPrintLayout *layout = nullptr;
  if ( !layoutName.isEmpty() )
  {
    layout = printLayoutByName( project, layoutName );
    if ( !layout )
      return QgsAiToolResult::error( u"No print layout named: %1"_s.arg( layoutName ) );
  }
  else if ( format == "pdf"_L1 )
    return QgsAiToolResult::error( u"Canvas export supports PNG only. Create a print layout first for PDF export."_s );
  else if ( !mCanvas )
    return QgsAiToolResult::error( u"No map canvas available."_s );

  if ( !QDir().mkpath( QFileInfo( outputPath ).absolutePath() ) )
    return QgsAiToolResult::error( u"Could not create output directory: %1"_s.arg( QFileInfo( outputPath ).absolutePath() ) );

  // keep the replaced file until rollback, outside the workspace
  // ponytail: copies stay in the OS temp dir until it cleans them; delete them with expired tokens if disk use matters
  QString backupPath;
  if ( existed )
  {
    backupPath = QDir( QDir::tempPath() ).filePath( u"strata-export-%1.%2"_s.arg( QUuid::createUuid().toString( QUuid::Id128 ), QFileInfo( outputPath ).suffix() ) );
    if ( !QFile::copy( outputPath, backupPath ) )
      return QgsAiToolResult::error( u"Could not keep a copy of %1 before replacing it; nothing was exported."_s.arg( outputPath ) );
  }
  const auto failed = [&outputPath, &backupPath]( const QString &message ) {
    if ( !backupPath.isEmpty() )
    {
      QFile::remove( outputPath );
      QFile::rename( backupPath, outputPath );
    }
    return QgsAiToolResult::error( message );
  };

  QJsonObject previewJson;
  if ( layout )
  {
    QgsLayoutExporter exporter( layout );
    QgsLayoutExporter::ExportResult result = QgsLayoutExporter::Success;
    const int dpi = std::clamp( args.value( u"dpi"_s ).toInt( 300 ), 36, 1200 );
    if ( format == "pdf"_L1 )
    {
      QgsLayoutExporter::PdfExportSettings settings;
      settings.dpi = dpi;
      result = exporter.exportToPdf( outputPath, settings );
    }
    else
    {
      QgsLayoutExporter::ImageExportSettings settings;
      settings.dpi = dpi;
      result = exporter.exportToImage( outputPath, settings );
    }

    if ( result != QgsLayoutExporter::Success )
      return failed( u"Layout export failed: %1"_s.arg( exportResultMessage( result ) ) );

    // the first page as the model will see it, long side 1200 px, sent like capture_map_canvas
    const QgsLayoutItemPage *firstPage = layout->pageCollection()->page( 0 );
    const QSizeF pageMm = firstPage ? firstPage->rect().size() : QSizeF();
    const double scale = pageMm.isEmpty() ? 0.0 : 1200.0 / std::max( pageMm.width(), pageMm.height() );
    const QImage preview = pageMm.isEmpty() ? QImage() : exporter.renderPageToImage( 0, QSize( std::round( pageMm.width() * scale ), std::round( pageMm.height() * scale ) ) );
    const QString previewDir = QgsAiVisualContextUtils::visualContextDirectory();
    QgsAiVisualContextUtils::cleanupOldVisualContextFiles( previewDir );
    const QString previewPath = QDir( previewDir ).filePath( u"layout_%1.png"_s.arg( QUuid::createUuid().toString( QUuid::WithoutBraces ) ) );
    if ( !preview.isNull() && preview.save( previewPath, "PNG" ) )
    {
      previewJson.insert( u"path"_s, previewPath );
      previewJson.insert( u"mime_type"_s, u"image/png"_s );
      previewJson.insert( u"width"_s, preview.width() );
      previewJson.insert( u"height"_s, preview.height() );
      previewJson.insert( u"page"_s, 1 );
      previewJson.insert( u"shared_with_ai"_s, QgsAiVisualContextUtils::hasVisualConsent() );
    }
  }
  else
  {
    QgsMapSettings settings = mCanvas->mapSettings();
    settings.setDevicePixelRatio( 1.0 );
    settings.setExtent( mCanvas->extent() );
    const QSize requestedSize( args.value( u"width"_s ).toInt( 0 ), args.value( u"height"_s ).toInt( 0 ) );
    settings.setOutputSize( exportRenderSize( requestedSize, settings.outputSize().isValid() ? settings.outputSize() : mCanvas->size() ) );

    QgsMapRendererParallelJob job( settings );
    job.start();
    job.waitForFinished();
    const QImage image = job.renderedImage();
    if ( image.isNull() )
      return failed( u"Map canvas render produced an empty image."_s );
    if ( !image.save( outputPath, "PNG" ) )
      return failed( u"Failed to save map export: %1"_s.arg( outputPath ) );
  }

  QJsonObject output;
  RollbackEntry rollback;
  rollback.type = RollbackType::DeleteFile;
  rollback.filePath = outputPath;
  rollback.backupPath = backupPath;
  rollback.description = existed ? u"Restore the file replaced by AI export."_s : u"Delete file exported by AI tool."_s;
  const QString token = storeRollback( rollback );

  QJsonObject diff;
  diff.insert( u"summary"_s, existed ? u"Replaced an existing file with the exported map."_s : u"Created exported map file."_s );
  diff.insert( u"path"_s, outputPath );
  diff.insert( u"existed_before"_s, existed );
  diff.insert( u"before_size_bytes"_s, existed ? static_cast<qint64>( QFileInfo( backupPath ).size() ) : 0 );
  diff.insert( u"after_size_bytes"_s, static_cast<qint64>( QFileInfo( outputPath ).size() ) );

  output.insert( u"path"_s, mContextProvider->workspaceRoot().isEmpty() ? outputPath : QDir( mContextProvider->workspaceRoot() ).relativeFilePath( outputPath ) );
  output.insert( u"absolute_path"_s, outputPath );
  output.insert( u"format"_s, format );
  output.insert( u"layout_name"_s, layoutName );
  output.insert( u"size_bytes"_s, static_cast<qint64>( QFileInfo( outputPath ).size() ) );
  output.insert( u"diff"_s, diff );
  output.insert( u"rollback_token"_s, token );
  output.insert( u"rollback"_s, rollbackJson( token, existed ? u"restore_replaced_file"_s : u"delete_exported_file"_s ) );
  if ( !previewJson.isEmpty() )
    output.insert( u"image"_s, previewJson );
  return QgsAiToolResult::ok( output );
}
