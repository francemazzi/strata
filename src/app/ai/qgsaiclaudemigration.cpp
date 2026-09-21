/***************************************************************************
    qgsaiclaudemigration.cpp
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

#include "qgsaiclaudemigration.h"

#include "qgsapplication.h"
#include "qgsauthmanager.h"
#include "qgssettings.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>

using namespace Qt::StringLiterals;

void QgsAiClaudeMigration::run()
{
  QgsSettings settings;
  if ( settings.value( u"ai/provider/claude/credentialMode"_s ).toString() == "oauth"_L1 )
  {
    if ( settings.value( u"ai/activeProvider"_s ).toString() == "Claude"_L1 )
      settings.setValue( u"ai/security/providerSelectionRequired"_s, true );
    settings.setValue( u"ai/provider/claude/credentialMode"_s, u"apiKey"_s );
    settings.setValue( u"ai/provider/claude/enabled"_s, false );
  }
  // These belong to Strata alone. Never inspect or change Claude Code's credentials.
  for ( const QString &key : { u"ai/provider/claude/subscriptionToken"_s, u"ai/provider/claude/oauth/refreshToken"_s } )
  {
    auto *auth = QgsApplication::authManager();
    if ( auth && !auth->isDisabled() && auth->existsAuthSetting( key ) )
      auth->removeAuthSetting( key ); // No decryption or master password prompt.
    settings.remove( key );
    settings.remove( key + u"_inVault"_s );
  }
  settings.remove( u"ai/provider/claude/oauth"_s );
  settings.remove( u"ai/provider/claude/subscription"_s );
  settings.remove( u"ai/provider/claude/cliPath"_s );
  const QDir temp( QStandardPaths::writableLocation( QStandardPaths::TempLocation ) );
  const QRegularExpression ownedFile( u"^strata-claude-setup-token-[0-9a-fA-F-]{36}\\.(log|ps1)$"_s );
  for ( const QFileInfo &file : temp.entryInfoList( { u"strata-claude-setup-token-*"_s }, QDir::Files | QDir::NoSymLinks ) )
  {
    if ( ownedFile.match( file.fileName() ).hasMatch() && file.isWritable() )
      QFile::remove( file.absoluteFilePath() );
  }
}
