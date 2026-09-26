/***************************************************************************
    qgsaidatabasetools.h
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

#ifndef QGSAIDATABASETOOLS_H
#define QGSAIDATABASETOOLS_H

#include "qgis_app.h"
#include "qgsaitool.h"

#include <QString>

using namespace Qt::StringLiterals;

class QgsProject;

enum class QgsAiSqlStatementKind
{
  ReadOnly,
  Mutation,
  Invalid
};

struct APP_EXPORT QgsAiSqlClassification
{
    QgsAiSqlStatementKind kind = QgsAiSqlStatementKind::Invalid;
    QString error;
};

/**
 * Classifies a SQL string for the PostGIS chat tools.
 *
 * Read-only: a single SELECT / WITH…SELECT / EXPLAIN (without ANALYZE of a write) / SHOW / VALUES / TABLE statement.
 * Mutation: DDL/DML, SELECT INTO, SELECT FOR UPDATE/SHARE, EXPLAIN ANALYZE of a write.
 * Invalid: empty SQL or more than one statement.
 */
APP_EXPORT QgsAiSqlClassification classifyAiSql( const QString &sql );

/**
 * list_database_connections: names of PostgreSQL connections saved in QGIS settings.
 * Never returns passwords or full URIs.
 */
class APP_EXPORT QgsAiListDatabaseConnectionsTool : public QgsAiTool
{
  public:
    QString name() const override { return u"list_database_connections"_s; }
    QString description() const override;
    QJsonObject schema() const override;
    QgsAiToolResult execute( const QJsonObject &args ) override;
};

/**
 * describe_database_schema: live schemas/tables/columns on a saved PostgreSQL connection.
 */
class APP_EXPORT QgsAiDescribeDatabaseSchemaTool : public QgsAiTool
{
  public:
    QString name() const override { return u"describe_database_schema"_s; }
    QString description() const override;
    QJsonObject schema() const override;
    QgsAiToolResult execute( const QJsonObject &args ) override;
};

/**
 * query_sql / execute_sql: one implementation, two registry names.
 * query_sql is read-only (Ask). execute_sql is mutating (approval, High).
 */
class APP_EXPORT QgsAiDatabaseSqlTool : public QgsAiTool
{
  public:
    QgsAiDatabaseSqlTool( QgsProject *project, bool readOnly );

    QString name() const override;
    QString description() const override;
    QJsonObject schema() const override;
    QgsAiToolResult execute( const QJsonObject &args ) override;
    bool requiresApproval() const override { return !mReadOnly; }
    //! Database writes have no rollback in Strata.
    bool canBeUndone() const override { return mReadOnly; }
    QgsAiToolRiskLevel riskLevel() const override { return mReadOnly ? QgsAiToolRiskLevel::Low : QgsAiToolRiskLevel::High; }

  private:
    QgsProject *mProject = nullptr;
    bool mReadOnly = true;
};

/**
 * export_layer_to_postgis: copies a project vector layer into a PostgreSQL table.
 */
class APP_EXPORT QgsAiExportLayerToPostgisTool : public QgsAiTool
{
  public:
    explicit QgsAiExportLayerToPostgisTool( QgsProject *project );

    QString name() const override { return u"export_layer_to_postgis"_s; }
    QString description() const override;
    QJsonObject schema() const override;
    QgsAiToolResult execute( const QJsonObject &args ) override;
    bool requiresApproval() const override { return true; }
    //! Writing (or replacing) a database table has no rollback in Strata.
    bool canBeUndone() const override { return false; }
    QgsAiToolRiskLevel riskLevel() const override { return QgsAiToolRiskLevel::High; }

  private:
    QgsProject *mProject = nullptr;
};

#endif // QGSAIDATABASETOOLS_H
