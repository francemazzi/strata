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
#include "ai/qgsaiclaudeoautherror.h"
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
#include <QSharedPointer>
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
      QCOMPARE( reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt(), 400 );
      const QByteArray body = reply->readAll();
      QVERIFY( body.contains( "Strata" ) );
      QVERIFY( body.contains( "Connection not verified" ) );
      QVERIFY( !body.contains( "state=wrong" ) );
      reply->deleteLater();
      QVERIFY( !QgsAiSecretStore::hasSecret( QgsAiClaudeOAuthClient::refreshTokenSettingKey() ) );
    }

    void callbackStoresTokensFromLocalEndpoint()
    {
      QgsAiTestLoopbackServer tokenServer;
      QVERIFY( tokenServer.listen( QHostAddress::LocalHost, 0 ) );
      const auto closeServer = qScopeGuard( [&tokenServer]() { tokenServer.close(); } );
      tokenServer.responses = { QgsAiTestLoopbackServer::jsonResponse(
        200,
        "OK",
        QJsonDocument(
          QJsonObject {
            { u"access_token"_s, u"access-from-callback"_s },
            { u"refresh_token"_s, u"refresh-from-callback"_s },
            { u"expires_in"_s, 3600 },
          }
        )
          .toJson( QJsonDocument::Compact )
      ) };
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
      QSignalSpy finished( reply, &QNetworkReply::finished );
      QVERIFY( succeeded.wait( 5000 ) );
      QCOMPARE( failed.count(), 0 );
      QVERIFY( finished.wait( 5000 ) || finished.count() == 1 );
      const QByteArray callbackBody = reply->readAll();
      QVERIFY( callbackBody.contains( "Claude is connected" ) );
      QVERIFY( callbackBody.contains( "setTimeout" ) );
      QVERIFY( !callbackBody.contains( "good-code" ) );
      QVERIFY( !callbackBody.contains( state.toUtf8() ) );
      QCOMPARE( reply->rawHeader( "Cache-Control" ), QByteArray( "no-store, max-age=0" ) );
      QVERIFY( reply->rawHeader( "Content-Security-Policy" ).contains( "default-src 'none'" ) );
      reply->deleteLater();
      QCOMPARE( QgsAiSecretStore::readSecret( QgsAiClaudeOAuthClient::accessTokenSettingKey() ), u"access-from-callback"_s );
      QCOMPARE( QgsAiSecretStore::readSecret( QgsAiClaudeOAuthClient::refreshTokenSettingKey() ), u"refresh-from-callback"_s );
      QVERIFY( !QgsAiSecretStore::readSecret( QgsAiClaudeOAuthClient::expiresAtSettingKey() ).isEmpty() );
      const QByteArray raw = tokenServer.lastRawRequest();
      QVERIFY2( raw.contains( "claude-cli/" ), raw.constData() );
      QVERIFY( tokenServer.lastRequestBody().contains( "good-code" ) );
    }

    void successPageWaitsForTokenExchange()
    {
      QgsAiTestLoopbackServer tokenServer;
      QVERIFY( tokenServer.listen( QHostAddress::LocalHost, 0 ) );
      auto response = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":3600}" ) );
      response.responseDelayMs = 180;
      tokenServer.responses = { response };
      QgsAiClaudeOAuthClient::setTokenEndpointForTesting( u"http://127.0.0.1:%1/"_s.arg( tokenServer.serverPort() ) );

      QgsAiClaudeOAuthClient client;
      QVERIFY( client.start() );
      const QString state = QUrlQuery( QUrl( client.currentAuthorizeUrl() ) ).queryItemValue( u"state"_s );
      QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( QNetworkRequest( QUrl( client.redirectUri() + u"?code=delayed&state="_s + state ) ) );
      QSignalSpy finished( reply, &QNetworkReply::finished );
      QTest::qWait( 60 );
      QCOMPARE( finished.count(), 0 );
      QVERIFY( finished.wait( 5000 ) );
      QVERIFY( reply->readAll().contains( "Claude is connected" ) );
      reply->deleteLater();
    }

    void rateLimitIsStructuredAndNeverLooksSuccessful()
    {
      QgsAiTestLoopbackServer tokenServer;
      QVERIFY( tokenServer.listen( QHostAddress::LocalHost, 0 ) );
      tokenServer.responses = {
        QgsAiTestLoopbackServer::
          jsonResponse( 429, "Too Many Requests", QByteArrayLiteral( "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"Rate limited <retry>\"}}" ), { { "Retry-After", "37" } } )
      };
      QgsAiClaudeOAuthClient::setTokenEndpointForTesting( u"http://127.0.0.1:%1/"_s.arg( tokenServer.serverPort() ) );

      QgsAiClaudeOAuthClient client;
      QVERIFY( client.start() );
      const QString state = QUrlQuery( QUrl( client.currentAuthorizeUrl() ) ).queryItemValue( u"state"_s );
      QSignalSpy failed( &client, &QgsAiClaudeOAuthClient::loginFailed );
      QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( QNetworkRequest( QUrl( client.redirectUri() + u"?code=limited-code&state="_s + state ) ) );
      QSignalSpy finished( reply, &QNetworkReply::finished );
      QVERIFY( failed.wait( 5000 ) );
      QVERIFY( finished.wait( 5000 ) || finished.count() == 1 );

      const QgsAiClaudeOAuthError error = failed.takeFirst().at( 0 ).value<QgsAiClaudeOAuthError>();
      QCOMPARE( error.category, QgsAiClaudeOAuthError::Category::RateLimited );
      QCOMPARE( error.providerCode, u"rate_limit_error"_s );
      QCOMPARE( error.retryAfterSeconds, 37 );
      QCOMPARE( reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt(), 429 );
      const QByteArray body = reply->readAll();
      QVERIFY( body.contains( "Try again later" ) );
      QVERIFY( body.contains( "Rate limited &lt;retry&gt;" ) );
      QVERIFY( !body.contains( "Claude is connected" ) );
      QVERIFY( !body.contains( "limited-code" ) );
      QVERIFY( !body.contains( state.toUtf8() ) );
      QVERIFY( !QgsAiClaudeOAuthClient::hasRefreshToken() );
      QVERIFY( !QgsAiSecretStore::hasSecret( QgsAiClaudeOAuthClient::accessTokenSettingKey() ) );
      reply->deleteLater();
    }

    void deniedAndMalformedCallbacksUseBrandedPages()
    {
      QgsAiClaudeOAuthClient deniedClient;
      QVERIFY( deniedClient.start() );
      const QString state = QUrlQuery( QUrl( deniedClient.currentAuthorizeUrl() ) ).queryItemValue( u"state"_s );
      QSignalSpy deniedFailure( &deniedClient, &QgsAiClaudeOAuthClient::loginFailed );
      const QUrl deniedUrl( deniedClient.redirectUri() + u"?error=access_denied&error_description=%3Cscript%3Ebad%3C/script%3E&state="_s + state );
      QNetworkReply *deniedReply = QgsNetworkAccessManager::instance()->get( QNetworkRequest( deniedUrl ) );
      QSignalSpy deniedFinished( deniedReply, &QNetworkReply::finished );
      QVERIFY( deniedFailure.wait( 5000 ) );
      QVERIFY( deniedFinished.wait( 5000 ) || deniedFinished.count() == 1 );
      const QByteArray deniedBody = deniedReply->readAll();
      QVERIFY( deniedBody.contains( "Connection cancelled" ) );
      QVERIFY( deniedBody.contains( "&lt;script&gt;bad&lt;/script&gt;" ) );
      QVERIFY( !deniedBody.contains( "<script>bad</script>" ) );
      QVERIFY( !deniedBody.contains( "setTimeout" ) );
      deniedReply->deleteLater();

      QgsAiClaudeOAuthClient malformedClient;
      QVERIFY( malformedClient.start() );
      QSignalSpy malformedFailure( &malformedClient, &QgsAiClaudeOAuthClient::loginFailed );
      QNetworkReply *malformedReply = QgsNetworkAccessManager::instance()->post( QNetworkRequest( QUrl( malformedClient.redirectUri() ) ), QByteArray() );
      QSignalSpy malformedFinished( malformedReply, &QNetworkReply::finished );
      QVERIFY( malformedFailure.wait( 5000 ) );
      QVERIFY( malformedFinished.wait( 5000 ) || malformedFinished.count() == 1 );
      QVERIFY( malformedReply->readAll().contains( "Connection not completed" ) );
      malformedReply->deleteLater();
    }

    void timeoutHasTypedFailure()
    {
      QgsAiClaudeOAuthClient client;
      client.setLoginTimeoutForTesting( 20 );
      QSignalSpy failed( &client, &QgsAiClaudeOAuthClient::loginFailed );
      QVERIFY( client.start() );
      QVERIFY( failed.wait( 1000 ) );
      const QgsAiClaudeOAuthError error = failed.takeFirst().at( 0 ).value<QgsAiClaudeOAuthError>();
      QCOMPARE( error.category, QgsAiClaudeOAuthError::Category::Timeout );
    }

    void incompleteTokenResponseLeavesNoCredential()
    {
      QgsAiTestLoopbackServer tokenServer;
      QVERIFY( tokenServer.listen( QHostAddress::LocalHost, 0 ) );
      tokenServer.responses = { QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"access_token\":\"partial\"}" ) ) };
      QgsAiClaudeOAuthClient::setTokenEndpointForTesting( u"http://127.0.0.1:%1/"_s.arg( tokenServer.serverPort() ) );
      QgsAiClaudeOAuthClient client;
      QVERIFY( client.start() );
      const QString state = QUrlQuery( QUrl( client.currentAuthorizeUrl() ) ).queryItemValue( u"state"_s );
      QSignalSpy failed( &client, &QgsAiClaudeOAuthClient::loginFailed );
      QgsNetworkAccessManager::instance()->get( QNetworkRequest( QUrl( client.redirectUri() + u"?code=incomplete&state="_s + state ) ) );
      QVERIFY( failed.wait( 5000 ) );
      const QgsAiClaudeOAuthError error = failed.takeFirst().at( 0 ).value<QgsAiClaudeOAuthError>();
      QCOMPARE( error.category, QgsAiClaudeOAuthError::Category::InvalidResponse );
      QVERIFY( !QgsAiClaudeOAuthClient::hasRefreshToken() );
      QVERIFY( !QgsAiSecretStore::hasSecret( QgsAiClaudeOAuthClient::accessTokenSettingKey() ) );
    }

    void credentialStorageFailureRollsBackPartialLogin()
    {
      auto values = std::make_shared<QHash<QString, QString>>();
      auto writeCount = std::make_shared<int>( 0 );
      QgsAiSecretStore::setBackendForTesting( [values, writeCount]( QgsAiSecretStore::Operation operation, const QString &key, const QString &value, QgsAiSecretStore::BackendCallback done ) {
        QTimer::singleShot( 0, qApp, [values, writeCount, operation, key, value, done]() {
          if ( operation == QgsAiSecretStore::Operation::Write )
          {
            ++( *writeCount );
            if ( *writeCount == 2 )
            {
              done( {} );
              return;
            }
            values->insert( key, value );
          }
          else if ( operation == QgsAiSecretStore::Operation::Remove )
          {
            values->remove( key );
          }
          const bool found = values->contains( key );
          done( { operation != QgsAiSecretStore::Operation::Read || found, operation == QgsAiSecretStore::Operation::Read && !found, values->value( key ) } );
        } );
      } );
      QgsAiSecretStore::resetCredentialCacheForTesting();

      QgsAiTestLoopbackServer tokenServer;
      QVERIFY( tokenServer.listen( QHostAddress::LocalHost, 0 ) );
      tokenServer.responses = { QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":3600}" ) ) };
      QgsAiClaudeOAuthClient::setTokenEndpointForTesting( u"http://127.0.0.1:%1/"_s.arg( tokenServer.serverPort() ) );

      QgsAiClaudeOAuthClient client;
      QVERIFY( client.start() );
      const QString state = QUrlQuery( QUrl( client.currentAuthorizeUrl() ) ).queryItemValue( u"state"_s );
      QSignalSpy failed( &client, &QgsAiClaudeOAuthClient::loginFailed );
      QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( QNetworkRequest( QUrl( client.redirectUri() + u"?code=storage-failure&state="_s + state ) ) );
      QVERIFY( failed.wait( 5000 ) );
      const QgsAiClaudeOAuthError error = failed.takeFirst().at( 0 ).value<QgsAiClaudeOAuthError>();
      QCOMPARE( error.category, QgsAiClaudeOAuthError::Category::CredentialStorage );
      QVERIFY( !QgsAiClaudeOAuthClient::hasRefreshToken() );
      QVERIFY( !QgsAiSecretStore::hasSecret( QgsAiClaudeOAuthClient::accessTokenSettingKey() ) );
      QVERIFY( !QgsAiSecretStore::hasSecret( QgsAiClaudeOAuthClient::expiresAtSettingKey() ) );
      QTRY_VERIFY( !values->contains( QgsAiSecretStore::keychainKey( QgsAiClaudeOAuthClient::accessTokenSettingKey() ) ) );
      QTRY_VERIFY( !values->contains( QgsAiSecretStore::keychainKey( QgsAiClaudeOAuthClient::refreshTokenSettingKey() ) ) );
      QTRY_VERIFY( !values->contains( QgsAiSecretStore::keychainKey( QgsAiClaudeOAuthClient::expiresAtSettingKey() ) ) );
      reply->deleteLater();
    }
};

QGSTEST_MAIN( TestQgsAiClaudeOAuthClient )
#include "testqgsaiclaudeoauthclient.moc"
