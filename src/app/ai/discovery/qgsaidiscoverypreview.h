// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIDISCOVERYPREVIEW_H
#define QGSAIDISCOVERYPREVIEW_H
#include "qgis_app.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class APP_EXPORT QgsAiDiscoveryPreview : public QWidget
{
    Q_OBJECT
  public:
    QgsAiDiscoveryPreview( const QJsonObject &plan, const QString &defaultDestination, QWidget *parent = nullptr );
  signals:
    void approved( const QJsonArray &selection, int maxCredits, const QString &destination );
    void canceled();
};
#endif
