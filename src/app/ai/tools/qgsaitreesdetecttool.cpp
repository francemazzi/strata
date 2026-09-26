/***************************************************************************
    qgsaitreesdetecttool.cpp
    ------------------------
    begin                : August 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsaitreesdetecttool.h"

#include <algorithm>
#include <cmath>

#include "qgsaimodelrouter.h"
#include "qgsaiplanclient.h"
#include "qgsaisettingsutils.h"
#include "qgsaitaskrunner.h"
#include "qgsaitoolschemautil.h"
#include "qgsnetworkaccessmanager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QString>
#include <QTimer>
#include <QUrl>

using namespace Qt::StringLiterals;

namespace trees_tool
{
  constexpr int REQUEST_TIMEOUT_MS = 20000;

  struct JsonResponse
  {
      bool success = false;
      int httpStatus = 0;
      QJsonObject body;
      QString error;
      //! The user pressed Stop while the request was pending.
      bool canceled = false;
  };

  JsonResponse sendJsonRequest( QgsNetworkAccessManager *networkManager, const QNetworkRequest &request, const QJsonObject *body )
  {
    QNetworkReply *reply = body ? networkManager->post( request, QJsonDocument( *body ).toJson( QJsonDocument::Compact ) ) : networkManager->get( request );
    if ( !reply )
      return { false, 0, {}, u"Unable to start the Strata trees request."_s };

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot( true );
    QObject::connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
    QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
    bool canceled = false;
    {
      const QgsAiCancelHookScope cancelScope( u"Strata trees detection"_s, [&loop]() { loop.quit(); } );
      timer.start( REQUEST_TIMEOUT_MS );
      loop.exec();
      canceled = cancelScope.canceledByUser();
    }

    const bool timedOut = !canceled && !timer.isActive();
    if ( canceled || timedOut )
      reply->abort();
    const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    const QByteArray responseBytes = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorString = reply->errorString();
    reply->deleteLater();

    if ( canceled )
    {
      JsonResponse response;
      response.error = u"Strata trees detection was canceled."_s;
      response.canceled = true;
      return response;
    }
    if ( timedOut )
      return { false, httpStatus, {}, u"Strata trees request timed out."_s };
    if ( networkError != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
    {
      const QString detail = responseBytes.isEmpty() ? networkErrorString : QString::fromUtf8( responseBytes.left( 500 ) );
      return { false, httpStatus, {}, u"Strata trees request failed (HTTP %1): %2"_s.arg( httpStatus ).arg( detail ) };
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson( responseBytes, &parseError );
    if ( parseError.error != QJsonParseError::NoError || !document.isObject() )
      return { false, httpStatus, {}, u"Strata trees returned a non-JSON response."_s };
    return { true, httpStatus, document.object(), {} };
  }

  QgsAiToolResult verifiedArtifactResult( const QString &jobId, const QJsonObject &job )
  {
    const QJsonObject artifact = job.value( u"artifact"_s ).toObject();
    const QString downloadUrlString = artifact.value( u"downloadUrl"_s ).toString().trimmed();
    const QUrl downloadUrl( downloadUrlString, QUrl::StrictMode );
    if ( !downloadUrl.isValid() || downloadUrl.host().isEmpty() )
      return QgsAiToolResult::error( u"Trees job '%1' returned an invalid artifact downloadUrl."_s.arg( jobId ) );
    const QString scheme = downloadUrl.scheme().toLower();
    if ( scheme != "https"_L1 && !( scheme == "http"_L1 && QgsAiSettingsUtils::isLocalHttpHost( downloadUrl.host() ) ) )
      return QgsAiToolResult::error( u"Trees artifact downloadUrl must use HTTPS (localhost HTTP is allowed for development)."_s );

    const QString sha256 = artifact.value( u"sha256"_s ).toString().trimmed().toLower();
    static const QRegularExpression sha256Pattern( u"^[0-9a-f]{64}$"_s );
    if ( !sha256Pattern.match( sha256 ).hasMatch() )
      return QgsAiToolResult::error( u"Trees job '%1' returned an invalid artifact SHA-256."_s.arg( jobId ) );

    const QJsonValue sizeValue = artifact.value( u"sizeBytes"_s );
    bool sizeOk = false;
    const double sizeBytes = sizeValue.isString() ? sizeValue.toString().toDouble( &sizeOk ) : sizeValue.toDouble( -1 );
    sizeOk = sizeValue.isDouble() || sizeOk;
    if ( !sizeOk || !std::isfinite( sizeBytes ) || sizeBytes < 0 || std::floor( sizeBytes ) != sizeBytes )
      return QgsAiToolResult::error( u"Trees job '%1' returned an invalid artifact sizeBytes."_s.arg( jobId ) );

    const QString format = artifact.value( u"format"_s ).toString().trimmed().toLower();
    if ( format != "geojson"_L1 )
      return QgsAiToolResult::error( u"Trees job '%1' returned unsupported artifact format '%2'."_s.arg( jobId, format ) );

    const QString expiresAt = artifact.value( u"expiresAt"_s ).toString().trimmed();
    if ( !QDateTime::fromString( expiresAt, Qt::ISODate ).isValid() )
      return QgsAiToolResult::error( u"Trees job '%1' returned an invalid artifact expiresAt."_s.arg( jobId ) );

    const QJsonObject provenance = job.value( u"provenance"_s ).toObject();
    const QStringList requiredProvenanceFields {
      u"sourceId"_s,
      u"endpointId"_s,
      u"sourceName"_s,
      u"serviceUri"_s,
      u"license"_s,
      u"attribution"_s,
      u"trustLevel"_s,
    };
    for ( const QString &field : requiredProvenanceFields )
    {
      if ( provenance.value( field ).toString().trimmed().isEmpty() )
        return QgsAiToolResult::error( u"Trees job '%1' returned incomplete provenance: missing %2."_s.arg( jobId, field ) );
    }

    const QJsonObject quality = job.value( u"quality"_s ).toObject();
    const bool qualityReported = job.value( u"quality"_s ).isObject() && quality.contains( u"counts"_s ) && quality.contains( u"imageryClass"_s ) && quality.contains( u"confidenceKind"_s );
    if ( !qualityReported )
      return QgsAiToolResult::error( u"Trees job '%1' returned no quality report."_s.arg( jobId ) );

    QJsonObject verifiedArtifact;
    verifiedArtifact.insert( u"downloadUrl"_s, downloadUrl.toString( QUrl::FullyEncoded ) );
    verifiedArtifact.insert( u"sha256"_s, sha256 );
    verifiedArtifact.insert( u"sizeBytes"_s, sizeBytes );
    verifiedArtifact.insert( u"format"_s, format );
    verifiedArtifact.insert( u"expiresAt"_s, expiresAt );

    QJsonObject output;
    output.insert( u"jobId"_s, jobId );
    output.insert( u"status"_s, u"completed"_s );
    output.insert( u"artifact"_s, verifiedArtifact );
    output.insert( u"provenance"_s, provenance );
    output.insert( u"quality"_s, quality );
    output.insert( u"artifactMetadataVerified"_s, true );
    output.insert(
      u"quality_checks"_s,
      QJsonObject {
        { u"artifact_metadata_valid"_s, true },
        { u"sha256_present"_s, true },
        { u"provenance_complete"_s, true },
        { u"quality_reported"_s, true },
        { u"passed"_s, true },
      }
    );
    output.insert( u"nextStep"_s, u"Call download_file with artifact.downloadUrl and expected_sha256, then add_layer_from_file. Report quality.imageryClass, quality.counts, and that height/DBH are estimates (estimate=true). Do not describe basemap_fallback as official AGEA orthophoto."_s );
    return QgsAiToolResult::ok( output );
  }
} // namespace trees_tool

QgsAiTreesDetectTool::QgsAiTreesDetectTool( QgsAiModelRouter *router, int pollIntervalMs, int maxPollAttempts )
  : mRouter( router )
  , mPollIntervalMs( std::max( 0, pollIntervalMs ) )
  , mMaxPollAttempts( std::max( 1, maxPollAttempts ) )
{}

QString QgsAiTreesDetectTool::description() const
{
  return u"Submits a public-tree detection job for an Italian bbox (street rows and parks, private parcels excluded) and polls until a GeoJSON artifact is ready. Region is optional and inferred from the bbox. Height and DBH are crown-allometry estimates, not field measurements. Validates download URL, SHA-256, size, expiry and the quality report; it never downloads or loads the layer. Pass the result to download_file (including expected_sha256), then add_layer_from_file. Always report quality.imageryClass and quality.counts to the user."_s;
}

QJsonObject QgsAiTreesDetectTool::schema() const
{
  QJsonObject properties;
  QJsonObject coordinate;
  coordinate.insert( u"type"_s, u"number"_s );
  QJsonObject bbox = propArray( coordinate, u"Detection bounds as [minX, minY, maxX, maxY] in WGS84. Use a municipal extent (max 150 km²)."_s );
  bbox.insert( u"minItems"_s, 4 );
  bbox.insert( u"maxItems"_s, 4 );
  properties.insert( u"bbox"_s, bbox );
  QJsonObject region = prop( u"string"_s, u"Italian region slug. Optional; inferred from the bbox centroid when omitted."_s );
  region.insert(
    u"enum"_s,
    QJsonArray {
      u"valle-aosta"_s,
      u"piemonte"_s,
      u"liguria"_s,
      u"lombardia"_s,
      u"trentino-alto-adige"_s,
      u"veneto"_s,
      u"friuli-venezia-giulia"_s,
      u"emilia-romagna"_s,
      u"toscana"_s,
      u"umbria"_s,
      u"marche"_s,
      u"lazio"_s,
      u"abruzzo"_s,
      u"molise"_s,
      u"campania"_s,
      u"puglia"_s,
      u"basilicata"_s,
      u"calabria"_s,
      u"sicilia"_s,
      u"sardegna"_s
    }
  );
  properties.insert( u"region"_s, region );
  QJsonObject format = prop( u"string"_s, u"Artifact format. v1 returns GeoJSON points."_s );
  format.insert( u"enum"_s, QJsonArray { u"geojson"_s } );
  properties.insert( u"format"_s, format );

  return schemaObject( properties, QJsonArray { u"bbox"_s, u"format"_s } );
}

bool QgsAiTreesDetectTool::isAvailable() const
{
  return mRouter && QgsAiModelRouter::isUsablePlanEndpoint( mRouter->providerSettings( QgsAiModelRouter::Provider::Plan ).endpoint ) && !mRouter->planSessionToken().trimmed().isEmpty();
}

QString QgsAiTreesDetectTool::availabilityReason() const
{
  if ( !mRouter )
    return u"Strata Plan router is not configured."_s;
  if ( !QgsAiModelRouter::isUsablePlanEndpoint( mRouter->providerSettings( QgsAiModelRouter::Provider::Plan ).endpoint ) )
    return u"Strata Plan endpoint is not configured."_s;
  return u"Sign in to Strata Plan to detect public trees."_s;
}

QgsAiToolResult QgsAiTreesDetectTool::execute( const QJsonObject &args )
{
  if ( !isAvailable() )
    return QgsAiToolResult::error( availabilityReason() );

  const QJsonArray bbox = args.value( u"bbox"_s ).toArray();
  if ( bbox.size() != 4 )
    return QgsAiToolResult::error( u"bbox must contain exactly four coordinates: [minX, minY, maxX, maxY]."_s );
  double coordinates[4];
  for ( int i = 0; i < 4; ++i )
  {
    if ( !bbox.at( i ).isDouble() || !std::isfinite( bbox.at( i ).toDouble() ) )
      return QgsAiToolResult::error( u"bbox coordinates must be finite numbers."_s );
    coordinates[i] = bbox.at( i ).toDouble();
  }
  if ( coordinates[0] >= coordinates[2] || coordinates[1] >= coordinates[3] )
    return QgsAiToolResult::error( u"bbox requires minX < maxX and minY < maxY."_s );

  const QString region = args.value( u"region"_s ).toString().trimmed().toLower();
  static const QStringList allowedRegions {
    u"valle-aosta"_s,
    u"piemonte"_s,
    u"liguria"_s,
    u"lombardia"_s,
    u"trentino-alto-adige"_s,
    u"veneto"_s,
    u"friuli-venezia-giulia"_s,
    u"emilia-romagna"_s,
    u"toscana"_s,
    u"umbria"_s,
    u"marche"_s,
    u"lazio"_s,
    u"abruzzo"_s,
    u"molise"_s,
    u"campania"_s,
    u"puglia"_s,
    u"basilicata"_s,
    u"calabria"_s,
    u"sicilia"_s,
    u"sardegna"_s
  };
  if ( !region.isEmpty() && !allowedRegions.contains( region ) )
    return QgsAiToolResult::error( u"Argument 'region' must be an Italian region slug (or omitted to infer from bbox)."_s );

  const QString format = args.value( u"format"_s ).toString().trimmed().toLower();
  if ( format != "geojson"_L1 )
    return QgsAiToolResult::error( u"Argument 'format' must be 'geojson'."_s );

  QgsNetworkAccessManager *networkManager = QgsNetworkAccessManager::instance();
  if ( !networkManager )
    return QgsAiToolResult::error( u"Network manager is not available."_s );
  const QString apiBase = QgsAiPlanClient::apiBaseForChatEndpoint( mRouter->providerSettings( QgsAiModelRouter::Provider::Plan ).endpoint );
  if ( apiBase.isEmpty() )
    return QgsAiToolResult::error( u"Cannot derive Strata Plan API base from the configured chat endpoint."_s );

  const QByteArray authorization = ( u"Bearer %1"_s.arg( mRouter->planSessionToken().trimmed() ) ).toUtf8();
  const auto requestForPath = [apiBase, authorization]( const QString &path ) {
    QNetworkRequest request( QUrl( apiBase + path ) );
    request.setHeader( QNetworkRequest::ContentTypeHeader, u"application/json"_s );
    request.setRawHeader( "Accept", "application/json" );
    request.setRawHeader( "Authorization", authorization );
    request.setTransferTimeout( trees_tool::REQUEST_TIMEOUT_MS );
    return request;
  };

  QJsonObject submitBody;
  submitBody.insert( u"bbox"_s, bbox );
  if ( !region.isEmpty() )
    submitBody.insert( u"region"_s, region );
  submitBody.insert( u"format"_s, format );
  mRouter->appendManagedToolContext( submitBody );

  const QNetworkRequest submitRequest = requestForPath( u"/v1/trees/detect"_s );
  const trees_tool::JsonResponse submit = trees_tool::sendJsonRequest( networkManager, submitRequest, &submitBody );
  if ( submit.canceled )
    return QgsAiToolResult::canceledResult( submit.error );
  if ( !submit.success )
    return QgsAiToolResult::error( submit.error );
  if ( submit.httpStatus != 202 )
    return QgsAiToolResult::error( u"Trees detection submission returned HTTP %1; expected 202."_s.arg( submit.httpStatus ) );

  QString jobId = submit.body.value( u"jobId"_s ).toString().trimmed();
  if ( jobId.isEmpty() )
    jobId = submit.body.value( u"id"_s ).toString().trimmed();
  if ( jobId.isEmpty() )
    return QgsAiToolResult::error( u"Trees detection response did not include jobId."_s );

  const QString encodedJobId = QString::fromUtf8( QUrl::toPercentEncoding( jobId ) );
  // Once Strata stops waiting, the job is canceled on the server too: it would keep spending quota.
  const auto cancelRemoteJob = [&requestForPath, &encodedJobId]() { QgsAiPlanClient::requestRemoteJobCancel( requestForPath( u"/v1/trees/jobs/%1/cancel"_s.arg( encodedJobId ) ) ); };
  for ( int attempt = 0; attempt < mMaxPollAttempts; ++attempt )
  {
    if ( QCoreApplication::closingDown() )
    {
      cancelRemoteJob();
      return QgsAiToolResult::error( u"Trees detection polling was cancelled because the application is closing."_s );
    }
    if ( attempt > 0 && mPollIntervalMs > 0 )
    {
      QEventLoop waitLoop;
      const QgsAiCancelHookScope waitScope( u"Strata trees detection"_s, [&waitLoop]() { waitLoop.quit(); } );
      QTimer::singleShot( mPollIntervalMs, &waitLoop, &QEventLoop::quit );
      waitLoop.exec();
      if ( waitScope.canceledByUser() )
      {
        cancelRemoteJob();
        return QgsAiToolResult::canceledResult( u"Strata trees detection was canceled; the job was canceled on the server too."_s );
      }
    }

    const QNetworkRequest pollRequest = requestForPath( u"/v1/trees/jobs/"_s + encodedJobId );
    const trees_tool::JsonResponse poll = trees_tool::sendJsonRequest( networkManager, pollRequest, nullptr );
    if ( poll.canceled )
    {
      cancelRemoteJob();
      return QgsAiToolResult::canceledResult( poll.error );
    }
    if ( !poll.success )
      return QgsAiToolResult::error( poll.error );

    const QString status = poll.body.value( u"status"_s ).toString().trimmed().toLower();
    if ( status == "completed"_L1 || status == "succeeded"_L1 )
      return trees_tool::verifiedArtifactResult( jobId, poll.body );
    if ( status == "failed"_L1 || status == "cancelled"_L1 || status == "expired"_L1 )
      return QgsAiToolResult::error( u"Trees detection job '%1' ended with status '%2'."_s.arg( jobId, status ) );
    if ( status != "queued"_L1 && status != "pending"_L1 && status != "running"_L1 && status != "processing"_L1 )
      return QgsAiToolResult::error( u"Trees detection job '%1' returned unknown status '%2'."_s.arg( jobId, status ) );
  }

  cancelRemoteJob();
  return QgsAiToolResult::error( u"Trees detection job '%1' did not finish after %2 polling attempts and was canceled."_s.arg( jobId ).arg( mMaxPollAttempts ) );
}
