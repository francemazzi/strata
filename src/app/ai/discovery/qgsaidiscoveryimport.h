// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIDISCOVERYIMPORT_H
#define QGSAIDISCOVERYIMPORT_H
#include "qgis_app.h"
#include "qgscoordinatetransformcontext.h"

#include <QJsonObject>
#include <QList>

class QgsMapLayer;
class QThread;
namespace QgsAiDiscoveryImport
{
  struct Result
  {
      QList<QgsMapLayer *> layers;
      QJsonObject outcome;
  };
  APP_EXPORT Result open( const QJsonObject &manifest, const QString &directory, const QgsCoordinateTransformContext &context, QThread *destinationThread );
} //namespace QgsAiDiscoveryImport
#endif
