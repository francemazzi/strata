/***************************************************************************
    qgsaitoolregistry.h
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

#ifndef QGSAITOOLREGISTRY_H
#define QGSAITOOLREGISTRY_H

#include <map>
#include <memory>

#include "qgis_app.h"
#include "qgsaiagentpolicy.h"
#include "qgsaitool.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>

class QgsAiMcpCallTool;

class APP_EXPORT QgsAiToolRegistry : public QObject
{
    Q_OBJECT

  public:
    explicit QgsAiToolRegistry( QObject *parent = nullptr );
    ~QgsAiToolRegistry() override;

    //! Registers \a tool. Takes ownership. Returns false if a tool with same name already exists.
    bool registerTool( std::unique_ptr<QgsAiTool> tool );

    //! Gateway used for namespaced mcp__* tools. Takes ownership.
    void setMcpCallProxy( std::unique_ptr<QgsAiMcpCallTool> proxy );

    //! Replaces the curated MCP tools advertised from the managed agent policy.
    void setManagedMcpTools( const QList<QgsAiManagedMcpTool> &tools );

    //! Returns the policy MCP tool with \a name, or nullptr.
    const QgsAiManagedMcpTool *findManagedMcpTool( const QString &name ) const;

    //! Returns pointer to registered tool, or nullptr.
    QgsAiTool *find( const QString &name ) const;

    //! Returns all registered tool names.
    QStringList toolNames() const;

    //! Returns registered tool names whose runtime dependencies are currently available.
    QStringList availableToolNames() const;

    //! Returns reasons for registered tools which are currently unavailable.
    QMap<QString, QString> unavailableToolReasons( const QStringList &toolNames = QStringList() ) const;

    int count() const { return mTools.size(); }

    /**
     * Returns a JSON array describing registered tools in Anthropic Claude tool-use format:
     * `[{ "name": "...", "description": "...", "input_schema": {...} }, ...]`.
     * If \a allowedTools is non-empty, only tools whose name is in the set are included.
     * The router converts this format to provider-specific payloads.
     */
    QJsonArray schemasJson( const QStringList &allowedTools = QStringList() ) const;

    enum class WireFormat
    {
      AnthropicTools,       //!< `[{name, description, input_schema}]` for Anthropic Messages API
      OpenAiResponses,      //!< `[{type:"function", name, description, parameters}]` for OpenAI Responses API
      OpenAiChatCompletions //!< `[{type:"function", function:{name, description, parameters}}]` for OpenAI Chat Completions API
    };

    /**
     * Returns the registered tool schemas in the wire format expected by \a format.
     * Use this when building the request payload for a specific provider.
     */
    QJsonArray schemasJsonForFormat( WireFormat format, const QStringList &allowedTools = QStringList() ) const;

    /**
     * Looks up the tool by \a name and runs it with \a args. If the tool is missing
     * the result is `success=false` with an actionable error message.
     */
    //! Runs a tool. \a callId, the model's tool call id, keys the gateway calls that change data.
    QgsAiToolResult execute( const QString &name, const QJsonObject &args, const QString &callId = QString() ) const;

    //! Removes all registered tools.
    void clear();

  signals:
    void toolRegistered( const QString &name );

  private:
    QJsonObject mcpSchemaEntry( const QgsAiManagedMcpTool &tool, WireFormat format ) const;
    bool mcpProxyAvailable() const;

    std::map<QString, std::unique_ptr<QgsAiTool>> mTools;
    std::unique_ptr<QgsAiMcpCallTool> mMcpProxy;
    QList<QgsAiManagedMcpTool> mManagedMcpTools;
};

#endif // QGSAITOOLREGISTRY_H
