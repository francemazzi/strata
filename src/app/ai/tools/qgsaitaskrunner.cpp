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
#include "qgstaskmanager.h"

#include <QEventLoop>
#include <QPointer>

using namespace Qt::StringLiterals;

namespace
{
  QPointer<QgsFeedback> sActiveFeedback;
  QPointer<QgsTask> sActiveTask;
  QString sActiveLabel;
  std::function<void( const QString &label, double progress )> sProgressHandler;

  class ActiveTaskGuard
  {
    public:
      ActiveTaskGuard( QgsFeedback *feedback, QgsTask *task, const QString &label )
        : mPreviousFeedback( sActiveFeedback )
        , mPreviousTask( sActiveTask )
        , mPreviousLabel( sActiveLabel )
      {
        sActiveFeedback = feedback;
        sActiveTask = task;
        sActiveLabel = label;
      }

      ~ActiveTaskGuard()
      {
        sActiveFeedback = mPreviousFeedback;
        sActiveTask = mPreviousTask;
        sActiveLabel = mPreviousLabel;
      }

    private:
      QPointer<QgsFeedback> mPreviousFeedback;
      QPointer<QgsTask> mPreviousTask;
      QString mPreviousLabel;
  };
} // namespace

QgsAiFunctionTask::QgsAiFunctionTask( const QString &description, std::function<bool( QgsFeedback * )> work, QgsFeedback *feedback )
  : QgsTask( description, QgsTask::CanCancel | QgsTask::CancelWithoutPrompt )
  , mWork( std::move( work ) )
  , mFeedback( feedback )
{}

void QgsAiFunctionTask::cancel()
{
  if ( mFeedback )
    mFeedback->cancel();
  QgsTask::cancel();
}

bool QgsAiFunctionTask::run()
{
  if ( isCanceled() || !mWork )
    return false;
  if ( mFeedback )
    connect( mFeedback, &QgsFeedback::progressChanged, this, &QgsAiFunctionTask::setProgress );
  return mWork( mFeedback ) && !isCanceled() && !( mFeedback && mFeedback->isCanceled() );
}

QgsAiActiveFeedbackScope::QgsAiActiveFeedbackScope( QgsFeedback *feedback )
  : mFeedback( feedback )
{
  mPreviousFeedback = sActiveFeedback;
  sActiveFeedback = feedback;
}

QgsAiActiveFeedbackScope::~QgsAiActiveFeedbackScope()
{
  sActiveFeedback = mPreviousFeedback;
}

void qgsAiSetBackgroundToolProgressHandler( const std::function<void( const QString &label, double progress )> &handler )
{
  sProgressHandler = handler;
}

bool qgsAiHasActiveBackgroundTool()
{
  return !sActiveFeedback.isNull() || !sActiveTask.isNull();
}

void qgsAiCancelActiveBackgroundTool()
{
  if ( sActiveFeedback )
    sActiveFeedback->cancel();
  if ( sActiveTask )
    sActiveTask->cancel();
}

QgsAiTaskWaitResult qgsAiRunTaskWithEventLoop( QgsTask *task, QgsFeedback *feedback, const QString &label, const std::function<void()> &onFinished )
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
    if ( onFinished )
      onFinished();
    result.canceled = feedback && feedback->isCanceled();
    result.error = result.canceled ? u"Canceled."_s : u"Task failed to start."_s;
    delete task;
    return result;
  }

  const ActiveTaskGuard guard( feedback, task, label );

  QEventLoop loop;
  bool completedOk = false;
  const auto finishRun = [&loop, &completedOk, &onFinished, task]() {
    completedOk = task->status() == QgsTask::Complete;
    if ( onFinished )
      onFinished();
    loop.quit();
  };
  QObject::connect( task, &QgsTask::taskCompleted, &loop, finishRun );
  QObject::connect( task, &QgsTask::taskTerminated, &loop, finishRun );
  QObject::connect(
    task,
    &QgsTask::progressChanged,
    &loop,
    [label]( double progress ) {
      if ( sProgressHandler )
        sProgressHandler( label, progress );
    },
    Qt::QueuedConnection
  );

  QgsApplication::taskManager()->addTask( task );
  loop.exec();

  result.canceled = feedback && feedback->isCanceled();
  result.succeeded = completedOk && !result.canceled;
  if ( !result.succeeded && result.error.isEmpty() )
    result.error = result.canceled ? u"Canceled."_s : u"Background task failed."_s;
  return result;
}

QgsAiTaskWaitResult qgsAiRunFunction( const QString &description, QgsFeedback *feedback, const std::function<bool( QgsFeedback * )> &work, bool forceGuiThread )
{
  QgsAiTaskWaitResult result;
  if ( !work )
  {
    result.error = u"Task is not available."_s;
    return result;
  }

  if ( forceGuiThread || !QgsApplication::taskManager() )
  {
    QgsAiActiveFeedbackScope scope( feedback );
    const bool ok = work( feedback );
    result.canceled = feedback && feedback->isCanceled();
    result.succeeded = ok && !result.canceled;
    if ( !result.succeeded )
      result.error = result.canceled ? u"Canceled."_s : u"Task failed."_s;
    return result;
  }

  auto *task = new QgsAiFunctionTask( description, work, feedback );
  return qgsAiRunTaskWithEventLoop( task, feedback, description );
}
