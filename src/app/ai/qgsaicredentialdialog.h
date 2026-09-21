/***************************************************************************
    qgsaicredentialdialog.h
    ---------------------
    begin                : June 2026
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

#ifndef QGSAICREDENTIALDIALOG_H
#define QGSAICREDENTIALDIALOG_H
#include <functional>

#include <QMap>
#include <QString>

class QWidget;
namespace QgsAiCredentialDialog
{
  // Context-bound continuation; a deleted dialog can never activate a provider.
  void save( QWidget *parent, const QMap<QString, QString> &credentials, std::function<void( bool )> finished );
} //namespace QgsAiCredentialDialog
#endif
