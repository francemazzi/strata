/***************************************************************************
    qgsaigissuggestionengine.h
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

#ifndef QGSAIGISSUGGESTIONENGINE_H
#define QGSAIGISSUGGESTIONENGINE_H

#include <memory>

#include "qgis_app.h"
#include "qgsfeedback.h"
#include "qgstaskmanager.h"

#include <QList>
#include <QString>

using namespace Qt::StringLiterals;

class QgsAbstractFeatureSource;
class QgsProject;

struct APP_EXPORT QgsAiGisSuggestion
{
    QString id;
    QString title;
    QString detail;
    QString actionPrompt;
    QString risk = u"low"_s; //!< Low | medium | high
};

/**
 * What the project health checks need from a project, read on the interface thread without
 * reading any feature. QgsAiGisSuggestionEngine::evaluate() samples the geometries later, on
 * any thread.
 */
struct APP_EXPORT QgsAiGisProjectSnapshot
{
    struct Layer
    {
        QString id;
        QString name;
        bool visible = false;
        bool crsValid = true;
        bool isVector = false;
        bool isRaster = false;
        long long featureCount = -1;
        bool problematicFields = false;
        bool rasterReadable = true;
        //! Source of the geometry sample; released once the sample is taken.
        std::shared_ptr<QgsAbstractFeatureSource> source;
        bool geometrySampled = false;
        int sampledFeatures = 0;
        bool invalidGeometry = false;
    };

    bool hasProject = false;
    bool hasLayout = false;
    QList<Layer> layers;
};

/**
 * Rule-based project health checks feeding the AI chat: automatic system-prompt
 * context, the in-chat suggestion card and the \c @gis mention.
 */
class APP_EXPORT QgsAiGisSuggestionEngine
{
  public:
    //! Features sampled per vector layer to look for invalid geometries.
    static constexpr int GEOMETRY_SAMPLE_SIZE = 200;

    /**
     * Reads \a project on the interface thread without reading features. With \a sampleGeometries
     * the snapshot keeps a feature source per vector layer for evaluate(); without, it carries the
     * samples remembered by rememberGeometrySamples().
     */
    static QgsAiGisProjectSnapshot snapshotProject( QgsProject *project, bool sampleGeometries );

    /**
     * Samples the geometries of \a snapshot that have a feature source, then returns its
     * suggestions. Safe on any thread; stops sampling when \a feedback is canceled.
     */
    static QList<QgsAiGisSuggestion> evaluate( QgsAiGisProjectSnapshot &snapshot, QgsFeedback *feedback = nullptr );

    //! Keeps the geometry samples of \a snapshot for later snapshots that don't sample. Interface thread.
    static void rememberGeometrySamples( const QgsAiGisProjectSnapshot &snapshot );

    //! Suggestions from the project and the remembered geometry samples. Reads no features.
    static QList<QgsAiGisSuggestion> suggestionsWithoutReadingFeatures( QgsProject *project );

    //! Snapshot and evaluation in one call, sampling geometries on the calling thread.
    static QList<QgsAiGisSuggestion> suggestionsForProject( QgsProject *project );

    /**
     * Markdown block "## Current project GIS health", capped at \a maxSuggestions
     * entries. \a full adds each suggestion's action prompt (used by the \c @gis
     * mention); the compact form is meant for automatic prompt injection.
     */
    static QString formatHealthBlock( const QList<QgsAiGisSuggestion> &suggestions, bool full, int maxSuggestions = 10 );

    //! Compact health block for the system prompt; empty when disabled or healthy. Reads no features.
    static QString promptHealthBlockForProject( QgsProject *project );

    static QString globalEnabledSettingsKey();
    static QString projectEnabledSettingsKey( const QString &projectFilePath );
    //! QStringList of suggestion ids dismissed from the in-chat card for this project.
    static QString dismissedSettingsKey( const QString &projectFilePath );
    static bool suggestionsEnabledForProject( QgsProject *project );
};

//! Samples the geometries of a project snapshot on a worker thread, hidden from the task list.
class APP_EXPORT QgsAiGisSuggestionTask : public QgsTask
{
  public:
    explicit QgsAiGisSuggestionTask( QgsAiGisProjectSnapshot snapshot );

    void cancel() override;

    //! The snapshot with its geometry samples once the task has completed.
    const QgsAiGisProjectSnapshot &snapshot() const { return mSnapshot; }
    QList<QgsAiGisSuggestion> suggestions() const { return mSuggestions; }

  protected:
    bool run() override;

  private:
    QgsAiGisProjectSnapshot mSnapshot;
    QList<QgsAiGisSuggestion> mSuggestions;
    QgsFeedback mFeedback;
};

#endif // QGSAIGISSUGGESTIONENGINE_H
