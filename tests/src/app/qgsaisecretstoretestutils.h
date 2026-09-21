/***************************************************************************
    qgsaisecretstoretestutils.h
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

#ifndef QGSAISECRETSTORETESTUTILS_H
#define QGSAISECRETSTORETESTUTILS_H
#include <memory>

#include "ai/qgsaisecretstore.h"

#include <QCoreApplication>
#include <QHash>
#include <QTimer>

inline std::shared_ptr<QHash<QString, QString>> installTestSecretBackend()
{
  QCoreApplication::processEvents();
  auto values = std::make_shared<QHash<QString, QString>>();
  QgsAiSecretStore::setBackendForTesting( [values]( QgsAiSecretStore::Operation operation, const QString &key, const QString &value, QgsAiSecretStore::BackendCallback done ) {
    QTimer::singleShot( 0, qApp, [values, operation, key, value, done]() {
      using Op = QgsAiSecretStore::Operation;
      if ( operation == Op::Write )
        values->insert( key, value );
      if ( operation == Op::Remove )
        values->remove( key );
      const bool found = values->contains( key );
      done( { operation != Op::Read || found, operation == Op::Read && !found, values->value( key ) } );
    } );
  } );
  return values;
}
#endif
