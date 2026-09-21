/***************************************************************************
    qgsaicredentialdialog.cpp
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

#include "qgsaicredentialdialog.h"

#include <memory>

#include "qgsaisecretstore.h"

#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

namespace QgsAiCredentialDialogInternal
{
  struct Save : std::enable_shared_from_this<Save>
  {
      QPointer<QWidget> parent;
      QMap<QString, QString> remaining;
      std::function<void( bool )> finished;
      void next()
      {
        if ( !parent )
          return;
        if ( remaining.isEmpty() )
        {
          finished( true );
          return;
        }
        const QString key = remaining.firstKey();
        const QString value = remaining.first();
        const auto self = shared_from_this();
        QgsAiSecretStore::writeSecretAsync( key, value, parent, [self, key, value]( const QgsAiSecretStore::SecretResult &result ) {
          if ( result.ok() )
          {
            self->remaining.remove( key );
            self->next();
            return;
          }
          auto *message = new QMessageBox( QMessageBox::Warning, QObject::tr( "Save credentials securely" ), result.error, QMessageBox::NoButton, self->parent );
          message->setAttribute( Qt::WA_DeleteOnClose );
          auto *retry = message->addButton( QObject::tr( "Retry" ), QMessageBox::AcceptRole );
          auto *session = message->addButton( QObject::tr( "Use only for this session" ), QMessageBox::ActionRole );
          session->setEnabled( result.sessionAllowed );
          message->addButton( QMessageBox::Cancel );
          message->setDefaultButton( retry );
          QObject::connect( message, &QMessageBox::finished, self->parent, [self, message, retry, session, key, value]() {
            if ( message->clickedButton() == retry )
            {
              self->next();
              return;
            }
            if ( message->clickedButton() == session )
            {
              QgsAiSecretStore::useForSession( key, value );
              self->remaining.remove( key );
              self->next();
              return;
            }
            self->finished( false );
          } );
          message->open();
        } );
      }
  };
} //namespace QgsAiCredentialDialogInternal
void QgsAiCredentialDialog::save( QWidget *parent, const QMap<QString, QString> &credentials, std::function<void( bool )> finished )
{
  const auto operation = std::make_shared<QgsAiCredentialDialogInternal::Save>();
  operation->parent = parent;
  operation->remaining = credentials;
  operation->finished = std::move( finished );
  operation->next();
}
