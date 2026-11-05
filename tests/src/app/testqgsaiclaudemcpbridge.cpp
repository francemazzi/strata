/***************************************************************************
    qgsaiclaudecodeauth.cpp
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

#include "ai/qgsaiclaudemcpbridge.h"
#include "qgstest.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
class TestQgsAiClaudeMcpBridge : public QObject
{
    Q_OBJECT
  private:
    QJsonObject server;
    int executions = 0;
    QgsAiClaudeMcpBridge bridge;
    QNetworkAccessManager network;
    QPair<int, QJsonObject> post( const QJsonObject &body, const QByteArray &origin = {}, bool authenticate = true )
    {
      QNetworkRequest request( QUrl( server.value( u"url"_s ).toString() ) );
      request.setHeader( QNetworkRequest::ContentTypeHeader, u"application/json"_s );
      if ( authenticate )
        request.setRawHeader( "Authorization", server.value( u"headers"_s ).toObject().value( u"Authorization"_s ).toString().toUtf8() );
      if ( !origin.isEmpty() )
        request.setRawHeader( "Origin", origin );
      QNetworkReply *reply = network.post( request, QJsonDocument( body ).toJson() );
      QSignalSpy done( reply, &QNetworkReply::finished );
      if ( !done.wait( 5000 ) )
      {
        reply->abort();
        reply->deleteLater();
        return {};
      }
      const auto result = qMakePair( reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt(), QJsonDocument::fromJson( reply->readAll() ).object() );
      reply->deleteLater();
      return result;
    }
    QJsonObject rpc( const QString &method, const QJsonObject &params = {}, int id = 1 ) const
    {
      return { { u"jsonrpc"_s, u"2.0"_s }, { u"id"_s, id }, { u"method"_s, method }, { u"params"_s, params } };
    }
  private slots:
    void init()
    {
      executions = 0;
      QVERIFY(
        bridge.start( { QJsonObject { { u"name"_s, u"test"_s }, { u"input_schema"_s, QJsonObject { { u"type"_s, u"object"_s } } } } }, [this]( const QString &, const QJsonObject &, const QString & ) {
          ++executions;
          return QgsAiClaudeMcpBridge::toolResult( u"done"_s );
        } )
      );
      server = bridge.configuration().value( u"mcpServers"_s ).toObject().value( u"strata"_s ).toObject();
    }
    void cleanup() { bridge.stop(); }
    void initializeAndTools()
    {
      const auto response = post( rpc( u"initialize"_s ) );
      QCOMPARE( response.first, 200 );
      QCOMPARE( response.second.value( u"result"_s ).toObject().value( u"protocolVersion"_s ).toString(), u"2025-03-26"_s );
      const auto list = post( rpc( u"tools/list"_s ) ).second.value( u"result"_s ).toObject().value( u"tools"_s ).toArray();
      QCOMPARE( list.size(), 1 );
      QVERIFY( list.first().toObject().contains( u"inputSchema"_s ) );
    }
    void authenticationAndOriginAreRequired()
    {
      QCOMPARE( post( rpc( u"tools/list"_s ), {}, false ).first, 401 );
      QCOMPARE( post( rpc( u"tools/list"_s ), "https://evil.example" ).first, 400 );
      QCOMPARE( executions, 0 );
    }
    void repeatedRequestsDoNotRepeatEffects()
    {
      const auto request = rpc( u"tools/call"_s, { { u"name"_s, u"test"_s }, { u"arguments"_s, QJsonObject() } }, 27 );
      const auto first = post( request );
      const auto second = post( request );
      QCOMPARE( first, second );
      QCOMPARE( executions, 1 );
      auto changed = request;
      changed.insert( u"params"_s, QJsonObject { { u"name"_s, u"test"_s }, { u"arguments"_s, QJsonObject { { u"different"_s, true } } } } );
      QVERIFY( post( changed ).second.value( u"result"_s ).toObject().value( u"isError"_s ).toBool() );
      QCOMPARE( executions, 1 );
    }
    void noArgumentToolsMayOmitArguments()
    {
      const auto response = post( rpc( u"tools/call"_s, { { u"name"_s, u"test"_s } } ) );
      QVERIFY( !response.second.value( u"result"_s ).toObject().value( u"isError"_s ).toBool() );
      QCOMPARE( executions, 1 );
    }
    void unadvertisedToolCannotExecute()
    {
      const auto response = post( rpc( u"tools/call"_s, { { u"name"_s, u"unavailable"_s }, { u"arguments"_s, QJsonObject() } } ) );
      QVERIFY( response.second.value( u"result"_s ).toObject().value( u"isError"_s ).toBool() );
      QCOMPARE( executions, 0 );
    }
    void stoppedServerInvalidatesConfiguration()
    {
      const auto old = server;
      bridge.stop();
      init();
      QVERIFY( old.value( u"headers"_s ) != server.value( u"headers"_s ) );
    }
};
QGSTEST_MAIN( TestQgsAiClaudeMcpBridge )
#include "testqgsaiclaudemcpbridge.moc"
