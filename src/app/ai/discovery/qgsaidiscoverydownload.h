// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIDISCOVERYDOWNLOAD_H
#define QGSAIDISCOVERYDOWNLOAD_H
#include "qgis_app.h"

#include <QNetworkAccessManager>
#include <QPointer>

class APP_EXPORT QgsAiDiscoveryDownload : public QObject
{
    Q_OBJECT
  public:
    explicit QgsAiDiscoveryDownload( QNetworkAccessManager *network, QObject *parent = nullptr );
    ~QgsAiDiscoveryDownload() override;
    void start( const QUrl &url, const QString &root, const QString &path, const QString &sha256, qint64 bytes );
    void cancel();
  signals:
    void finished( const QString &path, const QString &error );
    void progress( qint64 received, qint64 total );

  private:
    QNetworkAccessManager *mNetwork;
    QPointer<QNetworkReply> mReply;
};
#endif
