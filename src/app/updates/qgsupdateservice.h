#pragma once
#include "qgis_app.h"
#include "qgsupdatemanifest.h"
#include <QObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QFile>
#include <QNetworkReply>
#include <functional>

class APP_EXPORT QgsUpdateService : public QObject
{
    Q_OBJECT
  public:
    static QgsUpdateService *instance();
    QString status() const { return mStatus; }
    QString state() const { return mState; }
    QString version() const { return mPackage.version; }
    QString notes() const { return mNotes; }
    qint64 size() const { return mPackage.size; }
    void check( bool automatic = false );
    void download();
    void cancel();
    bool prepareInstall( QString *error );
    bool launchInstaller( QString *error );
    void acknowledgeLaunch();
  signals:
    void changed();
    void progress( qint64 received, qint64 total );

  private:
    explicit QgsUpdateService( QObject *parent );
    void setState( const QString &state, const QString &status );
    void fetch( const QUrl &url, std::function<void( QByteArray )> callback );
    void readRelease( const QByteArray &data );
    QString mState = QStringLiteral( "idle" ), mStatus, mNotes;
    QgsUpdatePackage mPackage;
    QPointer<QNetworkReply> mReply;
    QTemporaryDir mDirectory;
    QFile mDownload;
    QByteArray mManifest, mSignature;
    QString mPackagePath, mJobPath;
};
