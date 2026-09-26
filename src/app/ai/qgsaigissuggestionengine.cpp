/***************************************************************************
    qgsaigissuggestionengine.cpp
    ---------------------
    begin                : July 2026
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

#include "qgsaigissuggestionengine.h"

#include "qgsfeature.h"
#include "qgsfeatureiterator.h"
#include "qgsfeaturerequest.h"
#include "qgsfields.h"
#include "qgsgeometry.h"
#include "qgslayoutmanager.h"
#include "qgsproject.h"
#include "qgsrasterlayer.h"
#include "qgssettings.h"
#include "qgsvectorlayer.h"
#include "qgsvectorlayerfeatureiterator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

using namespace Qt::StringLiterals;

namespace
{
  QString projectHashForSettings( const QString &projectFile )
  {
    const QString source = projectFile.trimmed().isEmpty() ? u"unsaved"_s : QDir::cleanPath( projectFile );
    return QString::fromLatin1( QCryptographicHash::hash( source.toUtf8(), QCryptographicHash::Sha1 ).toHex() );
  }

  QgsAiGisSuggestion makeGisSuggestion( const QString &id, const QString &title, const QString &detail, const QString &actionPrompt, const QString &risk = u"low"_s )
  {
    QgsAiGisSuggestion suggestion;
    suggestion.id = id;
    suggestion.title = title;
    suggestion.detail = detail;
    suggestion.actionPrompt = actionPrompt;
    suggestion.risk = risk;
    return suggestion;
  }

  //! Geometry samples of the last completed check, by layer id: {features checked, invalid geometry found}.
  QHash<QString, QPair<int, bool>> sGisGeometrySamples;
  QMutex sGisGeometrySamplesMutex;

  void gisSampleGeometries( QgsAiGisProjectSnapshot::Layer &layer, QgsFeedback *feedback )
  {
    int checked = 0;
    bool invalid = false;
    {
      QgsFeatureRequest request;
      request.setLimit( QgsAiGisSuggestionEngine::GEOMETRY_SAMPLE_SIZE );
      request.setNoAttributes();
      request.setFeedback( feedback );
      QgsFeatureIterator it = layer.source->getFeatures( request );
      QgsFeature feature;
      while ( !invalid && it.nextFeature( feature ) )
      {
        ++checked;
        invalid = feature.hasGeometry() && !feature.geometry().isGeosValid();
      }
    }
    // The iterator is gone: release the provider connection now rather than with the snapshot.
    layer.source.reset();
    if ( feedback && feedback->isCanceled() )
      return;
    layer.geometrySampled = true;
    layer.sampledFeatures = checked;
    layer.invalidGeometry = invalid;
  }
} // namespace

QgsAiGisProjectSnapshot QgsAiGisSuggestionEngine::snapshotProject( QgsProject *project, bool sampleGeometries )
{
  QgsAiGisProjectSnapshot snapshot;
  if ( !project )
    return snapshot;

  snapshot.hasProject = true;
  snapshot.hasLayout = project->layoutManager() && !project->layoutManager()->layouts().isEmpty();

  const QMutexLocker locker( &sGisGeometrySamplesMutex );
  const QMap<QString, QgsMapLayer *> layers = project->mapLayers();
  for ( QgsMapLayer *mapLayer : layers )
  {
    if ( !mapLayer )
      continue;
    QgsAiGisProjectSnapshot::Layer layer;
    layer.id = mapLayer->id();
    layer.name = mapLayer->name();
    layer.visible = mapLayer->isValid() && mapLayer->opacity() > 0;
    layer.crsValid = mapLayer->crs().isValid();

    if ( QgsVectorLayer *vector = qobject_cast<QgsVectorLayer *>( mapLayer ) )
    {
      layer.isVector = true;
      layer.featureCount = vector->featureCount();

      QSet<QString> seenFieldNames;
      const QgsFields fields = vector->fields();
      for ( const QgsField &field : fields )
      {
        const QString normalized = field.name().trimmed().toLower();
        if ( normalized.isEmpty() || seenFieldNames.contains( normalized ) )
          layer.problematicFields = true;
        seenFieldNames.insert( normalized );
      }

      const bool hasGeometry = vector->isValid() && vector->geometryType() != Qgis::GeometryType::Null && vector->geometryType() != Qgis::GeometryType::Unknown;
      if ( hasGeometry && sampleGeometries )
      {
        layer.source = std::make_shared<QgsVectorLayerFeatureSource>( vector );
      }
      else if ( hasGeometry && sGisGeometrySamples.contains( layer.id ) )
      {
        const QPair<int, bool> sample = sGisGeometrySamples.value( layer.id );
        layer.geometrySampled = true;
        layer.sampledFeatures = sample.first;
        layer.invalidGeometry = sample.second;
      }
    }
    else if ( QgsRasterLayer *raster = qobject_cast<QgsRasterLayer *>( mapLayer ) )
    {
      layer.isRaster = true;
      layer.rasterReadable = raster->bandCount() > 0 && raster->width() > 0 && raster->height() > 0;
    }
    snapshot.layers << layer;
  }
  return snapshot;
}

QList<QgsAiGisSuggestion> QgsAiGisSuggestionEngine::evaluate( QgsAiGisProjectSnapshot &snapshot, QgsFeedback *feedback )
{
  QList<QgsAiGisSuggestion> suggestions;
  if ( !snapshot.hasProject )
    return suggestions;

  if ( snapshot.layers.isEmpty() )
  {
    suggestions << makeGisSuggestion( u"project-empty"_s, QObject::tr( "No layers loaded" ), QObject::tr( "The project has no map layers. Start by adding data or opening a demo project." ), QObject::tr( "Inspect this empty QGIS project and propose the next data-loading step. Do not modify anything without approval." ) );
    return suggestions;
  }

  bool hasVisibleLayer = false;
  for ( QgsAiGisProjectSnapshot::Layer &layer : snapshot.layers )
  {
    const QString &layerName = layer.name;
    if ( layer.visible )
      hasVisibleLayer = true;
    if ( !layer.crsValid )
    {
      suggestions << makeGisSuggestion(
        u"missing-crs:%1"_s.arg( layer.id ),
        QObject::tr( "Layer CRS is undefined: %1" ).arg( layerName ),
        QObject::tr( "Analyses and exports may be wrong until the CRS is set explicitly." ),
        QObject::tr( "Review layer '%1' and propose a safe CRS-fix plan. Ask before editing project metadata." ).arg( layerName ),
        u"medium"_s
      );
    }

    if ( layer.isVector )
    {
      if ( layer.featureCount == 0 )
      {
        suggestions << makeGisSuggestion(
          u"empty-vector:%1"_s.arg( layer.id ),
          QObject::tr( "Vector layer has no features: %1" ).arg( layerName ),
          QObject::tr( "Empty layers often indicate a bad filter, failed import or wrong data source." ),
          QObject::tr( "Inspect vector layer '%1' and suggest why it has no features. Do not delete data automatically." ).arg( layerName )
        );
      }
      else if ( layer.featureCount > 1000 )
      {
        suggestions << makeGisSuggestion(
          u"spatial-index:%1"_s.arg( layer.id ),
          QObject::tr( "Consider a spatial index: %1" ).arg( layerName ),
          QObject::tr( "Large vector layers are faster to select, filter and process when indexed." ),
          QObject::tr( "Plan a safe spatial-index creation for layer '%1' using native QGIS tools, with approval before mutation." ).arg( layerName ),
          u"medium"_s
        );
      }

      if ( layer.source && !( feedback && feedback->isCanceled() ) )
        gisSampleGeometries( layer, feedback );
      if ( layer.geometrySampled && layer.invalidGeometry )
      {
        suggestions << makeGisSuggestion(
          u"invalid-geometry:%1"_s.arg( layer.id ),
          QObject::tr( "Invalid geometry detected: %1" ).arg( layerName ),
          QObject::tr( "A sample of %1 features contains invalid geometry. Run a full validity check before overlay/area operations." ).arg( layer.sampledFeatures ),
          QObject::tr( "Check geometry validity for layer '%1' and propose a repair workflow. Ask before applying fixes." ).arg( layerName ),
          u"high"_s
        );
      }

      if ( layer.problematicFields )
      {
        suggestions << makeGisSuggestion(
          u"fields:%1"_s.arg( layer.id ),
          QObject::tr( "Problematic fields: %1" ).arg( layerName ),
          QObject::tr( "Duplicate or unnamed fields can break joins, expressions and exports." ),
          QObject::tr( "Inspect the schema of layer '%1' and propose field cleanup. Ask before editing fields." ).arg( layerName ),
          u"medium"_s
        );
      }
    }
    else if ( layer.isRaster && !layer.rasterReadable )
    {
      suggestions << makeGisSuggestion(
        u"raster-empty:%1"_s.arg( layer.id ),
        QObject::tr( "Raster has no readable pixels: %1" ).arg( layerName ),
        QObject::tr( "The raster provider reports no bands or zero dimensions." ),
        QObject::tr( "Inspect raster layer '%1' and propose a safe reload or data-source check." ).arg( layerName )
      );
    }
  }

  if ( !hasVisibleLayer )
  {
    suggestions << makeGisSuggestion(
      u"styling-visibility"_s,
      QObject::tr( "No visible layer content" ),
      QObject::tr( "All loaded layers appear hidden or fully transparent." ),
      QObject::tr( "Inspect project layer visibility and propose styling changes. Ask before changing layer styles." ),
      u"medium"_s
    );
  }

  if ( !snapshot.hasLayout )
  {
    suggestions << makeGisSuggestion(
      u"layout-missing"_s,
      QObject::tr( "No print layout configured" ),
      QObject::tr( "The project can be viewed, but there is no layout/export target ready for maps or reports." ),
      QObject::tr( "Create a plan for a print layout and map export for this project. Use approval before creating layout items." ),
      u"medium"_s
    );
  }

  return suggestions;
}

void QgsAiGisSuggestionEngine::rememberGeometrySamples( const QgsAiGisProjectSnapshot &snapshot )
{
  const QMutexLocker locker( &sGisGeometrySamplesMutex );
  sGisGeometrySamples.clear();
  for ( const QgsAiGisProjectSnapshot::Layer &layer : snapshot.layers )
  {
    if ( layer.geometrySampled )
      sGisGeometrySamples.insert( layer.id, qMakePair( layer.sampledFeatures, layer.invalidGeometry ) );
  }
}

QList<QgsAiGisSuggestion> QgsAiGisSuggestionEngine::suggestionsWithoutReadingFeatures( QgsProject *project )
{
  QgsAiGisProjectSnapshot snapshot = snapshotProject( project, false );
  return evaluate( snapshot );
}

QList<QgsAiGisSuggestion> QgsAiGisSuggestionEngine::suggestionsForProject( QgsProject *project )
{
  QgsAiGisProjectSnapshot snapshot = snapshotProject( project, true );
  return evaluate( snapshot );
}

QString QgsAiGisSuggestionEngine::formatHealthBlock( const QList<QgsAiGisSuggestion> &suggestions, bool full, int maxSuggestions )
{
  if ( suggestions.isEmpty() )
    return QString();

  QString block = u"## Current project GIS health\n"_s;
  block += "Automatic rule-based observations about the current QGIS project. Use them only when relevant to the user's request.\n"_L1;

  int shown = 0;
  for ( const QgsAiGisSuggestion &suggestion : suggestions )
  {
    if ( shown >= maxSuggestions )
      break;
    block += u"- [%1] %2 — %3\n"_s.arg( suggestion.risk, suggestion.title, suggestion.detail );
    if ( full )
      block += u"  Suggested action: %1\n"_s.arg( suggestion.actionPrompt );
    ++shown;
  }
  if ( suggestions.size() > maxSuggestions )
    block += u"…%1 more.\n"_s.arg( suggestions.size() - maxSuggestions );

  return block;
}

QString QgsAiGisSuggestionEngine::promptHealthBlockForProject( QgsProject *project )
{
  if ( !suggestionsEnabledForProject( project ) )
    return QString();

  return formatHealthBlock( suggestionsWithoutReadingFeatures( project ), false );
}

QString QgsAiGisSuggestionEngine::globalEnabledSettingsKey()
{
  return u"strata/gis_tab/enabled"_s;
}

QString QgsAiGisSuggestionEngine::projectEnabledSettingsKey( const QString &projectFilePath )
{
  return u"strata/gis_tab/project_enabled/%1"_s.arg( projectHashForSettings( projectFilePath ) );
}

QString QgsAiGisSuggestionEngine::dismissedSettingsKey( const QString &projectFilePath )
{
  return u"strata/gis_tab/dismissed/%1"_s.arg( projectHashForSettings( projectFilePath ) );
}

bool QgsAiGisSuggestionEngine::suggestionsEnabledForProject( QgsProject *project )
{
  QgsSettings settings;
  if ( !settings.value( globalEnabledSettingsKey(), true ).toBool() )
    return false;

  const QString projectFile = project ? project->fileName() : QString();
  return settings.value( projectEnabledSettingsKey( projectFile ), true ).toBool();
}

QgsAiGisSuggestionTask::QgsAiGisSuggestionTask( QgsAiGisProjectSnapshot snapshot )
  : QgsTask( QObject::tr( "Check project health" ), QgsTask::CanCancel | QgsTask::CancelWithoutPrompt | QgsTask::Silent | QgsTask::Hidden )
  , mSnapshot( std::move( snapshot ) )
{}

void QgsAiGisSuggestionTask::cancel()
{
  mFeedback.cancel();
  QgsTask::cancel();
}

bool QgsAiGisSuggestionTask::run()
{
  mSuggestions = QgsAiGisSuggestionEngine::evaluate( mSnapshot, &mFeedback );
  return !isCanceled();
}
