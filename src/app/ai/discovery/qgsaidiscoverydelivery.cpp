// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaidiscoverycontroller.h"
#include "qgsaidiscoverydownload.h"
#include "qgsaidiscoveryfiles.h"
#include "qgsaidiscoveryimport.h"
#include "qgsaifilecontextprovider.h"
#include "qgsaimodelrouter.h"
#include "qgsaiworkspacetrust.h"
#include "qgslayertree.h"
#include "qgsnetworkaccessmanager.h"
#include "qgsproject.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPushButton>
#include <QSaveFile>
#include <QString>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;
void QgsAiDiscoveryController::deliver( const QJsonObject &run )
{
  const QString id = run.value( u"id"_s ).toString();
  const auto grant = mGrants.value( id ).toObject();
  if ( grant.isEmpty() || grant.value( u"imported"_s ).toBool() || grant.value( u"canceled"_s ).toBool() || mDelivering.contains( id ) )
    return;
  // A saved project can survive a crash between adding layers and saving the local grant.
  if ( mProject )
    for ( auto group : mProject->layerTreeRoot()->findGroups( true ) )
      if ( group->customProperty( u"strata/discovery/runId"_s ).toString() == id && !group->findLayers().isEmpty() )
      {
        auto recovered = grant;
        recovered.insert( u"imported"_s, true );
        mGrants.insert( id, recovered );
        saveGrants();
        return;
      }
  if ( grant.value( u"planId"_s ) != run.value( u"planId"_s )
       || grant.value( u"version"_s ) != run.value( u"planVersion"_s )
       || grant.value( u"maxCredits"_s ) != run.value( u"budget"_s ).toObject().value( u"maxCredits"_s ) )
  {
    emit message( tr( "Autorizzazione del run non corrispondente all'anteprima." ) ); // # spellok: Italian UI text.
    return;
  }
  const auto selection = grant.value( u"selection"_s ).toArray();
  const auto actual = run.value( u"selection"_s ).toArray();
  if ( actual.size() != selection.size() )
    return;
  for ( int i = 0; i < actual.size(); ++i )
  {
    for ( const auto &key : { u"candidateId"_s, u"mode"_s } )
      if ( actual[i].toObject().value( key ) != selection[i].toObject().value( key ) )
        return;
    if ( actual[i].toObject().value( u"allowUnverifiedContent"_s ).toBool() != selection[i].toObject().value( u"allowUnverifiedContent"_s ).toBool() )
      return;
    if ( selection[i].toObject().contains( u"maxBytes"_s ) && actual[i].toObject().value( u"maxBytes"_s ).toInteger() > selection[i].toObject().value( u"maxBytes"_s ).toInteger() )
      return;
  }
  const QString root = grant.value( u"root"_s ).toString();
  if ( !mFiles || mFiles->workspaceRoot() != root || !QgsAiWorkspaceTrust::isTrusted( root ) )
    return;
  const auto artifact = run.value( u"artifact"_s ).toObject();
  const QString path = grant.value( u"destination"_s ).toString() + u".zip"_s;
  const QString currentScope = mScope;
  mDelivering.insert( id );
  if ( QFileInfo::exists( path ) )
  {
    verifyImport( id, path, grant, artifact );
    return;
  }
  auto download = new QgsAiDiscoveryDownload( QgsNetworkAccessManager::instance(), this );
  mDownloads.insert( id, download );
  connect( download, &QgsAiDiscoveryDownload::finished, this, [this, download, id, grant, artifact, currentScope]( const QString &path, const QString &error ) {
    download->deleteLater();
    mDownloads.remove( id );
    if ( mScope != currentScope || mGrants.value( id ).toObject().value( u"canceled"_s ).toBool() )
    {
      mDelivering.remove( id );
      return;
    }
    if ( !error.isEmpty() )
    {
      mDelivering.remove( id );
      emit message( error );
      return;
    }
    verifyImport( id, path, grant, artifact );
  } );
  download->start( QUrl( artifact.value( u"downloadUrl"_s ).toString() ), root, path, artifact.value( u"sha256"_s ).toString(), artifact.value( u"sizeBytes"_s ).toString().toLongLong() );
}
void QgsAiDiscoveryController::verifyImport( const QString &runId, const QString &path, const QJsonObject &grant, const QJsonObject &artifact )
{
  if ( !mProject )
  {
    mDelivering.remove( runId );
    return;
  }
  const QString currentScope = mScope;
  const auto project = mProject;
  const auto context = mProject->transformContext();
  auto mainThread = thread();
  const auto directory = grant.value( u"destination"_s ).toString();
  // Keep cleanup alive if the chat/controller closes while a provider is opening a layer.
  auto watcher = new QFutureWatcher<QgsAiDiscoveryImport::Result>();
  const QPointer<QgsAiDiscoveryController> self( this );
  connect( watcher, &QFutureWatcherBase::finished, watcher, [this, self, watcher, currentScope, runId, project, directory]() {
    const auto result = watcher->result();
    watcher->deleteLater();
    if ( !self )
    {
      qDeleteAll( result.layers );
      return;
    }
    mDelivering.remove( runId );
    if ( currentScope != mScope || !project || project != mProject || mGrants.value( runId ).toObject().value( u"canceled"_s ).toBool() )
    {
      qDeleteAll( result.layers );
      return;
    }
    if ( !result.layers.isEmpty() )
    {
      auto group = project->layerTreeRoot()->addGroup( tr( "Discovery · %1" ).arg( runId ) );
      group->setCustomProperty( u"strata/discovery/runId"_s, runId );
      for ( auto layer : result.layers )
      {
        project->addMapLayer( layer, false );
        group->addLayer( layer );
      }
    }
    QSaveFile report( QDir( directory ).filePath( u"desktop-review.json"_s ) );
    if ( QgsAiDiscoveryFiles::destinationError( mWorkspaceRoot, report.fileName() ).isEmpty() && report.open( QIODevice::WriteOnly ) )
    {
      report.write( QJsonDocument( result.outcome ).toJson() );
      report.commit();
    }
    auto grant = mGrants.value( runId ).toObject();
    grant.insert( u"imported"_s, !result.layers.isEmpty() );
    mGrants.insert( runId, grant );
    saveGrants();
    if ( mCancelButtons.value( runId ) )
      mCancelButtons.value( runId )->setEnabled( false );
    recordImport( runId, result.outcome );
    emit message(
      tr( "Importazione QGIS %1: %2 layer. CRS nativi conservati; riproiezione e clip raster non eseguiti." ).arg( result.outcome.value( u"status"_s ).toString() ).arg( result.layers.size() )
    );
  } );
  watcher->setFuture( QtConcurrent::run( [path, grant, artifact, directory, context, mainThread]() {
    const auto error = QgsAiDiscoveryFiles::destinationError( grant.value( u"root"_s ).toString(), directory );
    if ( !error.isEmpty() )
      return QgsAiDiscoveryImport::Result { {}, { { u"status"_s, u"FAILED"_s }, { u"error"_s, error }, { u"reprojectionPerformed"_s, false }, { u"rasterClipPerformed"_s, false } } };
    const auto unpacked = QgsAiDiscoveryFiles::unpack( path, directory, artifact.value( u"sha256"_s ).toString(), grant.value( u"approved"_s ).toArray() );
    if ( !unpacked.error.isEmpty() )
      return QgsAiDiscoveryImport::Result { {}, { { u"status"_s, u"FAILED"_s }, { u"error"_s, unpacked.error }, { u"reprojectionPerformed"_s, false }, { u"rasterClipPerformed"_s, false } } };
    return QgsAiDiscoveryImport::open( unpacked.manifest, directory, context, mainThread );
  } ) );
}
void QgsAiDiscoveryController::recordImport( const QString &runId, const QJsonObject &outcome )
{
  QJsonObject
    body { { u"planVersion"_s, mGrants.value( runId ).toObject().value( u"version"_s ) }, { u"reason"_s, u"Verifica e importazione desktop della selezione approvata"_s }, { u"outcome"_s, outcome } };
  mRouter->appendManagedToolContext( body );
  body.insert( u"workspaceId"_s, QgsAiWorkspaceTrust::workspaceHash( mWorkspaceRoot ) );
  mClient->submit( u"/v1/discovery/runs/%1/reviews"_s.arg( runId ), body );
}
