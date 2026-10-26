#ifndef STRATAREMOTECLIENT_H
#define STRATAREMOTECLIENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

class QLocalSocket;
class QNetworkReply;

class StrataRemoteClient : public QObject
{
    Q_OBJECT
  public:
    explicit StrataRemoteClient( QObject *parent = nullptr );
    void pair( const QUrl &apiUrl, const QString &code );
    void start();

  private:
    void readCredential( std::function<void( const QString & )> callback );
    void writeCredential( const QString &credential, std::function<void( bool )> callback );
    void sendJson( const QString &path, const QJsonObject &body, std::function<void( int, const QJsonObject & )> callback );
    void heartbeat();
    void claim();
    void executeClaim( const QJsonObject &task );
    void reportResult( const QJsonObject &task, const QJsonObject &bridgeResult );

    QNetworkAccessManager mNetwork;
    QTimer mHeartbeat;
    QTimer mClaim;
    QUrl mApiUrl;
    QString mInstallationId;
    QString mCredential;
    bool mBusy = false;
};

#endif
