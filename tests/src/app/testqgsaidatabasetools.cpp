/***************************************************************************
  testqgsaidatabasetools.cpp
  --------------------------
  begin                : September 2026
***************************************************************************/

#include "qgsconfig.h"

#include <cstdlib>
#include <memory>

#include "ai/qgsaiagentsessionmanager.h"
#include "ai/qgsaifilecontextprovider.h"
#include "ai/qgsaimodelrouter.h"
#include "ai/qgsaireviewpatchengine.h"
#include "ai/tools/qgsaidatabasetools.h"
#include "ai/tools/qgsaitaskrunner.h"
#include "ai/tools/qgsaitoolregistry.h"
#include "qgsabstractproviderconnection.h"
#include "qgsapplication.h"
#include "qgsexception.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgsproject.h"
#include "qgsprovidermetadata.h"
#include "qgsproviderregistry.h"
#include "qgssettings.h"
#include "qgstest.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>

using namespace Qt::StringLiterals;

class TestQgsAiDatabaseTools : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void classifyReadOnlySelect();
    void classifyWithSelectIsReadOnly();
    void classifyRejectsMultipleStatements();
    void classifySelectIntoIsMutation();
    void classifySelectForUpdateIsMutation();
    void classifyDeleteIsMutation();
    void classifyExplainIsReadOnly();
    void classifyExplainAnalyzeInsertIsMutation();
    void classifySideEffectFunctionsAsMutations();
    void listConnectionsOmitsPassword();
    void listConnectionsEmptyNote();
    void querySqlRejectsWritesWithoutConnecting();
    void executeSqlRequiresSql();
    void describeUnknownConnectionListsSavedNames();
    void exportMissingLayer();
    void askModeAllowsQuerySqlButNotExecuteSql();
    void queriesRunOnTheServerSafely();
    void exportNeverLosesTheExistingTable();
#ifdef ENABLE_PGTEST
    void queryExecuteAndExportAgainstPostgres();
#endif
};

void TestQgsAiDatabaseTools::initTestCase()
{
  QgsApplication::initQgis();
}

void TestQgsAiDatabaseTools::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsAiDatabaseTools::classifyReadOnlySelect()
{
  const QgsAiSqlClassification classification = classifyAiSql( u"SELECT id, name FROM public.roads WHERE ST_Intersects(geom, ST_MakePoint(0,0))"_s );
  QCOMPARE( classification.kind, QgsAiSqlStatementKind::ReadOnly );
  QVERIFY( classification.error.isEmpty() );
}

void TestQgsAiDatabaseTools::classifyWithSelectIsReadOnly()
{
  QCOMPARE( classifyAiSql( u"WITH x AS (SELECT 1 AS n) SELECT * FROM x"_s ).kind, QgsAiSqlStatementKind::ReadOnly );
}

void TestQgsAiDatabaseTools::classifyRejectsMultipleStatements()
{
  const QgsAiSqlClassification classification = classifyAiSql( u"SELECT 1; DELETE FROM roads"_s );
  QCOMPARE( classification.kind, QgsAiSqlStatementKind::Invalid );
  QVERIFY( classification.error.contains( u"one SQL statement"_s, Qt::CaseInsensitive ) );
}

void TestQgsAiDatabaseTools::classifySelectIntoIsMutation()
{
  QCOMPARE( classifyAiSql( u"SELECT * INTO tmp FROM roads"_s ).kind, QgsAiSqlStatementKind::Mutation );
}

void TestQgsAiDatabaseTools::classifySelectForUpdateIsMutation()
{
  QCOMPARE( classifyAiSql( u"SELECT * FROM roads FOR UPDATE"_s ).kind, QgsAiSqlStatementKind::Mutation );
}

void TestQgsAiDatabaseTools::classifyDeleteIsMutation()
{
  QCOMPARE( classifyAiSql( u"DELETE FROM roads WHERE id = 1"_s ).kind, QgsAiSqlStatementKind::Mutation );
}

void TestQgsAiDatabaseTools::classifyExplainIsReadOnly()
{
  QCOMPARE( classifyAiSql( u"EXPLAIN SELECT * FROM roads"_s ).kind, QgsAiSqlStatementKind::ReadOnly );
}

void TestQgsAiDatabaseTools::classifyExplainAnalyzeInsertIsMutation()
{
  QCOMPARE( classifyAiSql( u"EXPLAIN ANALYZE INSERT INTO roads(id) VALUES (1)"_s ).kind, QgsAiSqlStatementKind::Mutation );
}

void TestQgsAiDatabaseTools::classifySideEffectFunctionsAsMutations()
{
  // Looks like a read, but ends sessions, moves sequences or runs SQL elsewhere.
  for ( const QString &sql :
        { u"SELECT pg_terminate_backend( 1234 )"_s,
          u"SELECT nextval( 'roads_id_seq' )"_s,
          u"select setval('s', 1)"_s,
          u"SELECT * FROM dblink_exec( 'dbname=x', 'DROP TABLE t' )"_s,
          u"VALUES ( nextval( 's' ) )"_s,
          u"WITH x AS ( SELECT 1 ) SELECT pg_cancel_backend( 1 ) FROM x"_s } )
    QCOMPARE( classifyAiSql( sql ).kind, QgsAiSqlStatementKind::Mutation );
  // The names only count as calls, not inside strings.
  QCOMPARE( classifyAiSql( u"SELECT 'nextval' AS word, count(*) FROM roads"_s ).kind, QgsAiSqlStatementKind::ReadOnly );

  QgsAiDatabaseSqlTool query( nullptr, true );
  QJsonObject args;
  args.insert( u"connection_name"_s, u"missing"_s );
  args.insert( u"sql"_s, u"SELECT pg_terminate_backend( 1234 )"_s );
  const QgsAiToolResult result = query.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"execute_sql"_s ) );
}

namespace
{
  //! Saves a connection to the server named by QGIS_PGTEST_DB; false when none is configured.
  bool savePostgresTestConnection( const QString &name, QString &skipReason )
  {
    const QString connstring = qEnvironmentVariable( "QGIS_PGTEST_DB" );
    if ( connstring.isEmpty() )
    {
      skipReason = u"Set QGIS_PGTEST_DB to run the PostgreSQL checks of the AI database tools."_s;
      return false;
    }
    QgsProviderMetadata *md = QgsProviderRegistry::instance()->providerMetadata( u"postgres"_s );
    try
    {
      std::unique_ptr<QgsAbstractProviderConnection> conn( md->createConnection( u"%1 sslmode=disable"_s.arg( connstring ), QVariantMap() ) );
      md->saveConnection( conn.get(), name );
    }
    catch ( const QgsProviderConnectionException &ex )
    {
      skipReason = ex.what();
      return false;
    }
    return true;
  }

  QgsAiToolResult runSql( QgsAiDatabaseSqlTool &tool, const QString &connection, const QString &sql, int limit = 50 )
  {
    QJsonObject args;
    args.insert( u"connection_name"_s, connection );
    args.insert( u"sql"_s, sql );
    args.insert( u"limit"_s, limit );
    return tool.execute( args );
  }

  int countRows( QgsAiDatabaseSqlTool &query, const QString &connection, const QString &sql )
  {
    const QgsAiToolResult result = runSql( query, connection, sql );
    if ( !result.success )
      return -1;
    return result.output.toObject().value( u"rows"_s ).toArray().at( 0 ).toObject().value( u"c"_s ).toInt( -1 );
  }
} // namespace

void TestQgsAiDatabaseTools::queriesRunOnTheServerSafely()
{
  const QString name = u"ai_pg_safety"_s;
  QString skipReason;
  if ( !savePostgresTestConnection( name, skipReason ) )
    QSKIP( qPrintable( skipReason ) );

  QgsAiDatabaseSqlTool query( nullptr, true );
  QgsAiDatabaseSqlTool exec( nullptr, false );
  const QScopeGuard cleanup( [&] {
    runSql( exec, name, u"DROP TABLE IF EXISTS ai_big_table"_s );
    runSql( exec, name, u"DROP FUNCTION IF EXISTS ai_ro_write()"_s );
    runSql( exec, name, u"DROP TABLE IF EXISTS ai_ro_probe"_s );
    QgsSettings().remove( u"strata/ai/sql_timeout_s"_s );
    QgsProviderRegistry::instance()->providerMetadata( u"postgres"_s )->deleteConnection( name );
  } );

  // The server stops at the rows the tool can return, instead of sending two million.
  QgsAiToolResult created = runSql( exec, name, u"CREATE TABLE ai_big_table AS SELECT g AS n, md5( g::text ) AS label FROM generate_series( 1, 2000000 ) AS g"_s );
  QVERIFY2( created.success, created.errorMessage.toUtf8().constData() );
  QElapsedTimer clock;
  clock.start();
  const QgsAiToolResult page = runSql( query, name, u"SELECT * FROM ai_big_table -- every row"_s, 100 );
  const qint64 pageMs = clock.elapsed();
  QVERIFY2( page.success, page.errorMessage.toUtf8().constData() );
  QCOMPARE( page.output.toObject().value( u"rows"_s ).toArray().size(), 100 );
  QVERIFY2( pageMs < 1000, QString::number( pageMs ).toUtf8().constData() );

  // query_sql is read-only on the server too: a function that writes is refused.
  QVERIFY( runSql( exec, name, u"CREATE TABLE ai_ro_probe ( n integer )"_s ).success );
  const QgsAiToolResult function = runSql( exec, name, u"CREATE FUNCTION ai_ro_write() RETURNS integer LANGUAGE sql VOLATILE AS $$ INSERT INTO ai_ro_probe VALUES ( 1 ) RETURNING n $$"_s );
  QVERIFY2( function.success, function.errorMessage.toUtf8().constData() );
  const QgsAiToolResult refused = runSql( query, name, u"SELECT ai_ro_write()"_s );
  QVERIFY( !refused.success );
  QVERIFY2( refused.errorMessage.contains( u"read-only"_s ), refused.errorMessage.toUtf8().constData() );
  // The model sees its own SQL, not what Strata wrapped around it.
  QVERIFY( !refused.errorMessage.contains( u"strata_query"_s ) );
  QCOMPARE( countRows( query, name, u"SELECT count(*) AS c FROM ai_ro_probe"_s ), 0 );

  // A query that runs too long is stopped by the server.
  QgsSettings().setValue( u"strata/ai/sql_timeout_s"_s, 1 );
  clock.restart();
  const QgsAiToolResult slow = runSql( query, name, u"SELECT pg_sleep( 10 )"_s );
  QVERIFY( !slow.success );
  QVERIFY2( slow.errorMessage.contains( u"sql_timeout_s"_s ), slow.errorMessage.toUtf8().constData() );
  QVERIFY2( clock.elapsed() < 5000, QString::number( clock.elapsed() ).toUtf8().constData() );
  QgsSettings().remove( u"strata/ai/sql_timeout_s"_s );

  // Stop cancels the query on the server, not only in the window.
  QTimer::singleShot( 500, [] { qgsAiCancelActiveBackgroundTool(); } );
  clock.restart();
  const QgsAiToolResult stopped = runSql( query, name, u"SELECT pg_sleep( 30 )"_s );
  QVERIFY( stopped.canceled );
  QVERIFY2( clock.elapsed() < 5000, QString::number( clock.elapsed() ).toUtf8().constData() );
  QTRY_COMPARE_WITH_TIMEOUT( countRows( query, name, u"SELECT count(*) AS c FROM pg_stat_activity WHERE state = 'active' AND query LIKE '%pg_sleep( 30 )%' AND pid <> pg_backend_pid()"_s ), 0, 5000 );
}

void TestQgsAiDatabaseTools::exportNeverLosesTheExistingTable()
{
  const QString name = u"ai_pg_export"_s;
  QString skipReason;
  if ( !savePostgresTestConnection( name, skipReason ) )
    QSKIP( qPrintable( skipReason ) );

  QgsProject project;
  QgsAiDatabaseSqlTool query( nullptr, true );
  QgsAiDatabaseSqlTool exec( nullptr, false );
  const QScopeGuard cleanup( [&] {
    runSql( exec, name, u"DROP TABLE IF EXISTS ai_export_safe"_s );
    qgsAiSetBackgroundToolProgressHandler( {} );
    QgsProviderRegistry::instance()->providerMetadata( u"postgres"_s )->deleteConnection( name );
  } );

  auto makeLayer = [&project]( const QString &layerName, int count, int duplicateAt = -1 ) {
    auto *layer = new QgsVectorLayer( u"None?field=fid_value:integer&field=name:string"_s, layerName, u"memory"_s );
    QgsFeatureList features;
    features.reserve( count );
    for ( int i = 0; i < count; ++i )
    {
      QgsFeature feature( layer->fields() );
      feature.setAttribute( 0, i == duplicateAt ? 0 : i );
      feature.setAttribute( 1, u"row %1"_s.arg( i ) );
      features << feature;
    }
    layer->dataProvider()->addFeatures( features );
    project.addMapLayer( layer );
    return layer;
  };
  auto exportLayer = [&name]( QgsProject &project, QgsVectorLayer *layer ) {
    QgsAiExportLayerToPostgisTool exporter( &project );
    QJsonObject args;
    args.insert( u"layer_id"_s, layer->id() );
    args.insert( u"connection_name"_s, name );
    args.insert( u"schema"_s, u"public"_s );
    args.insert( u"table"_s, u"ai_export_safe"_s );
    args.insert( u"primary_key"_s, u"fid_value"_s );
    args.insert( u"overwrite"_s, true );
    return exporter.execute( args );
  };
  const QString leftovers = u"SELECT count(*) AS c FROM pg_tables WHERE tablename LIKE 'strata_tmp_%'"_s;

  const QgsAiToolResult first = exportLayer( project, makeLayer( u"three"_s, 3 ) );
  QVERIFY2( first.success, first.errorMessage.toUtf8().constData() );
  QCOMPARE( countRows( query, name, u"SELECT count(*) AS c FROM ai_export_safe"_s ), 3 );

  // Overwriting with the same rows works and replaces the table.
  const QgsAiToolResult again = exportLayer( project, makeLayer( u"five"_s, 5 ) );
  QVERIFY2( again.success, again.errorMessage.toUtf8().constData() );
  QCOMPARE( countRows( query, name, u"SELECT count(*) AS c FROM ai_export_safe"_s ), 5 );

  // An error half way (a duplicate key) leaves the table as it was.
  const QgsAiToolResult failed = exportLayer( project, makeLayer( u"broken"_s, 3000, 2500 ) );
  QVERIFY( !failed.success );
  QVERIFY2( failed.errorMessage.contains( u"was not changed"_s ), failed.errorMessage.toUtf8().constData() );
  QCOMPARE( countRows( query, name, u"SELECT count(*) AS c FROM ai_export_safe"_s ), 5 );
  QCOMPARE( countRows( query, name, leftovers ), 0 );

  // Stop half way does the same.
  qgsAiSetBackgroundToolProgressHandler( []( const QString &, double progress ) {
    if ( progress > 10 )
      qgsAiCancelActiveBackgroundTool();
  } );
  const QgsAiToolResult stopped = exportLayer( project, makeLayer( u"large"_s, 50000 ) );
  qgsAiSetBackgroundToolProgressHandler( {} );
  QVERIFY( stopped.canceled );
  QCOMPARE( countRows( query, name, u"SELECT count(*) AS c FROM ai_export_safe"_s ), 5 );
  QTRY_COMPARE_WITH_TIMEOUT( countRows( query, name, leftovers ), 0, 5000 );
}

void TestQgsAiDatabaseTools::listConnectionsOmitsPassword()
{
  QgsSettings settings;
  settings.setValue( u"/PostgreSQL/connections/lab/host"_s, u"localhost"_s );
  settings.setValue( u"/PostgreSQL/connections/lab/port"_s, u"5432"_s );
  settings.setValue( u"/PostgreSQL/connections/lab/database"_s, u"gis"_s );
  settings.setValue( u"/PostgreSQL/connections/lab/saveUsername"_s, u"true"_s );
  settings.setValue( u"/PostgreSQL/connections/lab/username"_s, u"postgres"_s );
  settings.setValue( u"/PostgreSQL/connections/lab/password"_s, u"super-secret-password"_s );
  settings.setValue( u"/PostgreSQL/connections/lab/authcfg"_s, u"abc123"_s );

  QgsAiListDatabaseConnectionsTool tool;
  const QgsAiToolResult result = tool.execute( QJsonObject() );
  QVERIFY( result.success );
  const QJsonObject output = result.output.toObject();
  const QByteArray json = QJsonDocument( output ).toJson( QJsonDocument::Compact );
  QVERIFY( !json.contains( "super-secret-password" ) );

  bool found = false;
  const QJsonArray connections = output.value( u"connections"_s ).toArray();
  for ( const QJsonValue &value : connections )
  {
    const QJsonObject entry = value.toObject();
    QVERIFY( !entry.contains( u"password"_s ) );
    QVERIFY( !entry.contains( u"uri"_s ) );
    if ( entry.value( u"name"_s ).toString() != "lab"_L1 )
      continue;
    found = true;
    QCOMPARE( entry.value( u"database"_s ).toString(), u"gis"_s );
    QCOMPARE( entry.value( u"host"_s ).toString(), u"localhost"_s );
    QCOMPARE( entry.value( u"username"_s ).toString(), u"postgres"_s );
    QCOMPARE( entry.value( u"has_authcfg"_s ).toBool(), true );
  }
  QVERIFY( found );

  settings.remove( u"/PostgreSQL/connections/lab"_s );
}

void TestQgsAiDatabaseTools::listConnectionsEmptyNote()
{
  QgsSettings settings;
  settings.remove( u"PostgreSQL/connections"_s );

  QgsAiListDatabaseConnectionsTool tool;
  const QgsAiToolResult result = tool.execute( QJsonObject() );
  QVERIFY( result.success );
  const QJsonObject output = result.output.toObject();
  if ( output.value( u"count"_s ).toInt() == 0 )
    QVERIFY( output.value( u"note"_s ).toString().contains( u"Browser"_s ) );
}

void TestQgsAiDatabaseTools::querySqlRejectsWritesWithoutConnecting()
{
  QgsAiDatabaseSqlTool tool( nullptr, true );
  QJsonObject args;
  args.insert( u"connection_name"_s, u"missing"_s );
  args.insert( u"sql"_s, u"DELETE FROM public.roads"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"query_sql"_s ) );
  QVERIFY( result.errorMessage.contains( u"execute_sql"_s ) );
}

void TestQgsAiDatabaseTools::executeSqlRequiresSql()
{
  QgsAiDatabaseSqlTool tool( nullptr, false );
  QJsonObject args;
  args.insert( u"connection_name"_s, u"lab"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"sql"_s, Qt::CaseInsensitive ) );
}

void TestQgsAiDatabaseTools::describeUnknownConnectionListsSavedNames()
{
  QgsSettings settings;
  settings.setValue( u"/PostgreSQL/connections/lab/database"_s, u"gis"_s );

  QgsAiDescribeDatabaseSchemaTool tool;
  QJsonObject args;
  args.insert( u"connection_name"_s, u"does_not_exist"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"does_not_exist"_s ) );
  QVERIFY( result.errorMessage.contains( u"lab"_s ) );

  settings.remove( u"/PostgreSQL/connections/lab"_s );
}

void TestQgsAiDatabaseTools::exportMissingLayer()
{
  QgsProject project;
  QgsAiExportLayerToPostgisTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, u"missing"_s );
  args.insert( u"connection_name"_s, u"lab"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"No vector layer"_s ) );
}

void TestQgsAiDatabaseTools::askModeAllowsQuerySqlButNotExecuteSql()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<QgsAiListDatabaseConnectionsTool>() );
  registry.registerTool( std::make_unique<QgsAiDescribeDatabaseSchemaTool>() );
  QgsProject project;
  registry.registerTool( std::make_unique<QgsAiDatabaseSqlTool>( &project, true ) );
  registry.registerTool( std::make_unique<QgsAiDatabaseSqlTool>( &project, false ) );
  registry.registerTool( std::make_unique<QgsAiExportLayerToPostgisTool>( &project ) );

  QgsAiModelRouter router;
  router.setToolRegistry( &registry );

  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( &router, &contextProvider, &reviewEngine );
  manager.setToolRegistry( &registry );

  QgsAiAgentBehaviorSettings updated = manager.agentBehaviorSettings();
  updated.allowCustomActions = true;
  manager.setAgentBehaviorSettings( updated );

  manager.setActiveAgent( u"reviewer"_s );
  QVERIFY( router.allowedTools().contains( u"list_database_connections"_s ) );
  QVERIFY( router.allowedTools().contains( u"describe_database_schema"_s ) );
  QVERIFY( router.allowedTools().contains( u"query_sql"_s ) );
  QVERIFY( !router.allowedTools().contains( u"execute_sql"_s ) );
  QVERIFY( !router.allowedTools().contains( u"export_layer_to_postgis"_s ) );

  manager.setActiveAgent( u"ask_before_edits"_s );
  QVERIFY( router.allowedTools().contains( u"query_sql"_s ) );
  QVERIFY( router.allowedTools().contains( u"execute_sql"_s ) );
  QVERIFY( router.allowedTools().contains( u"export_layer_to_postgis"_s ) );

  manager.setActiveAgent( u"editor"_s );
  QVERIFY( router.allowedTools().contains( u"execute_sql"_s ) );

  const QString prompt = manager.buildSystemPrompt();
  QVERIFY( prompt.contains( u"native:postgisexecutesql"_s ) );
  QVERIFY( prompt.contains( u"query_sql"_s ) );
}

#ifdef ENABLE_PGTEST
void TestQgsAiDatabaseTools::queryExecuteAndExportAgainstPostgres()
{
  QgsProviderMetadata *md = QgsProviderRegistry::instance()->providerMetadata( u"postgres"_s );
  QVERIFY( md );

  const QByteArray connstring = qgetenv( "QGIS_PGTEST_DB" );
  if ( connstring.isEmpty() && qEnvironmentVariableIsEmpty( "PGHOST" ) )
    QSKIP( "PostgreSQL integration test requires the POSTGRES test batch or QGIS_PGTEST_DB." );

  QString dbConn = QString::fromLocal8Bit( connstring );
  if ( dbConn.isEmpty() )
    dbConn = u"service=\"qgis_test\""_s;

  std::unique_ptr<QgsAbstractProviderConnection> conn;
  try
  {
    conn.reset( md->createConnection( u"%1 sslmode=disable"_s.arg( dbConn ), QVariantMap() ) );
  }
  catch ( const QgsProviderConnectionException &ex )
  {
    QSKIP( qPrintable( ex.what() ) );
  }
  QVERIFY( conn );
  md->saveConnection( conn.get(), u"ai_pg_test"_s );
  const QScopeGuard cleanup( [md] {
    try
    {
      md->deleteConnection( u"ai_pg_test"_s );
    }
    catch ( const QgsProviderConnectionException & )
    {}
  } );

  QgsAiDatabaseSqlTool query( nullptr, true );
  QJsonObject queryArgs;
  queryArgs.insert( u"connection_name"_s, u"ai_pg_test"_s );
  queryArgs.insert( u"sql"_s, u"SELECT 1 AS n"_s );
  const QgsAiToolResult queryResult = query.execute( queryArgs );
  QVERIFY2( queryResult.success, queryResult.errorMessage.toUtf8().constData() );
  const QJsonArray rows = queryResult.output.toObject().value( u"rows"_s ).toArray();
  QCOMPARE( rows.size(), 1 );
  QCOMPARE( rows.at( 0 ).toObject().value( u"n"_s ).toInt(), 1 );

  QgsProject project;
  QgsAiDatabaseSqlTool exec( &project, false );

  QJsonObject loadArgs;
  loadArgs.insert( u"connection_name"_s, u"ai_pg_test"_s );
  loadArgs.insert( u"sql"_s, u"SELECT 1 AS n"_s );
  loadArgs.insert( u"load_as_layer"_s, true );
  loadArgs.insert( u"layer_name"_s, u"ai_sql_layer"_s );
  const QgsAiToolResult loaded = exec.execute( loadArgs );
  QVERIFY2( loaded.success, loaded.errorMessage.toUtf8().constData() );
  const QString layerId = loaded.output.toObject().value( u"layer_id"_s ).toString();
  const QString token = loaded.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !layerId.isEmpty() );
  QVERIFY( !token.isEmpty() );
  QVERIFY( project.mapLayer( layerId ) );

  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, token );
  const QgsAiToolResult rolled = exec.execute( rollbackArgs );
  QVERIFY2( rolled.success, rolled.errorMessage.toUtf8().constData() );
  QVERIFY( !project.mapLayer( layerId ) );

  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string"_s, u"ai_export"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsFeature feature( layer->fields() );
  feature.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  feature.setAttribute( u"name"_s, u"one"_s );
  QVERIFY( layer->dataProvider()->addFeature( feature ) );
  project.addMapLayer( layer );

  QgsAiExportLayerToPostgisTool exporter( &project );
  QJsonObject exportArgs;
  exportArgs.insert( u"layer_id"_s, layer->id() );
  exportArgs.insert( u"connection_name"_s, u"ai_pg_test"_s );
  exportArgs.insert( u"schema"_s, u"public"_s );
  exportArgs.insert( u"table"_s, u"ai_chat_export_tmp"_s );
  exportArgs.insert( u"overwrite"_s, true );
  const QgsAiToolResult firstExport = exporter.execute( exportArgs );
  QVERIFY2( firstExport.success, firstExport.errorMessage.toUtf8().constData() );
  QCOMPARE( firstExport.output.toObject().value( u"feature_count"_s ).toInt(), 1 );

  const QgsAiToolResult secondExport = exporter.execute( exportArgs );
  QVERIFY2( secondExport.success, secondExport.errorMessage.toUtf8().constData() );
  QCOMPARE( secondExport.output.toObject().value( u"overwrite"_s ).toBool(), true );

  QJsonObject dropArgs;
  dropArgs.insert( u"connection_name"_s, u"ai_pg_test"_s );
  dropArgs.insert( u"sql"_s, u"DROP TABLE IF EXISTS public.ai_chat_export_tmp"_s );
  const QgsAiToolResult dropped = exec.execute( dropArgs );
  QVERIFY2( dropped.success, dropped.errorMessage.toUtf8().constData() );
}
#endif

QGSTEST_MAIN( TestQgsAiDatabaseTools )
#include "testqgsaidatabasetools.moc"
