/***************************************************************************
    qgsaiclaudecoderuntime.h
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

#ifndef QGSAICLAUDECODERUNTIME_H
#define QGSAICLAUDECODERUNTIME_H
#include "qgis_app.h"
#include "qgsaiclaudemcpbridge.h"
#include "qgsaiclaudecodeauth.h"
#include "qgsaimodels.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

class APP_EXPORT QgsAiClaudeCodeRuntime : public QObject
{
    Q_OBJECT
  public:
    explicit QgsAiClaudeCodeRuntime( QObject *parent = nullptr );
    ~QgsAiClaudeCodeRuntime() override;
    void start( const QJsonObject &payload, QgsAiClaudeMcpBridge::Executor executor );
    void cancel();
    static QByteArray inputMessage( const QJsonArray &messages );
    static QString errorMessage( const QString &category );
  signals:
    void progress( const QString &text );
    void usageReported( const QString &model, const QgsAiUsage &usage );
    void finished( bool success, const QString &text, const QString &error, int status );

  private:
    void launch();
    void readOutput();
    void consumeEvent( const QJsonObject &object );
    void complete( bool success, const QString &text, const QString &error, int status );
    QProcess mProcess;
    QgsAiClaudeCodeAuth mAuth;
    QgsAiClaudeMcpBridge mBridge;
    QTemporaryDir mDirectory;
    QTimer mWatchdog;
    QJsonObject mPayload;
    QByteArray mBuffer;
    QString mFinalText;
    QString mError;
    QString mErrorCategory;
    QString mModel;
    QgsAiUsage mUsage;
    int mRetryAfter = 60;
    bool mDone = true;
    bool mGotResult = false;
    bool mSuccessful = false;
};
#endif
