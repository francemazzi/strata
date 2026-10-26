#include "strataremoteprotocol.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

QString StrataRemoteProtocol::socketName()
{
  const QByteArray identity = QStandardPaths::writableLocation( QStandardPaths::HomeLocation ).toUtf8();
  return QStringLiteral( "strata-remote-%1" ).arg( QString::fromLatin1( QCryptographicHash::hash( identity, QCryptographicHash::Sha256 ).toHex().left( 16 ) ) );
}

QByteArray StrataRemoteProtocol::encode( const QJsonObject &message )
{
  QByteArray frame = QJsonDocument( message ).toJson( QJsonDocument::Compact );
  frame.append( '\n' );
  return frame;
}

bool StrataRemoteProtocol::takeFrame( QByteArray &buffer, QJsonObject &message, QString &error )
{
  if ( buffer.size() > MaxFrameBytes )
  {
    error = QStringLiteral( "IPC frame exceeds the 1 MiB limit." );
    buffer.clear();
    return false;
  }
  const qsizetype end = buffer.indexOf( '\n' );
  if ( end < 0 )
    return false;
  const QByteArray frame = buffer.left( end );
  buffer.remove( 0, end + 1 );
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson( frame, &parseError );
  if ( parseError.error != QJsonParseError::NoError || !document.isObject() )
  {
    error = QStringLiteral( "Invalid IPC JSON frame." );
    return false;
  }
  message = document.object();
  return true;
}

QString StrataRemoteProtocol::projectFingerprint( const QString &projectPath )
{
  const QFileInfo info( projectPath );
  if ( !info.exists() || !info.isFile() )
    return QString();
  const QByteArray material = info.canonicalFilePath().toUtf8() + '\0'
                              + QByteArray::number( info.size() ) + '\0'
                              + QByteArray::number( info.lastModified().toMSecsSinceEpoch() );
  return QString::fromLatin1( QCryptographicHash::hash( material, QCryptographicHash::Sha256 ).toHex() );
}
