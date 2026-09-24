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

#include "qgsapplication.h"
#include "qgsfeedback.h"
#include "qgsmessagelog.h"
#include "qgstaskmanager.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QPointer>
#include <QString>

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

bool qgsAiProviderUsesTransaction( const QgsVectorLayer *layer )
{
  return layer && layer->dataProvider() && layer->dataProvider()->transaction();
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
