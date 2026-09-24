/***************************************************************************
    testqgsaiclaudeoauthclient.cpp
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

#include "ai/qgsaiclaudeoauthclient.h"
#include "ai/qgsaisecretstore.h"
#include "qgsaisecretstoretestutils.h"
#include "qgsaitestloopbackserver.h"
#include "qgsnetworkaccessmanager.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

class TestQgsAiClaudeOAuthClient : public QObject
{
    Q_OBJECT
  private slots:
    void init()
    {
      installTestSecretBackend();
      QgsAiSecretStore::resetCredentialCacheForTesting();
      QgsAiClaudeOAuthClient::clearLogin();
      QgsSettings settings;
      settings.remove( QgsAiClaudeOAuthClient::refreshTokenSettingKey() + u"_inKeychain"_s );
      settings.remove( QgsAiClaudeOAuthClient::accessTokenSettingKey() + u"_inKeychain"_s );
      settings.remove( QgsAiClaudeOAuthClient::expiresAtSettingKey() + u"_inKeychain"_s );
      QgsAiClaudeOAuthClient::setTokenEndpointForTesting( QString() );
    }
    void cleanup() { QgsAiClaudeOAuthClient::setTokenEndpointForTesting( QString() ); }

    void authorizeUrlCarriesPkceAndLocalRedirect()
    {
      const QByteArray verifier = QgsAiClaudeOAuthClient::generateVerifier();
      const QString challenge = QgsAiClaudeOAuthClient::pkceChallenge( verifier );
      const QString expected = QString::fromLatin1( QCryptographicHash::hash( verifier, QCryptographicHash::Sha256 ).toBase64( QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals ) );
      QCOMPARE( challenge, expected );
      const QString urlText = QgsAiClaudeOAuthClient::authorizeUrl( u"http://127.0.0.1:9/callback"_s, u"state-1"_s, challenge );
      const QUrlQuery query { QUrl( urlText ) };
      QVERIFY( urlText.startsWith( u"https://claude.com/cai/oauth/authorize?"_s ) );
      QCOMPARE( query.queryItemValue( u"redirect_uri"_s ), u"http://127.0.0.1:9/callback"_s );
      QCOMPARE( query.queryItemValue( u"state"_s ), u"state-1"_s );
      QCOMPARE( query.queryItemValue( u"code_challenge"_s ), challenge );
      QCOMPARE( query.queryItemValue( u"code_challenge_method"_s ), u"S256"_s );
      QCOMPARE( query.queryItemValue( u"client_id"_s ), u"9d1c250a-e61b-44d9-88ed-5944d1962f5e"_s );
    }

    void mismatchedStateIsRejected()
    {
      QgsAiClaudeOAuthClient client;
      QString error;
      QVERIFY( client.start( &error ) );
      QSignalSpy failed( &client, &QgsAiClaudeOAuthClient::loginFailed );
      QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( QNetworkRequest( QUrl( client.redirectUri() + u"?code=abc&state=wrong"_s ) ) );
      QSignalSpy finished( reply, &QNetworkReply::finished );
      QVERIFY( failed.wait( 5000 ) );
      QVERIFY( finished.wait( 5000 ) || finished.count() == 1 );
      reply->deleteLater();
      QVERIFY( !QgsAiSecretStore::hasSecret( QgsAiClaudeOAuthClient::refreshTokenSettingKey() ) );
    }

    void callbackStoresTokensFromLocalEndpoint()
    {
      QgsAiTestLoopbackServer tokenServer;
      QVERIFY( tokenServer.listen( QHostAddress::LocalHost, 0 ) );
      const auto closeServer = qScopeGuard( [&tokenServer]() { tokenServer.close(); } );
      tokenServer.responses = { QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QJsonDocument( QJsonObject {
                                                                                                   { u"access_token"_s, u"access-from-callback"_s },
                                                                                                   { u"refresh_token"_s, u"refresh-from-callback"_s },
                                                                                                   { u"expires_in"_s, 3600 },
                                                                                                 } )
                                                                          .toJson( QJsonDocument::Compact ) ) };
      QgsAiClaudeOAuthClient::setTokenEndpointForTesting( u"http://127.0.0.1:%1/"_s.arg( tokenServer.serverPort() ) );

      QgsAiClaudeOAuthClient client;
      QString error;
      QVERIFY( client.start( &error ) );
      const QUrlQuery authorize { QUrl( client.currentAuthorizeUrl() ) };
      const QString state = authorize.queryItemValue( u"state"_s );
      QVERIFY( !state.isEmpty() );
      QSignalSpy succeeded( &client, &QgsAiClaudeOAuthClient::loginSucceeded );
      QSignalSpy failed( &client, &QgsAiClaudeOAuthClient::loginFailed );
      QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( QNetworkRequest( QUrl( client.redirectUri() + u"?code=good-code&state="_s + state ) ) );
      QVERIFY( succeeded.wait( 5000 ) );
      QCOMPARE( failed.count(), 0 );
      reply->deleteLater();
      QCOMPARE( QgsAiSecretStore::readSecret( QgsAiClaudeOAuthClient::accessTokenSettingKey() ), u"access-from-callback"_s );
      QCOMPARE( QgsAiSecretStore::readSecret( QgsAiClaudeOAuthClient::refreshTokenSettingKey() ), u"refresh-from-callback"_s );
      QVERIFY( !QgsAiSecretStore::readSecret( QgsAiClaudeOAuthClient::expiresAtSettingKey() ).isEmpty() );
      const QByteArray raw = tokenServer.lastRawRequest();
      QVERIFY2( raw.contains( "claude-cli/" ), raw.constData() );
      QVERIFY( tokenServer.lastRequestBody().contains( "good-code" ) );
    }
};

QGSTEST_MAIN( TestQgsAiClaudeOAuthClient )
#include "testqgsaiclaudeoauthclient.moc"
