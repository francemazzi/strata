// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaiarcgissnapshot.h"
#include "qgsaigeographicverification.h"
#include "qgsblockingnetworkrequest.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsfeedback.h"
#include "qgsrasterlayer.h"
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QTemporaryFile>
#include <QUrlQuery>
#include <gdal.h>
#include <cmath>
using namespace Qt::StringLiterals;

bool qgsAiWriteArcGisGeoTiff( const QByteArray &pixels, const QJsonObject &response, const QString &destination, QString &error )
{
  const auto fail = [&error]( const QString &text ) {
    error = text;
    return false;
  };
  if ( QFileInfo::exists( destination ) )
    return fail( u"Destination already exists; choose a new snapshot name."_s );
  const int width = response.value( u"width"_s ).toInt(), height = response.value( u"height"_s ).toInt();
  if ( width <= 0 || height <= 0 || width > 16384 || height > 16384 )
    return fail( u"Invalid export dimensions."_s );
  const auto image = QImage::fromData( pixels );
  if ( image.isNull() || image.width() != width || image.height() != height )
    return fail( u"Received pixels do not match export metadata."_s );
  const auto extent = response.value( u"extent"_s ).toObject();
  for ( const auto &key : { u"xmin"_s, u"ymin"_s, u"xmax"_s, u"ymax"_s } )
    if ( !extent.value( key ).isDouble() || !std::isfinite( extent.value( key ).toDouble() ) )
      return fail( u"Missing or invalid returned extent."_s );
  const double xmin = extent.value( u"xmin"_s ).toDouble(), ymin = extent.value( u"ymin"_s ).toDouble();
  const double xmax = extent.value( u"xmax"_s ).toDouble(), ymax = extent.value( u"ymax"_s ).toDouble();
  if ( xmax <= xmin || ymax <= ymin )
    return fail( u"Empty returned extent."_s );
  const auto sr = extent.value( u"spatialReference"_s ).toObject();
  const int wkid = sr.value( u"latestWkid"_s ).toInt( sr.value( u"wkid"_s ).toInt() );
  const auto crs = wkid > 0 ? QgsCoordinateReferenceSystem( u"EPSG:%1"_s.arg( wkid ) ) : QgsCoordinateReferenceSystem::fromWkt( sr.value( u"wkt"_s ).toString() );
  if ( !crs.isValid() )
    return fail( u"The export response has no usable CRS."_s );
  const QString directory = QFileInfo( destination ).absolutePath();
  QTemporaryFile png( directory + u"/.strata-export-XXXXXX.png"_s );
  QTemporaryFile tif( directory + u"/.strata-export-XXXXXX.tif"_s );
  if ( !png.open() || !tif.open() || png.write( pixels ) != pixels.size() )
    return fail( u"Cannot stage snapshot in workspace."_s );
  png.close();
  tif.close();
  GDALAllRegister();
  auto input = GDALOpen( png.fileName().toUtf8().constData(), GA_ReadOnly );
  if ( !input )
    return fail( u"GDAL cannot reopen the received image."_s );
  auto output = GDALCreateCopy( GDALGetDriverByName( "GTiff" ), tif.fileName().toUtf8().constData(), input, false, nullptr, nullptr, nullptr );
  GDALClose( input );
  if ( !output )
    return fail( u"Could not create GeoTIFF."_s );
  double transform[6] = { xmin, ( xmax - xmin ) / width, 0, ymax, 0, -( ymax - ymin ) / height };
  const QByteArray footprint = QJsonDocument( QJsonArray { xmin, ymin, xmax, ymax } ).toJson( QJsonDocument::Compact );
  const bool written = GDALSetProjection( output, crs.toWkt().toUtf8().constData() ) == CE_None
                       && GDALSetGeoTransform( output, transform ) == CE_None
                       && GDALSetMetadataItem( output, "STRATA_EXPORT_EXTENT", footprint.constData(), nullptr ) == CE_None;
  GDALClose( output );
  if ( !written )
    return fail( u"Could not embed snapshot georeferencing."_s );
  {
    // Close the verification provider before renaming, also on Windows.
    QgsRasterLayer reopened( tif.fileName(), u"Snapshot verification"_s, u"gdal"_s );
    const auto checks = qgsAiVerifyRasterSource( &reopened );
    if ( !reopened.isValid() || checks.isEmpty() )
      return fail( u"GeoTIFF could not be reopened."_s );
    for ( auto it = checks.begin(); it != checks.end(); ++it )
      if ( !it.value().toBool() )
        return fail( u"GeoTIFF verification failed: %1"_s.arg( it.key() ) );
    if ( image.hasAlphaChannel() && reopened.bandCount() != 4 )
      return fail( u"Snapshot transparency was not preserved."_s );
  }
  if ( !QFile::rename( tif.fileName(), destination ) )
    return fail( u"Cannot publish verified snapshot; destination may already exist."_s );
  return true;
}

QString qgsAiCreateArcGisSnapshot( const QJsonObject &args, const QString &destination, QgsFeedback *feedback )
{
  const QUrl service( args.value( u"uri"_s ).toString() );
  if (
    ( service.scheme() != "https"_L1 && service.scheme() != "http"_L1 )
    || service.host().isEmpty()
    || !service.userInfo().isEmpty()
    || !service.path().endsWith( "/MapServer"_L1, Qt::CaseInsensitive )
    || service.hasQuery()
  )
    return u"Expected an ArcGIS MapServer URL without credentials or query parameters."_s;
  const auto bbox = args.value( u"bbox"_s ).toArray();
  const QString crsId = args.value( u"crs"_s ).toString();
  const auto crs = QgsCoordinateReferenceSystem( crsId );
  const auto layers = args.value( u"layers"_s ).toArray();
  if ( bbox.size() != 4 || !crs.isValid() || layers.isEmpty() )
    return u"Snapshot requires bbox [xmin,ymin,xmax,ymax], CRS and selected layer IDs."_s;
  QStringList coordinates, ids;
  for ( const auto &value : bbox )
  {
    if ( !value.isDouble() || !std::isfinite( value.toDouble() ) )
      return u"Bounding coordinates must be finite numbers."_s;
    coordinates << QString::number( value.toDouble(), 'g', 17 );
  }
  if ( bbox[2].toDouble() <= bbox[0].toDouble() || bbox[3].toDouble() <= bbox[1].toDouble() )
    return u"Bounding area must be positive."_s;
  for ( const auto &value : layers )
  {
    if ( !value.isDouble() || value.toDouble() != value.toInt() || value.toInt() < 0 )
      return u"Layer IDs must be non-negative integers."_s;
    ids << QString::number( value.toInt() );
  }
  QString error;
  auto get = [&error, feedback]( const QUrl &url ) {
    QNetworkRequest request( url );
    request.setTransferTimeout( 60000 );
    request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy );
    QgsBlockingNetworkRequest network;
    if ( network.get( request, true, feedback ) != QgsBlockingNetworkRequest::NoError || network.reply().attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt() != 200 )
    {
      error = u"ArcGIS download failed or was canceled."_s;
      return QByteArray();
    }
    return network.reply().content();
  };
  QUrl metadataUrl( service );
  metadataUrl.setQuery( u"f=json"_s );
  const auto metadata = QJsonDocument::fromJson( get( metadataUrl ) ).object();
  if ( !error.isEmpty() )
    return error;
  if ( metadata.isEmpty() || metadata.contains( u"error"_s ) )
    return u"Cannot read ArcGIS MapServer capabilities."_s;
  const int maxWidth = std::clamp( metadata.value( u"maxImageWidth"_s ).toInt( 4096 ), 1, 16384 );
  const int maxHeight = std::clamp( metadata.value( u"maxImageHeight"_s ).toInt( 4096 ), 1, 16384 );
  const int requestedWidth = std::max( 1, args.value( u"width"_s ).toInt( 2048 ) );
  const int requestedHeight = std::max( 1, args.value( u"height"_s ).toInt( 2048 ) );
  const double scale = std::min( { 1.0, static_cast<double>( maxWidth ) / requestedWidth, static_cast<double>( maxHeight ) / requestedHeight } );
  const int width = std::max( 1, static_cast<int>( std::floor( requestedWidth * scale ) ) );
  const int height = std::max( 1, static_cast<int>( std::floor( requestedHeight * scale ) ) );
  const QString spatialRef = QString::fromUtf8( QJsonDocument( QJsonObject { { u"wkt"_s, crs.toWkt() } } ).toJson( QJsonDocument::Compact ) );
  QUrl exportUrl( service.toString() + u"/export"_s );
  QUrlQuery query;
  query.addQueryItem( u"f"_s, u"json"_s );
  query.addQueryItem( u"bbox"_s, coordinates.join( ',' ) );
  query.addQueryItem( u"bboxSR"_s, spatialRef );
  query.addQueryItem( u"imageSR"_s, spatialRef );
  query.addQueryItem( u"size"_s, u"%1,%2"_s.arg( width ).arg( height ) );
  query.addQueryItem( u"format"_s, u"png32"_s );
  query.addQueryItem( u"transparent"_s, u"true"_s );
  query.addQueryItem( u"layers"_s, u"show:"_s + ids.join( ',' ) );
  exportUrl.setQuery( query );
  const auto response = QJsonDocument::fromJson( get( exportUrl ) ).object();
  if ( !error.isEmpty() )
    return error;
  const QUrl href( response.value( u"href"_s ).toString() );
  if ( !href.isValid() || href.host() != service.host() || href.scheme() != service.scheme() || href.port() != service.port() || !href.userInfo().isEmpty() )
    return u"ArcGIS export returned an invalid or cross-origin image URL."_s;
  const auto pixels = get( href );
  if ( !error.isEmpty() )
    return error;
  if ( feedback && feedback->isCanceled() )
    return u"Snapshot canceled."_s;
  if ( !qgsAiWriteArcGisGeoTiff( pixels, response, destination, error ) )
    return error;
  return {};
}
