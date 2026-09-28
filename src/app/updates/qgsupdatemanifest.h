#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QJsonObject>
#include "qgis_app.h"
struct QgsUpdatePackage
{
    QString version, platform, architecture, format, sha256;
    QUrl url;
    qint64 size = 0;
    QJsonObject manifest;
};
namespace QgsUpdateManifest
{
  APP_EXPORT bool verify( const QByteArray &manifest, const QByteArray &signature, QString *error );
  APP_EXPORT bool select( const QByteArray &manifest, const QString &current, const QString &platform, const QString &architecture, QgsUpdatePackage &package, QString *error );
  APP_EXPORT bool verifyFile( const QString &path, const QgsUpdatePackage &package, QString *error );
  APP_EXPORT bool isReleaseUrl( const QUrl &url );
  APP_EXPORT QString platform();
} //namespace QgsUpdateManifest
