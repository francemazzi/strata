/***************************************************************************
    qgsaiindexingthrottle.h
    -----------------------
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

#ifndef QGSAIINDEXINGTHROTTLE_H
#define QGSAIINDEXINGTHROTTLE_H

#include "qgis_app.h"

#include <QString>

class QgsFeedback;

/**
 * How gently background indexing uses the computer: how many threads compute embeddings,
 * how long indexing pauses between batches, and when it waits for the user or for power.
 *
 * Indexing runs for minutes on a laptop; these rules keep the map and the rest of the
 * computer responsive meanwhile.
 */
class APP_EXPORT QgsAiIndexingThrottle
{
  public:
    //! The "Indexing speed" setting.
    enum class Speed
    {
      Low,    //!< One thread and long pauses: indexing barely shows.
      Normal, //!< Two threads and short pauses (default).
      High,   //!< Up to four threads, no pauses.
    };

    //! Indexing waits until the user has left the map alone this long.
    static constexpr int USER_QUIET_MS = 2000;

    //! Settings key of the speed: "low", "normal" or "high".
    static QString speedSettingsKey();
    //! Settings key of pausing on battery power (default on).
    static QString pauseOnBatterySettingsKey();

    //! The speed chosen in the settings.
    static Speed speed();

    //! Threads computing embeddings at \a speed, on a computer with \a idealThreads hardware threads.
    static int threadsForSpeed( Speed speed, int idealThreads );

    //! Pause after an embedding batch that took \a batchMs, so the average CPU use stays low.
    static int pauseAfterBatchMs( Speed speed, qint64 batchMs );

    //! Records that the user is working on the map. Call on the interface thread.
    static void noteUserActivity();

    //! True if the user worked on the map within USER_QUIET_MS.
    static bool userRecentlyActive();

    //! True while the computer runs on battery. Cached for a few seconds; safe on any thread.
    static bool onBatteryPower();

    /**
     * Called by indexing on a worker thread between two embedding batches: sleeps \a pauseMs,
     * then while the user is working on the map or, if the setting is on, while the computer
     * runs on battery. Returns false as soon as \a feedback is canceled.
     */
    static bool waitBeforeNextBatch( int pauseMs, QgsFeedback *feedback );

    //! Lowers the scheduling priority of the calling thread for good (threads made for the model).
    static void lowerCurrentThreadPriority();

    //! True on a thread inside a BackgroundIndexingScope: automatic indexing, which is throttled.
    static bool inBackgroundIndexing();

    /**
     * Marks the calling worker thread as running automatic indexing, and lowers its priority,
     * while it exists. Indexing asked for explicitly (a tool, "Rebuild now") is not throttled.
     */
    class APP_EXPORT BackgroundIndexingScope
    {
      public:
        BackgroundIndexingScope();
        ~BackgroundIndexingScope();

        BackgroundIndexingScope( const BackgroundIndexingScope & ) = delete;
        BackgroundIndexingScope &operator=( const BackgroundIndexingScope & ) = delete;

      private:
        int mPreviousPriority = 0;
        int mPreviousRelative = 0;
        bool mChanged = false;
    };
};

#endif // QGSAIINDEXINGTHROTTLE_H
