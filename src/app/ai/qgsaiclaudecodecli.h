/***************************************************************************
    qgsaiclaudecodecli.h
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

#ifndef QGSAICLAUDECODECLI_H
#define QGSAICLAUDECODECLI_H

#include <functional>

#include "qgis_app.h"

#include <QByteArray>
#include <QMetaType>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QUrl>

class QgsAiPtySession;
class QProcess;
class QTimer;

/**
 * Bridge to the official Claude Code CLI (`claude`).
 *
 * Strata never talks to Anthropic's OAuth endpoints and never reads the CLI's
 * own credential store: it only orchestrates the CLI.
 *
 * - locate()/probe(): find the executable (Finder-launched apps have a minimal
 *   PATH) and read `--version` / `auth status --json` without a TTY.
 * - startSetupToken(): run `claude setup-token` under a pseudo-terminal (the
 *   CLI is an Ink TUI that requires one), follow the browser approval and
 *   capture the long-lived `sk-ant-oat01-…` token it prints. On Windows a
 *   visible console is opened and its output is tee'd to a temp log instead.
 *
 * The pure parsing helpers are static so they can be unit-tested with captured
 * terminal output.
 */
class APP_EXPORT QgsAiClaudeCodeCli : public QObject
{
    Q_OBJECT

  public:
    struct CliInfo
    {
        QString path;
        QString version;
        bool loggedIn = false;
        QString email;
        QString orgName;
        QString subscriptionType;
        QString authMethod;
        QString error;
        bool found() const { return !path.isEmpty(); }
    };

    enum class State
    {
      Idle,
      Starting,
      WaitingForBrowser,
      WaitingForCode,
      Finishing
    };
    Q_ENUM( State )

    explicit QgsAiClaudeCodeCli( QObject *parent = nullptr );
    ~QgsAiClaudeCodeCli() override;

    // ---- discovery ----

    //! QgsSettings key holding the user's explicit executable override.
    static QString cliPathSettingKey();
    //! Well-known install locations, checked in order after the override.
    static QStringList candidatePaths();
    //! Extra directories searched for `claude` (and prepended to the child PATH).
    static QStringList extraSearchDirectories();

    /**
     * Returns the absolute path of the CLI: STRATA_CLAUDE_CLI env → settings
     * override → candidatePaths() → PATH → extraSearchDirectories(). Empty when
     * nothing is found; \a reason then explains why.
     */
    static QString locate( QString *reason = nullptr );

    //! Environment for spawning the CLI: augmented PATH, TERM, no ambient Anthropic credentials.
    static QProcessEnvironment childEnvironment( const QString &cliPath );

    //! True when the fully in-app pty flow is available (POSIX).
    static bool interactiveSessionSupported();
    //! Official installation instructions.
    static QUrl installDocsUrl();

    // ---- probe (async, no TTY) ----

    //! Runs `--version` then `auth status --json`; emits probeFinished().
    void probe( const QString &cliPath = QString() );
    bool isProbing() const { return mProbeProcess != nullptr; }

    // ---- setup-token session (async) ----

    bool startSetupToken( const QString &cliPath = QString(), QString *errorMessage = nullptr );
    //! Types the authorization code (plus Enter) into the CLI when it asked for one.
    void submitAuthorizationCode( const QString &code );
    void cancel();
    bool isRunning() const;
    State state() const { return mState; }
    //! Authorization URL printed by the CLI, when detected.
    QUrl authorizationUrl() const { return mAuthorizationUrl; }
    //! ANSI-stripped terminal transcript with tokens redacted.
    QString transcript() const;
    //! Last non-empty transcript lines, redacted (for error messages).
    QString transcriptTail( int lines = 6 ) const;
    void setTimeoutMs( int ms ) { mTimeoutMs = ms; }
    int timeoutMs() const { return mTimeoutMs; }
    //! False for the Windows console fallback (the user types in the console instead).
    bool supportsInteractiveInput() const;
    QString cliPath() const { return mCliPath; }

    // ---- pure helpers ----

    static QString stripAnsi( const QString &raw );
    //! Longest `sk-ant-oat01-…` token in \a text, re-joining lines hard-wrapped by the terminal.
    static QString extractSetupToken( const QString &text );
    static QUrl extractAuthorizeUrl( const QString &text );
    static bool detectsCodePrompt( const QString &text );
    static QString parseVersion( const QByteArray &output );
    static bool parseAuthStatus( const QByteArray &json, CliInfo &info );
    static QString redactTokens( const QString &text );

  signals:
    void probeFinished( const QgsAiClaudeCodeCli::CliInfo &info );
    void stateChanged( QgsAiClaudeCodeCli::State state );
    void browserUrlDetected( const QUrl &url );
    void authorizationCodeRequested();
    void tokenReceived( const QString &token );
    void failed( const QString &message );
    //! Emitted once per session after tokenReceived()/failed(), or after cancel().
    void finished( int exitCode );
    //! Redacted, ANSI-stripped output delta.
    void outputReceived( const QString &cleanChunk );

  private:
    void runProbeStep( const QStringList &arguments, std::function<void( int, const QByteArray & )> onDone );
    void finishProbe();
    void appendRawOutput( const QByteArray &bytes );
    void scheduleTokenSettle();
    void finalizeToken();
    void handleSessionFinished( int exitCode, bool crashed );
    void setState( State state );
    void resetSession();
    void onTimeout();
    bool startConsoleFallback( const QString &cliPath, QString *errorMessage );
    void pollFallbackLog();
    void stopFallback( bool deleteLog );

    QgsAiPtySession *mPty = nullptr;
    QProcess *mProbeProcess = nullptr;
    QTimer *mProbeTimeoutTimer = nullptr;
    QTimer *mTimeoutTimer = nullptr;
    QTimer *mTokenSettleTimer = nullptr;
    QTimer *mFallbackPollTimer = nullptr;
    QString mFallbackLogPath;
    qint64 mFallbackReadOffset = 0;
    bool mFallbackActive = false;

    QString mCliPath;
    CliInfo mProbeInfo;
    QByteArray mRawOutput;
    QString mTranscript;
    QString mToken;
    QUrl mAuthorizationUrl;
    State mState = State::Idle;
    int mTimeoutMs = 5 * 60 * 1000;
    bool mTokenEmitted = false;
    bool mCodePromptEmitted = false;
    bool mCancelled = false;
    QString mPendingFailure;
};

Q_DECLARE_METATYPE( QgsAiClaudeCodeCli::CliInfo )

#endif // QGSAICLAUDECODECLI_H
