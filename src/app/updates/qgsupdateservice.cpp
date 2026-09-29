#include "qgsupdateservice.h"
#include "qgsnetworkaccessmanager.h"
#include "qgssettings.h"
#include "qgsconfig.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThreadPool>
#include <QSysInfo>
#include <QStorageInfo>
#include "moc_qgsupdateservice.cpp"
using namespace Qt::StringLiterals;
QgsUpdateService::QgsUpdateService( QObject *parent )
  : QObject( parent )
{}
QgsUpdateService *QgsUpdateService::instance()
{
  static QgsUpdateService *service = new QgsUpdateService( QCoreApplication::instance() );
  return service;
}
void QgsUpdateService::setState( const QString &state, const QString &status )
{
  mState = state;
  mStatus = status;
  emit changed();
}
void QgsUpdateService::fetch( const QUrl &url, std::function<void( QByteArray )> callback )
{
  QNetworkRequest request( url );
  request.setTransferTimeout( 30000 );
  request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy );
  request.setRawHeader( "User-Agent", QByteArray( "Strata/" ) + STRATA_VERSION );
  mReply = QgsNetworkAccessManager::instance()->get( request );
  auto *reply = mReply.data();
  connect( reply, &QNetworkReply::readyRead, this, [reply]() {
    if ( reply->bytesAvailable() > 1024 * 1024 )
      reply->abort();
  } );
  connect( reply, &QNetworkReply::finished, this, [this, reply, callback]() {
    reply->deleteLater();
    if ( mReply != reply )
      return;
    mReply.clear();
    if ( reply->error() != QNetworkReply::NoError )
    {
      setState( u"error"_s, tr( "Update check failed: %1" ).arg( reply->errorString() ) );
      return;
    }
    callback( reply->readAll() );
  } );
}
void QgsUpdateService::check( bool automatic )
{
  if ( mReply || mState == "verifying"_L1 || mState == "ready"_L1 )
    return;
  QgsSettings settings;
  const qint64 now = QDateTime::currentSecsSinceEpoch();
  if ( automatic && ( !settings.value( u"strata/updates/checkAutomatically"_s, true ).toBool() || now - settings.value( u"strata/updates/lastCheck"_s, 0 ).toLongLong() < 86400 ) )
    return;
  settings.setValue( u"strata/updates/lastCheck"_s, now );
  setState( u"checking"_s, tr( "Checking for updates…" ) );
  QUrl feed( u"https://api.github.com/repos/francemazzi/strata/releases/latest"_s );
#ifdef STRATA_ENABLE_UPDATE_TEST_FEED
  const QUrl testFeed( qEnvironmentVariable( "STRATA_UPDATE_TEST_FEED" ) );
  if ( testFeed.scheme() == "http"_L1 && testFeed.host() == "127.0.0.1"_L1 )
    feed = testFeed;
#endif
  fetch( feed, [this]( const QByteArray &data ) { readRelease( data ); } );
}
void QgsUpdateService::readRelease( const QByteArray &data )
{
  const QJsonObject release = QJsonDocument::fromJson( data ).object();
  if ( release.isEmpty() || release.value( u"draft"_s ).toBool() || release.value( u"prerelease"_s ).toBool() )
  {
    setState( u"error"_s, tr( "No published stable release is available." ) );
    return;
  }
  const QString tag = release.value( u"tag_name"_s ).toString();
  if ( tag == u"strata-v%1"_s.arg( QString::fromUtf8( STRATA_VERSION ) ) )
  {
    setState( u"current"_s, tr( "Strata is up to date." ) );
    return;
  }
  QUrl manifest, signature;
  for ( const auto &value : release.value( u"assets"_s ).toArray() )
  {
    const auto asset = value.toObject();
    if ( asset.value( u"name"_s ).toString() == "update-manifest.json"_L1 )
      manifest = QUrl( asset.value( u"browser_download_url"_s ).toString() );
    if ( asset.value( u"name"_s ).toString() == "update-manifest.sig"_L1 )
      signature = QUrl( asset.value( u"browser_download_url"_s ).toString() );
  }
  if ( !QgsUpdateManifest::isReleaseUrl( manifest ) || !QgsUpdateManifest::isReleaseUrl( signature ) )
  {
    setState( u"error"_s, tr( "This release has no verified in-app update metadata." ) );
    return;
  }
  mNotes = release.value( u"body"_s ).toString();
  fetch( manifest, [this, signature, tag]( const QByteArray &data ) {
    mManifest = data;
    fetch( signature, [this, tag]( const QByteArray &data ) {
      mSignature = data;
      QString error;
      if (
        !QgsUpdateManifest::verify( mManifest, mSignature, &error )
        || !QgsUpdateManifest::select( mManifest, QString::fromUtf8( STRATA_VERSION ), QgsUpdateManifest::platform(), QSysInfo::buildCpuArchitecture(), mPackage, &error )
      )
      {
        setState( u"error"_s, error );
        return;
      }
      if ( tag != u"strata-v%1"_s.arg( mPackage.version ) )
      {
        setState( u"error"_s, tr( "Update version does not match its release." ) );
        return;
      }
      setState( u"available"_s, tr( "Strata %1 is available." ).arg( mPackage.version ) );
    } );
  } );
}
void QgsUpdateService::download()
{
  if ( mState != "available"_L1 || !mDirectory.isValid() )
    return;
  if ( QStorageInfo( mDirectory.path() ).bytesAvailable() < mPackage.size * 3 )
  {
    setState( u"error"_s, tr( "Not enough free space to stage the update." ) );
    return;
  }
  mPackagePath = mDirectory.filePath( u"package.%1"_s.arg( mPackage.format ) );
  mDownload.setFileName( mPackagePath );
  if ( !mDownload.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
  {
    setState( u"error"_s, mDownload.errorString() );
    return;
  }
  setState( u"downloading"_s, tr( "Downloading Strata %1…" ).arg( mPackage.version ) );
  QNetworkRequest request( mPackage.url );
  request.setTransferTimeout( 60000 );
  request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy );
  mReply = QgsNetworkAccessManager::instance()->get( request );
  auto *reply = mReply.data();
  connect( reply, &QNetworkReply::downloadProgress, this, &QgsUpdateService::progress );
  connect( reply, &QNetworkReply::readyRead, this, [this, reply]() {
    const QByteArray chunk = reply->readAll();
    if ( mDownload.size() + chunk.size() > mPackage.size || mDownload.write( chunk ) != chunk.size() )
      reply->abort();
  } );
  connect( reply, &QNetworkReply::finished, this, [this, reply]() {
    reply->deleteLater();
    if ( mReply != reply )
      return;
    mReply.clear();
    mDownload.close();
    if ( reply->error() != QNetworkReply::NoError )
    {
      QFile::remove( mPackagePath );
      setState( u"error"_s, tr( "Download failed: %1" ).arg( reply->errorString() ) );
      return;
    }
    setState( u"verifying"_s, tr( "Verifying update…" ) );
    const auto package = mPackage;
    const QString path = mPackagePath;
    const QPointer<QgsUpdateService> self( this );
    QThreadPool::globalInstance()->start( [self, package, path]() {
      QString error;
      const bool ok = QgsUpdateManifest::verifyFile( path, package, &error );
      if ( !self )
        return;
      QMetaObject::invokeMethod(
        self,
        [self, ok, error]() {
          if ( self )
            self->setState( ok ? u"ready"_s : u"error"_s, ok ? tr( "Update verified. Save your work, then install and restart." ) : error );
        },
        Qt::QueuedConnection
      );
    } );
  } );
}
void QgsUpdateService::cancel()
{
  if ( !mReply )
    return;
  auto reply = mReply;
  mReply.clear();
  reply->abort();
  mDownload.close();
  QFile::remove( mPackagePath );
  setState( u"idle"_s, tr( "Update cancelled. Your installation was not changed." ) );
}
