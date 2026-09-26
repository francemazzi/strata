/***************************************************************************
    qgsaidatabasetools.cpp
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

#include "qgsaidatabasetools.h"

#include <algorithm>
#include <memory>

#include "qgsabstractdatabaseproviderconnection.h"
#include "qgsaitaskrunner.h"
#include "qgsaitoolschemautil.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsdatasourceuri.h"
#include "qgsexception.h"
#include "qgsfeature.h"
#include "qgsfeatureiterator.h"
#include "qgsfeaturesink.h"
#include "qgsfeedback.h"
#include "qgsfields.h"
#include "qgsgeometry.h"
#include "qgsproject.h"
#include "qgsprovidermetadata.h"
#include "qgsproviderregistry.h"
#include "qgssettings.h"
#include "qgsvectorlayer.h"
#include "qgsvectorlayerexporter.h"
#include "qgsvectorlayerfeatureiterator.h"
#include "qgswkbtypes.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QThread>
#include <QUuid>
#include <QVariant>

using namespace Qt::StringLiterals;

namespace
{
  constexpr int MAX_DB_TOOL_RESULT_BYTES = 48000;
  constexpr int MAX_CELL_CHARS = 240;
  constexpr int MAX_WKT_CHARS = 400;
  constexpr int DEFAULT_SQL_LIMIT = 50;
  constexpr int MAX_SQL_LIMIT = 500;

  const QSet<QString> GEOM_COLUMN_NAMES = { u"geom"_s, u"geometry"_s, u"the_geom"_s, u"wkb_geometry"_s, u"shape"_s, u"geog"_s, u"geography"_s };

  const QSet<QString> WRITE_KEYWORDS = { u"INSERT"_s, u"DELETE"_s, u"MERGE"_s,   u"CREATE"_s,  u"DROP"_s,    u"ALTER"_s, u"GRANT"_s, u"REVOKE"_s, u"TRUNCATE"_s, u"VACUUM"_s,   u"CALL"_s,   u"COPY"_s,
                                         u"LISTEN"_s, u"NOTIFY"_s, u"REFRESH"_s, u"REINDEX"_s, u"CLUSTER"_s, u"LOAD"_s,  u"DO"_s,    u"LOCK"_s,   u"COMMENT"_s,  u"SECURITY"_s, u"EXECUTE"_s };

  //! Functions that act on the server even inside a SELECT: they need execute_sql and approval.
  const QSet<QString> SIDE_EFFECT_FUNCTIONS = {
    u"PG_TERMINATE_BACKEND"_s,
    u"PG_CANCEL_BACKEND"_s,
    u"PG_RELOAD_CONF"_s,
    u"PG_ROTATE_LOGFILE"_s,
    u"PG_PROMOTE"_s,
    u"PG_SWITCH_WAL"_s,
    u"PG_CREATE_RESTORE_POINT"_s,
    u"PG_ADVISORY_LOCK"_s,
    u"PG_ADVISORY_XACT_LOCK"_s,
    u"PG_TRY_ADVISORY_LOCK"_s,
    u"SETVAL"_s,
    u"NEXTVAL"_s,
    u"SET_CONFIG"_s,
    u"LO_IMPORT"_s,
    u"LO_EXPORT"_s,
    u"LO_UNLINK"_s,
    u"LO_CREATE"_s,
    u"DBLINK_EXEC"_s,
    u"DBLINK"_s,
    u"PG_READ_FILE"_s,
    u"PG_READ_BINARY_FILE"_s,
    u"PG_LS_DIR"_s,
    u"PG_FILE_WRITE"_s,
    u"PG_STAT_RESET"_s,
  };

  struct LayerRollbackEntry
  {
      QString layerId;
  };

  QHash<QString, LayerRollbackEntry> &layerRollbackStore()
  {
    static QHash<QString, LayerRollbackEntry> store;
    return store;
  }

  QString storeLayerRollback( const QString &layerId )
  {
    const QString token = QUuid::createUuid().toString( QUuid::WithoutBraces );
    layerRollbackStore().insert( token, LayerRollbackEntry { layerId } );
    return token;
  }

  QJsonObject rollbackJson( const QString &token, const QString &action )
  {
    QJsonObject rollback;
    rollback.insert( u"token"_s, token );
    rollback.insert( u"action"_s, action );
    rollback.insert( u"volatile"_s, true );
    return rollback;
  }

  QgsAiToolResult rollbackLayerAddition( QgsProject *project, const QString &token )
  {
    if ( !layerRollbackStore().contains( token ) )
      return QgsAiToolResult::error( u"Unknown or expired rollback token."_s );
    const LayerRollbackEntry entry = layerRollbackStore().take( token );
    if ( !project )
      return QgsAiToolResult::error( u"No active QgsProject available."_s );
    if ( !project->mapLayer( entry.layerId ) )
    {
      QJsonObject output;
      output.insert( u"status"_s, u"already_removed"_s );
      output.insert( u"rollback_token"_s, token );
      return QgsAiToolResult::ok( output );
    }

    project->removeMapLayer( entry.layerId );
    QJsonObject diff;
    diff.insert( u"summary"_s, u"Removed SQL query layer added by execute_sql."_s );
    diff.insert( u"layer_id"_s, entry.layerId );
    QJsonObject output;
    output.insert( u"status"_s, u"rolled_back"_s );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"diff"_s, diff );
    return QgsAiToolResult::ok( output );
  }

  QStringList savedPostgresConnectionNames()
  {
    QgsSettings settings;
    settings.beginGroup( u"PostgreSQL/connections"_s );
    return settings.childGroups();
  }

  QString missingConnectionError( const QString &name )
  {
    const QStringList names = savedPostgresConnectionNames();
    if ( names.isEmpty() )
      return u"No saved PostgreSQL connections. Create one in the QGIS Browser (PostGIS > New Connection) and save it before using database tools."_s;
    if ( name.isEmpty() )
      return u"Argument 'connection_name' is required. Saved names: %1"_s.arg( names.join( ", "_L1 ) );
    return u"No saved PostgreSQL connection named '%1'. Saved names: %2"_s.arg( name, names.join( ", "_L1 ) );
  }

  std::unique_ptr<QgsAbstractDatabaseProviderConnection> openPostgresConnection( const QString &name, QString &error )
  {
    if ( name.trimmed().isEmpty() )
    {
      error = missingConnectionError( QString() );
      return nullptr;
    }

    QgsProviderMetadata *metadata = QgsProviderRegistry::instance()->providerMetadata( u"postgres"_s );
    if ( !metadata )
    {
      error = u"PostgreSQL provider is not available in this QGIS build."_s;
      return nullptr;
    }

    if ( !savedPostgresConnectionNames().contains( name ) )
    {
      error = missingConnectionError( name );
      return nullptr;
    }

    try
    {
      return std::unique_ptr<QgsAbstractDatabaseProviderConnection>( static_cast<QgsAbstractDatabaseProviderConnection *>( metadata->createConnection( name ) ) );
    }
    catch ( const QgsProviderConnectionException &ex )
    {
      error = u"Could not open PostgreSQL connection '%1': %2"_s.arg( name, ex.what() );
      return nullptr;
    }
  }

  QString stripSqlLiteralsAndComments( const QString &sql )
  {
    QString out;
    out.reserve( sql.size() );
    const int n = sql.size();
    int i = 0;
    while ( i < n )
    {
      const QChar c = sql.at( i );
      if ( c == '-' && i + 1 < n && sql.at( i + 1 ) == '-' )
      {
        while ( i < n && sql.at( i ) != '\n' )
          ++i;
        out.append( ' ' );
        continue;
      }
      if ( c == '/' && i + 1 < n && sql.at( i + 1 ) == '*' )
      {
        i += 2;
        while ( i + 1 < n && !( sql.at( i ) == '*' && sql.at( i + 1 ) == '/' ) )
          ++i;
        i = std::min( i + 2, n );
        out.append( ' ' );
        continue;
      }
      if ( c == '\'' )
      {
        ++i;
        while ( i < n )
        {
          if ( sql.at( i ) == '\'' )
          {
            if ( i + 1 < n && sql.at( i + 1 ) == '\'' )
            {
              i += 2;
              continue;
            }
            ++i;
            break;
          }
          ++i;
        }
        out.append( ' ' );
        continue;
      }
      if ( c == '"' )
      {
        ++i;
        while ( i < n )
        {
          if ( sql.at( i ) == '"' )
          {
            if ( i + 1 < n && sql.at( i + 1 ) == '"' )
            {
              i += 2;
              continue;
            }
            ++i;
            break;
          }
          ++i;
        }
        out.append( ' ' );
        continue;
      }
      if ( c == '$' )
      {
        int j = i + 1;
        while ( j < n && ( sql.at( j ).isLetterOrNumber() || sql.at( j ) == '_' ) )
          ++j;
        if ( j < n && sql.at( j ) == '$' )
        {
          const QString tag = sql.mid( i, j - i + 1 );
          const int end = sql.indexOf( tag, j + 1 );
          if ( end >= 0 )
          {
            i = end + tag.size();
            out.append( ' ' );
            continue;
          }
        }
      }
      out.append( c );
      ++i;
    }
    return out;
  }

  QStringList sqlTokens( const QString &stripped )
  {
    const QStringList raw = stripped.split( QRegularExpression( u"[^A-Za-z0-9_]+"_s ), Qt::SkipEmptyParts );
    QStringList tokens;
    tokens.reserve( raw.size() );
    for ( const QString &token : raw )
      tokens.append( token.toUpper() );
    return tokens;
  }

  bool containsWriteKeyword( const QStringList &tokens )
  {
    for ( const QString &token : tokens )
    {
      if ( WRITE_KEYWORDS.contains( token ) )
        return true;
    }
    return false;
  }

  bool hasSelectInto( const QStringList &tokens )
  {
    for ( int i = 0; i + 1 < tokens.size(); ++i )
    {
      if ( tokens.at( i ) == "SELECT"_L1 && tokens.at( i + 1 ) == "INTO"_L1 )
        return true;
    }
    return false;
  }

  bool hasForUpdateOrShare( const QStringList &tokens )
  {
    for ( int i = 0; i + 1 < tokens.size(); ++i )
    {
      if ( tokens.at( i ) == "FOR"_L1 && ( tokens.at( i + 1 ) == "UPDATE"_L1 || tokens.at( i + 1 ) == "SHARE"_L1 ) )
        return true;
    }
    return false;
  }

  bool isSystemSchema( const QString &schema )
  {
    return schema.startsWith( u"pg_"_s, Qt::CaseInsensitive ) || schema.compare( u"information_schema"_s, Qt::CaseInsensitive ) == 0;
  }

  bool isGeometryColumnName( const QString &name )
  {
    return GEOM_COLUMN_NAMES.contains( name.trimmed().toLower() );
  }

  QgsGeometry geometryFromVariant( const QVariant &value )
  {
    QgsGeometry geometry;
    if ( value.userType() == QMetaType::QByteArray )
    {
      geometry.fromWkb( value.toByteArray() );
      return geometry;
    }
    const QString text = value.toString().trimmed();
    if ( text.isEmpty() )
      return geometry;
    if ( text.size() >= 8 && text.front().isDigit() )
    {
      geometry.fromWkb( QByteArray::fromHex( text.toLatin1() ) );
      if ( !geometry.isNull() )
        return geometry;
    }
    return QgsGeometry::fromWkt( text );
  }

  QJsonValue jsonFromSqlValue( const QVariant &value, const QString &columnName, bool includeGeometry )
  {
    if ( !value.isValid() || value.isNull() )
      return QJsonValue();

    const bool geometryColumn = isGeometryColumnName( columnName );
    if ( geometryColumn || value.userType() == QMetaType::QByteArray )
    {
      if ( !includeGeometry )
        return u"<geometry>"_s;
      const QgsGeometry geometry = geometryFromVariant( value );
      if ( geometry.isNull() )
        return u"<geometry>"_s;
      QString wkt = geometry.asWkt();
      if ( wkt.size() > MAX_WKT_CHARS )
        wkt = wkt.left( MAX_WKT_CHARS ) + u"…"_s;
      return wkt;
    }

    if ( value.userType() == QMetaType::QString || value.userType() == QMetaType::QByteArray )
    {
      QString text = value.toString();
      if ( text.size() > MAX_CELL_CHARS )
        text = text.left( MAX_CELL_CHARS ) + u"…"_s;
      return text;
    }

    const QJsonValue json = QJsonValue::fromVariant( value );
    if ( json.isUndefined() )
    {
      QString text = value.toString();
      if ( text.size() > MAX_CELL_CHARS )
        text = text.left( MAX_CELL_CHARS ) + u"…"_s;
      return text;
    }
    if ( json.isString() && json.toString().size() > MAX_CELL_CHARS )
      return QJsonValue( json.toString().left( MAX_CELL_CHARS ) + u"…"_s );
    return json;
  }

  QStringList tableFlagNames( const QgsAbstractDatabaseProviderConnection::TableFlags &flags )
  {
    QStringList names;
    if ( flags.testFlag( QgsAbstractDatabaseProviderConnection::TableFlag::Aspatial ) )
      names.append( u"aspatial"_s );
    if ( flags.testFlag( QgsAbstractDatabaseProviderConnection::TableFlag::Vector ) )
      names.append( u"vector"_s );
    if ( flags.testFlag( QgsAbstractDatabaseProviderConnection::TableFlag::Raster ) )
      names.append( u"raster"_s );
    if ( flags.testFlag( QgsAbstractDatabaseProviderConnection::TableFlag::View ) )
      names.append( u"view"_s );
    if ( flags.testFlag( QgsAbstractDatabaseProviderConnection::TableFlag::MaterializedView ) )
      names.append( u"materialized_view"_s );
    if ( flags.testFlag( QgsAbstractDatabaseProviderConnection::TableFlag::Foreign ) )
      names.append( u"foreign"_s );
    return names;
  }

  QJsonObject tablePropertyJson( const QgsAbstractDatabaseProviderConnection::TableProperty &property )
  {
    QJsonObject object;
    object.insert( u"schema"_s, property.schema() );
    object.insert( u"name"_s, property.tableName() );
    object.insert( u"flags"_s, QJsonArray::fromStringList( tableFlagNames( property.flags() ) ) );
    if ( !property.geometryColumn().isEmpty() )
      object.insert( u"geometry_column"_s, property.geometryColumn() );
    QJsonArray geometryTypes;
    QJsonArray crsIds;
    const auto geometryColumnTypes = property.geometryColumnTypes();
    for ( const QgsAbstractDatabaseProviderConnection::TableProperty::GeometryColumnType &columnType : geometryColumnTypes )
    {
      geometryTypes.append( QgsWkbTypes::displayString( columnType.wkbType ) );
      if ( columnType.crs.isValid() )
        crsIds.append( columnType.crs.authid() );
    }
    if ( !geometryTypes.isEmpty() )
      object.insert( u"geometry_type"_s, geometryTypes.size() == 1 ? geometryTypes.at( 0 ) : QJsonValue( geometryTypes ) );
    if ( !crsIds.isEmpty() )
      object.insert( u"crs"_s, crsIds.size() == 1 ? crsIds.at( 0 ) : QJsonValue( crsIds ) );
    if ( !property.primaryKeyColumns().isEmpty() )
      object.insert( u"primary_key"_s, QJsonArray::fromStringList( property.primaryKeyColumns() ) );
    return object;
  }

  bool outputExceedsCap( const QJsonObject &output )
  {
    return QJsonDocument( output ).toJson( QJsonDocument::Compact ).size() > MAX_DB_TOOL_RESULT_BYTES;
  }

  //! Longest a query may run on the server, in seconds (strata/ai/sql_timeout_s).
  int databaseToolsTimeoutSeconds( bool write )
  {
    const int configured = QgsSettings().value( u"strata/ai/sql_timeout_s"_s, 30 ).toInt();
    // Approved writes may legitimately take longer than a look at the data.
    return std::max( 1, write ? std::max( configured, 300 ) : configured );
  }

  /**
   * The statement actually sent to PostgreSQL. In one PQexec call, the statements share an implicit
   * transaction that ends with the call, so nothing leaks to the pooled connection: a read-only
   * statement runs READ ONLY, every statement has a server-side timeout, and a SELECT is wrapped
   * so that the server returns at most \a maxRows rows.
   */
  QString databaseToolsServerSql( const QString &sql, bool readOnly, bool wrapWithLimit, int maxRows, int timeoutSeconds )
  {
    QString body = sql.trimmed();
    while ( body.endsWith( ';' ) )
      body = body.chopped( 1 ).trimmed();
    QString statement = readOnly ? u"SET TRANSACTION READ ONLY; "_s : QString();
    statement += u"SET LOCAL statement_timeout = %1; "_s.arg( timeoutSeconds * 1000 );
    // New lines keep a trailing line comment of the query from swallowing the wrapper.
    if ( wrapWithLimit )
      statement += u"SELECT * FROM (\n%1\n) AS strata_query LIMIT %2"_s.arg( body ).arg( maxRows );
    else
      statement += body;
    return statement;
  }

  //! First keyword of a statement, uppercase, after opening parentheses.
  QString databaseToolsFirstKeyword( const QString &sql )
  {
    const QStringList tokens = sqlTokens( stripSqlLiteralsAndComments( sql ) );
    return tokens.isEmpty() ? QString() : tokens.constFirst();
  }

  /**
   * TRUE for the statements PostgreSQL refuses inside a transaction block (VACUUM, CREATE INDEX
   * CONCURRENTLY, CREATE DATABASE…): they must be sent alone, without the server-side timeout.
   */
  bool databaseToolsRunsOutsideTransaction( const QString &sql )
  {
    const QStringList tokens = sqlTokens( stripSqlLiteralsAndComments( sql ) );
    if ( tokens.isEmpty() )
      return false;
    const QString &first = tokens.constFirst();
    const QString second = tokens.size() > 1 ? tokens.at( 1 ) : QString();
    if ( first == "VACUUM"_L1 || tokens.contains( u"CONCURRENTLY"_s ) )
      return true;
    if ( ( first == "CREATE"_L1 || first == "DROP"_L1 || first == "ALTER"_L1 ) && ( second == "DATABASE"_L1 || second == "TABLESPACE"_L1 || second == "SYSTEM"_L1 || second == "SUBSCRIPTION"_L1 ) )
      return true;
    if ( first == "REINDEX"_L1 && ( second == "SYSTEM"_L1 || second == "DATABASE"_L1 ) )
      return true;
    return first == "CLUSTER"_L1 && tokens.size() == 1;
  }

  //! TRUE when a trailing comment hides the final semicolon: wrapping such a query would break it.
  bool databaseToolsEndsWithHiddenSemicolon( const QString &sql )
  {
    QString body = sql.trimmed();
    while ( body.endsWith( ';' ) )
      body = body.chopped( 1 ).trimmed();
    return stripSqlLiteralsAndComments( body ).trimmed().endsWith( ';' );
  }

  QJsonObject sqlResultJson( QgsAbstractDatabaseProviderConnection::QueryResult &result, const QStringList &columns, int offset, int limit, bool includeGeometry )
  {
    QJsonArray rows;
    int matched = 0;
    bool truncated = false;
    qsizetype resultBytes = 16;
    while ( result.hasNextRow() )
    {
      const QList<QVariant> values = result.nextRow();
      if ( matched++ < offset )
        continue;
      if ( static_cast<int>( rows.size() ) >= limit )
      {
        truncated = true;
        break;
      }

      QJsonObject row;
      for ( int i = 0; i < columns.size(); ++i )
      {
        const QString column = columns.at( i );
        const QVariant value = i < values.size() ? values.at( i ) : QVariant();
        if ( isGeometryColumnName( column ) && !includeGeometry )
          continue;
        row.insert( column, jsonFromSqlValue( value, column, includeGeometry ) );
      }

      // A running total: serializing every row again for each new one grew quadratically.
      resultBytes += QJsonDocument( row ).toJson( QJsonDocument::Compact ).size() + 1;
      if ( resultBytes > MAX_DB_TOOL_RESULT_BYTES )
      {
        truncated = true;
        break;
      }
      rows.append( row );
    }

    QJsonObject output;
    output.insert( u"columns"_s, QJsonArray::fromStringList( columns ) );
    output.insert( u"rows"_s, rows );
    output.insert( u"returned_count"_s, rows.size() );
    output.insert( u"offset"_s, offset );
    output.insert( u"limit"_s, limit );
    if ( truncated )
    {
      output.insert( u"truncated"_s, true );
      output.insert( u"next_offset"_s, offset + rows.size() );
      output.insert( u"note"_s, u"Result size was capped. Request a smaller page or omit geometry."_s );
    }
    return output;
  }

  int sqlLimitFromArgs( const QJsonObject &args )
  {
    return std::clamp( args.value( u"limit"_s ).toInt( DEFAULT_SQL_LIMIT ), 1, MAX_SQL_LIMIT );
  }

  int sqlOffsetFromArgs( const QJsonObject &args )
  {
    return std::max( 0, args.value( u"offset"_s ).toInt( 0 ) );
  }

  QString databaseToolsQuotedName( const QString &schema, const QString &table )
  {
    auto quoted = []( QString identifier ) { return u"\"%1\""_s.arg( identifier.replace( '"', "\"\""_L1 ) ); };
    return u"%1.%2"_s.arg( quoted( schema ), quoted( table ) );
  }

  QString sanitizeTableName( const QString &name )
  {
    QString table = name.trimmed();
    table.replace( '.', '_' );
    table.replace( ' ', QString() );
    return table.right( 63 );
  }
} // namespace

QgsAiSqlClassification classifyAiSql( const QString &sql )
{
  QgsAiSqlClassification classification;
  const QString stripped = stripSqlLiteralsAndComments( sql );
  QString body = stripped.trimmed();
  if ( body.isEmpty() )
  {
    classification.error = u"SQL is empty."_s;
    return classification;
  }
  if ( body.endsWith( ';' ) )
    body.chop( 1 );
  body = body.trimmed();
  if ( body.contains( ';' ) )
  {
    classification.error = u"Only one SQL statement is allowed per call. Split additional statements into separate tool calls."_s;
    return classification;
  }

  const QStringList tokens = sqlTokens( body );
  if ( tokens.isEmpty() )
  {
    classification.error = u"SQL is empty."_s;
    return classification;
  }

  QStringList working = tokens;
  while ( !working.isEmpty() && working.constFirst() == "("_L1 )
    working.removeFirst();
  if ( working.isEmpty() )
  {
    classification.error = u"SQL is empty."_s;
    return classification;
  }

  const QString first = working.constFirst();
  const bool explainAnalyze = first == "EXPLAIN"_L1 && working.size() > 1 && working.at( 1 ) == "ANALYZE"_L1;
  // A SELECT can end sessions, move sequences or read server files: not a read.
  const bool sideEffects = std::any_of( working.cbegin(), working.cend(), []( const QString &token ) { return SIDE_EFFECT_FUNCTIONS.contains( token ); } );
  const bool hasWrite = containsWriteKeyword( working ) || working.contains( u"UPDATE"_s ) || sideEffects;
  const bool writeLock = hasSelectInto( working ) || hasForUpdateOrShare( working );

  if ( first == "SELECT"_L1 )
  {
    if ( writeLock || containsWriteKeyword( working ) || sideEffects )
    {
      classification.kind = QgsAiSqlStatementKind::Mutation;
      return classification;
    }
    classification.kind = QgsAiSqlStatementKind::ReadOnly;
    return classification;
  }
  if ( first == "WITH"_L1 )
  {
    classification.kind = ( hasWrite || writeLock ) ? QgsAiSqlStatementKind::Mutation : QgsAiSqlStatementKind::ReadOnly;
    return classification;
  }
  if ( first == "EXPLAIN"_L1 )
  {
    if ( explainAnalyze && ( hasWrite || writeLock ) )
    {
      classification.kind = QgsAiSqlStatementKind::Mutation;
      return classification;
    }
    classification.kind = QgsAiSqlStatementKind::ReadOnly;
    return classification;
  }
  if ( first == "SHOW"_L1 || first == "VALUES"_L1 || first == "TABLE"_L1 )
  {
    classification.kind = sideEffects ? QgsAiSqlStatementKind::Mutation : QgsAiSqlStatementKind::ReadOnly;
    return classification;
  }

  classification.kind = QgsAiSqlStatementKind::Mutation;
  return classification;
}

QString QgsAiListDatabaseConnectionsTool::description() const
{
  return QStringLiteral(
    "Lists PostgreSQL/PostGIS connections saved in this QGIS profile (Browser names). "
    "Returns name, database, host, port, sslmode, username if saved, and whether an auth config is set. "
    "Never returns passwords. Use the returned name as connection_name for the other database tools."
  );
}

QJsonObject QgsAiListDatabaseConnectionsTool::schema() const
{
  return schemaObject( QJsonObject() );
}

QgsAiToolResult QgsAiListDatabaseConnectionsTool::execute( const QJsonObject & )
{
  const QStringList names = savedPostgresConnectionNames();
  QJsonArray connections;
  QgsSettings settings;
  for ( const QString &name : names )
  {
    const QString key = u"/PostgreSQL/connections/"_s + name;
    QJsonObject entry;
    entry.insert( u"name"_s, name );
    entry.insert( u"database"_s, settings.value( key + u"/database"_s ).toString() );
    entry.insert( u"host"_s, settings.value( key + u"/host"_s ).toString() );
    entry.insert( u"port"_s, settings.value( key + u"/port"_s ).toString() );
    const QString service = settings.value( key + u"/service"_s ).toString();
    if ( !service.isEmpty() )
      entry.insert( u"service"_s, service );
    entry.insert( u"sslmode"_s, QgsDataSourceUri::encodeSslMode( settings.enumValue( key + u"/sslmode"_s, QgsDataSourceUri::SslPrefer ) ) );
    if ( settings.value( key + u"/saveUsername"_s ).toString() == "true"_L1 )
      entry.insert( u"username"_s, settings.value( key + u"/username"_s ).toString() );
    entry.insert( u"has_authcfg"_s, !settings.value( key + u"/authcfg"_s ).toString().trimmed().isEmpty() );
    connections.append( entry );
  }

  QJsonObject output;
  output.insert( u"provider"_s, u"postgres"_s );
  output.insert( u"connections"_s, connections );
  output.insert( u"count"_s, connections.size() );
  if ( connections.isEmpty() )
    output.insert( u"note"_s, u"No saved PostgreSQL connections. Create one in the QGIS Browser (PostGIS > New Connection) and save credentials in the QGIS auth vault."_s );
  return QgsAiToolResult::ok( output );
}

QString QgsAiDescribeDatabaseSchemaTool::description() const
{
  return QStringLiteral(
    "Describes live PostgreSQL/PostGIS schemas, tables, geometry columns, CRS, primary keys, and (when table is set) fields and spatial indexes. "
    "Requires a saved QGIS connection name from list_database_connections. Use this instead of information_schema queries."
  );
}

QJsonObject QgsAiDescribeDatabaseSchemaTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"connection_name"_s, prop( u"string"_s, u"Saved QGIS PostgreSQL connection name."_s ) );
  properties.insert( u"schema"_s, prop( u"string"_s, u"Optional schema to list. Omit to list user schemas."_s ) );
  properties.insert( u"table"_s, prop( u"string"_s, u"Optional table name. When set, returns fields and spatial_index."_s ) );
  return schemaObject( properties, QJsonArray { u"connection_name"_s } );
}

namespace
{
  QgsAiToolResult describeDatabaseSchema( const QString &connectionName, const QString &schemaFilter, const QString &tableFilter, QgsFeedback *feedback )
  {
    QString error;
    std::unique_ptr<QgsAbstractDatabaseProviderConnection> connection = openPostgresConnection( connectionName, error );
    if ( !connection )
      return QgsAiToolResult::error( error );
    // Estimated metadata: resolving the geometry type of generic columns otherwise scans whole tables.
    QVariantMap configuration = connection->configuration();
    configuration.insert( u"estimatedMetadata"_s, true );
    connection->setConfiguration( configuration );

    try
    {
      if ( !tableFilter.isEmpty() )
      {
        const QString schema = schemaFilter.isEmpty() ? u"public"_s : schemaFilter;
        const QgsAbstractDatabaseProviderConnection::TableProperty property = connection->table( schema, tableFilter, feedback );
        QJsonObject output = tablePropertyJson( property );
        output.insert( u"connection_name"_s, connectionName );

        const QgsFields fields = connection->fields( schema, tableFilter, feedback );
        QJsonArray fieldArray;
        for ( int i = 0; i < fields.count(); ++i )
        {
          QJsonObject field;
          field.insert( u"name"_s, fields.at( i ).name() );
          field.insert( u"type"_s, fields.at( i ).displayType() );
          fieldArray.append( field );
        }
        output.insert( u"fields"_s, fieldArray );

        bool spatialIndex = false;
        if ( !property.geometryColumn().isEmpty() )
        {
          try
          {
            spatialIndex = connection->spatialIndexExists( schema, tableFilter, property.geometryColumn() );
          }
          catch ( const QgsProviderConnectionException & )
          {
            spatialIndex = false;
          }
        }
        output.insert( u"spatial_index"_s, spatialIndex );
        if ( outputExceedsCap( output ) )
        {
          output.insert( u"truncated"_s, true );
          output.remove( u"fields"_s );
          output.insert( u"note"_s, u"Field list omitted because the result exceeded the size cap."_s );
        }
        return QgsAiToolResult::ok( output );
      }

      QStringList schemas = schemaFilter.isEmpty() ? connection->schemas() : QStringList { schemaFilter };
      QStringList userSchemas;
      for ( const QString &schema : schemas )
      {
        if ( !isSystemSchema( schema ) )
          userSchemas.append( schema );
      }

      QJsonArray tables;
      bool truncated = false;
      QJsonObject output;
      output.insert( u"connection_name"_s, connectionName );
      output.insert( u"schemas"_s, QJsonArray::fromStringList( userSchemas ) );
      // Running total: re-serializing the whole catalog for every table grew quadratically.
      qsizetype outputBytes = QJsonDocument( output ).toJson( QJsonDocument::Compact ).size() + 64;

      for ( const QString &schema : userSchemas )
      {
        if ( feedback->isCanceled() )
          return QgsAiToolResult::canceledResult( u"Reading the database schema was stopped."_s );
        const QList<QgsAbstractDatabaseProviderConnection::TableProperty> properties = connection->tables( schema, QgsAbstractDatabaseProviderConnection::TableFlags(), feedback );
        for ( const QgsAbstractDatabaseProviderConnection::TableProperty &property : properties )
        {
          const QJsonObject table = tablePropertyJson( property );
          outputBytes += QJsonDocument( table ).toJson( QJsonDocument::Compact ).size() + 1;
          if ( outputBytes > MAX_DB_TOOL_RESULT_BYTES )
          {
            truncated = true;
            break;
          }
          tables.append( table );
        }
        if ( truncated )
          break;
      }

      output.insert( u"tables"_s, tables );
      output.insert( u"table_count"_s, tables.size() );
      if ( truncated )
      {
        output.insert( u"truncated"_s, true );
        output.insert( u"note"_s, u"Catalog truncated. Pass schema or table to narrow the request."_s );
      }
      return QgsAiToolResult::ok( output );
    }
    catch ( const QgsProviderConnectionException &ex )
    {
      return QgsAiToolResult::error( u"Could not describe database '%1': %2"_s.arg( connectionName, ex.what() ) );
    }
  }
} // namespace

QgsAiToolResult QgsAiDescribeDatabaseSchemaTool::execute( const QJsonObject &args )
{
  const QString connectionName = args.value( u"connection_name"_s ).toString().trimmed();
  const QString schemaFilter = args.value( u"schema"_s ).toString().trimmed();
  const QString tableFilter = args.value( u"table"_s ).toString().trimmed();

  // A remote catalog with thousands of tables takes seconds: read it in the background.
  auto result = std::make_shared<QgsAiToolResult>( QgsAiToolResult::error( u"Could not describe the database."_s ) );
  const QgsAiTaskWaitResult wait = qgsAiRunFunction( u"Reading the database schema"_s, [result, connectionName, schemaFilter, tableFilter]( QgsFeedback *feedback ) {
    *result = describeDatabaseSchema( connectionName, schemaFilter, tableFilter, feedback );
    return !feedback->isCanceled();
  } );
  if ( wait.canceled || wait.abandoned )
    return QgsAiToolResult::canceledResult( u"Reading the database schema was stopped."_s );
  return *result;
}


QgsAiDatabaseSqlTool::QgsAiDatabaseSqlTool( QgsProject *project, bool readOnly )
  : mProject( project )
  , mReadOnly( readOnly )
{}

QString QgsAiDatabaseSqlTool::name() const
{
  return mReadOnly ? u"query_sql"_s : u"execute_sql"_s;
}

QString QgsAiDatabaseSqlTool::description() const
{
  if ( mReadOnly )
  {
    return QStringLiteral(
      "Runs a single read-only PostgreSQL/PostGIS statement (SELECT, WITH…SELECT, EXPLAIN, SHOW) on a saved QGIS connection "
      "and returns paginated rows in the tool result. Rejects INSERT/UPDATE/DELETE/DDL, SELECT INTO, FOR UPDATE, and multiple statements. "
      "Prefer this over run_processing_algorithm(native:postgisexecutesql). Geometry is omitted unless include_geometry is true."
    );
  }
  return QStringLiteral(
    "Executes a single SQL statement on a saved PostgreSQL/PostGIS connection, including DDL and DML. "
    "SELECT results are returned as rows. Optional load_as_layer adds a query layer to the project (layer rollback only; SQL is not reverted). "
    "Requires approval. Prefer this over native:postgisexecutesql / native:postgisexecuteandloadsql. One statement per call."
  );
}

QJsonObject QgsAiDatabaseSqlTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"connection_name"_s, prop( u"string"_s, u"Saved QGIS PostgreSQL connection name."_s ) );
  properties.insert( u"sql"_s, prop( u"string"_s, u"A single SQL statement."_s ) );
  properties.insert( u"limit"_s, prop( u"integer"_s, u"Maximum rows to return for SELECT. Defaults to 50, max 500."_s ) );
  properties.insert( u"offset"_s, prop( u"integer"_s, u"Zero-based row offset for SELECT. Defaults to 0."_s ) );
  properties.insert( u"include_geometry"_s, prop( u"boolean"_s, u"If true, geometry columns are returned as WKT. Default false."_s ) );
  if ( !mReadOnly )
  {
    properties.insert( u"load_as_layer"_s, prop( u"boolean"_s, u"If true and SQL is a SELECT, load the result as a project layer."_s ) );
    properties.insert( u"layer_name"_s, prop( u"string"_s, u"Optional display name when load_as_layer is true."_s ) );
    properties.insert( u"geometry_column"_s, prop( u"string"_s, u"Optional geometry column when load_as_layer is true."_s ) );
    properties.insert( u"rollback_token"_s, prop( u"string"_s, u"Optional token to remove a layer previously added by execute_sql."_s ) );
  }
  QJsonArray required { u"connection_name"_s };
  if ( mReadOnly )
    required.append( u"sql"_s );
  return schemaObject( properties, required );
}

QgsAiToolResult QgsAiDatabaseSqlTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !mReadOnly )
  {
    const QString rollbackToken = args.value( u"rollback_token"_s ).toString().trimmed();
    if ( !rollbackToken.isEmpty() )
      return rollbackLayerAddition( project, rollbackToken );
  }

  const QString sql = args.value( u"sql"_s ).toString();
  if ( sql.trimmed().isEmpty() )
    return QgsAiToolResult::error( u"Argument 'sql' is required."_s );
  const QgsAiSqlClassification classification = classifyAiSql( sql );
  if ( classification.kind == QgsAiSqlStatementKind::Invalid )
    return QgsAiToolResult::error( classification.error );
  if ( mReadOnly && classification.kind != QgsAiSqlStatementKind::ReadOnly )
    return QgsAiToolResult::error( u"query_sql only allows a single SELECT / WITH…SELECT / EXPLAIN / SHOW statement. Use execute_sql for writes."_s );

  const bool loadAsLayer = !mReadOnly && args.value( u"load_as_layer"_s ).toBool( false );
  if ( loadAsLayer && classification.kind != QgsAiSqlStatementKind::ReadOnly )
    return QgsAiToolResult::error( u"load_as_layer is only supported for SELECT/WITH queries."_s );

  const QString connectionName = args.value( u"connection_name"_s ).toString().trimmed();
  const int limit = sqlLimitFromArgs( args );
  const int offset = sqlOffsetFromArgs( args );
  const bool includeGeometry = args.value( u"include_geometry"_s ).toBool( false );
  const bool readOnly = classification.kind == QgsAiSqlStatementKind::ReadOnly;
  const QString firstKeyword = databaseToolsFirstKeyword( sql );
  // SELECT, WITH, VALUES and TABLE return rows: the server stops at what the tool can return.
  const bool wrapWithLimit = readOnly
                             && ( firstKeyword == "SELECT"_L1 || firstKeyword == "WITH"_L1 || firstKeyword == "VALUES"_L1 || firstKeyword == "TABLE"_L1 )
                             && !databaseToolsEndsWithHiddenSemicolon( sql );
  const int timeoutSeconds = databaseToolsTimeoutSeconds( !readOnly );
  // Stop still cancels the statements that cannot run in a transaction block, but they have no server timeout.
  const bool alone = !readOnly && databaseToolsRunsOutsideTransaction( sql );
  const QString serverSql = alone ? sql.trimmed() : databaseToolsServerSql( sql, readOnly, wrapWithLimit, offset + limit + 1, timeoutSeconds );

  // The query runs in the background: Stop cancels it on the server (PQcancel) and the window
  // never waits for the database.
  struct SqlJob
  {
      QJsonObject output;
      QStringList columns;
      QString error;
  };
  auto job = std::make_shared<SqlJob>();
  const QString userSql = sql.trimmed();
  const QgsAiTaskWaitResult wait = qgsAiRunFunction( u"Running SQL"_s, [job, connectionName, serverSql, userSql, offset, limit, includeGeometry, timeoutSeconds]( QgsFeedback *feedback ) {
    QString error;
    std::unique_ptr<QgsAbstractDatabaseProviderConnection> connection = openPostgresConnection( connectionName, error );
    if ( !connection )
    {
      job->error = error;
      return true;
    }
    try
    {
      QgsAbstractDatabaseProviderConnection::QueryResult result = connection->execSql( serverSql, feedback );
      if ( feedback->isCanceled() )
        return false;
      job->columns = result.columns();
      job->output = sqlResultJson( result, job->columns, offset, limit, includeGeometry );
    }
    catch ( const QgsProviderConnectionException &ex )
    {
      if ( feedback->isCanceled() )
        return false;
      // The provider quotes the statement it sent: show the model its own SQL, not the wrapper.
      QString message = ex.what();
      message.replace( serverSql, userSql );
      if ( message.contains( "statement timeout"_L1, Qt::CaseInsensitive ) )
        job->error = u"The query ran longer than %1 s and the server stopped it (strata/ai/sql_timeout_s). Narrow it with WHERE or LIMIT."_s.arg( timeoutSeconds );
      else if ( message.contains( "read-only transaction"_L1, Qt::CaseInsensitive ) )
        job->error = u"query_sql runs read-only and the server refused a change: %1 Use execute_sql, which asks for approval."_s.arg( message );
      else
        job->error = u"Error executing SQL on '%1': %2"_s.arg( connectionName, message );
    }
    return true;
  } );
  if ( wait.canceled || wait.abandoned )
    return QgsAiToolResult::canceledResult( u"The SQL query was stopped and canceled on the server."_s );
  if ( !wait.succeeded )
    return QgsAiToolResult::error( wait.error.isEmpty() ? u"The SQL query failed."_s : wait.error );
  if ( !job->error.isEmpty() )
    return QgsAiToolResult::error( job->error );

  QJsonObject output = job->output;
  const QStringList &columns = job->columns;
  output.insert( u"connection_name"_s, connectionName );
  output.insert( u"statement_kind"_s, readOnly ? u"read"_s : u"write"_s );
  output.insert( u"sql"_s, sql.trimmed() );

  QJsonObject diff;
  diff.insert( u"summary"_s, readOnly ? u"Ran a read-only SQL statement on a PostgreSQL connection."_s : u"Executed SQL on a PostgreSQL connection. Database changes cannot be rolled back by Strata."_s );
  diff.insert( u"connection_name"_s, connectionName );
  diff.insert( u"rollback_supported"_s, false );

  if ( loadAsLayer )
  {
    if ( !project )
      return QgsAiToolResult::error( u"No active QgsProject available."_s );

    QgsAbstractDatabaseProviderConnection::SqlVectorLayerOptions options;
    options.sql = sql.trimmed();
    options.layerName = args.value( u"layer_name"_s ).toString().trimmed();
    if ( options.layerName.isEmpty() )
      options.layerName = u"QueryLayer"_s;
    options.geometryColumn = args.value( u"geometry_column"_s ).toString().trimmed();
    if ( options.geometryColumn.isEmpty() )
    {
      for ( const QString &column : columns )
      {
        if ( isGeometryColumnName( column ) )
        {
          options.geometryColumn = column;
          break;
        }
      }
    }

    // Making the query layer asks the server for its fields and geometry: in the background too.
    struct LayerJob
    {
        std::unique_ptr<QgsVectorLayer> layer;
        QString error;
    };
    auto layerJob = std::make_shared<LayerJob>();
    QThread *interfaceThread = QThread::currentThread();
    const QgsAiTaskWaitResult layerWait = qgsAiRunFunction( u"Loading the query layer"_s, [layerJob, connectionName, options, interfaceThread]( QgsFeedback * ) {
      QString error;
      std::unique_ptr<QgsAbstractDatabaseProviderConnection> connection = openPostgresConnection( connectionName, error );
      if ( !connection )
      {
        layerJob->error = error;
        return true;
      }
      try
      {
        std::unique_ptr<QgsVectorLayer> layer( connection->createSqlVectorLayer( options ) );
        if ( !layer || !layer->isValid() )
        {
          layerJob->error = u"SQL ran but the query layer could not be loaded: %1"_s.arg( layer ? layer->error().summary() : u"layer was not created"_s );
          return true;
        }
        layer->moveToThread( interfaceThread );
        layerJob->layer = std::move( layer );
      }
      catch ( const QgsProviderConnectionException &ex )
      {
        layerJob->error = u"SQL ran but the query layer could not be loaded: %1"_s.arg( ex.what() );
      }
      return true;
    } );
    if ( layerWait.canceled || layerWait.abandoned )
    {
      // Made in the worker and handed over: deleted from the interface thread's event loop.
      if ( layerJob->layer )
        layerJob->layer.release()->deleteLater();
      return QgsAiToolResult::canceledResult( u"Loading the query layer was stopped; the SQL already ran."_s );
    }
    if ( !layerJob->error.isEmpty() || !layerJob->layer )
      return QgsAiToolResult::error( layerJob->error.isEmpty() ? u"SQL ran but the query layer could not be loaded."_s : layerJob->error );

    QgsVectorLayer *added = layerJob->layer.release();
    project->addMapLayer( added );
    const QString token = storeLayerRollback( added->id() );
    output.insert( u"layer_id"_s, added->id() );
    output.insert( u"layer_name"_s, added->name() );
    output.insert( u"rollback_token"_s, token );
    output.insert( u"rollback"_s, rollbackJson( token, u"remove_added_layer"_s ) );
    diff.insert( u"rollback_supported"_s, true );
    diff.insert( u"layer_id"_s, added->id() );
    diff.insert( u"summary"_s, u"Executed SQL and loaded the result as a project layer. Only the layer can be rolled back, not database writes."_s );
  }

  output.insert( u"status"_s, u"ok"_s );
  output.insert( u"diff"_s, diff );
  return QgsAiToolResult::ok( output );
}

QgsAiExportLayerToPostgisTool::QgsAiExportLayerToPostgisTool( QgsProject *project )
  : mProject( project )
{}

QString QgsAiExportLayerToPostgisTool::description() const
{
  return QStringLiteral(
    "Exports a project vector layer into a PostgreSQL/PostGIS table on a saved QGIS connection. "
    "Prefer this over run_processing_algorithm(native:importintopostgis). Overwrite is destructive and defaults to false. "
    "Database writes cannot be rolled back."
  );
}

QJsonObject QgsAiExportLayerToPostgisTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"layer_id"_s, prop( u"string"_s, u"Project vector layer id to export."_s ) );
  properties.insert( u"connection_name"_s, prop( u"string"_s, u"Saved QGIS PostgreSQL connection name."_s ) );
  properties.insert( u"schema"_s, prop( u"string"_s, u"Destination schema. Defaults to public."_s ) );
  properties.insert( u"table"_s, prop( u"string"_s, u"Destination table name. Defaults to the layer name."_s ) );
  properties.insert( u"geometry_column"_s, prop( u"string"_s, u"Geometry column name. Defaults to geom."_s ) );
  properties.insert( u"primary_key"_s, prop( u"string"_s, u"Optional primary key field name from the source layer."_s ) );
  properties.insert( u"overwrite"_s, prop( u"boolean"_s, u"If true, overwrite an existing table. Default false. Destructive."_s ) );
  properties.insert( u"create_spatial_index"_s, prop( u"boolean"_s, u"Create a spatial index after export. Default true."_s ) );
  properties.insert( u"lowercase_names"_s, prop( u"boolean"_s, u"Convert field names to lowercase. Default true."_s ) );
  return schemaObject( properties, QJsonArray { u"layer_id"_s, u"connection_name"_s } );
}

QgsAiToolResult QgsAiExportLayerToPostgisTool::execute( const QJsonObject &args )
{
  QgsProject *project = mProject ? mProject : QgsProject::instance();
  if ( !project )
    return QgsAiToolResult::error( u"No active QgsProject available."_s );

  const QString layerId = args.value( u"layer_id"_s ).toString().trimmed();
  if ( layerId.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'layer_id' is required."_s );
  QgsVectorLayer *layer = qobject_cast<QgsVectorLayer *>( project->mapLayer( layerId ) );
  if ( !layer )
    return QgsAiToolResult::error( u"No vector layer with id: %1"_s.arg( layerId ) );

  const QString connectionName = args.value( u"connection_name"_s ).toString().trimmed();
  QString error;
  std::unique_ptr<QgsAbstractDatabaseProviderConnection> connection = openPostgresConnection( connectionName, error );
  if ( !connection )
    return QgsAiToolResult::error( error );

  const QString schema = args.value( u"schema"_s ).toString().trimmed().isEmpty() ? u"public"_s : args.value( u"schema"_s ).toString().trimmed();
  QString table = args.value( u"table"_s ).toString().trimmed();
  if ( table.isEmpty() )
    table = sanitizeTableName( layer->name() );
  else
    table = sanitizeTableName( table );
  if ( table.isEmpty() )
    return QgsAiToolResult::error( u"Destination table name is empty."_s );

  QString geomColumn = args.value( u"geometry_column"_s ).toString().trimmed();
  if ( geomColumn.isEmpty() )
    geomColumn = u"geom"_s;
  const bool overwrite = args.value( u"overwrite"_s ).toBool( false );
  const bool createIndex = args.contains( u"create_spatial_index"_s ) ? args.value( u"create_spatial_index"_s ).toBool() : true;
  const bool lowercaseNames = args.contains( u"lowercase_names"_s ) ? args.value( u"lowercase_names"_s ).toBool() : true;
  if ( lowercaseNames )
    geomColumn = geomColumn.toLower();
  if ( layer->wkbType() == Qgis::WkbType::NoGeometry )
    geomColumn.clear();

  // The rows go to a new table first: the one being replaced changes only once the export
  // succeeded, in a single transaction, so a failed or stopped export leaves it intact.
  const QString writeTable = overwrite ? u"strata_tmp_%1"_s.arg( QUuid::createUuid().toString( QUuid::Id128 ).left( 12 ) ) : table;
  QgsDataSourceUri uri( connection->uri() );
  uri.setSchema( schema );
  uri.setTable( writeTable );
  uri.setKeyColumn( args.value( u"primary_key"_s ).toString().trimmed() );
  uri.setGeometryColumn( geomColumn );

  QMap<QString, QVariant> options;
  if ( lowercaseNames )
    options.insert( u"lowercaseFieldNames"_s, true );

  // Features are read from a snapshot in the background: the layer may keep changing meanwhile.
  struct ExportJob
  {
      std::unique_ptr<QgsAbstractDatabaseProviderConnection> connection;
      std::unique_ptr<QgsVectorLayerFeatureSource> source;
      qint64 written = 0;
      bool spatialIndexCreated = false;
      QString error;
  };
  auto job = std::make_shared<ExportJob>();
  job->connection = std::move( connection );
  job->source = std::make_unique<QgsVectorLayerFeatureSource>( layer );
  const QgsFields fields = layer->fields();
  const Qgis::WkbType wkbType = layer->wkbType();
  const QgsCoordinateReferenceSystem crs = layer->crs();
  const long long featureCount = layer->featureCount();
  const QString destinationUri = uri.uri();

  const QgsAiTaskWaitResult wait
    = qgsAiRunFunction( u"Exporting to PostGIS"_s, [job, destinationUri, fields, wkbType, crs, options, featureCount, schema, table, writeTable, overwrite, geomColumn, createIndex]( QgsFeedback *feedback ) {
        QgsAbstractDatabaseProviderConnection *connection = job->connection.get();
        auto exporter = std::make_unique<QgsVectorLayerExporter>( destinationUri, u"postgres"_s, fields, wkbType, crs, false, options );
        if ( exporter->errorCode() != Qgis::VectorExportResult::Success )
        {
          job->error = u"Error exporting to PostGIS: %1"_s.arg( exporter->errorMessage() );
          return true;
        }

        // From here on writeTable is ours: whatever goes wrong, it is removed.
        auto dropWriteTable = [connection, schema, writeTable]() {
          try
          {
            connection->executeSql( u"DROP TABLE IF EXISTS %1"_s.arg( databaseToolsQuotedName( schema, writeTable ) ) );
          }
          catch ( const QgsProviderConnectionException & )
          {}
        };

        QgsFeatureIterator iterator = job->source->getFeatures();
        QgsFeature feature;
        QString exportError;
        while ( iterator.nextFeature( feature ) )
        {
          if ( feedback->isCanceled() )
            break;
          if ( !exporter->addFeature( feature, QgsFeatureSink::FastInsert ) )
          {
            exportError = exporter->errorMessage();
            break;
          }
          ++job->written;
          if ( featureCount > 0 && job->written % 200 == 0 )
            feedback->setProgress( 90.0 * static_cast<double>( job->written ) / static_cast<double>( featureCount ) );
        }
        if ( !feedback->isCanceled() && exportError.isEmpty() && !exporter->flushBuffer() )
          exportError = exporter->errorMessage();
        if ( exportError.isEmpty() && exporter->errorCode() != Qgis::VectorExportResult::Success )
          exportError = exporter->errorMessage();
        exporter.reset();
        if ( feedback->isCanceled() || !exportError.isEmpty() )
        {
          dropWriteTable();
          if ( !exportError.isEmpty() )
            job->error = overwrite ? u"Error exporting to PostGIS: %1 The existing table '%2' was not changed."_s.arg( exportError, table ) : u"Error exporting to PostGIS: %1"_s.arg( exportError );
          return !feedback->isCanceled();
        }

        if ( overwrite )
        {
          // One call, one implicit transaction: either the new table replaces the old one or nothing changes.
          try
          {
            connection->executeSql(
              u"DROP TABLE IF EXISTS %1; ALTER TABLE %2 RENAME TO \"%3\""_s.arg( databaseToolsQuotedName( schema, table ), databaseToolsQuotedName( schema, writeTable ), QString( table ).replace( '"', "\"\""_L1 ) )
            );
          }
          catch ( const QgsProviderConnectionException &ex )
          {
            dropWriteTable();
            job->error = u"The layer was exported, but the existing table '%1' could not be replaced (other objects may depend on it); it was not changed: %2"_s.arg( table, ex.what() );
            return true;
          }
          // Cosmetic: the primary key keeps the temporary name otherwise.
          try
          {
            connection->executeSql(
              u"ALTER INDEX IF EXISTS %1 RENAME TO \"%2_pkey\""_s.arg( databaseToolsQuotedName( schema, writeTable + u"_pkey"_s ), QString( table ).left( 58 ).replace( '"', "\"\""_L1 ) )
            );
          }
          catch ( const QgsProviderConnectionException & )
          {}
        }

        if ( !geomColumn.isEmpty() && createIndex )
        {
          try
          {
            QgsAbstractDatabaseProviderConnection::SpatialIndexOptions indexOptions;
            indexOptions.geometryColumnName = geomColumn;
            connection->createSpatialIndex( schema, table, indexOptions );
            job->spatialIndexCreated = true;
          }
          catch ( const QgsProviderConnectionException &ex )
          {
            job->error = u"Layer exported but creating the spatial index failed: %1"_s.arg( ex.what() );
            return true;
          }
        }
        feedback->setProgress( 95 );

        // Statistics for the planner. A new table has nothing to vacuum; VACUUM FULL would lock and rewrite it.
        try
        {
          connection->executeSql( u"ANALYZE %1"_s.arg( databaseToolsQuotedName( schema, table ) ) );
        }
        catch ( const QgsProviderConnectionException & )
        {}
        return true;
      } );
  if ( wait.canceled || wait.abandoned )
    return QgsAiToolResult::canceledResult(
      overwrite ? u"The export was stopped; the table '%1' was not changed."_s.arg( table ) : u"The export was stopped and the partial table '%1' was removed."_s.arg( table )
    );
  if ( !wait.succeeded && job->error.isEmpty() )
    return QgsAiToolResult::error( wait.error.isEmpty() ? u"The export failed."_s : wait.error );
  if ( !job->error.isEmpty() )
    return QgsAiToolResult::error( job->error );
  const qint64 written = job->written;
  const bool spatialIndexCreated = job->spatialIndexCreated;

  QJsonObject diff;
  diff.insert( u"summary"_s, overwrite ? u"Exported a vector layer to PostGIS, overwriting the destination table. Database writes cannot be rolled back."_s : u"Exported a vector layer to a new PostGIS table. Database writes cannot be rolled back."_s );
  diff.insert( u"schema"_s, schema );
  diff.insert( u"table"_s, table );
  diff.insert( u"overwrite"_s, overwrite );
  diff.insert( u"rollback_supported"_s, false );

  QJsonObject output;
  output.insert( u"status"_s, u"ok"_s );
  output.insert( u"connection_name"_s, connectionName );
  output.insert( u"schema"_s, schema );
  output.insert( u"table"_s, table );
  output.insert( u"geometry_column"_s, geomColumn );
  output.insert( u"feature_count"_s, written );
  output.insert( u"overwrite"_s, overwrite );
  output.insert( u"spatial_index"_s, spatialIndexCreated );
  output.insert( u"diff"_s, diff );
  return QgsAiToolResult::ok( output );
}
