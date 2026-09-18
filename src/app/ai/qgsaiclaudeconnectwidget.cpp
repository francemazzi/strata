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

#include "qgsaiclaudecodecli.h"
#include "qgsaimodelrouter.h"
#include "qgsaisettingsutils.h"
#include "qgscollapsiblegroupbox.h"
#include "qgssettings.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QStyle>
#include <QVBoxLayout>

#include "moc_qgsaiclaudeconnectwidget.cpp"

using namespace Qt::StringLiterals;
using QgsAiSettingsUtils::sectionHeader;
using QgsAiSettingsUtils::settingRow;

namespace
{
  constexpr int EXPIRY_WARNING_DAYS = 14;

  QStringList knownClaudeModels()
  {
    return { QgsAiModelRouter::defaultClaudeModel(), u"claude-opus-4-8"_s, u"claude-haiku-4-5"_s };
  }

  QString subscriptionTypeLabel( const QString &type )
  {
    if ( type.isEmpty() )
      return QString();
    return type.left( 1 ).toUpper() + type.mid( 1 );
  }
} // namespace

QgsAiClaudeConnectWidget::QgsAiClaudeConnectWidget( QgsAiModelRouter *modelRouter, QWidget *parent )
  : QWidget( parent )
  , mModelRouter( modelRouter )
{
  setObjectName( u"aiClaudeConnectWidget"_s );
  mCli = new QgsAiClaudeCodeCli( this );

  QVBoxLayout *layout = new QVBoxLayout( this );
  layout->setContentsMargins( 0, 0, 0, 0 );
  layout->setSpacing( 10 );

  // ---- Subscription / API key toggle ----
  QWidget *toggleRow = new QWidget( this );
  QHBoxLayout *toggleLayout = new QHBoxLayout( toggleRow );
  toggleLayout->setContentsMargins( 0, 0, 0, 0 );
  toggleLayout->setSpacing( 0 );
  mModeSubscriptionButton = new QPushButton( tr( "Claude subscription" ), toggleRow );
  mModeSubscriptionButton->setObjectName( u"aiClaudeModeSubscriptionButton"_s );
  mModeSubscriptionButton->setToolTip( tr( "Use your Claude Pro/Max plan through the Claude Code CLI." ) );
  mModeApiKeyButton = new QPushButton( tr( "Anthropic API key" ), toggleRow );
  mModeApiKeyButton->setObjectName( u"aiClaudeModeApiKeyButton"_s );
  mModeApiKeyButton->setToolTip( tr( "Pay per use with an API key from the Anthropic Console." ) );
  for ( QPushButton *button : { mModeSubscriptionButton, mModeApiKeyButton } )
  {
    button->setCheckable( true );
    button->setProperty( "aiSegment", true );
    button->setCursor( Qt::PointingHandCursor );
    toggleLayout->addWidget( button, 1 );
  }
  QButtonGroup *modeGroup = new QButtonGroup( toggleRow );
  modeGroup->setExclusive( true );
  modeGroup->addButton( mModeSubscriptionButton );
  modeGroup->addButton( mModeApiKeyButton );
  connect( mModeSubscriptionButton, &QPushButton::toggled, this, [this]( bool checked ) {
    if ( checked )
      setMode( QgsAiModelRouter::CredentialMode::OAuth );
  } );
  connect( mModeApiKeyButton, &QPushButton::toggled, this, [this]( bool checked ) {
    if ( checked )
      setMode( QgsAiModelRouter::CredentialMode::ApiKey );
  } );
  toggleRow->setMaximumWidth( 420 );
  layout->addWidget( toggleRow );

  mModeStack = new QStackedWidget( this );
  mModeStack->setObjectName( u"aiClaudeModeStack"_s );
  mModeStack->setSizePolicy( QSizePolicy::Preferred, QSizePolicy::Maximum );
  mModeStack->addWidget( buildSubscriptionPane() );
  mModeStack->addWidget( buildApiKeyPane() );
  layout->addWidget( mModeStack );

  mStatusLabel = new QLabel( this );
  mStatusLabel->setObjectName( u"aiClaudeStatusLabel"_s );
  mStatusLabel->setWordWrap( true );
  mStatusLabel->setMaximumWidth( 420 );
  layout->addWidget( mStatusLabel );

  // ---- Model ----
  mModelCombo = new QComboBox( this );
  mModelCombo->setObjectName( u"aiClaudeModelComboBox"_s );
  mModelCombo->setEditable( true );
  mModelCombo->addItems( knownClaudeModels() );
  const QString currentModel = mModelRouter ? mModelRouter->providerSettings( QgsAiModelRouter::Provider::Claude ).model : QgsAiModelRouter::defaultClaudeModel();
  mModelCombo->setCurrentText( currentModel.isEmpty() ? QgsAiModelRouter::defaultClaudeModel() : currentModel );
  layout->addWidget( settingRow( tr( "Model" ), tr( "Default model for the Claude provider." ), mModelCombo, this ) );

  // ---- Advanced ----
  mAdvancedGroup = new QgsCollapsibleGroupBox( tr( "Advanced" ), this );
  mAdvancedGroup->setObjectName( u"aiClaudeAdvancedGroupBox"_s );
  QFormLayout *advancedForm = new QFormLayout( mAdvancedGroup );
  mManualTokenEdit = new QLineEdit( mAdvancedGroup );
  mManualTokenEdit->setObjectName( u"aiClaudeManualTokenLineEdit"_s );
  mManualTokenEdit->setEchoMode( QLineEdit::Password );
  mManualTokenEdit->setPlaceholderText( tr( "Paste the token printed by: claude setup-token" ) );
  mManualTokenEdit->setToolTip( tr( "Last resort when the automatic connection cannot run. Saved when you click OK." ) );
  advancedForm->addRow( tr( "Subscription token" ), mManualTokenEdit );

  QWidget *cliPathRow = new QWidget( mAdvancedGroup );
  QHBoxLayout *cliPathLayout = new QHBoxLayout( cliPathRow );
  cliPathLayout->setContentsMargins( 0, 0, 0, 0 );
  cliPathLayout->setSpacing( 6 );
  mCliPathEdit = new QLineEdit( cliPathRow );
  mCliPathEdit->setObjectName( u"aiClaudeCliPathLineEdit"_s );
  mCliPathEdit->setReadOnly( true );
  mCliPathEdit->setPlaceholderText( tr( "Auto-detected" ) );
  mCliPathBrowseButton = new QPushButton( tr( "Choose…" ), cliPathRow );
  mCliPathBrowseButton->setObjectName( u"aiClaudeCliPathBrowseButton"_s );
  mCliPathResetButton = new QPushButton( tr( "Auto-detect" ), cliPathRow );
  mCliPathResetButton->setObjectName( u"aiClaudeCliPathResetButton"_s );
  cliPathLayout->addWidget( mCliPathEdit, 1 );
  cliPathLayout->addWidget( mCliPathBrowseButton );
  cliPathLayout->addWidget( mCliPathResetButton );
  advancedForm->addRow( tr( "Claude Code executable" ), cliPathRow );
  connect( mCliPathBrowseButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::chooseExecutable );
  connect( mCliPathResetButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::resetExecutable );
  // Always start collapsed: the group is auto-expanded only as an escape hatch after a failure.
  mAdvancedGroup->setSaveCollapsedState( false );
  mAdvancedGroup->setCollapsed( true );
  layout->addWidget( mAdvancedGroup );

  setStyleSheet( uR"css(
QPushButton[aiSegment="true"] { border: 1px solid palette(mid); background: transparent; padding: 6px 14px; }
QPushButton[aiSegment="true"]:checked { background: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); font-weight: 600; }
QPushButton#aiClaudeModeSubscriptionButton { border-top-left-radius: 6px; border-bottom-left-radius: 6px; }
QPushButton#aiClaudeModeApiKeyButton { border-top-right-radius: 6px; border-bottom-right-radius: 6px; }
QPushButton#aiClaudeConnectButton, QPushButton#aiClaudeSubmitCodeButton { background: palette(highlight); color: palette(highlighted-text); border: none; border-radius: 6px; padding: 9px 12px; font-weight: 600; }
QPushButton#aiClaudeConnectButton:disabled, QPushButton#aiClaudeSubmitCodeButton:disabled { background: palette(midlight); color: palette(dark); }
QLabel#aiClaudeAvatarLabel { background: palette(highlight); color: palette(highlighted-text); border-radius: 20px; font-weight: 600; }
QFrame#aiClaudeAccountCard { background: palette(button); border-radius: 10px; }
QLabel#aiClaudeStatusLabel[status="error"] { color: #e5534b; }
QLabel#aiClaudeExpiryLabel[expiry="warning"] { color: #d29922; }
QLabel#aiClaudeExpiryLabel[expiry="expired"] { color: #e5534b; }
QLabel[aiRole="rowDescription"] { color: palette(dark); }
)css"_s );

  // ---- CLI wiring ----
  connect( mCli, &QgsAiClaudeCodeCli::probeFinished, this, &QgsAiClaudeConnectWidget::onProbeFinished );
  connect( mCli, &QgsAiClaudeCodeCli::browserUrlDetected, this, &QgsAiClaudeConnectWidget::onBrowserUrlDetected );
  connect( mCli, &QgsAiClaudeCodeCli::authorizationCodeRequested, this, &QgsAiClaudeConnectWidget::onCodeRequested );
  connect( mCli, &QgsAiClaudeCodeCli::tokenReceived, this, &QgsAiClaudeConnectWidget::onTokenReceived );
  connect( mCli, &QgsAiClaudeCodeCli::failed, this, &QgsAiClaudeConnectWidget::onSessionFailed );
  connect( mCli, &QgsAiClaudeCodeCli::finished, this, &QgsAiClaudeConnectWidget::onSessionFinished );

  // Fresh installs land on the subscription path (the recommended one); an
  // explicit API-key configuration is respected.
  const QgsAiModelRouter::ProviderSettings settings = mModelRouter ? mModelRouter->providerSettings( QgsAiModelRouter::Provider::Claude ) : QgsAiModelRouter::ProviderSettings();
  const bool hasApiKey = mModelRouter && mModelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::Claude );
  const bool subscriptionMode = settings.credentialMode == QgsAiModelRouter::CredentialMode::OAuth || !hasApiKey;
  ( subscriptionMode ? mModeSubscriptionButton : mModeApiKeyButton )->setChecked( true );
  setMode( subscriptionMode ? QgsAiModelRouter::CredentialMode::OAuth : QgsAiModelRouter::CredentialMode::ApiKey );

  setSubscriptionState( isConnected() ? SubscriptionState::Connected : SubscriptionState::NotConnected );
  updateCliPathField();
  refreshCliStatus();
}

QgsAiClaudeConnectWidget::~QgsAiClaudeConnectWidget()
{
  if ( mCli && mCli->isRunning() )
    mCli->cancel();
}

// ------------------------------------------------------------------- panes

QWidget *QgsAiClaudeConnectWidget::buildSubscriptionPane()
{
  QWidget *pane = new QWidget( this );
  QVBoxLayout *layout = new QVBoxLayout( pane );
  layout->setContentsMargins( 0, 0, 0, 0 );
  layout->setSpacing( 8 );

  QLabel *intro = new QLabel( tr( "Strata runs the Claude Code CLI for you: approve the login in your browser and the subscription token is stored locally." ), pane );
  intro->setWordWrap( true );
  intro->setProperty( "aiRole", u"rowDescription"_s );
  layout->addWidget( intro );

  mSubscriptionStack = new QStackedWidget( pane );
  mSubscriptionStack->setObjectName( u"aiClaudeSubscriptionStack"_s );
  mSubscriptionStack->setSizePolicy( QSizePolicy::Preferred, QSizePolicy::Maximum );
  mSubscriptionStack->addWidget( buildNotConnectedPane( pane ) );
  mSubscriptionStack->addWidget( buildConnectingPane( pane ) );
  mSubscriptionStack->addWidget( buildConnectedPane( pane ) );
  layout->addWidget( mSubscriptionStack );
  return pane;
}

QWidget *QgsAiClaudeConnectWidget::buildApiKeyPane()
{
  QWidget *pane = new QWidget( this );
  QVBoxLayout *layout = new QVBoxLayout( pane );
  layout->setContentsMargins( 0, 0, 0, 0 );
  layout->setSpacing( 8 );

  mApiKeyEdit = new QLineEdit( pane );
  mApiKeyEdit->setObjectName( u"aiClaudeApiKeyLineEdit"_s );
  mApiKeyEdit->setEchoMode( QLineEdit::Password );
  const bool hasApiKey = mModelRouter && mModelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::Claude );
  mApiKeyEdit->setPlaceholderText( hasApiKey ? tr( "Saved locally — enter a new key only to replace it" ) : tr( "sk-ant-api03-…" ) );
  layout->addWidget( settingRow( tr( "API key" ), tr( "Stored locally. Leave empty to keep the saved key." ), mApiKeyEdit, pane ) );
  return pane;
}

QWidget *QgsAiClaudeConnectWidget::buildNotConnectedPane( QWidget *parent )
{
  QWidget *pane = new QWidget( parent );
  pane->setMaximumWidth( 420 );
  QVBoxLayout *layout = new QVBoxLayout( pane );
  layout->setContentsMargins( 0, 0, 0, 0 );
  layout->setSpacing( 8 );

  mCliStatusLabel = new QLabel( pane );
  mCliStatusLabel->setObjectName( u"aiClaudeCliStatusLabel"_s );
  mCliStatusLabel->setWordWrap( true );
  layout->addWidget( mCliStatusLabel );

  mInstallLink = new QLabel( pane );
  mInstallLink->setObjectName( u"aiClaudeInstallLink"_s );
  mInstallLink->setTextFormat( Qt::RichText );
  mInstallLink->setOpenExternalLinks( true );
  mInstallLink->setText( u"<a href=\"%1\">%2</a>"_s.arg( QgsAiClaudeCodeCli::installDocsUrl().toString(), tr( "Install Claude Code…" ) ) );
  mInstallLink->setVisible( false );
  layout->addWidget( mInstallLink );

  QWidget *toolsRow = new QWidget( pane );
  QHBoxLayout *toolsLayout = new QHBoxLayout( toolsRow );
  toolsLayout->setContentsMargins( 0, 0, 0, 0 );
  mChooseExecutableButton = new QPushButton( tr( "Choose executable…" ), toolsRow );
  mChooseExecutableButton->setObjectName( u"aiClaudeChooseExecutableButton"_s );
  mRecheckButton = new QPushButton( tr( "Check again" ), toolsRow );
  mRecheckButton->setObjectName( u"aiClaudeRecheckButton"_s );
  toolsLayout->addWidget( mChooseExecutableButton );
  toolsLayout->addWidget( mRecheckButton );
  toolsLayout->addStretch( 1 );
  layout->addWidget( toolsRow );
  connect( mChooseExecutableButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::chooseExecutable );
  connect( mRecheckButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::refreshCliStatus );

  mConnectButton = new QPushButton( tr( "Connect Claude Code" ), pane );
  mConnectButton->setObjectName( u"aiClaudeConnectButton"_s );
  mConnectButton->setCursor( Qt::PointingHandCursor );
  mConnectButton->setEnabled( false );
  layout->addWidget( mConnectButton );
  connect( mConnectButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::startConnect );
  return pane;
}

QWidget *QgsAiClaudeConnectWidget::buildConnectingPane( QWidget *parent )
{
  QWidget *pane = new QWidget( parent );
  pane->setMaximumWidth( 420 );
  QVBoxLayout *layout = new QVBoxLayout( pane );
  layout->setContentsMargins( 0, 0, 0, 0 );
  layout->setSpacing( 8 );

  mProgressLabel = new QLabel( pane );
  mProgressLabel->setObjectName( u"aiClaudeProgressLabel"_s );
  mProgressLabel->setWordWrap( true );
  layout->addWidget( mProgressLabel );

  mAuthCodeRow = new QWidget( pane );
  mAuthCodeRow->setObjectName( u"aiClaudeAuthCodeRow"_s );
  QHBoxLayout *codeLayout = new QHBoxLayout( mAuthCodeRow );
  codeLayout->setContentsMargins( 0, 0, 0, 0 );
  codeLayout->setSpacing( 6 );
  mAuthCodeEdit = new QLineEdit( mAuthCodeRow );
  mAuthCodeEdit->setObjectName( u"aiClaudeAuthCodeLineEdit"_s );
  mAuthCodeEdit->setPlaceholderText( tr( "Authorization code from the browser" ) );
  mSubmitCodeButton = new QPushButton( tr( "Submit" ), mAuthCodeRow );
  mSubmitCodeButton->setObjectName( u"aiClaudeSubmitCodeButton"_s );
  codeLayout->addWidget( mAuthCodeEdit, 1 );
  codeLayout->addWidget( mSubmitCodeButton );
  mAuthCodeRow->setVisible( false );
  layout->addWidget( mAuthCodeRow );
  connect( mSubmitCodeButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::submitCode );
  connect( mAuthCodeEdit, &QLineEdit::returnPressed, this, &QgsAiClaudeConnectWidget::submitCode );

  QWidget *buttonRow = new QWidget( pane );
  QHBoxLayout *buttonLayout = new QHBoxLayout( buttonRow );
  buttonLayout->setContentsMargins( 0, 0, 0, 0 );
  mOpenBrowserAgainButton = new QPushButton( tr( "Open browser again" ), buttonRow );
  mOpenBrowserAgainButton->setObjectName( u"aiClaudeOpenBrowserAgainButton"_s );
  mOpenBrowserAgainButton->setEnabled( false );
  mCancelButton = new QPushButton( tr( "Cancel" ), buttonRow );
  mCancelButton->setObjectName( u"aiClaudeCancelButton"_s );
  buttonLayout->addWidget( mOpenBrowserAgainButton );
  buttonLayout->addWidget( mCancelButton );
  buttonLayout->addStretch( 1 );
  layout->addWidget( buttonRow );
  connect( mOpenBrowserAgainButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::openBrowserAgain );
  connect( mCancelButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::cancelConnect );
  return pane;
}

QWidget *QgsAiClaudeConnectWidget::buildConnectedPane( QWidget *parent )
{
  QWidget *pane = new QWidget( parent );
  pane->setMaximumWidth( 420 );
  QVBoxLayout *layout = new QVBoxLayout( pane );
  layout->setContentsMargins( 0, 0, 0, 0 );
  layout->setSpacing( 8 );

  QFrame *card = new QFrame( pane );
  card->setObjectName( u"aiClaudeAccountCard"_s );
  QHBoxLayout *cardLayout = new QHBoxLayout( card );
  cardLayout->setContentsMargins( 12, 12, 12, 12 );
  cardLayout->setSpacing( 12 );

  mAvatarLabel = new QLabel( card );
  mAvatarLabel->setObjectName( u"aiClaudeAvatarLabel"_s );
  mAvatarLabel->setFixedSize( 40, 40 );
  mAvatarLabel->setAlignment( Qt::AlignCenter );
  QFont avatarFont = mAvatarLabel->font();
  avatarFont.setPointSize( avatarFont.pointSize() + 4 );
  avatarFont.setBold( true );
  mAvatarLabel->setFont( avatarFont );
  cardLayout->addWidget( mAvatarLabel );

  QVBoxLayout *identityLayout = new QVBoxLayout();
  identityLayout->setContentsMargins( 0, 0, 0, 0 );
  identityLayout->setSpacing( 2 );
  mConnectedTitleLabel = new QLabel( card );
  mConnectedTitleLabel->setObjectName( u"aiClaudeConnectedTitleLabel"_s );
  QFont titleFont = mConnectedTitleLabel->font();
  titleFont.setBold( true );
  mConnectedTitleLabel->setFont( titleFont );
  mConnectedDetailLabel = new QLabel( card );
  mConnectedDetailLabel->setObjectName( u"aiClaudeConnectedDetailLabel"_s );
  mConnectedDetailLabel->setProperty( "aiRole", u"rowDescription"_s );
  mConnectedDetailLabel->setWordWrap( true );
  mExpiryLabel = new QLabel( card );
  mExpiryLabel->setObjectName( u"aiClaudeExpiryLabel"_s );
  mExpiryLabel->setProperty( "aiRole", u"rowDescription"_s );
  identityLayout->addWidget( mConnectedTitleLabel );
  identityLayout->addWidget( mConnectedDetailLabel );
  identityLayout->addWidget( mExpiryLabel );
  cardLayout->addLayout( identityLayout, 1 );
  layout->addWidget( card );

  QWidget *buttonRow = new QWidget( pane );
  QHBoxLayout *buttonLayout = new QHBoxLayout( buttonRow );
  buttonLayout->setContentsMargins( 0, 0, 0, 0 );
  mDisconnectButton = new QPushButton( tr( "Disconnect" ), buttonRow );
  mDisconnectButton->setObjectName( u"aiClaudeDisconnectButton"_s );
  mReconnectButton = new QPushButton( tr( "Reconnect" ), buttonRow );
  mReconnectButton->setObjectName( u"aiClaudeReconnectButton"_s );
  buttonLayout->addWidget( mDisconnectButton );
  buttonLayout->addWidget( mReconnectButton );
  buttonLayout->addStretch( 1 );
  layout->addWidget( buttonRow );
  connect( mDisconnectButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::disconnectSubscription );
  connect( mReconnectButton, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::startConnect );
  return pane;
}

// --------------------------------------------------------------- accessors

QgsAiModelRouter::CredentialMode QgsAiClaudeConnectWidget::selectedCredentialMode() const
{
  return mModeSubscriptionButton->isChecked() ? QgsAiModelRouter::CredentialMode::OAuth : QgsAiModelRouter::CredentialMode::ApiKey;
}

QString QgsAiClaudeConnectWidget::modelText() const
{
  return mModelCombo->currentText().trimmed();
}

QString QgsAiClaudeConnectWidget::pendingApiKey() const
{
  return mApiKeyEdit->text().trimmed();
}

QString QgsAiClaudeConnectWidget::pendingManualToken() const
{
  return mManualTokenEdit->text().simplified().remove( u' ' );
}

bool QgsAiClaudeConnectWidget::isConnected() const
{
  return mModelRouter && mModelRouter->claudeSubscriptionInfo().connected;
}

// ------------------------------------------------------------------- state

void QgsAiClaudeConnectWidget::setMode( QgsAiModelRouter::CredentialMode mode )
{
  mModeStack->setCurrentIndex( mode == QgsAiModelRouter::CredentialMode::OAuth ? 0 : 1 );
}

void QgsAiClaudeConnectWidget::setSubscriptionState( SubscriptionState state )
{
  switch ( state )
  {
    case SubscriptionState::NotConnected:
      mSubscriptionStack->setCurrentIndex( 0 );
      break;
    case SubscriptionState::Connecting:
      mSubscriptionStack->setCurrentIndex( 1 );
      break;
    case SubscriptionState::Connected:
      updateConnectedCard();
      mSubscriptionStack->setCurrentIndex( 2 );
      break;
  }
}

void QgsAiClaudeConnectWidget::setBusy( bool busy )
{
  if ( mBusy == busy )
    return;
  mBusy = busy;
  mModeSubscriptionButton->setEnabled( !busy );
  mModeApiKeyButton->setEnabled( !busy );
  mAdvancedGroup->setEnabled( !busy );
  emit busyChanged( busy );
}

void QgsAiClaudeConnectWidget::setStatus( const QString &text, bool error )
{
  mStatusLabel->setText( text );
  mStatusLabel->setProperty( "status", error ? u"error"_s : QString() );
  repolish( mStatusLabel );
}

void QgsAiClaudeConnectWidget::repolish( QWidget *widget )
{
  if ( QStyle *style = widget->style() )
  {
    style->unpolish( widget );
    style->polish( widget );
  }
}

void QgsAiClaudeConnectWidget::refreshCliStatus()
{
  if ( mProbePending )
    return;
  mProbePending = true;
  mConnectButton->setEnabled( false );
  mCliStatusLabel->setText( tr( "Looking for the Claude Code CLI…" ) );
  mCli->probe();
}

void QgsAiClaudeConnectWidget::onProbeFinished( const QgsAiClaudeCodeCli::CliInfo &info )
{
  mProbePending = false;
  mCliInfo = info;
  updateCliStatusLabel();
  updateCliPathField();

  if ( mConnectAfterProbe )
  {
    // Connect was requested (e.g. from the chat dock) while the CLI was still being detected.
    mConnectAfterProbe = false;
    startConnect();
    return;
  }

  if ( mRefreshAccountAfterProbe && mModelRouter )
  {
    mRefreshAccountAfterProbe = false;
    QgsAiModelRouter::ClaudeSubscriptionInfo stored = mModelRouter->claudeSubscriptionInfo();
    if ( stored.connected && !stored.fromEnvironment )
    {
      stored.cliVersion = info.version;
      if ( info.loggedIn )
      {
        stored.email = info.email;
        stored.orgName = info.orgName;
        stored.subscriptionType = info.subscriptionType;
      }
      mModelRouter->updateClaudeSubscriptionInfo( stored );
      updateConnectedCard();
    }
  }
}

void QgsAiClaudeConnectWidget::updateCliStatusLabel()
{
  const bool found = mCliInfo.found();
  mInstallLink->setVisible( !found );
  mConnectButton->setEnabled( found && !mBusy );
  mConnectButton->setToolTip( found ? QString() : tr( "Install Claude Code or choose its executable first." ) );

  if ( !found )
  {
    mCliStatusLabel->setText( mCliInfo.error.isEmpty() ? tr( "Claude Code CLI not found." ) : mCliInfo.error );
    return;
  }

  QString text = mCliInfo.version.isEmpty() ? tr( "Claude Code found" ) : tr( "Claude Code %1 found" ).arg( mCliInfo.version );
  if ( mCliInfo.loggedIn )
  {
    const QString plan = subscriptionTypeLabel( mCliInfo.subscriptionType );
    if ( !mCliInfo.email.isEmpty() )
      text += plan.isEmpty() ? tr( " · signed in as %1" ).arg( mCliInfo.email ) : tr( " · signed in as %1 (%2)" ).arg( mCliInfo.email, plan );
    else
      text += tr( " · signed in" );
  }
  else
  {
    text += tr( " · not signed in yet (you will log in through your browser)" );
  }
  mCliStatusLabel->setText( text );
}

void QgsAiClaudeConnectWidget::updateCliPathField()
{
  QgsSettings settings;
  const QString override = settings.value( QgsAiClaudeCodeCli::cliPathSettingKey() ).toString().trimmed();
  mCliPathEdit->setText( override.isEmpty() ? mCliInfo.path : override );
  mCliPathEdit->setPlaceholderText( override.isEmpty() ? tr( "Auto-detected" ) : QString() );
  mCliPathResetButton->setEnabled( !override.isEmpty() );
}

void QgsAiClaudeConnectWidget::updateConnectedCard()
{
  if ( !mModelRouter )
    return;
  const QgsAiModelRouter::ClaudeSubscriptionInfo info = mModelRouter->claudeSubscriptionInfo();

  const QString initial = info.email.isEmpty() ? u"C"_s : info.email.left( 1 ).toUpper();
  mAvatarLabel->setText( initial );

  if ( info.fromEnvironment )
  {
    mConnectedTitleLabel->setText( tr( "Using CLAUDE_CODE_OAUTH_TOKEN from the environment" ) );
    mConnectedDetailLabel->setText( tr( "Unset the variable to manage the connection from here." ) );
    mExpiryLabel->clear();
    mExpiryLabel->setVisible( false );
    mDisconnectButton->setEnabled( false );
    mReconnectButton->setEnabled( mCliInfo.found() );
    return;
  }

  mConnectedTitleLabel->setText( tr( "Claude subscription connected" ) );
  QStringList details;
  if ( info.source == "manual"_L1 )
    details << tr( "Token pasted manually" );
  else if ( !info.cliVersion.isEmpty() )
    details << tr( "via Claude Code %1" ).arg( info.cliVersion );
  else
    details << tr( "via Claude Code" );
  if ( !info.email.isEmpty() )
  {
    const QString plan = subscriptionTypeLabel( info.subscriptionType );
    details << ( plan.isEmpty() ? info.email : u"%1 (%2)"_s.arg( info.email, plan ) );
  }
  mConnectedDetailLabel->setText( details.join( u" · "_s ) );

  const QDateTime expiresAt = info.expiresAt();
  mExpiryLabel->setVisible( true );
  QString expiryState;
  if ( !expiresAt.isValid() )
  {
    mExpiryLabel->setText( tr( "Token stored locally." ) );
  }
  else
  {
    const qint64 daysLeft = QDateTime::currentDateTimeUtc().daysTo( expiresAt );
    const QString date = QLocale().toString( expiresAt.toLocalTime().date(), QLocale::ShortFormat );
    if ( daysLeft < 0 )
    {
      expiryState = u"expired"_s;
      mExpiryLabel->setText( tr( "Token stored locally · expired on %1 — reconnect to keep using Claude." ).arg( date ) );
    }
    else if ( daysLeft <= EXPIRY_WARNING_DAYS )
    {
      expiryState = u"warning"_s;
      mExpiryLabel->setText( tr( "Token stored locally · expires on %1 (%n day(s) left)", nullptr, static_cast<int>( daysLeft ) ).arg( date ) );
    }
    else
    {
      mExpiryLabel->setText( tr( "Token stored locally · expires on %1" ).arg( date ) );
    }
  }
  mExpiryLabel->setProperty( "expiry", expiryState );
  repolish( mExpiryLabel );
  mDisconnectButton->setEnabled( !mBusy );
  mReconnectButton->setEnabled( mCliInfo.found() && !mBusy );
}

// ----------------------------------------------------------------- actions

void QgsAiClaudeConnectWidget::chooseExecutable()
{
  const QString path = QFileDialog::getOpenFileName( this, tr( "Choose the Claude Code executable" ), mCliInfo.found() ? mCliInfo.path : QDir::homePath() );
  if ( path.isEmpty() )
    return;
  QgsSettings settings;
  settings.setValue( QgsAiClaudeCodeCli::cliPathSettingKey(), path );
  updateCliPathField();
  refreshCliStatus();
}

void QgsAiClaudeConnectWidget::resetExecutable()
{
  QgsSettings settings;
  settings.remove( QgsAiClaudeCodeCli::cliPathSettingKey() );
  updateCliPathField();
  refreshCliStatus();
}

void QgsAiClaudeConnectWidget::startConnect()
{
  if ( !mModelRouter || mBusy )
    return;
  if ( mProbePending )
  {
    mConnectAfterProbe = true;
    mModeSubscriptionButton->setChecked( true );
    setStatus( tr( "Looking for the Claude Code CLI…" ) );
    return;
  }
  if ( !mCliInfo.found() )
  {
    setStatus( tr( "Claude Code CLI not found. Install it or choose the executable under Advanced." ), true );
    return;
  }

  mModeSubscriptionButton->setChecked( true );
  mConnectedBeforeSession = isConnected();
  mAuthorizationUrl = QUrl();
  mAuthCodeEdit->clear();
  mAuthCodeRow->setVisible( false );
  mOpenBrowserAgainButton->setEnabled( false );

  QString error;
  if ( !mCli->startSetupToken( mCliInfo.path, &error ) )
  {
    setStatus( error.isEmpty() ? tr( "Unable to start Claude Code." ) : error, true );
    mAdvancedGroup->setCollapsed( false );
    return;
  }

  mSessionActive = true;
  setBusy( true );
  setStatus( QString() );
  mProgressLabel->setText( mCli->supportsInteractiveInput() ? tr( "Starting Claude Code…" ) : tr( "Finish the login in the console window that just opened. Strata picks up the token automatically." ) );
  setSubscriptionState( SubscriptionState::Connecting );
}

void QgsAiClaudeConnectWidget::cancelConnect()
{
  if ( !mSessionActive )
    return;
  mProgressLabel->setText( tr( "Cancelling…" ) );
  mCli->cancel();
}

void QgsAiClaudeConnectWidget::disconnectSubscription()
{
  if ( !mModelRouter || mBusy )
    return;
  mModelRouter->disconnectClaudeSubscription();
  setSubscriptionState( SubscriptionState::NotConnected );
  setStatus( tr( "Disconnected. Claude will use an API key if one is configured." ) );
  emit connectionStateChanged();
}

void QgsAiClaudeConnectWidget::openBrowserAgain()
{
  if ( !mAuthorizationUrl.isValid() )
    return;
  QDesktopServices::openUrl( mAuthorizationUrl );
}

void QgsAiClaudeConnectWidget::submitCode()
{
  const QString code = mAuthCodeEdit->text().trimmed();
  if ( code.isEmpty() )
    return;
  mCli->submitAuthorizationCode( code );
  mAuthCodeEdit->clear();
  mAuthCodeRow->setVisible( false );
  mProgressLabel->setText( tr( "Completing the login…" ) );
}

void QgsAiClaudeConnectWidget::onBrowserUrlDetected( const QUrl &url )
{
  mAuthorizationUrl = url;
  mOpenBrowserAgainButton->setEnabled( true );
  if ( mCli->supportsInteractiveInput() )
    mProgressLabel->setText( tr( "Approve the login in your browser. If it did not open, use “Open browser again”." ) );
}

void QgsAiClaudeConnectWidget::onCodeRequested()
{
  if ( !mCli->supportsInteractiveInput() )
    return;
  mProgressLabel->setText( tr( "Paste the authorization code shown in the browser." ) );
  mAuthCodeRow->setVisible( true );
  mAuthCodeEdit->setFocus();
}

void QgsAiClaudeConnectWidget::onTokenReceived( const QString &token )
{
  if ( !mModelRouter )
    return;
  QgsAiModelRouter::ClaudeSubscriptionInfo info;
  info.source = u"claude-code-cli"_s;
  info.cliVersion = mCliInfo.version;
  if ( mCliInfo.loggedIn )
  {
    info.email = mCliInfo.email;
    info.orgName = mCliInfo.orgName;
    info.subscriptionType = mCliInfo.subscriptionType;
  }
  QString error;
  if ( !mModelRouter->connectClaudeSubscription( token, info, &error ) )
  {
    setStatus( error, true );
    setSubscriptionState( mConnectedBeforeSession ? SubscriptionState::Connected : SubscriptionState::NotConnected );
    return;
  }
  setSubscriptionState( SubscriptionState::Connected );
  setStatus( tr( "Connected. Claude is ready to use." ) );
  emit connectionStateChanged();
  // The account shown in the card comes from the CLI's own login: refresh it now
  // that the browser flow completed.
  mRefreshAccountAfterProbe = true;
  refreshCliStatus();
}

void QgsAiClaudeConnectWidget::onSessionFailed( const QString &message )
{
  setStatus( message, true );
  setSubscriptionState( mConnectedBeforeSession ? SubscriptionState::Connected : SubscriptionState::NotConnected );
  // Give the user the manual escape hatch right away.
  mAdvancedGroup->setCollapsed( false );
}

void QgsAiClaudeConnectWidget::onSessionFinished( int exitCode )
{
  Q_UNUSED( exitCode )
  mSessionActive = false;
  setBusy( false );
  if ( mSubscriptionStack->currentIndex() == 1 )
  {
    // Cancelled (or finished without a token and no failure reported): restore the previous pane.
    setSubscriptionState( isConnected() ? SubscriptionState::Connected : SubscriptionState::NotConnected );
    if ( mStatusLabel->text().isEmpty() )
      setStatus( tr( "Connection cancelled." ) );
  }
  updateConnectedCard();
  updateCliStatusLabel();
}
