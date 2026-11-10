/***************************************************************************
    qgsailocalservercatalog.cpp
    ---------------------
    begin                : November 2026
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

#include "qgsailocalservercatalog.h"

#include <algorithm>

#include "qgsmessagelog.h"
#include "qgsnetworkaccessmanager.h"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

#include "moc_qgsailocalservercatalog.cpp"

using namespace Qt::StringLiterals;

namespace
{
  constexpr int OLLAMA_DEFAULT_PORT = 11434;
  // A stopped local server refuses the connection at once; the short timeout
  // only matters for a custom host that drops packets.
  constexpr int PROBE_TIMEOUT_MS = 1500;
  constexpr int SHOW_TIMEOUT_MS = 5000;
  constexpr qint64 PROBE_THROTTLE_MS = 5000;

  bool sameModels( const QList<QgsAiLocalServerCatalog::ModelInfo> &a, const QList<QgsAiLocalServerCatalog::ModelInfo> &b )
  {
    if ( a.size() != b.size() )
      return false;
    for ( int i = 0; i < a.size(); ++i )
    {
      const QgsAiLocalServerCatalog::ModelInfo &left = a.at( i );
      const QgsAiLocalServerCatalog::ModelInfo &right = b.at( i );
      if (
        left.id != right.id || left.contextLength != right.contextLength || left.supportsTools != right.supportsTools || left.supportsThinking != right.supportsThinking || left.supportsVision != right.supportsVision
      )
        return false;
    }
    return true;
  }
} //namespace

QString QgsAiLocalServerCatalog::ModelInfo::displayLabel() const
{
  QStringList details;
  if ( !parameterSize.isEmpty() )
    details << parameterSize;
  if ( contextLength > 0 )
    details << u"%1k ctx"_s.arg( contextLength / 1000 );
  return details.isEmpty() ? id : u"%1 — %2"_s.arg( id, details.join( ", "_L1 ) );
}

QgsAiLocalServerCatalog::QgsAiLocalServerCatalog( QObject *parent )
  : QObject( parent )
{}

QString QgsAiLocalServerCatalog::normalizedBaseUrl( const QString &value, int defaultPort )
{
  QString text = value.trimmed();
  if ( text.isEmpty() )
    return QString();
  if ( !text.contains( "://"_L1 ) )
    text.prepend( u"http://"_s );

  QUrl url( text );
  if ( !url.isValid() )
    return QString();
  // OLLAMA_HOST may hold only a port (":11435") or the bind-all address.
  if ( url.host().isEmpty() || url.host() == "0.0.0.0"_L1 )
    url.setHost( u"127.0.0.1"_s );
  else if ( url.host() == "::"_L1 )
    url.setHost( u"::1"_s );
  if ( url.port() < 0 && url.scheme() == "http"_L1 )
    url.setPort( defaultPort );

  QString normalized = url.toString( QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo );
  while ( normalized.endsWith( '/'_L1 ) )
    normalized.chop( 1 );
  return normalized;
}

QString QgsAiLocalServerCatalog::defaultOllamaBaseUrl()
{
  const QString fromEnvironment = normalizedBaseUrl( QString::fromUtf8( qgetenv( "OLLAMA_HOST" ) ), OLLAMA_DEFAULT_PORT );
  return fromEnvironment.isEmpty() ? u"http://127.0.0.1:%1"_s.arg( OLLAMA_DEFAULT_PORT ) : fromEnvironment;
}

bool QgsAiLocalServerCatalog::isLoopbackUrl( const QString &baseUrl )
{
  const QString host = QUrl( baseUrl ).host();
  if ( host.compare( "localhost"_L1, Qt::CaseInsensitive ) == 0 )
    return true;
  const QHostAddress address( host );
  return !address.isNull() && address.isLoopback();
}

bool QgsAiLocalServerCatalog::parseOllamaShow( const QByteArray &body, ModelInfo &info )
{
  const QJsonDocument doc = QJsonDocument::fromJson( body );
  if ( !doc.isObject() )
    return false;
  const QJsonObject root = doc.object();

  // Servers older than the capabilities field cannot tell us what a model
  // supports: offer it rather than hiding every model on such an install.
  if ( !root.contains( u"capabilities"_s ) )
  {
    info.supportsTools = true;
  }
  else
  {
    bool completion = false;
    bool tools = false;
    const QJsonArray capabilities = root.value( u"capabilities"_s ).toArray();
    for ( const QJsonValue &capability : capabilities )
    {
      const QString name = capability.toString();
      if ( name == "completion"_L1 )
        completion = true;
      else if ( name == "tools"_L1 )
        tools = true;
      else if ( name == "thinking"_L1 )
        info.supportsThinking = true;
      else if ( name == "vision"_L1 )
        info.supportsVision = true;
    }
    info.supportsTools = completion && tools;
  }

  // The native window is reported as "<architecture>.context_length".
  const QJsonObject modelInfo = root.value( u"model_info"_s ).toObject();
  for ( auto it = modelInfo.constBegin(); it != modelInfo.constEnd(); ++it )
  {
    if ( it.key().endsWith( ".context_length"_L1 ) )
    {
      info.contextLength = it.value().toInt();
      break;
    }
  }

  const QString parameterSize = root.value( u"details"_s ).toObject().value( u"parameter_size"_s ).toString();
  if ( !parameterSize.isEmpty() )
    info.parameterSize = parameterSize;
  return true;
}

void QgsAiLocalServerCatalog::setBaseUrl( const QString &baseUrl )
{
  QString normalized = baseUrl.trimmed();
  while ( normalized.endsWith( '/'_L1 ) )
    normalized.chop( 1 );
  if ( normalized == mBaseUrl )
    return;

  mBaseUrl = normalized;
  // Invalidate any probe still in flight against the previous server.
  ++mGeneration;
  mProbing = false;
  mLastProbeMs = 0;
  mShowCache.clear();
  applyResult( ServerState::Unknown, QString(), {}, 0 );
}

QgsAiLocalServerCatalog::ModelInfo QgsAiLocalServerCatalog::model( const QString &id ) const
{
  for ( const ModelInfo &info : mModels )
  {
    if ( info.id == id )
      return info;
  }
  return ModelInfo();
}

void QgsAiLocalServerCatalog::markUnreachable()
{
  ++mGeneration;
  mProbing = false;
  applyResult( ServerState::NotRunning, QString(), {}, 0 );
}

void QgsAiLocalServerCatalog::applyResult( ServerState state, const QString &version, const QList<ModelInfo> &models, int installedCount )
{
  const bool differs = state != mState || version != mVersion || installedCount != mInstalledCount || !sameModels( models, mModels );
  mState = state;
  mVersion = version;
  mModels = models;
  mInstalledCount = installedCount;
  if ( differs )
    emit changed();
}

void QgsAiLocalServerCatalog::refresh( bool force )
{
  if ( mBaseUrl.isEmpty() || mProbing )
    return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if ( !force && mLastProbeMs > 0 && now - mLastProbeMs < PROBE_THROTTLE_MS )
    return;

  QgsNetworkAccessManager *networkManager = QgsNetworkAccessManager::instance();
  if ( !networkManager )
    return;

  mLastProbeMs = now;
  mProbing = true;
  const int generation = ++mGeneration;

  QNetworkRequest request( QUrl( mBaseUrl + u"/api/version"_s ) );
  request.setTransferTimeout( PROBE_TIMEOUT_MS );
  QNetworkReply *reply = networkManager->get( request );
  connect( reply, &QNetworkReply::finished, reply, &QObject::deleteLater );
  connect( reply, &QNetworkReply::finished, this, [this, reply, generation]() {
    if ( generation != mGeneration )
      return;
    const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    if ( reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
    {
      mProbing = false;
      applyResult( ServerState::NotRunning, QString(), {}, 0 );
      return;
    }
    mPendingVersion = QJsonDocument::fromJson( reply->readAll() ).object().value( u"version"_s ).toString();
    fetchTags( generation );
  } );
}

void QgsAiLocalServerCatalog::fetchTags( int generation )
{
  QNetworkRequest request( QUrl( mBaseUrl + u"/api/tags"_s ) );
  request.setTransferTimeout( SHOW_TIMEOUT_MS );
  QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( request );
  connect( reply, &QNetworkReply::finished, reply, &QObject::deleteLater );
  connect( reply, &QNetworkReply::finished, this, [this, reply, generation]() {
    if ( generation != mGeneration )
      return;

    mPendingInstalled.clear();
    if ( reply->error() == QNetworkReply::NoError )
    {
      const QJsonArray installed = QJsonDocument::fromJson( reply->readAll() ).object().value( u"models"_s ).toArray();
      for ( const QJsonValue &item : installed )
      {
        const QJsonObject obj = item.toObject();
        InstalledModel model;
        model.name = obj.value( u"name"_s ).toString();
        if ( model.name.isEmpty() )
          model.name = obj.value( u"model"_s ).toString();
        if ( model.name.isEmpty() )
          continue;
        model.digest = obj.value( u"digest"_s ).toString();
        model.parameterSize = obj.value( u"details"_s ).toObject().value( u"parameter_size"_s ).toString();
        mPendingInstalled.append( model );
      }
    }
    else
    {
      QgsMessageLog::logMessage( u"Local AI server answered the version probe but not the model list: %1"_s.arg( reply->errorString() ), u"AI"_s, Qgis::MessageLevel::Warning, false );
    }

    mPendingShows = 0;
    for ( const InstalledModel &model : std::as_const( mPendingInstalled ) )
    {
      if ( model.digest.isEmpty() || !mShowCache.contains( model.digest ) )
        ++mPendingShows;
    }
    if ( mPendingShows == 0 )
    {
      finishProbe( generation );
      return;
    }
    // Copy: fetchShow() replies can re-enter and the list must stay stable.
    const QList<InstalledModel> installed = mPendingInstalled;
    for ( const InstalledModel &model : installed )
    {
      if ( model.digest.isEmpty() || !mShowCache.contains( model.digest ) )
        fetchShow( generation, model.name, model.digest );
    }
  } );
}

void QgsAiLocalServerCatalog::fetchShow( int generation, const QString &name, const QString &digest )
{
  QNetworkRequest request( QUrl( mBaseUrl + u"/api/show"_s ) );
  request.setHeader( QNetworkRequest::ContentTypeHeader, u"application/json"_s );
  request.setTransferTimeout( SHOW_TIMEOUT_MS );
  QJsonObject body;
  body.insert( u"model"_s, name );
  QNetworkReply *reply = QgsNetworkAccessManager::instance()->post( request, QJsonDocument( body ).toJson( QJsonDocument::Compact ) );
  connect( reply, &QNetworkReply::finished, reply, &QObject::deleteLater );
  connect( reply, &QNetworkReply::finished, this, [this, reply, generation, name, digest]() {
    if ( generation != mGeneration )
      return;

    ModelInfo info;
    // A model whose details cannot be read stays hidden: offering it would
    // risk an agent turn against a model without tool calling.
    if ( reply->error() == QNetworkReply::NoError && parseOllamaShow( reply->readAll(), info ) )
      mShowCache.insert( digest.isEmpty() ? name : digest, info );

    if ( --mPendingShows <= 0 )
      finishProbe( generation );
  } );
}

void QgsAiLocalServerCatalog::finishProbe( int generation )
{
  if ( generation != mGeneration )
    return;

  QList<ModelInfo> models;
  for ( const InstalledModel &installed : std::as_const( mPendingInstalled ) )
  {
    const QString cacheKey = installed.digest.isEmpty() ? installed.name : installed.digest;
    if ( !mShowCache.contains( cacheKey ) )
      continue;
    ModelInfo info = mShowCache.value( cacheKey );
    if ( !info.supportsTools )
      continue;
    info.id = installed.name;
    if ( info.parameterSize.isEmpty() )
      info.parameterSize = installed.parameterSize;
    models.append( info );
  }
  std::sort( models.begin(), models.end(), []( const ModelInfo &a, const ModelInfo &b ) { return a.id.compare( b.id, Qt::CaseInsensitive ) < 0; } );

  mProbing = false;
  applyResult( ServerState::Running, mPendingVersion, models, static_cast<int>( mPendingInstalled.size() ) );
}
