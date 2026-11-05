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

#include "qgsaiclaudecodeauth.h"
#include "qgssettings.h"
#include <QFileDialog>
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
#include <QTimer>
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

  mCredentialMode = new QComboBox( this );
  mCredentialMode->setObjectName( u"aiClaudeCredentialMode"_s );
  mCredentialMode->addItem( tr( "Claude subscription" ), true );
  mCredentialMode->addItem( tr( "Anthropic API key" ), false );
  mCredentialMode->setCurrentIndex(
    router && router->providerSettings( QgsAiModelRouter::Provider::Claude ).credentialMode == QgsAiModelRouter::CredentialMode::ApiKey && router->hasStoredApiKey( QgsAiModelRouter::Provider::Claude )
      ? 1
      : 0
  );
  layout->addWidget( mCredentialMode );
  mSubscriptionModel = new QComboBox( this );
  mSubscriptionModel->setObjectName( u"aiClaudeSubscriptionModel"_s );
  mSubscriptionModel->setEditable( true );
  mSubscriptionModel->addItems( { u"sonnet"_s, u"opus"_s, u"haiku"_s } );
  mSubscriptionModel->setCurrentText( QgsSettings().value( u"ai/provider/claude/code/model"_s, u"sonnet"_s ).toString() );
  layout->addWidget( mSubscriptionModel );
  mLogin = new QgsAiClaudeCodeAuth( this );
  mConnectButton = new QPushButton( tr( "Connect Claude subscription" ), this );
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
  connect( mLogin, &QgsAiClaudeCodeAuth::statusChanged, this, [this]( bool ready, const QString &message ) {
    const int wait = QgsAiClaudeCodeAuth::cooldownSeconds();
    setConnectionState(
      !ready && wait > 0 ? ConnectionState::RateLimited
      : ready            ? ConnectionState::Connected
                         : ConnectionState::Disconnected,
      !ready && wait > 0 ? tr( "Wait %1 seconds before trying Claude again." ).arg( wait ) : message
    );
    if ( !ready && wait > 0 )
      QTimer::singleShot( wait * 1000, this, [this]() { refreshStatus(); } );
  } );
  connect( mLogoutButton, &QPushButton::clicked, this, [this]() {
    if ( mConnectionState == ConnectionState::Waiting )
    {
      mLogin->cancel();
      refreshStatus();
      return;
    }
    setConnectionState( ConnectionState::Waiting, tr( "Signing out of the Claude connection used by Strata…" ) );
    mLogin->logout();
  } );
  auto *install = new QPushButton( tr( "Install or update Claude Code" ), this );
  install->setObjectName( u"aiClaudeInstallButton"_s );
  layout->addWidget( install );
  connect( install, &QPushButton::clicked, this, []() { QDesktopServices::openUrl( QUrl( u"https://code.claude.com/docs/en/setup"_s ) ); } );
  auto *executable = new QPushButton( tr( "Locate Claude Code…" ), this );
  layout->addWidget( executable );
  connect( executable, &QPushButton::clicked, this, [this]() {
    const QString path = QFileDialog::getOpenFileName( this, tr( "Select the official Claude Code executable" ), QgsAiClaudeCodeAuth::executablePath() );
    if ( path.isEmpty() )
      return;
    QgsSettings().setValue( u"ai/provider/claude/code/executable"_s, path );
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
    mModelCombo->setCurrentText(
      QgsSettings()
        .value(
          u"ai/provider/claude/apiModel"_s,
          router->providerSettings( QgsAiModelRouter::Provider::Claude ).credentialMode == QgsAiModelRouter::CredentialMode::ApiKey ? router->providerSettings( QgsAiModelRouter::Provider::Claude ).model
                                                                                                                                    : QgsAiModelRouter::defaultClaudeModel()
        )
        .toString()
    );
  form->addWidget( mModelCombo );
  auto *use = new QPushButton( tr( "Use in this chat" ), advanced );
  use->setObjectName( u"aiClaudeUseButton"_s );
  layout->addWidget( use );
  connect( use, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::useRequested );
  connect( mCredentialMode, qOverload<int>( &QComboBox::currentIndexChanged ), this, [this, advanced]( int ) {
    mSubscriptionModel->setEnabled( usesSubscription() );
    advanced->setCollapsed( usesSubscription() );
  } );
  mSubscriptionModel->setEnabled( usesSubscription() );
  refreshStatus();
}

void QgsAiClaudeConnectWidget::startLogin()
{
  setConnectionState( ConnectionState::Waiting );
  mLogin->login();
}

bool QgsAiClaudeConnectWidget::usesSubscription() const
{
  return mCredentialMode->currentData().toBool();
}

QString QgsAiClaudeConnectWidget::modelText() const
{
  return ( usesSubscription() ? mSubscriptionModel : mModelCombo )->currentText().trimmed();
}

QString QgsAiClaudeConnectWidget::pendingApiKey() const
{
  return usesSubscription() ? QString() : mApiKeyEdit->text().trimmed();
}

void QgsAiClaudeConnectWidget::refreshStatus()
{
  setConnectionState( ConnectionState::Waiting, tr( "Checking the official Claude Code connection…" ) );
  mLogin->checkStatus();
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
      if ( body.isEmpty() )
        body = tr( "Connect your Claude subscription through the official Claude Code sign-in." );
      color = u"#718078"_s;
      break;
    case ConnectionState::Waiting:
      title = tr( "Waiting for Claude" );
      if ( body.isEmpty() )
        body = tr( "Complete the official Claude Code sign-in in the browser. Strata will update automatically." );
      color = u"#2f78a0"_s;
      break;
    case ConnectionState::Connected:
      title = tr( "Claude connected" );
      if ( body.isEmpty() )
        body = tr( "Claude is connected. Choose a model and select Use in this chat." );
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
  mConnectButton->setEnabled( !waiting && !connected && QgsAiClaudeCodeAuth::cooldownSeconds() == 0 );
  mConnectButton->setText( retry ? tr( "Try again" ) : tr( "Connect Claude subscription" ) );
  mLogoutButton->setEnabled( waiting || connected );
  mLogoutButton->setText( waiting ? tr( "Cancel" ) : tr( "Log out" ) );
}
