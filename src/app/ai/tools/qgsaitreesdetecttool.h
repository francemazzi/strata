/***************************************************************************
    qgsaitreesdetecttool.h
    ----------------------
    begin                : August 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSAITREESDETECTTOOL_H
#define QGSAITREESDETECTTOOL_H

#include "qgis_app.h"
#include "qgsaitool.h"

#include <QString>

using namespace Qt::StringLiterals;

class QgsAiModelRouter;

/**
 * Submits a public-tree detection job and waits for the GeoJSON
 * artifact. The artifact is not downloaded or added to the project.
 */
class APP_EXPORT QgsAiTreesDetectTool : public QgsAiTool
{
  public:
    explicit QgsAiTreesDetectTool( QgsAiModelRouter *router, int pollIntervalMs = 1000, int maxPollAttempts = 60 );

    QString name() const override { return u"trees_detect"_s; }
    QString description() const override;
    QJsonObject schema() const override;
    QgsAiToolResult execute( const QJsonObject &args ) override;

    /**
     * The detection starts a Strata Cloud job that uses the plan quota, so Ask-before-edits
     * offers it with the generic approval. It leaves the project unchanged, so Agent mode
     * runs it without asking.
     */
    bool requiresApproval() const override { return true; }
    QgsAiToolRiskLevel riskLevel() const override { return QgsAiToolRiskLevel::Medium; }
    bool isAvailable() const override;
    QString availabilityReason() const override;

  private:
    QgsAiModelRouter *mRouter = nullptr;
    int mPollIntervalMs = 1000;
    int mMaxPollAttempts = 60;
};

#endif // QGSAITREESDETECTTOOL_H
