/***************************************************************************
  testqgsaitaskrunner.cpp
  --------------------------
  begin                : September 2026
***************************************************************************/

#include <atomic>
#include <memory>
#include <vector>

#include "ai/tools/qgsaitaskrunner.h"
#include "qgsapplication.h"
#include "qgsexpression.h"
#include "qgsexpressionfunction.h"
#include "qgsfeedback.h"
#include "qgsproject.h"
#include "qgstaskmanager.h"
#include "qgstest.h"
#include "qgsvectorlayer.h"

#include <QPointer>
#include <QScopeGuard>
#include <QString>
#include <QThread>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace
{
  //! Blocks the worker until \a flag is set or \a feedback is canceled; true if the flag was set.
  bool waitForFlag( const std::shared_ptr<std::atomic_bool> &flag, QgsFeedback *feedback, int timeoutMs = 10000 )
  {
    for ( int waited = 0; waited < timeoutMs && !*flag && !feedback->isCanceled(); waited += 5 )
      QThread::msleep( 5 );
    return *flag;
  }

  //! Mimics AiProcessingRunnerTask when prepare() fails: it cancels itself before being queued.
  class SelfCancelingTask : public QgsAiBackgroundTask
  {
    public:
      SelfCancelingTask()
        : QgsAiBackgroundTask( u"self-cancel"_s )
      {
        cancel();
      }

    protected:
      bool run() override { return true; }
  };

  //! Stands in for a function registered from Python with @qgsfunction: neither built in nor a context function.
  class RegisteredTestFunction : public QgsExpressionFunction
  {
    public:
      RegisteredTestFunction()
        : QgsExpressionFunction( u"strata_ai_test_function"_s, 0, u"Custom"_s )
      {}

      QVariant func( const QVariantList &, const QgsExpressionContext *, QgsExpression *, const QgsExpressionNodeFunction * ) override { return 1; }
  };
} // namespace

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
    void internalCancelIsNotUserCancel();
    void failedWorkIsNotCanceled();
    void abandonedWaitLeavesTaskSafe();
    void dependentLayerRemovalStopsTask();
    void stopWinsOverLayerRemoval();
    void waitForActiveTasksReturnsWhenWorkersStop();
    void waitForActiveTasksTimesOut();
    void guiThreadWorkRunsInline();
    void tasksAreSilentAndCancelWithoutPrompt();
    void cancelHookRunsOnStop();
    void progressHandlerReceivesUpdates();
    void guiThreadExpressionFunction_data();
    void guiThreadExpressionFunction();
    void registeredFunctionNeedsGuiThread();
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
  auto workerThread = std::make_shared<QThread *>( nullptr );
  const QgsAiTaskWaitResult result = qgsAiRunFunction( u"off-thread"_s, [workerThread]( QgsFeedback * ) {
    *workerThread = QThread::currentThread();
    return true;
  } );
  QVERIFY( result.succeeded );
  QVERIFY( !result.canceled );
  QVERIFY( *workerThread );
  QVERIFY( *workerThread != QThread::currentThread() );
  QVERIFY( !qgsAiHasActiveBackgroundTool() );
}

void TestQgsAiTaskRunner::pumpsInterfaceDuringWait()
{
  // Only a GUI-thread timer can release the worker: success proves the wait pumped events.
  auto released = std::make_shared<std::atomic_bool>( false );
  QTimer::singleShot( 0, [released]() { *released = true; } );
  const QgsAiTaskWaitResult result = qgsAiRunFunction( u"pump"_s, [released]( QgsFeedback *feedback ) { return waitForFlag( released, feedback ); } );
  QVERIFY( result.succeeded );
}

void TestQgsAiTaskRunner::cancelViaStopSetsCanceled()
{
  QTimer::singleShot( 0, []() { qgsAiCancelActiveBackgroundTool(); } );
  const QgsAiTaskWaitResult result = qgsAiRunFunction( u"stop"_s, []( QgsFeedback *feedback ) {
    while ( !feedback->isCanceled() )
      QThread::msleep( 5 );
    return false;
  } );
  QVERIFY( result.canceled );
  QVERIFY( !result.succeeded );
  QVERIFY( !result.abandoned );
}

void TestQgsAiTaskRunner::cancelViaTaskSetsCanceled()
{
  QPointer<QgsAiFunctionTask> task;
  QTimer::singleShot( 0, [&task]() {
    if ( task )
      task->cancel();
  } );
  task = new QgsAiFunctionTask( u"task-cancel"_s, []( QgsFeedback *feedback ) {
    while ( !feedback->isCanceled() )
      QThread::msleep( 5 );
    return false;
  } );
  const QgsAiTaskWaitResult result = qgsAiRunTaskWithEventLoop( task, u"task-cancel"_s );
  QVERIFY( result.canceled );
  QVERIFY( !result.succeeded );
}

void TestQgsAiTaskRunner::internalCancelIsNotUserCancel()
{
  auto *task = new SelfCancelingTask();
  QVERIFY( task->feedback()->isCanceled() );
  bool finishedCalled = false;
  const QgsAiTaskWaitResult result = qgsAiRunTaskWithEventLoop( task, u"self-cancel"_s, [&finishedCalled]() { finishedCalled = true; } );
  QVERIFY( finishedCalled );
  QVERIFY( !result.succeeded );
  QVERIFY( !result.canceled );
  QVERIFY( !result.error.isEmpty() );
}

void TestQgsAiTaskRunner::failedWorkIsNotCanceled()
{
  const QgsAiTaskWaitResult result = qgsAiRunFunction( u"fails"_s, []( QgsFeedback * ) { return false; } );
  QVERIFY( !result.succeeded );
  QVERIFY( !result.canceled );
}

void TestQgsAiTaskRunner::abandonedWaitLeavesTaskSafe()
{
  // Strata quitting stops every event loop while the worker still runs. The runner must
  // return without touching the task, and the task must finish on its own state.
  struct State
  {
      std::atomic_bool started { false };
      std::atomic_bool finished { false };
      std::vector<int> written;
  };
  auto state = std::make_shared<State>();
  QTimer::singleShot( 0, []() { qgsAiQuitBackgroundWaitLoopsForTesting(); } );
  const QgsAiTaskWaitResult result = qgsAiRunFunction( u"abandoned"_s, [state]( QgsFeedback *feedback ) {
    state->started = true;
    for ( int i = 0; i < 100 && !feedback->isCanceled(); ++i )
    {
      state->written.push_back( i );
      QThread::msleep( 5 );
    }
    state->finished = true;
    return true;
  } );
  QVERIFY( result.abandoned );
  QVERIFY( result.canceled );
  QVERIFY( !result.succeeded );
  QVERIFY( !qgsAiHasActiveBackgroundTool() );
  // The runner asked the task to stop; it winds down without the caller's stack.
  QTRY_VERIFY_WITH_TIMEOUT( state->finished || !state->started, 10000 );
  QTRY_VERIFY_WITH_TIMEOUT( QgsApplication::taskManager()->countActiveTasks() == 0, 10000 );
}

void TestQgsAiTaskRunner::dependentLayerRemovalStopsTask()
{
  auto *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326"_s, u"dependent"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer );
  const QString layerId = layer->id();

  QgsAiBackgroundRunOptions options;
  options.dependentLayers = { layer };
  QTimer::singleShot( 0, [layerId]() { QgsProject::instance()->removeMapLayer( layerId ); } );
  const QgsAiTaskWaitResult result = qgsAiRunFunction(
    u"dependent"_s,
    []( QgsFeedback *feedback ) {
      while ( !feedback->isCanceled() )
        QThread::msleep( 5 );
      return false;
    },
    options
  );
  QVERIFY( !result.succeeded );
  // Removing the layer is a tool error the model can react to, not a Stop that ends the turn.
  QVERIFY( !result.canceled );
  QVERIFY( result.layerRemoved );
  QVERIFY( !result.error.isEmpty() );
  QVERIFY( !QgsProject::instance()->mapLayer( layerId ) );
}

void TestQgsAiTaskRunner::stopWinsOverLayerRemoval()
{
  auto *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326"_s, u"dependent"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsProject::instance()->addMapLayer( layer );
  const QString layerId = layer->id();

  QgsAiBackgroundRunOptions options;
  options.dependentLayers = { layer };
  QTimer::singleShot( 0, [layerId]() {
    qgsAiCancelActiveBackgroundTool();
    QgsProject::instance()->removeMapLayer( layerId );
  } );
  const QgsAiTaskWaitResult result = qgsAiRunFunction(
    u"stop-and-remove"_s,
    []( QgsFeedback *feedback ) {
      while ( !feedback->isCanceled() )
        QThread::msleep( 5 );
      return false;
    },
    options
  );
  QVERIFY( result.canceled );
  QVERIFY( !result.layerRemoved );
}

void TestQgsAiTaskRunner::waitForActiveTasksReturnsWhenWorkersStop()
{
  // Nothing running: returns at once.
  QVERIFY( qgsAiWaitForActiveTasks( 0 ) );

  auto *task = new QgsAiFunctionTask( u"quit-wait"_s, []( QgsFeedback *feedback ) {
    while ( !feedback->isCanceled() )
      QThread::msleep( 5 );
    return false;
  } );
  const QPointer<QgsAiFunctionTask> taskGuard( task );
  QgsApplication::taskManager()->addTask( task );
  QTRY_VERIFY_WITH_TIMEOUT( !taskGuard || taskGuard->status() == QgsTask::Running, 5000 );

  task->cancel();
  QVERIFY( qgsAiWaitForActiveTasks( 5000 ) );
  QCOMPARE( QgsApplication::taskManager()->countActiveTasks(), 0 );
}

void TestQgsAiTaskRunner::waitForActiveTasksTimesOut()
{
  // A worker that ignores cancel for a while.
  auto release = std::make_shared<std::atomic_bool>( false );
  auto *task = new QgsAiFunctionTask( u"slow-quit"_s, [release]( QgsFeedback * ) {
    while ( !*release )
      QThread::msleep( 5 );
    return false;
  } );
  const QPointer<QgsAiFunctionTask> taskGuard( task );
  QgsApplication::taskManager()->addTask( task );
  QTRY_VERIFY_WITH_TIMEOUT( !taskGuard || taskGuard->status() == QgsTask::Running, 5000 );

  task->cancel();
  QVERIFY( !qgsAiWaitForActiveTasks( 50 ) );

  *release = true;
  QVERIFY( qgsAiWaitForActiveTasks( 5000 ) );
}

void TestQgsAiTaskRunner::guiThreadWorkRunsInline()
{
  QgsAiBackgroundRunOptions options;
  options.forceGuiThread = true;
  auto workerThread = std::make_shared<QThread *>( nullptr );
  const QgsAiTaskWaitResult result = qgsAiRunFunction(
    u"gui"_s,
    [workerThread]( QgsFeedback *feedback ) {
      *workerThread = QThread::currentThread();
      return qgsAiHasActiveBackgroundTool() && !feedback->isCanceled();
    },
    options
  );
  QVERIFY( result.succeeded );
  QCOMPARE( *workerThread, QThread::currentThread() );
  QVERIFY( !qgsAiHasActiveBackgroundTool() );
}

void TestQgsAiTaskRunner::tasksAreSilentAndCancelWithoutPrompt()
{
  const QgsAiFunctionTask task( u"flags"_s, []( QgsFeedback * ) { return true; } );
  QVERIFY( task.flags() & QgsTask::CanCancel );
  QVERIFY( task.flags() & QgsTask::CancelWithoutPrompt );
  QVERIFY( task.flags() & QgsTask::Silent );
  QVERIFY( !task.canceledByUser() );
}

void TestQgsAiTaskRunner::cancelHookRunsOnStop()
{
  bool hookRan = false;
  {
    const QgsAiCancelHookScope scope( u"network"_s, [&hookRan]() { hookRan = true; } );
    QVERIFY( qgsAiHasActiveBackgroundTool() );
    QVERIFY( !scope.canceledByUser() );
    qgsAiCancelActiveBackgroundTool();
    QVERIFY( hookRan );
    QVERIFY( scope.canceledByUser() );
  }
  QVERIFY( !qgsAiHasActiveBackgroundTool() );
}

void TestQgsAiTaskRunner::progressHandlerReceivesUpdates()
{
  QString seenLabel;
  double seenProgress = -1;
  qgsAiSetBackgroundToolProgressHandler( [&]( const QString &label, double progress ) {
    seenLabel = label;
    seenProgress = progress;
  } );
  const QgsAiTaskWaitResult result = qgsAiRunFunction( u"progress-label"_s, []( QgsFeedback *feedback ) {
    feedback->setProgress( 42 );
    QThread::msleep( 20 );
    return true;
  } );
  qgsAiSetBackgroundToolProgressHandler( {} );
  QVERIFY( result.succeeded );
  QCOMPARE( seenLabel, u"progress-label"_s );
  QVERIFY( seenProgress >= 0 );
}

void TestQgsAiTaskRunner::guiThreadExpressionFunction_data()
{
  QTest::addColumn<QString>( "expression" );
  QTest::addColumn<QString>( "function" );

  QTest::newRow( "share of total" ) << u"\"pop\" / sum(\"pop\")"_s << u"sum"_s;
  QTest::newRow( "grouped mean" ) << u"mean(\"pop\", group_by:=\"kind\")"_s << u"mean"_s;
  QTest::newRow( "aggregate" ) << u"aggregate('other', 'sum', \"pop\")"_s << u"aggregate"_s;
  QTest::newRow( "relation aggregate" ) << u"relation_aggregate('rel', 'count', \"fid\")"_s << u"relation_aggregate"_s;
  QTest::newRow( "nested in a branch" ) << u"CASE WHEN \"a\" > 0 THEN 1 ELSE count(\"a\") END"_s << u"count"_s;
  QTest::newRow( "overlay" ) << u"overlay_intersects('other')"_s << u"overlay_intersects"_s;
  QTest::newRow( "represent value" ) << u"represent_value(\"kind\")"_s << u"represent_value"_s;
  QTest::newRow( "display expression" ) << u"display_expression()"_s << u"display_expression"_s;
  QTest::newRow( "eval" ) << u"eval('1 + 1')"_s << u"eval"_s;
  QTest::newRow( "plain functions" ) << u"upper(\"name\") || length(\"name\")"_s << QString();
  QTest::newRow( "get_feature is thread safe" ) << u"get_feature('other', 'id', 1)"_s << QString();
  QTest::newRow( "context function" ) << u"project_color('red')"_s << QString();
  QTest::newRow( "empty" ) << QString() << QString();
  QTest::newRow( "parser error" ) << u"\"pop\" *"_s << QString();
}

void TestQgsAiTaskRunner::guiThreadExpressionFunction()
{
  QFETCH( QString, expression );
  QFETCH( QString, function );
  QCOMPARE( qgsAiGuiThreadExpressionFunction( expression ), function );
}

void TestQgsAiTaskRunner::registeredFunctionNeedsGuiThread()
{
  QVERIFY( QgsExpression::registerFunction( new RegisteredTestFunction(), true ) );
  const auto unregister = qScopeGuard( []() { QgsExpression::unregisterFunction( u"strata_ai_test_function"_s ); } );
  QCOMPARE( qgsAiGuiThreadExpressionFunction( u"strata_ai_test_function() + 1"_s ), u"strata_ai_test_function"_s );
}

QGSTEST_MAIN( TestQgsAiTaskRunner )
#include "testqgsaitaskrunner.moc"
