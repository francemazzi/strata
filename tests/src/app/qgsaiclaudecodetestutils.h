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

#ifndef QGSAICLAUDECODETESTUTILS_H
#define QGSAICLAUDECODETESTUTILS_H
#include <cstdio>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QDir>
#include "ai/qgsaiclaudecodeauth.h"
using namespace Qt::StringLiterals;
inline void fakeClaudeWrite( const QJsonObject &event )
{
  const QByteArray line = QJsonDocument( event ).toJson( QJsonDocument::Compact ) + '\n';
  // Force the runtime to assemble a line across reads.
  fwrite( line.constData(), 1, 7, stdout );
  fflush( stdout );
  QThread::msleep( 5 );
  fwrite( line.constData() + 7, 1, static_cast<size_t>( line.size() - 7 ), stdout );
  fflush( stdout );
}
inline bool isFakeClaudeInvocation( int argc, char **argv )
{
  return argc > 1 && ( QByteArray( argv[1] ) == "--version" || QByteArray( argv[1] ) == "auth" || QByteArray( argv[1] ) == "--print" );
}
inline int fakeClaudeMain( int argc, char **argv )
{
  QCoreApplication app( argc, argv );
  const QStringList args = app.arguments();
  const QByteArray scenario = qgetenv( "STRATA_TEST_CLAUDE_CASE" );
  if ( args[1] == "--version"_L1 )
  {
    fputs( scenario == "old" ? "2.0.0 (Claude Code)\n" : "2.1.283 (Claude Code)\n", stdout );
    return 0;
  }
  if ( args[1] == "auth"_L1 )
  {
    if ( args.value( 2 ) == "status"_L1 )
    {
      QJsonObject status { { u"loggedIn"_s, scenario != "signed_out" }, { u"authMethod"_s, scenario == "api_key" ? u"api_key"_s : u"claude.ai"_s } };
      const QByteArray json = QJsonDocument( status ).toJson();
      fwrite( json.constData(), 1, static_cast<size_t>( json.size() ), stdout );
    }
    return 0;
  }
  if ( scenario == "hang" )
    return app.exec();
  if ( !qgetenv( "ANTHROPIC_API_KEY" ).isEmpty() || !args.contains( u"--restricted"_s ) || !args.contains( u"--no-session-persistence"_s ) || args.contains( u"--bare"_s ) || !args.contains( u"--strict-mcp-config"_s ) )
    return 17;
  QFile input;
  if ( !input.open( stdin, QIODevice::ReadOnly ) )
    return 18;
  if ( !args.contains( u"--mcp-config"_s ) )
  {
    // Only the initialization frame is allowed; the catalog closes without a prompt.
    const QJsonObject request = QJsonDocument::fromJson( input.readLine() ).object();
    if ( request.value( u"type"_s ) != "control_request"_L1 || request.value( u"request"_s ).toObject().value( u"subtype"_s ) != "initialize"_L1 )
      return 18;
    fakeClaudeWrite(
      { { u"type"_s, u"control_response"_s },
        { u"response"_s,
          QJsonObject {
            { u"subtype"_s, u"success"_s },
            { u"request_id"_s, request.value( u"request_id"_s ) },
            { u"response"_s, QJsonObject { { u"models"_s, QJsonArray { QJsonObject { { u"value"_s, u"claude-from-cli"_s }, { u"displayName"_s, u"Discovered Claude"_s } } } } } }
          } } }
    );
    return 0;
  }
  if ( !QJsonDocument::fromJson( input.readAll().trimmed() ).isObject() )
    return 18;
  if ( scenario == "rate_limit" )
  {
    fakeClaudeWrite( { { u"type"_s, u"assistant"_s }, { u"error"_s, u"rate_limit_error"_s } } );
    fakeClaudeWrite( { { u"type"_s, u"result"_s }, { u"subtype"_s, u"error_during_execution"_s }, { u"is_error"_s, true }, { u"result"_s, u"sensitive diagnostic must not be shown"_s } } );
    return 1;
  }
  QFile config( args.value( args.indexOf( u"--mcp-config"_s ) + 1 ) );
  if ( !config.open( QIODevice::ReadOnly ) )
    return 19;
  const QJsonObject server = QJsonDocument::fromJson( config.readAll() ).object().value( u"mcpServers"_s ).toObject().value( u"strata"_s ).toObject();
  QNetworkAccessManager nam;
  const auto rpc = [&]( const QString &method, const QJsonObject &params, int id ) {
    QNetworkRequest request( QUrl( server.value( u"url"_s ).toString() ) );
    request.setHeader( QNetworkRequest::ContentTypeHeader, u"application/json"_s );
    request.setRawHeader( "Authorization", server.value( u"headers"_s ).toObject().value( u"Authorization"_s ).toString().toUtf8() );
    request.setRawHeader( "Accept", "application/json, text/event-stream" );
    QNetworkReply *reply = nam.post( request, QJsonDocument( QJsonObject { { u"jsonrpc"_s, u"2.0"_s }, { u"id"_s, id }, { u"method"_s, method }, { u"params"_s, params } } ).toJson() );
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot( true );
    QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
    QObject::connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
    timer.start( 10000 );
    loop.exec();
    const QJsonObject response = QJsonDocument::fromJson( reply->readAll() ).object();
    reply->deleteLater();
    return response;
  };
  const auto init = rpc( u"initialize"_s, { { u"protocolVersion"_s, u"2025-03-26"_s } }, 1 );
  if ( !init.contains( u"result"_s ) )
    return 20;
  fakeClaudeWrite(
    { { u"type"_s, u"system"_s },
      { u"subtype"_s, u"init"_s },
      { u"model"_s, u"claude-test-model"_s },
      { u"mcp_servers"_s, QJsonArray { QJsonObject { { u"name"_s, u"strata"_s }, { u"status"_s, u"connected"_s } } } } }
  );
  const QJsonArray tools = rpc( u"tools/list"_s, {}, 2 ).value( u"result"_s ).toObject().value( u"tools"_s ).toArray();
  if ( !tools.isEmpty() )
  {
    const QJsonObject call { { u"name"_s, tools.first().toObject().value( u"name"_s ) }, { u"arguments"_s, QJsonObject() } };
    const auto first = rpc( u"tools/call"_s, call, 3 );
    const auto duplicate = rpc( u"tools/call"_s, call, 3 );
    if ( scenario == "budget" )
      rpc( u"tools/call"_s, call, 4 );
    if ( first != duplicate )
      return 21;
  }
  fakeClaudeWrite( { { u"type"_s, u"stream_event"_s }, { u"event"_s, QJsonObject { { u"delta"_s, QJsonObject { { u"type"_s, u"text_delta"_s }, { u"text"_s, u"Fixture response"_s } } } } } } );
  fakeClaudeWrite(
    { { u"type"_s, u"result"_s }, { u"subtype"_s, u"success"_s }, { u"result"_s, u"Fixture response"_s }, { u"usage"_s, QJsonObject { { u"input_tokens"_s, 11 }, { u"output_tokens"_s, 7 } } } }
  );
  return 0;
}
#endif
