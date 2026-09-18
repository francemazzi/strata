/***************************************************************************
    qgsaiptysession.h
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

#ifndef QGSAIPTYSESSION_H
#define QGSAIPTYSESSION_H

#include "qgis_app.h"

#include <QByteArray>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

class QSocketNotifier;
class QTimer;

/**
 * Minimal pseudo-terminal wrapper: runs a program that insists on a TTY
 * (interactive CLIs such as Claude Code) with its stdin/stdout attached to a
 * pty master owned by the caller.
 *
 * POSIX only (posix_openpt + fork). On Windows start() fails and the caller
 * must fall back to a visible console. After the fork the child only performs
 * async-signal-safe calls before execve(), so it is safe inside a multithreaded
 * Qt/Cocoa process.
 *
 * The pty window size is configurable so that terminal UIs that hard-wrap at
 * the terminal width (Ink) keep long tokens and URLs on a single line.
 */
class APP_EXPORT QgsAiPtySession : public QObject
{
    Q_OBJECT

  public:
    explicit QgsAiPtySession( QObject *parent = nullptr );
    ~QgsAiPtySession() override;

    //! True when this platform can run pty sessions (POSIX).
    static bool isSupported();

    /**
     * Spawns \a program with \a arguments inside a fresh pty of \a columns × \a rows
     * cells. \a program must be an absolute path (no PATH lookup happens in the child).
     * Returns false with \a errorMessage set when the session cannot be started.
     */
    bool start( const QString &program, const QStringList &arguments, const QProcessEnvironment &environment, int columns = 1000, int rows = 50, QString *errorMessage = nullptr );

    bool isRunning() const;
    qint64 processId() const { return mPid; }

    //! Writes raw bytes to the child's terminal input. Returns the number of bytes written.
    qint64 write( const QByteArray &data );
    //! Sends Ctrl+C to the child terminal.
    void sendInterrupt();
    //! Polite stop: Ctrl+C, then SIGTERM, then SIGKILL.
    void terminate();
    //! Immediate SIGKILL.
    void kill();

  signals:
    //! Raw terminal output (includes ANSI escape sequences).
    void outputReceived( const QByteArray &chunk );
    //! The child exited. \a crashed is true when it was killed by a signal.
    void finished( int exitCode, bool crashed );

  private:
    void onReadyRead();
    void drainOutput();
    void pollChildExit();
    void handleExit( int status );
    void closeMaster();
    void signalChild( int signal );

    int mMasterFd = -1;
    qint64 mPid = -1;
    QSocketNotifier *mNotifier = nullptr;
    QTimer *mExitPollTimer = nullptr;
    bool mSlaveClosed = false;
    int mSlaveClosedPolls = 0;
    bool mFinishedEmitted = false;
};

#endif // QGSAIPTYSESSION_H
