/***************************************************************************
  testqgsaiindexingactivity.cpp
  -----------------------------
  begin                : September 2026
  copyright            : (C) 2026
***************************************************************************/

#include <atomic>

#include "ai/index/qgsaiembeddingprovider.h"
#include "ai/index/qgsaiindexingactivity.h"
#include "ai/index/qgsaiindexingscheduler.h"
#include "ai/index/qgsaiindexingthrottle.h"
#include "ai/index/qgsailayerindexcoordinator.h"
#include "ai/index/qgsaiworkspaceindex.h"
#include "ai/qgsaiagentsessionmanager.h"
#include "ai/qgsaichatdockwidget.h"
#include "ai/qgsaifilecontextprovider.h"
#include "ai/qgsaimodelrouter.h"
#include "ai/qgsaireviewpatchengine.h"
#include "qgsapplication.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

using namespace Qt::StringLiterals;

namespace
{
  //! Takes a while per batch, so a pass is still running when the test looks at it.
  class SlowProvider : public QgsAiEmbeddingProvider
  {
    public:
      QString providerId() const override { return u"slow-local"_s; }
      QString displayName() const override { return u"Slow local"_s; }
      int embeddingDimension() const override { return 3; }
      bool isAvailable( QString *errorMessage = nullptr ) const override
      {
        if ( !available && errorMessage )
          *errorMessage = u"The local model is not installed: download it from the AI settings."_s;
        return available;
      }
      bool embed( const QStringList &texts, QList<QVector<float>> &out, QString * = nullptr, int = 64 ) override
      {
        QThread::msleep( 150 );
        out.clear();
        for ( int i = 0; i < texts.size(); ++i )
          out.append( QVector<float> { 1.0f, 0.0f, static_cast<float>( i ) } );
        return true;
      }
      std::atomic_bool available { true };
  };
} // namespace

class TestQgsAiIndexingActivity : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void progressShowsWithinASecondAndPauseStopsIt();
    void unavailableIndexIsExplained();
    void chatHeaderShowsIndexing();
};

void TestQgsAiIndexingActivity::initTestCase()
{
  // Indexing must not wait for the power adapter on the test machine.
  QgsSettings().setValue( QgsAiIndexingThrottle::pauseOnBatterySettingsKey(), false );
}

void TestQgsAiIndexingActivity::cleanupTestCase()
{
  QgsSettings().remove( QgsAiIndexingThrottle::pauseOnBatterySettingsKey() );
}

static void writeManyLines( const QString &path, int lines )
{
  QFile file( path );
  if ( !file.open( QIODevice::WriteOnly ) )
    return;
  for ( int i = 0; i < lines; ++i )
    file.write( u"line %1 about parcels, roads and trees in the municipality\n"_s.arg( i ).toUtf8() );
}

void TestQgsAiIndexingActivity::progressShowsWithinASecondAndPauseStopsIt()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  writeManyLines( QDir( root.path() ).filePath( u"notes.md"_s ), 4000 );

  QgsAiFileContextProvider contextProvider( root.path() );
  SlowProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QgsAiIndexingScheduler scheduler( &index );
  QgsAiLayerIndexCoordinator coordinator( &index );
  QgsAiIndexingActivity activity( &index, &scheduler, &coordinator );
  QVERIFY( QgsAiIndexingActivity::summaryText( activity.state() ).isEmpty() );

  QElapsedTimer clock;
  clock.start();
  scheduler.scheduleWorkspaceIndexing( 0 );
  QTRY_VERIFY_WITH_TIMEOUT( activity.state().active, 5000 );
  QVERIFY2( clock.elapsed() < 1000, QString::number( clock.elapsed() ).toUtf8().constData() );
  QVERIFY( QgsAiIndexingActivity::summaryText( activity.state() ).startsWith( u"Indexing · files"_s ) );
  QTRY_VERIFY_WITH_TIMEOUT( activity.state().filePercent > 20, 10000 );

  // Pause stops the pass within a batch…
  activity.setPaused( true );
  QTRY_VERIFY_WITH_TIMEOUT( !scheduler.isRunning(), 3000 );
  QVERIFY( activity.state().paused );
  QVERIFY( !activity.state().active );
  QCOMPARE( QgsAiIndexingActivity::summaryText( activity.state() ), u"Indexing paused"_s );
  QVERIFY( activity.state().problem.isEmpty() );

  // …and resume runs it again.
  activity.setPaused( false );
  QTRY_VERIFY_WITH_TIMEOUT( scheduler.isRunning(), 5000 );
  scheduler.shutdown();
  QTRY_VERIFY_WITH_TIMEOUT( !scheduler.isRunning(), 5000 );
}

void TestQgsAiIndexingActivity::unavailableIndexIsExplained()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  QgsAiFileContextProvider contextProvider( root.path() );
  SlowProvider provider;
  provider.available = false;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QgsAiIndexingScheduler scheduler( &index );
  QgsAiLayerIndexCoordinator coordinator( &index );
  QgsAiIndexingActivity activity( &index, &scheduler, &coordinator );

  activity.refresh();
  const QgsAiIndexingActivity::State state = activity.state();
  QCOMPARE( QgsAiIndexingActivity::summaryText( state ), u"Index unavailable"_s );
  QVERIFY( QgsAiIndexingActivity::detailText( state ).contains( u"download it from the AI settings"_s ) );

  // Nothing to say when indexing is off.
  scheduler.setAutomaticEnabled( false );
  activity.refresh();
  QVERIFY( QgsAiIndexingActivity::summaryText( activity.state() ).isEmpty() );
}

void TestQgsAiIndexingActivity::chatHeaderShowsIndexing()
{
  QTemporaryDir root;
  QVERIFY( root.isValid() );
  writeManyLines( QDir( root.path() ).filePath( u"notes.md"_s ), 4000 );

  QgsAiFileContextProvider contextProvider( root.path() );
  SlowProvider provider;
  QgsAiWorkspaceIndex index( &contextProvider, &provider );
  QgsAiIndexingScheduler scheduler( &index );
  QgsAiLayerIndexCoordinator coordinator( &index );
  QgsAiIndexingActivity activity( &index, &scheduler, &coordinator );

  QgsAiModelRouter router;
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  dock.setIndexingActivity( &activity );
  dock.show();

  QFrame *indicator = dock.findChild<QFrame *>( u"aiIndexingIndicator"_s );
  QToolButton *status = dock.findChild<QToolButton *>( u"aiIndexingStatusButton"_s );
  QToolButton *pause = dock.findChild<QToolButton *>( u"aiIndexingPauseButton"_s );
  QVERIFY( indicator && status && pause );
  QVERIFY( !indicator->isVisible() );

  scheduler.scheduleWorkspaceIndexing( 0 );
  QTRY_VERIFY_WITH_TIMEOUT( indicator->isVisible(), 1000 );
  QVERIFY( status->text().startsWith( "Indexing"_L1 ) );
  QCOMPARE( pause->text(), u"Pause"_s );

  pause->click();
  QVERIFY( activity.isPaused() );
  QCOMPARE( pause->text(), u"Resume"_s );
  QTRY_VERIFY_WITH_TIMEOUT( !scheduler.isRunning(), 3000 );

  scheduler.shutdown();
}

QGSTEST_MAIN( TestQgsAiIndexingActivity )
#include "testqgsaiindexingactivity.moc"
