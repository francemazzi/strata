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

#include <QJsonArray>
#include <QJsonObject>

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

QgsAiToolResult QgsAiMcpCallTool::executeNamed( const QString &name, const QJsonObject &args ) const
{
  QJsonObject body;
  body.insert( u"name"_s, name );
  body.insert( u"arguments"_s, args );
  QgsAiToolResult result = postSearch( u"/v1/tools/mcp-call"_s, body );
  if ( !result.success || !result.output.isObject() )
    return result;

  const QJsonObject root = result.output.toObject();
  const QJsonValue output = root.value( u"output"_s );
  if ( output.isUndefined() || output.isNull() )
    return result;
  return QgsAiToolResult::ok( output );
}
