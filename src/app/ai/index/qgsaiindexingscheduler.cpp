/***************************************************************************
    qgsaiindexingscheduler.cpp
    --------------------------
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

#include "qgsaiindexingscheduler.h"

#include <algorithm>

#include "ai/tools/qgsaitaskrunner.h"
#include "qgsaiworkspaceindex.h"
#include "qgsapplication.h"
#include "qgsfeedback.h"
#include "qgsmessagelog.h"
#include "qgstaskmanager.h"

#include <QString>

using namespace Qt::StringLiterals;

namespace
{
  //! Scans, reads and embeds the workspace files on a worker thread. Stops within one batch when canceled.
  class QgsAiWorkspaceIndexTask final : public QgsAiBackgroundTask
  {
    public:
      QgsAiWorkspaceIndexTask( QgsAiWorkspaceIndex *index, const QString &workspaceRoot, int maxFiles )
        : QgsAiBackgroundTask( QObject::tr( "Index AI workspace" ) )
        , mIndex( index )
        , mWorkspaceRoot( workspaceRoot )
        , mMaxFiles( maxFiles )
      {}

      QString errorMessage() const { return mErrorMessage; }

    protected:
      bool run() override
      {
        if ( !mIndex )
        {
          mErrorMessage = QObject::tr( "Workspace index is unavailable." );
          return false;
        }
        if ( isCanceled() )
          return false;

        // The feedback lives on the interface thread: progress reaches the task queued.
        connect( mFeedback.get(), &QgsFeedback::progressChanged, this, &QgsAiWorkspaceIndexTask::setProgress );

        QString error;
        QList<QgsAiWorkspaceIndex::WorkspaceFileSnapshot> snapshot;
        bool ok = QgsAiWorkspaceIndex::scanWorkspaceFileSnapshot( mWorkspaceRoot, mMaxFiles, snapshot, &error, mFeedback.get() );
        if ( ok )
          ok = mIndex->reindex( snapshot, mWorkspaceRoot, &error, mFeedback.get() );
        mIndex->closeDatabaseConnectionForCurrentThread();
        if ( !ok )
          mErrorMessage = error;
        return ok && !isCanceled();
      }

    private:
      QPointer<QgsAiWorkspaceIndex> mIndex;
      QString mWorkspaceRoot;
      int mMaxFiles = QgsAiWorkspaceIndex::DEFAULT_MAX_FILES;
      QString mErrorMessage;
  };
} // namespace

QgsAiIndexingScheduler::QgsAiIndexingScheduler( QgsAiWorkspaceIndex *index, QObject *parent )
  : QObject( parent )
  , mIndex( index )
{
  mDebounceTimer.setSingleShot( true );
  connect( &mDebounceTimer, &QTimer::timeout, this, &QgsAiIndexingScheduler::startWorkspaceIndexing );
}

void QgsAiIndexingScheduler::setAutomaticEnabled( bool enabled )
{
  mAutomaticEnabled = enabled;
  if ( !mAutomaticEnabled )
  {
    mDebounceTimer.stop();
    mRerunRequested = false;
    cancel();
  }
}

void QgsAiIndexingScheduler::scheduleStartupIndexing( int delayMs )
{
  scheduleWorkspaceIndexing( delayMs );
}

void QgsAiIndexingScheduler::scheduleWorkspaceIndexing( int delayMs )
{
  if ( mShutdown || !mAutomaticEnabled || !mIndex || !mIndex->embeddingProviderAvailable() )
    return;
  mDebounceTimer.start( std::max( 0, delayMs ) );
}

void QgsAiIndexingScheduler::cancel()
{
  if ( mRunningTask )
    mRunningTask->cancel();
}

bool QgsAiIndexingScheduler::isRunning() const
{
  return mRunningTask && mRunningTask->isActive();
}

void QgsAiIndexingScheduler::shutdown()
{
  mShutdown = true;
  mDebounceTimer.stop();
  mRerunRequested = false;
  cancel();
}

void QgsAiIndexingScheduler::startWorkspaceIndexing()
{
  if ( mShutdown || !mAutomaticEnabled || !mIndex || !mIndex->embeddingProviderAvailable() )
    return;

  const QString workspaceRoot = mIndex->workspaceRoot();
  if ( isRunning() )
  {
    // Run again when this pass ends. A pass for a workspace that is no longer open is outdated.
    mRerunRequested = true;
    if ( mRunningRoot != workspaceRoot )
      mRunningTask->cancel();
    return;
  }
  if ( workspaceRoot.isEmpty() )
    return;

  QgsTaskManager *manager = QgsApplication::taskManager();
  if ( !manager )
  {
    QString error;
    QList<QgsAiWorkspaceIndex::WorkspaceFileSnapshot> snapshot;
    const bool snapshotOk = QgsAiWorkspaceIndex::scanWorkspaceFileSnapshot( workspaceRoot, QgsAiWorkspaceIndex::DEFAULT_MAX_FILES, snapshot, &error );
    const bool ok = snapshotOk && mIndex->reindex( snapshot, workspaceRoot, &error );
    if ( !ok && !error.isEmpty() )
      QgsMessageLog::logMessage( u"AI workspace indexing failed: %1"_s.arg( error ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );
    return;
  }

  QgsAiWorkspaceIndexTask *task = new QgsAiWorkspaceIndexTask( mIndex, workspaceRoot, QgsAiWorkspaceIndex::DEFAULT_MAX_FILES );
  mRunningTask = task;
  mRunningRoot = workspaceRoot;
  mRerunRequested = false;
  const auto finish = [this, task]() {
    if ( mRunningTask == task )
    {
      mRunningTask = nullptr;
      mRunningRoot.clear();
    }
    if ( mRerunRequested && !mShutdown && mAutomaticEnabled )
    {
      mRerunRequested = false;
      scheduleWorkspaceIndexing( 1000 );
    }
  };
  connect( task, &QgsTask::taskCompleted, this, [finish]() {
    QgsMessageLog::logMessage( u"AI workspace index updated in background."_s, u"AI/Index"_s, Qgis::MessageLevel::Info, false );
    finish();
  } );
  connect( task, &QgsTask::taskTerminated, this, [task, finish]() {
    const QString error = task->errorMessage();
    if ( !error.isEmpty() )
      QgsMessageLog::logMessage( u"AI workspace background indexing stopped: %1"_s.arg( error ), u"AI/Index"_s, Qgis::MessageLevel::Info, false );
    finish();
  } );
  // Lowest priority: background indexing must never take precedence over
  // user-initiated tasks (in QGIS higher priority numbers win, so 0 is lowest).
  manager->addTask( task, 0 );
}
