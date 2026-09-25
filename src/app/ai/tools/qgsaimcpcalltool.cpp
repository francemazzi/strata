/***************************************************************************
    qgsaimcpcalltool.cpp
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

#include "qgsaimcpcalltool.h"

#include "qgsaitoolschemautil.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUuid>

using namespace Qt::StringLiterals;

QString QgsAiMcpCallTool::description() const
{
  return u"Executes a curated MCP tool through the Strata Plan gateway. Tool names are mcp__<server>__<tool>."_s;
}

QJsonObject QgsAiMcpCallTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"name"_s, prop( u"string"_s, u"Namespaced MCP tool name, mcp__<server>__<tool>."_s ) );
  properties.insert( u"arguments"_s, QJsonObject { { u"type"_s, u"object"_s } } );
  return schemaObject( properties, QJsonArray { u"name"_s } );
}

QgsAiToolResult QgsAiMcpCallTool::execute( const QJsonObject &args )
{
  const QString name = args.value( u"name"_s ).toString().trimmed();
  if ( name.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'name' is required."_s );
  const QJsonObject nestedArgs = args.value( u"arguments"_s ).toObject();
  return executeNamed( name, nestedArgs );
}

QgsAiToolResult QgsAiMcpCallTool::executeNamed( const QString &name, const QJsonObject &args, bool mutating, const QString &callId ) const
{
  QJsonObject body;
  body.insert( u"name"_s, name );
  body.insert( u"arguments"_s, args );

  // Only calls that change data need a key: running a read twice is harmless.
  QString fingerprint;
  QString idempotencyKey;
  if ( mutating )
  {
    const QByteArray canonical = name.toUtf8() + '\n' + QJsonDocument( args ).toJson( QJsonDocument::Compact );
    fingerprint = QString::fromLatin1( QCryptographicHash::hash( canonical, QCryptographicHash::Sha256 ).toHex().left( 24 ) );
    // A retry after an unknown outcome reuses the first key; otherwise the tool call id, with the
    // arguments' hash so two providers' "call_0" never collide.
    idempotencyKey = mUncertainCalls.value( fingerprint );
    if ( idempotencyKey.isEmpty() )
      idempotencyKey = u"strata-%1-%2"_s.arg( callId.isEmpty() ? QUuid::createUuid().toString( QUuid::Id128 ) : callId.left( 100 ), fingerprint );
    body.insert( u"idempotencyKey"_s, idempotencyKey );
  }

  bool outcomeUnknown = false;
  QgsAiToolResult result = postSearch( u"/v1/tools/mcp-call"_s, body, mRequestTimeoutMs, idempotencyKey, &outcomeUnknown );
  if ( mutating )
  {
    if ( outcomeUnknown && !result.success )
    {
      // "Canceled" or "timed out" reads as "try again" to a model, and a retry would change the data twice.
      mUncertainCalls.insert( fingerprint, idempotencyKey );
      const QString message = u"Outcome uncertain: %1 changes data and the request %2 before the gateway answered, so the change may have happened anyway. "
                              "Do not simply repeat it: first check whether the change is there. Repeating it with the same arguments returns the first outcome instead of running it twice."_s
                                .arg( name, result.canceled ? u"was stopped"_s : u"got no answer"_s );
      QgsAiToolResult uncertain = result.canceled ? QgsAiToolResult::canceledResult( message ) : QgsAiToolResult::error( message );
      uncertain.output = QJsonObject { { u"status"_s, u"outcome_uncertain"_s }, { u"retryable"_s, false } };
      return uncertain;
    }
    mUncertainCalls.remove( fingerprint );
  }
  if ( !result.success || !result.output.isObject() )
    return result;

  const QJsonObject root = result.output.toObject();
  const QJsonValue output = root.value( u"output"_s );
  if ( output.isUndefined() || output.isNull() )
    return result;
  return QgsAiToolResult::ok( output );
}
