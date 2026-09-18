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
#include "qgsaiclaudecodecli.h"
#include "qgsaimodelrouter.h"

#include <QPointer>
#include <QWidget>

class QgsCollapsibleGroupBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

/**
 * Claude provider section of the AI settings: a "Claude subscription" /
 * "Anthropic API key" toggle, and — for the subscription — a one-click
 * "Connect Claude Code" flow that runs `claude setup-token` for the user,
 * follows the browser approval and stores the token. Connect/disconnect
 * apply immediately; the mode, model, API key and manually pasted token are
 * persisted by the host dialog on accept.
 */
class APP_EXPORT QgsAiClaudeConnectWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit QgsAiClaudeConnectWidget( QgsAiModelRouter *modelRouter, QWidget *parent = nullptr );
    ~QgsAiClaudeConnectWidget() override;

    //! Credential mode selected with the toggle (not yet persisted).
    QgsAiModelRouter::CredentialMode selectedCredentialMode() const;
    //! Model id typed or picked in the model field.
    QString modelText() const;
    //! API key typed in the API-key pane, empty when untouched.
    QString pendingApiKey() const;
    //! Token pasted in the Advanced group (whitespace scrubbed), empty when untouched.
    QString pendingManualToken() const;
    //! True while `claude setup-token` runs; the host dialog blocks closing meanwhile.
    bool isBusy() const { return mBusy; }
    //! True when a subscription token is stored (or provided by the environment).
    bool isConnected() const;

  public slots:
    //! Starts (or restarts) the Claude Code connection flow.
    void startConnect();
    void cancelConnect();
    //! Forgets the stored token and returns Claude to API-key mode.
    void disconnectSubscription();
    //! Re-detects the CLI and refreshes the status line.
    void refreshCliStatus();

  signals:
    //! A token was stored or removed; the chat dock should rebuild its model menu.
    void connectionStateChanged();
    void busyChanged( bool busy );

  private:
    enum class SubscriptionState
    {
      NotConnected,
      Connecting,
      Connected
    };

    QWidget *buildSubscriptionPane();
    QWidget *buildApiKeyPane();
    QWidget *buildNotConnectedPane( QWidget *parent );
    QWidget *buildConnectingPane( QWidget *parent );
    QWidget *buildConnectedPane( QWidget *parent );
    void setMode( QgsAiModelRouter::CredentialMode mode );
    void setSubscriptionState( SubscriptionState state );
    void setBusy( bool busy );
    void setStatus( const QString &text, bool error = false );
    void updateCliStatusLabel();
    void updateConnectedCard();
    void updateCliPathField();
    void chooseExecutable();
    void resetExecutable();
    void openBrowserAgain();
    void submitCode();
    void onProbeFinished( const QgsAiClaudeCodeCli::CliInfo &info );
    void onBrowserUrlDetected( const QUrl &url );
    void onCodeRequested();
    void onTokenReceived( const QString &token );
    void onSessionFailed( const QString &message );
    void onSessionFinished( int exitCode );
    static void repolish( QWidget *widget );

    QPointer<QgsAiModelRouter> mModelRouter;
    QgsAiClaudeCodeCli *mCli = nullptr;
    QgsAiClaudeCodeCli::CliInfo mCliInfo;
    bool mProbePending = false;
    bool mConnectAfterProbe = false;
    bool mRefreshAccountAfterProbe = false;
    bool mBusy = false;
    bool mSessionActive = false;
    bool mConnectedBeforeSession = false;
    QUrl mAuthorizationUrl;

    QPushButton *mModeSubscriptionButton = nullptr;
    QPushButton *mModeApiKeyButton = nullptr;
    QStackedWidget *mModeStack = nullptr;
    QComboBox *mModelCombo = nullptr;
    QLineEdit *mApiKeyEdit = nullptr;

    QStackedWidget *mSubscriptionStack = nullptr;
    QLabel *mCliStatusLabel = nullptr;
    QLabel *mInstallLink = nullptr;
    QPushButton *mChooseExecutableButton = nullptr;
    QPushButton *mRecheckButton = nullptr;
    QPushButton *mConnectButton = nullptr;

    QLabel *mProgressLabel = nullptr;
    QPushButton *mOpenBrowserAgainButton = nullptr;
    QWidget *mAuthCodeRow = nullptr;
    QLineEdit *mAuthCodeEdit = nullptr;
    QPushButton *mSubmitCodeButton = nullptr;
    QPushButton *mCancelButton = nullptr;

    QLabel *mAvatarLabel = nullptr;
    QLabel *mConnectedTitleLabel = nullptr;
    QLabel *mConnectedDetailLabel = nullptr;
    QLabel *mExpiryLabel = nullptr;
    QPushButton *mDisconnectButton = nullptr;
    QPushButton *mReconnectButton = nullptr;

    QLabel *mStatusLabel = nullptr;
    QgsCollapsibleGroupBox *mAdvancedGroup = nullptr;
    QLineEdit *mManualTokenEdit = nullptr;
    QLineEdit *mCliPathEdit = nullptr;
    QPushButton *mCliPathBrowseButton = nullptr;
    QPushButton *mCliPathResetButton = nullptr;
};

#endif // QGSAICLAUDECONNECTWIDGET_H
