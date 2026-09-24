/***************************************************************************
    qgsaiclaudeconnectwidget.h
    ---------------------
    begin                : September 2026
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

#ifndef QGSAICLAUDECONNECTWIDGET_H
#define QGSAICLAUDECONNECTWIDGET_H
#include "qgis_app.h"

#include <QWidget>

class QgsAiClaudeOAuthClient;
class QgsAiModelRouter;
class QLabel;
class QLineEdit;
class QComboBox;
class APP_EXPORT QgsAiClaudeConnectWidget : public QWidget
{
    Q_OBJECT
  public:
    explicit QgsAiClaudeConnectWidget( QgsAiModelRouter *router, QWidget *parent = nullptr );
    QString modelText() const;
    QString pendingApiKey() const;
  signals:
    void cloudRequested();
    void useRequested();

  private:
    void refreshStatus();

    QgsAiModelRouter *mRouter = nullptr;
    QgsAiClaudeOAuthClient *mLogin = nullptr;
    QLabel *mStatus = nullptr;
    QLineEdit *mApiKeyEdit = nullptr;
    QComboBox *mModelCombo = nullptr;
};
#endif
