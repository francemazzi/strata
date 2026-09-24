/***************************************************************************
    qgsaiclaudeoauthclient.cpp
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

#include "qgsaiclaudeoauthclient.h"

#include "qgsaisecretstore.h"
#include "qgsnetworkaccessmanager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

using namespace Qt::StringLiterals;

namespace
{
  constexpr const char *CLAUDE_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
  constexpr const char *CLAUDE_AUTHORIZE_URL = "https://claude.com/cai/oauth/authorize";
  constexpr const char *CLAUDE_TOKEN_URL = "https://platform.claude.com/v1/oauth/token";
  constexpr const char *CLAUDE_SCOPE = "user:profile user:inference";
  constexpr const char *CLAUDE_USER_AGENT = "claude-cli/2.1.274 (external, cli)";
  constexpr qint64 EXPIRY_BUFFER_MS = 5LL * 60LL * 1000LL;

  QString tokenEndpointOverride;

  QString base64Url( const QByteArray &bytes )
  {
    return QString::fromLatin1( bytes.toBase64( QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals ) );
  }

  QString tokenEndpoint()
  {
    return tokenEndpointOverride.isEmpty() ? QString::fromUtf8( CLAUDE_TOKEN_URL ) : tokenEndpointOverride;
  }

  bool storeLoginSecret( const QString &key, const QString &value, QString *errorMessage )
  {
    if ( QgsAiSecretStore::storageState( key ) == QgsAiSecretStore::StorageState::SessionOnly )
    {
      QgsAiSecretStore::useForSession( key, value );
      return true;
    }
    if ( QgsAiSecretStore::writeSecret( key, value ) )
      return true;
    if ( errorMessage )
      *errorMessage = QObject::tr( "Claude credentials could not be saved securely." );
    return false;
  }

  QJsonObject postJsonBlocking( const QUrl &url, const QJsonObject &payload, int timeoutMs, int &httpStatus, QString *errorMessage )
  {
    httpStatus = 0;
    QgsNetworkAccessManager *nam = QgsNetworkAccessManager::instance();
    if ( !nam )
    {
      if ( errorMessage )
        *errorMessage = u"Network manager is not available."_s;
      return {};
    }

    QNetworkRequest request( url );
    request.setHeader( QNetworkRequest::ContentTypeHeader, u"application/json"_s );
    request.setAttribute( static_cast<QNetworkRequest::Attribute>( QgsNetworkRequestParameters::AttributeUserAgentSuffix ), QString::fromUtf8( CLAUDE_USER_AGENT ) );
    request.setTransferTimeout( timeoutMs );

    QNetworkReply *reply = nam->post( request, QJsonDocument( payload ).toJson( QJsonDocument::Compact ) );
    if ( !reply )
    {
      if ( errorMessage )
        *errorMessage = u"Unable to start the Claude token request."_s;
      return {};
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot( true );
    QObject::connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
    QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
    timer.start( timeoutMs );
    loop.exec();

    if ( timer.isActive() )
      timer.stop();
    else
      reply->abort();

    httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson( body );
    const QJsonObject object = doc.isObject() ? doc.object() : QJsonObject();
    if ( networkError != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
    {
      if ( errorMessage )
      {
        QString detail = object.value( u"error_description"_s ).toString();
        if ( detail.isEmpty() )
          detail = object.value( u"error"_s ).toString();
        if ( detail.isEmpty() )
          detail = QString::fromUtf8( body.left( 300 ) );
        if ( detail.isEmpty() )
          detail = u"Claude token request failed."_s;
        *errorMessage = u"Claude token request failed (HTTP %1): %2"_s.arg( httpStatus ).arg( detail );
      }
      return {};
    }
    return object;
  }

  void respondHtml( QTcpSocket *socket, int status, const QByteArray &body )
  {
    const QByteArray header = QByteArrayLiteral( "HTTP/1.1 " ) + QByteArray::number( status ) + QByteArrayLiteral( " \r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " ) + QByteArray::number( body.size() ) + QByteArrayLiteral( "\r\n\r\n" );
    socket->write( header );
    socket->write( body );
    socket->disconnectFromHost();
  }
} // namespace

QgsAiClaudeOAuthClient::QgsAiClaudeOAuthClient( QObject *parent )
  : QObject( parent )
{
}

QString QgsAiClaudeOAuthClient::authorizeUrl( const QString &redirectUri, const QString &state, const QString &codeChallenge )
{
  QUrlQuery query;
  query.addQueryItem( u"code"_s, u"true"_s );
  query.addQueryItem( u"client_id"_s, QString::fromUtf8( CLAUDE_CLIENT_ID ) );
  query.addQueryItem( u"response_type"_s, u"code"_s );
  query.addQueryItem( u"redirect_uri"_s, redirectUri );
  query.addQueryItem( u"scope"_s, QString::fromUtf8( CLAUDE_SCOPE ) );
  query.addQueryItem( u"code_challenge"_s, codeChallenge );
  query.addQueryItem( u"code_challenge_method"_s, u"S256"_s );
  query.addQueryItem( u"state"_s, state );
  QUrl url( QString::fromUtf8( CLAUDE_AUTHORIZE_URL ) );
  url.setQuery( query );
  return url.toString( QUrl::FullyEncoded );
}

QString QgsAiClaudeOAuthClient::pkceChallenge( const QByteArray &verifier )
{
  return base64Url( QCryptographicHash::hash( verifier, QCryptographicHash::Sha256 ) );
}

QByteArray QgsAiClaudeOAuthClient::generateVerifier()
{
  QByteArray bytes( 32, '\0' );
  for ( int i = 0; i < bytes.size(); ++i )
    bytes[i] = static_cast<char>( QRandomGenerator::system()->generate() & 0xff );
  return base64Url( bytes ).toLatin1();
}

bool QgsAiClaudeOAuthClient::start( QString *errorMessage )
{
  cancel();
  mFinished = false;
  mVerifier = generateVerifier();
  mState = base64Url( generateVerifier() );

  mServer = new QTcpServer( this );
  if ( !mServer->listen( QHostAddress::LocalHost, 0 ) )
  {
    const QString error = tr( "Unable to listen for the Claude login on this computer." );
    if ( errorMessage )
      *errorMessage = error;
    cancel();
    return false;
  }

  mRedirectUri = u"http://127.0.0.1:%1/callback"_s.arg( mServer->serverPort() );
  connect( mServer, &QTcpServer::newConnection, this, &QgsAiClaudeOAuthClient::handlePendingConnection );

  mTimeout = new QTimer( this );
  mTimeout->setSingleShot( true );
  connect( mTimeout, &QTimer::timeout, this, [this]() { fail( tr( "Claude login timed out. Try Connect Claude again." ) ); } );
  mTimeout->start( 5 * 60 * 1000 );
  return true;
}

void QgsAiClaudeOAuthClient::cancel()
{
  mFinished = true;
  if ( mTimeout )
  {
    mTimeout->stop();
    mTimeout->deleteLater();
    mTimeout = nullptr;
  }
  if ( mServer )
  {
    mServer->close();
    mServer->deleteLater();
    mServer = nullptr;
  }
  mVerifier.clear();
  mState.clear();
  mRedirectUri.clear();
}

QString QgsAiClaudeOAuthClient::currentAuthorizeUrl() const
{
  if ( mRedirectUri.isEmpty() || mState.isEmpty() || mVerifier.isEmpty() )
    return {};
  return authorizeUrl( mRedirectUri, mState, pkceChallenge( mVerifier ) );
}

QString QgsAiClaudeOAuthClient::redirectUri() const
{
  return mRedirectUri;
}

void QgsAiClaudeOAuthClient::fail( const QString &errorMessage )
{
  if ( mFinished && !mServer )
    return;
  cancel();
  emit loginFailed( errorMessage );
}

void QgsAiClaudeOAuthClient::handlePendingConnection()
{
  if ( !mServer || mFinished )
    return;
  QTcpSocket *socket = mServer->nextPendingConnection();
  if ( !socket )
    return;
  mServer->close();

  const auto readCallback = [this, socket]() {
    if ( mFinished )
      return;
    if ( !socket->canReadLine() && !socket->bytesAvailable() )
      return;
    const QByteArray request = socket->readAll();
    const int lineEnd = request.indexOf( "\r\n" );
    const QByteArray requestLine = lineEnd < 0 ? request : request.left( lineEnd );
    const QList<QByteArray> parts = requestLine.split( ' ' );
    if ( parts.size() < 2 || parts.at( 0 ) != "GET" )
    {
      respondHtml( socket, 400, QByteArrayLiteral( "<p>Claude login failed.</p>" ) );
      fail( tr( "Claude login received an unexpected response." ) );
      return;
    }

    const QUrl url( QString::fromUtf8( QByteArrayLiteral( "http://127.0.0.1" ) + parts.at( 1 ) ) );
    const QUrlQuery query( url );
    if ( query.queryItemValue( u"state"_s ) != mState )
    {
      respondHtml( socket, 400, QByteArrayLiteral( "<p>Claude login was rejected.</p>" ) );
      fail( tr( "Claude login state did not match. Try Connect Claude again." ) );
      return;
    }
    const QString code = query.queryItemValue( u"code"_s );
    if ( code.isEmpty() )
    {
      const QString detail = query.queryItemValue( u"error_description"_s, QUrl::FullyDecoded );
      respondHtml( socket, 400, QByteArrayLiteral( "<p>Claude login was not approved.</p>" ) );
      fail( detail.isEmpty() ? tr( "Claude login was not approved." ) : detail );
      return;
    }

    respondHtml( socket, 200, QByteArrayLiteral( "<p>Claude is connected. You can close this window and return to Strata.</p>" ) );
    QString error;
    if ( !exchangeCode( code, &error ) )
    {
      fail( error );
      return;
    }
    cancel();
    emit loginSucceeded();
  };
  connect( socket, &QTcpSocket::readyRead, this, readCallback );
  socket->setParent( this );
  if ( socket->bytesAvailable() > 0 )
    readCallback();
}

bool QgsAiClaudeOAuthClient::exchangeCode( const QString &code, QString *errorMessage )
{
  QJsonObject payload;
  payload.insert( u"grant_type"_s, u"authorization_code"_s );
  payload.insert( u"code"_s, code );
  payload.insert( u"redirect_uri"_s, mRedirectUri );
  payload.insert( u"client_id"_s, QString::fromUtf8( CLAUDE_CLIENT_ID ) );
  payload.insert( u"code_verifier"_s, QString::fromLatin1( mVerifier ) );
  payload.insert( u"state"_s, mState );

  int httpStatus = 0;
  const QJsonObject tokenObject = postJsonBlocking( QUrl( tokenEndpoint() ), payload, 30000, httpStatus, errorMessage );
  if ( tokenObject.isEmpty() )
    return false;

  const QString accessToken = tokenObject.value( u"access_token"_s ).toString();
  const QString refreshToken = tokenObject.value( u"refresh_token"_s ).toString();
  if ( accessToken.isEmpty() || refreshToken.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"Claude token response did not include an access token and a refresh token."_s;
    return false;
  }

  const qint64 expiresIn = static_cast<qint64>( tokenObject.value( u"expires_in"_s ).toDouble( 3600 ) );
  const QString expiresAt = QString::number( QDateTime::currentMSecsSinceEpoch() + expiresIn * 1000 );
  return storeLoginSecret( accessTokenSettingKey(), accessToken, errorMessage )
         && storeLoginSecret( refreshTokenSettingKey(), refreshToken, errorMessage )
         && storeLoginSecret( expiresAtSettingKey(), expiresAt, errorMessage );
}

bool QgsAiClaudeOAuthClient::refreshAccessToken( AccessToken &token, QString *errorMessage )
{
  const QString refreshToken = QgsAiSecretStore::readSecret( refreshTokenSettingKey() );
  const QString accessToken = QgsAiSecretStore::readSecret( accessTokenSettingKey() );
  bool expiryOk = false;
  const qint64 expiresAt = QgsAiSecretStore::readSecret( expiresAtSettingKey() ).toLongLong( &expiryOk );
  if ( !accessToken.isEmpty() && expiryOk && expiresAt > QDateTime::currentMSecsSinceEpoch() + EXPIRY_BUFFER_MS )
  {
    token.token = accessToken;
    return true;
  }
  if ( refreshToken.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"Missing Claude refresh token. Connect Claude in provider settings."_s;
    return false;
  }

  QJsonObject payload;
  payload.insert( u"grant_type"_s, u"refresh_token"_s );
  payload.insert( u"refresh_token"_s, refreshToken );
  payload.insert( u"client_id"_s, QString::fromUtf8( CLAUDE_CLIENT_ID ) );
  payload.insert( u"scope"_s, QString::fromUtf8( CLAUDE_SCOPE ) );

  int httpStatus = 0;
  const QJsonObject object = postJsonBlocking( QUrl( tokenEndpoint() ), payload, 30000, httpStatus, errorMessage );
  if ( object.isEmpty() )
    return false;

  token.token = object.value( u"access_token"_s ).toString();
  if ( token.token.isEmpty() )
  {
    if ( errorMessage )
      *errorMessage = u"Claude refresh response is missing an access token."_s;
    return false;
  }

  const QString rotated = object.value( u"refresh_token"_s ).toString();
  const qint64 expiresIn = static_cast<qint64>( object.value( u"expires_in"_s ).toDouble( 3600 ) );
  const QString expiresAtText = QString::number( QDateTime::currentMSecsSinceEpoch() + expiresIn * 1000 );
  if ( !storeLoginSecret( accessTokenSettingKey(), token.token, errorMessage ) )
    return false;
  if ( !storeLoginSecret( expiresAtSettingKey(), expiresAtText, errorMessage ) )
    return false;
  if ( !rotated.isEmpty() && rotated != refreshToken && !storeLoginSecret( refreshTokenSettingKey(), rotated, errorMessage ) )
    return false;
  return true;
}

bool QgsAiClaudeOAuthClient::hasRefreshToken()
{
  return QgsAiSecretStore::hasSecret( refreshTokenSettingKey() );
}

bool QgsAiClaudeOAuthClient::clearLogin( QString * )
{
  QgsAiSecretStore::removeSecret( refreshTokenSettingKey() );
  QgsAiSecretStore::removeSecret( accessTokenSettingKey() );
  QgsAiSecretStore::removeSecret( expiresAtSettingKey() );
  return true;
}

QString QgsAiClaudeOAuthClient::refreshTokenSettingKey()
{
  return u"ai/provider/claude/login/refreshToken"_s;
}

QString QgsAiClaudeOAuthClient::accessTokenSettingKey()
{
  return u"ai/provider/claude/login/accessToken"_s;
}

QString QgsAiClaudeOAuthClient::expiresAtSettingKey()
{
  return u"ai/provider/claude/login/expiresAt"_s;
}

QString QgsAiClaudeOAuthClient::oauthBetaHeader()
{
  return u"oauth-2025-04-20"_s;
}

void QgsAiClaudeOAuthClient::setTokenEndpointForTesting( const QString &url )
{
  tokenEndpointOverride = url.trimmed();
}

#include "moc_qgsaiclaudeoauthclient.cpp"
