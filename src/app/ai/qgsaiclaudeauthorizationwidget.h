/***************************************************************************
    qgsaiclaudeauthorizationwidget.h
    ------------------------------
    begin                : November 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************/
/*                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSAICLAUDEAUTHORIZATIONWIDGET_H
#define QGSAICLAUDEAUTHORIZATIONWIDGET_H

#include "qgis_app.h"

#include <QUrl>
#include <QWidget>
#include <functional>

class QgsAiClaudeCodeAuth;

class APP_EXPORT QgsAiClaudeAuthorizationWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit QgsAiClaudeAuthorizationWidget( QgsAiClaudeCodeAuth *auth, QWidget *parent = nullptr, std::function<bool( const QUrl & )> openUrl = {} );

  private:
    QUrl mUrl;
};

#endif
