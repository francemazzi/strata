/***************************************************************************
  testqgsaiindexingthrottle.cpp
  -----------------------------
  begin                : September 2026
  copyright            : (C) 2026
***************************************************************************/

#include <thread>

#include "ai/index/qgsaiindexingthrottle.h"
#include "qgsfeedback.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QElapsedTimer>
#include <QString>

using namespace Qt::StringLiterals;

class TestQgsAiIndexingThrottle : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void threadsFollowTheSpeed();
    void pausesKeepTheAverageLoadLow();
    void speedComesFromTheSettings();
    void indexingWaitsWhileTheUserWorksOnTheMap();
    void cancelInterruptsTheWait();
    void onlyBackgroundIndexingIsThrottled();
};

void TestQgsAiIndexingThrottle::init()
{
  // The test machine may run on battery: only the user activity rule is under test here.
  QgsSettings().setValue( QgsAiIndexingThrottle::pauseOnBatterySettingsKey(), false );
}

void TestQgsAiIndexingThrottle::cleanup()
{
  QgsSettings settings;
  settings.remove( QgsAiIndexingThrottle::pauseOnBatterySettingsKey() );
  settings.remove( QgsAiIndexingThrottle::speedSettingsKey() );
}

void TestQgsAiIndexingThrottle::threadsFollowTheSpeed()
{
  using Speed = QgsAiIndexingThrottle::Speed;
  QCOMPARE( QgsAiIndexingThrottle::threadsForSpeed( Speed::Low, 16 ), 1 );
  QCOMPARE( QgsAiIndexingThrottle::threadsForSpeed( Speed::Normal, 16 ), 2 );
  QCOMPARE( QgsAiIndexingThrottle::threadsForSpeed( Speed::High, 16 ), 4 );
  // Never more threads than the computer has.
  QCOMPARE( QgsAiIndexingThrottle::threadsForSpeed( Speed::High, 2 ), 2 );
  QCOMPARE( QgsAiIndexingThrottle::threadsForSpeed( Speed::Normal, 1 ), 1 );
  QCOMPARE( QgsAiIndexingThrottle::threadsForSpeed( Speed::Normal, 0 ), 1 );
}

void TestQgsAiIndexingThrottle::pausesKeepTheAverageLoadLow()
{
  using Speed = QgsAiIndexingThrottle::Speed;
  QCOMPARE( QgsAiIndexingThrottle::pauseAfterBatchMs( Speed::High, 1000 ), 0 );
  // Normal: a quarter of the batch time, within 100 ms and 1 s.
  QCOMPARE( QgsAiIndexingThrottle::pauseAfterBatchMs( Speed::Normal, 1000 ), 250 );
  QCOMPARE( QgsAiIndexingThrottle::pauseAfterBatchMs( Speed::Normal, 10 ), 100 );
  QCOMPARE( QgsAiIndexingThrottle::pauseAfterBatchMs( Speed::Normal, 20000 ), 1000 );
  // Low: as long as the batch, so one thread works half of the time.
  QCOMPARE( QgsAiIndexingThrottle::pauseAfterBatchMs( Speed::Low, 1000 ), 1000 );
}

void TestQgsAiIndexingThrottle::speedComesFromTheSettings()
{
  QgsSettings settings;
  settings.remove( QgsAiIndexingThrottle::speedSettingsKey() );
  QCOMPARE( QgsAiIndexingThrottle::speed(), QgsAiIndexingThrottle::Speed::Normal );
  settings.setValue( QgsAiIndexingThrottle::speedSettingsKey(), u"low"_s );
  QCOMPARE( QgsAiIndexingThrottle::speed(), QgsAiIndexingThrottle::Speed::Low );
  settings.setValue( QgsAiIndexingThrottle::speedSettingsKey(), u"high"_s );
  QCOMPARE( QgsAiIndexingThrottle::speed(), QgsAiIndexingThrottle::Speed::High );
  settings.setValue( QgsAiIndexingThrottle::speedSettingsKey(), u"unexpected"_s );
  QCOMPARE( QgsAiIndexingThrottle::speed(), QgsAiIndexingThrottle::Speed::Normal );
}

void TestQgsAiIndexingThrottle::indexingWaitsWhileTheUserWorksOnTheMap()
{
  QElapsedTimer clock;
  clock.start();
  QVERIFY( QgsAiIndexingThrottle::waitBeforeNextBatch( 50, nullptr ) );
  QVERIFY( clock.elapsed() < QgsAiIndexingThrottle::USER_QUIET_MS );

  QgsAiIndexingThrottle::noteUserActivity();
  QVERIFY( QgsAiIndexingThrottle::userRecentlyActive() );
  clock.restart();
  QVERIFY( QgsAiIndexingThrottle::waitBeforeNextBatch( 0, nullptr ) );
  QVERIFY2( clock.elapsed() >= QgsAiIndexingThrottle::USER_QUIET_MS - 300, QString::number( clock.elapsed() ).toUtf8().constData() );
  QVERIFY( !QgsAiIndexingThrottle::userRecentlyActive() );
}

void TestQgsAiIndexingThrottle::cancelInterruptsTheWait()
{
  QgsFeedback feedback;
  std::thread canceller( [&feedback]() {
    std::this_thread::sleep_for( std::chrono::milliseconds( 150 ) );
    feedback.cancel();
  } );
  QElapsedTimer clock;
  clock.start();
  const bool completed = QgsAiIndexingThrottle::waitBeforeNextBatch( 5000, &feedback );
  canceller.join();
  QVERIFY( !completed );
  QVERIFY2( clock.elapsed() < 1000, QString::number( clock.elapsed() ).toUtf8().constData() );
}

void TestQgsAiIndexingThrottle::onlyBackgroundIndexingIsThrottled()
{
  QVERIFY( !QgsAiIndexingThrottle::inBackgroundIndexing() );
  {
    const QgsAiIndexingThrottle::BackgroundIndexingScope background;
    QVERIFY( QgsAiIndexingThrottle::inBackgroundIndexing() );
    // Other threads are not marked.
    bool otherThreadMarked = true;
    std::thread other( [&otherThreadMarked]() { otherThreadMarked = QgsAiIndexingThrottle::inBackgroundIndexing(); } );
    other.join();
    QVERIFY( !otherThreadMarked );
  }
  QVERIFY( !QgsAiIndexingThrottle::inBackgroundIndexing() );
}

QGSTEST_MAIN( TestQgsAiIndexingThrottle )
#include "testqgsaiindexingthrottle.moc"
