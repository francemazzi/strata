// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaidiscoverydownload.h"

#include <memory>

#include "qgsaidiscoveryfiles.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QSaveFile>
#include <QTimer>

#include "moc_qgsaidiscoverydownload.cpp"

QgsAiDiscoveryDownload::QgsAiDiscoveryDownload( QNetworkAccessManager *network, QObject *parent )
  : QObject( parent )
  , mNetwork( network )
{}
QgsAiDiscoveryDownload::~QgsAiDiscoveryDownload()
{
  if ( mReply )
  {
    mReply->disconnect( this );
    mReply->abort();
    mReply->deleteLater();
  }
}
void QgsAiDiscoveryDownload::cancel()
{
  if ( mReply )
    mReply->abort();
}
void QgsAiDiscoveryDownload::start( const QUrl &url, const QString &root, const QString &path, const QString &sha256, qint64 bytes )
{
  using namespace QgsAiDiscoveryFiles;
  const QString validation = destinationError( root, path );
  if ( mReply || !validation.isEmpty() || !safeUrl( url ) || !validDigest( sha256 ) || bytes < 1 || bytes > MaxKitBytes || QFileInfo::exists( path ) )
  {
    emit finished( {}, validation.isEmpty() ? tr( "Download non autorizzato o destinazione già esistente." ) : validation );
    return;
  }
  if ( !QDir().mkpath( QFileInfo( path ).absolutePath() ) )
  {
    emit finished( {}, tr( "Cartella non scrivibile." ) );
    return;
  }
  auto file = std::make_shared<QSaveFile>( path );
  if ( !file->open( QIODevice::WriteOnly ) )
  {
    emit finished( {}, file->errorString() );
    return;
  }
  QNetworkRequest req( url );
  req.setTransferTimeout( 30000 );
  // Signed artifact URL: no credentials and no redirects to a different destination.
  req.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy );
  auto reply = mNetwork->get( req );
  mReply = reply;
  reply->setReadBufferSize( 262144 );
  auto received = std::make_shared<qint64>( 0 );
  auto hash = std::make_shared<QCryptographicHash>( QCryptographicHash::Sha256 );
  auto drain = [reply, file, hash, received, bytes]() {
    while ( reply->bytesAvailable() )
    {
      const QByteArray chunk = reply->read( 65536 );
      *received += chunk.size();
      if ( *received > bytes || file->write( chunk ) != chunk.size() )
      {
        reply->abort();
        return;
      }
      hash->addData( chunk );
    }
  };
  connect( reply, &QIODevice::readyRead, this, drain );
  connect( reply, &QNetworkReply::downloadProgress, this, &QgsAiDiscoveryDownload::progress );
  QTimer::singleShot( 1800000, reply, [reply]() {
    if ( reply->isRunning() )
      reply->abort();
  } );
  connect( reply, &QNetworkReply::finished, this, [this, reply, file, hash, received, bytes, sha256, path, drain]() {
    drain();
    mReply = nullptr;
    reply->deleteLater();
    const auto status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    if ( reply->error() != QNetworkReply::NoError || status != 200 || *received != bytes || hash->result().toHex() != sha256.toLatin1().toLower() )
    {
      file->cancelWriting();
      emit finished( {}, tr( "Download interrotto, dimensione o hash non validi." ) );
      return;
    }
    if ( !file->commit() )
    {
      emit finished( {}, file->errorString() );
      return;
    }
    emit finished( path, {} );
  } );
}
