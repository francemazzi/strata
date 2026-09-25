/***************************************************************************
    qgsailayerindexcoordinator.cpp
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

#include "qgsailayerindexcoordinator.h"

#include <algorithm>
#include <utility>

#include "ai/tools/qgsaitaskrunner.h"
#include "qgsaiworkspaceindex.h"
#include "qgsapplication.h"
#include "qgsfeedback.h"
#include "qgsmaplayer.h"
#include "qgsmessagelog.h"
#include "qgsproject.h"
#include "qgstaskmanager.h"
#include "qgsvectorlayer.h"

#include <QPointer>
#include <QString>

#include "moc_qgsailayerindexcoordinator.cpp"

using namespace Qt::StringLiterals;

namespace
{
  struct LayerIndexResult
  {
      QString layerId;
      bool success = false;
      QString errorMessage;
  };

  //! Reads, embeds and stores one prepared layer on a worker thread. Stops within one batch when canceled.
  class QgsAiLayerIndexTask final : public QgsAiBackgroundTask
  {
    public:
      QgsAiLayerIndexTask( QgsAiWorkspaceIndex *index, QgsAiWorkspaceIndex::WorkspaceLayerSnapshot snapshot )
        : QgsAiBackgroundTask( QObject::tr( "Index AI layers" ) )
        , mIndex( index )
        , mSnapshot( std::move( snapshot ) )
      {}

      LayerIndexResult result() const { return mResult; }
      QString errorMessage() const { return mErrorMessage; }

    protected:
      bool run() override
      {
        if ( !mIndex )
        {
          mErrorMessage = QObject::tr( "Workspace index is unavailable." );
          mResult = { mSnapshot.scopedLayerId, false, mErrorMessage };
          return false;
        }

        if ( isCanceled() )
        {
          mIndex->closeDatabaseConnectionForCurrentThread();
          return false;
        }

        QString reindexError;
        const bool ok = mIndex->reindexLayerSnapshot( mSnapshot, &reindexError, mFeedback.get() );
        mResult = { mSnapshot.scopedLayerId, ok, reindexError };
        if ( !ok && !mIndex->embeddingProviderAvailable() )
          mErrorMessage = reindexError;

        mIndex->closeDatabaseConnectionForCurrentThread();
        return ok && !isCanceled();
      }

    private:
      QPointer<QgsAiWorkspaceIndex> mIndex;
      QgsAiWorkspaceIndex::WorkspaceLayerSnapshot mSnapshot;
      LayerIndexResult mResult;
      QString mErrorMessage;
  };
} // namespace

QgsAiLayerIndexCoordinator::QgsAiLayerIndexCoordinator( QgsAiWorkspaceIndex *index, QObject *parent )
  : QObject( parent )
  , mIndex( index )
  , mProject( QgsProject::instance() )
{
  mDebounceTimer.setSingleShot( true );
  connect( &mDebounceTimer, &QTimer::timeout, this, &QgsAiLayerIndexCoordinator::flushDirty );
}

void QgsAiLayerIndexCoordinator::setEnabled( bool enabled )
{
  if ( mEnabled == enabled )
    return;
  mEnabled = enabled;
  if ( mEnabled )
    connectProjectSignals();
  else
  {
    if ( mRunningTask )
      mRunningTask->cancel();
    mDirtyLayers.clear();
    mUseBulkDebounce = false;
    disconnectProjectSignals();
    // Layer indexing is off: search uses no layer chunk.
    mActiveLayerIds.clear();
    publishActiveLayers();
  }
}

void QgsAiLayerIndexCoordinator::setDebounceMs( int ms )
{
  mDebounceMs = std::max( 0, ms );
}

void QgsAiLayerIndexCoordinator::setBulkDebounceMs( int ms )
{
  mBulkDebounceMs = std::max( 0, ms );
}

void QgsAiLayerIndexCoordinator::setProject( QgsProject *project )
{
  if ( mProject == project )
    return;
  const bool wasEnabled = mEnabled;
  if ( wasEnabled )
    disconnectProjectSignals();
  mProject = project ? project : QgsProject::instance();
  if ( wasEnabled )
    connectProjectSignals();
}

void QgsAiLayerIndexCoordinator::connectProjectSignals()
{
  if ( !mProject )
    return;
  connect( mProject, &QgsProject::layerWasAdded, this, &QgsAiLayerIndexCoordinator::onLayerAdded );
  connect( mProject, qOverload<const QString &>( &QgsProject::layerWillBeRemoved ), this, &QgsAiLayerIndexCoordinator::onLayerWillBeRemoved );
  connect( mProject, &QgsProject::aboutToBeCleared, this, [this]() { mProjectClosing = true; } );
  connect( mProject, &QgsProject::cleared, this, [this]() {
    mProjectClosing = false;
    mActiveLayerIds.clear();
    publishActiveLayers();
  } );
  mActiveLayerIds.clear();
  const QMap<QString, QgsMapLayer *> existing = mProject->mapLayers();
  for ( auto it = existing.constBegin(); it != existing.constEnd(); ++it )
  {
    connectLayerSignals( it.value() );
    if ( it.value() )
      mActiveLayerIds.insert( it.key() );
  }
  publishActiveLayers();
  scheduleAllLayers();
}

void QgsAiLayerIndexCoordinator::publishActiveLayers()
{
  if ( mIndex )
    mIndex->setActiveLayerIds( mActiveLayerIds );
}

void QgsAiLayerIndexCoordinator::disconnectProjectSignals()
{
  if ( !mProject )
    return;
  disconnect( mProject, nullptr, this, nullptr );
  const QMap<QString, QgsMapLayer *> existing = mProject->mapLayers();
  for ( auto it = existing.constBegin(); it != existing.constEnd(); ++it )
  {
    if ( it.value() )
      disconnect( it.value(), nullptr, this, nullptr );
  }
}

void QgsAiLayerIndexCoordinator::connectLayerSignals( QgsMapLayer *layer )
{
  if ( !layer )
    return;

  // The chunks carry the name and CRS of the layer, and come from its data source.
  connect( layer, &QgsMapLayer::nameChanged, this, &QgsAiLayerIndexCoordinator::onLayerChanged, Qt::UniqueConnection );
  connect( layer, &QgsMapLayer::crsChanged, this, &QgsAiLayerIndexCoordinator::onLayerChanged, Qt::UniqueConnection );
  connect( layer, &QgsMapLayer::dataSourceChanged, this, &QgsAiLayerIndexCoordinator::onLayerChanged, Qt::UniqueConnection );

  QgsVectorLayer *v = qobject_cast<QgsVectorLayer *>( layer );
  if ( !v )
  {
    connect( layer, &QgsMapLayer::dataChanged, this, &QgsAiLayerIndexCoordinator::onLayerChanged, Qt::UniqueConnection );
    return;
  }

  // Vector edits are indexed once saved: edits in progress change on every keystroke and may be
  // rolled back. Saving emits the committed* signals, then editingStopped.
  connect( v, &QgsVectorLayer::subsetStringChanged, this, &QgsAiLayerIndexCoordinator::onLayerChanged, Qt::UniqueConnection );
  connect( v, &QgsMapLayer::editingStopped, this, &QgsAiLayerIndexCoordinator::onLayerChanged, Qt::UniqueConnection );
  {
    connect( v, &QgsVectorLayer::committedAttributesDeleted, this, &QgsAiLayerIndexCoordinator::onVectorLayerCommitted, Qt::UniqueConnection );
    connect( v, &QgsVectorLayer::committedAttributesAdded, this, &QgsAiLayerIndexCoordinator::onVectorLayerCommitted, Qt::UniqueConnection );
    connect( v, &QgsVectorLayer::committedFeaturesAdded, this, &QgsAiLayerIndexCoordinator::onVectorLayerCommitted, Qt::UniqueConnection );
    connect( v, &QgsVectorLayer::committedFeaturesRemoved, this, &QgsAiLayerIndexCoordinator::onVectorLayerCommitted, Qt::UniqueConnection );
    connect( v, &QgsVectorLayer::committedAttributeValuesChanges, this, &QgsAiLayerIndexCoordinator::onVectorLayerCommitted, Qt::UniqueConnection );
    connect( v, &QgsVectorLayer::committedGeometriesChanges, this, &QgsAiLayerIndexCoordinator::onVectorLayerCommitted, Qt::UniqueConnection );
  }
}

void QgsAiLayerIndexCoordinator::onLayerAdded( QgsMapLayer *layer )
{
  if ( !layer )
    return;
  connectLayerSignals( layer );
  mActiveLayerIds.insert( layer->id() );
  publishActiveLayers();
  scheduleDirty( layer->id() );
}

void QgsAiLayerIndexCoordinator::onLayerWillBeRemoved( const QString &layerId )
{
  mDirtyLayers.remove( layerId );
  mActiveLayerIds.remove( layerId );
  publishActiveLayers();
  // Its chunks would be written back after the removal below.
  if ( mRunningTask && mRunningLayerId == layerId )
    mRunningTask->cancel();
  // Closing the project keeps its layers indexed: reopening it reuses them.
  if ( !mIndex || mProjectClosing )
    return;
  if ( mRunningTask && mRunningLayerId == layerId )
    mRemovedWhileRunning.insert( layerId );
  const QgsAiPerfScope perf( u"index"_s, u"remove_layer"_s, 5 );
  QString err;
  if ( !mIndex->removeLayer( layerId, &err ) )
    QgsMessageLog::logMessage( u"Layer index: removeLayer(%1) failed: %2"_s.arg( layerId, err ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );
}

void QgsAiLayerIndexCoordinator::onLayerChanged()
{
  QgsMapLayer *layer = qobject_cast<QgsMapLayer *>( sender() );
  if ( !layer )
    return;
  scheduleDirty( layer->id() );
}

void QgsAiLayerIndexCoordinator::onVectorLayerCommitted( const QString &layerId )
{
  scheduleDirty( layerId );
}

void QgsAiLayerIndexCoordinator::scheduleAllLayers()
{
  if ( !mProject )
    return;

  mUseBulkDebounce = true;
  const QMap<QString, QgsMapLayer *> existing = mProject->mapLayers();
  for ( auto it = existing.constBegin(); it != existing.constEnd(); ++it )
  {
    if ( it.value() )
      mDirtyLayers.insert( it.value()->id() );
  }
  startDebounceTimer();
}

void QgsAiLayerIndexCoordinator::scheduleDirty( const QString &layerId )
{
  if ( layerId.isEmpty() )
    return;
  if ( !mIndex || !mIndex->embeddingProviderAvailable() )
    return;
  mDirtyLayers.insert( layerId );
  startDebounceTimer();
}

void QgsAiLayerIndexCoordinator::beginBulkOperation()
{
  mBulkOperationDepth++;
  mDebounceTimer.stop();
  // Leave the CPU to the import: stop the layer being embedded and index it again afterwards.
  if ( mRunningTask && mRunningTask->isActive() )
  {
    if ( !mRunningLayerId.isEmpty() )
      mDirtyLayers.insert( mRunningLayerId );
    mRunningTask->cancel();
  }
}

void QgsAiLayerIndexCoordinator::endBulkOperation()
{
  if ( mBulkOperationDepth == 0 )
    return;
  mBulkOperationDepth--;
  if ( mBulkOperationDepth == 0 && mEnabled && !mDirtyLayers.isEmpty() )
  {
    // resume after the bulk debounce window, one layer per flush as usual
    mUseBulkDebounce = true;
    startDebounceTimer();
  }
}

void QgsAiLayerIndexCoordinator::setInterFlushDelayMs( int ms )
{
  mInterFlushDelayMs = std::max( 0, ms );
}

void QgsAiLayerIndexCoordinator::startDebounceTimer()
{
  if ( mBulkOperationDepth > 0 )
    return;
  const int ms = mUseBulkDebounce ? mBulkDebounceMs : mDebounceMs;
  mDebounceTimer.start( ms );
}

void QgsAiLayerIndexCoordinator::scheduleNextFlush()
{
  // separates consecutive main-thread snapshots with real event-loop idle time,
  // instead of chaining them back-to-back at 0 ms
  if ( mBulkOperationDepth > 0 )
    return;
  mDebounceTimer.start( mInterFlushDelayMs );
}

bool QgsAiLayerIndexCoordinator::isRunning() const
{
  return mRunningTask && mRunningTask->isActive();
}

void QgsAiLayerIndexCoordinator::shutdown()
{
  mShutdown = true;
  setEnabled( false );
}

void QgsAiLayerIndexCoordinator::flushDirty()
{
  if ( mBulkOperationDepth > 0 || mShutdown )
    return;

  mUseBulkDebounce = false;

  if ( !mIndex || !mEnabled || !mIndex->embeddingProviderAvailable() )
  {
    mDirtyLayers.clear();
    return;
  }
  if ( mRunningTask && mRunningTask->isActive() )
    return;
  if ( mDirtyLayers.isEmpty() )
    return;

  const QString layerId = *mDirtyLayers.constBegin();
  mDirtyLayers.remove( layerId );

  emit reindexStarted( layerId );

  // Prepare the layer on the main thread (QgsMapLayer is not thread-safe): cheap metadata and a
  // feature source. Reading its features, embedding and SQLite writes run in QgsAiLayerIndexTask.
  QgsAiWorkspaceIndex::WorkspaceLayerSnapshot snapshot;
  QString snapshotError;
  bool snapshotOk = false;
  {
    const QgsAiPerfScope perf( u"index"_s, u"layer_snapshot"_s );
    snapshotOk = mIndex->createWorkspaceLayerSnapshotForLayer( layerId, snapshot, &snapshotError );
  }
  if ( !snapshotOk )
  {
    emit reindexFinished( layerId, false, snapshotError );
    QgsMessageLog::logMessage( u"Layer index: snapshot(%1) failed: %2"_s.arg( layerId, snapshotError ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );
    if ( mEnabled && !mDirtyLayers.isEmpty() && mIndex ) // provider availability is re-checked when the flush fires
      scheduleNextFlush();
    return;
  }

  QgsTaskManager *taskManager = QgsApplication::taskManager();
  if ( !taskManager )
  {
    QString err;
    const bool ok = mIndex->reindexLayerSnapshot( snapshot, &err );
    emit reindexFinished( layerId, ok, err );
    if ( !ok )
      QgsMessageLog::logMessage( u"Layer index: reindexLayer(%1) failed: %2"_s.arg( layerId, err ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );

    if ( mEnabled && !mDirtyLayers.isEmpty() && mIndex ) // provider availability is re-checked when the flush fires
      scheduleNextFlush();
    return;
  }

  QgsAiLayerIndexTask *task = new QgsAiLayerIndexTask( mIndex, std::move( snapshot ) );
  mRunningTask = task;
  mRunningLayerId = layerId;

  auto finishTask = [this, task]( bool terminated ) {
    const LayerIndexResult result = task->result();
    if ( !result.layerId.isEmpty() )
    {
      if ( !result.success )
        QgsMessageLog::logMessage( u"Layer index: reindexLayer(%1) failed: %2"_s.arg( result.layerId, result.errorMessage ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );
      emit reindexFinished( result.layerId, result.success, result.errorMessage );
    }

    if ( terminated && !task->errorMessage().isEmpty() )
      QgsMessageLog::logMessage( u"Layer index background task failed: %1"_s.arg( task->errorMessage() ), u"AI/Index"_s, Qgis::MessageLevel::Warning, false );

    if ( mRunningTask == task )
    {
      mRunningTask = nullptr;
      mRunningLayerId.clear();
    }

    // The user removed the layer while its chunks were being written: remove them again.
    if ( mRemovedWhileRunning.remove( result.layerId ) && mIndex )
    {
      QString err;
      mIndex->removeLayer( result.layerId, &err );
    }

    if ( mEnabled && !mDirtyLayers.isEmpty() && mIndex ) // provider availability is re-checked when the flush fires
      scheduleNextFlush();
  };

  connect( task, &QgsTask::taskCompleted, this, [finishTask]() { finishTask( false ); } );
  connect( task, &QgsTask::taskTerminated, this, [finishTask]() { finishTask( true ); } );
  taskManager->addTask( task, 0 );
}
