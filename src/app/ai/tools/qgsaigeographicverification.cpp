// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaigeographicverification.h"
#include "qgsrasterlayer.h"
#include "qgscoordinatereferencesystem.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <gdal.h>
#include <cmath>
#include <algorithm>
using namespace Qt::StringLiterals;

QJsonObject qgsAiVerifyRasterSource( QgsMapLayer *layer )
{
  const auto *raster = qobject_cast<QgsRasterLayer *>( layer );
  if ( !raster || raster->providerType() != "gdal"_L1 || !QFileInfo::exists( raster->source() ) )
    return {};
  QJsonObject checks;
  GDALDatasetH dataset = GDALOpen( raster->source().toUtf8().constData(), GA_ReadOnly );
  checks.insert( u"raster_reopened"_s, dataset != nullptr );
  if ( !dataset )
    return checks;
  const auto crs = QgsCoordinateReferenceSystem::fromWkt( QString::fromUtf8( GDALGetProjectionRef( dataset ) ) );
  checks.insert( u"raster_source_crs_valid"_s, crs.isValid() );
  checks.insert( u"raster_source_matches_layer_crs"_s, crs.isValid() && crs == layer->crs() );
  const int width = GDALGetRasterXSize( dataset ), height = GDALGetRasterYSize( dataset );
  checks.insert( u"raster_dimensions_match"_s, width == raster->width() && height == raster->height() );
  double gt[6] = {};
  const bool valid = GDALGetGeoTransform( dataset, gt ) == CE_None
                     && width > 0
                     && height > 0
                     && std::all_of( std::begin( gt ), std::end( gt ), []( double value ) { return std::isfinite( value ); } )
                     && std::abs( gt[1] * gt[5] - gt[2] * gt[4] ) > 0;
  checks.insert( u"raster_transform_valid"_s, valid );
  if ( const char *metadata = GDALGetMetadataItem( dataset, "STRATA_EXPORT_EXTENT", nullptr ) )
  {
    const auto expected = QJsonDocument::fromJson( QByteArray( metadata ) ).array();
    const double tolerance = std::max( std::abs( gt[1] ), std::abs( gt[5] ) ) * 0.5;
    const bool matches = valid
                         && expected.size() == 4
                         && gt[2] == 0
                         && gt[4] == 0
                         && std::abs( gt[0] - expected[0].toDouble() ) <= tolerance
                         && std::abs( gt[3] + height * gt[5] - expected[1].toDouble() ) <= tolerance
                         && std::abs( gt[0] + width * gt[1] - expected[2].toDouble() ) <= tolerance
                         && std::abs( gt[3] - expected[3].toDouble() ) <= tolerance;
    checks.insert( u"snapshot_extent_matches"_s, matches );
  }
  GDALClose( dataset );
  return checks;
}
