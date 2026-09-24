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

#include "qgis_app.h"
#include "qgstaskmanager.h"

#include <functional>

#include <QString>

class QgsFeedback;

/**
 * QgsTask that runs a std::function on a worker thread.
 *
 * cancel() also cancels the associated QgsFeedback so Stop reaches the work.
 */
class APP_EXPORT QgsAiFunctionTask : public QgsTask
{
  public:
    QgsAiFunctionTask( const QString &description, std::function<bool( QgsFeedback * )> work, QgsFeedback *feedback );

    void cancel() override;

  protected:
    bool run() override;

  private:
    std::function<bool( QgsFeedback * )> mWork;
    QgsFeedback *mFeedback = nullptr;
};

/**
 * RAII that registers \a feedback as the active AI background tool feedback.
 * Used by run_python so Stop can cancel the Processing bridge without a QgsTask.
 */
class APP_EXPORT QgsAiActiveFeedbackScope
{
  public:
    explicit QgsAiActiveFeedbackScope( QgsFeedback *feedback );
    ~QgsAiActiveFeedbackScope();

    QgsAiActiveFeedbackScope( const QgsAiActiveFeedbackScope & ) = delete;
    QgsAiActiveFeedbackScope &operator=( const QgsAiActiveFeedbackScope & ) = delete;

  private:
    QgsFeedback *mFeedback = nullptr;
    QgsFeedback *mPreviousFeedback = nullptr;
};

struct APP_EXPORT QgsAiTaskWaitResult
{
    bool succeeded = false;
    bool canceled = false;
    QString error;
};

/**
 * Adds \a task to the task manager and pumps the GUI until it finishes.
 * Copies results in the task's finished() / connected slots before the task is deleted.
 * Never calls waitForFinished.
 */
APP_EXPORT QgsAiTaskWaitResult qgsAiRunTaskWithEventLoop( QgsTask *task, QgsFeedback *feedback, const QString &label, const std::function<void()> &onFinished = {} );

//! Reports progress for the AI background tool currently running.
APP_EXPORT void qgsAiSetBackgroundToolProgressHandler( const std::function<void( const QString &label, double progress )> &handler );
//! True while an AI tool is waiting on the shared background helper or an active feedback scope.
APP_EXPORT bool qgsAiHasActiveBackgroundTool();
//! Cancels the AI background tool currently running, including Processing and run_python.
APP_EXPORT void qgsAiCancelActiveBackgroundTool();

#endif // QGSAITASKRUNNER_H
