// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaidiscoverycontroller.h"

#include "qgsaidiscoverydownload.h"
#include "qgsaidiscoveryfiles.h"
#include "qgsaidiscoverypresentation.h"
#include "qgsaidiscoverypreview.h"
#include "qgsaifilecontextprovider.h"
#include "qgsaimodelrouter.h"
#include "qgsaiplanclient.h"
#include "qgsaiworkspacetrust.h"
#include "qgscoordinatetransform.h"
#include "qgsmapcanvas.h"
#include "qgsnetworkaccessmanager.h"
#include "qgsproject.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>

#include "moc_qgsaidiscoverycontroller.cpp"

using namespace Qt::StringLiterals;

QgsAiDiscoveryController::QgsAiDiscoveryController( QgsAiModelRouter *router, QgsAiFileContextProvider *files, QgsMapCanvas *canvas, QgsProject *project, QObject *parent )
  : QObject( parent )
  , mRouter( router )
  , mFiles( files )
  , mCanvas( canvas )
  , mProject( project )
{
  mClient = new QgsAiDiscoveryClient(
    QgsNetworkAccessManager::instance(),
    [this]( QNetworkRequest &req ) {
      if ( !mRouter || !mFiles || mFiles->workspaceRoot() != mWorkspaceRoot )
        return false;
      const auto token = mRouter->planSessionToken().split( '.' );
      const auto claims = token.size() == 3 ? QJsonDocument::fromJson( QByteArray::fromBase64( token[1].toLatin1(), QByteArray::Base64UrlEncoding ) ).object() : QJsonObject();
      if ( claims.value( u"sub"_s ).toString() != mAccountId || QUrl( QgsAiPlanClient::apiBaseForChatEndpoint( mRouter->providerSettings( QgsAiModelRouter::Provider::Plan ).endpoint ) ) != mApiBase )
        return false;
      return mRouter->applyAuthentication( QgsAiModelRouter::Provider::Plan, req );
    },
    this
  );
  connect( mClient, &QgsAiDiscoveryClient::updated, this, &QgsAiDiscoveryController::updated );
  connect( mClient, &QgsAiDiscoveryClient::failed, this, [this]( const QString &id, const QString &error ) { emit message( tr( "Discovery %1: %2" ).arg( id, error ) ); } );
  connect( files, &QgsAiFileContextProvider::workspaceRootChanged, this, [this]() { scope(); } );
}
bool QgsAiDiscoveryController::available() const
{
  return mRouter && mRouter->isProviderUsable( QgsAiModelRouter::Provider::Plan );
}
bool QgsAiDiscoveryController::scope()
{
  if ( !available() || !mFiles )
  {
    mClient->pause();
    return false;
  }
  const QUrl base( QgsAiPlanClient::apiBaseForChatEndpoint( mRouter->providerSettings( QgsAiModelRouter::Provider::Plan ).endpoint ) );
  const auto token = mRouter->planSessionToken().split( '.' );
  const auto claims = token.size() == 3 ? QJsonDocument::fromJson( QByteArray::fromBase64( token[1].toLatin1(), QByteArray::Base64UrlEncoding ) ).object() : QJsonObject();
  const QString user = claims.value( u"sub"_s ).toString();
  if ( user.isEmpty() || !QgsAiDiscoveryFiles::safeUrl( base ) )
    return false;
  const QString key = u"strata/discovery/"_s
                      + QString::fromLatin1( QCryptographicHash::hash( ( base.toString() + '\n' + user + '\n' + mFiles->workspaceRoot() ).toUtf8(), QCryptographicHash::Sha256 ).toHex() );
  if ( mScope != key )
  {
    const auto downloads = mDownloads;
    for ( auto download : downloads )
      if ( download )
        download->cancel();
    mAccountId = user;
    mWorkspaceRoot = mFiles->workspaceRoot();
    mApiBase = base;
    mScope = key;
    mShown.clear();
    mLastStates.clear();
    mProgress.clear();
    mCancelButtons.clear();
    mGrants = QJsonDocument::fromJson( QSettings().value( key + u"/grants"_s ).toByteArray() ).object();
    mClient->setScope( base, key + u"/requests"_s );
  }
  return true;
}
void QgsAiDiscoveryController::resume()
{
  if ( !scope() )
    return;
  mClient->resume();
  for ( auto it = mGrants.begin(); it != mGrants.end(); ++it )
    if ( !it.value().toObject().value( u"imported"_s ).toBool() && !it.value().toObject().value( u"canceled"_s ).toBool() && mClient->snapshot( it.key() ).value( u"id"_s ) == it.key() )
      mClient->watch( u"runs"_s, it.key() );
}
void QgsAiDiscoveryController::saveGrants()
{
  QSettings().setValue( mScope + u"/grants"_s, QJsonDocument( mGrants ).toJson( QJsonDocument::Compact ) );
}
QJsonObject QgsAiDiscoveryController::execute( const QString &tool, QJsonObject args )
{
  auto error = []( const QString &text ) { return QJsonObject { { u"error"_s, text } }; };
  if ( !scope() )
    return error( tr( "Accesso Strata e workspace richiesti." ) );
  if ( tool == "discovery_status"_L1 || tool == "discovery_cancel"_L1 || tool == "discovery_run"_L1 )
  {
    QString id = args.value( u"id"_s ).toString();
    if ( !QRegularExpression( u"^[A-Za-z0-9_-]{1,100}$"_s ).match( id ).hasMatch() )
      return error( tr( "ID non valido." ) );
    const QString kind = args.value( u"kind"_s ).toString( u"plans"_s );
    if ( kind != "plans"_L1 && kind != "runs"_L1 && kind != "resolve"_L1 )
      return error( tr( "Tipo risorsa non valido." ) );
    if ( ( tool == "discovery_cancel"_L1 && kind == "resolve"_L1 ) || ( tool == "discovery_run"_L1 && kind != "plans"_L1 ) )
      return error( tr( "Tipo risorsa non valido per questo tool." ) );
    if ( tool == "discovery_cancel"_L1 )
    {
      const auto pending = mClient->snapshot( id );
      if ( pending.contains( u"id"_s ) )
        id = pending.value( u"id"_s ).toString();
      else if ( pending.contains( u"requestId"_s ) )
      {
        if ( mClient->discardUnsent( id ) )
          return { { u"requestId"_s, id }, { u"status"_s, u"CANCELLED"_s } }; // # spellok: API protocol status.
        auto grant = mGrants.value( id ).toObject();
        grant.insert( u"canceled"_s, true );
        mGrants.insert( id, grant );
        saveGrants();
        return { { u"requestId"_s, id }, { u"status"_s, u"cancellation_requested"_s } };
      }
      if ( mDownloads.value( id ) )
        mDownloads.value( id )->cancel();
      auto grant = mGrants.value( id ).toObject();
      grant.insert( u"canceled"_s, true );
      mGrants.insert( id, grant );
      saveGrants();
      QJsonObject body;
      mRouter->appendManagedToolContext( body );
      body.insert( u"workspaceId"_s, QgsAiWorkspaceTrust::workspaceHash( mWorkspaceRoot ) );
      const auto requestId = mClient->submit( u"/v1/discovery/%1/%2/cancel"_s.arg( kind, id ), body );
      return { { u"requestId"_s, requestId }, { u"status"_s, u"cancellation_requested"_s } };
    }
    const auto result = mClient->snapshot( id );
    if ( tool == "discovery_run"_L1 )
    {
      if ( ( result.value( u"status"_s ) == "SUCCEEDED"_L1 || result.value( u"status"_s ) == "PARTIAL"_L1 ) && result.contains( u"candidates"_s ) )
      {
        mShown.remove( id );
        showPreview( result );
      }
      else
        mClient->watch( u"plans"_s, id );
      return { { u"status"_s, u"awaiting_user_selection"_s }, { u"planId"_s, id } };
    }
    if ( kind != "resolve"_L1 && ( !result.contains( u"requestId"_s ) || result.contains( u"id"_s ) ) )
      mClient->watch( kind, result.value( u"id"_s ).toString( id ) );
    return result.isEmpty() ? QJsonObject { { u"id"_s, id }, { u"status"_s, u"pending"_s } } : result;
  }
  if ( tool == "discovery_search"_L1 )
  {
    const auto resolution = mClient->snapshot( args.value( u"resolutionId"_s ).toString() );
    if ( resolution.value( u"status"_s ) != "ready"_L1 )
      return error( tr( "Prima risolvere e verificare area e profilo con discovery_resolve." ) );
    const int credits = args.value( u"maxCredits"_s ).toInt( 20 );
    if ( credits < 0 || credits > 20 )
      return error( tr( "Budget ricerca consentito: 0–20 crediti." ) );
    args = { { u"brief"_s, resolution.value( u"brief"_s ) }, { u"maxCredits"_s, credits } };
    emit message( tr( "Ricerca delle fonti avviata: massimo %1 crediti. L'acquisizione richiede una selezione successiva." ).arg( credits ) );
  }
  else if ( tool == "discovery_resolve"_L1 )
  {
    if ( args.take( u"useMap"_s ).toBool() )
    {
      if ( !mCanvas || !mProject )
        return error( tr( "Mappa non disponibile." ) );
      try
      {
        QgsCoordinateTransform transform( mCanvas->mapSettings().destinationCrs(), QgsCoordinateReferenceSystem( u"EPSG:4326"_s ), mProject );
        const auto extent = transform.transformBoundingBox( mCanvas->extent() );
        if ( extent.isEmpty() || !extent.isFinite() )
          return error( tr( "Extent della mappa non valido." ) );
        args.insert( u"bbox"_s, QJsonArray { extent.xMinimum(), extent.yMinimum(), extent.xMaximum(), extent.yMaximum() } );
        args.insert( u"aoiSource"_s, u"map"_s );
      }
      catch ( const QgsCsException & )
      {
        return error( tr( "Impossibile trasformare l'area in EPSG:4326." ) ); // # spellok: Italian UI text.
      }
    }
  }
  else
    return error( tr( "Tool discovery sconosciuto." ) );
  mRouter->appendManagedToolContext( args );
  args.insert( u"workspaceId"_s, QgsAiWorkspaceTrust::workspaceHash( mWorkspaceRoot ) );
  const QString path = tool == "discovery_resolve"_L1 ? u"/v1/tools/discovery-resolve"_s : u"/v1/discovery/plans"_s;
  return { { u"requestId"_s, mClient->submit( path, args ) }, { u"status"_s, u"pending"_s } };
}
void QgsAiDiscoveryController::showPreview( const QJsonObject &plan )
{
  const QString id = plan.value( u"id"_s ).toString();
  if ( mShown.contains( id ) )
    return;
  mShown.insert( id );
  const QString currentScope = mScope;
  auto preview = new QgsAiDiscoveryPreview( plan, u"discovery/%1"_s.arg( id ) );
  connect( preview, &QgsAiDiscoveryPreview::approved, this, [this, plan, currentScope]( const QJsonArray &selection, int budget, const QString &destination ) {
    if ( scope() && currentScope == mScope )
      approve( plan, selection, budget, destination );
  } );
  connect( preview, &QgsAiDiscoveryPreview::canceled, this, [this, id, currentScope]() {
    if ( scope() && currentScope == mScope )
      execute( u"discovery_cancel"_s, { { u"id"_s, id }, { u"kind"_s, u"plans"_s } } );
  } );
  emit previewReady( preview );
}
void QgsAiDiscoveryController::approve( const QJsonObject &plan, const QJsonArray &selection, int maxCredits, const QString &destination )
{
  const QString root = mFiles->workspaceRoot();
  const QString path = mFiles->normalizePath( destination );
  if ( !QgsAiWorkspaceTrust::isTrusted( root ) || !QgsAiDiscoveryFiles::destinationError( root, path ).isEmpty() || QFileInfo::exists( path ) )
  {
    emit message( tr( "Destinazione non valida, esistente o workspace non autorizzato. Riaprire l'anteprima per correggere." ) );
    return;
  }
  QJsonArray approved;
  for ( const auto &item : selection )
    for ( const auto &value : plan.value( u"candidates"_s ).toArray() )
    {
      auto candidate = value.toObject();
      if ( candidate.value( u"id"_s ) == item.toObject().value( u"candidateId"_s ) )
      {
        candidate.insert( u"mode"_s, item.toObject().value( u"mode"_s ) );
        approved.append( candidate );
      }
    }
  if ( approved.size() != selection.size() || selection.isEmpty() )
    return;
  QJsonObject body { { u"planId"_s, plan.value( u"id"_s ) }, { u"planVersion"_s, plan.value( u"version"_s ) }, { u"selection"_s, selection }, { u"maxCredits"_s, maxCredits } };
  mRouter->appendManagedToolContext( body );
  body.insert( u"workspaceId"_s, QgsAiWorkspaceTrust::workspaceHash( mWorkspaceRoot ) );
  const auto key = mClient->submit( u"/v1/discovery/runs"_s, body );
  mGrants
    .insert( key, QJsonObject { { u"planId"_s, plan.value( u"id"_s ) }, { u"version"_s, plan.value( u"version"_s ) }, { u"selection"_s, selection }, { u"approved"_s, approved }, { u"maxCredits"_s, maxCredits }, { u"destination"_s, path }, { u"root"_s, root } } );
  saveGrants();
}
void QgsAiDiscoveryController::updated( const QString &kind, const QJsonObject &result )
{
  const QString id = result.value( u"id"_s ).toString();
  const QString state = result.value( u"status"_s ).toString();
  if ( kind == "resolve"_L1 )
  {
    const auto brief = result.value( u"brief"_s ).toObject();
    if ( state == "ready"_L1 )
      emit message( tr( "Area verificata: %1. La ricerca delle fonti può iniziare." ).arg( brief.value( u"comune"_s ).toString( tr( "extent selezionato" ) ) ) );
    else
      for ( const auto &question : result.value( u"questions"_s ).toArray() )
        emit message( question.toObject().value( u"prompt"_s ).toString() );
    return;
  }
  if ( kind == "review"_L1 )
    return;
  const QString pendingKey = result.value( u"requestId"_s ).toString();
  if ( pendingKey != id && !id.isEmpty() && mGrants.value( pendingKey ).toObject().value( u"canceled"_s ).toBool() )
  {
    mGrants.insert( id, mGrants.take( pendingKey ) );
    saveGrants();
    execute( u"discovery_cancel"_s, { { u"id"_s, id }, { u"kind"_s, kind } } );
    return;
  }
  if ( kind == "plans"_L1 && ( result.value( u"cancelRequested"_s ).toBool() || mGrants.value( id ).toObject().value( u"canceled"_s ).toBool() ) )
  {
    emit message( tr( "Ricerca annullata." ) );
    return;
  }
  if ( kind == "plans"_L1 && ( state == "SUCCEEDED"_L1 || state == "PARTIAL"_L1 ) && result.contains( u"candidates"_s ) )
    showPreview( result );
  if ( kind == "runs"_L1 )
  {
    if ( !mProgress.value( id ) && !id.isEmpty() )
    {
      auto controls = new QWidget;
      auto layout = new QVBoxLayout( controls );
      auto progress = new QLabel( controls );
      progress->setTextFormat( Qt::PlainText );
      progress->setWordWrap( true );
      layout->addWidget( progress );
      auto cancel = new QPushButton( tr( "Annulla acquisizione / importazione" ), controls );
      layout->addWidget( cancel );
      mProgress.insert( id, progress );
      mCancelButtons.insert( id, cancel );
      const QString currentScope = mScope;
      connect( cancel, &QPushButton::clicked, this, [this, id, currentScope, cancel]() {
        if ( scope() && currentScope == mScope )
          execute( u"discovery_cancel"_s, { { u"id"_s, id }, { u"kind"_s, u"runs"_s } } );
        cancel->setEnabled( false );
      } );
      emit controlsReady( controls );
    }
    const auto progress = result.value( u"progress"_s ).toObject();
    if ( mProgress.value( id ) )
      mProgress.value( id )->setText(
        tr( "%1 · %2 · %3/%4 candidati · %5 crediti" )
          .arg( id, state )
          .arg( progress.value( u"completed"_s ).toInt() )
          .arg( progress.value( u"total"_s ).toInt() )
          .arg( result.value( u"budget"_s ).toObject().value( u"creditsUsed"_s ).toInt() )
        + '\n'
        + QgsAiDiscoveryPresentation::runResults( result )
      );

    const QString key = result.value( u"requestId"_s ).toString();
    if ( mGrants.contains( key ) && !mGrants.contains( id ) )
    {
      mGrants.insert( id, mGrants.take( key ) );
      saveGrants();
    }
    if ( result.value( u"artifact"_s ).isObject() )
      deliver( result );
  }
  const QString summary = tr( "Discovery %1 · %2 · %3" ).arg( id, state, result.value( u"phase"_s ).toString() );
  if ( mLastStates.value( id ) != summary )
  {
    mLastStates.insert( id, summary );
    emit message( summary );
  }
}
