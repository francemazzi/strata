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
#include <QFrame>
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

  mStatusFrame = new QFrame( this );
  mStatusFrame->setObjectName( u"aiClaudeStatusCard"_s );
  auto *statusLayout = new QVBoxLayout( mStatusFrame );
  statusLayout->setContentsMargins( 12, 10, 12, 10 );
  statusLayout->setSpacing( 3 );
  mStatusTitle = new QLabel( mStatusFrame );
  mStatusTitle->setObjectName( u"aiClaudeLoginStatusTitle"_s );
  statusLayout->addWidget( mStatusTitle );
  mStatus = new QLabel( mStatusFrame );
  mStatus->setWordWrap( true );
  mStatus->setObjectName( u"aiClaudeLoginStatus"_s );
  statusLayout->addWidget( mStatus );
  layout->addWidget( mStatusFrame );

  mLogin = new QgsAiClaudeOAuthClient( this );
  mConnectButton = new QPushButton( tr( "Connect Claude" ), this );
  mConnectButton->setObjectName( u"aiClaudeConnectButton"_s );
  mLogoutButton = new QPushButton( tr( "Log out" ), this );
  mLogoutButton->setObjectName( u"aiClaudeLogoutButton"_s );
  auto *buttons = new QWidget( this );
  auto *buttonRow = new QHBoxLayout( buttons );
  buttonRow->setContentsMargins( 0, 0, 0, 0 );
  buttonRow->addWidget( mConnectButton );
  buttonRow->addWidget( mLogoutButton );
  layout->addWidget( buttons );

  connect( mConnectButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::startLogin );
  connect( mLogin, &QgsAiClaudeOAuthClient::loginSucceeded, this, [this]() {
    QString error;
    if ( mRouter && !mRouter->setCredentialMode( QgsAiModelRouter::Provider::Claude, QgsAiModelRouter::CredentialMode::OAuth, &error ) )
    {
      QgsAiClaudeOAuthClient::clearLogin();
      setConnectionState( ConnectionState::Error, tr( "Claude connected, but Strata could not activate the subscription." ), error );
      return;
    }
    setConnectionState( ConnectionState::Connected );
  } );
  connect( mLogin, &QgsAiClaudeOAuthClient::loginFailed, this, [this]( const QgsAiClaudeOAuthError &error ) {
    QString message = error.userMessage;
    if ( error.retryAfterSeconds > 0 )
      message += tr( " Try again in about %1 seconds." ).arg( error.retryAfterSeconds );
    setConnectionState( error.category == QgsAiClaudeOAuthError::Category::RateLimited ? ConnectionState::RateLimited : ConnectionState::Error, message, error.technicalDetail );
  } );
  connect( mLogoutButton, &QPushButton::clicked, this, [this]() {
    if ( mConnectionState == ConnectionState::Waiting )
    {
      mLogin->cancel();
      setConnectionState( ConnectionState::Disconnected );
      return;
    }
    mLogin->cancel();
    QgsAiClaudeOAuthClient::clearLogin();
    if ( mRouter )
      mRouter->setCredentialMode( QgsAiModelRouter::Provider::Claude, QgsAiModelRouter::CredentialMode::ApiKey );
    setConnectionState( ConnectionState::Disconnected );
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

void QgsAiClaudeConnectWidget::startLogin()
{
  QString error;
  if ( !mLogin->start( &error ) )
  {
    setConnectionState( ConnectionState::Error, error );
    return;
  }

  setConnectionState( ConnectionState::Waiting );
  if ( QDesktopServices::openUrl( QUrl( mLogin->currentAuthorizeUrl() ) ) )
    return;

  mLogin->cancel();
  setConnectionState( ConnectionState::Error, tr( "Strata could not open the browser. Check your default browser and try again." ) );
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
    setConnectionState( ConnectionState::Connected );
  else
    setConnectionState( ConnectionState::Disconnected );
}

void QgsAiClaudeConnectWidget::setConnectionState( ConnectionState state, const QString &message, const QString &technicalDetail )
{
  mConnectionState = state;
  QString title;
  QString body = message;
  QString color;
  switch ( state )
  {
    case ConnectionState::Disconnected:
      title = tr( "Claude not connected" );
      body = tr( "Connect in the browser to use your Claude subscription in Strata." );
      color = u"#718078"_s;
      break;
    case ConnectionState::Waiting:
      title = tr( "Waiting for Claude" );
      body = tr( "Approve the connection in the browser. Strata will update automatically." );
      color = u"#2f78a0"_s;
      break;
    case ConnectionState::Connected:
      title = tr( "Claude connected" );
      body = tr( "Your Claude subscription is ready to use." );
      color = u"#16835c"_s;
      break;
    case ConnectionState::RateLimited:
      title = tr( "Try again later" );
      color = u"#b16814"_s;
      break;
    case ConnectionState::Error:
      title = tr( "Connection not completed" );
      color = u"#b53c38"_s;
      break;
  }

  mStatusTitle->setText( title );
  mStatus->setText( body );
  mStatus->setToolTip( technicalDetail );
  mStatusFrame->setStyleSheet(
    u"QFrame#aiClaudeStatusCard { background: palette(alternate-base); border-left: 4px solid %1; border-radius: 7px; } QLabel { background: transparent; border: 0; } QLabel#aiClaudeLoginStatusTitle { color: %1; font-weight: 700; }"_s
      .arg( color )
  );

  const bool waiting = state == ConnectionState::Waiting;
  const bool connected = state == ConnectionState::Connected;
  const bool retry = state == ConnectionState::RateLimited || state == ConnectionState::Error;
  mConnectButton->setEnabled( !waiting && !connected );
  mConnectButton->setText( retry ? tr( "Try again" ) : tr( "Connect Claude" ) );
  mLogoutButton->setEnabled( waiting || connected );
  mLogoutButton->setText( waiting ? tr( "Cancel" ) : tr( "Log out" ) );
}
