// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIDISCOVERYCLIENT_H
#define QGSAIDISCOVERYCLIENT_H
#include <functional>

#include "qgis_app.h"

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>

//! Non-blocking discovery transport. Persisted requests contain no credentials.
class APP_EXPORT QgsAiDiscoveryClient : public QObject
{
    Q_OBJECT
  public:
    using Auth = std::function<bool( QNetworkRequest & )>;
    QgsAiDiscoveryClient( QNetworkAccessManager *network, Auth auth, QObject *parent = nullptr );
    void setScope( const QUrl &base, const QString &settingsKey );
    QString submit( const QString &path, const QJsonObject &body );
    bool discardUnsent( const QString &id );
    void watch( const QString &kind, const QString &id );
    QJsonObject snapshot( const QString &id ) const;
    void resume();
    void pause();
  signals:
    void updated( const QString &kind, const QJsonObject &resource );
    void failed( const QString &requestId, const QString &message );

  private:
    void request( const QString &key, const QString &path, const QJsonObject &body, bool post );
    void save();
    void poll();
    QNetworkAccessManager *mNetwork;
    Auth mAuth;
    QUrl mBase;
    QString mScope;
    QJsonObject mPending, mWatched, mSnapshots;
    QSet<QString> mInFlight;
    QMap<QString, int> mFailures;
    QTimer mTimer;
    int mGeneration = 0;
};
#endif
