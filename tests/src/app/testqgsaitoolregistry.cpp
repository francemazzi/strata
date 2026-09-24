/***************************************************************************
  testqgsaitoolregistry.cpp
  --------------------------
  begin                : April 2026
  copyright            : (C) 2026
***************************************************************************/

#include <cmath>
#include <memory>

#include "ai/qgsaiagentpolicy.h"
#include "ai/qgsaiauditlog.h"
#include "ai/qgsaifilecontextprovider.h"
#include "ai/qgsaiworkspacetrust.h"
#include "ai/tools/qgsaidownloadfiletool.h"
#include "ai/tools/qgsailayertools.h"
#include "ai/tools/qgsaireadtools.h"
#include "ai/tools/qgsairunpythontool.h"
#include "ai/tools/qgsaitoolregistry.h"
#include "qgsapplication.h"
#include "qgscategorizedsymbolrenderer.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsexception.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgsgraduatedsymbolrenderer.h"
#include "qgslayertree.h"
#include "qgslayertreelayer.h"
#include "qgslayoutitemlegend.h"
#include "qgslayoutitempicture.h"
#include "qgslayoutitemscalebar.h"
#include "qgslayoutmanager.h"
#include "qgslayoutpagecollection.h"
#include "qgsmapcanvas.h"
#include "qgsnativealgorithms.h"
#include "qgspallabeling.h"
#include "qgsprintlayout.h"
#include "qgsprocessingalgorithm.h"
#include "qgsprocessingprovider.h"
#include "qgsprocessingregistry.h"
#include "qgsproject.h"
#include "qgsrectangle.h"
#include "qgsrenderer.h"
#include "qgssettings.h"
#include "qgssinglesymbolrenderer.h"
#include "qgstest.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"
#include "qgsvectorlayerlabeling.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace
{
  //! Fails in prepareAlgorithm(), which the AI runner's task calls in its constructor.
  class FailingPrepareAlgorithm : public QgsProcessingAlgorithm
  {
    public:
      QString name() const override { return u"failingprepare"_s; }
      QString displayName() const override { return u"Failing prepare"_s; }
      void initAlgorithm( const QVariantMap & = QVariantMap() ) override {}
      QgsProcessingAlgorithm *createInstance() const override { return new FailingPrepareAlgorithm(); }

    protected:
      bool prepareAlgorithm( const QVariantMap &, QgsProcessingContext &, QgsProcessingFeedback * ) override { throw QgsProcessingException( u"prepare boom"_s ); }
      QVariantMap processAlgorithm( const QVariantMap &, QgsProcessingContext &, QgsProcessingFeedback * ) override { return QVariantMap(); }
  };

  class AiTestProcessingProvider : public QgsProcessingProvider
  {
    public:
      QString id() const override { return u"aitest"_s; }
      QString name() const override { return u"AI test"_s; }

    protected:
      void loadAlgorithms() override { addAlgorithm( new FailingPrepareAlgorithm() ); }
  };

  class FakeEchoTool : public QgsAiTool
  {
    public:
      explicit FakeEchoTool( const QString &name, bool requiresApproval = false, QgsAiToolRiskLevel riskLevel = QgsAiToolRiskLevel::Low )
        : mName( name )
        , mRequiresApproval( requiresApproval )
        , mRiskLevel( riskLevel )
      {}
      FakeEchoTool( const QString &name, bool requiresApproval, bool available, QgsAiToolRiskLevel riskLevel = QgsAiToolRiskLevel::Low )
        : mName( name )
        , mRequiresApproval( requiresApproval )
        , mAvailable( available )
        , mRiskLevel( riskLevel )
      {}

      QString name() const override { return mName; }
      QString description() const override { return u"Echoes the 'text' argument."_s; }

      QJsonObject schema() const override
      {
        QJsonObject properties;
        QJsonObject textProp;
        textProp.insert( u"type"_s, u"string"_s );
        properties.insert( u"text"_s, textProp );

        QJsonObject schemaObject;
        schemaObject.insert( u"type"_s, u"object"_s );
        schemaObject.insert( u"properties"_s, properties );

        QJsonArray required;
        required.append( u"text"_s );
        schemaObject.insert( u"required"_s, required );
        return schemaObject;
      }

      QgsAiToolResult execute( const QJsonObject &args ) override
      {
        if ( !args.contains( u"text"_s ) )
          return QgsAiToolResult::error( u"missing 'text'"_s );
        return QgsAiToolResult::ok( args.value( u"text"_s ) );
      }

      bool requiresApproval() const override { return mRequiresApproval; }
      QgsAiToolRiskLevel riskLevel() const override { return mRiskLevel; }
      bool isAvailable() const override { return mAvailable; }
      QString availabilityReason() const override { return u"tool unavailable"_s; }

    private:
      QString mName;
      bool mRequiresApproval;
      bool mAvailable = true;
      QgsAiToolRiskLevel mRiskLevel = QgsAiToolRiskLevel::Low;
  };
} //namespace

class TestQgsAiToolRegistry : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void registerAndLookup();
    void rejectsDuplicateNames();
    void rejectsEmptyNameAndNull();
    void schemasJsonContainsAllTools();
    void schemasJsonFilter();
    void mcpGatewayExecuteRequiresProxy();
    void unavailableToolsAreHiddenAndNotExecuted();
    void unavailableToolReasonsAreReported();
    void executeRoundTrip();
    void registryAuditsRiskyToolMetadataOnly();
    void captureMapCanvasRequiresConsent();
    void captureMapCanvasCreatesCappedPng();
    void runPythonDiagnosticsAreConservative();
    void runPythonFeatureLoopHintDoesNotChangeDiagnosis();
    void setCanvasExtentSetsZoomsAndRollsBack();
    void setCanvasExtentIgnoresEmptyOptionalStrings();
    void addLayerFromFileRejectsUnusableVectors();
    void addLayerFromFileRejectsSidecarFiles();
    void addLayerFromFileContextQualityCheckKeepsInterfaceResponsive();
    void addLayerFromServiceLoadsXyzAndRollsBack();
    void styleLayerAppliesNativeChanges();
    void advancedStyleLayerAppliesRenderersLabelsAndRollback();
    void createPrintLayoutAndExportMap();
    void processingToolReportsMissingAlgorithm();
    void processingToolAcceptsJsonEnumAndRunsOffThread();
    void processingToolRunsNoThreadingOnMainThread();
    void processingPrepareFailureIsAnErrorNotACancel();
    void clearEmptiesRegistry();
    void trustGatingHidesRiskyTools();
};

void TestQgsAiToolRegistry::initTestCase()
{
  QgsApplication::initQgis();
  if ( QgsApplication::processingRegistry() && !QgsApplication::processingRegistry()->algorithmById( u"native:serviceareafromlayer"_s ) )
    QgsApplication::processingRegistry()->addProvider( new QgsNativeAlgorithms( QgsApplication::processingRegistry() ) );
}

void TestQgsAiToolRegistry::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsAiToolRegistry::registerAndLookup()
{
  QgsAiToolRegistry registry;
  QSignalSpy spy( &registry, &QgsAiToolRegistry::toolRegistered );

  QVERIFY( registry.registerTool( std::make_unique<FakeEchoTool>( u"echo"_s ) ) );
  QCOMPARE( registry.count(), 1 );
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.takeFirst().at( 0 ).toString(), u"echo"_s );

  QgsAiTool *tool = registry.find( u"echo"_s );
  QVERIFY( tool );
  QCOMPARE( tool->name(), u"echo"_s );
  QVERIFY( !registry.find( u"missing"_s ) );
  QCOMPARE( registry.toolNames(), QStringList() << u"echo"_s );
}

void TestQgsAiToolRegistry::rejectsDuplicateNames()
{
  QgsAiToolRegistry registry;
  QVERIFY( registry.registerTool( std::make_unique<FakeEchoTool>( u"echo"_s ) ) );
  QVERIFY( !registry.registerTool( std::make_unique<FakeEchoTool>( u"echo"_s ) ) );
  QCOMPARE( registry.count(), 1 );
}

void TestQgsAiToolRegistry::rejectsEmptyNameAndNull()
{
  QgsAiToolRegistry registry;
  QVERIFY( !registry.registerTool( nullptr ) );
  QVERIFY( !registry.registerTool( std::make_unique<FakeEchoTool>( QString() ) ) );
  QCOMPARE( registry.count(), 0 );
}

void TestQgsAiToolRegistry::schemasJsonContainsAllTools()
{
  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<FakeEchoTool>( u"alpha"_s ) );
  registry.registerTool( std::make_unique<FakeEchoTool>( u"beta"_s ) );

  const QJsonArray schemas = registry.schemasJson();
  QCOMPARE( schemas.size(), 2 );

  QSet<QString> names;
  for ( const QJsonValue &value : schemas )
  {
    const QJsonObject obj = value.toObject();
    QVERIFY( obj.contains( u"name"_s ) );
    QVERIFY( obj.contains( u"description"_s ) );
    QVERIFY( obj.contains( u"input_schema"_s ) );
    names.insert( obj.value( u"name"_s ).toString() );
  }
  QVERIFY( names.contains( u"alpha"_s ) );
  QVERIFY( names.contains( u"beta"_s ) );
}

void TestQgsAiToolRegistry::schemasJsonFilter()
{
  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<FakeEchoTool>( u"read_file"_s ) );
  registry.registerTool( std::make_unique<FakeEchoTool>( u"apply_patch"_s, true ) );

  const QJsonArray filtered = registry.schemasJson( QStringList() << u"read_file"_s );
  QCOMPARE( filtered.size(), 1 );
  QCOMPARE( filtered.first().toObject().value( u"name"_s ).toString(), u"read_file"_s );
}

void TestQgsAiToolRegistry::mcpGatewayExecuteRequiresProxy()
{
  QgsAiToolRegistry registry;
  QgsAiManagedMcpTool mcp;
  mcp.name = u"mcp__nominatim__geocode"_s;
  mcp.description = u"Geocode a place"_s;
  mcp.enabled = true;
  registry.setManagedMcpTools( { mcp } );

  const QgsAiToolResult result = registry.execute( u"mcp__nominatim__geocode"_s, QJsonObject() );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"MCP gateway"_s ) );
}

void TestQgsAiToolRegistry::unavailableToolsAreHiddenAndNotExecuted()
{
  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<FakeEchoTool>( u"run_python"_s, true, false ) );

  QVERIFY( registry.availableToolNames().isEmpty() );
  QVERIFY( registry.schemasJson().isEmpty() );

  QJsonObject args;
  args.insert( u"text"_s, u"hello"_s );
  const QgsAiToolResult result = registry.execute( u"run_python"_s, args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"unavailable"_s ) );
}

void TestQgsAiToolRegistry::unavailableToolReasonsAreReported()
{
  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<FakeEchoTool>( u"run_python"_s, true, false ) );
  registry.registerTool( std::make_unique<FakeEchoTool>( u"read_file"_s ) );

  const QMap<QString, QString> reasons = registry.unavailableToolReasons();
  QCOMPARE( reasons.size(), 1 );
  QCOMPARE( reasons.value( u"run_python"_s ), u"tool unavailable"_s );

  const QMap<QString, QString> filtered = registry.unavailableToolReasons( QStringList() << u"read_file"_s );
  QVERIFY( filtered.isEmpty() );
}

void TestQgsAiToolRegistry::executeRoundTrip()
{
  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<FakeEchoTool>( u"echo"_s ) );

  QgsAiTool *tool = registry.find( u"echo"_s );
  QVERIFY( tool );

  QJsonObject args;
  args.insert( u"text"_s, u"hello"_s );
  const QgsAiToolResult result = tool->execute( args );
  QVERIFY( result.success );
  QCOMPARE( result.output.toString(), u"hello"_s );

  const QgsAiToolResult missing = tool->execute( QJsonObject() );
  QVERIFY( !missing.success );
  QVERIFY( !missing.errorMessage.isEmpty() );
}

void TestQgsAiToolRegistry::registryAuditsRiskyToolMetadataOnly()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  const QString auditPath = tempDir.filePath( u"audit.log"_s );
  QgsAiAuditLog::setFilePathOverride( auditPath );
  const auto cleanup = qScopeGuard( []() { QgsAiAuditLog::setFilePathOverride( QString() ); } );

  QgsAiToolRegistry registry;
  QVERIFY( registry.registerTool( std::make_unique<FakeEchoTool>( u"mutating_tool"_s, true, QgsAiToolRiskLevel::Medium ) ) );

  QJsonObject args;
  args.insert( u"text"_s, u"secret payload that must not be logged"_s );
  const QgsAiToolResult result = registry.execute( u"mutating_tool"_s, args );
  QVERIFY( result.success );

  QFile file( auditPath );
  QVERIFY( file.open( QIODevice::ReadOnly | QIODevice::Text ) );
  const QString line = QString::fromUtf8( file.readAll() );
  QVERIFY2( line.contains( u"| tool_event |"_s ), qPrintable( line ) );
  QVERIFY( line.contains( u"event=execute"_s ) );
  QVERIFY( line.contains( u"tool=mutating_tool"_s ) );
  QVERIFY( line.contains( u"risk=medium"_s ) );
  QVERIFY( line.contains( u"success=true"_s ) );
  QVERIFY( line.contains( u"args_sha256"_s ) );
  QVERIFY( !line.contains( u"secret payload"_s ) );
}

void TestQgsAiToolRegistry::captureMapCanvasRequiresConsent()
{
  QgsSettings settings;
  settings.remove( u"strata/visual_context/image_send_consent"_s );
  settings.remove( u"geoai/visual_context/image_send_consent"_s );

  QgsMapCanvas canvas;
  QgsAiCaptureMapCanvasTool tool( &canvas );
  const QgsAiToolResult result = tool.execute( QJsonObject() );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"consent"_s, Qt::CaseInsensitive ) );
}

void TestQgsAiToolRegistry::captureMapCanvasCreatesCappedPng()
{
  QgsSettings settings;
  settings.setValue( u"strata/visual_context/image_send_consent"_s, true );

  QgsMapCanvas canvas;
  canvas.resize( 2000, 1000 );
  canvas.setDestinationCrs( QgsCoordinateReferenceSystem( u"EPSG:4326"_s ) );
  canvas.setExtent( QgsRectangle( 0, 0, 10, 5 ) );

  QgsAiCaptureMapCanvasTool tool( &canvas );
  QJsonObject args;
  args.insert( u"max_longest_side"_s, 500 );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( result.success );

  const QJsonObject output = result.output.toObject();
  const QJsonObject imageJson = output.value( u"image"_s ).toObject();
  const QString path = imageJson.value( u"path"_s ).toString();
  QVERIFY( QFileInfo::exists( path ) );
  QCOMPARE( imageJson.value( u"mime_type"_s ).toString(), u"image/png"_s );

  const QImage image( path );
  QVERIFY( !image.isNull() );
  QVERIFY( image.width() <= 500 );
  QVERIFY( image.height() <= 500 );
  QCOMPARE( image.width(), imageJson.value( u"width"_s ).toInt() );
  QCOMPARE( image.height(), imageJson.value( u"height"_s ).toInt() );

  settings.remove( u"strata/visual_context/image_send_consent"_s );
  settings.remove( u"geoai/visual_context/image_send_consent"_s );
}

void TestQgsAiToolRegistry::runPythonDiagnosticsAreConservative()
{
  QJsonObject diagnosis = QgsAiRunPythonTool::diagnoseCapturedOutput( u"Processed field named error_code successfully."_s, u"warning: error budget is low"_s, QString() );
  QCOMPARE( diagnosis.value( u"status"_s ).toString(), u"ok"_s );
  QVERIFY( diagnosis.value( u"diagnostics"_s ).toArray().isEmpty() );

  diagnosis = QgsAiRunPythonTool::diagnoseCapturedOutput( QString(), QString(), u"Traceback (most recent call last):\nValueError: bad value"_s, u"ValueError"_s, u"bad value"_s );
  QCOMPARE( diagnosis.value( u"failure_code"_s ).toString(), u"python_exception"_s );
  QCOMPARE( diagnosis.value( u"exception_type"_s ).toString(), u"ValueError"_s );

  diagnosis = QgsAiRunPythonTool::diagnoseCapturedOutput( u"<ServiceExceptionReport><ServiceException code=\"LayerNotDefined\">missing</ServiceException></ServiceExceptionReport>"_s, QString(), QString() );
  QCOMPARE( diagnosis.value( u"failure_code"_s ).toString(), u"service_exception"_s );

  diagnosis = QgsAiRunPythonTool::diagnoseCapturedOutput( QString(), u"Provider is not valid for this URI"_s, QString() );
  QCOMPARE( diagnosis.value( u"failure_code"_s ).toString(), u"invalid_provider"_s );

  diagnosis = QgsAiRunPythonTool::diagnoseCapturedOutput( u"{\"success\":false,\"message\":\"No output layer was created\"}"_s, QString(), QString() );
  QCOMPARE( diagnosis.value( u"failure_code"_s ).toString(), u"explicit_failure"_s );
  QCOMPARE( diagnosis.value( u"failure_message"_s ).toString(), u"No output layer was created"_s );
}

void TestQgsAiToolRegistry::runPythonFeatureLoopHintDoesNotChangeDiagnosis()
{
  QCOMPARE( QgsAiRunPythonTool::featureLoopHints( u"print('ok')"_s ), QStringList() );
  QCOMPARE( QgsAiRunPythonTool::featureLoopHints( u"for f in layer.getFeatures():\n    print(f.id())"_s ), QStringList { u"slow_feature_loop"_s } );

  QJsonObject diagnosis = QgsAiRunPythonTool::diagnoseCapturedOutput( u"ok"_s, QString(), QString() );
  QCOMPARE( diagnosis.value( u"status"_s ).toString(), u"ok"_s );
  QVERIFY( !diagnosis.contains( u"hints"_s ) );
}

void TestQgsAiToolRegistry::setCanvasExtentSetsZoomsAndRollsBack()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );

  QgsFeature first( layer->fields() );
  first.setAttribute( u"name"_s, u"Alpha"_s );
  first.setGeometry( QgsGeometry::fromWkt( u"Point(20 20)"_s ) );
  QgsFeature second( layer->fields() );
  second.setAttribute( u"name"_s, u"Beta"_s );
  second.setGeometry( QgsGeometry::fromWkt( u"Point(30 25)"_s ) );
  QgsFeatureList features;
  features << first << second;
  QVERIFY( layer->dataProvider()->addFeatures( features ) );
  layer->updateExtents();
  project.addMapLayer( layer );

  QgsMapCanvas canvas;
  canvas.resize( 640, 360 );
  canvas.setDestinationCrs( QgsCoordinateReferenceSystem( u"EPSG:4326"_s ) );
  canvas.setExtent( QgsRectangle( 0, 0, 10, 10 ) );
  canvas.setLayers( QList<QgsMapLayer *>() << layer );
  const QgsRectangle originalExtent = canvas.extent();

  QgsAiSetCanvasExtentTool tool( &canvas, &project );
  QVERIFY( tool.requiresApproval() );
  QCOMPARE( tool.riskLevel(), QgsAiToolRiskLevel::Low );

  QJsonObject extent;
  extent.insert( u"xmin"_s, 1 );
  extent.insert( u"ymin"_s, 2 );
  extent.insert( u"xmax"_s, 5 );
  extent.insert( u"ymax"_s, 6 );
  QJsonObject args;
  args.insert( u"extent"_s, extent );
  args.insert( u"crs"_s, u"EPSG:4326"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonObject output = result.output.toObject();
  const QString rollbackToken = output.value( u"rollback_token"_s ).toString();
  QVERIFY( !rollbackToken.isEmpty() );
  QVERIFY( output.contains( u"diff"_s ) );
  QCOMPARE( canvas.mapSettings().destinationCrs().authid(), u"EPSG:4326"_s );
  QGSCOMPARENEAR( canvas.extent().center().x(), 3.0, 0.0001 );
  QGSCOMPARENEAR( canvas.extent().center().y(), 4.0, 0.0001 );
  QVERIFY( canvas.extent().contains( QgsRectangle( 1, 2, 5, 6 ) ) );

  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, rollbackToken );
  const QgsAiToolResult rollback = tool.execute( rollbackArgs );
  QVERIFY2( rollback.success, qPrintable( rollback.errorMessage ) );
  QGSCOMPARENEAR( canvas.extent().center().x(), originalExtent.center().x(), 0.0001 );
  QGSCOMPARENEAR( canvas.extent().center().y(), originalExtent.center().y(), 0.0001 );

  QJsonObject zoomLayerArgs;
  zoomLayerArgs.insert( u"zoom_to_layer"_s, layer->id() );
  const QgsAiToolResult zoomLayer = tool.execute( zoomLayerArgs );
  QVERIFY2( zoomLayer.success, qPrintable( zoomLayer.errorMessage ) );
  QVERIFY( canvas.extent().contains( layer->extent() ) );

  QVERIFY( !features.isEmpty() );
  layer->selectByIds( QgsFeatureIds() << features.constFirst().id() );
  QJsonObject zoomSelectionArgs;
  zoomSelectionArgs.insert( u"zoom_to_selection"_s, layer->id() );
  const QgsAiToolResult zoomSelection = tool.execute( zoomSelectionArgs );
  QVERIFY2( zoomSelection.success, qPrintable( zoomSelection.errorMessage ) );
  QVERIFY( canvas.extent().contains( QgsRectangle( 20, 20, 20, 20 ) ) );
}

void TestQgsAiToolRegistry::setCanvasExtentIgnoresEmptyOptionalStrings()
{
  QgsProject project;
  QgsMapCanvas canvas;
  canvas.resize( 640, 360 );
  canvas.setDestinationCrs( QgsCoordinateReferenceSystem( u"EPSG:4326"_s ) );
  canvas.setExtent( QgsRectangle( 0, 0, 10, 10 ) );

  QgsAiSetCanvasExtentTool tool( &canvas, &project );
  QJsonObject args;
  args.insert( u"crs"_s, QString() );
  args.insert( u"zoom_to_layer"_s, u"  "_s );
  args.insert( u"zoom_to_selection"_s, QJsonValue::Null );
  args.insert( u"scale"_s, 2500 );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QGSCOMPARENEAR( canvas.scale(), 2500.0, 0.1 );

  QJsonObject invalidSelection;
  invalidSelection.insert( u"zoom_to_selection"_s, false );
  const QgsAiToolResult invalidResult = tool.execute( invalidSelection );
  QVERIFY( !invalidResult.success );
  QVERIFY( invalidResult.errorMessage.contains( u"zoom_to_selection"_s ) );
}

void TestQgsAiToolRegistry::addLayerFromFileRejectsUnusableVectors()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QFile emptyGeoJson( tempDir.filePath( u"empty.geojson"_s ) );
  QVERIFY( emptyGeoJson.open( QIODevice::WriteOnly | QIODevice::Text ) );
  QVERIFY( emptyGeoJson.write( R"({"type":"FeatureCollection","features":[]})" ) > 0 );
  emptyGeoJson.close();

  QFile geometrylessGeoJson( tempDir.filePath( u"geometryless.geojson"_s ) );
  QVERIFY( geometrylessGeoJson.open( QIODevice::WriteOnly | QIODevice::Text ) );
  QVERIFY( geometrylessGeoJson.write( R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"id":1},"geometry":null}]})" ) > 0 );
  geometrylessGeoJson.close();

  QFile tableCsv( tempDir.filePath( u"table.csv"_s ) );
  QVERIFY( tableCsv.open( QIODevice::WriteOnly | QIODevice::Text ) );
  QVERIFY( tableCsv.write( "id,name\n1,Alpha\n" ) > 0 );
  tableCsv.close();

  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsProject project;
  QgsAiAddLayerFromFileTool tool( &contextProvider, &project );

  QJsonObject emptyArgs;
  emptyArgs.insert( u"path"_s, u"empty.geojson"_s );
  const QgsAiToolResult emptyResult = tool.execute( emptyArgs );
  QVERIFY( !emptyResult.success );
  QVERIFY( emptyResult.errorMessage.contains( u"zero features"_s ) );
  QCOMPARE( project.mapLayers().size(), 0 );

  QJsonObject geometrylessArgs;
  geometrylessArgs.insert( u"path"_s, u"geometryless.geojson"_s );
  const QgsAiToolResult geometrylessResult = tool.execute( geometrylessArgs );
  QVERIFY( !geometrylessResult.success );
  QVERIFY( geometrylessResult.errorMessage.contains( u"no geometry"_s ) );
  QCOMPARE( project.mapLayers().size(), 0 );

  QJsonObject tableArgs;
  tableArgs.insert( u"path"_s, u"table.csv"_s );
  const QgsAiToolResult tableResult = tool.execute( tableArgs );
  QVERIFY2( tableResult.success, qPrintable( tableResult.errorMessage ) );
  const QJsonObject output = tableResult.output.toObject();
  QCOMPARE( output.value( u"feature_count"_s ).toVariant().toLongLong(), 1 );
  QCOMPARE( output.value( u"spatial"_s ).toBool(), false );
  QVERIFY( output.value( u"extent"_s ).isNull() );
  QCOMPARE( project.mapLayers().size(), 1 );
}

void TestQgsAiToolRegistry::addLayerFromFileRejectsSidecarFiles()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QFile geojson( tempDir.filePath( u"layer.geojson"_s ) );
  QVERIFY( geojson.open( QIODevice::WriteOnly ) );
  QVERIFY( geojson.write( R"({"type":"FeatureCollection","features":[]})" ) > 0 );
  geojson.close();
  QFile qmd( tempDir.filePath( u"layer.qmd"_s ) );
  QVERIFY( qmd.open( QIODevice::WriteOnly ) );
  QVERIFY( qmd.write( "<qgis/>" ) > 0 );
  qmd.close();
  QFile qml( tempDir.filePath( u"layer.qml"_s ) );
  QVERIFY( qml.open( QIODevice::WriteOnly ) );
  QVERIFY( qml.write( "<qgis/>" ) > 0 );
  qml.close();

  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsProject project;
  QgsAiAddLayerFromFileTool tool( &contextProvider, &project );

  QJsonObject qmdArgs;
  qmdArgs.insert( u"path"_s, u"layer.qmd"_s );
  const QgsAiToolResult qmdResult = tool.execute( qmdArgs );
  QVERIFY( !qmdResult.success );
  QVERIFY( qmdResult.errorMessage.contains( u"sidecar"_s ) );
  QVERIFY( qmdResult.errorMessage.contains( u"layer.geojson"_s ) );
  QCOMPARE( project.mapLayers().size(), 0 );

  QJsonObject qmlArgs;
  qmlArgs.insert( u"path"_s, u"layer.qml"_s );
  const QgsAiToolResult qmlResult = tool.execute( qmlArgs );
  QVERIFY( !qmlResult.success );
  QVERIFY( qmlResult.errorMessage.contains( u"style"_s ) );
  QVERIFY( qmlResult.errorMessage.contains( u"layer.geojson"_s ) );
  QCOMPARE( project.mapLayers().size(), 0 );

  // The suggestion prefers the primary dataset over a same-named .csv export.
  for ( const QString &name : { u"roads.csv"_s, u"roads.shp"_s, u"roads.qmd"_s } )
  {
    QFile file( tempDir.filePath( name ) );
    QVERIFY( file.open( QIODevice::WriteOnly ) );
    QVERIFY( file.write( "x" ) > 0 );
  }
  QJsonObject roadsArgs;
  roadsArgs.insert( u"path"_s, u"roads.qmd"_s );
  const QgsAiToolResult roadsResult = tool.execute( roadsArgs );
  QVERIFY( !roadsResult.success );
  QVERIFY2( roadsResult.errorMessage.contains( u"Open 'roads.shp' instead"_s ), qPrintable( roadsResult.errorMessage ) );
  QCOMPARE( project.mapLayers().size(), 0 );
}

void TestQgsAiToolRegistry::addLayerFromFileContextQualityCheckKeepsInterfaceResponsive()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QFile geojson( tempDir.filePath( u"trees.geojson"_s ) );
  QVERIFY( geojson.open( QIODevice::WriteOnly | QIODevice::Text ) );
  QByteArray body = R"({"type":"FeatureCollection","features":[)";
  for ( int i = 0; i < 2500; ++i )
  {
    if ( i > 0 )
      body += ',';
    body += QByteArray( R"({"type":"Feature","properties":{"estimate":1,"context":"park"},"geometry":{"type":"Point","coordinates":[)" ) + QByteArray::number( i ) + ",0]}}";
  }
  body += "]}";
  QVERIFY( geojson.write( body ) > 0 );
  geojson.close();

  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsProject project;
  QgsAiAddLayerFromFileTool tool( &contextProvider, &project );
  QJsonObject args;
  args.insert( u"path"_s, u"trees.geojson"_s );

  bool interfaceEventsRan = false;
  QTimer interfaceTimer;
  interfaceTimer.setSingleShot( true );
  QObject::connect( &interfaceTimer, &QTimer::timeout, &interfaceTimer, [&interfaceEventsRan]() { interfaceEventsRan = true; } );
  interfaceTimer.start( 0 );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QVERIFY2( interfaceEventsRan, "add_layer quality check blocked the interface thread" );
  QCOMPARE( result.output.toObject().value( u"quality_checks"_s ).toObject().value( u"context_values_valid"_s ).toBool(), true );
  QCOMPARE( project.mapLayers().size(), 1 );
}

void TestQgsAiToolRegistry::addLayerFromServiceLoadsXyzAndRollsBack()
{
  QgsProject project;
  QgsAiAddLayerFromServiceTool tool( &project );
  QVERIFY( tool.requiresApproval() );
  QCOMPARE( tool.riskLevel(), QgsAiToolRiskLevel::High );

  QJsonObject args;
  args.insert( u"provider"_s, u"xyz"_s );
  args.insert( u"uri"_s, u"type=xyz&url=file://tile.openstreetmap.org/%7Bz%7D/%7Bx%7D/%7By%7D.png&zmax=19&zmin=0"_s );
  args.insert( u"name"_s, u"Local XYZ"_s );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonObject output = result.output.toObject();
  const QString layerId = output.value( u"layer_id"_s ).toString();
  QVERIFY( !layerId.isEmpty() );
  QCOMPARE( output.value( u"provider"_s ).toString(), u"xyz"_s );
  QCOMPARE( output.value( u"provider_key"_s ).toString(), u"wms"_s );
  QVERIFY( output.contains( u"diff"_s ) );
  QCOMPARE( project.mapLayers().size(), 1 );
  QVERIFY( project.mapLayer( layerId ) );

  const QString rollbackToken = output.value( u"rollback_token"_s ).toString();
  QVERIFY( !rollbackToken.isEmpty() );
  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, rollbackToken );
  const QgsAiToolResult rollback = tool.execute( rollbackArgs );
  QVERIFY2( rollback.success, qPrintable( rollback.errorMessage ) );
  QCOMPARE( project.mapLayers().size(), 0 );
}

void TestQgsAiToolRegistry::styleLayerAppliesNativeChanges()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string"_s, u"Points"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  project.addMapLayer( layer );

  QgsAiStyleLayerTool tool( &project );
  QVERIFY( tool.requiresApproval() );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"opacity"_s, 0.35 );
  args.insert( u"visible"_s, false );
  args.insert( u"color"_s, u"#ff0000"_s );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QString rollbackToken = result.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !rollbackToken.isEmpty() );
  QVERIFY( result.output.toObject().contains( u"diff"_s ) );
  QVERIFY( std::abs( layer->opacity() - 0.35 ) < 0.001 );

  QgsLayerTreeLayer *node = project.layerTreeRoot()->findLayer( layer->id() );
  QVERIFY( node );
  QVERIFY( !node->itemVisibilityChecked() );

  QgsSingleSymbolRenderer *renderer = dynamic_cast<QgsSingleSymbolRenderer *>( layer->renderer() );
  QVERIFY( renderer );
  QCOMPARE( renderer->symbol()->color(), QColor( u"#ff0000"_s ) );

  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, rollbackToken );
  const QgsAiToolResult rollback = tool.execute( rollbackArgs );
  QVERIFY2( rollback.success, qPrintable( rollback.errorMessage ) );
  QVERIFY( std::abs( layer->opacity() - 1.0 ) < 0.001 );
  QVERIFY( node->itemVisibilityChecked() );
}

void TestQgsAiToolRegistry::advancedStyleLayerAppliesRenderersLabelsAndRollback()
{
  QgsProject project;
  QgsVectorLayer *categorizedLayer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=category:string&field=name:string"_s, u"Categories"_s, u"memory"_s );
  QVERIFY( categorizedLayer->isValid() );

  QgsFeature first( categorizedLayer->fields() );
  first.setAttribute( u"category"_s, u"A"_s );
  first.setAttribute( u"name"_s, u"Alpha"_s );
  QgsFeature second( categorizedLayer->fields() );
  second.setAttribute( u"category"_s, u"B"_s );
  second.setAttribute( u"name"_s, u"Beta"_s );
  QgsFeature third( categorizedLayer->fields() );
  third.setAttribute( u"category"_s, u"A"_s );
  third.setAttribute( u"name"_s, u"Again"_s );
  QVERIFY( categorizedLayer->dataProvider()->addFeatures( QgsFeatureList() << first << second << third ) );
  project.addMapLayer( categorizedLayer );

  QgsAiAdvancedStyleTool tool( &project );
  QVERIFY( tool.requiresApproval() );
  QCOMPARE( tool.riskLevel(), QgsAiToolRiskLevel::Medium );

  QJsonObject labels;
  labels.insert( u"enabled"_s, true );
  labels.insert( u"field"_s, u"name"_s );

  QJsonObject categorizedArgs;
  categorizedArgs.insert( u"layer_id"_s, categorizedLayer->id() );
  categorizedArgs.insert( u"renderer"_s, u"categorized"_s );
  categorizedArgs.insert( u"field"_s, u"category"_s );
  categorizedArgs.insert( u"labels"_s, labels );
  const QgsAiToolResult categorizedResult = tool.execute( categorizedArgs );
  QVERIFY2( categorizedResult.success, qPrintable( categorizedResult.errorMessage ) );
  const QString categorizedRollbackToken = categorizedResult.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !categorizedRollbackToken.isEmpty() );

  QgsCategorizedSymbolRenderer *categorizedRenderer = dynamic_cast<QgsCategorizedSymbolRenderer *>( categorizedLayer->renderer() );
  QVERIFY( categorizedRenderer );
  QCOMPARE( categorizedRenderer->categories().size(), 2 );
  QVERIFY( categorizedLayer->labelsEnabled() );
  QVERIFY( categorizedLayer->labeling() );
  QCOMPARE( categorizedLayer->labeling()->settings().fieldName, u"name"_s );

  QJsonObject categorizedRollbackArgs;
  categorizedRollbackArgs.insert( u"rollback_token"_s, categorizedRollbackToken );
  const QgsAiToolResult categorizedRollback = tool.execute( categorizedRollbackArgs );
  QVERIFY2( categorizedRollback.success, qPrintable( categorizedRollback.errorMessage ) );
  QVERIFY( dynamic_cast<QgsSingleSymbolRenderer *>( categorizedLayer->renderer() ) );
  QVERIFY( !categorizedLayer->labelsEnabled() );

  QgsVectorLayer *graduatedLayer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=value:double"_s, u"Values"_s, u"memory"_s );
  QVERIFY( graduatedLayer->isValid() );
  QgsFeature low( graduatedLayer->fields() );
  low.setAttribute( u"value"_s, 1.0 );
  QgsFeature mid( graduatedLayer->fields() );
  mid.setAttribute( u"value"_s, 5.0 );
  QgsFeature high( graduatedLayer->fields() );
  high.setAttribute( u"value"_s, 9.0 );
  QVERIFY( graduatedLayer->dataProvider()->addFeatures( QgsFeatureList() << low << mid << high ) );
  project.addMapLayer( graduatedLayer );

  QJsonObject graduatedArgs;
  graduatedArgs.insert( u"layer_id"_s, graduatedLayer->id() );
  graduatedArgs.insert( u"renderer"_s, u"graduated"_s );
  graduatedArgs.insert( u"field"_s, u"value"_s );
  graduatedArgs.insert( u"classes"_s, 3 );
  const QgsAiToolResult graduatedResult = tool.execute( graduatedArgs );
  QVERIFY2( graduatedResult.success, qPrintable( graduatedResult.errorMessage ) );
  const QString graduatedRollbackToken = graduatedResult.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !graduatedRollbackToken.isEmpty() );

  QgsGraduatedSymbolRenderer *graduatedRenderer = dynamic_cast<QgsGraduatedSymbolRenderer *>( graduatedLayer->renderer() );
  QVERIFY( graduatedRenderer );
  QCOMPARE( graduatedRenderer->ranges().size(), 3 );

  QJsonObject graduatedRollbackArgs;
  graduatedRollbackArgs.insert( u"rollback_token"_s, graduatedRollbackToken );
  const QgsAiToolResult graduatedRollback = tool.execute( graduatedRollbackArgs );
  QVERIFY2( graduatedRollback.success, qPrintable( graduatedRollback.errorMessage ) );
  QVERIFY( dynamic_cast<QgsSingleSymbolRenderer *>( graduatedLayer->renderer() ) );
}

void TestQgsAiToolRegistry::createPrintLayoutAndExportMap()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string"_s, u"Points"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  project.addMapLayer( layer );

  QgsMapCanvas canvas;
  canvas.resize( 640, 360 );
  canvas.setDestinationCrs( QgsCoordinateReferenceSystem( u"EPSG:4326"_s ) );
  canvas.setExtent( QgsRectangle( -10, -5, 10, 5 ) );
  canvas.setLayers( QList<QgsMapLayer *>() << layer );

  QgsAiCreatePrintLayoutTool createTool( &project, &canvas );
  QVERIFY( createTool.requiresApproval() );

  QJsonObject createArgs;
  createArgs.insert( u"name"_s, u"AI Layout"_s );
  createArgs.insert( u"title"_s, u"Project map"_s );
  const QgsAiToolResult createResult = createTool.execute( createArgs );
  QVERIFY2( createResult.success, qPrintable( createResult.errorMessage ) );
  const QString layoutRollbackToken = createResult.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !layoutRollbackToken.isEmpty() );
  QVERIFY( createResult.output.toObject().contains( u"diff"_s ) );
  QVERIFY( project.layoutManager()->layoutByName( u"AI Layout"_s ) );

  QgsPrintLayout *layout = dynamic_cast<QgsPrintLayout *>( project.layoutManager()->layoutByName( u"AI Layout"_s ) );
  QVERIFY( layout );
  QCOMPARE( layout->pageCollection()->pageCount(), 1 );

  QgsAiEditPrintLayoutTool editLayoutTool( &project );
  QVERIFY( editLayoutTool.requiresApproval() );
  QCOMPARE( editLayoutTool.riskLevel(), QgsAiToolRiskLevel::Medium );

  QJsonObject editLayoutArgs;
  editLayoutArgs.insert( u"layout_name"_s, u"AI Layout"_s );
  editLayoutArgs.insert( u"add_legend"_s, true );
  editLayoutArgs.insert( u"add_scalebar"_s, true );
  editLayoutArgs.insert( u"add_north_arrow"_s, true );
  editLayoutArgs.insert( u"add_page"_s, true );
  const QgsAiToolResult editLayout = editLayoutTool.execute( editLayoutArgs );
  QVERIFY2( editLayout.success, qPrintable( editLayout.errorMessage ) );
  const QString editLayoutRollbackToken = editLayout.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !editLayoutRollbackToken.isEmpty() );
  QCOMPARE( layout->pageCollection()->pageCount(), 2 );

  QList<QgsLayoutItemLegend *> legends;
  layout->layoutItems<QgsLayoutItemLegend>( legends );
  QCOMPARE( legends.size(), 1 );
  QList<QgsLayoutItemScaleBar *> scaleBars;
  layout->layoutItems<QgsLayoutItemScaleBar>( scaleBars );
  QCOMPARE( scaleBars.size(), 1 );
  QList<QgsLayoutItemPicture *> pictures;
  layout->layoutItems<QgsLayoutItemPicture>( pictures );
  QCOMPARE( pictures.size(), 1 );
  QCOMPARE( pictures.constFirst()->picturePath(), u":/images/north_arrows/layout_default_north_arrow.svg"_s );

  QJsonObject editLayoutRollbackArgs;
  editLayoutRollbackArgs.insert( u"rollback_token"_s, editLayoutRollbackToken );
  const QgsAiToolResult editLayoutRollback = editLayoutTool.execute( editLayoutRollbackArgs );
  QVERIFY2( editLayoutRollback.success, qPrintable( editLayoutRollback.errorMessage ) );
  QCOMPARE( layout->pageCollection()->pageCount(), 1 );
  layout->layoutItems<QgsLayoutItemLegend>( legends );
  QCOMPARE( legends.size(), 0 );
  layout->layoutItems<QgsLayoutItemScaleBar>( scaleBars );
  QCOMPARE( scaleBars.size(), 0 );
  layout->layoutItems<QgsLayoutItemPicture>( pictures );
  QCOMPARE( pictures.size(), 0 );

  QgsAiExportMapTool exportTool( &contextProvider, &project, &canvas );
  QVERIFY( exportTool.requiresApproval() );

  QJsonObject canvasExportArgs;
  canvasExportArgs.insert( u"path"_s, u"exports/canvas.png"_s );
  canvasExportArgs.insert( u"format"_s, u"png"_s );
  canvasExportArgs.insert( u"width"_s, 320 );
  canvasExportArgs.insert( u"height"_s, 180 );
  const QgsAiToolResult canvasExport = exportTool.execute( canvasExportArgs );
  QVERIFY2( canvasExport.success, qPrintable( canvasExport.errorMessage ) );
  const QString canvasPath = canvasExport.output.toObject().value( u"absolute_path"_s ).toString();
  const QString canvasRollbackToken = canvasExport.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !canvasRollbackToken.isEmpty() );
  QVERIFY( canvasExport.output.toObject().contains( u"diff"_s ) );
  QVERIFY( QFileInfo::exists( canvasPath ) );
  const QImage canvasImage( canvasPath );
  QVERIFY( !canvasImage.isNull() );
  QCOMPARE( canvasImage.size(), QSize( 320, 180 ) );

  QJsonObject layoutExportArgs;
  layoutExportArgs.insert( u"path"_s, u"exports/layout.png"_s );
  layoutExportArgs.insert( u"format"_s, u"png"_s );
  layoutExportArgs.insert( u"layout_name"_s, u"AI Layout"_s );
  const QgsAiToolResult layoutExport = exportTool.execute( layoutExportArgs );
  QVERIFY2( layoutExport.success, qPrintable( layoutExport.errorMessage ) );
  const QString layoutPath = layoutExport.output.toObject().value( u"absolute_path"_s ).toString();
  const QString layoutExportRollbackToken = layoutExport.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !layoutExportRollbackToken.isEmpty() );
  QVERIFY( QFileInfo::exists( layoutPath ) );
  QVERIFY( QFileInfo( layoutPath ).size() > 0 );

  QJsonObject canvasRollbackArgs;
  canvasRollbackArgs.insert( u"rollback_token"_s, canvasRollbackToken );
  const QgsAiToolResult canvasRollback = exportTool.execute( canvasRollbackArgs );
  QVERIFY2( canvasRollback.success, qPrintable( canvasRollback.errorMessage ) );
  QVERIFY( !QFileInfo::exists( canvasPath ) );

  QJsonObject layoutExportRollbackArgs;
  layoutExportRollbackArgs.insert( u"rollback_token"_s, layoutExportRollbackToken );
  const QgsAiToolResult layoutExportRollback = exportTool.execute( layoutExportRollbackArgs );
  QVERIFY2( layoutExportRollback.success, qPrintable( layoutExportRollback.errorMessage ) );
  QVERIFY( !QFileInfo::exists( layoutPath ) );

  QJsonObject layoutRollbackArgs;
  layoutRollbackArgs.insert( u"rollback_token"_s, layoutRollbackToken );
  const QgsAiToolResult layoutRollback = createTool.execute( layoutRollbackArgs );
  QVERIFY2( layoutRollback.success, qPrintable( layoutRollback.errorMessage ) );
  QVERIFY( !project.layoutManager()->layoutByName( u"AI Layout"_s ) );
}

void TestQgsAiToolRegistry::processingToolReportsMissingAlgorithm()
{
  QgsProject project;
  QgsAiRunProcessingAlgorithmTool tool( &project );
  QVERIFY( tool.requiresApproval() );

  QJsonObject args;
  args.insert( u"algorithm_id"_s, u"strata:missing_algorithm"_s );
  args.insert( u"parameters"_s, QJsonObject() );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"Unknown Processing algorithm"_s ) || result.errorMessage.contains( u"not available"_s ) );
}

void TestQgsAiToolRegistry::processingToolAcceptsJsonEnumAndRunsOffThread()
{
  if ( !QgsApplication::processingRegistry()->providerById( u"native"_s ) )
    QgsApplication::processingRegistry()->addProvider( new QgsNativeAlgorithms( QgsApplication::processingRegistry() ) );

  QgsProject project;
  project.setCrs( QgsCoordinateReferenceSystem( u"EPSG:3857"_s ) );
  QgsAiRunProcessingAlgorithmTool tool( &project );

  const auto parameterByName = []( const QJsonArray &parameters, const QString &name ) {
    for ( const QJsonValue &value : parameters )
    {
      const QJsonObject object = value.toObject();
      if ( object.value( u"name"_s ).toString() == name )
        return object;
    }
    return QJsonObject();
  };

  QJsonObject serviceAreaDryRun;
  serviceAreaDryRun.insert( u"algorithm_id"_s, u"native:serviceareafromlayer"_s );
  serviceAreaDryRun.insert( u"dry_run"_s, true );
  const QgsAiToolResult serviceAreaMetadata = tool.execute( serviceAreaDryRun );
  QVERIFY2( serviceAreaMetadata.success, qPrintable( serviceAreaMetadata.errorMessage ) );
  const QJsonObject strategy = parameterByName( serviceAreaMetadata.output.toObject().value( u"parameters"_s ).toArray(), u"STRATEGY"_s );
  QVERIFY( strategy.value( u"default"_s ).isDouble() );
  QCOMPARE( strategy.value( u"default"_s ).toInt(), 0 );
  QCOMPARE( strategy.value( u"options"_s ).toArray().size(), 2 );
  const QJsonObject outputLinesMeta = parameterByName( serviceAreaMetadata.output.toObject().value( u"parameters"_s ).toArray(), u"OUTPUT_LINES"_s );
  QVERIFY( outputLinesMeta.value( u"create_by_default"_s ).toBool() );

  QJsonObject snapDryRun;
  snapDryRun.insert( u"algorithm_id"_s, u"native:snapgeometries"_s );
  snapDryRun.insert( u"dry_run"_s, true );
  const QgsAiToolResult snapMetadata = tool.execute( snapDryRun );
  QVERIFY2( snapMetadata.success, qPrintable( snapMetadata.errorMessage ) );
  const QJsonObject behavior = parameterByName( snapMetadata.output.toObject().value( u"parameters"_s ).toArray(), u"BEHAVIOR"_s );
  QVERIFY( behavior.value( u"default"_s ).isDouble() );
  QCOMPARE( behavior.value( u"default"_s ).toInt(), 0 );
  QCOMPARE( behavior.value( u"options"_s ).toArray().size(), 8 );

  auto *network = new QgsVectorLayer( u"LineString?crs=EPSG:3857"_s, u"network"_s, u"memory"_s );
  QVERIFY( network->isValid() );
  QgsFeature line( network->fields() );
  line.setGeometry( QgsGeometry::fromWkt( u"LineString (0 0, 100 0)"_s ) );
  QVERIFY( network->dataProvider()->addFeature( line ) );
  network->updateExtents();
  project.addMapLayer( network );

  auto *starts = new QgsVectorLayer( u"Point?crs=EPSG:3857"_s, u"starts"_s, u"memory"_s );
  QVERIFY( starts->isValid() );
  QgsFeature start( starts->fields() );
  start.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 0, 0 ) ) );
  QVERIFY( starts->dataProvider()->addFeature( start ) );
  starts->updateExtents();
  project.addMapLayer( starts );

  QJsonObject serviceAreaParameters;
  serviceAreaParameters.insert( u"INPUT"_s, network->id() );
  serviceAreaParameters.insert( u"START_POINTS"_s, starts->id() );
  serviceAreaParameters.insert( u"STRATEGY"_s, 0 );
  serviceAreaParameters.insert( u"TRAVEL_COST2"_s, 50 );
  serviceAreaParameters.insert( u"DEFAULT_DIRECTION"_s, 2 );
  serviceAreaParameters.insert( u"DEFAULT_SPEED"_s, 50 );
  serviceAreaParameters.insert( u"TOLERANCE"_s, 0 );
  serviceAreaParameters.insert( u"POINT_TOLERANCE"_s, 5 );
  serviceAreaParameters.insert( u"OUTPUT_LINES"_s, u"TEMPORARY_OUTPUT"_s );
  QJsonObject serviceAreaArgs;
  serviceAreaArgs.insert( u"algorithm_id"_s, u"native:serviceareafromlayer"_s );
  serviceAreaArgs.insert( u"parameters"_s, serviceAreaParameters );
  bool interfaceEventsRan = false;
  QTimer interfaceTimer;
  interfaceTimer.setSingleShot( true );
  QObject::connect( &interfaceTimer, &QTimer::timeout, &interfaceTimer, [&interfaceEventsRan]() { interfaceEventsRan = true; } );
  interfaceTimer.start( 0 );
  const QgsAiToolResult serviceArea = tool.execute( serviceAreaArgs );
  QVERIFY2( serviceArea.success, qPrintable( serviceArea.errorMessage ) );
  QVERIFY2( interfaceEventsRan, "Processing blocked the interface thread until the algorithm returned" );
  QVERIFY( serviceArea.output.toObject().value( u"result"_s ).toObject().contains( u"OUTPUT_LINES"_s ) );
  const QJsonArray serviceAreaLoaded = serviceArea.output.toObject().value( u"loaded_layers"_s ).toArray();
  QCOMPARE( serviceAreaLoaded.size(), 1 );
  const QString serviceAreaLayerId = serviceAreaLoaded.at( 0 ).toObject().value( u"id"_s ).toString();
  QVERIFY( project.mapLayer( serviceAreaLayerId ) );
  QCOMPARE( project.mapLayers().size(), 3 );

  QJsonArray behaviorValue;
  behaviorValue.append( 0 );
  QJsonObject snapParameters;
  snapParameters.insert( u"INPUT"_s, starts->id() );
  snapParameters.insert( u"REFERENCE_LAYER"_s, network->id() );
  snapParameters.insert( u"TOLERANCE"_s, 10 );
  snapParameters.insert( u"BEHAVIOR"_s, behaviorValue );
  snapParameters.insert( u"OUTPUT"_s, u"TEMPORARY_OUTPUT"_s );
  QJsonObject snapArgs;
  snapArgs.insert( u"algorithm_id"_s, u"native:snapgeometries"_s );
  snapArgs.insert( u"parameters"_s, snapParameters );
  const QgsAiToolResult snap = tool.execute( snapArgs );
  QVERIFY2( snap.success, qPrintable( snap.errorMessage ) );
  QVERIFY( snap.output.toObject().value( u"result"_s ).toObject().contains( u"OUTPUT"_s ) );
  QCOMPARE( snap.output.toObject().value( u"loaded_layers"_s ).toArray().size(), 1 );
  QCOMPARE( project.mapLayers().size(), 4 );

  QJsonObject bufferParameters;
  bufferParameters.insert( u"INPUT"_s, starts->id() );
  bufferParameters.insert( u"DISTANCE"_s, 10 );
  QJsonObject bufferArgs;
  bufferArgs.insert( u"algorithm_id"_s, u"native:buffer"_s );
  bufferArgs.insert( u"parameters"_s, bufferParameters );
  const QgsAiToolResult buffer = tool.execute( bufferArgs );
  QVERIFY2( buffer.success, qPrintable( buffer.errorMessage ) );
  QCOMPARE( buffer.output.toObject().value( u"loaded_layers"_s ).toArray().size(), 1 );
  QVERIFY( project.mapLayer( buffer.output.toObject().value( u"loaded_layers"_s ).toArray().at( 0 ).toObject().value( u"id"_s ).toString() ) );
  QCOMPARE( project.mapLayers().size(), 5 );
}

void TestQgsAiToolRegistry::processingToolRunsNoThreadingOnMainThread()
{
  if ( !QgsApplication::processingRegistry()->providerById( u"native"_s ) )
    QgsApplication::processingRegistry()->addProvider( new QgsNativeAlgorithms( QgsApplication::processingRegistry() ) );

  QgsProject project;
  auto *points = new QgsVectorLayer( u"Point?crs=EPSG:4326"_s, u"points"_s, u"memory"_s );
  QVERIFY( points->isValid() );
  QgsFeature point( points->fields() );
  point.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( 0, 0 ) ) );
  QVERIFY( points->dataProvider()->addFeature( point ) );
  project.addMapLayer( points );

  auto *poly = new QgsVectorLayer( u"Polygon?crs=EPSG:4326"_s, u"poly"_s, u"memory"_s );
  QVERIFY( poly->isValid() );
  QgsFeature polygon( poly->fields() );
  polygon.setGeometry( QgsGeometry::fromWkt( u"Polygon ((-1 -1, 1 -1, 1 1, -1 1, -1 -1))"_s ) );
  QVERIFY( poly->dataProvider()->addFeature( polygon ) );
  project.addMapLayer( poly );

  QgsAiRunProcessingAlgorithmTool tool( &project );
  QJsonArray predicates;
  predicates.append( 0 );
  QJsonObject parameters;
  parameters.insert( u"INPUT"_s, points->id() );
  parameters.insert( u"INTERSECT"_s, poly->id() );
  parameters.insert( u"PREDICATE"_s, predicates );
  parameters.insert( u"METHOD"_s, 0 );
  QJsonObject args;
  args.insert( u"algorithm_id"_s, u"native:selectbylocation"_s );
  args.insert( u"parameters"_s, parameters );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( points->selectedFeatureCount(), 1 );
}

void TestQgsAiToolRegistry::processingPrepareFailureIsAnErrorNotACancel()
{
  if ( !QgsApplication::processingRegistry()->providerById( u"aitest"_s ) )
    QVERIFY( QgsApplication::processingRegistry()->addProvider( new AiTestProcessingProvider() ) );

  QgsProject project;
  QgsAiRunProcessingAlgorithmTool tool( &project );
  QJsonObject args;
  args.insert( u"algorithm_id"_s, u"aitest:failingprepare"_s );
  args.insert( u"parameters"_s, QJsonObject() );
  const QgsAiToolResult result = tool.execute( args );
  // The model must see the real error and be able to retry: this is not a user Stop.
  QVERIFY( !result.success );
  QVERIFY( !result.canceled );
  QVERIFY2( result.errorMessage.contains( u"prepare boom"_s ), qPrintable( result.errorMessage ) );
}

void TestQgsAiToolRegistry::clearEmptiesRegistry()
{
  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<FakeEchoTool>( u"echo"_s ) );
  QCOMPARE( registry.count(), 1 );
  registry.clear();
  QCOMPARE( registry.count(), 0 );
  QVERIFY( !registry.find( u"echo"_s ) );
}

void TestQgsAiToolRegistry::trustGatingHidesRiskyTools()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<QgsAiDownloadFileTool>( &contextProvider, nullptr ) );

  // Unknown trust state ⇒ restricted: the risky tool is neither advertised nor executable.
  QCOMPARE( QgsAiWorkspaceTrust::state( tempDir.path() ), QgsAiWorkspaceTrust::State::Unknown );
  QVERIFY( !registry.availableToolNames().contains( u"download_file"_s ) );
  QgsAiToolResult blocked = registry.execute( u"download_file"_s, QJsonObject() );
  QVERIFY( !blocked.success );
  QVERIFY2( blocked.errorMessage.contains( u"not trusted"_s ), qPrintable( blocked.errorMessage ) );

  // Trusted ⇒ advertised again.
  QgsAiWorkspaceTrust::setState( tempDir.path(), QgsAiWorkspaceTrust::State::Trusted );
  QVERIFY( registry.availableToolNames().contains( u"download_file"_s ) );

  // Revoking trust hides it once more.
  QgsAiWorkspaceTrust::setState( tempDir.path(), QgsAiWorkspaceTrust::State::Untrusted );
  QVERIFY( !registry.availableToolNames().contains( u"download_file"_s ) );
}

QGSTEST_MAIN( TestQgsAiToolRegistry )
#include "testqgsaitoolregistry.moc"
