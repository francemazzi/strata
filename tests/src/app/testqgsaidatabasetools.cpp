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

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QString>
#include <QTemporaryDir>
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
    void listConnectionsOmitsPassword();
    void listConnectionsEmptyNote();
    void querySqlRejectsWritesWithoutConnecting();
    void executeSqlRequiresSql();
    void describeUnknownConnectionListsSavedNames();
    void exportMissingLayer();
    void askModeAllowsQuerySqlButNotExecuteSql();
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

  const char *connstring = getenv( "QGIS_PGTEST_DB" );
  QString dbConn = connstring ? QString::fromLocal8Bit( connstring ) : QString();
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
