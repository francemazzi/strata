/***************************************************************************
    qgsaiclaudemcpbridge.cpp
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

#include "qgsaiclaudemcpbridge.h"
#include <memory>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QScopedValueRollback>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include "moc_qgsaiclaudemcpbridge.cpp"
using namespace Qt::StringLiterals;

QgsAiClaudeMcpBridge::QgsAiClaudeMcpBridge( QObject *parent )
  : QObject( parent )
{
  connect( &mServer, &QTcpServer::newConnection, this, &QgsAiClaudeMcpBridge::acceptConnection );
}
bool QgsAiClaudeMcpBridge::start( const QJsonArray &tools, Executor executor )
{
  stop();
  mTools = tools;
  mExecutor = std::move( executor );
  mRunId = QUuid::createUuid().toString( QUuid::WithoutBraces );
  QByteArray random( 32, '\0' );
  for ( char &byte : random )
    byte = static_cast<char>( QRandomGenerator::system()->generate() & 255 );
  mToken = random.toHex();
  return mServer.listen( QHostAddress::LocalHost, 0 );
}
void QgsAiClaudeMcpBridge::stop()
{
  mServer.close();
  const auto sockets = mSockets;
  for ( QTcpSocket *socket : sockets )
  {
    socket->disconnect( this );
    socket->abort();
    socket->deleteLater();
  }
  mSockets.clear();
  mToken.clear();
  mResponses.clear();
  mRequests.clear();
  mExecutor = {};
}
QJsonObject QgsAiClaudeMcpBridge::configuration() const
{
  return {
    { u"mcpServers"_s,
      QJsonObject {
        { u"strata"_s,
          QJsonObject { { u"type"_s, u"http"_s }, { u"url"_s, u"http://127.0.0.1:%1/mcp"_s.arg( mServer.serverPort() ) }, { u"headers"_s, QJsonObject { { u"Authorization"_s, QString::fromLatin1( "Bearer " + mToken ) } } } } }
      } }
  };
}
QJsonObject QgsAiClaudeMcpBridge::toolResult( const QString &text, bool error )
{
  return { { u"content"_s, QJsonArray { QJsonObject { { u"type"_s, u"text"_s }, { u"text"_s, text } } } }, { u"isError"_s, error } };
}
void QgsAiClaudeMcpBridge::respond( QTcpSocket *socket, int status, const QByteArray &body )
{
  if ( !mSockets.contains( socket ) )
    return;
  mSockets.remove( socket );
  const QByteArray reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 405 ? "Method Not Allowed" : status == 401 ? "Unauthorized" : "Bad Request";
  socket->write(
    "HTTP/1.1 "
    + QByteArray::number( status )
    + ' '
    + reason
    + "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "
    + QByteArray::number( body.size() )
    + "\r\n\r\n"
    + body
  );
  socket->disconnectFromHost();
}
void QgsAiClaudeMcpBridge::acceptConnection()
{
  while ( mServer.hasPendingConnections() )
  {
    QTcpSocket *socket = mServer.nextPendingConnection();
    socket->setParent( this );
    mSockets.insert( socket );
    auto buffer = std::make_shared<QByteArray>();
    connect( socket, &QTcpSocket::disconnected, this, [this, socket]() {
      mSockets.remove( socket );
      socket->deleteLater();
    } );
    connect( socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
      if ( !mSockets.contains( socket ) )
        return;
      *buffer += socket->readAll();
      if ( buffer->size() > 2 * 1024 * 1024 )
      {
        respond( socket, 400 );
        return;
      }
      const int headerEnd = buffer->indexOf( "\r\n\r\n" );
      if ( headerEnd < 0 )
      {
        if ( buffer->size() > 16384 )
          respond( socket, 400 );
        return;
      }
      if ( headerEnd > 16384 )
      {
        respond( socket, 400 );
        return;
      }
      const auto lines = buffer->left( headerEnd ).split( '\n' );
      qint64 length = 0;
      bool seenLength = false;
      for ( const QByteArray &line : lines )
      {
        if ( line.toLower().startsWith( "transfer-encoding:" ) )
        {
          respond( socket, 400 );
          return;
        }
        if ( line.toLower().startsWith( "content-length:" ) )
        {
          bool valid = false;
          length = line.mid( 15 ).trimmed().toLongLong( &valid );
          if ( seenLength || !valid || length < 0 || length > 2 * 1024 * 1024 )
          {
            respond( socket, 400 );
            return;
          }
          seenLength = true;
        }
      }
      if ( buffer->size() < headerEnd + 4 + length )
        return;
      const QByteArray request = buffer->left( headerEnd + 4 + length );
      buffer->clear();
      // Exactly one request per connection, including while tools pump the event loop.
      socket->setProperty( "requestDispatched", true );
      socket->disconnect( this );
      connect( socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater );
      handleRequest( socket, request );
    } );
    QTimer::singleShot( 15000, socket, [this, socket]() {
      // Only idle connections remain without a request being dispatched.
      if ( !socket->property( "requestDispatched" ).toBool() )
        respond( socket, 400 );
    } );
  }
}
void QgsAiClaudeMcpBridge::handleRequest( QTcpSocket *socket, const QByteArray &request )
{
  const int headerEnd = request.indexOf( "\r\n\r\n" );
  const QList<QByteArray> lines = request.left( headerEnd ).split( '\n' );
  const auto first = lines.value( 0 ).trimmed().split( ' ' );
  QMap<QByteArray, QByteArray> headers;
  for ( int i = 1; i < lines.size(); ++i )
  {
    const int colon = lines[i].indexOf( ':' );
    if ( colon > 0 )
      headers.insert( lines[i].left( colon ).trimmed().toLower(), lines[i].mid( colon + 1 ).trimmed() );
  }
  const QByteArray host = "127.0.0.1:" + QByteArray::number( mServer.serverPort() );
  if ( headers.value( "host" ) != host || ( headers.contains( "origin" ) && headers.value( "origin" ) != "http://" + host ) )
  {
    respond( socket, 400 );
    return;
  }
  if ( mToken.isEmpty() || headers.value( "authorization" ) != "Bearer " + mToken )
  {
    respond( socket, 401 );
    return;
  }
  if ( first.size() != 3 || first[1] != "/mcp" )
  {
    respond( socket, 400 );
    return;
  }
  if ( first[0] != "POST" )
  {
    respond( socket, 405 );
    return;
  }
  if ( !headers.value( "content-type" ).startsWith( "application/json" ) )
  {
    respond( socket, 400 );
    return;
  }
  const QJsonDocument doc = QJsonDocument::fromJson( request.mid( headerEnd + 4 ) );
  if ( !doc.isObject() || doc.object().value( u"jsonrpc"_s ) != "2.0"_L1 )
  {
    respond( socket, 400 );
    return;
  }
  const QJsonObject message = doc.object();
  if ( !message.contains( u"id"_s ) )
  {
    respond( socket, 202 );
    return;
  }
  const QJsonObject result = dispatch( message );
  respond( socket, 200, QJsonDocument( result ).toJson( QJsonDocument::Compact ) );
}
QJsonObject QgsAiClaudeMcpBridge::dispatch( const QJsonObject &request )
{
  const QJsonValue id = request.value( u"id"_s );
  QJsonObject response { { u"jsonrpc"_s, u"2.0"_s }, { u"id"_s, id } };
  const QString method = request.value( u"method"_s ).toString();
  const QJsonObject params = request.value( u"params"_s ).toObject();
  QJsonObject result;
  if ( method == "initialize"_L1 )
    result
      = { { u"protocolVersion"_s, u"2025-03-26"_s }, { u"capabilities"_s, QJsonObject { { u"tools"_s, QJsonObject() } } }, { u"serverInfo"_s, QJsonObject { { u"name"_s, u"Strata"_s }, { u"version"_s, u"1.0"_s } } } };
  else if ( method == "ping"_L1 )
    result = {};
  else if ( method == "tools/list"_L1 )
  {
    QJsonArray tools;
    for ( const QJsonValue &value : mTools )
    {
      QJsonObject tool = value.toObject();
      tool.insert( u"inputSchema"_s, tool.take( u"input_schema"_s ) );
      tools.append( tool );
    }
    result.insert( u"tools"_s, tools );
  }
  else if ( method == "tools/call"_L1 )
  {
    const QString name = params.value( u"name"_s ).toString();
    bool allowed = false;
    for ( const QJsonValue &value : mTools )
      allowed |= value.toObject().value( u"name"_s ).toString() == name;
    const QString key = QString::fromUtf8( QJsonDocument( QJsonArray { id } ).toJson( QJsonDocument::Compact ) );
    const QByteArray fingerprint = QJsonDocument( params ).toJson( QJsonDocument::Compact );
    if ( mResponses.contains( key ) && mRequests.value( key ) == fingerprint )
      return mResponses.value( key );
    if ( !allowed || !mExecutor || mExecuting || mRequests.contains( key ) || ( params.contains( u"arguments"_s ) && !params.value( u"arguments"_s ).isObject() ) )
      result = toolResult( tr( "This tool call is unavailable, duplicated, or not permitted in the current Strata turn." ), true );
    else
    {
      mRequests.insert( key, fingerprint );
      const QScopedValueRollback<bool> guard( mExecuting, true );
      emit executingChanged( true );
      // Copy: cancellation can clear the member while a tool processes events.
      const Executor execute = mExecutor;
      result
        = execute( name, params.value( u"arguments"_s ).toObject(), u"strata_%1_%2"_s.arg( mRunId, QString::fromLatin1( QCryptographicHash::hash( key.toUtf8(), QCryptographicHash::Sha256 ).toHex() ) ) );
      emit executingChanged( false );
    }
    response.insert( u"result"_s, result );
    mResponses.insert( key, response );
    return response;
  }
  else
  {
    response.insert( u"error"_s, QJsonObject { { u"code"_s, -32601 }, { u"message"_s, u"Method not found"_s } } );
    return response;
  }
  response.insert( u"result"_s, result );
  return response;
}
