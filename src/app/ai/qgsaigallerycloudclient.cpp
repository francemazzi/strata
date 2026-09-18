/***************************************************************************
    qgsaigallerycloudclient.cpp
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

#include "qgsaigallerycloudclient.h"

#include <memory>

#include "qgsnetworkaccessmanager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

#include "moc_qgsaigallerycloudclient.cpp"

using namespace Qt::StringLiterals;

namespace
{
  constexpr int REQUEST_TIMEOUT_MS = 20000;

  void setJsonHeaders( QNetworkRequest &request, const QString &sessionToken )
  {
    request.setHeader( QNetworkRequest::ContentTypeHeader, u"application/json"_s );
    request.setRawHeader( "Accept", "application/json" );
    if ( !sessionToken.trimmed().isEmpty() )
      request.setRawHeader( "Authorization", ( u"Bearer %1"_s.arg( sessionToken.trimmed() ) ).toUtf8() );
    request.setTransferTimeout( REQUEST_TIMEOUT_MS );
  }

  QString responseError( QNetworkReply *reply, const QByteArray &body )
  {
    const int httpStatus = reply ? reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt() : 0;
    const QJsonObject root = QJsonDocument::fromJson( body ).object();
    QString message = root.value( u"message"_s ).toString();
    if ( message.isEmpty() && reply )
      message = reply->errorString();
    return httpStatus > 0 ? QObject::tr( "Strata gallery request failed (HTTP %1): %2" ).arg( httpStatus ).arg( message ) : message;
  }

  QString encoded( const QString &value )
  {
    return QString::fromLatin1( QUrl::toPercentEncoding( value ) );
  }

  QList<QgsAiGalleryCloudClient::Connector> connectorsFromJson( const QJsonValue &value )
  {
    QList<QgsAiGalleryCloudClient::Connector> out;
    for ( const QJsonValue &item : value.toArray() )
    {
      const QJsonObject obj = item.toObject();
      QgsAiGalleryCloudClient::Connector connector;
      connector.slug = obj.value( u"slug"_s ).toString();
      if ( connector.slug.isEmpty() )
        continue;
      connector.enabled = obj.value( u"enabled"_s ).toBool( true );
      out << connector;
    }
    return out;
  }

  QgsAiGalleryCloudClient::PackSummary packFromJson( const QJsonObject &obj )
  {
    QgsAiGalleryCloudClient::PackSummary pack;
    pack.slug = obj.value( u"slug"_s ).toString();
    pack.name = obj.value( u"name"_s ).toString();
    pack.description = obj.value( u"description"_s ).toString();
    for ( const QJsonValue &item : obj.value( u"skills"_s ).toArray() )
    {
      const QString slug = item.toString();
      if ( !slug.isEmpty() )
        pack.skillSlugs << slug;
    }
    for ( const QJsonValue &item : obj.value( u"rules"_s ).toArray() )
    {
      const QString slug = item.toString();
      if ( !slug.isEmpty() )
        pack.ruleSlugs << slug;
    }
    pack.connectors = connectorsFromJson( obj.value( u"connectors"_s ) );
    return pack;
  }

  QgsAiGalleryCloudClient::McpServer mcpServerFromJson( const QJsonObject &obj )
  {
    QgsAiGalleryCloudClient::McpServer server;
    server.slug = obj.value( u"slug"_s ).toString();
    server.name = obj.value( u"name"_s ).toString();
    server.description = obj.value( u"description"_s ).toString();
    server.mutating = obj.value( u"mutating"_s ).toBool( false );
    server.enabled = obj.value( u"enabled"_s ).toBool( true );
    server.scanStatus = obj.value( u"scanStatus"_s ).toString();
    return server;
  }

  QNetworkReply *getJson( const QString &url, const QString &sessionToken )
  {
    QgsNetworkAccessManager *networkManager = QgsNetworkAccessManager::instance();
    if ( !networkManager )
      return nullptr;
    // Brace-init: `QNetworkRequest request( QUrl( url ) )` parses as a function declaration.
    QNetworkRequest request{ QUrl( url ) };
    setJsonHeaders( request, sessionToken );
    return networkManager->get( request );
  }
} // namespace

QgsAiGalleryCloudClient::QgsAiGalleryCloudClient( QObject *parent )
  : QObject( parent )
{
  qRegisterMetaType<QgsAiGalleryCloudClient::PackSummary>();
  qRegisterMetaType<QgsAiGalleryCloudClient::PackImport>();
  qRegisterMetaType<QgsAiGalleryCloudClient::McpServer>();
}

void QgsAiGalleryCloudClient::fetchPacks( const QString &apiBase, const QString &sessionToken )
{
  QNetworkReply *reply = getJson( apiBase + u"/v1/gallery/packs"_s, sessionToken );
  if ( !reply )
  {
    emit requestFailed( tr( "Network manager is not available." ) );
    return;
  }
  connect( reply, &QNetworkReply::finished, reply, &QObject::deleteLater );
  connect( reply, &QNetworkReply::finished, this, [this, reply]() {
    const QByteArray body = reply->readAll();
    const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    if ( reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
    {
      emit requestFailed( responseError( reply, body ) );
      return;
    }
    QList<PackSummary> packs;
    for ( const QJsonValue &item : QJsonDocument::fromJson( body ).object().value( u"items"_s ).toArray() )
    {
      PackSummary pack = packFromJson( item.toObject() );
      if ( !pack.slug.isEmpty() )
        packs << pack;
    }
    emit packsFetched( packs );
  } );
}

void QgsAiGalleryCloudClient::fetchPackForImport( const QString &apiBase, const QString &sessionToken, const QString &slug )
{
  QNetworkReply *reply = getJson( apiBase + u"/v1/gallery/packs/%1"_s.arg( encoded( slug ) ), sessionToken );
  if ( !reply )
  {
    emit requestFailed( tr( "Network manager is not available." ) );
    return;
  }
  connect( reply, &QNetworkReply::finished, reply, &QObject::deleteLater );
  connect( reply, &QNetworkReply::finished, this, [this, reply, apiBase, sessionToken]() {
    const QByteArray body = reply->readAll();
    const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    if ( reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
    {
      emit requestFailed( responseError( reply, body ) );
      return;
    }
    auto sharedImport = std::make_shared<PackImport>();
    sharedImport->pack = packFromJson( QJsonDocument::fromJson( body ).object() );
    auto remaining = std::make_shared<int>( sharedImport->pack.skillSlugs.size() + sharedImport->pack.ruleSlugs.size() );
    auto failed = std::make_shared<bool>( false );
    auto finishChild = [this, remaining, sharedImport, failed]() {
      --( *remaining );
      if ( *remaining != 0 || *failed )
        return;
      emit packImportReady( *sharedImport );
    };
    if ( *remaining == 0 )
    {
      emit packImportReady( *sharedImport );
      return;
    }

    auto fetchChild = [this, apiBase, sessionToken, sharedImport, failed, finishChild]( const QString &path, bool skill ) {
      QNetworkReply *childReply = getJson( apiBase + path, sessionToken );
      if ( !childReply )
      {
        *failed = true;
        emit requestFailed( tr( "Network manager is not available." ) );
        finishChild();
        return;
      }
      connect( childReply, &QNetworkReply::finished, childReply, &QObject::deleteLater );
      connect( childReply, &QNetworkReply::finished, this, [this, childReply, sharedImport, skill, failed, finishChild]() {
        const QByteArray childBody = childReply->readAll();
        const int childStatus = childReply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
        if ( childReply->error() != QNetworkReply::NoError || childStatus < 200 || childStatus >= 300 )
        {
          *failed = true;
          emit requestFailed( responseError( childReply, childBody ) );
          finishChild();
          return;
        }
        const QJsonObject obj = QJsonDocument::fromJson( childBody ).object();
        if ( skill )
        {
          PackSkill child;
          child.slug = obj.value( u"slug"_s ).toString();
          child.name = obj.value( u"name"_s ).toString();
          child.description = obj.value( u"description"_s ).toString();
          child.content = obj.value( u"content"_s ).toString();
          sharedImport->skills << child;
        }
        else
        {
          PackRule child;
          child.slug = obj.value( u"slug"_s ).toString();
          child.name = obj.value( u"name"_s ).toString();
          child.description = obj.value( u"description"_s ).toString();
          child.content = obj.value( u"content"_s ).toString();
          sharedImport->rules << child;
        }
        finishChild();
      } );
    };

    for ( const QString &skillSlug : sharedImport->pack.skillSlugs )
      fetchChild( u"/v1/gallery/skills/%1"_s.arg( encoded( skillSlug ) ), true );
    for ( const QString &ruleSlug : sharedImport->pack.ruleSlugs )
      fetchChild( u"/v1/gallery/rules/%1"_s.arg( encoded( ruleSlug ) ), false );
  } );
}

void QgsAiGalleryCloudClient::fetchMcpServers( const QString &apiBase, const QString &sessionToken )
{
  QNetworkReply *reply = getJson( apiBase + u"/v1/mcp/servers"_s, sessionToken );
  if ( !reply )
  {
    emit requestFailed( tr( "Network manager is not available." ) );
    return;
  }
  connect( reply, &QNetworkReply::finished, reply, &QObject::deleteLater );
  connect( reply, &QNetworkReply::finished, this, [this, reply]() {
    const QByteArray body = reply->readAll();
    const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    if ( reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
    {
      emit requestFailed( responseError( reply, body ) );
      return;
    }
    QList<McpServer> servers;
    for ( const QJsonValue &item : QJsonDocument::fromJson( body ).object().value( u"items"_s ).toArray() )
    {
      McpServer server = mcpServerFromJson( item.toObject() );
      if ( !server.slug.isEmpty() )
        servers << server;
    }
    emit mcpServersFetched( servers );
  } );
}

void QgsAiGalleryCloudClient::setMcpServerEnabled( const QString &apiBase, const QString &sessionToken, const QString &slug, bool enabled )
{
  QgsNetworkAccessManager *networkManager = QgsNetworkAccessManager::instance();
  if ( !networkManager )
  {
    emit requestFailed( tr( "Network manager is not available." ) );
    return;
  }
  QNetworkRequest request( QUrl( apiBase + u"/v1/mcp/servers/%1"_s.arg( encoded( slug ) ) ) );
  setJsonHeaders( request, sessionToken );
  QJsonObject body;
  body.insert( u"enabled"_s, enabled );
  QNetworkReply *reply = networkManager->sendCustomRequest( request, "PATCH", QJsonDocument( body ).toJson( QJsonDocument::Compact ) );
  if ( !reply )
  {
    emit requestFailed( tr( "Unable to update the MCP connector." ) );
    return;
  }
  connect( reply, &QNetworkReply::finished, reply, &QObject::deleteLater );
  connect( reply, &QNetworkReply::finished, this, [this, reply]() {
    const QByteArray payload = reply->readAll();
    const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    if ( reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
    {
      emit requestFailed( responseError( reply, payload ) );
      return;
    }
    emit mcpServerUpdated( mcpServerFromJson( QJsonDocument::fromJson( payload ).object() ) );
  } );
}
