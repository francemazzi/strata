/***************************************************************************
    qgsaicrsutils.h -- CRS metadata shared by AI tools and indexing
    Copyright (C) 2026 Francesco Mazzi
    SPDX-License-Identifier: GPL-2.0-or-later
***************************************************************************/
#ifndef QGSAICRSUTILS_H
#define QGSAICRSUTILS_H

#include "qgis_app.h"
#include "qgscoordinatereferencesystem.h"
#include <QJsonObject>
#include <QVariantMap>

class QgsMapLayer;

//! Pure metadata serialization. Never resolves a provider CRS or queries its source.
APP_EXPORT QJsonObject qgsAiCrsDetails( const QgsCoordinateReferenceSystem &crs, bool spatial = true, const QVariantMap &resolution = {} );
APP_EXPORT QJsonObject qgsAiLayerCrsDetails( const QgsMapLayer *layer );
//! Single-line, bounded label for untrusted CRS names in the model context.
APP_EXPORT QString qgsAiCrsLabel( const QJsonObject &details );

#endif
