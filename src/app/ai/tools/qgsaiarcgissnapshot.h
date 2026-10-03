// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIARCGISSNAPSHOT_H
#define QGSAIARCGISSNAPSHOT_H
#include "qgis_app.h"
#include <QJsonObject>
class QgsFeedback;
APP_EXPORT QString qgsAiCreateArcGisSnapshot( const QJsonObject &args, const QString &destination, QgsFeedback *feedback );
APP_EXPORT bool qgsAiWriteArcGisGeoTiff( const QByteArray &pixels, const QJsonObject &response, const QString &destination, QString &error );
#endif
