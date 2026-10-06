/***************************************************************************
    qgsaicrsutils.cpp
    Copyright (C) 2026 Francesco Mazzi
    SPDX-License-Identifier: GPL-2.0-or-later
***************************************************************************/
#include "qgsaicrsutils.h"
#include "qgsmaplayer.h"
#include "qgsvectorlayer.h"
#include "qgsvectordataprovider.h"
#include <QRegularExpression>

using namespace Qt::StringLiterals;

QString qgsAiCrsLabel( const QJsonObject &details )
{
  QString text = details.value( u"label"_s ).toString();
  text.replace( QRegularExpression( u"[\\x00-\\x1f\\x7f<>]"_s ), u" "_s );
  return text.simplified().left( 256 );
}

QJsonObject qgsAiCrsDetails( const QgsCoordinateReferenceSystem &crs, bool spatial, const QVariantMap &resolution )
{
  const QString authid = crs.authid();
  const QString authority = authid.section( ':', 0, 0 ).toUpper();
  const bool portable = !authid.isEmpty() && authority != "USER"_L1 && authority != "QGIS"_L1 && authority != "CUSTOM"_L1;
  const bool valid = spatial && crs.isValid();
  QString status = !spatial ? u"not_applicable"_s : valid ? u"valid"_s : resolution.value( u"status"_s, u"missing"_s ).toString();
  if ( !valid && spatial && status == "valid"_L1 )
    status = u"unresolved"_s;
  QString label = !spatial                    ? u"Not applicable (non-spatial layer)"_s
                  : valid                     ? ( portable ? authid : u"Custom CRS (valid)"_s )
                  : status == "unresolved"_L1 ? u"CRS unresolved"_s
                                              : u"CRS not specified"_s;
  if ( valid && !portable && !crs.description().isEmpty() && crs.description() != "unknown"_L1 )
    label += u" — %1"_s.arg( crs.description() );
  QJsonObject result { { u"is_valid"_s, valid }, { u"authid"_s, authid }, { u"description"_s, crs.description() }, { u"status"_s, status }, { u"label"_s, label } };
  result.insert( u"label"_s, qgsAiCrsLabel( result ) );
  if ( valid && !portable )
    result.insert( u"wkt"_s, crs.toWkt( Qgis::CrsWktVariant::Preferred ) );
  if ( spatial && resolution.contains( u"source_srid"_s ) )
    result.insert( u"source_srid"_s, QJsonValue::fromVariant( resolution.value( u"source_srid"_s ) ) );
  if ( spatial && !valid && resolution.contains( u"diagnostic"_s ) )
    result.insert( u"diagnostic"_s, QJsonObject::fromVariantMap( resolution.value( u"diagnostic"_s ).toMap() ) );
  return result;
}

QJsonObject qgsAiLayerCrsDetails( const QgsMapLayer *layer )
{
  if ( !layer )
    return qgsAiCrsDetails( QgsCoordinateReferenceSystem() );
  QVariantMap resolution;
  if ( const auto *vector = qobject_cast<const QgsVectorLayer *>( layer ); vector && vector->dataProvider() && vector->providerType() == "postgres"_L1 )
    resolution = vector->dataProvider()->property( "crsResolution" ).toMap();
  return qgsAiCrsDetails( layer->crs(), layer->isSpatial(), resolution );
}
