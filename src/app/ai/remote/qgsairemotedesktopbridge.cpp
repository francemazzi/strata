#include "qgsairemotedesktopbridge.h"

#include "qgsaitool.h"
#include "qgsaitoolregistry.h"
#include "qgsproject.h"
#include "strataremoteprotocol.h"

#include <QJsonObject>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <qt6keychain/keychain.h>

namespace
{
  constexpr auto KeychainService = "Strata Remote Runner";
  constexpr auto CredentialKey = "runner-credential";
}

QgsAiRemoteDesktopBridge::QgsAiRemoteDesktopBridge( QgsProject *project, QgsAiToolRegistry *tools, QObject *parent )
  : QObject( parent )
  , mProject( project )
  , mTools( tools )
{
}

void QgsAiRemoteDesktopBridge::start()
{
  auto *job = new QKeychain::ReadPasswordJob( QString::fromLatin1( KeychainService ), this );
  job->setKey( QString::fromLatin1( CredentialKey ) );
  connect( job, &QKeychain::Job::finished, this, [this]( QKeychain::Job *finished ) {
    const auto *read = static_cast<QKeychain::ReadPasswordJob *>( finished );
    if ( finished->error() == QKeychain::NoError && !read->textData().isEmpty() )
      listenWithCredential( read->textData() );
  } );
  job->start();
}

bool QgsAiRemoteDesktopBridge::isListening() const
{
  return mServer && mServer->isListening();
}

void QgsAiRemoteDesktopBridge::listenWithCredential( const QString &credential )
{
  mCredential = credential;
  mServer = new QLocalServer( this );
  mServer->setSocketOptions( QLocalServer::UserAccessOption );
  const QString name = StrataRemoteProtocol::socketName();
  QLocalServer::removeServer( name );
  if ( !mServer->listen( name ) )
    return;
  connect( mServer, &QLocalServer::newConnection, this, &QgsAiRemoteDesktopBridge::acceptConnection );
}

void QgsAiRemoteDesktopBridge::acceptConnection()
{
  while ( QLocalSocket *socket = mServer->nextPendingConnection() )
  {
    connect( socket, &QLocalSocket::readyRead, this, [this, socket]() { readSocket( socket ); } );
    connect( socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater );
  }
}

void QgsAiRemoteDesktopBridge::readSocket( QLocalSocket *socket )
{
  QByteArray buffer = socket->property( "strataBuffer" ).toByteArray() + socket->readAll();
  QJsonObject message;
  QString error;
  if ( StrataRemoteProtocol::takeFrame( buffer, message, error ) )
    handleMessage( socket, message );
  else if ( !error.isEmpty() )
  {
    socket->write( StrataRemoteProtocol::encode( { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), error } } ) );
    socket->disconnectFromServer();
  }
  socket->setProperty( "strataBuffer", buffer );
}

void QgsAiRemoteDesktopBridge::handleMessage( QLocalSocket *socket, const QJsonObject &message )
{
  if ( message.value( QStringLiteral( "auth" ) ).toString() != mCredential )
  {
    socket->write( StrataRemoteProtocol::encode( { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), QStringLiteral( "IPC authentication failed." ) } } ) );
    socket->disconnectFromServer();
    return;
  }
  const QString taskId = message.value( QStringLiteral( "id" ) ).toString();
  const QString capability = message.value( QStringLiteral( "capability" ) ).toString();
  emit taskStarted( taskId, capability );
  const QJsonObject result = executeReadOnly( message );
  socket->write( StrataRemoteProtocol::encode( result ) );
  socket->flush();
  emit taskFinished( taskId, result.value( QStringLiteral( "success" ) ).toBool() );
  socket->disconnectFromServer();
}

QJsonObject QgsAiRemoteDesktopBridge::executeReadOnly( const QJsonObject &message ) const
{
  if ( message.value( QStringLiteral( "type" ) ).toString() != QStringLiteral( "execute" ) )
    return { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), QStringLiteral( "Unsupported IPC message." ) } };
  if ( message.value( QStringLiteral( "capability" ) ).toString() != QStringLiteral( "project.inspect" ) )
    return { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), QStringLiteral( "Capability is not authorized by this Desktop bridge." ) } };
  if ( !mProject || !mTools )
    return { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), QStringLiteral( "STRATA project tools are unavailable." ) } };
  const QString expected = message.value( QStringLiteral( "projectFingerprint" ) ).toString();
  const QString actual = StrataRemoteProtocol::projectFingerprint( mProject->fileName() );
  if ( expected.isEmpty() || actual.isEmpty() || expected != actual )
    return { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), QStringLiteral( "Project fingerprint mismatch; task rejected." ) }, { QStringLiteral( "projectFingerprint" ), actual } };

  QgsAiTool *tool = mTools->find( QStringLiteral( "list_project_layers" ) );
  if ( !tool || tool->requiresApproval() || tool->riskLevel() != QgsAiToolRiskLevel::Low )
    return { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), QStringLiteral( "Read-only project inspection tool is unavailable." ) } };
  const QgsAiToolResult toolResult = mTools->execute( tool->name(), {} );
  if ( !toolResult.success )
    return { { QStringLiteral( "success" ), false }, { QStringLiteral( "error" ), toolResult.errorMessage } };
  return {
    { QStringLiteral( "success" ), true },
    { QStringLiteral( "projectFingerprint" ), actual },
    { QStringLiteral( "projectName" ), QFileInfo( mProject->fileName() ).fileName() },
    { QStringLiteral( "projectCrs" ), mProject->crs().authid() },
    { QStringLiteral( "inspection" ), toolResult.output }
  };
}
