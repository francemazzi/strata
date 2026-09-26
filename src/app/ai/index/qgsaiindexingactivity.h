/***************************************************************************
    qgsaiindexingactivity.h
    -----------------------
    begin                : September 2026
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

#ifndef QGSAIINDEXINGACTIVITY_H
#define QGSAIINDEXINGACTIVITY_H

#include "qgis_app.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QgsAiIndexingScheduler;
class QgsAiLayerIndexCoordinator;
class QgsAiWorkspaceIndex;

/**
 * What background indexing is doing, for the user: the files pass and the layer queue in
 * one state, with pause and resume for both.
 *
 * Indexing can keep the computer busy for minutes; whoever sees it slow down must be able to
 * tell why at a glance and stop it.
 */
class APP_EXPORT QgsAiIndexingActivity : public QObject
{
    Q_OBJECT

  public:
    struct State
    {
        //! Files or layers are being indexed right now.
        bool active = false;
        bool paused = false;
        //! Indexing waits because the computer runs on battery.
        bool waitingForPower = false;
        //! Progress of the files pass, -1 when none runs.
        double filePercent = -1;
        int layersDone = 0;
        int layersTotal = 0;
        QString currentLayer;
        //! Why indexing cannot run or last failed, worded for the user.
        QString problem;
    };

    QgsAiIndexingActivity( QgsAiWorkspaceIndex *index, QgsAiIndexingScheduler *scheduler, QgsAiLayerIndexCoordinator *coordinator, QObject *parent = nullptr );

    State state() const;

    //! One line for the chat header, empty when there is nothing to show.
    static QString summaryText( const State &state );
    //! Details for a tooltip.
    static QString detailText( const State &state );

    bool isPaused() const { return mPaused; }

  public slots:
    //! Pauses or resumes both the files pass and the layer queue.
    void setPaused( bool paused );
    //! Checks again whether indexing can run (after a settings change, a model download…).
    void refresh();

  signals:
    void changed();

  private:
    void onLayerQueueChanged();

    QPointer<QgsAiWorkspaceIndex> mIndex;
    QPointer<QgsAiIndexingScheduler> mScheduler;
    QPointer<QgsAiLayerIndexCoordinator> mCoordinator;
    QTimer mRefreshTimer;
    bool mPaused = false;
    double mFilePercent = -1;
    int mLayersDone = 0;
    QString mAvailabilityProblem;
    QString mLastError;
};

#endif // QGSAIINDEXINGACTIVITY_H
