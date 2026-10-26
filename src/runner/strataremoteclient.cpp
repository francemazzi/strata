#include "strataremoteclient.h"
#include "strataremoteprotocol.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSysInfo>
#include <QUuid>
#include <qt6keychain/keychain.h>

namespace
{
  constexpr auto KeychainService = "Strata Remote Runner";
  constexpr auto CredentialKey = "runner-credential";
}

StrataRemoteClient::StrataRemoteClient( QObject *parent )
  : QObject( parent )
{
  mHeartbeat.setInterval( 20000 );
  mClaim.setInterval( 3000 );
  connect( &mHeartbeat, &QTimer::timeout, this, &StrataRemoteClient::heartbeat );
  connect( &mClaim, &QTimer::timeout, this, &StrataRemoteClient::claim );
}

void StrataRemoteClient::pair( const QUrl &apiUrl, const QString &code )
{
  mApiUrl = apiUrl;
  sendJson( QStringLiteral( "/v1/remote-runner/pair" ), {
    { QStringLiteral( "code" ), code },
    { QStringLiteral( "platform" ), QSysInfo::productType() },
    { QStringLiteral( "version" ), QCoreApplication::applicationVersion() },
    { QStringLiteral( "machineName" ), QSysInfo::machineHostName() },
    { QStringLiteral( "capabilities" ), QJsonArray { QJsonObject { { QStringLiteral( "name" ), QStringLiteral( "project.inspect" ) } } } }
  }, [this]( int status, const QJsonObject &response ) {
    if ( status != 200 || response.value( QStringLiteral( "credential" ) ).toString().isEmpty() )
    {
      qCritical( "Pairing failed (HTTP %d).", status );
      QCoreApplication::exit( 2 );
      return;
    }
    mInstallationId = response.value( QStringLiteral( "installationId" ) ).toString();
    QSettings settings;
    settings.setValue( QStringLiteral( "remoteRunner/apiUrl" ), mApiUrl.toString() );
    settings.setValue( QStringLiteral( "remoteRunner/installationId" ), mInstallationId );
    writeCredential( response.value( QStringLiteral( "credential" ) ).toString(), []( bool ok ) {
      qInfo( "%s", ok ? "Runner paired. Credential stored in the OS keychain." : "Pairing succeeded, but keychain persistence failed." );
      QCoreApplication::exit( ok ? 0 : 3 );
    } );
  } );
}

void StrataRemoteClient::start()
{
  QSettings settings;
  mApiUrl = QUrl( settings.value( QStringLiteral( "remoteRunner/apiUrl" ) ).toString() );
  mInstallationId = settings.value( QStringLiteral( "remoteRunner/installationId" ) ).toString();
  const bool loopback = mApiUrl.host() == QStringLiteral( "127.0.0.1" ) || mApiUrl.host() == QStringLiteral( "localhost" );
  if ( !mApiUrl.isValid() || ( mApiUrl.scheme() != QStringLiteral( "https" ) && !loopback ) || mInstallationId.isEmpty() )
  {
    qCritical( "Runner is not paired, or its endpoint is not HTTPS." );
    QCoreApplication::exit( 2 );
    return;
  }
  readCredential( [this]( const QString &credential ) {
    if ( credential.isEmpty() )
    {
      qCritical( "Runner credential is missing from the OS keychain." );
      QCoreApplication::exit( 2 );
      return;
    }
    mCredential = credential;
    heartbeat();
    claim();
    mHeartbeat.start();
    mClaim.start();
  } );
}

void StrataRemoteClient::readCredential( std::function<void( const QString & )> callback )
{
  auto *job = new QKeychain::ReadPasswordJob( QString::fromLatin1( KeychainService ), this );
  job->setKey( QString::fromLatin1( CredentialKey ) );
  connect( job, &QKeychain::Job::finished, this, [callback = std::move( callback )]( QKeychain::Job *finished ) {
    const auto *read = static_cast<QKeychain::ReadPasswordJob *>( finished );
    callback( finished->error() == QKeychain::NoError ? read->textData() : QString() );
  } );
  job->start();
}

void StrataRemoteClient::writeCredential( const QString &credential, std::function<void( bool )> callback )
{
  auto *job = new QKeychain::WritePasswordJob( QString::fromLatin1( KeychainService ), this );
  job->setKey( QString::fromLatin1( CredentialKey ) );
  job->setTextData( credential );
  connect( job, &QKeychain::Job::finished, this, [callback = std::move( callback )]( QKeychain::Job *finished ) {
    callback( finished->error() == QKeychain::NoError );
  } );
  job->start();
}

void StrataRemoteClient::sendJson( const QString &path, const QJsonObject &body, std::function<void( int, const QJsonObject & )> callback )
{
  const QUrl target = mApiUrl.resolved( QUrl( path ) );
  if ( target.scheme() != QStringLiteral( "https" ) && target.host() != QStringLiteral( "127.0.0.1" ) && target.host() != QStringLiteral( "localhost" ) )
  {
    callback( 0, {} );
    return;
  }
  QNetworkRequest request( target );
  request.setHeader( QNetworkRequest::ContentTypeHeader, QStringLiteral( "application/json" ) );
  if ( !mCredential.isEmpty() )
    request.setRawHeader( "Authorization", QByteArray( "Bearer " ) + mCredential.toUtf8() );
  QNetworkReply *reply = mNetwork.post( request, QJsonDocument( body ).toJson( QJsonDocument::Compact ) );
  connect( reply, &QNetworkReply::finished, this, [reply, callback = std::move( callback )]() {
    const int status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    const QJsonObject response = QJsonDocument::fromJson( reply->readAll() ).object();
    reply->deleteLater();
    callback( status, response );
  } );
}

void StrataRemoteClient::heartbeat()
{
  sendJson( QStringLiteral( "/v1/remote-runner/heartbeat" ), {
    { QStringLiteral( "installationId" ), mInstallationId },
    { QStringLiteral( "capabilities" ), QJsonArray { QJsonObject { { QStringLiteral( "name" ), QStringLiteral( "project.inspect" ) } } } }
  }, []( int, const QJsonObject & ) {} );
}

void StrataRemoteClient::claim()
{
  if ( mBusy )
    return;
  mBusy = true;
  sendJson( QStringLiteral( "/v1/remote-runner/tasks/claim" ), { { QStringLiteral( "installationId" ), mInstallationId } },
            [this]( int status, const QJsonObject &response ) {
    const QJsonValue task = response.value( QStringLiteral( "task" ) );
    if ( status == 200 && task.isObject() )
      executeClaim( task.toObject() );
    else
      mBusy = false;
  } );
}

void StrataRemoteClient::executeClaim( const QJsonObject &task )
{
  auto *socket = new QLocalSocket( this );
  connect( socket, &QLocalSocket::connected, this, [socket, task]() {
    QJsonObject request = task;
    request.insert( QStringLiteral( "type" ), QStringLiteral( "execute" ) );
    request.insert( QStringLiteral( "auth" ), socket->property( "auth" ).toString() );
    socket->write( StrataRemoteProtocol::encode( request ) );
  } );
  connect( socket, &QLocalSocket::readyRead, this, [this, socket, task]() {
    QByteArray buffer = socket->property( "buffer" ).toByteArray() + socket->readAll();
    QJsonObject result;
    QString error;
    if ( StrataRemoteProtocol::takeFrame( buffer, result, error ) )
    {
      socket->setProperty( "completed", true );
      reportResult( task, result );
      socket->disconnectFromServer();
    }
    socket->setProperty( "buffer", buffer );
  } );
  connect( socket, &QLocalSocket::errorOccurred, this, [this, socket, task]( QLocalSocket::LocalSocketError ) {
    if ( socket->property( "completed" ).toBool() )
      return;
    socket->setProperty( "completed", true );
    reportResult( task, { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), socket->errorString() } } );
  } );
  socket->setProperty( "auth", mCredential );
  socket->connectToServer( StrataRemoteProtocol::socketName(), QIODevice::ReadWrite );
  QTimer::singleShot( 30000, socket, [this, socket, task]() {
    if ( socket->property( "completed" ).toBool() )
      return;
    socket->setProperty( "completed", true );
    socket->abort();
    reportResult( task, { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), QStringLiteral( "Desktop bridge timed out." ) } } );
  } );
}

void StrataRemoteClient::reportResult( const QJsonObject &task, const QJsonObject &bridgeResult )
{
  const QString taskId = task.value( QStringLiteral( "id" ) ).toString();
  sendJson( QStringLiteral( "/v1/remote-runner/tasks/%1/result" ).arg( taskId ), {
    { QStringLiteral( "installationId" ), mInstallationId },
    { QStringLiteral( "leaseToken" ), task.value( QStringLiteral( "leaseToken" ) ).toString() },
    { QStringLiteral( "requestId" ), QUuid::createUuid().toString( QUuid::WithoutBraces ) },
    { QStringLiteral( "status" ), bridgeResult.value( QStringLiteral( "success" ) ).toBool() ? QStringLiteral( "SUCCEEDED" ) : QStringLiteral( "FAILED" ) },
    { QStringLiteral( "result" ), bridgeResult },
    { QStringLiteral( "errorCode" ), bridgeResult.value( QStringLiteral( "success" ) ).toBool() ? QString() : QStringLiteral( "desktop_bridge_failed" ) }
  }, [this]( int, const QJsonObject & ) { mBusy = false; } );
}
