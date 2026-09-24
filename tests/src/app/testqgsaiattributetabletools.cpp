/***************************************************************************
  testqgsaiattributetabletools.cpp
  --------------------------
  begin                : July 2026
  copyright            : (C) 2026
***************************************************************************/

#include "ai/tools/qgsaiattributetabletools.h"
#include "ai/tools/qgsailayertools.h"
#include "qgsapplication.h"
#include "qgsfeature.h"
#include "qgsgeometry.h"
#include "qgslayertree.h"
#include "qgslayertreelayer.h"
#include "qgsproject.h"
#include "qgstest.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTimer>

using namespace Qt::StringLiterals;

class TestQgsAiAttributeTableTools : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void queryFeaturesFiltersAndPaginates();
    void queryFeaturesCapsLargeResult();
    void reorderLayersUsesNativeTreeOrder();
    void batchUpdateAttributesUpdatesAndRollsBack();
    void batchUpdateAttributesKeepsInterfaceResponsive();
    void selectFeaturesUpdatesLayerSelection();
    void selectFeaturesSupportsModeAndBbox();
    void selectFeaturesKeepsInterfaceResponsive();
    void identifyFeaturesAtReturnsMatchingFeature();
};

void TestQgsAiAttributeTableTools::initTestCase()
{
  QgsApplication::initQgis();
}

void TestQgsAiAttributeTableTools::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

static QgsVectorLayer *makePlacesLayer( QgsProject &project )
{
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string&field=value:integer"_s, u"Places"_s, u"memory"_s );
  Q_ASSERT( layer->isValid() );

  QgsFeature first( layer->fields() );
  first.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  first.setAttribute( u"name"_s, u"one"_s );
  first.setAttribute( u"value"_s, 1 );
  QgsFeature second( layer->fields() );
  second.setGeometry( QgsGeometry::fromWkt( u"Point(2 2)"_s ) );
  second.setAttribute( u"name"_s, u"two"_s );
  second.setAttribute( u"value"_s, 2 );
  QgsFeature third( layer->fields() );
  third.setGeometry( QgsGeometry::fromWkt( u"Point(3 3)"_s ) );
  third.setAttribute( u"name"_s, u"three"_s );
  third.setAttribute( u"value"_s, 3 );
  layer->dataProvider()->addFeatures( QgsFeatureList() << first << second << third );
  project.addMapLayer( layer );
  return layer;
}

void TestQgsAiAttributeTableTools::queryFeaturesFiltersAndPaginates()
{
  QgsProject project;
  QgsVectorLayer *layer = makePlacesLayer( project );
  QVERIFY( layer );

  QgsAiQueryFeaturesTool tool( &project );
  QVERIFY( !tool.requiresApproval() );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"filter_expression"_s, u"\"value\" >= 2"_s );
  args.insert( u"offset"_s, 1 );
  args.insert( u"limit"_s, 1 );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonObject output = result.output.toObject();
  QCOMPARE( output.value( u"feature_count"_s ).toInt(), 2 );
  const QJsonArray features = output.value( u"features"_s ).toArray();
  QCOMPARE( features.size(), 1 );
  QCOMPARE( features.at( 0 ).toObject().value( u"attributes"_s ).toObject().value( u"name"_s ).toString(), u"three"_s );
}

void TestQgsAiAttributeTableTools::queryFeaturesCapsLargeResult()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=payload:string(60000)"_s, u"Large values"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsFeature feature( layer->fields() );
  feature.setGeometry( QgsGeometry::fromWkt( u"Point(1 1)"_s ) );
  feature.setAttribute( u"payload"_s, QString( 60000, u'x' ) );
  QVERIFY( layer->dataProvider()->addFeatures( QgsFeatureList() << feature ) );
  project.addMapLayer( layer );

  QgsAiQueryFeaturesTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"limit"_s, 10 );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonObject output = result.output.toObject();
  QVERIFY( output.value( u"truncated"_s ).toBool() );
  QCOMPARE( output.value( u"features"_s ).toArray().size(), 0 );
  QCOMPARE( output.value( u"next_offset"_s ).toInt(), 0 );
}

void TestQgsAiAttributeTableTools::reorderLayersUsesNativeTreeOrder()
{
  QgsProject project;
  QgsVectorLayer *first = makePlacesLayer( project );
  QgsVectorLayer *second = new QgsVectorLayer( u"Point?crs=EPSG:4326"_s, u"Second"_s, u"memory"_s );
  QgsVectorLayer *third = new QgsVectorLayer( u"Point?crs=EPSG:4326"_s, u"Third"_s, u"memory"_s );
  QVERIFY( second->isValid() && third->isValid() );
  project.addMapLayer( second );
  project.addMapLayer( third );

  QgsAiReorderLayersTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_ids"_s, QJsonArray { second->id(), first->id(), third->id() } );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonArray order = result.output.toObject().value( u"layer_order"_s ).toArray();
  QCOMPARE( order.at( 0 ).toString(), second->id() );
  QCOMPARE( order.at( 1 ).toString(), first->id() );
  QCOMPARE( order.at( 2 ).toString(), third->id() );
  QVERIFY( project.mapLayer( first->id() ) == first );
  QVERIFY( project.mapLayer( second->id() ) == second );
  QVERIFY( project.mapLayer( third->id() ) == third );
}

void TestQgsAiAttributeTableTools::batchUpdateAttributesUpdatesAndRollsBack()
{
  QgsProject project;
  QgsVectorLayer *layer = makePlacesLayer( project );
  QVERIFY( layer );

  QgsAiBatchUpdateAttributesTool tool( &project );
  QVERIFY( tool.requiresApproval() );
  QCOMPARE( tool.riskLevel(), QgsAiToolRiskLevel::High );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"filter_expression"_s, u"\"value\" >= 2"_s );
  args.insert( u"field_name"_s, u"name"_s );
  args.insert( u"value"_s, u"updated"_s );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( result.output.toObject().value( u"updated_feature_count"_s ).toInt(), 2 );
  const QString rollbackToken = result.output.toObject().value( u"rollback_token"_s ).toString();
  QVERIFY( !rollbackToken.isEmpty() );

  QgsFeatureIterator it = layer->getFeatures();
  QgsFeature feature;
  int updatedCount = 0;
  while ( it.nextFeature( feature ) )
  {
    if ( feature.attribute( u"value"_s ).toInt() >= 2 )
    {
      QCOMPARE( feature.attribute( u"name"_s ).toString(), u"updated"_s );
      updatedCount++;
    }
  }
  QCOMPARE( updatedCount, 2 );

  QJsonObject rollbackArgs;
  rollbackArgs.insert( u"rollback_token"_s, rollbackToken );
  const QgsAiToolResult rollback = tool.execute( rollbackArgs );
  QVERIFY2( rollback.success, qPrintable( rollback.errorMessage ) );

  QJsonObject queryArgs;
  queryArgs.insert( u"layer_id"_s, layer->id() );
  queryArgs.insert( u"filter_expression"_s, u"\"value\" >= 2"_s );
  QgsAiQueryFeaturesTool queryTool( &project );
  const QgsAiToolResult query = queryTool.execute( queryArgs );
  QVERIFY2( query.success, qPrintable( query.errorMessage ) );
  const QJsonArray features = query.output.toObject().value( u"features"_s ).toArray();
  QCOMPARE( features.at( 0 ).toObject().value( u"attributes"_s ).toObject().value( u"name"_s ).toString(), u"two"_s );
  QCOMPARE( features.at( 1 ).toObject().value( u"attributes"_s ).toObject().value( u"name"_s ).toString(), u"three"_s );
}

void TestQgsAiAttributeTableTools::selectFeaturesUpdatesLayerSelection()
{
  QgsProject project;
  QgsVectorLayer *layer = makePlacesLayer( project );
  QVERIFY( layer );

  QgsAiSelectFeaturesTool tool( &project );
  QVERIFY( tool.requiresApproval() );
  QCOMPARE( tool.riskLevel(), QgsAiToolRiskLevel::Low );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"mode"_s, u"replace"_s );
  args.insert( u"filter_expression"_s, u"\"value\" >= 2"_s );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QCOMPARE( result.output.toObject().value( u"selected_count"_s ).toInt(), 2 );
  QCOMPARE( layer->selectedFeatureIds().size(), 2 );

  QJsonObject emptyArgs;
  emptyArgs.insert( u"layer_id"_s, layer->id() );
  emptyArgs.insert( u"mode"_s, u"replace"_s );
  emptyArgs.insert( u"filter_expression"_s, u"\"value\" > 100"_s );
  const QgsAiToolResult empty = tool.execute( emptyArgs );
  QVERIFY2( empty.success, qPrintable( empty.errorMessage ) );
  QCOMPARE( empty.output.toObject().value( u"selected_count"_s ).toInt(), 0 );
  QCOMPARE( layer->selectedFeatureIds().size(), 0 );
}

void TestQgsAiAttributeTableTools::selectFeaturesSupportsModeAndBbox()
{
  QgsProject project;
  QgsVectorLayer *layer = makePlacesLayer( project );
  QVERIFY( layer );

  QgsAiSelectFeaturesTool tool( &project );
  QJsonObject bboxArgs;
  bboxArgs.insert( u"layer_id"_s, layer->id() );
  bboxArgs.insert( u"mode"_s, u"replace"_s );
  QJsonObject bbox;
  bbox.insert( u"xmin"_s, 0.5 );
  bbox.insert( u"ymin"_s, 0.5 );
  bbox.insert( u"xmax"_s, 1.5 );
  bbox.insert( u"ymax"_s, 1.5 );
  bboxArgs.insert( u"bbox"_s, bbox );
  const QgsAiToolResult bboxResult = tool.execute( bboxArgs );
  QVERIFY2( bboxResult.success, qPrintable( bboxResult.errorMessage ) );
  QCOMPARE( layer->selectedFeatureIds().size(), 1 );
  QCOMPARE( bboxResult.output.toObject().value( u"matched_feature_count"_s ).toInt(), 1 );

  QJsonObject addArgs;
  addArgs.insert( u"layer_id"_s, layer->id() );
  addArgs.insert( u"mode"_s, u"add"_s );
  addArgs.insert( u"filter_expression"_s, u"\"value\" = 3"_s );
  const QgsAiToolResult addResult = tool.execute( addArgs );
  QVERIFY2( addResult.success, qPrintable( addResult.errorMessage ) );
  QCOMPARE( layer->selectedFeatureIds().size(), 2 );
}

void TestQgsAiAttributeTableTools::batchUpdateAttributesKeepsInterfaceResponsive()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=name:string&field=value:integer"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsFeatureList features;
  for ( int i = 0; i < 2500; ++i )
  {
    QgsFeature feature( layer->fields() );
    feature.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "Point(%1 %1)" ).arg( i ) ) );
    feature.setAttribute( u"name"_s, u"row"_s );
    feature.setAttribute( u"value"_s, i );
    features.push_back( feature );
  }
  QVERIFY( layer->dataProvider()->addFeatures( features ) );
  project.addMapLayer( layer );

  QgsAiBatchUpdateAttributesTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"filter_expression"_s, u"\"value\" >= 0"_s );
  args.insert( u"field_name"_s, u"name"_s );
  args.insert( u"value"_s, u"updated"_s );

  bool interfaceEventsRan = false;
  QTimer interfaceTimer;
  interfaceTimer.setSingleShot( true );
  QObject::connect( &interfaceTimer, &QTimer::timeout, &interfaceTimer, [&interfaceEventsRan]() { interfaceEventsRan = true; } );
  interfaceTimer.start( 0 );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QVERIFY2( interfaceEventsRan, "Batch update blocked the interface thread" );
}

void TestQgsAiAttributeTableTools::selectFeaturesKeepsInterfaceResponsive()
{
  QgsProject project;
  QgsVectorLayer *layer = new QgsVectorLayer( u"Point?crs=EPSG:4326&field=value:integer"_s, u"Places"_s, u"memory"_s );
  QVERIFY( layer->isValid() );
  QgsFeatureList features;
  for ( int i = 0; i < 2500; ++i )
  {
    QgsFeature feature( layer->fields() );
    feature.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "Point(%1 %1)" ).arg( i ) ) );
    feature.setAttribute( u"value"_s, i );
    features.push_back( feature );
  }
  QVERIFY( layer->dataProvider()->addFeatures( features ) );
  project.addMapLayer( layer );

  QgsAiSelectFeaturesTool tool( &project );
  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"filter_expression"_s, u"\"value\" >= 0"_s );

  bool interfaceEventsRan = false;
  QTimer interfaceTimer;
  interfaceTimer.setSingleShot( true );
  QObject::connect( &interfaceTimer, &QTimer::timeout, &interfaceTimer, [&interfaceEventsRan]() { interfaceEventsRan = true; } );
  interfaceTimer.start( 0 );

  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QVERIFY2( interfaceEventsRan, "Feature selection blocked the interface thread" );
}

void TestQgsAiAttributeTableTools::identifyFeaturesAtReturnsMatchingFeature()
{
  QgsProject project;
  QgsVectorLayer *layer = makePlacesLayer( project );
  QVERIFY( layer );

  QgsAiIdentifyFeaturesAtTool tool( &project );
  QVERIFY( !tool.requiresApproval() );

  QJsonObject args;
  args.insert( u"layer_id"_s, layer->id() );
  args.insert( u"x"_s, 2.0 );
  args.insert( u"y"_s, 2.0 );
  args.insert( u"crs"_s, u"EPSG:4326"_s );
  args.insert( u"tolerance"_s, 0.05 );
  const QgsAiToolResult result = tool.execute( args );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  const QJsonArray features = result.output.toObject().value( u"features"_s ).toArray();
  QCOMPARE( features.size(), 1 );
  QCOMPARE( features.at( 0 ).toObject().value( u"attributes"_s ).toObject().value( u"name"_s ).toString(), u"two"_s );
  QCOMPARE( features.at( 0 ).toObject().value( u"attributes"_s ).toObject().value( u"value"_s ).toInt(), 2 );
}

QGSTEST_MAIN( TestQgsAiAttributeTableTools )
#include "testqgsaiattributetabletools.moc"
