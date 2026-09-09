// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIDISCOVERYFILES_H
#define QGSAIDISCOVERYFILES_H
#include "qgis_app.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

namespace QgsAiDiscoveryFiles
{
  constexpr qint64 MaxKitBytes = 2147483648LL;
  APP_EXPORT bool validDigest( const QString &digest );
  APP_EXPORT bool safeUrl( const QUrl &url );
  APP_EXPORT bool safeName( const QString &name );
  //! Reused by both download_file and the approved discovery downloader; refuses symlink ancestors.
  APP_EXPORT QString destinationError( const QString &root, const QString &destination );
  struct Result
  {
      QJsonObject manifest;
      QString error;
  };
  //! Stream archive contents to a new directory, verifying every entry against the manifest.
  APP_EXPORT Result unpack( const QString &zipPath, const QString &directory, const QString &expectedHash, const QJsonArray &approved );
} //namespace QgsAiDiscoveryFiles
#endif
