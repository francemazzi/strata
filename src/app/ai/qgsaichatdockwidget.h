/***************************************************************************
    qgsaichatdockwidget.h
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

#ifndef QGSAICHATDOCKWIDGET_H
#define QGSAICHATDOCKWIDGET_H

#include "qgis_app.h"
#include "qgsaiagentsessionmanager.h"
#include "qgsaigissuggestionengine.h"
#include "qgsdockwidget.h"

#include <QElapsedTimer>
#include <QList>
#include <QPointer>

class QAction;
class QCheckBox;
class QEvent;
class QFrame;
class QHBoxLayout;
class QJsonObject;
class QLabel;
class QListWidget;
class QPushButton;
class QShowEvent;
class QTextEdit;
class QTimer;
class QToolButton;
class QVBoxLayout;

class QgsAiChatPromptEdit;
class QgsAiDiscoveryController;
class QgsAiIndexingActivity;
class QgsAiLayerIndexCoordinator;
class QgsAiModelRouter;
class QgsAiPlanClient;
class QgsAiReviewPatchEngine;
class QgsScrollArea;

class APP_EXPORT QgsAiChatDockWidget : public QgsDockWidget
{
    Q_OBJECT

  public:
    QgsAiChatDockWidget( QgsAiAgentSessionManager *sessionManager, QgsAiModelRouter *modelRouter, QgsAiReviewPatchEngine *reviewEngine, QWidget *parent = nullptr );

    void setDiscoveryController( QgsAiDiscoveryController *controller );
    void setLayerIndexCoordinator( QgsAiLayerIndexCoordinator *coordinator );
    //! Shows what background indexing is doing in the chat header, with pause and resume.
    void setIndexingActivity( QgsAiIndexingActivity *activity );

    //! Puts the cursor in the message box, ready to type (the keyboard shortcut of the chat).
    void focusPrompt();

    //! Messages typed while the assistant works, sent in order when it finishes.
    int queuedMessageCount() const { return static_cast<int>( mQueuedMessages.size() ); }

  signals:
    void embeddingProviderSettingsChanged();

  public slots:
    void rebuildHistoryMenu();

  protected:
    bool eventFilter( QObject *watched, QEvent *event ) override;
    void showEvent( QShowEvent *event ) override;

  private slots:
    void sendMessage();
    void attachFile();
    void clearFileContext();
    void refreshProposalList();
    void previewProposal();
    void acceptProposal();
    void rejectProposal();
    void acceptPartialProposal();
    void openProviderSettings();
    void onModeSelected( QAction *action );
    void onModelSelected( QAction *action );
    void cancelRunningRequest();
    void onSendOrStopClicked();
    void onNewChatClicked();
    void onHistoryEntryTriggered( QAction *action );
    void reloadTranscriptFromHistory();
    void refreshGisSuggestionCard();

  private:
    QString selectedProposalId() const;
    //! Prompts for the workspace trust decision on the first AI interaction with an undecided workspace.
    void ensureWorkspaceTrustDecision();
    void appendTranscriptMessage( const QString &role, const QString &content );
    void appendTranscriptMessage( const QgsAiChatMessage &message );
    //! Undo button or "cannot be undone" note under a tool result card.
    QWidget *createToolResultActionsWidget( const QgsAiChatMessage &message );
    //! Shows "Undo this turn" on the user messages whose turn has changes that can be undone.
    void refreshUndoTurnButtons();
    void undoToolFromChat( const QString &toolMessageId );
    //! Copy on assistant messages; Copy, Edit and Retry on user messages.
    QHBoxLayout *createMessageActionsRow( const QgsAiChatMessage &message, QWidget *card );
    void editAndResendFromChat( const QString &messageId, const QString &text );
    //! Sends a prepared message now (after the workspace trust question when still undecided).
    void dispatchMessage( const QString &text, const QList<QgsAiChatContextFile> &contextFiles );
    void sendNextQueuedMessage();
    void refreshQueueBar();
    void retryFromChat();
    //! TRUE when the transcript is scrolled to (or near) its end.
    bool isTranscriptAtBottom() const;
    void renderStreamingText();
    void undoTurnFromChat( const QString &messageId );
    //! The card of the tool running now: what it does, for how long, and Stop.
    void showLiveToolCard( const QString &callId, const QString &toolName, const QVariantMap &args );
    void updateLiveToolProgress( const QString &callId, double percent, const QString &label );
    void closeLiveToolCard( const QString &callId );
    //! One line describing a tool call for the user ("calculate_field · Parcels · AREA").
    static QString toolCallSummary( const QString &toolName, const QVariantMap &args );
    QString renderToolMessageMarkdown( const QgsAiChatMessage &message ) const;
    static QString renderMarkdown( const QString &md );
    QWidget *createMessageWidget(
      const QString &role, const QString &content, const QVariantMap &metadata = QVariantMap(), const QString &messageId = QString(), QgsAiChatRole messageRole = QgsAiChatRole::Assistant
    );
    QWidget *createCollapsibleSection( const QString &title, const QString &content, const QString &language = QString(), bool collapsed = true );
    QWidget *createPlanActionsWidget( const QString &messageId, const QString &planMarkdown, const QVariantMap &metadata );
    QWidget *createQuestionsWidget( const QString &messageId, const QJsonObject &payload, const QVariantMap &metadata );
    QWidget *createToolLimitActionsWidget( const QString &messageId, const QVariantMap &metadata );
    void clearTranscriptWidgets();
    void scrollTranscriptToBottom();
    void showRequestError( const QgsAiChatMessage &message );
    void hideRequestError();
    //! Opens the settings dialog on \a section.
    void openProviderSettingsSection( const QString &section );
    void setModeLabel( const QString &label );
    void markMessageStatus( const QString &messageId, const QVariantMap &metadata, const QString &key, const QString &value );
    void acceptPlan( const QString &messageId, const QString &planMarkdown, const QVariantMap &metadata );
    QStringList disallowedWorkflowTools( const QString &planMarkdown, const QVariantMap &metadata ) const;
    /**
     * When the plan references tools the current Agent allowlist cannot run, marks the plan
     * blocked, posts an actionable notice, and returns true. Keeps Agent mode (no Plan bounce).
     */
    bool blockPlanExecutionForDisallowedTools( const QString &messageId, const QString &planMarkdown, const QVariantMap &metadata );
    QString saveWorkflowPlan( const QString &planMarkdown, const QString &messageId, QString *errorMessage = nullptr ) const;
    QString exportWorkflowReport( const QString &planMarkdown, const QString &messageId, QString *errorMessage = nullptr ) const;
    void dryRunWorkflowPlan( const QString &messageId, const QString &planMarkdown );
    void runWorkflowPlan( const QString &messageId, const QString &planMarkdown, const QVariantMap &metadata );
    void sendPlanRevision( const QString &messageId, const QString &planMarkdown, const QVariantMap &metadata, QTextEdit *revisionEdit );
    void sendQuestionAnswers( const QString &messageId, const QVariantMap &metadata, QWidget *questionsCard );
    void appendStreamChunk( const QString &chunk );
    void closeStreamingAssistantMessage();
    void updateRuntimeState( const QString &state, const QString &detail );
    //! Refreshes the per-session token/cost label in the status row.
    void updateSessionUsage( const QgsAiUsage &total );
    void applyPillStyling();
    void initModeMenu();
    void initModelMenu();
    //! (Re)builds the model picker menu, filtered to currently synced providers. Safe to call repeatedly.
    void rebuildModelMenu();
    void refreshPlanModels();
    void refreshPlanAgentPolicy();
    //! Pill caption for the active model, e.g. "Codex · GPT-5.4 ▾".
    QString modelPillLabel( QgsAiModelRouter::Provider provider, const QString &displayName ) const;
    void updateFileContextChip();
    void updateMentionPopup();
    void hideMentionPopup();
    void insertSelectedMention();
    void insertMentionFile( const QString &relativePath );
    void rebuildAttachmentChips();
    QList<QgsAiChatContextFile> contextFilesForCurrentMessage( const QString &text ) const;
    bool addAttachedFile( const QString &path );
    void promoteAttachedFile( const QString &path );
    void setAttachmentState( const QString &path, const QString &state );
    void setRequestRunning( bool running );
    void maybeShowWelcomeBanner();
    void sendGisSuggestionToChat( const QgsAiGisSuggestion &suggestion );
    void dismissGisSuggestion( const QString &suggestionId );
    void showGisSuggestions( const QList<QgsAiGisSuggestion> &suggestions );
    void refreshIndexingIndicator();

    struct AttachedFile
    {
        QString filePath;
        bool allowExternal = true;
        bool chatEligible = true;
        bool knowledgeEligible = false;
        QString state;
    };

    QPointer<QgsAiAgentSessionManager> mSessionManager;
    QPointer<QgsAiModelRouter> mModelRouter;
    QPointer<QgsAiPlanClient> mPlanClient;
    QPointer<QgsAiReviewPatchEngine> mReviewEngine;
    QPointer<QgsAiLayerIndexCoordinator> mLayerIndexCoordinator;
    QPointer<QgsAiIndexingActivity> mIndexingActivity;
    QFrame *mIndexingIndicator = nullptr;
    QToolButton *mIndexingStatusButton = nullptr;
    QToolButton *mIndexingPauseButton = nullptr;

    QgsScrollArea *mTranscriptScrollArea = nullptr;
    QWidget *mTranscriptContainer = nullptr;
    QVBoxLayout *mTranscriptLayout = nullptr;
    QgsAiChatPromptEdit *mInputTextEdit = nullptr;

    QToolButton *mNewChatButton = nullptr;
    QToolButton *mHistoryButton = nullptr;
    QToolButton *mModePill = nullptr;
    QToolButton *mModelPill = nullptr;
    QToolButton *mAttachButton = nullptr;
    QToolButton *mSettingsButton = nullptr;
    QToolButton *mSendButton = nullptr;
    QPushButton *mCancelButton = nullptr;

    QWidget *mFileContextChipRow = nullptr;
    QHBoxLayout *mFileContextChipLayout = nullptr;
    QList<AttachedFile> mAttachedFiles;

    QFrame *mGisCardContainer = nullptr;
    QToolButton *mGisCardToggle = nullptr;
    QWidget *mGisCardBody = nullptr;
    QVBoxLayout *mGisCardBodyLayout = nullptr;
    QTimer *mGisCardRefreshTimer = nullptr;
    QPointer<QgsAiGisSuggestionTask> mGisSuggestionTask;
    bool mGisSuggestionRefreshPending = false;
    QList<QgsAiGisSuggestion> mGisSuggestions;

    QFrame *mErrorBanner = nullptr;
    QLabel *mErrorTitleLabel = nullptr;
    QLabel *mErrorBodyLabel = nullptr;
    QPushButton *mErrorActionButton = nullptr;

    QFrame *mMentionPopup = nullptr;
    QListWidget *mMentionList = nullptr;
    int mMentionStartPosition = -1;

    QWidget *mReviewContainer = nullptr;
    QListWidget *mProposalList = nullptr;
    QLabel *mReviewStatusLabel = nullptr;
    QLabel *mRuntimeStatusLabel = nullptr;
    QLabel *mUsageLabel = nullptr;

    bool mStreamingInProgress = false;
    QTextEdit *mStreamingTextEdit = nullptr;
    //! Raw text streamed so far, rendered as markdown every 80 ms.
    QString mStreamingText;
    QTimer *mStreamingRenderTimer = nullptr;
    bool mRequestRunning = false;

    struct QueuedMessage
    {
        QString text;
        QList<QgsAiChatContextFile> contextFiles;
    };
    QList<QueuedMessage> mQueuedMessages;
    QWidget *mQueueBar = nullptr;
    QLabel *mQueueLabel = nullptr;

    QPointer<QFrame> mLiveToolCard;
    QLabel *mLiveToolElapsed = nullptr;
    QLabel *mLiveToolProgress = nullptr;
    QTimer *mLiveToolTimer = nullptr;
    QElapsedTimer mLiveToolClock;
    QString mLiveToolCallId;
    QList<QPointer<QPushButton>> mUndoTurnButtons;
};

#endif // QGSAICHATDOCKWIDGET_H
