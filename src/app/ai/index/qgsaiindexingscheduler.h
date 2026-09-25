/***************************************************************************
    qgsaiindexingscheduler.h
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

#ifndef QGSAIINDEXINGSCHEDULER_H
#define QGSAIINDEXINGSCHEDULER_H

#include "qgis_app.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QgsAiWorkspaceIndex;
class QgsTask;

/**
 * Runs workspace file indexing in a background task after a debounce.
 *
 * The file scan, the reading and the embeddings all run in the task. A request that
 * arrives while a pass runs is not dropped: the pass is rerun when it ends, and a pass
 * for a workspace that is no longer current is canceled.
 */
class APP_EXPORT QgsAiIndexingScheduler : public QObject
{
  public:
    explicit QgsAiIndexingScheduler( QgsAiWorkspaceIndex *index, QObject *parent = nullptr );

    bool automaticEnabled() const { return mAutomaticEnabled; }
    //! Disabling also cancels a running pass.
    void setAutomaticEnabled( bool enabled );
    void scheduleStartupIndexing( int delayMs = 10000 );
    void scheduleWorkspaceIndexing( int delayMs = 5000 );
    //! Cancels the running pass. It stops within one embedding batch.
    void cancel();

    //! True while a pass runs in the background.
    bool isRunning() const;

    //! Stops scheduling for good (Strata is quitting) and cancels the running pass.
    void shutdown();

  private:
    void startWorkspaceIndexing();
    QPointer<QgsAiWorkspaceIndex> mIndex;
    QPointer<QgsTask> mRunningTask;
    QString mRunningRoot;
    QTimer mDebounceTimer;
    bool mAutomaticEnabled = true;
    bool mRerunRequested = false;
    bool mShutdown = false;
};

#endif // QGSAIINDEXINGSCHEDULER_H
