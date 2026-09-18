/***************************************************************************
    qgsaidatahubextracttool.cpp
    ---------------------------
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

#include "qgsaidatahubextracttool.h"

#include <algorithm>
#include <cmath>

#include "qgsaimodelrouter.h"
#include "qgsaiplanclient.h"
#include "qgsaisettingsutils.h"
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

namespace datahub_tool
{
  constexpr int REQUEST_TIMEOUT_MS = 20000;

  struct JsonResponse
  {
      bool success = false;
      int httpStatus = 0;
      QJsonObject body;
      QString error;
  };

  JsonResponse sendJsonRequest( QgsNetworkAccessManager *networkManager, const QNetworkRequest &request, const QJsonObject *body )
  {
    QNetworkReply *reply = body ? networkManager->post( request, QJsonDocument( *body ).toJson( QJsonDocument::Compact ) ) : networkManager->get( request );
    if ( !reply )
      return { false, 0, {}, u"Unable to start the Strata DataHub request."_s };

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot( true );
    QObject::connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
    QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
    timer.start( REQUEST_TIMEOUT_MS );
    loop.exec();

    const bool timedOut = !timer.isActive();
    if ( timedOut )
      reply->abort();
    const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    const QByteArray responseBytes = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorString = reply->errorString();
    reply->deleteLater();

    if ( timedOut )
      return { false, httpStatus, {}, u"Strata DataHub request timed out."_s };
    if ( networkError != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 )
    {
      const QString detail = responseBytes.isEmpty() ? networkErrorString : QString::fromUtf8( responseBytes.left( 500 ) );
      return { false, httpStatus, {}, u"Strata DataHub request failed (HTTP %1): %2"_s.arg( httpStatus ).arg( detail ) };
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson( responseBytes, &parseError );
    if ( parseError.error != QJsonParseError::NoError || !document.isObject() )
      return { false, httpStatus, {}, u"Strata DataHub returned a non-JSON response."_s };
    return { true, httpStatus, document.object(), {} };
  }

  QgsAiToolResult verifiedArtifactResult( const QString &jobId, const QJsonObject &job )
  {
    const QJsonObject artifact = job.value( u"artifact"_s ).toObject();
    const QString downloadUrlString = artifact.value( u"downloadUrl"_s ).toString().trimmed();
    const QUrl downloadUrl( downloadUrlString, QUrl::StrictMode );
    if ( !downloadUrl.isValid() || downloadUrl.host().isEmpty() )
      return QgsAiToolResult::error( u"DataHub job '%1' returned an invalid artifact downloadUrl."_s.arg( jobId ) );
    const QString scheme = downloadUrl.scheme().toLower();
    if ( scheme != "https"_L1 && !( scheme == "http"_L1 && QgsAiSettingsUtils::isLocalHttpHost( downloadUrl.host() ) ) )
      return QgsAiToolResult::error( u"DataHub artifact downloadUrl must use HTTPS (localhost HTTP is allowed for development)."_s );

    const QString sha256 = artifact.value( u"sha256"_s ).toString().trimmed().toLower();
    static const QRegularExpression sha256Pattern( u"^[0-9a-f]{64}$"_s );
    if ( !sha256Pattern.match( sha256 ).hasMatch() )
      return QgsAiToolResult::error( u"DataHub job '%1' returned an invalid artifact SHA-256."_s.arg( jobId ) );

    const QJsonValue sizeValue = artifact.value( u"sizeBytes"_s );
    bool sizeOk = false;
    const double sizeBytes = sizeValue.isString() ? sizeValue.toString().toDouble( &sizeOk ) : sizeValue.toDouble( -1 );
    sizeOk = sizeValue.isDouble() || sizeOk;
    if ( !sizeOk || !std::isfinite( sizeBytes ) || sizeBytes < 0 || std::floor( sizeBytes ) != sizeBytes )
      return QgsAiToolResult::error( u"DataHub job '%1' returned an invalid artifact sizeBytes."_s.arg( jobId ) );

    const QString format = artifact.value( u"format"_s ).toString().trimmed().toLower();
    if ( format != "geojson"_L1 && format != "zip"_L1 )
      return QgsAiToolResult::error( u"DataHub job '%1' returned unsupported artifact format '%2'."_s.arg( jobId, format ) );

    const QString expiresAt = artifact.value( u"expiresAt"_s ).toString().trimmed();
    if ( !QDateTime::fromString( expiresAt, Qt::ISODate ).isValid() )
      return QgsAiToolResult::error( u"DataHub job '%1' returned an invalid artifact expiresAt."_s.arg( jobId ) );

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
        return QgsAiToolResult::error( u"DataHub job '%1' returned incomplete provenance: missing %2."_s.arg( jobId, field ) );
    }

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
    output.insert( u"artifactMetadataVerified"_s, true );
    output.insert(
      u"quality_checks"_s,
      QJsonObject {
        { u"artifact_metadata_valid"_s, true },
        { u"sha256_present"_s, true },
        { u"provenance_complete"_s, true },
        { u"passed"_s, true },
      }
    );
    output.insert( u"nextStep"_s, u"Call download_file with artifact.downloadUrl and expected_sha256, then add_layer_from_file if the user wants the layer loaded."_s );
    return QgsAiToolResult::ok( output );
  }
} // namespace datahub_tool

QgsAiDataHubExtractTool::QgsAiDataHubExtractTool( QgsAiModelRouter *router, int pollIntervalMs, int maxPollAttempts )
  : mRouter( router )
  , mPollIntervalMs( std::max( 0, pollIntervalMs ) )
  , mMaxPollAttempts( std::max( 1, maxPollAttempts ) )
{}

QString QgsAiDataHubExtractTool::description() const
{
  return u"Submits a DataHub extraction job and polls until an artifact is ready. Use format geojson for WFS bbox extracts, or zip for official HTTP cadastral datasets (Agenzia delle Entrate regional GetDataset.zip). Validates and returns download URL, SHA-256, size, format and expiry; it never downloads or loads the layer. Pass the result to download_file (including expected_sha256), then add_layer_from_file when appropriate."_s;
}

QJsonObject QgsAiDataHubExtractTool::schema() const
{
  QJsonObject properties;
  properties.insert( u"endpointId"_s, prop( u"string"_s, u"DataHub WFS or HTTP zip endpoint identifier returned by catalog_search."_s ) );
  properties.insert( u"catalogRecordId"_s, prop( u"string"_s, u"Optional DataHub catalog record identifier returned by catalog_search."_s ) );
  properties.insert( u"typeName"_s, prop( u"string"_s, u"WFS feature type name, or the cadastral dataset file name for HTTP zip endpoints (e.g. LOMBARDIA.zip)."_s ) );

  QJsonObject coordinate;
  coordinate.insert( u"type"_s, u"number"_s );
  QJsonObject bbox = propArray( coordinate, u"Extraction bounds as [minX, minY, maxX, maxY] in the supplied CRS."_s );
  bbox.insert( u"minItems"_s, 4 );
  bbox.insert( u"maxItems"_s, 4 );
  properties.insert( u"bbox"_s, bbox );
  QJsonObject format = prop( u"string"_s, u"Artifact format. Use geojson for WFS extracts or zip for HTTP cadastral datasets."_s );
  format.insert( u"enum"_s, QJsonArray { u"geojson"_s, u"zip"_s } );
  properties.insert( u"format"_s, format );

  return schemaObject( properties, QJsonArray { u"endpointId"_s, u"typeName"_s, u"bbox"_s, u"format"_s } );
}

bool QgsAiDataHubExtractTool::isAvailable() const
{
  return mRouter && QgsAiModelRouter::isUsablePlanEndpoint( mRouter->providerSettings( QgsAiModelRouter::Provider::Plan ).endpoint ) && !mRouter->planSessionToken().trimmed().isEmpty();
}

QString QgsAiDataHubExtractTool::availabilityReason() const
{
  if ( !mRouter )
    return u"Strata Plan router is not configured."_s;
  if ( !QgsAiModelRouter::isUsablePlanEndpoint( mRouter->providerSettings( QgsAiModelRouter::Provider::Plan ).endpoint ) )
    return u"Strata Plan endpoint is not configured."_s;
  return u"Sign in to Strata Plan to use DataHub extraction."_s;
}

QgsAiToolResult QgsAiDataHubExtractTool::execute( const QJsonObject &args )
{
  if ( !isAvailable() )
    return QgsAiToolResult::error( availabilityReason() );

  const QString endpointId = args.value( u"endpointId"_s ).toString().trimmed();
  if ( endpointId.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'endpointId' is required."_s );
  const QString catalogRecordId = args.value( u"catalogRecordId"_s ).toString().trimmed();
  const QString typeName = args.value( u"typeName"_s ).toString().trimmed();
  if ( typeName.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'typeName' is required."_s );

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

  const QString format = args.value( u"format"_s ).toString().trimmed().toLower();
  if ( format != "geojson"_L1 && format != "zip"_L1 )
    return QgsAiToolResult::error( u"Argument 'format' must be 'geojson' or 'zip'."_s );

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
    request.setTransferTimeout( datahub_tool::REQUEST_TIMEOUT_MS );
    return request;
  };

  QJsonObject submitBody;
  submitBody.insert( u"endpointId"_s, endpointId );
  if ( !catalogRecordId.isEmpty() )
    submitBody.insert( u"catalogRecordId"_s, catalogRecordId );
  submitBody.insert( u"typeName"_s, typeName );
  submitBody.insert( u"bbox"_s, bbox );
  submitBody.insert( u"format"_s, format );
  mRouter->appendManagedToolContext( submitBody );

  const QNetworkRequest submitRequest = requestForPath( u"/v1/datahub/extract"_s );
  const datahub_tool::JsonResponse submit = datahub_tool::sendJsonRequest( networkManager, submitRequest, &submitBody );
  if ( !submit.success )
    return QgsAiToolResult::error( submit.error );
  if ( submit.httpStatus != 202 )
    return QgsAiToolResult::error( u"DataHub extraction submission returned HTTP %1; expected 202."_s.arg( submit.httpStatus ) );

  QString jobId = submit.body.value( u"jobId"_s ).toString().trimmed();
  if ( jobId.isEmpty() )
    jobId = submit.body.value( u"id"_s ).toString().trimmed();
  if ( jobId.isEmpty() )
    return QgsAiToolResult::error( u"DataHub extraction response did not include jobId."_s );

  const QString encodedJobId = QString::fromUtf8( QUrl::toPercentEncoding( jobId ) );
  for ( int attempt = 0; attempt < mMaxPollAttempts; ++attempt )
  {
    if ( QCoreApplication::closingDown() )
      return QgsAiToolResult::error( u"DataHub extraction polling was cancelled because the application is closing."_s );
    if ( attempt > 0 && mPollIntervalMs > 0 )
    {
      QEventLoop waitLoop;
      QTimer::singleShot( mPollIntervalMs, &waitLoop, &QEventLoop::quit );
      waitLoop.exec();
    }

    const QNetworkRequest pollRequest = requestForPath( u"/v1/datahub/jobs/"_s + encodedJobId );
    const datahub_tool::JsonResponse poll = datahub_tool::sendJsonRequest( networkManager, pollRequest, nullptr );
    if ( !poll.success )
      return QgsAiToolResult::error( poll.error );

    const QString status = poll.body.value( u"status"_s ).toString().trimmed().toLower();
    if ( status == "completed"_L1 || status == "succeeded"_L1 )
      return datahub_tool::verifiedArtifactResult( jobId, poll.body );
    if ( status == "failed"_L1 || status == "cancelled"_L1 || status == "expired"_L1 )
      return QgsAiToolResult::error( u"DataHub extraction job '%1' ended with status '%2'."_s.arg( jobId, status ) );
    if ( status != "queued"_L1 && status != "pending"_L1 && status != "running"_L1 && status != "processing"_L1 )
      return QgsAiToolResult::error( u"DataHub extraction job '%1' returned unknown status '%2'."_s.arg( jobId, status ) );
  }

  return QgsAiToolResult::error( u"DataHub extraction job '%1' did not finish after %2 polling attempts."_s.arg( jobId ).arg( mMaxPollAttempts ) );
}
