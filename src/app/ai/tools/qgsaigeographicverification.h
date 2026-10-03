// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIGEOGRAPHICVERIFICATION_H
#define QGSAIGEOGRAPHICVERIFICATION_H
#include "qgis_app.h"
#include <QJsonObject>
class QgsMapLayer;
class QgsProject;
APP_EXPORT QJsonObject qgsAiVerifyLayerQuality( QgsMapLayer *layer, QgsProject *project, bool *canceled = nullptr );
APP_EXPORT QJsonObject qgsAiVerifyRasterSource( QgsMapLayer *layer );
#endif
