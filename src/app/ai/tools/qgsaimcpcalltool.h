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

class APP_EXPORT QgsAiMcpCallTool : public QgsAiWebSearchToolBase
{
  public:
    using QgsAiWebSearchToolBase::QgsAiWebSearchToolBase;

    QString name() const override { return u"mcp_call"_s; }
    QString description() const override;
    QJsonObject schema() const override;
    QgsAiToolResult execute( const QJsonObject &args ) override;
    QgsAiToolResult executeNamed( const QString &name, const QJsonObject &args ) const;
};

#endif // QGSAIMCPCALLTOOL_H
