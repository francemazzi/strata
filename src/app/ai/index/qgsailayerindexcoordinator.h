/***************************************************************************
    qgsailayerindexcoordinator.h
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

#ifndef QGSAILAYERINDEXCOORDINATOR_H
#define QGSAILAYERINDEXCOORDINATOR_H

#include "qgis_app.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

class QgsAiWorkspaceIndex;
class QgsMapLayer;
class QgsProject;
class QgsTask;

/**
 * Listens to QgsProject layer-add/remove events and to per-layer data/editing
 * signals, batching the affected layer ids and re-indexing them via QgsAiWorkspaceIndex
 * after a debounce window. Removed layers are deleted from the index immediately.
 *
 * Disabled by default: setEnabled(true) wires the QgsProject signals; setEnabled(false)
 * disconnects them. This lets the user opt in / out without recreating the coordinator.
 */
class APP_EXPORT QgsAiLayerIndexCoordinator : public QObject
{
    Q_OBJECT

  public:
    explicit QgsAiLayerIndexCoordinator( QgsAiWorkspaceIndex *index, QObject *parent = nullptr );
    //! Cancels the running layer and waits for it: the task reads the index, which may go next.
    ~QgsAiLayerIndexCoordinator() override;

    bool isEnabled() const { return mEnabled; }
    void setEnabled( bool enabled );

    int debounceMs() const { return mDebounceMs; }
    void setDebounceMs( int ms );
    int bulkDebounceMs() const { return mBulkDebounceMs; }
    void setBulkDebounceMs( int ms );
    void scheduleAllLayers();

    /**
     * Pauses indexing flushes for the duration of a bulk operation (e.g. a batched
     * layer import). Refcounted and nesting-safe. Unlike setEnabled( false ), this is
     * non-destructive: the dirty set and the signal connections are preserved, so
     * layers added during the pause are indexed once the operation ends.
     */
    void beginBulkOperation();

    //! Ends a bulk operation. At depth zero, indexing resumes after the bulk debounce window.
    void endBulkOperation();

    bool isBulkOperationActive() const { return mBulkOperationDepth > 0; }

    //! True while a layer is being indexed in the background.
    bool isRunning() const;

    /**
     * Holds layer indexing back until resumed. The running layer stops within one batch and is
     * indexed again on resume, with every layer that changed meanwhile.
     */
    void setPaused( bool paused );
    bool isPaused() const { return mPaused; }

    //! Layers waiting to be indexed, the running one included.
    int pendingLayerCount() const;

    //! Name of the layer being indexed, empty when none.
    QString runningLayerName() const;

    //! Stops indexing for good (Strata is quitting): disconnects and cancels the running layer.
    void shutdown();

    //! Idle gap between two consecutive per-layer snapshot flushes (main-thread work).
    int interFlushDelayMs() const { return mInterFlushDelayMs; }
    void setInterFlushDelayMs( int ms );

    /**
     * Override the QgsProject the coordinator listens to. Defaults to QgsProject::instance()
     * when null is passed. Useful in tests.
     */
    void setProject( QgsProject *project );

  signals:
    //! The set of layers waiting to be indexed changed.
    void queueChanged();
    void reindexStarted( const QString &layerId );
    void reindexFinished( const QString &layerId, bool success, const QString &errorMessage );

  private slots:
    void onLayerAdded( QgsMapLayer *layer );
    void onLayerWillBeRemoved( const QString &layerId );
    void onLayerChanged();
    void onVectorLayerCommitted( const QString &layerId );
    void flushDirty();

  private:
    void connectProjectSignals();
    void disconnectProjectSignals();
    void connectLayerSignals( QgsMapLayer *layer );
    void scheduleDirty( const QString &layerId );
    void startDebounceTimer();
    void scheduleNextFlush();
    //! Tells the index which layers are open, so search leaves out the others.
    void publishActiveLayers();

    QgsAiWorkspaceIndex *mIndex = nullptr;
    QgsProject *mProject = nullptr;
    QSet<QString> mDirtyLayers;
    QTimer mDebounceTimer;
    QPointer<QgsTask> mRunningTask;
    QString mRunningLayerId;
    //! Layers of the open project.
    QSet<QString> mActiveLayerIds;
    //! The project is being cleared: its layers leave, but their chunks stay for the next time it opens.
    bool mProjectClosing = false;
    //! Layers the user removed while their task was running, whose chunks it may still write.
    QSet<QString> mRemovedWhileRunning;
    bool mEnabled = false;
    bool mShutdown = false;
    bool mPaused = false;
    bool mUseBulkDebounce = false;
    int mDebounceMs = 5000;
    int mBulkDebounceMs = 15000;
    int mBulkOperationDepth = 0;
    int mInterFlushDelayMs = 250;
};

#endif // QGSAILAYERINDEXCOORDINATOR_H
