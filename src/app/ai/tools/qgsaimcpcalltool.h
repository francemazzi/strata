/***************************************************************************
    qgsaimcpcalltool.h
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

#ifndef QGSAIMCPCALLTOOL_H
#define QGSAIMCPCALLTOOL_H

#include "qgis_app.h"
#include "qgsaiwebsearchtool.h"

#include <QHash>
#include <QString>

using namespace Qt::StringLiterals;

class APP_EXPORT QgsAiMcpCallTool : public QgsAiWebSearchToolBase
{
  public:
    using QgsAiWebSearchToolBase::QgsAiWebSearchToolBase;

    QString name() const override { return u"mcp_call"_s; }
    QString description() const override;
    QJsonObject schema() const override;
    QgsAiToolResult execute( const QJsonObject &args ) override;

    /**
     * Runs an MCP tool through the gateway. A \a mutating call carries an idempotency key built
     * from \a callId: when its outcome is unknown (Stop, timeout), the model is told so, and
     * repeating it with the same arguments reuses the key, so the gateway does not run it twice.
     */
    QgsAiToolResult executeNamed( const QString &name, const QJsonObject &args, bool mutating = false, const QString &callId = QString() ) const;

    void setRequestTimeoutMs( int timeoutMs ) { mRequestTimeoutMs = timeoutMs; }

  private:
    int mRequestTimeoutMs = 20000;
    //! Idempotency keys of the mutating calls whose outcome is unknown, by tool name and arguments.
    mutable QHash<QString, QString> mUncertainCalls;
};

#endif // QGSAIMCPCALLTOOL_H
