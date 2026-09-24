/***************************************************************************
  testqgsaitaskrunner.cpp
  --------------------------
  begin                : September 2026
***************************************************************************/

#include "ai/tools/qgsaitaskrunner.h"
#include "qgsapplication.h"
#include "qgsfeedback.h"
#include "qgstest.h"

#include <QThread>
#include <QTimer>
#include <memory>

using namespace Qt::StringLiterals;

class TestQgsAiTaskRunner : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void runsWorkOffMainThread();
    void pumpsInterfaceDuringWait();
    void cancelViaStopSetsCanceled();
    void cancelViaTaskSetsCanceled();
    void progressHandlerReceivesUpdates();
};

void TestQgsAiTaskRunner::initTestCase()
{
  QgsApplication::initQgis();
}

void TestQgsAiTaskRunner::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsAiTaskRunner::runsWorkOffMainThread()
{
  QVERIFY( QgsApplication::taskManager() );
  const QThread *mainThread = QThread::currentThread();
  const QThread *workerThread = nullptr;
  auto feedback = std::make_unique<QgsFeedback>();
  auto *task = new QgsAiFunctionTask(
    u"off-thread"_s,
    [&]( QgsFeedback * ) {
      workerThread = QThread::currentThread();
      return true;
    },
    feedback.get()
  );
  const QgsAiTaskWaitResult result = qgsAiRunTaskWithEventLoop( task, feedback.get(), u"off-thread"_s );
  QVERIFY( result.succeeded );
  QVERIFY( !result.canceled );
  QVERIFY( workerThread );
  QVERIFY( workerThread != mainThread );
}

void TestQgsAiTaskRunner::pumpsInterfaceDuringWait()
{
  auto feedback = std::make_unique<QgsFeedback>();
  bool interfaceEventsRan = false;
  QTimer interfaceTimer;
  interfaceTimer.setSingleShot( true );
  QObject::connect( &interfaceTimer, &QTimer::timeout, &interfaceTimer, [&interfaceEventsRan]() { interfaceEventsRan = true; } );
  interfaceTimer.start( 0 );

  auto *task = new QgsAiFunctionTask(
    u"pump"_s,
    []( QgsFeedback *workerFeedback ) {
      int spins = 0;
      while ( !workerFeedback->isCanceled() && spins < 200 )
      {
        QThread::msleep( 5 );
        ++spins;
      }
      return true;
    },
    feedback.get()
  );
  QTimer::singleShot( 20, feedback.get(), [&]() { feedback->cancel(); } );
  const QgsAiTaskWaitResult result = qgsAiRunTaskWithEventLoop( task, feedback.get(), u"pump"_s );
  QVERIFY( interfaceEventsRan );
  QVERIFY( result.canceled || result.succeeded );
}

void TestQgsAiTaskRunner::cancelViaStopSetsCanceled()
{
  auto feedback = std::make_unique<QgsFeedback>();
  QTimer::singleShot( 0, []() { qgsAiCancelActiveBackgroundTool(); } );
  auto *task = new QgsAiFunctionTask(
    u"stop"_s,
    []( QgsFeedback *workerFeedback ) {
      while ( !workerFeedback->isCanceled() )
        QThread::msleep( 5 );
      return false;
    },
    feedback.get()
  );
  const QgsAiTaskWaitResult result = qgsAiRunTaskWithEventLoop( task, feedback.get(), u"stop"_s );
  QVERIFY( result.canceled );
  QVERIFY( !result.succeeded );
}

void TestQgsAiTaskRunner::cancelViaTaskSetsCanceled()
{
  auto feedback = std::make_unique<QgsFeedback>();
  QgsAiFunctionTask *task = nullptr;
  QTimer::singleShot( 0, [&task]() {
    if ( task )
      task->cancel();
  } );
  task = new QgsAiFunctionTask(
    u"task-cancel"_s,
    []( QgsFeedback *workerFeedback ) {
      while ( !workerFeedback->isCanceled() )
        QThread::msleep( 5 );
      return false;
    },
    feedback.get()
  );
  const QgsAiTaskWaitResult result = qgsAiRunTaskWithEventLoop( task, feedback.get(), u"task-cancel"_s );
  QVERIFY( result.canceled );
  QVERIFY( !result.succeeded );
}

void TestQgsAiTaskRunner::progressHandlerReceivesUpdates()
{
  QString seenLabel;
  double seenProgress = -1;
  qgsAiSetBackgroundToolProgressHandler( [&]( const QString &label, double progress ) {
    seenLabel = label;
    seenProgress = progress;
  } );
  auto feedback = std::make_unique<QgsFeedback>();
  auto *task = new QgsAiFunctionTask(
    u"progress"_s,
    []( QgsFeedback *workerFeedback ) {
      workerFeedback->setProgress( 42 );
      QThread::msleep( 20 );
      return true;
    },
    feedback.get()
  );
  const QgsAiTaskWaitResult result = qgsAiRunTaskWithEventLoop( task, feedback.get(), u"progress-label"_s );
  qgsAiSetBackgroundToolProgressHandler( {} );
  QVERIFY( result.succeeded );
  QCOMPARE( seenLabel, u"progress-label"_s );
  QVERIFY( seenProgress >= 0 );
}

QGSTEST_MAIN( TestQgsAiTaskRunner )
#include "testqgsaitaskrunner.moc"
