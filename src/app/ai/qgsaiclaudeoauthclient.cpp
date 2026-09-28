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

#include "qgsaioauthcallbackpage.h"
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

  struct TokenRequestResult
  {
      QJsonObject payload;
      QgsAiClaudeOAuthError error;
      bool ok() const { return !payload.isEmpty(); }
  };

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

  TokenRequestResult postClaudeTokenRequest( const QUrl &url, const QJsonObject &payload, int timeoutMs )
  {
    QgsNetworkAccessManager *nam = QgsNetworkAccessManager::instance();
    if ( !nam )
    {
      return { {}, QgsAiClaudeOAuthErrorParser::fromTokenReply( 0, {}, {}, false, QObject::tr( "Network manager is not available." ) ) };
    }

    QNetworkRequest request( url );
    request.setHeader( QNetworkRequest::ContentTypeHeader, u"application/json"_s );
    request.setAttribute( static_cast<QNetworkRequest::Attribute>( QgsNetworkRequestParameters::AttributeUserAgentSuffix ), QString::fromUtf8( CLAUDE_USER_AGENT ) );
    request.setTransferTimeout( timeoutMs );

    QNetworkReply *reply = nam->post( request, QJsonDocument( payload ).toJson( QJsonDocument::Compact ) );
    if ( !reply )
      return { {}, QgsAiClaudeOAuthErrorParser::fromTokenReply( 0, {}, {}, false, QObject::tr( "Unable to start the Claude token request." ) ) };

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot( true );
    QObject::connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
    QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
    timer.start( timeoutMs );
    loop.exec();

    const bool timedOut = !timer.isActive();
    if ( !timedOut )
      timer.stop();
    else
      reply->abort();

    const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    const QByteArray body = reply->readAll();
    const QByteArray retryAfter = reply->rawHeader( "Retry-After" );
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkDetail = reply->errorString();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson( body );
    const QJsonObject object = doc.isObject() ? doc.object() : QJsonObject();
    if ( networkError != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
      return { {}, QgsAiClaudeOAuthErrorParser::fromTokenReply( httpStatus, body, retryAfter, timedOut, networkDetail ) };
    if ( object.isEmpty() )
    {
      QgsAiClaudeOAuthError error;
      error.category = QgsAiClaudeOAuthError::Category::InvalidResponse;
      error.userMessage = QObject::tr( "Claude returned an empty connection response. Try again from Strata." );
      error.technicalDetail = QObject::tr( "The token endpoint returned HTTP %1 without credentials." ).arg( httpStatus );
      return { {}, error };
    }
    return { object, {} };
  }

  void respondHtml( QTcpSocket *socket, int status, QgsAiOAuthCallbackPage::State state, const QString &title, const QString &message, const QString &detail = QString() )
  {
    socket->write( QgsAiOAuthCallbackPage::httpResponse( status, QgsAiOAuthCallbackPage::render( state, title, message, detail ) ) );
    socket->disconnectFromHost();
  }

  QString errorText( const QgsAiClaudeOAuthError &error )
  {
    return error.technicalDetail.isEmpty() ? error.userMessage : u"%1 (%2)"_s.arg( error.userMessage, error.technicalDetail );
  }
} // namespace

QgsAiClaudeOAuthClient::QgsAiClaudeOAuthClient( QObject *parent )
  : QObject( parent )
{
  qRegisterMetaType<QgsAiClaudeOAuthError>();
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
  connect( mTimeout, &QTimer::timeout, this, [this]() {
    QgsAiClaudeOAuthError error;
    error.category = QgsAiClaudeOAuthError::Category::Timeout;
    error.userMessage = tr( "Claude login timed out. Try Connect Claude again." );
    fail( error );
  } );
  mTimeout->start( mLoginTimeoutMs );
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

void QgsAiClaudeOAuthClient::fail( const QgsAiClaudeOAuthError &error )
{
  if ( mFinished && !mServer )
    return;
  cancel();
  emit loginFailed( error );
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
      QgsAiClaudeOAuthError error;
      error.category = QgsAiClaudeOAuthError::Category::InvalidRequest;
      error.userMessage = tr( "Claude login received an unexpected response. Try again from Strata." );
      respondHtml( socket, 400, QgsAiOAuthCallbackPage::State::Error, tr( "Connection not completed" ), error.userMessage );
      fail( error );
      return;
    }

    const QUrl url( QString::fromUtf8( QByteArrayLiteral( "http://127.0.0.1" ) + parts.at( 1 ) ) );
    const QUrlQuery query( url );
    if ( query.queryItemValue( u"state"_s ) != mState )
    {
      QgsAiClaudeOAuthError error;
      error.category = QgsAiClaudeOAuthError::Category::StateMismatch;
      error.userMessage = tr( "Claude login could not be verified. Try Connect Claude again." );
      respondHtml( socket, 400, QgsAiOAuthCallbackPage::State::Error, tr( "Connection not verified" ), error.userMessage );
      fail( error );
      return;
    }
    const QString code = query.queryItemValue( u"code"_s );
    if ( code.isEmpty() )
    {
      QgsAiClaudeOAuthError error;
      error.category = QgsAiClaudeOAuthError::Category::AuthorizationDenied;
      error.userMessage = tr( "Claude login was not approved. No connection was saved." );
      error.technicalDetail = QgsAiClaudeOAuthErrorParser::safeDetail( query.queryItemValue( u"error_description"_s, QUrl::FullyDecoded ) );
      respondHtml( socket, 400, QgsAiOAuthCallbackPage::State::Error, tr( "Connection cancelled" ), error.userMessage, error.technicalDetail );
      fail( error );
      return;
    }

    QgsAiClaudeOAuthError error;
    if ( !exchangeCode( code, &error ) )
    {
      const bool limited = error.category == QgsAiClaudeOAuthError::Category::RateLimited;
      respondHtml(
        socket,
        limited ? 429 : 502,
        limited ? QgsAiOAuthCallbackPage::State::RateLimited : QgsAiOAuthCallbackPage::State::Error,
        limited ? tr( "Try again later" ) : tr( "Connection not completed" ),
        error.userMessage,
        error.technicalDetail
      );
      fail( error );
      return;
    }
    respondHtml( socket, 200, QgsAiOAuthCallbackPage::State::Success, tr( "Claude is connected" ), tr( "Your Claude subscription is ready to use in Strata." ) );
    cancel();
    emit loginSucceeded();
  };
  connect( socket, &QTcpSocket::readyRead, this, readCallback );
  socket->setParent( this );
  if ( socket->bytesAvailable() > 0 )
    readCallback();
}

bool QgsAiClaudeOAuthClient::exchangeCode( const QString &code, QgsAiClaudeOAuthError *error )
{
  QJsonObject payload;
  payload.insert( u"grant_type"_s, u"authorization_code"_s );
  payload.insert( u"code"_s, code );
  payload.insert( u"redirect_uri"_s, mRedirectUri );
  payload.insert( u"client_id"_s, QString::fromUtf8( CLAUDE_CLIENT_ID ) );
  payload.insert( u"code_verifier"_s, QString::fromLatin1( mVerifier ) );
  payload.insert( u"state"_s, mState );

  const TokenRequestResult result = postClaudeTokenRequest( QUrl( tokenEndpoint() ), payload, 30000 );
  if ( !result.ok() )
  {
    if ( error )
      *error = result.error;
    return false;
  }

  const QJsonObject tokenObject = result.payload;
  const QString accessToken = tokenObject.value( u"access_token"_s ).toString();
  const QString refreshToken = tokenObject.value( u"refresh_token"_s ).toString();
  if ( accessToken.isEmpty() || refreshToken.isEmpty() )
  {
    if ( error )
    {
      error->category = QgsAiClaudeOAuthError::Category::InvalidResponse;
      error->userMessage = tr( "Claude returned an incomplete connection response. Try again from Strata." );
      error->technicalDetail = tr( "The token response did not include both required credentials." );
    }
    return false;
  }

  const qint64 expiresIn = static_cast<qint64>( tokenObject.value( u"expires_in"_s ).toDouble( 3600 ) );
  const QString expiresAt = QString::number( QDateTime::currentMSecsSinceEpoch() + expiresIn * 1000 );
  QString storageError;
  const bool stored = storeLoginSecret( accessTokenSettingKey(), accessToken, &storageError )
                      && storeLoginSecret( expiresAtSettingKey(), expiresAt, &storageError )
                      && storeLoginSecret( refreshTokenSettingKey(), refreshToken, &storageError );
  if ( stored )
    return true;

  clearLogin();
  if ( error )
  {
    error->category = QgsAiClaudeOAuthError::Category::CredentialStorage;
    error->userMessage = tr( "Claude approved the connection, but Strata could not save it securely." );
    error->technicalDetail = QgsAiClaudeOAuthErrorParser::safeDetail( storageError );
  }
  return false;
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

  const TokenRequestResult result = postClaudeTokenRequest( QUrl( tokenEndpoint() ), payload, 30000 );
  if ( !result.ok() )
  {
    if ( errorMessage )
      *errorMessage = errorText( result.error );
    return false;
  }

  const QJsonObject object = result.payload;
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
