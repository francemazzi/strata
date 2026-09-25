/***************************************************************************
    qgsaitaskrunner.h
    ---------------------
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

#ifndef QGSAITASKRUNNER_H
#define QGSAITASKRUNNER_H

#include <atomic>
#include <functional>
#include <memory>

#include "qgis_app.h"
#include "qgstaskmanager.h"

#include <QList>
#include <QMetaObject>
#include <QString>
#include <QStringList>

class QgsFeedback;
class QgsMapLayer;
class QgsVectorLayer;
struct QgsAiActiveRegistration;

/**
 * Base class for the tasks AI tools wait on.
 *
 * The task shares ownership of its feedback (and subclasses of everything their worker touches),
 * so it stays valid if the waiting tool returns early, e.g. when Strata quits mid-run.
 *
 * cancel() also cancels the feedback. Once the runner has queued the task, a cancel counts as a
 * user cancel (Stop button or task manager). Cancels raised before that, such as a failed
 * prepare step in a subclass constructor, don't: they are tool errors the model can retry.
 */
class APP_EXPORT QgsAiBackgroundTask : public QgsTask
{
  public:
    explicit QgsAiBackgroundTask( const QString &description, std::shared_ptr<QgsFeedback> feedback = nullptr );

    void cancel() override;

    //! Feedback shared with the worker. Never null.
    QgsFeedback *feedback() const { return mFeedback.get(); }

    //! True if the task was canceled after qgsAiRunTaskWithEventLoop() queued it.
    bool canceledByUser() const { return mCanceledByUser; }

    //! Called by qgsAiRunTaskWithEventLoop() right before queueing the task.
    void armUserCancel() { mUserCancelArmed = true; }

  protected:
    std::shared_ptr<QgsFeedback> mFeedback;

  private:
    std::atomic_bool mUserCancelArmed { false };
    std::atomic_bool mCanceledByUser { false };
};

/**
 * Work run on a worker thread by qgsAiRunFunction().
 *
 * The work is copied into the task, which may outlive the calling tool when Strata quits:
 * capture state by value (e.g. a std::shared_ptr to the tool's inputs and outputs), never by reference.
 */
using QgsAiBackgroundWork = std::function<bool( QgsFeedback *feedback )>;

//! Task that runs a QgsAiBackgroundWork on a worker thread.
class APP_EXPORT QgsAiFunctionTask : public QgsAiBackgroundTask
{
  public:
    QgsAiFunctionTask( const QString &description, QgsAiBackgroundWork work, std::shared_ptr<QgsFeedback> feedback = nullptr );

  protected:
    bool run() override;

  private:
    QgsAiBackgroundWork mWork;
};

/**
 * RAII that registers \a feedback as the active AI background tool, so Stop cancels it and its
 * progress reaches the chat. Used by run_python and by work that must stay on the GUI thread.
 */
class APP_EXPORT QgsAiActiveFeedbackScope
{
  public:
    explicit QgsAiActiveFeedbackScope( QgsFeedback *feedback, const QString &label = QString() );
    ~QgsAiActiveFeedbackScope();

    QgsAiActiveFeedbackScope( const QgsAiActiveFeedbackScope & ) = delete;
    QgsAiActiveFeedbackScope &operator=( const QgsAiActiveFeedbackScope & ) = delete;

    //! True once the user pressed Stop while the scope was active.
    bool canceledByUser() const;

  private:
    std::shared_ptr<QgsAiActiveRegistration> mRegistration;
    QMetaObject::Connection mProgressConnection;
};

/**
 * RAII for tools that wait on their own event loop, such as network requests or polling.
 *
 * \a onCancel runs on the GUI thread when the user presses Stop. It should abort the pending
 * work, e.g. with QNetworkReply::abort() and QEventLoop::quit().
 */
class APP_EXPORT QgsAiCancelHookScope
{
  public:
    QgsAiCancelHookScope( const QString &label, std::function<void()> onCancel );
    ~QgsAiCancelHookScope();

    QgsAiCancelHookScope( const QgsAiCancelHookScope & ) = delete;
    QgsAiCancelHookScope &operator=( const QgsAiCancelHookScope & ) = delete;

    //! True once the user pressed Stop while the scope was active.
    bool canceledByUser() const;

  private:
    std::shared_ptr<QgsAiActiveRegistration> mRegistration;
};

struct APP_EXPORT QgsAiTaskWaitResult
{
    bool succeeded = false;
    //! The user stopped the work (Stop button or task manager), or Strata is quitting.
    bool canceled = false;
    //! The wait ended before the task finished because Strata is quitting. The task's results are unavailable.
    bool abandoned = false;
    //! A layer the task depended on was removed while it ran. That is a tool error, not a Stop.
    bool layerRemoved = false;
    QString error;
};

struct APP_EXPORT QgsAiBackgroundRunOptions
{
    //! Run on the calling (GUI) thread instead, e.g. when the provider shares a transaction connection.
    bool forceGuiThread = false;
    //! Layers the work reads. QGIS cancels the task if one is removed, and refuses to close the project meanwhile.
    QList<QgsMapLayer *> dependentLayers;
};

/**
 * Adds \a task to the task manager and pumps the GUI until it finishes.
 *
 * \a onFinished runs on the GUI thread while the task still exists, so it can copy results out.
 * It does not run when the wait is abandoned. Never calls waitForFinished.
 */
APP_EXPORT QgsAiTaskWaitResult qgsAiRunTaskWithEventLoop( QgsAiBackgroundTask *task, const QString &label, const std::function<void()> &onFinished = {} );

//! Runs \a work on a worker thread, or on the GUI thread when \a options ask for it, and waits for it.
APP_EXPORT QgsAiTaskWaitResult qgsAiRunFunction( const QString &description, QgsAiBackgroundWork work, const QgsAiBackgroundRunOptions &options = QgsAiBackgroundRunOptions() );

//! Reports progress for the AI background tool currently running.
APP_EXPORT void qgsAiSetBackgroundToolProgressHandler( const std::function<void( const QString &label, double progress )> &handler );
//! True while an AI tool is waiting on the shared background helper, an active feedback scope or a cancel hook.
APP_EXPORT bool qgsAiHasActiveBackgroundTool();
//! Cancels the AI background tools currently running, including Processing, run_python and network waits.
APP_EXPORT void qgsAiCancelActiveBackgroundTool();

/**
 * Pumps events, ignoring user input, until the task manager has no active task left, or until
 * \a timeoutMs elapses. Returns TRUE if no task is active anymore.
 *
 * Used when Strata quits after stopping the AI tools and canceling the background tasks: their
 * workers may still read project layers, so the project must not be closed before they return.
 */
APP_EXPORT bool qgsAiWaitForActiveTasks( int timeoutMs );

//! Outcome of qgsAiApplyInSlices().
enum class QgsAiSliceResult
{
  Completed,
  Canceled, //!< Stop was pressed; the changes applied so far stay for the caller to undo.
  Failed,   //!< An item could not be applied.
};

/**
 * Applies \a count changes on the interface thread, calling \a apply for each index, in slices of
 * about \a sliceMs milliseconds with the event loop turning in between: the window stays
 * responsive, progress reaches the chat and Stop ends the work between two slices. Returns why
 * it ended; \a failedIndex receives the index \a apply refused.
 */
APP_EXPORT QgsAiSliceResult qgsAiApplyInSlices( const QString &label, int count, const std::function<bool( int index )> &apply, int *failedIndex = nullptr, int sliceMs = 50 );

//! Outcome of qgsAiRunProcess().
struct APP_EXPORT QgsAiProcessResult
{
    bool started = false;
    bool canceled = false;
    bool timedOut = false;
    int exitCode = -1;
    QString standardOutput;
    QString standardError;
    //! Why the process could not start.
    QString error;
};

/**
 * Runs \a program with \a arguments and waits for it with an event loop: the interface stays
 * responsive, Stop kills the process within a moment, and so does \a timeoutMs. \a unsetVariables
 * are removed from the environment the process inherits. \a label names the work for Stop.
 */
APP_EXPORT QgsAiProcessResult qgsAiRunProcess( const QString &label, const QString &program, const QStringList &arguments, int timeoutMs, const QStringList &unsetVariables = QStringList() );

/**
 * Quits the nested event loops of the background waits in progress, like QCoreApplication::exit()
 * does when Strata quits, but without stopping later event loops. For tests only.
 */
APP_EXPORT void qgsAiQuitBackgroundWaitLoopsForTesting();

//! Logs a per-phase timing line under the "AI/Perf" message log tag.
APP_EXPORT void qgsAiLogPerf( const QString &tool, const QString &phase, qint64 elapsedMs );

/**
 * Marks a unit of AI work on the calling thread for performance diagnostics.
 *
 * On destruction it logs the elapsed time with qgsAiLogPerf(), when it reaches \a minLogMs (use it
 * for frequent calls that are usually instant). On the GUI thread it also tells the stall monitor
 * which AI work was running, so a stall report can name it.
 */
class APP_EXPORT QgsAiPerfScope
{
  public:
    QgsAiPerfScope( const QString &tool, const QString &phase, int minLogMs = 0 );
    ~QgsAiPerfScope();

    QgsAiPerfScope( const QgsAiPerfScope & ) = delete;
    QgsAiPerfScope &operator=( const QgsAiPerfScope & ) = delete;

    //! Milliseconds since the scope started.
    qint64 elapsedMs() const;

  private:
    QString mTool;
    QString mPhase;
    int mMinLogMs = 0;
    qint64 mStartMs = 0;
    qint64 mRecordId = -1;
};

/**
 * Logs GUI-thread stalls longer than \a thresholdMs as "gui_stall" lines under "AI/Perf", naming
 * the QgsAiPerfScope work that overlapped each stall. Call on the GUI thread; a threshold of 0 or
 * less stops the monitor. Off by default: Strata enables it when STRATA_AI_GUI_STALL_MS is set.
 */
APP_EXPORT void qgsAiSetGuiStallMonitorThreshold( int thresholdMs );

//! True if \a layer's provider reads through a shared transaction connection, which must stay on the GUI thread.
APP_EXPORT bool qgsAiProviderUsesTransaction( const QgsVectorLayer *layer );

/**
 * Name of the first function in \a expression that must be evaluated on the GUI thread, or an
 * empty string when the expression can run on a worker.
 *
 * These are the aggregates and the other functions that read a live layer (QGIS marks them
 * "NOT thread safe"), eval(), whose expression is only known at run time, and functions
 * registered from Python or by plugins, which may touch the GUI or the project.
 */
APP_EXPORT QString qgsAiGuiThreadExpressionFunction( const QString &expression );

/**
 * Records whether a vector layer changed while a background scan of it was running.
 *
 * Connects to the layer's edit, data, field and subset signals for its lifetime.
 */
class APP_EXPORT QgsAiLayerChangeWatch
{
  public:
    explicit QgsAiLayerChangeWatch( QgsVectorLayer *layer );
    ~QgsAiLayerChangeWatch();

    QgsAiLayerChangeWatch( const QgsAiLayerChangeWatch & ) = delete;
    QgsAiLayerChangeWatch &operator=( const QgsAiLayerChangeWatch & ) = delete;

    bool changed() const { return mChanged; }

  private:
    bool mChanged = false;
    QList<QMetaObject::Connection> mConnections;
};

#endif // QGSAITASKRUNNER_H
