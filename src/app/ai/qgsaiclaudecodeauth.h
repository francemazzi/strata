/***************************************************************************
    qgsaiclaudecodeauth.h
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

#ifndef QGSAICLAUDECODEAUTH_H
#define QGSAICLAUDECODEAUTH_H
#include "qgis_app.h"
#include <QObject>
#include <QProcess>
#include <QTimer>

// Only the unmodified official CLI handles subscription credentials.
class APP_EXPORT QgsAiClaudeCodeAuth : public QObject
{
    Q_OBJECT
  public:
    explicit QgsAiClaudeCodeAuth( QObject *parent = nullptr );
    ~QgsAiClaudeCodeAuth() override;
    static QString executablePath();
    static QString configDirectory();
    static QProcessEnvironment environment();
    static bool isConfigured();
    static int cooldownSeconds();
    static void setCooldown( int seconds );
    void checkStatus();
    void login();
    void logout();
    void cancel();
    bool isBusy() const { return mProcess.state() != QProcess::NotRunning; }
  signals:
    void statusChanged( bool connected, const QString &message );

  private:
    void begin( const QString &operation );
    void run( const QStringList &arguments );
    void finished( int exitCode );
    void fail( const QString &message );
    QProcess mProcess;
    QTimer mTimer;
    QByteArray mOutput;
    QString mOperation;
    bool mCheckingVersion = false;
    bool mRateLimited = false;
};
#endif
