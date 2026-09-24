/***************************************************************************
    qgsaiclaudeconnectwidget.cpp
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

#include "qgsaiclaudeconnectwidget.h"

#include "qgsaiclaudeoauthclient.h"
#include "qgsaimodelrouter.h"
#include "qgscollapsiblegroupbox.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "moc_qgsaiclaudeconnectwidget.cpp"

using namespace Qt::StringLiterals;

QgsAiClaudeConnectWidget::QgsAiClaudeConnectWidget( QgsAiModelRouter *router, QWidget *parent )
  : QWidget( parent )
  , mRouter( router )
{
  auto *layout = new QVBoxLayout( this );
  layout->setContentsMargins( 0, 0, 0, 0 );

  mStatus = new QLabel( this );
  mStatus->setWordWrap( true );
  mStatus->setObjectName( u"aiClaudeLoginStatus"_s );
  layout->addWidget( mStatus );

  mLogin = new QgsAiClaudeOAuthClient( this );
  auto *connectButton = new QPushButton( tr( "Connect Claude" ), this );
  connectButton->setObjectName( u"aiClaudeConnectButton"_s );
  auto *logoutButton = new QPushButton( tr( "Log out" ), this );
  logoutButton->setObjectName( u"aiClaudeLogoutButton"_s );
  auto *buttons = new QWidget( this );
  auto *buttonRow = new QHBoxLayout( buttons );
  buttonRow->setContentsMargins( 0, 0, 0, 0 );
  buttonRow->addWidget( connectButton );
  buttonRow->addWidget( logoutButton );
  layout->addWidget( buttons );

  connect( connectButton, &QPushButton::clicked, this, [this]() {
    QString error;
    if ( !mLogin->start( &error ) )
    {
      mStatus->setText( error );
      return;
    }
    mStatus->setText( tr( "Approve Claude in the browser, then return here." ) );
    QDesktopServices::openUrl( QUrl( mLogin->currentAuthorizeUrl() ) );
  } );
  connect( mLogin, &QgsAiClaudeOAuthClient::loginSucceeded, this, [this]() {
    QString error;
    if ( mRouter )
      mRouter->setCredentialMode( QgsAiModelRouter::Provider::Claude, QgsAiModelRouter::CredentialMode::OAuth, &error );
    refreshStatus();
  } );
  connect( mLogin, &QgsAiClaudeOAuthClient::loginFailed, this, [this]( const QString &error ) {
    mStatus->setText( error );
  } );
  connect( logoutButton, &QPushButton::clicked, this, [this]() {
    mLogin->cancel();
    QgsAiClaudeOAuthClient::clearLogin();
    if ( mRouter )
      mRouter->setCredentialMode( QgsAiModelRouter::Provider::Claude, QgsAiModelRouter::CredentialMode::ApiKey );
    refreshStatus();
  } );

  auto *cloud = new QPushButton( tr( "Sign in to Strata Cloud" ), this );
  cloud->setObjectName( u"aiClaudeCloudButton"_s );
  layout->addWidget( cloud );
  connect( cloud, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::cloudRequested );

  auto *advanced = new QgsCollapsibleGroupBox( tr( "Configure an API key (advanced)" ), this );
  advanced->setSaveCollapsedState( false );
  advanced->setCollapsed( !router || !router->hasStoredApiKey( QgsAiModelRouter::Provider::Claude ) );
  layout->addWidget( advanced );
  auto *form = new QVBoxLayout( advanced );
  auto *billing = new QLabel( tr( "Anthropic API usage is billed separately from a Claude subscription." ), advanced );
  billing->setWordWrap( true );
  form->addWidget( billing );
  mApiKeyEdit = new QLineEdit( advanced );
  mApiKeyEdit->setEchoMode( QLineEdit::Password );
  mApiKeyEdit->setObjectName( u"aiClaudeApiKeyLineEdit"_s );
  mApiKeyEdit->setPlaceholderText( tr( "API key — leave empty to keep the saved key" ) );
  form->addWidget( mApiKeyEdit );
  mModelCombo = new QComboBox( advanced );
  mModelCombo->setEditable( true );
  mModelCombo->addItems( { u"claude-sonnet-5"_s, u"claude-opus-4-8"_s, u"claude-haiku-4-5"_s } );
  if ( router )
    mModelCombo->setCurrentText( router->providerSettings( QgsAiModelRouter::Provider::Claude ).model );
  form->addWidget( mModelCombo );
  auto *use = new QPushButton( tr( "Use in this chat" ), advanced );
  use->setObjectName( u"aiClaudeUseButton"_s );
  form->addWidget( use );
  connect( use, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::useRequested );
  refreshStatus();
}

QString QgsAiClaudeConnectWidget::modelText() const
{
  return mModelCombo->currentText().trimmed();
}

QString QgsAiClaudeConnectWidget::pendingApiKey() const
{
  return mApiKeyEdit->text().trimmed();
}

void QgsAiClaudeConnectWidget::refreshStatus()
{
  if ( QgsAiClaudeOAuthClient::hasRefreshToken() )
    mStatus->setText( tr( "Signed in with a Claude subscription." ) );
  else
    mStatus->setText( tr( "Not signed in. Connect Claude opens the browser and finishes on this computer." ) );
}
