/***************************************************************************
    qgsaiagentsessionmanager.h
    ---------------------
    begin                : April 2026
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

#ifndef QGSAIAGENTSESSIONMANAGER_H
#define QGSAIAGENTSESSIONMANAGER_H

#include <functional>

#include "ai/index/qgsaiworkspaceindex.h"
#include "qgis_app.h"
#include "qgsaiagentpolicy.h"
#include "qgsaichathistorystore.h"
#include "qgsaimodelrouter.h"
#include "qgsaimodels.h"
#include "qgsaitool.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsrectangle.h"

#include <QEventLoop>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>

using namespace Qt::StringLiterals;

class QgsAiFileContextProvider;
class QgsAiReviewPatchEngine;
class QgsAiToolRegistry;
class QgsTask;
class QTimer;

/**
 * What the user is looking at in the map: the model gets it with every message, so "the
 * selected features" or "this area" need no layer name or coordinates.
 */
struct APP_EXPORT QgsAiMapContext
{
    //! Layer selected in the Layers panel, empty for none.
    QString activeLayerId;
    QgsRectangle extent;
    QgsCoordinateReferenceSystem crs;
    double scale = 0;
};

struct APP_EXPORT QgsAiChatContextFile
{
    QString filePath;
    QString selectedText;
    bool allowExternal = false;
};

/**
 * User-configurable behavior for the agent: rules, skills and a master toggle that
 * gates whether the agent is allowed to perform custom actions (tool use). The
 * settings are persisted in QgsSettings and loaded once at construction time.
 */
struct APP_EXPORT QgsAiAgentBehaviorSettings
{
    static constexpr int DEFAULT_TOOL_CALL_PAUSE_LIMIT = 20;
    static constexpr int MIN_TOOL_CALL_PAUSE_LIMIT = 1;
    static constexpr int MAX_TOOL_CALL_PAUSE_LIMIT = 50;
    static constexpr int DEFAULT_TOTAL_TOOL_CALL_LIMIT = 48;
    static constexpr int MIN_TOTAL_TOOL_CALL_LIMIT = 6;
    static constexpr int MAX_TOTAL_TOOL_CALL_LIMIT = 200;
    static constexpr int MIN_RUN_PYTHON_TIMEOUT_SECONDS = 10;
    static constexpr int MAX_RUN_PYTHON_TIMEOUT_SECONDS = 3600;
    static constexpr int DEFAULT_RUN_PYTHON_TIMEOUT_SECONDS = 120;

    //! Master toggle. When false the agent must not use any custom tool/action. On by default: the first prompt acts.
    bool allowCustomActions = true;
    //! Inline rules text injected into the system prompt.
    QString rulesText;
    //! Inline skills text injected into the system prompt.
    QString skillsText;
    //! When true, also load .md/.txt files from rulesPath inside the workspace.
    bool loadWorkspaceRules = true;
    //! When true, also load .md/.txt files from skillsPath inside the workspace.
    bool loadWorkspaceSkills = true;
    //! Workspace-relative directory for rules files. Defaults to ".strata/rules".
    QString rulesPath = u".strata/rules"_s;
    //! Workspace-relative directory for skills files. Defaults to ".strata/skills".
    QString skillsPath = u".strata/skills"_s;
    //! Tool-use rounds allowed before pausing for explicit user continuation.
    int maxToolIterationsPerTurn = DEFAULT_TOOL_CALL_PAUSE_LIMIT;
    //! Cumulative tool-use rounds allowed for one user turn, across Continue blocks.
    int maxTotalToolIterationsPerTurn = DEFAULT_TOTAL_TOOL_CALL_LIMIT;
    //! Continue tool blocks automatically until the cumulative cap is reached.
    bool autoContinueToolBlocks = false;
    //! When true, one low-risk run_python approval grants subsequent low-risk Python runs for this app session.
    bool rememberPythonApprovalsForSession = false;
    //! Seconds before run_python is interrupted. Time spent in patched Processing.execute is excluded.
    int runPythonTimeoutSeconds = DEFAULT_RUN_PYTHON_TIMEOUT_SECONDS;
};

class APP_EXPORT QgsAiAgentSessionManager : public QObject
{
    Q_OBJECT

  public:
    explicit QgsAiAgentSessionManager( QgsAiModelRouter *router, QgsAiFileContextProvider *contextProvider, QgsAiReviewPatchEngine *reviewEngine, QObject *parent = nullptr );
    ~QgsAiAgentSessionManager() override;

    QStringList availableAgents() const;
    QString activeAgent() const { return mActiveAgent; }
    void setActiveAgent( const QString &agentName );

    //! Stores the active agent as the one to start with next time (the user picked it).
    void rememberActiveAgent() const;

    //! Where the map context comes from (the main window's canvas).
    void setMapContextProvider( const std::function<QgsAiMapContext()> &provider ) { mMapContextProvider = provider; }

    /**
     * Lines about the map sent with each message: view, active layer and its selection. Empty
     * without a provider or when the user left the map context out.
     */
    QString mapContextText() const;

    //! One short line for the chat ("Parcels · 12 selected · 1:5,000"), empty when there is nothing to say.
    QString mapContextSummary() const;

    //! Answers toolApprovalRequested(): the tool call runs when \a approved.
    void resolveToolApproval( const QString &callId, bool approved );

    //! The user may leave the map context out of the next messages.
    void setMapContextIncluded( bool included ) { mMapContextIncluded = included; }
    bool isMapContextIncluded() const { return mMapContextIncluded; }

    //! Settings key of the agent Strata starts with.
    static QString startAgentSettingsKey() { return u"strata/agent/mode"_s; }

    QList<QgsAiChatMessage> history() const { return mHistory; }
    void clearHistory();
    bool updateMessageMetadata( const QString &messageId, const QVariantMap &metadata );

    //! TRUE if the tool result \a message carries a rollback token and was not undone yet.
    static bool toolMessageCanBeUndone( const QgsAiChatMessage &message );

    /**
     * Undoes the tool call behind the tool result message \a toolMessageId directly, without the
     * model, with the rollback token the tool returned. The message is marked undone and a note
     * tells the model. Not while a request runs. Returns FALSE with \a error when it fails.
     */
    bool undoToolCall( const QString &toolMessageId, QString *error = nullptr );

    //! Tool result messages of the turn that contains \a messageId which can still be undone, newest first.
    QStringList undoableToolCallsInTurn( const QString &messageId ) const;

    /**
     * Undoes every tool call of the turn containing \a messageId that can be undone, newest first.
     * Returns how many were undone; \a failures lists the ones that could not be.
     */
    int undoTurn( const QString &messageId, QStringList *failures = nullptr );

    /**
     * Sends the last user message again and drops the answer after it, e.g. after an error.
     * Changes of the dropped answer are undone first; returns FALSE with \a error when a
     * request runs, nothing can be retried, or a change cannot be undone.
     */
    bool retryLastTurn( QString *error = nullptr );

    /**
     * Replaces the user message \a messageId with \a text and sends it, dropping everything
     * after it. Changes made since are undone first, as for retryLastTurn().
     */
    bool editAndResend( const QString &messageId, const QString &text, QString *error = nullptr );

    /**
     * Sets the persistent chat history store. When set, every message appended
     * to the in-memory history is also written to SQLite when the current
     * history scope is persistent. Pass nullptr to disable persistence.
     * Ownership is not transferred.
     */
    void setHistoryStore( QgsAiChatHistoryStore *store ) { mHistoryStore = store; }
    QgsAiChatHistoryStore *historyStore() const { return mHistoryStore; }

    //! Returns the persisted sessions for the current history scope ordered by most recent first.
    QList<QgsAiChatHistoryStore::SessionInfo> listSessions() const;
    //! Returns the active session id, or an empty string if no session has been created yet.
    QString activeSessionId() const { return mActiveSessionId; }
    //! Loads \a sessionId from the store, replacing the in-memory history. Emits historyReplaced().
    void loadSession( const QString &sessionId );

    /**
     * Closes the current session and starts a fresh empty one (the new session row
     * is created lazily on the first user message). Emits historyReplaced().
     */
    void startNewSession();
    //! Renames the active session in the persistent store and emits sessionListChanged().
    void renameActiveSession( const QString &title );
    //! Renames \a sessionId in the persistent store. Emits sessionListChanged().
    void renameSession( const QString &sessionId, const QString &title );
    //! Removes \a sessionId from the store. If it was the active one, starts a new session.
    void deleteSession( const QString &sessionId );

    //! Returns a stable per-project chat history scope key for \a projectFilePath.
    static QString chatHistoryScopeKeyForProjectFile( const QString &projectFilePath );

    //! Applies a per-project chat history scope. Empty scope means unsaved project and disables persistence.
    void setProjectChatHistoryScopeKey( const QString &scopeKey );

    //! Resets the current chat and switches to the unsaved-project memory-only history scope.
    void resetProjectChatHistoryScope();

    //! Returns true when the current chat history scope can persist to SQLite.
    bool hasPersistentChatHistoryScope() const;

    //! Returns the current explicit chat history scope key, or an empty string for unsaved/legacy scopes.
    QString chatHistoryScopeKey() const;

    //! Extracts a fenced ```strata_agent_plan JSON block from assistant text, if present.
    static QJsonObject extractAgentPlanJson( const QString &text );

    //! Validates the agent V2 plan JSON contract used by Plan mode and executable runs.
    static bool validateAgentPlanJson( const QJsonObject &plan, QString *errorMessage = nullptr );

    /**
     * Returns the subset of \a requestedTools (tool names referenced by a plan's steps)
     * which cannot be resolved against \a allowedTools.
     *
     * Planner models sometimes emit near-miss names ("add_layer" for
     * "add_layer_from_file", "run_processing" for "run_processing_algorithm") or
     * pseudo-tools describing user interaction ("optional_user_input"): these are
     * normalized or ignored instead of blocking plan execution. Only names with no
     * plausible match among the allowed tools are returned.
     */
    static QStringList unresolvedPlanTools( const QStringList &requestedTools, const QStringList &allowedTools );

    //! Runtime metadata-only memory for recent agent decisions/tool events.
    QVariantList agentMemoryEvents() const { return mAgentMemory; }

    void sendUserMessage( const QString &text, const QString &filePath = QString(), const QString &selectedText = QString() );
    void sendUserMessage( const QString &text, const QList<QgsAiChatContextFile> &contextFiles );
    bool continueAfterToolLimit( const QString &messageId );
    void cancelActiveRequest();
    bool hasActiveRequest() const { return !mActiveRequestId.isEmpty() || mAwaitingAgentRunApproval || !mRetrievalTask.isNull(); }

    /**
     * Appends \a message to the in-memory (and persistent) history without starting a model
     * request. Used for local notices and for seeding transcript state in tests.
     */
    void appendHistoryMessage( const QgsAiChatMessage &message );

    //! Appends a local assistant notice (no model request).
    void appendAssistantNotice( const QString &text );
    QStringList projectFileCandidates( const QString &query, int maxResults = 25 ) const;
    QString resolveProjectFile( const QString &filePath ) const;
    QString workspaceRoot() const;
    void setWorkspaceRoot( const QString &workspaceRoot );

    //! Returns the file context provider backing this session, used by QgsAiRulesSkillsStore.
    QgsAiFileContextProvider *fileContextProvider() const { return mContextProvider; }

    void setToolRegistry( QgsAiToolRegistry *registry );
    QgsAiToolRegistry *toolRegistry() const { return mToolRegistry; }
    //! Returns tool names allowed by the current local mode and managed policy.
    QStringList allowedToolNamesForActiveAgent() const;

    /**
     * Sets the workspace RAG index used to retrieve relevant chunks for each
     * user message before contacting the model. Pass nullptr to disable retrieval.
     */
    void setWorkspaceIndex( QgsAiWorkspaceIndex *index ) { mWorkspaceIndex = index; }
    QgsAiWorkspaceIndex *workspaceIndex() const { return mWorkspaceIndex; }

    //! Maximum number of chunks injected into the system prompt for a single turn.
    static constexpr int RETRIEVAL_TOP_K = 8;
    //! Hard byte cap for the "Retrieved context" block appended to the system prompt, every turn.
    static constexpr int RETRIEVAL_BYTE_CAP = 16 * 1024;
    //! Retrieved chunks scoring more than this below the best one are left out.
    static constexpr float RETRIEVAL_SCORE_SPREAD = 0.1f;

    /**
     * Keeps the \a hits (sorted best first) within \a spread of the best score: when a chunk
     * matches the question well, weaker ones are noise that costs tokens every turn.
     */
    static QList<QgsAiWorkspaceIndex::Chunk> filterRetrievedChunks( const QList<QgsAiWorkspaceIndex::Chunk> &hits, float spread = RETRIEVAL_SCORE_SPREAD );

    /**
     * Renders \a chunks as a textual block ready to be appended to the system prompt.
     * Truncates with a marker if the total size exceeds \a byteCap. Public for
     * unit-testing the formatting in isolation.
     */
    static QString formatRetrievedContext( const QList<QgsAiWorkspaceIndex::Chunk> &chunks, int byteCap = RETRIEVAL_BYTE_CAP );

    /**
     * Overload taking the layer-WKT privacy setting explicitly, so the rendering can
     * run on a worker thread with the setting captured on the main thread.
     */
    static QString formatRetrievedContext( const QList<QgsAiWorkspaceIndex::Chunk> &chunks, int byteCap, bool includeLayerWkt );

    /**
     * Wraps untrusted content (RAG chunks, file snippets, free-text tool output)
     * in an `<untrusted-data source="…">` block, neutralizing any nested wrapper
     * markers so the content cannot escape the block. Public for unit testing.
     */
    static QString wrapUntrusted( const QString &sourceLabel, const QString &text );

    //! Flattens an untrusted label (layer/file name) to a single safe line for the wrapper attribute.
    static QString sanitizeUntrustedLabel( const QString &label );

    /**
     * Returns the current agent behavior settings (rules, skills, custom actions toggle).
     * The values are kept in sync with QgsSettings.
     */
    QgsAiAgentBehaviorSettings agentBehaviorSettings() const { return mBehaviorSettings; }

    /**
     * Persists \a settings in QgsSettings and propagates the master toggle to the
     * model router so subsequent requests reflect the new tool-use policy.
     */
    void setAgentBehaviorSettings( const QgsAiAgentBehaviorSettings &settings );

    //! Applies the managed Strata Cloud policy fetched from `/v1/agents/policy`.
    void setManagedAgentPolicy( const QgsAiManagedAgentPolicy &policy );
    QgsAiManagedAgentPolicy managedAgentPolicy() const { return mManagedAgentPolicy; }

    /**
     * Returns the rules text combined from inline settings and per-file workspace
     * rules. Rules with alwaysApply=true are inlined in full; others contribute only
     * a name/description reference the agent can expand via the read_file tool.
     */
    QString collectRulesContent() const;
    /**
     * Returns the skills text combined from inline settings and a compact
     * name/description index of per-file workspace skills (progressive disclosure —
     * the agent loads a skill's full SKILL.md body on demand via the read_file tool).
     */
    QString collectSkillsContent() const;

    /**
     * Returns the cumulative token/cost accounting for the current session,
     * summed across every model response (including tool-call rounds).
     */
    QgsAiUsage sessionUsage() const { return mSessionUsage; }

    //! Rough token budget for the conversation history sent to the provider (excludes system prompt).
    static constexpr int HISTORY_TOKEN_BUDGET = 32768;

    /**
     * Returns only the explicitly selected provider when usable. An empty list
     * requires user action; another provider is never used as a paid fallback.
     */
    QList<QgsAiModelRouter::Provider> providerFallbackOrder() const;

  signals:
    void messageAdded( const QgsAiChatMessage &message );
    void proposalCreated( const QString &proposalId );
    void responseChunkReceived( const QString &chunk );
    void requestStateChanged( const QString &state, const QString &detail );
    void requestRunningChanged( bool running );

    //! A tool call starts running, after any approval.
    void toolStarted( const QString &callId, const QString &toolName, const QVariantMap &args );
    //! Progress of the running tool call, 0 to 100, with what it is doing.
    void toolProgress( const QString &callId, double percent, const QString &label );
    //! The tool call ended; its result message follows through messageAdded().
    void toolFinished( const QString &callId, bool success, qint64 elapsedMs );
    //! Tool calls were undone from the chat; the transcript should render again.
    void toolCallsUndone();

    /**
     * A tool call waits for the user's approval; answer with resolveToolApproval(). While
     * nothing is connected, a message box asks instead.
     */
    void toolApprovalRequested( const QString &callId, const QString &toolName, const QVariantMap &args, const QString &riskLevel );

    /**
     * Emitted whenever the cumulative per-session token/cost accounting changes:
     * after every model response carrying usage, and with an empty total when a
     * new session starts (runtime accumulation only, not persisted).
     */
    void sessionUsageChanged( const QgsAiUsage &total );

    /**
     * Emitted after loadSession() / startNewSession(). The UI should clear the
     * transcript and re-render from history().
     */
    void historyReplaced();

    /**
     * Emitted whenever a session is created, renamed or deleted. The UI should
     * rebuild its history list on the next open.
     */
    void sessionListChanged();

  private:
    void startProviderAttempt( QgsAiModelRouter::Provider provider );
    bool needsManagedTaskApproval( QgsAiModelRouter::Provider provider ) const;
    void createManagedAgentRun();
    void approveManagedAgentRun();
    void completeManagedAgentRun();
    void sendAgentSessionHeartbeat();
    void closeDesktopAgentSession();
    QString planApiBase() const;
    QString actionableError( const QString &providerName, const QString &errorMessage, int httpStatus ) const;
    QgsAiChatMessage buildAssistantMessage( const QString &text ) const;
    QgsAiChatMessage buildAssistantToolUseMessage( const QString &text, const QList<QgsAiToolCall> &calls ) const;
    QgsAiChatMessage buildToolResultMessage( const QgsAiToolCall &call, const QgsAiToolResult &result ) const;
    QString buildContextSummary( const QList<QgsAiChatContextFile> &contextFiles, bool &contextBlocked ) const;
    bool tryBuildPatchProposal( const QString &text, QgsAiPatchProposal &proposal ) const;
    QString buildSystemPrompt( const QString &extraContext = QString() ) const;
    QString formatAgentMemoryForPrompt() const;

    /**
     * Runs workspace-index retrieval for the last user message on a background task,
     * caches the resulting context block, then dispatches \a firstProvider. When
     * retrieval is not applicable (no index, unavailable provider, empty history)
     * the dispatch happens synchronously with an empty cache, exactly like today.
     */
    void beginRetrievalThenDispatch( QgsAiModelRouter::Provider firstProvider );
    QStringList allowedToolsForActiveAgent() const;
    bool isToolAllowedForActiveAgent( const QString &toolName ) const;
    void refreshRouterToolPolicy();
    void syncRunPythonApprovalSettings();
    //! Per-profile folder where Processing Toolbox picks up user scripts.
    static QString processingScriptsFolder();
    QList<QgsAiChatMessage> trimHistoryByTokenBudget( int budgetTokens ) const;
    QList<QgsAiChatMessage> buildOutgoingMessages() const;
    void onToolCallsRequested( const QString &requestId, const QString &providerName, const QString &assistantText, const QList<QgsAiToolCall> &calls );
    void rememberAgentEvent( const QString &event, const QVariantMap &metadata );
    //! Undoes one tool call and marks its message; the caller adds the note for the model.
    bool undoToolCallWithoutNote( const QString &toolMessageId, QString *error, QString *toolName );
    //! Tells the model which tool calls the user undid.
    void recordUndoNote( const QStringList &toolNames );
    //! Asks the user whether \a call may run: in the chat when it is connected, otherwise with a message box.
    bool askToolApproval( const QgsAiToolCall &call, QgsAiToolRiskLevel risk );
    //! Undoes the changes of the history from \a index on, then drops those messages.
    bool dropHistoryFrom( int index, QString *error );

    void loadPersistedBehaviorSettings();
    void persistBehaviorSettings() const;
    QString readWorkspaceTextFiles( const QString &relativeDir ) const;
    //! Renders enabled rules from the per-file store: full body when alwaysApply, a reference line otherwise.
    QString formatRulesFromStore( const QString &relativeDir ) const;
    //! Renders a compact name/description index of enabled skills from the per-file store.
    QString formatSkillsFromStore( const QString &relativeDir ) const;

    /**
     * Appends \a message to mHistory, persists it (if a store is configured and
     * a session is active), and emits messageAdded().
     */
    void recordHistoryMessage( const QgsAiChatMessage &message );

    /**
     * Ensures there is an active session id. If none, creates a new one using
     * \a firstUserText to derive the title.
     */
    void ensureActiveSession( const QString &firstUserText );
    //! Trims the input to a 50-char single-line title for the session list.
    static QString deriveSessionTitle( const QString &text );
    void resetCurrentSessionState( bool emitHistorySignal );
    void persistCurrentHistoryToStore();

    QgsAiModelRouter *mRouter = nullptr;
    QgsAiFileContextProvider *mContextProvider = nullptr;
    QgsAiReviewPatchEngine *mReviewEngine = nullptr;
    QgsAiToolRegistry *mToolRegistry = nullptr;
    QString mActiveAgent = u"planner"_s;
    QList<QgsAiChatMessage> mHistory;
    QList<QgsAiModelRouter::Provider> mPendingProviders;
    QString mActiveRequestId;
    //! Id of the tool call running now, for its progress.
    QString mRunningToolCallId;
    std::function<QgsAiMapContext()> mMapContextProvider;
    //! The approval being waited for, answered by resolveToolApproval() or Stop.
    QPointer<QEventLoop> mApprovalLoop;
    QString mApprovalCallId;
    bool mApprovalGranted = false;
    bool mMapContextIncluded = true;
    QgsAiModelRouter::Provider mActiveProvider = QgsAiModelRouter::Provider::OpenAi;
    QString mCurrentPrompt;
    QList<QgsAiChatContextFile> mCurrentContextFiles;
    QString mStreamedText;
    int mToolIterations = 0;
    int mTotalToolIterations = 0;
    int mConsecutiveFailedToolRounds = 0;
    QHash<QString, int> mToolCallFingerprints;
    QHash<QString, int> mToolCallCounts;
    //! True when the most recent completed tool round reported at least one failure.
    bool mLastToolRoundHadError = false;
    //! Guards a single empty-response recovery attempt after a failed tool round.
    bool mEmptyErrorRecoveryAttempted = false;
    QgsAiAgentBehaviorSettings mBehaviorSettings;
    QgsAiManagedAgentPolicy mManagedAgentPolicy;
    QgsAiWorkspaceIndex *mWorkspaceIndex = nullptr;
    //! Retrieved-context block for the current turn; computed once per user message.
    QString mCachedRetrievalContext;
    //! Id of the user message mCachedRetrievalContext was computed for.
    QString mRetrievalContextMessageId;
    //! Non-null exactly while a background retrieval task is in flight.
    QPointer<QgsTask> mRetrievalTask;
    //! Set when Stop cancels the tool currently running inside the current tool round.
    bool mToolRunCanceled = false;
    //! True while onToolCallsRequested is executing tools (including nested event loops).
    bool mExecutingToolCalls = false;
    //! Incremented on reset, loadSession and startNewSession so in-flight tool results are discarded.
    quint64 mSessionGeneration = 0;
    //! Every spawned retrieval task still alive, including ones detached by a cancel.
    QList<QPointer<QgsTask>> mLiveRetrievalTasks;
    QgsAiChatHistoryStore *mHistoryStore = nullptr;
    QString mActiveSessionId;
    int mNextMessageOrdering = 0;
    QgsAiUsage mSessionUsage;
    QVariantList mAgentMemory;
    QString mDesktopClientSessionId;
    QString mAgentRunId;
    bool mAwaitingAgentRunApproval = false;
    QTimer *mAgentHeartbeatTimer = nullptr;

    friend class TestQgsAiDatabaseTools;
    friend class TestQgsAiAgentSessionManager;
};

#endif // QGSAIAGENTSESSIONMANAGER_H
