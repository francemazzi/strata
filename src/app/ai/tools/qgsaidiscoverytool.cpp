// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaidiscoverytool.h"

#include "../discovery/qgsaidiscoverycontroller.h"
#include "qgsaitoolschemautil.h"

#include <QString>

using namespace Qt::StringLiterals;

QgsAiDiscoveryTool::QgsAiDiscoveryTool( const QString &name, QgsAiDiscoveryController *controller )
  : mName( name )
  , mController( controller )
{}
bool QgsAiDiscoveryTool::isAvailable() const
{
  return mController && mController->available();
}
QString QgsAiDiscoveryTool::description() const
{
  if ( mName == "discovery_resolve"_L1 )
    return u"Resolve a GIS research intent to a verified AOI and versioned workflow/profile. Returns requestId immediately; use discovery_status(kind=resolve) for the result. Never invent a bbox. useMap only when the user chooses the current map."_s;
  if ( mName == "discovery_search"_L1 )
    return u"Start bounded source research from a ready discovery_resolve requestId. Maximum 20 search credits, reducible with maxCredits. Returns immediately; a selectable preview appears in chat. No downloads."_s;
  if ( mName == "discovery_run"_L1 )
    return u"Show an existing plan's preview for user selection and one explicit acquisition confirmation. The user selects candidates, file/service, destination and budget. The agent cannot approve on the user's behalf."_s;
  if ( mName == "discovery_cancel"_L1 )
    return u"Cancel an owned plan/run and any corresponding desktop download. Idempotent; returns immediately."_s;
  return u"Read cached discovery status and refresh asynchronously. Use requestId for resolve or resource id for plans/runs. Polling and resumption do not block chat. Operational success, requirement coverage and QGIS import are separate."_s;
}
QJsonObject QgsAiDiscoveryTool::schema() const
{
  QJsonObject properties;
  QJsonArray required;
  if ( mName == "discovery_resolve"_L1 )
  {
    properties.insert( u"intent"_s, prop( u"string"_s, u"User's GIS data request."_s ) );
    required.append( u"intent"_s );
    for ( const auto &key : { u"comune"_s, u"territory"_s, u"administrativeCode"_s } )
      properties.insert( key, prop( u"string"_s, key ) );
    properties.insert( u"workflow"_s, QJsonObject { { u"type"_s, u"string"_s }, { u"enum"_s, QJsonArray { u"inquadramento"_s, u"verde_urbano"_s, u"vincoli"_s, u"fotovoltaico"_s, u"forestale"_s } } } );
    properties.insert( u"profile"_s, QJsonObject { { u"type"_s, u"string"_s }, { u"enum"_s, QJsonArray { u"public_green_trees"_s, u"nbs_urban_climate"_s } } } );
    properties.insert( u"useMap"_s, prop( u"boolean"_s, u"Use current map only when explicitly selected by the user. Extent is transformed to EPSG:4326 locally."_s ) );
    properties.insert( u"bbox"_s, QJsonObject { { u"type"_s, u"array"_s }, { u"items"_s, QJsonObject { { u"type"_s, u"number"_s } } }, { u"minItems"_s, 4 }, { u"maxItems"_s, 4 } } );
    properties.insert( u"trustedOnly"_s, prop( u"boolean"_s, u"Use curated publishers only. Defaults true."_s ) );
  }
  else if ( mName == "discovery_search"_L1 )
  {
    properties.insert( u"resolutionId"_s, prop( u"string"_s, u"Request ID of a ready discovery_resolve result."_s ) );
    required.append( u"resolutionId"_s );
    properties.insert( u"maxCredits"_s, QJsonObject { { u"type"_s, u"integer"_s }, { u"minimum"_s, 0 }, { u"maximum"_s, 20 }, { u"default"_s, 20 } } );
  }
  else
  {
    properties.insert( u"id"_s, prop( u"string"_s, u"Plan/run ID or local resolve request ID."_s ) );
    required.append( u"id"_s );
    QJsonArray kinds { u"plans"_s };
    if ( mName != "discovery_run"_L1 )
      kinds.append( u"runs"_s );
    if ( mName == "discovery_status"_L1 )
      kinds.append( u"resolve"_s );
    properties.insert( u"kind"_s, QJsonObject { { u"type"_s, u"string"_s }, { u"enum"_s, kinds } } );
  }
  return schemaObject( properties, required );
}
QgsAiToolResult QgsAiDiscoveryTool::execute( const QJsonObject &args )
{
  if ( !isAvailable() )
    return QgsAiToolResult::error( u"Strata Plan is not connected."_s );
  const auto result = mController->execute( mName, args );
  return result.contains( u"error"_s ) ? QgsAiToolResult::error( result.value( u"error"_s ).toString() ) : QgsAiToolResult::ok( result );
}
