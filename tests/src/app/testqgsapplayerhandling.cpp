/***************************************************************************
  testqgsapplayerhandling.cpp
  --------------------------------
  begin                : August 2026
***************************************************************************/

#include "layers/qgsapplayerhandling.h"
#include "qgsfileutils.h"
#include "qgsprovidersublayerdetails.h"
#include "qgssettings.h"
#include "qgssettingsentryimpl.h"
#include "qgstest.h"

#include <QFile>
#include <QString>
#include <QTemporaryDir>

using namespace Qt::StringLiterals;

class TestQgsAppLayerHandling : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void guardrailForcesAskUserAboveThreshold();
    void guardrailDisabledWithZeroThreshold();
    void guardrailAppliesToAskExcludingRasterBands();
    void otherPromptModesUnaffected();
    void pathIsSidecarFileDetectsShapefileParts();
    void openLayerSkipsShapefileSidecars();

  private:
    static QList<QgsProviderSublayerDetails> makeDetails( int count, Qgis::LayerType type = Qgis::LayerType::Vector );
};

void TestQgsAppLayerHandling::initTestCase()
{
  QgsApplication::initQgis();
}

void TestQgsAppLayerHandling::cleanupTestCase()
{
  QgsApplication::exitQgis();
}

void TestQgsAppLayerHandling::cleanup()
{
  QgsSettings().remove( u"qgis/promptForSublayers"_s );
  QgsAppLayerHandling::settingsSublayerPromptThreshold->setValue( QgsAppLayerHandling::settingsSublayerPromptThreshold->defaultValue() );
}

QList<QgsProviderSublayerDetails> TestQgsAppLayerHandling::makeDetails( int count, Qgis::LayerType type )
{
  QList<QgsProviderSublayerDetails> details;
  details.reserve( count );
  for ( int i = 0; i < count; i++ )
  {
    QgsProviderSublayerDetails d;
    d.setType( type );
    d.setProviderKey( type == Qgis::LayerType::Raster ? u"gdal"_s : u"ogr"_s );
    d.setUri( u"dummy_uri_%1"_s.arg( i ) );
    d.setName( u"layer_%1"_s.arg( i ) );
    details.append( d );
  }
  return details;
}

void TestQgsAppLayerHandling::guardrailForcesAskUserAboveThreshold()
{
  QgsSettings().setEnumValue( u"qgis/promptForSublayers"_s, Qgis::SublayerPromptMode::NeverAskLoadAll );

  // default threshold is 20: 21 layers must ask, 20 may load all
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 21 ) ), QgsAppLayerHandling::SublayerHandling::AskUser );
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 20 ) ), QgsAppLayerHandling::SublayerHandling::LoadAll );

  QgsAppLayerHandling::settingsSublayerPromptThreshold->setValue( 5 );
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 6 ) ), QgsAppLayerHandling::SublayerHandling::AskUser );
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 5 ) ), QgsAppLayerHandling::SublayerHandling::LoadAll );
}

void TestQgsAppLayerHandling::guardrailDisabledWithZeroThreshold()
{
  QgsSettings().setEnumValue( u"qgis/promptForSublayers"_s, Qgis::SublayerPromptMode::NeverAskLoadAll );
  QgsAppLayerHandling::settingsSublayerPromptThreshold->setValue( 0 );

  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 500 ) ), QgsAppLayerHandling::SublayerHandling::LoadAll );
}

void TestQgsAppLayerHandling::guardrailAppliesToAskExcludingRasterBands()
{
  QgsSettings().setEnumValue( u"qgis/promptForSublayers"_s, Qgis::SublayerPromptMode::AskExcludingRasterBands );

  // all-raster sources normally load all: the guardrail still applies above the threshold
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 21, Qgis::LayerType::Raster ) ), QgsAppLayerHandling::SublayerHandling::AskUser );
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 5, Qgis::LayerType::Raster ) ), QgsAppLayerHandling::SublayerHandling::LoadAll );

  // any non-raster layer asks regardless of counts
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 2 ) ), QgsAppLayerHandling::SublayerHandling::AskUser );
}

void TestQgsAppLayerHandling::otherPromptModesUnaffected()
{
  QgsSettings().setEnumValue( u"qgis/promptForSublayers"_s, Qgis::SublayerPromptMode::AlwaysAsk );
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 2 ) ), QgsAppLayerHandling::SublayerHandling::AskUser );

  QgsSettings().setEnumValue( u"qgis/promptForSublayers"_s, Qgis::SublayerPromptMode::NeverAskSkip );
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 500 ) ), QgsAppLayerHandling::SublayerHandling::AbortLoading );

  // non-layer items always ask, independent of the guardrail
  QgsSettings().setEnumValue( u"qgis/promptForSublayers"_s, Qgis::SublayerPromptMode::NeverAskLoadAll );
  QCOMPARE( QgsAppLayerHandling::shouldAskUserForSublayers( makeDetails( 1 ), true ), QgsAppLayerHandling::SublayerHandling::AskUser );
}

void TestQgsAppLayerHandling::pathIsSidecarFileDetectsShapefileParts()
{
  const QString dataDir = QStringLiteral( TEST_DATA_DIR ) + '/';
  QVERIFY( QgsFileUtils::pathIsSidecarFile( dataDir + u"points.prj"_s ) );
  QVERIFY( QgsFileUtils::pathIsSidecarFile( dataDir + u"points.shx"_s ) );
  QVERIFY( QgsFileUtils::pathIsSidecarFile( dataDir + u"points.dbf"_s ) );
  QVERIFY( !QgsFileUtils::pathIsSidecarFile( dataDir + u"points.shp"_s ) );

  QTemporaryDir tmp;
  QVERIFY( tmp.isValid() );
  const QString loneDbf = tmp.filePath( u"table.dbf"_s );
  QVERIFY( QFile::copy( dataDir + u"points.dbf"_s, loneDbf ) );
  QVERIFY( !QgsFileUtils::pathIsSidecarFile( loneDbf ) );

  const QString lonePrj = tmp.filePath( u"only.prj"_s );
  QVERIFY( QFile::copy( dataDir + u"points.prj"_s, lonePrj ) );
  QVERIFY( QgsFileUtils::pathIsSidecarFile( lonePrj ) );

  QVERIFY( QFile::copy( dataDir + u"points.shp"_s, tmp.filePath( u"layer.shp"_s ) ) );
  QVERIFY( QFile::copy( dataDir + u"points.shx"_s, tmp.filePath( u"layer.shx"_s ) ) );
  QVERIFY( QFile::copy( dataDir + u"points.dbf"_s, tmp.filePath( u"layer.dbf"_s ) ) );
  QVERIFY( QFile::copy( dataDir + u"points.prj"_s, tmp.filePath( u"layer.prj"_s ) ) );
  QFile cpg( tmp.filePath( u"layer.cpg"_s ) );
  QVERIFY( cpg.open( QIODevice::WriteOnly ) );
  QCOMPARE( cpg.write( "UTF-8" ), 5 );
  cpg.close();
  QVERIFY( QgsFileUtils::pathIsSidecarFile( tmp.filePath( u"layer.cpg"_s ) ) );
  QVERIFY( QgsFileUtils::pathIsSidecarFile( tmp.filePath( u"layer.prj"_s ) ) );
  QVERIFY( !QgsFileUtils::pathIsSidecarFile( tmp.filePath( u"layer.shp"_s ) ) );
}

void TestQgsAppLayerHandling::openLayerSkipsShapefileSidecars()
{
  const QString dataDir = QStringLiteral( TEST_DATA_DIR ) + '/';
  bool ok = false;
  QCOMPARE( QgsAppLayerHandling::openLayer( dataDir + u"points.prj"_s, ok, false, true, false ).size(), 0 );
  QVERIFY( ok );

  ok = false;
  QCOMPARE( QgsAppLayerHandling::openLayer( dataDir + u"points.shx"_s, ok, false, true, false ).size(), 0 );
  QVERIFY( ok );

  ok = false;
  QCOMPARE( QgsAppLayerHandling::openLayer( dataDir + u"points.dbf"_s, ok, false, true, false ).size(), 0 );
  QVERIFY( ok );
}

QGSTEST_MAIN( TestQgsAppLayerHandling )
#include "testqgsapplayerhandling.moc"
