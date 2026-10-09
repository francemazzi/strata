/***************************************************************************
     testqgspostgrescrsdefinition.cpp
     --------------------------------
    Copyright (C) 2026 Francesco Mazzi
    SPDX-License-Identifier: GPL-2.0-or-later
 ***************************************************************************/
#include "qgstest.h"

#include "qgsapplication.h"
#include "qgscoordinatereferencesystem.h"
#include "qgspostgresconn.h"

#include <QObject>
#include <QVariantMap>

using namespace Qt::StringLiterals;

// spatial_ref_sys row for SRID 3003 as shipped by PostGIS (3.4 included): WKT1 with an embedded
// TOWGS84 datum shift and AUTHORITY tags.
static const QString sLegacyWkt
  = uR"wkt(PROJCS["Monte Mario / Italy zone 1",GEOGCS["Monte Mario",DATUM["Monte_Mario",SPHEROID["International 1924",6378388,297,AUTHORITY["EPSG","7022"]],TOWGS84[-104.1,-49.1,-9.9,0.971,-2.917,0.714,-11.68],AUTHORITY["EPSG","6265"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4265"]],PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",9],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",1500000],PARAMETER["false_northing",0],UNIT["metre",1,AUTHORITY["EPSG","9001"]],AXIS["X",EAST],AXIS["Y",NORTH],AUTHORITY["EPSG","3003"]])wkt"_s;
static const QString sLegacyProj = u"+proj=tmerc +lat_0=0 +lon_0=9 +k=0.9996 +x_0=1500000 +y_0=0 +ellps=intl +towgs84=-104.1,-49.1,-9.9,0.971,-2.917,0.714,-11.68 +units=m +no_defs"_s;

class TestQgsPostgresCrsDefinition : public QgsTest
{
    Q_OBJECT

  public:
    TestQgsPostgresCrsDefinition()
      : QgsTest( u"PostGIS catalog CRS definitions"_s )
    {}

  private slots:
    void initTestCase() { QgsApplication::init(); }

    void declaredAuthorityWins()
    {
      for ( const QString &authName : { u"EPSG"_s, u"epsg"_s } )
      {
        QVariantMap details;
        const QgsCoordinateReferenceSystem crs = QgsPostgresConn::crsFromCatalogDefinition( authName, u"3003"_s, sLegacyWkt, sLegacyProj, details );
        QVERIFY( crs.isValid() );
        QCOMPARE( crs.authid(), u"EPSG:3003"_s );
        QCOMPARE( details.value( u"definition"_s ).toString(), u"authority"_s );
        QVERIFY( !details.contains( u"definition_authid"_s ) );
      }
    }

    void legacyWktWithoutAuthority()
    {
      // No usable authority in the row (NULL/empty or a local name): the authority the WKT itself
      // declares is used instead of an unidentified BoundCRS.
      for ( const QString &authName : { QString(), u"custom"_s } )
      {
        QVariantMap details;
        const QgsCoordinateReferenceSystem crs = QgsPostgresConn::crsFromCatalogDefinition( authName, QString(), sLegacyWkt, sLegacyProj, details );
        QVERIFY( crs.isValid() );
        QCOMPARE( crs.authid(), u"EPSG:3003"_s );
        QCOMPARE( crs, QgsCoordinateReferenceSystem( u"EPSG:3003"_s ) );
        QCOMPARE( details.value( u"definition"_s ).toString(), u"legacy_bound"_s );
        QCOMPARE( details.value( u"definition_authid"_s ).toString(), u"EPSG:3003"_s );
        QVERIFY( !details.contains( u"identified_authid"_s ) );
      }
    }

    void legacyWktStaysBoundWithoutIdentification()
    {
      // Behaviour of the core WKT path, documented here so a change upstream is noticed.
      const QgsCoordinateReferenceSystem bound = QgsCoordinateReferenceSystem::fromWkt( sLegacyWkt );
      QVERIFY( bound.isValid() );
      QVERIFY( bound.authid().isEmpty() );
      QCOMPARE( bound.description(), u"Monte Mario / Italy zone 1"_s );
    }

    void legacyProjWithoutWkt()
    {
      QVariantMap details;
      const QgsCoordinateReferenceSystem crs = QgsPostgresConn::crsFromCatalogDefinition( QString(), QString(), QString(), sLegacyProj, details );
      QVERIFY( crs.isValid() );
      QCOMPARE( crs.authid(), u"EPSG:3003"_s );
      QCOMPARE( details.value( u"definition"_s ).toString(), u"proj"_s );
    }

    void modernWktWithoutAuthority()
    {
      const QString wkt = QgsCoordinateReferenceSystem( u"EPSG:3003"_s ).toWkt( Qgis::CrsWktVariant::Wkt1Gdal );
      QVERIFY( !wkt.contains( "TOWGS84"_L1 ) );
      QVariantMap details;
      const QgsCoordinateReferenceSystem crs = QgsPostgresConn::crsFromCatalogDefinition( QString(), QString(), wkt, QString(), details );
      QCOMPARE( crs.authid(), u"EPSG:3003"_s );
      QCOMPARE( details.value( u"definition"_s ).toString(), u"wkt"_s );
    }

    void customDefinitionStaysCustom()
    {
      const QgsCoordinateReferenceSystem custom = QgsCoordinateReferenceSystem::fromProj( u"+proj=tmerc +lat_0=0 +lon_0=9.123456 +k=0.9996 +x_0=500000 +y_0=0 +ellps=GRS80 +units=m"_s );
      QVariantMap details;
      const QgsCoordinateReferenceSystem crs = QgsPostgresConn::crsFromCatalogDefinition( u"LOCAL"_s, u"990051"_s, custom.toWkt( Qgis::CrsWktVariant::Preferred ), custom.toProj(), details );
      QVERIFY( crs.isValid() );
      QVERIFY( crs.authid().isEmpty() );
      QCOMPARE( crs, custom );
      QCOMPARE( details.value( u"definition"_s ).toString(), u"wkt"_s );
      QVERIFY( !details.contains( u"definition_authid"_s ) );
      QVERIFY( !details.contains( u"identified_authid"_s ) );
    }

    void invalidDefinition()
    {
      QVariantMap details;
      const QgsCoordinateReferenceSystem crs = QgsPostgresConn::crsFromCatalogDefinition( u"LOCAL"_s, u"990054"_s, u"invalid"_s, u"invalid"_s, details );
      QVERIFY( !crs.isValid() );
      QVERIFY( !details.contains( u"definition"_s ) );
    }

    void projDatabaseStatus()
    {
      const QVariantMap status = QgsPostgresConn::projDatabaseStatus();
      QVERIFY( status.value( u"available"_s ).toBool() );
      QVERIFY( !status.value( u"path"_s ).toString().isEmpty() );
      QVERIFY( !status.contains( u"error"_s ) );
    }
};

QGSTEST_MAIN( TestQgsPostgresCrsDefinition )
#include "testqgspostgrescrsdefinition.moc"
