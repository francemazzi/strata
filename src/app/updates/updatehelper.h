#pragma once
#include <QString>
#include <QJsonObject>
namespace UpdateHelper
{
  bool copyTree( const QString &source, const QString &target, QString *error, bool overwrite = false );
  bool extractZip( const QString &source, const QString &target, QString *error );
  bool verifyNative( const QString &candidate, const QString &installed, QString *error );
  bool parentAlive( qint64 pid );
  bool install( const QJsonObject &job, const QString &jobPath, QString *error );
} //namespace UpdateHelper
