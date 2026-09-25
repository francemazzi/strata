/***************************************************************************
    qgsaiindexingthrottle.cpp
    -------------------------
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

#include "qgsaiindexingthrottle.h"

#include <algorithm>
#include <atomic>

#include "qgsfeedback.h"
#include "qgssettings.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QThread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined( Q_OS_MACOS )
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <pthread.h>
#include <pthread/qos.h>
#elif defined( Q_OS_LINUX )
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace Qt::StringLiterals;

namespace
{
  //! Monotonic milliseconds of the last user activity on the map, 0 if none.
  std::atomic<qint64> sThrottleLastUserActivityMs { 0 };
  std::atomic<qint64> sThrottleBatteryCheckedMs { -1 };
  std::atomic_bool sThrottleOnBattery { false };
  thread_local bool tThrottleBackgroundIndexing = false;

  qint64 throttleClockMs()
  {
    static QElapsedTimer clock = []() {
      QElapsedTimer timer;
      timer.start();
      return timer;
    }();
    // Never 0, which means "no activity yet".
    return clock.elapsed() + 1;
  }

  bool throttleReadBatteryState()
  {
#ifdef Q_OS_WIN
    SYSTEM_POWER_STATUS status;
    return GetSystemPowerStatus( &status ) && status.ACLineStatus == 0;
#elif defined( Q_OS_MACOS )
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if ( !info )
      return false;
    bool battery = false;
    if ( CFStringRef type = IOPSGetProvidingPowerSourceType( info ) )
      battery = CFStringCompare( type, CFSTR( kIOPMBatteryPowerKey ), 0 ) == kCFCompareEqualTo;
    CFRelease( info );
    return battery;
#elif defined( Q_OS_LINUX )
    // On battery when a battery exists and no mains supply is online.
    const QDir supplies( u"/sys/class/power_supply"_s );
    bool hasBattery = false;
    const QStringList names = supplies.entryList( QDir::Dirs | QDir::NoDotAndDotDot );
    for ( const QString &name : names )
    {
      QFile typeFile( supplies.filePath( name + u"/type"_s ) );
      if ( !typeFile.open( QIODevice::ReadOnly ) )
        continue;
      const QByteArray type = typeFile.readAll().trimmed();
      if ( type == "Battery" )
        hasBattery = true;
      else if ( type == "Mains" )
      {
        QFile online( supplies.filePath( name + u"/online"_s ) );
        if ( online.open( QIODevice::ReadOnly ) && online.readAll().trimmed() == "1" )
          return false;
      }
    }
    return hasBattery;
#else
    return false;
#endif
  }

  //! Sleeps \a ms in short steps; false if \a feedback is canceled meanwhile.
  bool throttleSleep( int ms, QgsFeedback *feedback )
  {
    QElapsedTimer timer;
    timer.start();
    while ( timer.elapsed() < ms )
    {
      if ( feedback && feedback->isCanceled() )
        return false;
      QThread::msleep( static_cast<unsigned long>( std::min<qint64>( 50, ms - timer.elapsed() ) ) );
    }
    return !( feedback && feedback->isCanceled() );
  }
} // namespace

QString QgsAiIndexingThrottle::speedSettingsKey()
{
  return u"strata/index/speed"_s;
}

QString QgsAiIndexingThrottle::pauseOnBatterySettingsKey()
{
  return u"strata/index/pause_on_battery"_s;
}

QgsAiIndexingThrottle::Speed QgsAiIndexingThrottle::speed()
{
  const QString value = QgsSettings().value( speedSettingsKey(), u"normal"_s ).toString().trimmed().toLower();
  if ( value == "low"_L1 )
    return Speed::Low;
  if ( value == "high"_L1 )
    return Speed::High;
  return Speed::Normal;
}

int QgsAiIndexingThrottle::threadsForSpeed( Speed speed, int idealThreads )
{
  const int available = std::max( 1, idealThreads );
  switch ( speed )
  {
    case Speed::Low:
      return 1;
    case Speed::Normal:
      return std::min( 2, available );
    case Speed::High:
      return std::min( 4, available );
  }
  return 1;
}

int QgsAiIndexingThrottle::pauseAfterBatchMs( Speed speed, qint64 batchMs )
{
  switch ( speed )
  {
    case Speed::Low:
      // Idle as long as it worked: half the time on one thread.
      return static_cast<int>( std::clamp<qint64>( batchMs, 200, 5000 ) );
    case Speed::Normal:
      return static_cast<int>( std::clamp<qint64>( batchMs / 4, 100, 1000 ) );
    case Speed::High:
      return 0;
  }
  return 0;
}

void QgsAiIndexingThrottle::noteUserActivity()
{
  sThrottleLastUserActivityMs = throttleClockMs();
}

bool QgsAiIndexingThrottle::userRecentlyActive()
{
  const qint64 last = sThrottleLastUserActivityMs.load();
  return last > 0 && throttleClockMs() - last < USER_QUIET_MS;
}

bool QgsAiIndexingThrottle::onBatteryPower()
{
  const qint64 now = throttleClockMs();
  const qint64 checked = sThrottleBatteryCheckedMs.load();
  if ( checked < 0 || now - checked > 10000 )
  {
    sThrottleOnBattery = throttleReadBatteryState();
    sThrottleBatteryCheckedMs = now;
  }
  return sThrottleOnBattery;
}

bool QgsAiIndexingThrottle::waitBeforeNextBatch( int pauseMs, QgsFeedback *feedback )
{
  if ( pauseMs > 0 && !throttleSleep( pauseMs, feedback ) )
    return false;
  const bool pauseOnBattery = QgsSettings().value( pauseOnBatterySettingsKey(), true ).toBool();
  while ( userRecentlyActive() || ( pauseOnBattery && onBatteryPower() ) )
  {
    if ( !throttleSleep( userRecentlyActive() ? 200 : 2000, feedback ) )
      return false;
  }
  return !( feedback && feedback->isCanceled() );
}

void QgsAiIndexingThrottle::lowerCurrentThreadPriority()
{
#ifdef Q_OS_WIN
  SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_LOWEST );
#elif defined( Q_OS_MACOS )
  // Utility QoS: lower priority, and efficiency cores first on Apple silicon.
  pthread_set_qos_class_self_np( QOS_CLASS_UTILITY, 0 );
#elif defined( Q_OS_LINUX )
  setpriority( PRIO_PROCESS, static_cast<id_t>( syscall( SYS_gettid ) ), 10 );
#endif
}

bool QgsAiIndexingThrottle::inBackgroundIndexing()
{
  return tThrottleBackgroundIndexing;
}

QgsAiIndexingThrottle::BackgroundIndexingScope::BackgroundIndexingScope()
{
  tThrottleBackgroundIndexing = true;
#ifdef Q_OS_WIN
  mPreviousPriority = GetThreadPriority( GetCurrentThread() );
  mChanged = mPreviousPriority != THREAD_PRIORITY_ERROR_RETURN && SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_LOWEST );
#elif defined( Q_OS_MACOS )
  qos_class_t previous = QOS_CLASS_UNSPECIFIED;
  if ( pthread_get_qos_class_np( pthread_self(), &previous, &mPreviousRelative ) == 0 )
  {
    mPreviousPriority = static_cast<int>( previous );
    mChanged = pthread_set_qos_class_self_np( QOS_CLASS_UTILITY, 0 ) == 0;
  }
#endif
  // Linux: a thread cannot raise its priority back without privileges, so worker threads shared
  // with other tasks keep theirs; only the threads made for the model run lowered.
}

QgsAiIndexingThrottle::BackgroundIndexingScope::~BackgroundIndexingScope()
{
  tThrottleBackgroundIndexing = false;
  if ( !mChanged )
    return;
#ifdef Q_OS_WIN
  SetThreadPriority( GetCurrentThread(), mPreviousPriority );
#elif defined( Q_OS_MACOS )
  const qos_class_t previous = static_cast<qos_class_t>( mPreviousPriority );
  if ( previous == QOS_CLASS_UNSPECIFIED || pthread_set_qos_class_self_np( previous, mPreviousRelative ) != 0 )
    pthread_set_qos_class_self_np( QOS_CLASS_DEFAULT, 0 );
#endif
}
