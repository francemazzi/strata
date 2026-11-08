// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef QGSAIDISCOVERYTOOL_H
#define QGSAIDISCOVERYTOOL_H
#include "qgsaitool.h"

#include <QString>

using namespace Qt::StringLiterals;

class QgsAiDiscoveryController;
class APP_EXPORT QgsAiDiscoveryTool : public QgsAiTool
{
  public:
    QgsAiDiscoveryTool( const QString &name, QgsAiDiscoveryController *controller );
    QString name() const override { return mName; }
    QString description() const override;
    QJsonObject schema() const override;
    QgsAiToolResult execute( const QJsonObject &args ) override;
    bool isAvailable() const override;

    /**
     * discovery_run asks in its own preview dialog. discovery_cancel stops a remote job,
     * so Ask-before-edits offers it with the generic approval.
     */
    bool requiresApproval() const override { return mName == "discovery_run"_L1 || mName == "discovery_cancel"_L1; }
    QgsAiToolApprovalMode approvalMode() const override
    {
      if ( mName == "discovery_run"_L1 )
        return QgsAiToolApprovalMode::SelfApproved;
      return mName == "discovery_cancel"_L1 ? QgsAiToolApprovalMode::Generic : QgsAiToolApprovalMode::None;
    }
    QgsAiToolRiskLevel riskLevel() const override { return mName == "discovery_run"_L1 ? QgsAiToolRiskLevel::High : QgsAiToolRiskLevel::Low; }

  private:
    QString mName;
    QgsAiDiscoveryController *mController;
};
#endif
