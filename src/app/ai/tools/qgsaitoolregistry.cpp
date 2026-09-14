/***************************************************************************
    qgsaitoolregistry.cpp
    ---------------------
    begin                : April 2026
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

#include "qgsaitoolregistry.h"

#include "qgsaiauditlog.h"
#include "qgsaimcpcalltool.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include "moc_qgsaitoolregistry.cpp"

using namespace Qt::StringLiterals;

namespace
{
  QString sha256Hex( const QByteArray &data )
  {
    return QString::fromLatin1( QCryptographicHash::hash( data, QCryptographicHash::Sha256 ).toHex() );
  }

  QStringList sortedKeys( const QJsonObject &object )
  {
    QStringList keys = object.keys();
    keys.sort( Qt::CaseInsensitive );
    return keys;
  }

  void auditToolExecution( const QgsAiTool *tool, const QJsonObject &args, const QgsAiToolResult &result )
  {
    if ( !tool )
      return;
    if ( !tool->requiresApproval() && tool->riskLevel() == QgsAiToolRiskLevel::Low )
      return;

    const QByteArray argsBytes = QJsonDocument( args ).toJson( QJsonDocument::Compact );
    QJsonObject metadata;
    metadata.insert( u"args_sha256"_s, sha256Hex( argsBytes ) );
    metadata.insert( u"args_bytes"_s, argsBytes.size() );
    metadata.insert( u"args_keys"_s, sortedKeys( args ).join( ',' ) );

    if ( result.success )
    {
      if ( result.output.isObject() )
      {
        const QJsonObject outputObject = result.output.toObject();
        const QByteArray outputBytes = QJsonDocument( outputObject ).toJson( QJsonDocument::Compact );
        metadata.insert( u"output_sha256"_s, sha256Hex( outputBytes ) );
        metadata.insert( u"output_bytes"_s, outputBytes.size() );
        metadata.insert( u"output_keys"_s, sortedKeys( outputObject ).join( ',' ) );
      }
      else
      {
        const QByteArray outputBytes = result.output.toVariant().toString().toUtf8();
        metadata.insert( u"output_sha256"_s, sha256Hex( outputBytes ) );
        metadata.insert( u"output_bytes"_s, outputBytes.size() );
        metadata.insert( u"output_kind"_s, result.output.isString() ? u"string"_s : u"value"_s );
      }
    }
    else
    {
      metadata.insert( u"error_sha256"_s, sha256Hex( result.errorMessage.toUtf8() ) );
      metadata.insert( u"error_bytes"_s, result.errorMessage.toUtf8().size() );
    }

    QgsAiAuditLog::appendToolEvent( u"execute"_s, tool->name(), QgsAiToolRiskLevelName( tool->riskLevel() ), result.success, metadata );
  }
} // namespace

QgsAiToolRegistry::QgsAiToolRegistry( QObject *parent )
  : QObject( parent )
{}

QgsAiToolRegistry::~QgsAiToolRegistry() = default;

bool QgsAiToolRegistry::registerTool( std::unique_ptr<QgsAiTool> tool )
{
  if ( !tool )
    return false;

  const QString toolName = tool->name();
  if ( toolName.isEmpty() || mTools.contains( toolName ) )
    return false;

  mTools.emplace( toolName, std::move( tool ) );
  emit toolRegistered( toolName );
  return true;
}

void QgsAiToolRegistry::setMcpCallProxy( std::unique_ptr<QgsAiMcpCallTool> proxy )
{
  mMcpProxy = std::move( proxy );
}

void QgsAiToolRegistry::setManagedMcpTools( const QList<QgsAiManagedMcpTool> &tools )
{
  mManagedMcpTools = tools;
}

const QgsAiManagedMcpTool *QgsAiToolRegistry::findManagedMcpTool( const QString &name ) const
{
  for ( const QgsAiManagedMcpTool &tool : mManagedMcpTools )
  {
    if ( tool.name == name )
      return &tool;
  }
  return nullptr;
}

bool QgsAiToolRegistry::mcpProxyAvailable() const
{
  return mMcpProxy && mMcpProxy->isAvailable();
}

QgsAiTool *QgsAiToolRegistry::find( const QString &name ) const
{
  const auto it = mTools.find( name );
  return it == mTools.end() ? nullptr : it->second.get();
}

QStringList QgsAiToolRegistry::toolNames() const
{
  QStringList names;
  names.reserve( static_cast<int>( mTools.size() ) );
  for ( const auto &entry : mTools )
    names.append( entry.first );
  return names;
}

QStringList QgsAiToolRegistry::availableToolNames() const
{
  QStringList names;
  names.reserve( static_cast<int>( mTools.size() ) );
  for ( const auto &entry : mTools )
  {
    const QgsAiTool *tool = entry.second.get();
    if ( tool && tool->isAvailable() )
      names.append( entry.first );
  }
  if ( mcpProxyAvailable() )
  {
    for ( const QgsAiManagedMcpTool &tool : mManagedMcpTools )
    {
      if ( tool.enabled && !tool.name.isEmpty() )
        names.append( tool.name );
    }
  }
  return names;
}

QMap<QString, QString> QgsAiToolRegistry::unavailableToolReasons( const QStringList &toolNames ) const
{
  const QSet<QString> filter( toolNames.begin(), toolNames.end() );
  QMap<QString, QString> reasons;
  for ( auto it = mTools.cbegin(); it != mTools.cend(); ++it )
  {
    if ( !filter.isEmpty() && !filter.contains( it->first ) )
      continue;

    const QgsAiTool *tool = it->second.get();
    if ( !tool || tool->isAvailable() )
      continue;

    QString reason = tool->availabilityReason().trimmed();
    if ( reason.isEmpty() )
      reason = u"runtime dependency or policy gate is not satisfied"_s;
    reasons.insert( it->first, reason );
  }
  return reasons;
}

QJsonArray QgsAiToolRegistry::schemasJson( const QStringList &allowedTools ) const
{
  return schemasJsonForFormat( WireFormat::AnthropicTools, allowedTools );
}

QJsonArray QgsAiToolRegistry::schemasJsonForFormat( WireFormat format, const QStringList &allowedTools ) const
{
  const QSet<QString> filter( allowedTools.begin(), allowedTools.end() );
  QJsonArray array;
  for ( auto it = mTools.cbegin(); it != mTools.cend(); ++it )
  {
    if ( !filter.isEmpty() && !filter.contains( it->first ) )
      continue;

    const QgsAiTool *tool = it->second.get();
    if ( !tool || !tool->isAvailable() )
      continue;

    QJsonObject entry;
    switch ( format )
    {
      case WireFormat::AnthropicTools:
        entry.insert( u"name"_s, tool->name() );
        entry.insert( u"description"_s, tool->description() );
        entry.insert( u"input_schema"_s, tool->schema() );
        break;

      case WireFormat::OpenAiResponses:
        entry.insert( u"type"_s, u"function"_s );
        entry.insert( u"name"_s, tool->name() );
        entry.insert( u"description"_s, tool->description() );
        entry.insert( u"parameters"_s, tool->schema() );
        break;

      case WireFormat::OpenAiChatCompletions:
      {
        QJsonObject function;
        function.insert( u"name"_s, tool->name() );
        function.insert( u"description"_s, tool->description() );
        function.insert( u"parameters"_s, tool->schema() );
        entry.insert( u"type"_s, u"function"_s );
        entry.insert( u"function"_s, function );
        break;
      }
    }
    array.append( entry );
  }
  if ( mcpProxyAvailable() )
  {
    for ( const QgsAiManagedMcpTool &tool : mManagedMcpTools )
    {
      if ( !tool.enabled || tool.name.isEmpty() )
        continue;
      if ( !filter.isEmpty() && !filter.contains( tool.name ) )
        continue;
      array.append( mcpSchemaEntry( tool, format ) );
    }
  }
  return array;
}

QJsonObject QgsAiToolRegistry::mcpSchemaEntry( const QgsAiManagedMcpTool &tool, WireFormat format ) const
{
  QJsonObject schema = tool.inputSchema;
  if ( schema.isEmpty() )
  {
    schema.insert( u"type"_s, u"object"_s );
    schema.insert( u"properties"_s, QJsonObject() );
  }
  QJsonObject entry;
  switch ( format )
  {
    case WireFormat::AnthropicTools:
      entry.insert( u"name"_s, tool.name );
      entry.insert( u"description"_s, tool.description );
      entry.insert( u"input_schema"_s, schema );
      break;
    case WireFormat::OpenAiResponses:
      entry.insert( u"type"_s, u"function"_s );
      entry.insert( u"name"_s, tool.name );
      entry.insert( u"description"_s, tool.description );
      entry.insert( u"parameters"_s, schema );
      break;
    case WireFormat::OpenAiChatCompletions:
    {
      QJsonObject function;
      function.insert( u"name"_s, tool.name );
      function.insert( u"description"_s, tool.description );
      function.insert( u"parameters"_s, schema );
      entry.insert( u"type"_s, u"function"_s );
      entry.insert( u"function"_s, function );
      break;
    }
  }
  return entry;
}

QgsAiToolResult QgsAiToolRegistry::execute( const QString &name, const QJsonObject &args ) const
{
  if ( name.startsWith( u"mcp__"_s ) )
  {
    if ( !mMcpProxy )
      return QgsAiToolResult::error( u"MCP gateway is not configured."_s );
    if ( !mMcpProxy->isAvailable() )
    {
      const QString reason = mMcpProxy->availabilityReason();
      return QgsAiToolResult::error( reason.isEmpty() ? u"MCP gateway is not available."_s : reason );
    }
    const QgsAiManagedMcpTool *definition = findManagedMcpTool( name );
    if ( !definition || !definition->enabled )
      return QgsAiToolResult::error( u"MCP tool is not enabled: %1"_s.arg( name ) );
    const QgsAiToolResult result = mMcpProxy->executeNamed( name, args );
    auditToolExecution( mMcpProxy.get(), args, result );
    return result;
  }

  QgsAiTool *tool = find( name );
  if ( !tool )
    return QgsAiToolResult::error( u"Unknown tool: %1"_s.arg( name ) );
  if ( !tool->isAvailable() )
  {
    const QString reason = tool->availabilityReason();
    return QgsAiToolResult::error( reason.isEmpty() ? u"Tool is not available: %1"_s.arg( name ) : reason );
  }
  const QgsAiToolResult result = tool->execute( args );
  auditToolExecution( tool, args, result );
  return result;
}

void QgsAiToolRegistry::clear()
{
  mTools.clear();
  mManagedMcpTools.clear();
}
