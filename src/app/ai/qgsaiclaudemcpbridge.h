/***************************************************************************
    qgsaiclaudemcpbridge.h
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

#ifndef QGSAICLAUDEMCPBRIDGE_H
#define QGSAICLAUDEMCPBRIDGE_H
#include "qgis_app.h"
#include <functional>
#include <QJsonArray>
#include <QJsonObject>
#include <QTcpServer>
#include <QSet>
class QTcpSocket;

// A short-lived Streamable HTTP server. This is the only execution path for CLI tools.
class APP_EXPORT QgsAiClaudeMcpBridge : public QObject
{
    Q_OBJECT
  public:
    using Executor = std::function<QJsonObject( const QString &, const QJsonObject &, const QString & )>;
    explicit QgsAiClaudeMcpBridge( QObject *parent = nullptr );
    bool start( const QJsonArray &tools, Executor executor );
    void stop();
    QJsonObject configuration() const;
    static QJsonObject toolResult( const QString &text, bool error = false );
  signals:
    void executingChanged( bool executing );

  private:
    void acceptConnection();
    void handleRequest( QTcpSocket *socket, const QByteArray &request );
    QJsonObject dispatch( const QJsonObject &request );
    void respond( QTcpSocket *socket, int status, const QByteArray &body = QByteArray() );
    QTcpServer mServer;
    QSet<QTcpSocket *> mSockets;
    QByteArray mToken;
    QString mRunId;
    QJsonArray mTools;
    Executor mExecutor;
    QMap<QString, QJsonObject> mResponses;
    QMap<QString, QByteArray> mRequests;
    bool mExecuting = false;
};
#endif
