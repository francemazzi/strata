#ifndef QGSAIREMOTEDESKTOPBRIDGE_H
#define QGSAIREMOTEDESKTOPBRIDGE_H

#include "qgis_app.h"

#include <QByteArray>
#include <QObject>

class QLocalServer;
class QLocalSocket;
class QgsAiToolRegistry;
class QgsProject;

class APP_EXPORT QgsAiRemoteDesktopBridge : public QObject
{
    Q_OBJECT
  public:
    explicit QgsAiRemoteDesktopBridge( QgsProject *project, QgsAiToolRegistry *tools, QObject *parent = nullptr );
    void start();
    bool isListening() const;

  signals:
    void taskStarted( const QString &taskId, const QString &capability );
    void taskFinished( const QString &taskId, bool success );

  private:
    void listenWithCredential( const QString &credential );
    void acceptConnection();
    void readSocket( QLocalSocket *socket );
    void handleMessage( QLocalSocket *socket, const QJsonObject &message );
    QJsonObject executeReadOnly( const QJsonObject &message ) const;

    QgsProject *mProject = nullptr;
    QgsAiToolRegistry *mTools = nullptr;
    QLocalServer *mServer = nullptr;
    QString mCredential;
};

#endif
