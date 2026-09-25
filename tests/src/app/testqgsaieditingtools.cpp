/***************************************************************************
  testqgsaieditingtools.cpp
  --------------------------
  begin                : July 2026
  copyright            : (C) 2026
***************************************************************************/

#include "ai/tools/qgsaieditingtools.h"
#include "ai/tools/qgsaitaskrunner.h"
#include "qgsaitestbackgroundprobe.h"
#include "qgsapplication.h"
#include "qgsfeature.h"
#include "qgsfeaturerequest.h"
#include "qgsfield.h"
#include "qgsgeometry.h"
#include "qgsproject.h"
#include "qgstest.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"
#include "qgsvectorlayerjoininfo.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

using namespace Qt::StringLiterals;

class TestQgsAiEditingTools : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void editFeatureGeometryMovesVertexAndRollsBack();
    void editFeatureGeometrySplitsFeatureAndRollsBack();
    void editFeatureGeometryRejectsInvalidResult();
    void updateFeatureAttributesUpdatesAndRollsBack();
    void updateFeatureAttributesRejectsIncompatibleType();
    void calculateFieldCreatesFieldForFilteredFeaturesAndRollsBack();
    void calculateFieldRejectsInvalidExpression();
    void calculateFieldKeepsInterfaceResponsive();
    void calculateFieldSeesUncommittedEdits();
    void calculateFieldUsesVirtualFields();
    void calculateFieldAbortsWhenLayerChangesDuringRun();
    void calculateFieldCancelLeavesLayerUnchanged();
    void calculateFieldIncludesAddedFeatures();
    void calculateFieldUsesJoinedFields();
    void calculateFieldReportsRemovedLayer();
    void calculateFieldEvaluatesAggregatesOnInterfaceThread();
};

void TestQgsAiEditingTools::initTestCase()
{
  QgsApplication::initQgis();
}

void TestQgsAiEditingTools::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsAiEditingTools::editFeatureGeometryMovesVertexAndRollsBack()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Polygon?crs=EPSG:4326"_s, u"Parcels"_s, u"memory"_s );
  QVERIFY( layer->isValid() );

  QgsFeature feature;
  feature.setGeometry( QgsGeometry::fromWkt( u"Polygon((0 0, 10 0, 10 10, 0 10, 0 0))"_s ) );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << feature ) );
  layer->updateExtents();
  project.addMapLayer( layer );

  QgsFeature storedFeature;
  QVERIFY( layer->getFeatures().nextFeature( storedFeature ) );
  const QgsFeatureId featureId = storedFeature.id();
  const QString originalWkt = storedFeature.geometry().asWkt();

  QgsAiEditFeatureGeometryTool tool( &project );
  QVERIFY( tool.requiresApproval() );
  QCOMPARE( tool.riskLevel(), QgsAiToolRiskLevel::High );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"feature_id"_s, static_cast<qint64>( featureId ) );
  args.insert( u"operation"_s, u"move_vertex"_s );
  args.insert( u"vertex_index"_s, 1 );
  args.insert( u"x"_s, 12.0 );
  args.insert( u"y"_s, 0.0 );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QString rollbackToken = result.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !rollbackToken.isEmpty() );
  QVERIFY( result.output.toObject().contains( u"diff"_s ) );

  QgsFeature editedFeature;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( featureId ) ).nextFeature( editedFeature ) );
  QCOMPARE( editedFeature.geometry().vertexAt( 1 ).x(), 12.0 );
  QCOMPARE( editedFeature.geometry().vertexAt( 1 ).y(), 0.0 );

  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, rollbackToken );
  const QgsAiToolResult rollback = tool.execute( rollbackArgs );
  QVERIFY2( rollback.success, qPrintable( rollback.errorMessage ) );

  QgsFeature rolledBackFeature;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( featureId ) ).nextFeature( rolledBackFeature ) );
  QCOMPARE( rolledBackFeature.geometry().asWkt(), originalWkt );
}

void TestQgsAiEditingTools::editFeatureGeometrySplitsFeatureAndRollsBack()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Polygon?crs=EPSG:4326&field=name:string"_s, u"Parcels"_s, u"memory"_s );
  QVERIFY( layer->isValid() );

  QgsFeature feature( layer->fields() );
  feature.setAttribute( u"name"_s, u"Parcel A"_s );
  feature.setGeometry( QgsGeometry::fromWkt( u"Polygon((0 0, 10 0, 10 10, 0 10, 0 0))"_s ) );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << feature ) );
  layer->updateExtents();
  project.addMapLayer( layer );

  QgsFeature storedFeature;
  QVERIFY( layer->getFeatures().nextFeature( storedFeature ) );
  const QgsFeatureId featureId = storedFeature.id();
  const QString originalWkt = storedFeature.geometry().asWkt();

  QJsonObject start;
  start.insert( u"x"_s, 5.0 );
  start.insert( u"y"_s, -1.0 );
  QJsonObject end;
  end.insert( u"x"_s, 5.0 );
  end.insert( u"y"_s, 11.0 );
  QJsonArray splitLine;
  splitLine << start << end;

  QgsAiEditFeatureGeometryTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"feature_id"_s, static_cast<qint64>( featureId ) );
  args.insert( u"operation"_s, u"split_feature"_s );
  args.insert( u"split_line"_s, splitLine );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( result.output.toObject().value( u"operation"_s ).toString(), u"split_feature"_s );
  const QString rollbackToken = result.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !rollbackToken.isEmpty() );
  QCOMPARE( layer->featureCount(), static_cast<long long>( 2 ) );

  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, rollbackToken );
  const QgsAiToolResult rollback = tool.execute( rollbackArgs );
  QVERIFY2( rollback.success, qPrintable( rollback.errorMessage ) );
  QCOMPARE( layer->featureCount(), static_cast<long long>( 1 ) );

  QgsFeature rolledBackFeature;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( featureId ) ).nextFeature( rolledBackFeature ) );
  QCOMPARE( rolledBackFeature.geometry().asWkt(), originalWkt );
}

void TestQgsAiEditingTools::editFeatureGeometryRejectsInvalidResult()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Polygon?crs=EPSG:4326"_s, u"Parcels"_s, u"memory"_s );
  QVERIFY( layer->isValid() );

  QgsFeature feature;
  feature.setGeometry( QgsGeometry::fromWkt( u"Polygon((0 0, 10 0, 10 10, 0 10, 0 0))"_s ) );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << feature ) );
  layer->updateExtents();
  project.addMapLayer( layer );

  QgsFeature storedFeature;
  QVERIFY( layer->getFeatures().nextFeature( storedFeature ) );
  const QgsFeatureId featureId = storedFeature.id();
  const QString originalWkt = storedFeature.geometry().asWkt();

  QgsAiEditFeatureGeometryTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"feature_id"_s, static_cast<qint64>( featureId ) );
  args.insert( u"operation"_s, u"insert_vertex"_s );
  args.insert( u"before_vertex"_s, 2 );
  args.insert( u"x"_s, 0.0 );
  args.insert( u"y"_s, 10.0 );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"invalid geometry"_s ) );

  QgsFeature unchangedFeature;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( featureId ) ).nextFeature( unchangedFeature ) );
  QCOMPARE( unchangedFeature.geometry().asWkt(), originalWkt );
}

void TestQgsAiEditingTools::updateFeatureAttributesUpdatesAndRollsBack()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string&field=height:double"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );

  QgsFeature feature( layer->fields() );
  feature.setGeometry( QgsGeometry::fromWkt( u"Point(1 2)"_s ) );
  feature.setAttribute( u"name"_s, u"old"_s );
  feature.setAttribute( u"height"_s, 1.5 );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << feature ) );
  project.addMapLayer( layer );

  QgsFeature storedFeature;
  QVERIFY( layer->getFeatures().nextFeature( storedFeature ) );
  const QgsFeatureId featureId = storedFeature.id();

  QgsAiUpdateFeatureAttributesTool tool( &project );
  QVERIFY( tool.requiresApproval() );
  QCOMPARE( tool.riskLevel(), QgsAiToolRiskLevel::High );

  QJsonObject attributes;
  attributes.insert( u"name"_s, u"new"_s );
  attributes.insert( u"height"_s, 7.25 );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"feature_id"_s, static_cast<qint64>( featureId ) );
  args.insert( u"attributes"_s, attributes );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QString rollbackToken = result.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !rollbackToken.isEmpty() );

  QgsFeature editedFeature;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( featureId ) ).nextFeature( editedFeature ) );
  QCOMPARE( editedFeature.attribute( u"name"_s ).toString(), u"new"_s );
  QCOMPARE( editedFeature.attribute( u"height"_s ).toDouble(), 7.25 );

  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, rollbackToken );
  const QgsAiToolResult rollback = tool.execute( rollbackArgs );
  QVERIFY2( rollback.success, qPrintable( rollback.errorMessage ) );

  QgsFeature rolledBackFeature;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( featureId ) ).nextFeature( rolledBackFeature ) );
  QCOMPARE( rolledBackFeature.attribute( u"name"_s ).toString(), u"old"_s );
  QCOMPARE( rolledBackFeature.attribute( u"height"_s ).toDouble(), 1.5 );
}

void TestQgsAiEditingTools::updateFeatureAttributesRejectsIncompatibleType()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string&field=height:double"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );

  QgsFeature feature( layer->fields() );
  feature.setGeometry( QgsGeometry::fromWkt( u"Point(1 2)"_s ) );
  feature.setAttribute( u"name"_s, u"old"_s );
  feature.setAttribute( u"height"_s, 1.5 );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << feature ) );
  project.addMapLayer( layer );

  QgsFeature storedFeature;
  QVERIFY( layer->getFeatures().nextFeature( storedFeature ) );
  const QgsFeatureId featureId = storedFeature.id();

  QgsAiUpdateFeatureAttributesTool tool( &project );
  QJsonObject attributes;
  attributes.insert( u"height"_s, u"not-a-number"_s );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"feature_id"_s, static_cast<qint64>( featureId ) );
  args.insert( u"attributes"_s, attributes );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"height"_s ) );

  QgsFeature unchangedFeature;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( featureId ) ).nextFeature( unchangedFeature ) );
  QCOMPARE( unchangedFeature.attribute( u"height"_s ).toDouble(), 1.5 );
}

void TestQgsAiEditingTools::calculateFieldCreatesFieldForFilteredFeaturesAndRollsBack()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=value:double"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );

  QgsFeature first( layer->fields() );
  first.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  first.setAttribute( u"value"_s, 2.0 );
  QgsFeature second( layer->fields() );
  second.setGeometry( QgsGeometry::fromWkt( u"Point(2 2)"_s ) );
  second.setAttribute( u"value"_s, 3.0 );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << first << second ) );
  project.addMapLayer( layer );

  QgsAiCalculateFieldTool tool( &project );
  QVERIFY( tool.requiresApproval() );
  QCOMPARE( tool.riskLevel(), QgsAiToolRiskLevel::High );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"double_value"_s );
  args.insert( u"expression"_s, u"\"value\" * 2"_s );
  args.insert( u"create_field"_s, true );
  args.insert( u"field_type"_s, u"double"_s );
  args.insert( u"filter_expression"_s, u"\"value\" > 2"_s );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( result.output.toObject().value( u"updated_feature_count"_s ).toInt(), 1 );
  const QString rollbackToken = result.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !rollbackToken.isEmpty() );

  const int calculatedIndex = layer->fields().lookupField( u"double_value"_s );
  QVERIFY( calculatedIndex >= 0 );

  QgsFeatureIterator it = layer->getFeatures();
  QgsFeature feature;
  int updatedCount = 0;
  while ( it.nextFeature( feature ) )
  {
    if ( feature.attribute( u"value"_s ).toDouble() > 2.0 )
    {
      QCOMPARE( feature.attribute( calculatedIndex ).toDouble(), 6.0 );
      updatedCount++;
    }
    else
    {
      QVERIFY( feature.attribute( calculatedIndex ).isNull() );
    }
  }
  QCOMPARE( updatedCount, 1 );

  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, rollbackToken );
  const QgsAiToolResult rollback = tool.execute( rollbackArgs );
  QVERIFY2( rollback.success, qPrintable( rollback.errorMessage ) );
  QCOMPARE( layer->fields().lookupField( u"double_value"_s ), -1 );
}

void TestQgsAiEditingTools::calculateFieldRejectsInvalidExpression()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=value:double"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );

  QgsFeature feature( layer->fields() );
  feature.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  feature.setAttribute( u"value"_s, 2.0 );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << feature ) );
  project.addMapLayer( layer );

  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"broken"_s );
  args.insert( u"expression"_s, u"\"value\" *"_s );
  args.insert( u"create_field"_s, true );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"parser error"_s ) );
  QCOMPARE( layer->fields().lookupField( u"broken"_s ), -1 );
}

static QgsVectorLayer *makeCalculatedLayer( QgsProject &project, int featureCount )
{
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=value:double"_s, u"Places"_s, u"memory"_s );
  Q_ASSERT( layer->isValid() );
  QgsFeatureList features;
  features.reserve( featureCount );
  for ( int i = 0; i < featureCount; ++i )
  {
    QgsFeature feature( layer->fields() );
    feature.setGeometry( QgsGeometry::fromWkt( u"Point(%1 %1)"_s.arg( i ) ) );
    feature.setAttribute( u"value"_s, static_cast<double>( i ) );
    features.push_back( feature );
  }
  // Not inside Q_ASSERT: that would compile the insertion out of release builds.
  const bool added = layer->dataProvider()->addFeatures( features );
  Q_ASSERT( added );
  Q_UNUSED( added )
  project.addMapLayer( layer );
  return layer;
}

void TestQgsAiEditingTools::calculateFieldKeepsInterfaceResponsive()
{
  QgsProject project;
  QgsVectorLayer *layer = makeCalculatedLayer( project, 2500 );
  QCOMPARE( layer->featureCount(), 2500LL );
  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"copy"_s );
  args.insert( u"expression"_s, u"\"value\""_s );
  args.insert( u"create_field"_s, true );

  const QgsAiTestBackgroundProbe probe;
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QVERIFY2( probe.maxLoopLevel() >= 1, "Field calculation blocked the interface thread" );
  QCOMPARE( result.output.toObject().value( u"updated_feature_count"_s ).toInt(), 2500 );
}

void TestQgsAiEditingTools::calculateFieldSeesUncommittedEdits()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=value:double"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsFeature first( layer->fields() );
  first.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  first.setAttribute( u"value"_s, 2.0 );
  QgsFeature second( layer->fields() );
  second.setGeometry( QgsGeometry::fromWkt( u"Point(2 2)"_s ) );
  second.setAttribute( u"value"_s, 3.0 );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << first << second ) );
  project.addMapLayer( layer );

  QgsFeature stored;
  QVERIFY( layer->getFeatures().nextFeature( stored ) );
  QVERIFY( layer->startEditing() );
  QVERIFY( layer->changeAttributeValue( stored.id(), layer->fields().lookupField( u"value"_s ), 10.0 ) );

  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"double_value"_s );
  args.insert( u"expression"_s, u"\"value\" * 2"_s );
  args.insert( u"create_field"_s, true );
  args.insert( u"field_type"_s, u"double"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QVERIFY( layer->isEditable() );
  QVERIFY( layer->isModified() );

  const int calculatedIndex = layer->fields().lookupField( u"double_value"_s );
  QVERIFY( calculatedIndex >= 0 );

  QgsFeature edited;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( stored.id() ) ).nextFeature( edited ) );
  QCOMPARE( edited.attribute( u"value"_s ).toDouble(), 10.0 );
  QCOMPARE( edited.attribute( calculatedIndex ).toDouble(), 20.0 );

  QgsFeature providerFeature;
  QVERIFY( layer->dataProvider()->getFeatures( QgsFeatureRequest().setFilterFid( stored.id() ) ).nextFeature( providerFeature ) );
  QCOMPARE( providerFeature.attribute( u"value"_s ).toDouble(), 2.0 );
  QCOMPARE( layer->dataProvider()->fields().lookupField( u"double_value"_s ), -1 );
}

void TestQgsAiEditingTools::calculateFieldUsesVirtualFields()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=value:double"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsFeature feature( layer->fields() );
  feature.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  feature.setAttribute( u"value"_s, 4.0 );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << feature ) );
  layer->addExpressionField( u"\"value\" * 10"_s, QgsField( u"virtual_value"_s, QMetaType::Type::Double ) );
  project.addMapLayer( layer );

  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"copied"_s );
  args.insert( u"expression"_s, u"\"virtual_value\""_s );
  args.insert( u"create_field"_s, true );
  args.insert( u"field_type"_s, u"double"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );

  const int copiedIndex = layer->fields().lookupField( u"copied"_s );
  QVERIFY( copiedIndex >= 0 );
  QgsFeature stored;
  QVERIFY( layer->getFeatures().nextFeature( stored ) );
  QCOMPARE( stored.attribute( copiedIndex ).toDouble(), 40.0 );
}

void TestQgsAiEditingTools::calculateFieldAbortsWhenLayerChangesDuringRun()
{
  QgsProject project;
  QgsVectorLayer *layer = makeCalculatedLayer( project, 2500 );
  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"copy"_s );
  args.insert( u"expression"_s, u"\"value\""_s );
  args.insert( u"create_field"_s, true );

  const QgsAiTestBackgroundProbe probe( [layer]() { layer->startEditing(); } );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( result.errorMessage.contains( u"Layer changed while computing"_s ) );
  QCOMPARE( layer->fields().lookupField( u"copy"_s ), -1 );
}

void TestQgsAiEditingTools::calculateFieldCancelLeavesLayerUnchanged()
{
  QgsProject project;
  QgsVectorLayer *layer = makeCalculatedLayer( project, 2500 );
  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"copy"_s );
  args.insert( u"expression"_s, u"\"value\""_s );
  args.insert( u"create_field"_s, true );

  const QgsAiTestBackgroundProbe probe( []() { qgsAiCancelActiveBackgroundTool(); } );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( result.canceled );
  QVERIFY( !result.success );
  QCOMPARE( layer->fields().lookupField( u"copy"_s ), -1 );
  QVERIFY( !layer->isEditable() );
}

void TestQgsAiEditingTools::calculateFieldIncludesAddedFeatures()
{
  QgsProject project;
  QgsVectorLayer *layer = makeCalculatedLayer( project, 3 );
  QVERIFY( layer->startEditing() );
  QgsFeature added( layer->fields() );
  added.setGeometry( QgsGeometry::fromWkt( u"Point(9 9)"_s ) );
  added.setAttribute( u"value"_s, 21.0 );
  QVERIFY( layer->addFeature( added ) );
  QVERIFY( added.id() < 0 );

  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"value"_s );
  args.insert( u"expression"_s, u"\"value\" * 2"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( result.output.toObject().value( u"updated_feature_count"_s ).toInt(), 4 );

  // The uncommitted feature, with its temporary negative id, was calculated too.
  QgsFeature calculated;
  QVERIFY( layer->getFeatures( QgsFeatureRequest().setFilterFid( added.id() ) ).nextFeature( calculated ) );
  QCOMPARE( calculated.attribute( u"value"_s ).toDouble(), 42.0 );
  QVERIFY( layer->isEditable() );
}

void TestQgsAiEditingTools::calculateFieldUsesJoinedFields()
{
  QgsProject project;
  QgsVectorLayer *target = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=code:integer"_s, u"Target"_s, u"memory"_s );
  QVERIFY( target->isValid() );
  QgsFeature targetFeature( target->fields() );
  targetFeature.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  targetFeature.setAttribute( u"code"_s, 7 );
  QVERIFY( target->dataProvider()->addFeatures( QgsFeatureList() << targetFeature ) );
  project.addMapLayer( target );

  QgsVectorLayer *lookup = new QgsVectorLayer( u"None?field=code:integer&field=rate:double"_s, u"Lookup"_s, u"memory"_s );
  QVERIFY( lookup->isValid() );
  QgsFeature lookupFeature( lookup->fields() );
  lookupFeature.setAttribute( u"code"_s, 7 );
  lookupFeature.setAttribute( u"rate"_s, 1.5 );
  QVERIFY( lookup->dataProvider()->addFeatures( QgsFeatureList() << lookupFeature ) );
  project.addMapLayer( lookup );

  QgsVectorLayerJoinInfo join;
  join.setJoinLayer( lookup );
  join.setJoinFieldName( u"code"_s );
  join.setTargetFieldName( u"code"_s );
  join.setPrefix( u"lookup_"_s );
  join.setUsingMemoryCache( true );
  QVERIFY( target->addJoin( join ) );
  QVERIFY( target->fields().lookupField( u"lookup_rate"_s ) >= 0 );

  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, target->id() );
  args.insert( u"field_name"_s, u"scaled"_s );
  args.insert( u"expression"_s, u"\"lookup_rate\" * 10"_s );
  args.insert( u"create_field"_s, true );
  args.insert( u"field_type"_s, u"double"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );

  QgsFeature stored;
  QVERIFY( target->getFeatures().nextFeature( stored ) );
  QCOMPARE( stored.attribute( u"scaled"_s ).toDouble(), 15.0 );
}

void TestQgsAiEditingTools::calculateFieldReportsRemovedLayer()
{
  QgsProject project;
  QgsVectorLayer *layer = makeCalculatedLayer( project, 2500 );
  const QString layerId = layer->id();
  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layerId );
  args.insert( u"field_name"_s, u"copy"_s );
  args.insert( u"expression"_s, u"\"value\""_s );
  args.insert( u"create_field"_s, true );

  // The user removes the layer while the scan runs: an error the model can react to, no crash.
  const QgsAiTestBackgroundProbe probe( [&project, layerId]() { project.removeMapLayer( layerId ); } );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY( !result.success );
  QVERIFY( !result.canceled );
  QVERIFY2( result.errorMessage.contains( u"removed"_s ), qPrintable( result.errorMessage ) );
  QVERIFY( !project.mapLayer( layerId ) );
}

void TestQgsAiEditingTools::calculateFieldEvaluatesAggregatesOnInterfaceThread()
{
  QgsProject project;
  QgsVectorLayer *layer = makeCalculatedLayer( project, 4 );
  QgsAiCalculateFieldTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"field_name"_s, u"share"_s );
  args.insert( u"expression"_s, u"\"value\" / sum(\"value\")"_s );
  args.insert( u"create_field"_s, true );
  args.insert( u"field_type"_s, u"double"_s );

  // sum() reads the live layer, so the scan runs on the GUI thread: its progress arrives outside
  // any nested event loop.
  const QgsAiTestBackgroundProbe probe;
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( probe.maxLoopLevel(), 0 );

  const int shareIndex = layer->fields().lookupField( u"share"_s );
  QVERIFY( shareIndex >= 0 );
  QgsFeature feature;
  QgsFeatureIterator it = layer->getFeatures();
  while ( it.nextFeature( feature ) )
    QGSCOMPARENEAR( feature.attribute( shareIndex ).toDouble(), feature.attribute( u"value"_s ).toDouble() / 6.0, 1e-9 );
}

QGSTEST_MAIN( TestQgsAiEditingTools )
#include "testqgsaieditingtools.moc"
