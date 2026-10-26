#ifndef STRATAREMOTEPROTOCOL_H
#define STRATAREMOTEPROTOCOL_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace StrataRemoteProtocol
{
  constexpr int MaxFrameBytes = 1024 * 1024;
  QString socketName();
  QByteArray encode( const QJsonObject &message );
  bool takeFrame( QByteArray &buffer, QJsonObject &message, QString &error );
  QString projectFingerprint( const QString &projectPath );
}

#endif
