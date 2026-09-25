/***************************************************************************
    qgsaitaskrunner.cpp
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

#include "qgsaitaskrunner.h"

#include <algorithm>
#include <atomic>

#include "qgsapplication.h"
#include "qgsexpression.h"
#include "qgsexpressioncontext.h"
#include "qgsexpressionfunction.h"
#include "qgsfeedback.h"
#include "qgsmessagelog.h"
#include "qgstaskmanager.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

using namespace Qt::StringLiterals;

//! One AI tool currently waiting on background work. GUI thread only.
struct QgsAiActiveRegistration
{
    QPointer<QgsFeedback> feedback;
    QPointer<QgsAiBackgroundTask> task;
    std::function<void()> cancelHook;
    QPointer<QEventLoop> waitLoop;
    QString label;
    bool canceledByUser = false;
};

namespace
{
  // Ahead of long Toolbox jobs in the shared pool: the agent turn is waiting on it.
  constexpr int AI_TASK_PRIORITY = 100;

  QList<std::shared_ptr<QgsAiActiveRegistration>> sActiveRegistrations;
  std::function<void( const QString &label, double progress )> sProgressHandler;

  std::shared_ptr<QgsAiActiveRegistration> registerActive( const QString &label )
  {
    auto registration = std::make_shared<QgsAiActiveRegistration>();
    registration->label = label;
    sActiveRegistrations.append( registration );
    return registration;
  }

  void reportProgress( const QString &label, double progress )
  {
    if ( sProgressHandler )
      sProgressHandler( label, progress );
  }

  class RegistrationGuard
  {
    public:
      explicit RegistrationGuard( std::shared_ptr<QgsAiActiveRegistration> registration )
        : mRegistration( std::move( registration ) )
      {}

      ~RegistrationGuard() { sActiveRegistrations.removeAll( mRegistration ); }

      RegistrationGuard( const RegistrationGuard & ) = delete;
      RegistrationGuard &operator=( const RegistrationGuard & ) = delete;

    private:
      std::shared_ptr<QgsAiActiveRegistration> mRegistration;
  };
} // namespace

QgsAiBackgroundTask::QgsAiBackgroundTask( const QString &description, std::shared_ptr<QgsFeedback> feedback )
  : QgsTask( description, QgsTask::CanCancel | QgsTask::CancelWithoutPrompt | QgsTask::Silent )
  , mFeedback( feedback ? std::move( feedback ) : std::make_shared<QgsFeedback>() )
{}

void QgsAiBackgroundTask::cancel()
{
  if ( mUserCancelArmed )
    mCanceledByUser = true;
  mFeedback->cancel();
  QgsTask::cancel();
}

QgsAiFunctionTask::QgsAiFunctionTask( const QString &description, QgsAiBackgroundWork work, std::shared_ptr<QgsFeedback> feedback )
  : QgsAiBackgroundTask( description, std::move( feedback ) )
  , mWork( std::move( work ) )
{}

bool QgsAiFunctionTask::run()
{
  if ( isCanceled() || !mWork )
    return false;
  QgsFeedback *feedback = mFeedback.get();
  connect( feedback, &QgsFeedback::progressChanged, this, &QgsAiFunctionTask::setProgress );
  return mWork( feedback ) && !isCanceled() && !feedback->isCanceled();
}

QgsAiActiveFeedbackScope::QgsAiActiveFeedbackScope( QgsFeedback *feedback, const QString &label )
  : mRegistration( registerActive( label ) )
{
  mRegistration->feedback = feedback;
  if ( feedback )
  {
    // Auto connection to a GUI-thread context: the handler never runs on a worker thread.
    mProgressConnection = QObject::connect( feedback, &QgsFeedback::progressChanged, QCoreApplication::instance(), [label]( double progress ) { reportProgress( label, progress ); } );
  }
}

QgsAiActiveFeedbackScope::~QgsAiActiveFeedbackScope()
{
  if ( mProgressConnection )
    QObject::disconnect( mProgressConnection );
  sActiveRegistrations.removeAll( mRegistration );
}

bool QgsAiActiveFeedbackScope::canceledByUser() const
{
  return mRegistration->canceledByUser;
}

QgsAiCancelHookScope::QgsAiCancelHookScope( const QString &label, std::function<void()> onCancel )
  : mRegistration( registerActive( label ) )
{
  mRegistration->cancelHook = std::move( onCancel );
}

QgsAiCancelHookScope::~QgsAiCancelHookScope()
{
  sActiveRegistrations.removeAll( mRegistration );
}

bool QgsAiCancelHookScope::canceledByUser() const
{
  return mRegistration->canceledByUser;
}

void qgsAiSetBackgroundToolProgressHandler( const std::function<void( const QString &label, double progress )> &handler )
{
  sProgressHandler = handler;
}

bool qgsAiHasActiveBackgroundTool()
{
  return !sActiveRegistrations.isEmpty();
}

void qgsAiCancelActiveBackgroundTool()
{
  // Copy: a cancel hook or a terminated task can end its registration while we iterate.
  const QList<std::shared_ptr<QgsAiActiveRegistration>> registrations = sActiveRegistrations;
  for ( const std::shared_ptr<QgsAiActiveRegistration> &registration : registrations )
  {
    registration->canceledByUser = true;
    if ( registration->feedback )
      registration->feedback->cancel();
    if ( registration->task )
      registration->task->cancel();
    if ( registration->cancelHook )
      registration->cancelHook();
  }
}

bool qgsAiWaitForActiveTasks( int timeoutMs )
{
  QgsTaskManager *manager = QgsApplication::taskManager();
  if ( !manager || manager->countActiveTasks() == 0 )
    return true;

  QEventLoop loop;
  const auto quitWhenIdle = [&loop, manager]() {
    if ( manager->countActiveTasks() == 0 )
      loop.quit();
  };
  QObject::connect( manager, &QgsTaskManager::allTasksFinished, &loop, &QEventLoop::quit );
  // Safety net in case the last task ends between the check above and the connection.
  QTimer poll;
  poll.setInterval( 50 );
  QObject::connect( &poll, &QTimer::timeout, &loop, quitWhenIdle );
  poll.start();
  QTimer::singleShot( std::max( 0, timeoutMs ), &loop, &QEventLoop::quit );
  loop.exec( QEventLoop::ExcludeUserInputEvents );
  return manager->countActiveTasks() == 0;
}

QgsAiProcessResult qgsAiRunProcess( const QString &label, const QString &program, const QStringList &arguments, int timeoutMs, const QStringList &unsetVariables )
{
  QgsAiProcessResult result;
  QProcess process;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  for ( const QString &variable : unsetVariables )
    environment.remove( variable );
  process.setProcessEnvironment( environment );

  QEventLoop loop;
  QObject::connect( &process, &QProcess::finished, &loop, &QEventLoop::quit );
  QObject::connect( &process, &QProcess::errorOccurred, &loop, [&loop, &process]( QProcess::ProcessError error ) {
    if ( error == QProcess::FailedToStart || process.state() == QProcess::NotRunning )
      loop.quit();
  } );
  // Keep the pipes drained: a verbose process must never block on a full pipe.
  QObject::connect( &process, &QProcess::readyReadStandardOutput, &loop, [&result, &process]() { result.standardOutput += QString::fromUtf8( process.readAllStandardOutput() ); } );
  QObject::connect( &process, &QProcess::readyReadStandardError, &loop, [&result, &process]() { result.standardError += QString::fromUtf8( process.readAllStandardError() ); } );
  QTimer timeout;
  timeout.setSingleShot( true );
  QObject::connect( &timeout, &QTimer::timeout, &loop, [&result, &loop]() {
    result.timedOut = true;
    loop.quit();
  } );

  process.start( program, arguments );
  if ( !process.waitForStarted( 10000 ) )
  {
    result.error = process.errorString();
    return result;
  }
  result.started = true;
  {
    const QgsAiCancelHookScope cancelScope( label, [&loop]() { loop.quit(); } );
    timeout.start( std::max( 1, timeoutMs ) );
    if ( process.state() != QProcess::NotRunning )
      loop.exec();
    result.canceled = cancelScope.canceledByUser();
  }
  if ( process.state() != QProcess::NotRunning )
  {
    // Stopped or timed out: end the process, politely first.
    process.terminate();
    if ( !process.waitForFinished( 1000 ) )
    {
      process.kill();
      process.waitForFinished( 1000 );
    }
  }
  result.standardOutput += QString::fromUtf8( process.readAllStandardOutput() );
  result.standardError += QString::fromUtf8( process.readAllStandardError() );
  if ( !result.canceled && !result.timedOut )
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
  return result;
}

void qgsAiQuitBackgroundWaitLoopsForTesting()
{
  for ( const std::shared_ptr<QgsAiActiveRegistration> &registration : std::as_const( sActiveRegistrations ) )
  {
    if ( registration->waitLoop )
      registration->waitLoop->quit();
  }
}

QgsAiTaskWaitResult qgsAiRunTaskWithEventLoop( QgsAiBackgroundTask *task, const QString &label, const std::function<void()> &onFinished )
{
  QgsAiTaskWaitResult result;
  if ( !task )
  {
    result.error = u"Task is not available."_s;
    return result;
  }

  if ( !QgsApplication::taskManager() )
  {
    result.error = u"Task manager is not available."_s;
    delete task;
    return result;
  }

  if ( task->status() == QgsTask::Terminated )
  {
    // The task gave up before being queued (e.g. Processing prepare() failed). That is a tool
    // error the caller reports from its feedback, not a user cancel.
    if ( onFinished )
      onFinished();
    result.error = u"Task failed to start."_s;
    delete task;
    return result;
  }

  QEventLoop loop;
  const std::shared_ptr<QgsAiActiveRegistration> registration = registerActive( label );
  registration->feedback = task->feedback();
  registration->task = task;
  registration->waitLoop = &loop;
  const RegistrationGuard guard( registration );

  // Every slot below uses &loop as context, so none of them can run after this function returns.
  bool done = false;
  bool completedOk = false;
  bool canceledByUser = false;
  const auto finishRun = [&loop, &done, &completedOk, &canceledByUser, &onFinished, task]() {
    done = true;
    completedOk = task->status() == QgsTask::Complete;
    canceledByUser = task->canceledByUser();
    if ( onFinished )
      onFinished();
    loop.quit();
  };
  QObject::connect( task, &QgsTask::taskCompleted, &loop, finishRun );
  QObject::connect( task, &QgsTask::taskTerminated, &loop, finishRun );
  // Auto: direct when QgsTask re-emits progress on this thread, queued when a task emits it from
  // its worker. Either way the handler runs here, and before the completion that quits the loop.
  QObject::connect( task, &QgsTask::progressChanged, &loop, [label]( double progress ) { reportProgress( label, progress ); } );

  // The task manager cancels the task when one of these layers is removed. Remember them to
  // report that as a tool error instead of a Stop.
  QList<QPointer<QgsMapLayer>> dependentLayers;
  const QList<QgsMapLayer *> taskDependentLayers = task->dependentLayers();
  for ( QgsMapLayer *layer : taskDependentLayers )
    dependentLayers << layer;

  const QPointer<QgsAiBackgroundTask> taskGuard( task );
  task->armUserCancel();
  QgsApplication::taskManager()->addTask( task, AI_TASK_PRIORITY );
  if ( !done )
    loop.exec();

  if ( !done )
  {
    // Something stopped every event loop (QCoreApplication::exit() when Strata quits) while
    // the worker still runs. The task owns what it uses: ask it to stop and never touch it again.
    if ( taskGuard )
      taskGuard->cancel();
    result.canceled = true;
    result.abandoned = true;
    result.error = u"Strata is closing; the background task was stopped."_s;
    return result;
  }

  const bool layerRemoved = std::any_of( dependentLayers.cbegin(), dependentLayers.cend(), []( const QPointer<QgsMapLayer> &layer ) { return layer.isNull(); } );
  if ( layerRemoved && !registration->canceledByUser )
  {
    result.layerRemoved = true;
    result.error = u"A layer used by the task was removed while it was running."_s;
    return result;
  }

  result.canceled = canceledByUser || registration->canceledByUser;
  result.succeeded = completedOk && !result.canceled;
  if ( !result.succeeded )
    result.error = result.canceled ? u"Canceled."_s : u"Background task failed."_s;
  return result;
}

QgsAiTaskWaitResult qgsAiRunFunction( const QString &description, QgsAiBackgroundWork work, const QgsAiBackgroundRunOptions &options )
{
  QgsAiTaskWaitResult result;
  if ( !work )
  {
    result.error = u"Task is not available."_s;
    return result;
  }

  if ( options.forceGuiThread || !QgsApplication::taskManager() )
  {
    QgsFeedback feedback;
    const QgsAiActiveFeedbackScope scope( &feedback, description );
    const bool ok = work( &feedback );
    result.canceled = scope.canceledByUser();
    result.succeeded = ok && !result.canceled;
    if ( !result.succeeded )
      result.error = result.canceled ? u"Canceled."_s : u"Task failed."_s;
    return result;
  }

  auto *task = new QgsAiFunctionTask( description, std::move( work ) );
  if ( !options.dependentLayers.isEmpty() )
    task->setDependentLayers( options.dependentLayers );
  return qgsAiRunTaskWithEventLoop( task, description );
}

void qgsAiLogPerf( const QString &tool, const QString &phase, qint64 elapsedMs )
{
  QgsMessageLog::logMessage( u"%1 %2 elapsedMs=%3"_s.arg( tool, phase ).arg( elapsedMs ), u"AI/Perf"_s, Qgis::MessageLevel::Info, false );
}

namespace
{
  //! Process-wide monotonic clock shared by perf scopes and the stall monitor.
  qint64 aiPerfClockMs()
  {
    static const QElapsedTimer clock = []() {
      QElapsedTimer timer;
      timer.start();
      return timer;
    }();
    return clock.elapsed();
  }

  //! A GUI-thread QgsAiPerfScope, kept while the stall monitor runs so a stall can be attributed.
  struct AiPerfRecord
  {
      qint64 id = -1;
      QString label;
      qint64 startMs = 0;
      //! -1 while the scope is still open.
      qint64 endMs = -1;
  };

  QMutex sAiPerfRecordsMutex;
  QList<AiPerfRecord> sAiPerfRecords;
  qint64 sAiPerfNextRecordId = 0;
  std::atomic_bool sAiPerfStallMonitorActive { false };

  class AiGuiStallMonitor : public QObject
  {
    public:
      explicit AiGuiStallMonitor( QObject *parent )
        : QObject( parent )
      {
        mTimer.setTimerType( Qt::PreciseTimer );
        mTimer.setInterval( TICK_MS );
        QObject::connect( &mTimer, &QTimer::timeout, this, [this]() { tick(); } );
      }

      void start( int thresholdMs )
      {
        mThresholdMs = thresholdMs;
        mLastTickMs = aiPerfClockMs();
        mTimer.start();
      }

      void stop() { mTimer.stop(); }

    private:
      static constexpr int TICK_MS = 20;

      void tick()
      {
        const qint64 now = aiPerfClockMs();
        const qint64 stallMs = now - mLastTickMs - TICK_MS;
        if ( stallMs > mThresholdMs )
          report( mLastTickMs, now, stallMs );
        mLastTickMs = now;
        prune( now );
      }

      static void report( qint64 fromMs, qint64 toMs, qint64 stallMs )
      {
        QStringList labels;
        {
          const QMutexLocker locker( &sAiPerfRecordsMutex );
          for ( const AiPerfRecord &record : std::as_const( sAiPerfRecords ) )
          {
            // Name only work that explains a real share of the stall: a quick scope that merely
            // ran during a long stall caused by something else is not the culprit.
            const qint64 overlapMs = std::min( record.endMs < 0 ? toMs : record.endMs, toMs ) - std::max( record.startMs, fromMs );
            if ( ( overlapMs * 3 >= stallMs || overlapMs >= 100 ) && !labels.contains( record.label ) )
              labels << record.label;
          }
        }
        QgsMessageLog::logMessage( u"gui_stall ms=%1 during=%2"_s.arg( stallMs ).arg( labels.isEmpty() ? u"unknown"_s : labels.join( ',' ) ), u"AI/Perf"_s, Qgis::MessageLevel::Info, false );
      }

      static void prune( qint64 nowMs )
      {
        // Keep closed scopes long enough to attribute the stall that just ended.
        const QMutexLocker locker( &sAiPerfRecordsMutex );
        sAiPerfRecords
          .erase( std::remove_if( sAiPerfRecords.begin(), sAiPerfRecords.end(), [nowMs]( const AiPerfRecord &record ) { return record.endMs >= 0 && record.endMs < nowMs - 10000; } ), sAiPerfRecords.end() );
      }

      QTimer mTimer;
      int mThresholdMs = 0;
      qint64 mLastTickMs = 0;
  };

  QPointer<AiGuiStallMonitor> sAiPerfStallMonitor;

  bool aiPerfOnGuiThread()
  {
    return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread();
  }
} // namespace

QgsAiPerfScope::QgsAiPerfScope( const QString &tool, const QString &phase, int minLogMs )
  : mTool( tool )
  , mPhase( phase )
  , mMinLogMs( minLogMs )
  , mStartMs( aiPerfClockMs() )
{
  if ( sAiPerfStallMonitorActive && aiPerfOnGuiThread() )
  {
    const QMutexLocker locker( &sAiPerfRecordsMutex );
    mRecordId = sAiPerfNextRecordId++;
    AiPerfRecord record;
    record.id = mRecordId;
    record.label = u"%1:%2"_s.arg( tool, phase );
    record.startMs = mStartMs;
    sAiPerfRecords.append( record );
  }
}

QgsAiPerfScope::~QgsAiPerfScope()
{
  const qint64 endMs = aiPerfClockMs();
  if ( mRecordId >= 0 )
  {
    const QMutexLocker locker( &sAiPerfRecordsMutex );
    for ( AiPerfRecord &record : sAiPerfRecords )
    {
      if ( record.id == mRecordId )
      {
        record.endMs = endMs;
        break;
      }
    }
  }
  if ( endMs - mStartMs >= mMinLogMs )
    qgsAiLogPerf( mTool, mPhase, endMs - mStartMs );
}

qint64 QgsAiPerfScope::elapsedMs() const
{
  return aiPerfClockMs() - mStartMs;
}

void qgsAiSetGuiStallMonitorThreshold( int thresholdMs )
{
  if ( thresholdMs <= 0 )
  {
    sAiPerfStallMonitorActive = false;
    if ( sAiPerfStallMonitor )
      sAiPerfStallMonitor->stop();
    return;
  }

  if ( !QCoreApplication::instance() )
    return;
  if ( !sAiPerfStallMonitor )
    sAiPerfStallMonitor = new AiGuiStallMonitor( QCoreApplication::instance() );
  sAiPerfStallMonitor->start( thresholdMs );
  sAiPerfStallMonitorActive = true;
}

bool qgsAiProviderUsesTransaction( const QgsVectorLayer *layer )
{
  return layer && layer->dataProvider() && layer->dataProvider()->transaction();
}

QString qgsAiGuiThreadExpressionFunction( const QString &expression )
{
  if ( expression.trimmed().isEmpty() )
    return QString();
  const QgsExpression parsed( expression );
  if ( parsed.hasParserError() )
    return QString();

  // Marked "NOT thread safe" in qgsexpressionfunction.cpp: they read a live layer. eval() runs an
  // expression only known at run time.
  static const QSet<QString> sLiveLayerFunctions { u"represent_value"_s, u"represent_attributes"_s, u"maptip"_s, u"display_expression"_s, u"eval"_s };

  // Sorted, so the name reported for an expression is always the same.
  const QSet<QString> referenced = parsed.referencedFunctions();
  QStringList names( referenced.cbegin(), referenced.cend() );
  names.sort();
  const QStringList &builtins = QgsExpression::BuiltinFunctions();
  for ( const QString &name : std::as_const( names ) )
  {
    if ( sLiveLayerFunctions.contains( name ) || name.startsWith( "overlay_"_L1 ) )
      return name;
    const int index = QgsExpression::functionIndex( name );
    if ( index < 0 )
      continue;
    const QgsExpressionFunction *function = QgsExpression::Functions().at( index );
    // aggregate(), relation_aggregate() and the layer aggregates such as sum() or mean() call
    // QgsVectorLayer::aggregate() on the live layer from the evaluating thread.
    if ( function->groups().contains( "Aggregates"_L1 ) )
      return name;
    // Registered from Python (@qgsfunction) or by a plugin. Context functions such as
    // project_color() read copies made for the evaluation and are safe.
    if ( !builtins.contains( name ) && !dynamic_cast<const QgsScopedExpressionFunction *>( function ) )
      return name;
  }
  return QString();
}

QgsAiLayerChangeWatch::QgsAiLayerChangeWatch( QgsVectorLayer *layer )
{
  if ( !layer )
    return;
  const auto mark = [this]() { mChanged = true; };
  mConnections << QObject::connect( layer, &QgsVectorLayer::layerModified, mark );
  mConnections << QObject::connect( layer, &QgsVectorLayer::editingStarted, mark );
  mConnections << QObject::connect( layer, &QgsVectorLayer::editingStopped, mark );
  mConnections << QObject::connect( layer, &QgsMapLayer::dataChanged, mark );
  mConnections << QObject::connect( layer, &QgsVectorLayer::updatedFields, mark );
  mConnections << QObject::connect( layer, &QgsVectorLayer::subsetStringChanged, mark );
}

QgsAiLayerChangeWatch::~QgsAiLayerChangeWatch()
{
  for ( const QMetaObject::Connection &connection : std::as_const( mConnections ) )
    QObject::disconnect( connection );
}
