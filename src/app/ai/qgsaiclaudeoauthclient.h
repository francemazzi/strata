/***************************************************************************
    qgsaiclaudeoauthclient.h
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

#ifndef QGSAICLAUDEOAUTHCLIENT_H
#define QGSAICLAUDEOAUTHCLIENT_H

#include "qgis_app.h"

#include <QObject>
#include <QString>

class QTcpServer;
class QTimer;

/**
 * Browser login for a Claude subscription. Strata opens the authorize page and
 * receives the redirect on 127.0.0.1. No Claude Code process is started and
 * Claude Code's own credential file is never read.
 */
class APP_EXPORT QgsAiClaudeOAuthClient : public QObject
{
    Q_OBJECT

  public:
    struct AccessToken
    {
        QString token;
    };

    explicit QgsAiClaudeOAuthClient( QObject *parent = nullptr );

    static QString authorizeUrl( const QString &redirectUri, const QString &state, const QString &codeChallenge );
    static QString pkceChallenge( const QByteArray &verifier );
    static QByteArray generateVerifier();

    bool start( QString *errorMessage = nullptr );
    void cancel();
    QString currentAuthorizeUrl() const;
    QString redirectUri() const;

    static bool refreshAccessToken( AccessToken &token, QString *errorMessage = nullptr );
    static bool hasRefreshToken();
    static bool clearLogin( QString *errorMessage = nullptr );
    static QString refreshTokenSettingKey();
    static QString accessTokenSettingKey();
    static QString expiresAtSettingKey();
    static QString oauthBetaHeader();
    //! Points token exchange and refresh at a local server. Empty restores production.
    static void setTokenEndpointForTesting( const QString &url );

  signals:
    void loginSucceeded();
    void loginFailed( const QString &errorMessage );

  private:
    void fail( const QString &errorMessage );
    void handlePendingConnection();
    bool exchangeCode( const QString &code, QString *errorMessage );

    QTcpServer *mServer = nullptr;
    QTimer *mTimeout = nullptr;
    QByteArray mVerifier;
    QString mState;
    QString mRedirectUri;
    bool mFinished = false;
};

#endif // QGSAICLAUDEOAUTHCLIENT_H
