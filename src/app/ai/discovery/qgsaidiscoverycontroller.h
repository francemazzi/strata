// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIDISCOVERYCONTROLLER_H
#define QGSAIDISCOVERYCONTROLLER_H
#include "qgis_app.h"
#include "qgsaidiscoveryclient.h"

#include <QJsonArray>
#include <QPointer>
#include <QWidget>

class QgsAiModelRouter;
class QgsAiFileContextProvider;
class QgsAiDiscoveryPreview;
class QgsAiDiscoveryDownload;
class QLabel;
class QPushButton;
class QgsMapCanvas;
class QgsProject;
class APP_EXPORT QgsAiDiscoveryController : public QObject
{
    Q_OBJECT
  public:
    QgsAiDiscoveryController( QgsAiModelRouter *router, QgsAiFileContextProvider *files, QgsMapCanvas *canvas, QgsProject *project, QObject *parent = nullptr );
    QJsonObject execute( const QString &tool, QJsonObject args );
    bool available() const;
    void resume();
  signals:
    void previewReady( QgsAiDiscoveryPreview *preview );
    void controlsReady( QWidget *widget );
    void message( const QString &text );

  private:
    bool scope();
    void showPreview( const QJsonObject &plan );
    void approve( const QJsonObject &plan, const QJsonArray &selection, int maxCredits, const QString &destination );
    void updated( const QString &kind, const QJsonObject &result );
    void deliver( const QJsonObject &run );
    void verifyImport( const QString &runId, const QString &path, const QJsonObject &grant, const QJsonObject &artifact );
    void recordImport( const QString &runId, const QJsonObject &outcome );
    void saveGrants();
    QPointer<QgsAiModelRouter> mRouter;
    QPointer<QgsAiFileContextProvider> mFiles;
    QPointer<QgsMapCanvas> mCanvas;
    QPointer<QgsProject> mProject;
    QgsAiDiscoveryClient *mClient;
    QString mScope, mAccountId, mWorkspaceRoot;
    QUrl mApiBase;
    QMap<QString, QString> mLastStates;
    QMap<QString, QPointer<QLabel>> mProgress;
    QMap<QString, QPointer<QPushButton>> mCancelButtons;
    QJsonObject mGrants;
    QSet<QString> mDelivering, mShown;
    QMap<QString, QPointer<QgsAiDiscoveryDownload>> mDownloads;
};
#endif
