/***************************************************************************
  testqgsaichatdockwidget.cpp
  ---------------------------
  begin                : April 2026
***************************************************************************/

#include <memory>

#include "ai/index/qgsaiembeddingprovider.h"
#include "ai/index/qgsaiworkspaceindex.h"
#include "ai/qgsaiagentsessionmanager.h"
#include "ai/qgsaichatdockwidget.h"
#include "ai/qgsaichathistorystore.h"
#include "ai/qgsaichatpromptedit.h"
#include "ai/qgsaifilecontextprovider.h"
#include "ai/qgsaimodelrouter.h"
#include "ai/qgsaiplanclient.h"
#include "ai/qgsaireviewpatchengine.h"
#include "ai/qgsaisecretstore.h"
#include "ai/qgsaisettingsdialog.h"
#include "ai/qgsaiworkspacetrust.h"
#include "ai/tools/qgsaiechotool.h"
#include "ai/tools/qgsaitoolregistry.h"
#include "qgsaisecretstoretestutils.h"
#include "qgsaitestloopbackserver.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsproject.h"
#include "qgssettings.h"
#include "qgstest.h"
#include "qgsvectorlayer.h"

#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QPair>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QString>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVariantMap>

using namespace Qt::StringLiterals;

namespace
{
  QString transcriptText( const QgsAiChatDockWidget &dock )
  {
    QWidget *container = dock.findChild<QWidget *>( u"aiTranscriptContainer"_s );
    if ( !container )
      return QString();

    QStringList parts;
    const QList<QLabel *> labels = container->findChildren<QLabel *>();
    for ( QLabel *label : labels )
      parts << label->text();
    const QList<QTextEdit *> edits = container->findChildren<QTextEdit *>();
    for ( QTextEdit *edit : edits )
      parts << edit->toPlainText();
    return parts.join( '\n' );
  }

  QString visibleLabelText( const QgsAiChatDockWidget &dock )
  {
    QWidget *container = dock.findChild<QWidget *>( u"aiTranscriptContainer"_s );
    if ( !container )
      return QString();

    QStringList parts;
    const QList<QLabel *> labels = container->findChildren<QLabel *>();
    for ( QLabel *label : labels )
      parts << label->text();
    return parts.join( '\n' );
  }

  QStringList modelMenuTexts( const QMenu *menu )
  {
    QStringList texts;
    if ( !menu )
      return texts;

    for ( const QAction *action : menu->actions() )
    {
      if ( !action )
        continue;
      const QString text = QString( action->text() ).remove( '&' );
      if ( action->isSeparator() && text.isEmpty() )
        continue;
      texts << text;
    }
    return texts;
  }

  QStringList selectableModelMenuTexts( const QMenu *menu )
  {
    QStringList texts;
    if ( !menu )
      return texts;

    for ( const QAction *action : menu->actions() )
    {
      if ( action && action->isEnabled() && action->isCheckable() )
        texts << QString( action->text() ).remove( '&' );
    }
    return texts;
  }

  void clearPlanModelPickerState()
  {
    QgsSettings settings;
    const QStringList groups = {
      u"ai/provider/openai"_s,
      u"ai/provider/claude"_s,
      u"ai/provider/openrouter"_s,
      u"ai/provider/codex"_s,
      u"ai/provider/plan"_s,
    };
    for ( const QString &group : groups )
      settings.remove( group );
    settings.remove( u"ai/activeProvider"_s );
    settings.remove( u"ai/security/secretsMigrated_v1"_s );

    QgsAiSecretStore::removeSecret( u"ai/provider/openai/apiKey"_s );
    QgsAiSecretStore::removeSecret( u"ai/provider/claude/apiKey"_s );
    QgsAiSecretStore::removeSecret( u"ai/provider/openrouter/apiKey"_s );
    QgsAiSecretStore::removeSecret( u"ai/provider/plan/token"_s );

    QFile::remove( QgsAiPlanClient::cacheFilePath() );
    QFile::remove( QgsAiPlanClient::agentPolicyCacheFilePath() );
    QFile::remove( QgsAiPlanClient::modelPreferencesCacheFilePath() );
  }

  [[nodiscard]] auto isolatePlanModelPickerState()
  {
    const QList<QByteArray> envNames = {
      "OPENAI_API_KEY",
      "CLAUDE_API_KEY",
      "ANTHROPIC_API_KEY",
      "OPENROUTER_API_KEY",
      "STRATA_PLAN_TOKEN",
    };
    QList<QPair<QByteArray, QByteArray>> savedEnv;
    for ( const QByteArray &name : envNames )
    {
      if ( qEnvironmentVariableIsSet( name.constData() ) )
        savedEnv.append( { name, qgetenv( name.constData() ) } );
      qunsetenv( name.constData() );
    }

    clearPlanModelPickerState();
    return qScopeGuard( [savedEnv]() {
      clearPlanModelPickerState();
      for ( const QPair<QByteArray, QByteArray> &entry : savedEnv )
        qputenv( entry.first.constData(), entry.second );
    } );
  }

  //! Returns a rollback token on each change, records the tokens taken back, and runs onChange meanwhile.
  class DockUndoableTool : public QgsAiTool
  {
    public:
      DockUndoableTool( QStringList *undone, std::function<void()> onChange )
        : mUndone( undone )
        , mOnChange( std::move( onChange ) )
      {}
      QString name() const override { return u"set_value"_s; }
      QString description() const override { return u"undoable tool"_s; }
      QJsonObject schema() const override { return QJsonObject { { u"type"_s, u"object"_s } }; }
      QgsAiToolResult execute( const QJsonObject &args ) override
      {
        const QString token = args.value( u"rollback_token"_s ).toString();
        if ( !token.isEmpty() )
        {
          *mUndone << token;
          return QgsAiToolResult::ok( QJsonObject { { u"status"_s, u"ok"_s } } );
        }
        mOnChange();
        QJsonObject output;
        output.insert( u"status"_s, u"ok"_s );
        output.insert( u"rollback_token"_s, u"tok_1"_s );
        output.insert( u"diff"_s, QJsonObject { { u"summary"_s, u"Changed value 1."_s }, { u"rollback_supported"_s, true } } );
        return QgsAiToolResult::ok( output );
      }

    private:
      QStringList *mUndone = nullptr;
      std::function<void()> mOnChange;
  };
} // namespace

class TestQgsAiChatDockWidget : public QObject
{
    Q_OBJECT

  private slots:
    void init() { installTestSecretBackend(); }
    void hasRuntimeWidgets();
    void planLoginModelPickerListsManagedAndByoModels();
    void emptyModelMenuOffersCloudSignIn();
    void unavailableSelectedProviderIsNotReplacedInModelPill();
    void gisCardShowsSuggestionAndSendsReview();
    void gisMentionAttachesHealthBlock();
    void usesPaletteBasedCursorStyling();
    void doesNotDuplicateStreamedAssistantResponse();
    void rendersToolResultWithoutRawJson();
    void rendersQuerySqlRowsAsMarkdownTable();
    void rendersExportLayerToPostgisSummary();
    void collapsesTechnicalCodeBlocks();
    void transcriptMessagesFitNarrowDockWithoutHorizontalScroll();
    void acceptingPlanSwitchesToAgentAndSendsPlan();
    void acceptingAgentPlanJsonSwitchesToAgent();
    void acceptingPlanWithDisallowedToolsStaysInAgentAndBlocks();
    void pickedModeIsRemembered();
    void toolCardsShowLiveStateAndUndo();
    void messagesTypedDuringATurnAreQueued();
    void emptyChatSuggestsPromptsForTheProject();
    void acceptingPlanWithAllowedToolsStaysInAgentAndExecutes();
    void cancelClearsOrphanStreamingAssistantCard();
    void workflowComposerExportsReportAndDryRun();
    void questionCardSendsStructuredAnswers();
    void toolLimitMessageShowsContinueButton();
    void settingsDialogContainsManualIndexingControls();
    void settingsSaveFailureStaysOpen();
    void settingsSessionOnlyRequiresChoice();
    void historyMenuPromptsForSavedProjectWhenUnsaved();
    void historyMenuDoesNotShowWorkspaceHistoryForUnsavedProject();
    void historyMenuShowsOnlyCurrentProjectSessions();
    void dropLocalFileCreatesAttachmentChip();
    void dropDoesNotInsertFileUriText();
};

void TestQgsAiChatDockWidget::settingsSaveFailureStaysOpen()
{
  const auto guard = isolatePlanModelPickerState();
  QCoreApplication::processEvents();
  using Store = QgsAiSecretStore;
  Store::BackendCallback pendingWrite;
  Store::setBackendForTesting( [&pendingWrite]( Store::Operation operation, const QString &, const QString &, Store::BackendCallback done ) {
    if ( operation == Store::Operation::Write )
      pendingWrite = done;
    else
      done( { false, true, {} } );
  } );
  QgsAiModelRouter router;
  router.setActiveProvider( QgsAiModelRouter::Provider::Claude );
  QgsAiSettingsDialog dialog( nullptr, &router, nullptr );
  QSignalSpy accepted( &dialog, &QDialog::accepted );
  dialog.show();
  auto *key = dialog.findChild<QLineEdit *>( u"aiOpenAiKeyLineEdit"_s );
  QVERIFY( key );
  key->setText( u"test-api-key"_s );
  dialog.accept();
  QTRY_VERIFY( pendingWrite );
  dialog.reject();
  dialog.close();
  dialog.accept();
  QVERIFY( dialog.isVisible() );
  QCOMPARE( accepted.size(), 0 );
  QVERIFY( !router.providerSettings( QgsAiModelRouter::Provider::OpenAi ).enabled );
  pendingWrite( {} );
  QTRY_VERIFY( dialog.findChild<QMessageBox *>() );
  auto *error = dialog.findChild<QMessageBox *>();
  error->button( QMessageBox::Cancel )->click();
  QCoreApplication::processEvents();
  QVERIFY( dialog.isVisible() );
  QCOMPARE( accepted.size(), 0 );
  QVERIFY( !router.providerSettings( QgsAiModelRouter::Provider::OpenAi ).enabled );
  QCOMPARE( router.activeProvider(), QgsAiModelRouter::Provider::Claude );
  QVERIFY( !QgsSettings().contains( u"ai/provider/openai/apiKey"_s ) );
  dialog.reject();
  QVERIFY( !dialog.isVisible() );
}

void TestQgsAiChatDockWidget::settingsSessionOnlyRequiresChoice()
{
  const auto guard = isolatePlanModelPickerState();
  QCoreApplication::processEvents();
  using Store = QgsAiSecretStore;
  Store::setBackendForTesting( []( Store::Operation, const QString &, const QString &, Store::BackendCallback done ) { done( {} ); } );
  QgsAiModelRouter router;
  router.setActiveProvider( QgsAiModelRouter::Provider::Claude );
  QgsAiSettingsDialog dialog( nullptr, &router, nullptr );
  QSignalSpy accepted( &dialog, &QDialog::accepted );
  dialog.show();
  dialog.findChild<QLineEdit *>( u"aiOpenAiKeyLineEdit"_s )->setText( u"test-session-key"_s );
  dialog.accept();
  QTRY_VERIFY( dialog.findChild<QMessageBox *>() );
  auto *error = dialog.findChild<QMessageBox *>();
  QAbstractButton *session = nullptr;
  for ( auto *button : error->buttons() )
    if ( button->text().contains( u"this session"_s ) )
      session = button;
  QVERIFY( session );
  session->click();
  QTRY_COMPARE( accepted.size(), 1 );
  QCOMPARE( Store::storageState( u"ai/provider/openai/apiKey"_s ), Store::StorageState::SessionOnly );
  QVERIFY( router.isProviderUsable( QgsAiModelRouter::Provider::OpenAi ) );
  QCOMPARE( router.activeProvider(), QgsAiModelRouter::Provider::Claude );
  QCOMPARE( router.resolveProvider(), QgsAiModelRouter::Provider::Claude );
  QVERIFY( !QgsSettings().contains( u"ai/provider/openai/apiKey"_s ) );
}

void TestQgsAiChatDockWidget::hasRuntimeWidgets()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QLabel *runtimeLabel = dock.findChild<QLabel *>( u"aiRuntimeStatusLabel"_s );
  QPushButton *cancelButton = dock.findChild<QPushButton *>( u"aiCancelRequestButton"_s );
  QVERIFY( runtimeLabel );
  QVERIFY( cancelButton );
  QVERIFY( !cancelButton->isEnabled() );
  QVERIFY( runtimeLabel->text().contains( u"idle"_s, Qt::CaseInsensitive ) );
}

void TestQgsAiChatDockWidget::planLoginModelPickerListsManagedAndByoModels()
{
  const auto guard = isolatePlanModelPickerState();

  QList<QgsAiPlanClient::ModelInfo> models;
  QgsAiPlanClient::ModelInfo managed;
  managed.id = u"managed-plan"_s;
  managed.label = u"Strata Managed"_s;
  managed.provider = u"strata"_s;
  managed.capabilities = { u"chat"_s, u"tools"_s };
  models << managed;

  QgsAiPlanClient::ModelInfo gpt4oMini;
  gpt4oMini.id = u"openai/gpt-4o-mini"_s;
  gpt4oMini.label = u"GPT-4o mini"_s;
  gpt4oMini.provider = u"strata"_s;
  gpt4oMini.capabilities = { u"chat"_s };
  models << gpt4oMini;

  QgsAiPlanClient::ModelInfo disabled;
  disabled.id = u"deepseek/deepseek-v4-flash"_s;
  disabled.label = u"DeepSeek V4 Flash"_s;
  disabled.provider = u"strata"_s;
  disabled.capabilities = { u"chat"_s };
  models << disabled;

  QgsAiPlanClient::ModelInfo embeddingOnly;
  embeddingOnly.id = u"text-embedding-3-small"_s;
  embeddingOnly.label = u"Embedding only"_s;
  embeddingOnly.provider = u"strata"_s;
  embeddingOnly.capabilities = { u"embedding"_s };
  models << embeddingOnly;
  QgsAiPlanClient::writeCachedModels( models );

  QgsAiManagedAgentPolicy policy;
  policy.allowedModels = {
    u"managed-plan"_s,
    u"openai/gpt-4o-mini"_s,
    u"deepseek/deepseek-v4-flash"_s,
    u"text-embedding-3-small"_s,
  };
  QgsAiPlanClient::writeCachedAgentPolicy( policy );
  QgsAiPlanClient::writeCachedModelPreferences( {
    QgsAiPlanClient::ModelPreferenceInfo { u"deepseek/deepseek-v4-flash"_s, false },
  } );

  QgsAiModelRouter router;
  QVERIFY( router.storeApiKey( QgsAiModelRouter::Provider::OpenRouter, u"sk-or-picker-test"_s ) );
  QVERIFY( router.storeApiKey( QgsAiModelRouter::Provider::Claude, u"sk-ant-picker-test"_s ) );

  QgsAiModelRouter::ProviderSettings openRouterSettings = router.providerSettings( QgsAiModelRouter::Provider::OpenRouter );
  openRouterSettings.model = u"anthropic/claude-sonnet-4.6"_s;
  openRouterSettings.enabled = true;
  router.setProviderSettings( QgsAiModelRouter::Provider::OpenRouter, openRouterSettings );

  QgsAiModelRouter::ProviderSettings claudeSettings = router.providerSettings( QgsAiModelRouter::Provider::Claude );
  claudeSettings.model = u"claude-sonnet-5"_s;
  claudeSettings.enabled = true;
  router.setProviderSettings( QgsAiModelRouter::Provider::Claude, claudeSettings );

  QString error;
  QVERIFY2( router.setPlanSessionToken( u"strata-plan-picker-token"_s, &error ), qPrintable( error ) );
  QgsAiModelRouter::ProviderSettings planSettings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
  planSettings.endpoint = u"http://127.0.0.1:9/ai/messages"_s;
  planSettings.model = u"managed-plan"_s;
  planSettings.enabled = true;
  router.setProviderSettings( QgsAiModelRouter::Provider::Plan, planSettings );
  router.setActiveProvider( QgsAiModelRouter::Provider::OpenRouter );

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  QApplication::processEvents();

  QToolButton *modelPill = dock.findChild<QToolButton *>( u"aiModelPill"_s );
  QVERIFY( modelPill );
  QMenu *menu = modelPill->menu();
  QVERIFY( menu );

  // BYO sections coexist with the managed Strata section (no "Plan backend" label).
  const QStringList menuTexts = modelMenuTexts( menu );
  QVERIFY( !menuTexts.contains( u"Plan backend"_s ) );
  QVERIFY( menuTexts.contains( u"Strata"_s ) );
  QVERIFY( menuTexts.contains( u"OpenRouter"_s ) );
  QVERIFY( menuTexts.contains( u"Anthropic"_s ) );

  const QStringList selectableTexts = selectableModelMenuTexts( menu );
  QVERIFY( !selectableTexts.isEmpty() );
  // Managed catalog entries, filtered by policy/preferences/capabilities. The managed-plan alias
  // (label "Strata Managed") is a hidden infra default that routes to Lite — never user-selectable.
  QVERIFY( !selectableTexts.contains( u"Strata Managed"_s ) );
  QVERIFY( selectableTexts.contains( u"GPT-4o mini"_s ) );
  QVERIFY( !selectableTexts.contains( u"Embedding only"_s ) );
  // BYO entries stay selectable while signed in.
  QVERIFY( selectableTexts.contains( u"Claude Sonnet 4.6"_s ) );
  QVERIFY( selectableTexts.contains( u"Claude Sonnet 5"_s ) );
  // Disabled managed preference filters the Plan copy; the BYO OpenRouter row remains.
  QCOMPARE( selectableTexts.count( u"DeepSeek V4 Flash"_s ), 1 );
  // Rebuilding the menu must never hijack the user's explicit provider choice.
  QCOMPARE( router.activeProvider(), QgsAiModelRouter::Provider::OpenRouter );
  QCOMPARE( router.resolveProvider(), QgsAiModelRouter::Provider::OpenRouter );
}

void TestQgsAiChatDockWidget::unavailableSelectedProviderIsNotReplacedInModelPill()
{
  const auto guard = isolatePlanModelPickerState();
  QgsSettings settings;
  settings.setValue( u"ai/activeProvider"_s, u"Claude"_s );
  settings.setValue( u"ai/provider/claude/credentialMode"_s, u"oauth"_s );
  settings.setValue( u"ai/security/providerSelectionRequired"_s, true );
  const auto clearSelection = qScopeGuard( []() { QgsSettings().remove( u"ai/security/providerSelectionRequired"_s ); } );
  QgsAiModelRouter router;
  QVERIFY( router.requiresProviderSelection() );
  QVERIFY( router.storeApiKey( QgsAiModelRouter::Provider::OpenRouter, u"sk-or-picker-test"_s ) );
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  QApplication::processEvents();

  QToolButton *pill = dock.findChild<QToolButton *>( u"aiModelPill"_s );
  QVERIFY( pill );
  QVERIFY( pill->text().startsWith( router.providerDisplayName( QgsAiModelRouter::Provider::Claude ) ) );
  QVERIFY( pill->text().contains( u"Choose a provider"_s ) );
  QVERIFY( !pill->text().contains( u"OpenRouter"_s ) );
  QAction *choice = nullptr;
  for ( QAction *action : pill->menu()->actions() )
  {
    QVERIFY( !action->isChecked() );
    if ( action->isCheckable() )
      choice = action;
  }
  QVERIFY( choice );
  choice->trigger();
  QCOMPARE( router.activeProvider(), QgsAiModelRouter::Provider::OpenRouter );
  QVERIFY( !router.requiresProviderSelection() );
  QVERIFY( pill->text().startsWith( "OpenRouter"_L1 ) );
}

void TestQgsAiChatDockWidget::gisCardShowsSuggestionAndSendsReview()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QSettings settings;
  const QString unsavedHash = QString::fromLatin1( QCryptographicHash::hash( QByteArrayLiteral( "unsaved" ), QCryptographicHash::Sha1 ).toHex() );
  const QString globalKey = u"strata/gis_tab/enabled"_s;
  const QString unsavedProjectKey = u"strata/gis_tab/project_enabled/%1"_s.arg( unsavedHash );
  const QString dismissedKey = u"strata/gis_tab/dismissed/%1"_s.arg( unsavedHash );
  const QVariant savedGlobal = settings.value( globalKey );
  const QVariant savedProject = settings.value( unsavedProjectKey );
  const QVariant savedDismissed = settings.value( dismissedKey );
  settings.setValue( globalKey, true );
  settings.setValue( unsavedProjectKey, true );
  settings.remove( dismissedKey );

  QgsProject::instance()->clear();
  QgsVectorLayer *noCrsLayer = new QgsVectorLayer( u"Point?field=name:string&crs=EPSG:4326"_s, u"No CRS points"_s, u"memory"_s );
  QVERIFY( noCrsLayer->isValid() );
  noCrsLayer->setCrs( QgsCoordinateReferenceSystem() );
  QgsProject::instance()->addMapLayer( noCrsLayer );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  dock.resize( 400, 520 );
  dock.show();
  QApplication::processEvents();

  // The dedicated GIS tab is gone: suggestions live in the chat now.
  QVERIFY( !dock.findChild<QWidget *>( u"aiGisSuggestionsTab"_s ) );
  QVERIFY( !dock.findChild<QListWidget *>( u"aiGisSuggestionList"_s ) );

  // The check samples geometries on a worker thread: the card appears when it ends.
  QFrame *card = dock.findChild<QFrame *>( u"aiGisSuggestionCard"_s );
  QVERIFY( card );
  QTRY_VERIFY_WITH_TIMEOUT( card->isVisible(), 10000 );

  QPushButton *review = dock.findChild<QPushButton *>( u"aiGisCardReviewButton"_s );
  QVERIFY( review );

  // While a turn runs, Analyze, mode and model are locked; they unlock when it ends.
  QToolButton *modePill = dock.findChild<QToolButton *>( u"aiModePill"_s );
  QToolButton *modelPill = dock.findChild<QToolButton *>( u"aiModelPill"_s );
  QVERIFY( modePill );
  QVERIFY( modelPill );
  manager.requestRunningChanged( true );
  QVERIFY( !review->isEnabled() );
  QVERIFY( !modePill->isEnabled() );
  QVERIFY( !modelPill->isEnabled() );
  manager.requestRunningChanged( false );
  QVERIFY( review->isEnabled() );
  QVERIFY( modePill->isEnabled() );
  QVERIFY( modelPill->isEnabled() );

  review->click();

  QCOMPARE( manager.activeAgent(), u"ask_before_edits"_s );
  QVERIFY( !manager.history().isEmpty() );
  QVERIFY( manager.history().first().content.contains( u"GIS suggestion selected"_s ) );
  QVERIFY( manager.history().first().content.contains( u"ask-before-edits"_s ) );

  // Dismissing every visible suggestion hides the card, and the dismissals
  // are persisted per project so a refresh does not resurface them.
  for ( int guard = 0; guard < 10 && card->isVisible(); ++guard )
  {
    QToolButton *dismiss = dock.findChild<QToolButton *>( u"aiGisCardDismissButton"_s );
    QVERIFY( dismiss );
    dismiss->click();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    QApplication::processEvents();
  }
  QVERIFY( !card->isVisible() );
  QVERIFY( QMetaObject::invokeMethod( &dock, "refreshGisSuggestionCard", Qt::DirectConnection ) );
  QApplication::processEvents();
  QVERIFY( !card->isVisible() );

  QgsProject::instance()->clear();
  if ( savedGlobal.isValid() )
    settings.setValue( globalKey, savedGlobal );
  else
    settings.remove( globalKey );
  if ( savedProject.isValid() )
    settings.setValue( unsavedProjectKey, savedProject );
  else
    settings.remove( unsavedProjectKey );
  if ( savedDismissed.isValid() )
    settings.setValue( dismissedKey, savedDismissed );
  else
    settings.remove( dismissedKey );
}

void TestQgsAiChatDockWidget::gisMentionAttachesHealthBlock()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsProject::instance()->clear();

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  // Pre-trust the workspace so sendMessage() does not block on the modal trust prompt.
  const QString trustRoot = QgsAiWorkspaceTrust::currentWorkspaceRoot();
  const QgsAiWorkspaceTrust::State savedTrust = trustRoot.isEmpty() ? QgsAiWorkspaceTrust::State::Unknown : QgsAiWorkspaceTrust::state( trustRoot );
  if ( !trustRoot.isEmpty() )
    QgsAiWorkspaceTrust::setState( trustRoot, QgsAiWorkspaceTrust::State::Trusted );

  QgsAiChatPromptEdit *input = dock.findChild<QgsAiChatPromptEdit *>( u"aiPromptInput"_s );
  QVERIFY( input );
  input->setPlainText( u"@gis check the project"_s );
  QVERIFY( QMetaObject::invokeMethod( &dock, "sendMessage", Qt::DirectConnection ) );

  QVERIFY( !manager.history().isEmpty() );
  const QString sent = manager.history().first().content;
  QVERIFY( sent.contains( u"@gis check the project"_s ) );
  QVERIFY( sent.contains( u"Current project GIS health"_s ) );
  QVERIFY( sent.contains( u"No layers loaded"_s ) );

  if ( !trustRoot.isEmpty() )
    QgsAiWorkspaceTrust::setState( trustRoot, savedTrust );
  QgsProject::instance()->clear();
}

void TestQgsAiChatDockWidget::usesPaletteBasedCursorStyling()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QTextEdit *input = dock.findChild<QTextEdit *>( u"aiPromptInput"_s );
  QToolButton *sendButton = dock.findChild<QToolButton *>( u"aiSendButton"_s );
  QToolButton *modePill = dock.findChild<QToolButton *>( u"aiModePill"_s );
  QVERIFY( input );
  QVERIFY( sendButton );
  QVERIFY( modePill );
  QVERIFY( input->styleSheet().contains( u"palette(base)"_s ) );
  QVERIFY( sendButton->styleSheet().contains( u"palette(highlight)"_s ) );
  QVERIFY( modePill->styleSheet().contains( u"palette(button)"_s ) );

  QgsAiChatMessage assistantMessage;
  assistantMessage.id = u"assistant-style"_s;
  assistantMessage.role = QgsAiChatRole::Assistant;
  assistantMessage.content = u"Styled response."_s;
  manager.messageAdded( assistantMessage );

  QFrame *message = dock.findChild<QFrame *>( u"aiMessage"_s );
  QVERIFY( message );
  QVERIFY( message->styleSheet().contains( u"palette(base)"_s ) );
  QVERIFY( message->styleSheet().contains( u"palette(mid)"_s ) );
  static const QRegularExpression hexColorRe( u"#[0-9a-fA-F]{3,8}\\b"_s );
  QVERIFY( !hexColorRe.match( message->styleSheet() ).hasMatch() );
}

void TestQgsAiChatDockWidget::doesNotDuplicateStreamedAssistantResponse()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QgsAiChatMessage assistantMessage;
  assistantMessage.id = u"assistant-1"_s;
  assistantMessage.role = QgsAiChatRole::Assistant;
  assistantMessage.content = u"Ciao! Come posso aiutarti con QGIS oggi?"_s;

  manager.responseChunkReceived( u"Ciao! Come posso "_s );
  manager.responseChunkReceived( u"aiutarti con QGIS oggi?"_s );
  // The streamed text is rendered as markdown every 80 ms.
  QTRY_COMPARE_WITH_TIMEOUT( transcriptText( dock ).count( u"Ciao! Come posso aiutarti con QGIS oggi?"_s ), 1, 2000 );

  manager.messageAdded( assistantMessage );
  QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
  QApplication::processEvents();
  QCOMPARE( transcriptText( dock ).count( u"Ciao! Come posso aiutarti con QGIS oggi?"_s ), 1 );
}

void TestQgsAiChatDockWidget::rendersToolResultWithoutRawJson()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QJsonObject output;
  output.insert( u"status"_s, u"ok"_s );
  output.insert( u"http_status"_s, 200 );
  output.insert( u"bytes_written"_s, 1234 );
  output.insert( u"dest_path"_s, tempDir.filePath( u"data/pomponesco.geojson"_s ) );

  QgsAiChatMessage toolMessage;
  toolMessage.role = QgsAiChatRole::Tool;
  toolMessage.content = QString::fromUtf8( QJsonDocument( output ).toJson( QJsonDocument::Compact ) );
  toolMessage.metadata.insert( u"tool_name"_s, u"download_file"_s );
  QVariantMap args;
  args.insert( u"url"_s, u"https://overpass-api.de/api/interpreter?data=secret-query"_s );
  toolMessage.metadata.insert( u"tool_args"_s, args );

  manager.messageAdded( toolMessage );
  const QString plain = transcriptText( dock );
  QVERIFY( plain.contains( u"download_file"_s ) );
  QVERIFY( plain.contains( u"overpass-api.de"_s ) );
  QVERIFY( plain.contains( u"data/pomponesco.geojson"_s ) );
  QVERIFY( !plain.contains( u"{\"status\""_s ) );
  QVERIFY( !plain.contains( u"secret-query"_s ) );
}

void TestQgsAiChatDockWidget::rendersQuerySqlRowsAsMarkdownTable()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QJsonObject output;
  output.insert( u"status"_s, u"ok"_s );
  output.insert( u"connection_name"_s, u"lab"_s );
  output.insert( u"sql"_s, u"SELECT id, name FROM trees LIMIT 2"_s );
  output.insert( u"columns"_s, QJsonArray { u"id"_s, u"name"_s } );
  QJsonArray rows;
  rows.append( QJsonObject { { u"id"_s, 1 }, { u"name"_s, u"oak"_s } } );
  rows.append( QJsonObject { { u"id"_s, 2 }, { u"name"_s, u"pine"_s } } );
  output.insert( u"rows"_s, rows );
  output.insert( u"returned_count"_s, 2 );

  QgsAiChatMessage toolMessage;
  toolMessage.role = QgsAiChatRole::Tool;
  toolMessage.content = QString::fromUtf8( QJsonDocument( output ).toJson( QJsonDocument::Compact ) );
  toolMessage.metadata.insert( u"tool_name"_s, u"query_sql"_s );

  manager.messageAdded( toolMessage );
  const QString plain = transcriptText( dock );
  QVERIFY( plain.contains( u"query_sql"_s ) );
  QVERIFY( plain.contains( u"SELECT id, name FROM trees"_s ) );
  QVERIFY( plain.contains( u"oak"_s ) );
  QVERIFY( plain.contains( u"pine"_s ) );
  QVERIFY( plain.contains( u"<table"_s ) || plain.contains( u"| id |"_s ) || plain.contains( u"| id | name |"_s ) );
}

void TestQgsAiChatDockWidget::rendersExportLayerToPostgisSummary()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QJsonObject output;
  output.insert( u"status"_s, u"ok"_s );
  output.insert( u"schema"_s, u"public"_s );
  output.insert( u"table"_s, u"trees"_s );
  output.insert( u"feature_count"_s, 12 );
  output.insert( u"overwrite"_s, true );
  output.insert( u"spatial_index"_s, true );

  QgsAiChatMessage toolMessage;
  toolMessage.role = QgsAiChatRole::Tool;
  toolMessage.content = QString::fromUtf8( QJsonDocument( output ).toJson( QJsonDocument::Compact ) );
  toolMessage.metadata.insert( u"tool_name"_s, u"export_layer_to_postgis"_s );

  manager.messageAdded( toolMessage );
  const QString plain = transcriptText( dock );
  QVERIFY( plain.contains( u"export_layer_to_postgis"_s ) );
  QVERIFY( plain.contains( u"public.trees"_s ) );
  QVERIFY( plain.contains( u"12"_s ) );
}

void TestQgsAiChatDockWidget::collapsesTechnicalCodeBlocks()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QgsAiChatMessage assistantMessage;
  assistantMessage.id = u"assistant-code"_s;
  assistantMessage.role = QgsAiChatRole::Assistant;
  assistantMessage.content = u"Here is the result.\n```python\nprint('hidden')\n```\nDone."_s;

  manager.messageAdded( assistantMessage );

  QToolButton *toggle = dock.findChild<QToolButton *>( u"aiTechnicalToggle"_s );
  QTextEdit *details = dock.findChild<QTextEdit *>( u"aiTechnicalContent"_s );
  QVERIFY( toggle );
  QVERIFY( details );
  QVERIFY( !details->isVisible() );
  QVERIFY( details->toPlainText().contains( u"print('hidden')"_s ) );
  QVERIFY( !visibleLabelText( dock ).contains( u"print('hidden')"_s ) );
}

void TestQgsAiChatDockWidget::transcriptMessagesFitNarrowDockWithoutHorizontalScroll()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  dock.resize( 300, 520 );
  dock.show();
  QApplication::processEvents();

  QgsAiChatMessage assistantMessage;
  assistantMessage.id = u"assistant-wide-table"_s;
  assistantMessage.role = QgsAiChatRole::Assistant;
  assistantMessage.content
    = u"Ecco l'elenco completo dei layer.\n\n"
      "| Nome layer | Tipo layer | Geometria | N. feature | CRS |\n"
      "| --- | --- | --- | ---: | --- |\n"
      "| dbgt_AB_CDA_AB_CDA_SUP_SR_nome_molto_lungo_senza_spazi | vector | MultiPolygonZWithVeryLongGeometryName | 19382 | EPSG:7791-long-crs-description-without-natural-breaks |\n"
      "| dbgt_ACC_PC_ACC_PC_POS_layer_con_nome_esteso | vector | Point | 41942 | non definito - percorso /workspace/strata/tests_ai/Dati/strata_test_brescia.qgz |\n\n"
      "```json\n"
      "{\"project_file\":\"/workspace/strata/tests_ai/Dati/strata_test_brescia.qgz\",\"layer_count\":118}\n"
      "```"_s;

  manager.messageAdded( assistantMessage );
  manager.responseChunkReceived( u"streaming-token-without-spaces-or-natural-breaks-EPSG7791-layer-name-dbgt_ACC_PC_ACC_PC_POS"_s );
  QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
  QApplication::processEvents();

  QAbstractScrollArea *scrollArea = dock.findChild<QAbstractScrollArea *>( u"aiTranscriptScrollArea"_s );
  QWidget *container = dock.findChild<QWidget *>( u"aiTranscriptContainer"_s );
  QVERIFY( scrollArea );
  QVERIFY( container );
  QVERIFY( scrollArea->viewport() );
  QCOMPARE( scrollArea->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff );

  const int viewportWidth = scrollArea->viewport()->width();
  QVERIFY( viewportWidth > 0 );
  QVERIFY2( container->width() <= viewportWidth + 1, qPrintable( u"Transcript container width %1 exceeds viewport width %2"_s.arg( container->width() ).arg( viewportWidth ) ) );

  const QList<QFrame *> frames = container->findChildren<QFrame *>();
  bool checkedTranscriptFrame = false;
  for ( QFrame *frame : frames )
  {
    if ( frame->objectName() != "aiMessage"_L1 && frame->objectName() != "aiStreamingMessage"_L1 && frame->objectName() != "aiPlanCard"_L1 && frame->objectName() != "aiQuestionsCard"_L1 )
      continue;

    checkedTranscriptFrame = true;
    QVERIFY2( frame->width() <= viewportWidth + 1, qPrintable( u"%1 width %2 exceeds viewport width %3"_s.arg( frame->objectName() ).arg( frame->width() ).arg( viewportWidth ) ) );
  }
  QVERIFY( checkedTranscriptFrame );

  const QList<QTextEdit *> transcriptEdits = container->findChildren<QTextEdit *>();
  QVERIFY( !transcriptEdits.isEmpty() );
  for ( QTextEdit *edit : transcriptEdits )
  {
    QCOMPARE( edit->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff );
    QVERIFY( edit->lineWrapMode() != QTextEdit::NoWrap );
  }
}

void TestQgsAiChatDockWidget::acceptingPlanSwitchesToAgentAndSendsPlan()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QgsAiChatMessage planMessage;
  planMessage.id = u"plan-1"_s;
  planMessage.role = QgsAiChatRole::Assistant;
  planMessage.content = u"<proposed_plan>\n1. Read files\n2. Patch UI\n</proposed_plan>"_s;
  planMessage.metadata.insert( u"ui_kind"_s, u"plan"_s );
  planMessage.metadata.insert( u"plan_markdown"_s, u"1. Read files\n2. Patch UI"_s );
  planMessage.metadata.insert( u"plan_status"_s, u"pending"_s );

  manager.messageAdded( planMessage );
  QPushButton *accept = dock.findChild<QPushButton *>( u"aiAcceptPlanButton"_s );
  QPushButton *saveWorkflow = dock.findChild<QPushButton *>( u"aiSaveWorkflowButton"_s );
  QPushButton *dryRunWorkflow = dock.findChild<QPushButton *>( u"aiDryRunWorkflowButton"_s );
  QPushButton *runWorkflow = dock.findChild<QPushButton *>( u"aiRunWorkflowButton"_s );
  QPushButton *exportReport = dock.findChild<QPushButton *>( u"aiExportWorkflowReportButton"_s );
  QVERIFY( accept );
  QVERIFY( saveWorkflow );
  QVERIFY( dryRunWorkflow );
  QVERIFY( runWorkflow );
  QVERIFY( exportReport );
  accept->click();

  QCOMPARE( manager.activeAgent(), u"editor"_s );
  QVERIFY( !manager.history().isEmpty() );
  QVERIFY( manager.history().first().content.contains( u"Accepted plan"_s ) );
  QVERIFY( manager.history().first().content.contains( u"Patch UI"_s ) );
  QVERIFY( manager.history().first().content.contains( u"Saved reusable workflow"_s ) );

  QDir workflowDir( QDir( tempDir.path() ).filePath( u".strata/workflows"_s ) );
  const QFileInfoList workflows = workflowDir.entryInfoList( QStringList { u"*.strataflow"_s }, QDir::Files );
  QCOMPARE( workflows.size(), 1 );
  QFile workflowFile( workflows.first().absoluteFilePath() );
  QVERIFY( workflowFile.open( QIODevice::ReadOnly ) );
  const QJsonObject workflow = QJsonDocument::fromJson( workflowFile.readAll() ).object();
  QCOMPARE( workflow.value( u"kind"_s ).toString(), u"strataflow"_s );
  QCOMPARE( workflow.value( u"version"_s ).toInt(), 1 );
  QCOMPARE( workflow.value( u"mode"_s ).toString(), u"auto_edit"_s );
  QVERIFY( workflow.value( u"planMarkdown"_s ).toString().contains( u"Patch UI"_s ) );
  QVERIFY( workflow.value( u"runner"_s ).toObject().value( u"dryRunSupported"_s ).toBool() );
  QVERIFY( workflow.value( u"runner"_s ).toObject().value( u"requiresApproval"_s ).toBool() );
  QVERIFY( workflow.value( u"provenance"_s ).toObject().value( u"metadataOnly"_s ).toBool() );
  QCOMPARE( workflow.value( u"steps"_s ).toArray().size(), 2 );
}

void TestQgsAiChatDockWidget::acceptingAgentPlanJsonSwitchesToAgent()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QJsonObject step;
  step.insert( u"id"_s, u"s1"_s );
  step.insert( u"title"_s, u"Load boundary"_s );
  step.insert( u"risk"_s, u"medium"_s );
  step.insert( u"requires_approval"_s, true );
  step.insert( u"depends_on"_s, QJsonArray() );

  QJsonObject plan;
  plan.insert( u"version"_s, 1 );
  plan.insert( u"objective"_s, u"Download official boundary"_s );
  plan.insert( u"mode"_s, u"auto_edit"_s );
  plan.insert( u"steps"_s, QJsonArray { step } );
  const QString planJson = QString::fromUtf8( QJsonDocument( plan ).toJson( QJsonDocument::Compact ) );

  QgsAiChatMessage planMessage;
  planMessage.id = u"agent-plan-1"_s;
  planMessage.role = QgsAiChatRole::Assistant;
  planMessage.content = u"Ready.\n```strata_agent_plan\n%1\n```"_s.arg( planJson );
  planMessage.metadata.insert( u"ui_kind"_s, u"agent_plan"_s );
  planMessage.metadata.insert( u"plan_json"_s, planJson );
  planMessage.metadata.insert( u"plan_status"_s, u"pending"_s );

  manager.messageAdded( planMessage );
  QVERIFY( visibleLabelText( dock ).contains( u"Download official boundary"_s ) );
  QVERIFY( visibleLabelText( dock ).contains( u"Load boundary"_s ) );
  QVERIFY( !visibleLabelText( dock ).contains( u"strata_agent_plan"_s ) );

  QPushButton *accept = dock.findChild<QPushButton *>( u"aiAcceptPlanButton"_s );
  QVERIFY( accept );
  accept->click();

  QCOMPARE( manager.activeAgent(), u"editor"_s );
  QVERIFY( !manager.history().isEmpty() );
  QVERIFY( manager.history().first().content.contains( u"Accepted plan"_s ) );
  QVERIFY( manager.history().first().content.contains( u"Load boundary"_s ) );
}

void TestQgsAiChatDockWidget::pickedModeIsRemembered()
{
  QgsSettings().remove( u"strata/agent"_s );
  const QScopeGuard cleanup( [] { QgsSettings().remove( u"strata/agent"_s ); } );
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  // A new profile starts in Agent mode.
  QToolButton *pill = dock.findChild<QToolButton *>( u"aiModePill"_s );
  QVERIFY( pill && pill->menu() );
  QVERIFY( pill->text().startsWith( "Agent"_L1 ) );

  for ( QAction *action : pill->menu()->actions() )
  {
    if ( action->text() == "Ask"_L1 )
      action->trigger();
  }
  QCOMPARE( manager.activeAgent(), u"reviewer"_s );
  QCOMPARE( QgsSettings().value( QgsAiAgentSessionManager::startAgentSettingsKey() ).toString(), u"reviewer"_s );
}

void TestQgsAiChatDockWidget::toolCardsShowLiveStateAndUndo()
{
  const auto isolated = isolatePlanModelPickerState();
  QgsSettings settings;
  settings.remove( u"ai/provider/openrouter"_s );
  settings.remove( u"strata/agent"_s );
  const auto cleanup = qScopeGuard( [&settings]() {
    settings.remove( u"ai/provider/openrouter"_s );
    settings.remove( u"ai/network/maxRetries"_s );
    settings.remove( u"strata/agent"_s );
  } );

  QgsAiTestLoopbackServer server;
  server.responses
    << QgsAiTestLoopbackServer::
         jsonResponse( 200, "OK", QByteArrayLiteral( R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"set_value","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})" ) )
    << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"choices":[{"message":{"role":"assistant","content":"Done"},"finish_reason":"stop"}]})" ) );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );
  // The key through the environment: this test has no secret store (the guard above unsets it again).
  qputenv( "OPENROUTER_API_KEY", "sk-or-loopback-test" );
  settings.setValue( u"ai/network/maxRetries"_s, 0 );

  QStringList undone;
  QString liveTitle;
  QgsAiChatDockWidget *dockPtr = nullptr;
  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<DockUndoableTool>( &undone, [&dockPtr, &liveTitle]() {
    // While the tool runs, its card shows what it does.
    QFrame *card = dockPtr ? dockPtr->findChild<QFrame *>( u"aiLiveToolCard"_s ) : nullptr;
    QLabel *title = card ? card->findChild<QLabel *>( u"aiLiveToolTitle"_s ) : nullptr;
    liveTitle = title ? title->text() : QString();
  } ) );
  QgsAiModelRouter router;
  router.setToolRegistry( &registry );
  QgsAiModelRouter::ProviderSettings providerSettings = router.providerSettings( QgsAiModelRouter::Provider::OpenRouter );
  providerSettings.endpoint = u"http://127.0.0.1:%1/api/v1/chat/completions"_s.arg( server.serverPort() );
  providerSettings.model = u"test/model"_s;
  providerSettings.enabled = true;
  router.setProviderSettings( QgsAiModelRouter::Provider::OpenRouter, providerSettings );
  router.setActiveProvider( QgsAiModelRouter::Provider::OpenRouter );

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( &router, &contextProvider, &reviewEngine );
  manager.setToolRegistry( &registry );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  dockPtr = &dock;
  dock.show();

  manager.sendUserMessage( u"change a value"_s );
  const auto answered = [&manager]() {
    const QList<QgsAiChatMessage> history = manager.history();
    return std::any_of( history.cbegin(), history.cend(), []( const QgsAiChatMessage &message ) { return message.content == "Done"_L1; } );
  };
  QTRY_VERIFY_WITH_TIMEOUT( answered() || ( manager.history().size() > 1 && !manager.hasActiveRequest() ), 60000 );
  QTRY_VERIFY_WITH_TIMEOUT( !manager.hasActiveRequest(), 10000 );
  if ( !answered() )
  {
    QStringList transcript;
    for ( const QgsAiChatMessage &message : manager.history() )
      transcript << qgsAiChatRoleToString( message.role ) + u": "_s + message.content.left( 200 );
    QFAIL( qPrintable( transcript.join( '\n' ) ) );
  }

  QVERIFY2( liveTitle.startsWith( "set_value"_L1 ), qPrintable( liveTitle ) );
  QTRY_VERIFY( !dock.findChild<QFrame *>( u"aiLiveToolCard"_s ) );

  // The result card says what changed, without raw tokens, and can be undone.
  bool summaryShown = false;
  for ( QLabel *body : dock.findChildren<QLabel *>( u"aiMessageBody"_s ) )
  {
    summaryShown = summaryShown || body->text().contains( u"Changed value 1."_s );
    QVERIFY( !body->text().contains( u"tok_1"_s ) );
  }
  QVERIFY( summaryShown );
  QPushButton *undo = dock.findChild<QPushButton *>( u"aiUndoToolButton"_s );
  QVERIFY( undo && undo->isEnabled() );
  // Messages have their actions: Copy on the answer, Copy, Edit and Retry on the question.
  QCOMPARE( dock.findChildren<QToolButton *>( u"aiCopyMessageButton"_s ).size(), 2 );
  QVERIFY( dock.findChild<QToolButton *>( u"aiEditMessageButton"_s ) );
  QVERIFY( dock.findChild<QToolButton *>( u"aiRetryMessageButton"_s ) );
  QVERIFY( dock.findChild<QPushButton *>( u"aiRequestErrorRetry"_s ) );
  QPushButton *undoTurn = dock.findChild<QPushButton *>( u"aiUndoTurnButton"_s );
  QVERIFY( undoTurn && !undoTurn->isHidden() );

  undoTurn->click();
  QCOMPARE( undone, QStringList( { u"tok_1"_s } ) );
  QTRY_VERIFY( dock.findChild<QLabel *>( u"aiToolUndoneLabel"_s ) );
  QTRY_VERIFY( !dock.findChild<QPushButton *>( u"aiUndoToolButton"_s ) );
}

void TestQgsAiChatDockWidget::messagesTypedDuringATurnAreQueued()
{
  const auto isolated = isolatePlanModelPickerState();
  QgsSettings settings;
  settings.remove( u"ai/provider/openrouter"_s );
  settings.remove( u"strata/agent"_s );
  const auto cleanup = qScopeGuard( [&settings]() {
    settings.remove( u"ai/provider/openrouter"_s );
    settings.remove( u"ai/network/maxRetries"_s );
    settings.remove( u"strata/agent"_s );
  } );

  QgsAiTestLoopbackServer server;
  QgsAiTestLoopbackServer::ScriptedResponse slow
    = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"choices":[{"message":{"role":"assistant","content":"First answer"},"finish_reason":"stop"}]})" ) );
  slow.responseDelayMs = 800;
  server.responses << slow << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( R"({"choices":[{"message":{"role":"assistant","content":"Second answer"},"finish_reason":"stop"}]})" ) );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );
  qputenv( "OPENROUTER_API_KEY", "sk-or-loopback-test" );
  settings.setValue( u"ai/network/maxRetries"_s, 0 );

  QgsAiModelRouter router;
  QgsAiModelRouter::ProviderSettings providerSettings = router.providerSettings( QgsAiModelRouter::Provider::OpenRouter );
  providerSettings.endpoint = u"http://127.0.0.1:%1/api/v1/chat/completions"_s.arg( server.serverPort() );
  providerSettings.model = u"test/model"_s;
  providerSettings.enabled = true;
  router.setProviderSettings( QgsAiModelRouter::Provider::OpenRouter, providerSettings );
  router.setActiveProvider( QgsAiModelRouter::Provider::OpenRouter );

  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( &router, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  dock.show();
  const QString trustRoot = QgsAiWorkspaceTrust::currentWorkspaceRoot();
  const QgsAiWorkspaceTrust::State savedTrust = trustRoot.isEmpty() ? QgsAiWorkspaceTrust::State::Unknown : QgsAiWorkspaceTrust::state( trustRoot );
  if ( !trustRoot.isEmpty() )
    QgsAiWorkspaceTrust::setState( trustRoot, QgsAiWorkspaceTrust::State::Trusted );
  const auto restoreTrust = qScopeGuard( [trustRoot, savedTrust]() {
    if ( !trustRoot.isEmpty() )
      QgsAiWorkspaceTrust::setState( trustRoot, savedTrust );
  } );

  QgsAiChatPromptEdit *input = dock.findChild<QgsAiChatPromptEdit *>( u"aiPromptInput"_s );
  QWidget *queueBar = dock.findChild<QWidget *>( u"aiQueueBar"_s );
  QVERIFY( input && queueBar );
  input->setPlainText( u"first question"_s );
  QVERIFY( QMetaObject::invokeMethod( &dock, "sendMessage", Qt::DirectConnection ) );
  QTRY_VERIFY_WITH_TIMEOUT( manager.hasActiveRequest(), 10000 );

  // The message box stays open while the assistant works; what is sent now waits in a queue.
  QVERIFY( input->isEnabled() );
  input->setPlainText( u"second question"_s );
  QVERIFY( QMetaObject::invokeMethod( &dock, "sendMessage", Qt::DirectConnection ) );
  QCOMPARE( dock.queuedMessageCount(), 1 );
  QVERIFY( !queueBar->isHidden() );
  QVERIFY( input->toPlainText().isEmpty() );

  const auto contents = [&manager]() {
    QStringList texts;
    for ( const QgsAiChatMessage &message : manager.history() )
      texts << message.content;
    return texts;
  };
  QTRY_VERIFY_WITH_TIMEOUT( contents().contains( u"Second answer"_s ), 60000 );
  QCOMPARE( contents(), QStringList( { u"first question"_s, u"First answer"_s, u"second question"_s, u"Second answer"_s } ) );
  QCOMPARE( dock.queuedMessageCount(), 0 );
  QVERIFY( queueBar->isHidden() );
}

void TestQgsAiChatDockWidget::emptyChatSuggestsPromptsForTheProject()
{
  QgsProject::instance()->clear();
  const auto cleanup = qScopeGuard( []() { QgsProject::instance()->clear(); } );
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  dock.show();

  // An empty chat offers prompts that work, even without a project.
  QTRY_VERIFY( dock.findChild<QFrame *>( u"aiEmptyState"_s ) );
  QVERIFY( dock.findChildren<QPushButton *>( u"aiSuggestedPrompt"_s ).size() >= 3 );

  // With a layer, the prompts name it.
  QgsVectorLayer *layer = new QgsVectorLayer( u"Polygon?crs=EPSG:3003"_s, u"Parcels"_s, u"memory"_s );
  QgsProject::instance()->addMapLayer( layer );
  const auto hasPrompt = [&dock]( const QString &text ) {
    const QList<QPushButton *> prompts = dock.findChildren<QPushButton *>( u"aiSuggestedPrompt"_s );
    return std::any_of( prompts.cbegin(), prompts.cend(), [&text]( QPushButton *button ) { return button->text() == text; } );
  };
  QTRY_VERIFY_WITH_TIMEOUT( hasPrompt( u"Buffer Parcels by 100 m and add the result to the map."_s ), 5000 );

  // A click sends the prompt, and the suggestions go away.
  const QString trustRoot = QgsAiWorkspaceTrust::currentWorkspaceRoot();
  const QgsAiWorkspaceTrust::State savedTrust = trustRoot.isEmpty() ? QgsAiWorkspaceTrust::State::Unknown : QgsAiWorkspaceTrust::state( trustRoot );
  if ( !trustRoot.isEmpty() )
    QgsAiWorkspaceTrust::setState( trustRoot, QgsAiWorkspaceTrust::State::Trusted );
  const auto restoreTrust = qScopeGuard( [trustRoot, savedTrust]() {
    if ( !trustRoot.isEmpty() )
      QgsAiWorkspaceTrust::setState( trustRoot, savedTrust );
  } );
  const QList<QPushButton *> prompts = dock.findChildren<QPushButton *>( u"aiSuggestedPrompt"_s );
  for ( QPushButton *button : prompts )
  {
    if ( button->text().startsWith( "Buffer Parcels"_L1 ) )
    {
      button->click();
      break;
    }
  }
  QVERIFY( !manager.history().isEmpty() );
  QCOMPARE( manager.history().first().content, u"Buffer Parcels by 100 m and add the result to the map."_s );
  QTRY_VERIFY( !dock.findChild<QFrame *>( u"aiEmptyState"_s ) );
}

void TestQgsAiChatDockWidget::acceptingPlanWithDisallowedToolsStaysInAgentAndBlocks()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<QgsAiEchoTool>() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  manager.setToolRegistry( &registry );
  // Tools turned off in the settings make the Agent allowlist empty.
  QgsAiAgentBehaviorSettings behavior = manager.agentBehaviorSettings();
  behavior.allowCustomActions = false;
  manager.setAgentBehaviorSettings( behavior );
  const QScopeGuard restoreTools( [] { QgsSettings().remove( u"strata/agent"_s ); } );

  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QJsonObject step;
  step.insert( u"id"_s, u"s1"_s );
  step.insert( u"title"_s, u"Echo probe"_s );
  step.insert( u"tool"_s, u"echo"_s );
  step.insert( u"risk"_s, u"low"_s );
  step.insert( u"requires_approval"_s, false );
  step.insert( u"depends_on"_s, QJsonArray() );

  QJsonObject plan;
  plan.insert( u"version"_s, 1 );
  plan.insert( u"objective"_s, u"Probe allowlist gate"_s );
  plan.insert( u"mode"_s, u"auto_edit"_s );
  plan.insert( u"steps"_s, QJsonArray { step } );
  const QString planJson = QString::fromUtf8( QJsonDocument( plan ).toJson( QJsonDocument::Compact ) );

  QgsAiChatMessage planMessage;
  planMessage.id = u"blocked-plan-1"_s;
  planMessage.role = QgsAiChatRole::Assistant;
  planMessage.content = u"Ready.\n```strata_agent_plan\n%1\n```"_s.arg( planJson );
  planMessage.metadata.insert( u"ui_kind"_s, u"agent_plan"_s );
  planMessage.metadata.insert( u"plan_json"_s, planJson );
  planMessage.metadata.insert( u"plan_status"_s, u"pending"_s );
  manager.appendHistoryMessage( planMessage );

  QPushButton *accept = dock.findChild<QPushButton *>( u"aiAcceptPlanButton"_s );
  QVERIFY( accept );
  QVERIFY( accept->isEnabled() );
  accept->click();
  // reloadTranscriptFromHistory() clears cards with deleteLater(); flush them so
  // findChild does not return the stale pending Accept button.
  QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
  QApplication::processEvents();

  QCOMPARE( manager.activeAgent(), u"editor"_s );
  bool sawBlocked = false;
  bool sawExecutePrompt = false;
  bool sawNotice = false;
  for ( const QgsAiChatMessage &message : manager.history() )
  {
    if ( message.id == planMessage.id )
      sawBlocked = message.metadata.value( u"plan_status"_s ).toString() == "blocked"_L1;
    if ( message.role == QgsAiChatRole::User && message.content.contains( u"Execute the accepted plan"_s ) )
      sawExecutePrompt = true;
    if ( message.role == QgsAiChatRole::Assistant && message.content.contains( u"echo"_s ) && message.content.contains( u"blocked"_s, Qt::CaseInsensitive ) )
      sawNotice = true;
  }
  QVERIFY( sawBlocked );
  QVERIFY( !sawExecutePrompt );
  QVERIFY( sawNotice );

  QPushButton *reject = dock.findChild<QPushButton *>( u"aiRejectPlanButton"_s );
  QVERIFY( reject );
  QVERIFY( reject->isEnabled() );
  QCOMPARE( reject->property( "plan_status" ).toString(), u"blocked"_s );
  accept = dock.findChild<QPushButton *>( u"aiAcceptPlanButton"_s );
  QVERIFY( accept );
  QCOMPARE( accept->property( "plan_status" ).toString(), u"blocked"_s );
  QVERIFY( !accept->isEnabled() );
}

void TestQgsAiChatDockWidget::acceptingPlanWithAllowedToolsStaysInAgentAndExecutes()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsSettings settings;
  settings.remove( u"strata/agent"_s );
  const auto cleanup = qScopeGuard( [&settings]() { settings.remove( u"strata/agent"_s ); } );

  QgsAiToolRegistry registry;
  registry.registerTool( std::make_unique<QgsAiEchoTool>() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  manager.setToolRegistry( &registry );

  QgsAiAgentBehaviorSettings behavior = manager.agentBehaviorSettings();
  behavior.allowCustomActions = true;
  manager.setAgentBehaviorSettings( behavior );

  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QJsonObject step;
  step.insert( u"id"_s, u"s1"_s );
  step.insert( u"title"_s, u"Echo probe"_s );
  step.insert( u"tool"_s, u"echo"_s );
  step.insert( u"risk"_s, u"low"_s );
  step.insert( u"requires_approval"_s, false );
  step.insert( u"depends_on"_s, QJsonArray() );

  QJsonObject plan;
  plan.insert( u"version"_s, 1 );
  plan.insert( u"objective"_s, u"Execute with allowlist"_s );
  plan.insert( u"mode"_s, u"auto_edit"_s );
  plan.insert( u"steps"_s, QJsonArray { step } );
  const QString planJson = QString::fromUtf8( QJsonDocument( plan ).toJson( QJsonDocument::Compact ) );

  QgsAiChatMessage planMessage;
  planMessage.id = u"allowed-plan-1"_s;
  planMessage.role = QgsAiChatRole::Assistant;
  planMessage.content = u"Ready.\n```strata_agent_plan\n%1\n```"_s.arg( planJson );
  planMessage.metadata.insert( u"ui_kind"_s, u"agent_plan"_s );
  planMessage.metadata.insert( u"plan_json"_s, planJson );
  planMessage.metadata.insert( u"plan_status"_s, u"pending"_s );
  manager.appendHistoryMessage( planMessage );

  QPushButton *accept = dock.findChild<QPushButton *>( u"aiAcceptPlanButton"_s );
  QVERIFY( accept );
  accept->click();

  QCOMPARE( manager.activeAgent(), u"editor"_s );
  QVERIFY( !manager.history().isEmpty() );
  bool sawExecutePrompt = false;
  bool sawBlockedNotice = false;
  for ( const QgsAiChatMessage &message : manager.history() )
  {
    if ( message.role == QgsAiChatRole::User && message.content.contains( u"Execute the accepted plan"_s ) )
      sawExecutePrompt = true;
    if ( message.role == QgsAiChatRole::Assistant && message.content.contains( u"execution is blocked"_s ) )
      sawBlockedNotice = true;
    if ( message.id == planMessage.id )
      QCOMPARE( message.metadata.value( u"plan_status"_s ).toString(), u"accepted"_s );
  }
  QVERIFY( sawExecutePrompt );
  QVERIFY( !sawBlockedNotice );
}

void TestQgsAiChatDockWidget::cancelClearsOrphanStreamingAssistantCard()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  manager.requestRunningChanged( true );
  manager.responseChunkReceived( u"partial stream"_s );
  QVERIFY( dock.findChild<QFrame *>( u"aiStreamingMessage"_s ) );

  manager.requestRunningChanged( false );
  QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
  QApplication::processEvents();

  QVERIFY( !dock.findChild<QFrame *>( u"aiStreamingMessage"_s ) );
}

void TestQgsAiChatDockWidget::workflowComposerExportsReportAndDryRun()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QgsAiChatMessage planMessage;
  planMessage.id = u"workflow-plan-1"_s;
  planMessage.role = QgsAiChatRole::Assistant;
  planMessage.content = u"<proposed_plan>\n1. Inspect layer\n2. Export map\n</proposed_plan>"_s;
  planMessage.metadata.insert( u"ui_kind"_s, u"plan"_s );
  planMessage.metadata.insert( u"plan_markdown"_s, u"1. Inspect layer\n2. Export map"_s );
  planMessage.metadata.insert( u"plan_status"_s, u"pending"_s );

  manager.messageAdded( planMessage );
  QPushButton *dryRun = dock.findChild<QPushButton *>( u"aiDryRunWorkflowButton"_s );
  QPushButton *runWorkflow = dock.findChild<QPushButton *>( u"aiRunWorkflowButton"_s );
  QPushButton *exportReport = dock.findChild<QPushButton *>( u"aiExportWorkflowReportButton"_s );
  QVERIFY( dryRun );
  QVERIFY( runWorkflow );
  QVERIFY( exportReport );

  exportReport->click();
  QDir workflowDir( QDir( tempDir.path() ).filePath( u".strata/workflows"_s ) );
  const QFileInfoList reports = workflowDir.entryInfoList( QStringList { u"*.report.json"_s }, QDir::Files );
  QCOMPARE( reports.size(), 1 );
  QFile reportFile( reports.first().absoluteFilePath() );
  QVERIFY( reportFile.open( QIODevice::ReadOnly ) );
  const QJsonObject report = QJsonDocument::fromJson( reportFile.readAll() ).object();
  QCOMPARE( report.value( u"kind"_s ).toString(), u"strataflow_report"_s );
  QVERIFY( report.value( u"workflowPath"_s ).toString().endsWith( ".strataflow"_L1 ) );
  QVERIFY( report.value( u"provenance"_s ).toObject().value( u"metadataOnly"_s ).toBool() );

  dryRun->click();
  QCOMPARE( manager.activeAgent(), u"planner"_s );
  QVERIFY( !manager.history().isEmpty() );
  QVERIFY( manager.history().first().content.contains( u"Dry-run this .strataflow workflow"_s ) );
  QVERIFY( manager.history().first().content.contains( u"Do not call mutating tools"_s ) );

  runWorkflow->click();
  QCOMPARE( manager.activeAgent(), u"editor"_s );
  QStringList history;
  for ( const QgsAiChatMessage &message : manager.history() )
    history << message.content;
  QVERIFY( history.join( '\n' ).contains( u"Run this .strataflow workflow"_s ) );
  QVERIFY( history.join( '\n' ).contains( u"per-tool safety checks"_s ) );
}

void TestQgsAiChatDockWidget::questionCardSendsStructuredAnswers()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QJsonObject option;
  option.insert( u"id"_s, u"default"_s );
  option.insert( u"label"_s, u"Default"_s );
  option.insert( u"description"_s, u"Recommended"_s );
  QJsonObject question;
  question.insert( u"id"_s, u"scope"_s );
  question.insert( u"type"_s, u"single"_s );
  question.insert( u"question"_s, u"Which scope?"_s );
  QJsonArray options;
  options.append( option );
  question.insert( u"options"_s, options );
  question.insert( u"allow_other"_s, true );
  QJsonObject payload;
  QJsonArray questions;
  questions.append( question );
  payload.insert( u"questions"_s, questions );

  QgsAiChatMessage questionsMessage;
  questionsMessage.id = u"questions-1"_s;
  questionsMessage.role = QgsAiChatRole::Assistant;
  questionsMessage.content = u"Need one decision.\n```qgis_ai_questions\n{}\n```"_s;
  questionsMessage.metadata.insert( u"ui_kind"_s, u"questions"_s );
  questionsMessage.metadata.insert( u"questions_json"_s, QString::fromUtf8( QJsonDocument( payload ).toJson( QJsonDocument::Compact ) ) );
  questionsMessage.metadata.insert( u"questions_status"_s, u"pending"_s );

  manager.messageAdded( questionsMessage );

  QRadioButton *optionButton = dock.findChild<QRadioButton *>( u"aiQuestionOption"_s );
  QPushButton *submit = dock.findChild<QPushButton *>( u"aiSubmitQuestionAnswersButton"_s );
  QVERIFY( optionButton );
  QVERIFY( submit );
  optionButton->setChecked( true );
  submit->click();

  QCOMPARE( manager.activeAgent(), u"planner"_s );
  QVERIFY( !manager.history().isEmpty() );
  QVERIFY( manager.history().first().content.contains( u"qgis_ai_answers"_s ) );
  QVERIFY( manager.history().first().content.contains( u"default"_s ) );
}

void TestQgsAiChatDockWidget::toolLimitMessageShowsContinueButton()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;

  {
    QgsAiAgentSessionManager manager( &router, &contextProvider, &reviewEngine );
    QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

    QgsAiChatMessage limitMessage;
    limitMessage.id = u"tool-limit-pending"_s;
    limitMessage.role = QgsAiChatRole::Assistant;
    limitMessage.content = u"Maximum number reached."_s;
    limitMessage.metadata.insert( u"ui_kind"_s, u"tool_limit"_s );
    limitMessage.metadata.insert( u"tool_limit_status"_s, u"pending"_s );
    limitMessage.metadata.insert( u"tool_limit"_s, 5 );

    manager.messageAdded( limitMessage );
    QPushButton *continueButton = dock.findChild<QPushButton *>( u"aiContinueToolLimitButton"_s );
    QVERIFY( continueButton );
    QCOMPARE( continueButton->text(), u"Continue"_s );
    QVERIFY( continueButton->isEnabled() );

    manager.requestRunningChanged( true );
    QVERIFY( !continueButton->isEnabled() );
    manager.requestRunningChanged( false );
    QVERIFY( continueButton->isEnabled() );
  }

  {
    QgsAiAgentSessionManager manager( &router, &contextProvider, &reviewEngine );
    QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

    QgsAiChatMessage limitMessage;
    limitMessage.id = u"tool-limit-continued"_s;
    limitMessage.role = QgsAiChatRole::Assistant;
    limitMessage.content = u"Maximum number reached."_s;
    limitMessage.metadata.insert( u"ui_kind"_s, u"tool_limit"_s );
    limitMessage.metadata.insert( u"tool_limit_status"_s, u"continued"_s );
    limitMessage.metadata.insert( u"tool_limit"_s, 5 );

    manager.messageAdded( limitMessage );
    QPushButton *continueButton = dock.findChild<QPushButton *>( u"aiContinueToolLimitButton"_s );
    QVERIFY( continueButton );
    QVERIFY( !continueButton->isEnabled() );
  }
}

void TestQgsAiChatDockWidget::settingsDialogContainsManualIndexingControls()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsSettings settings;
  const QString openAiKey = u"ai/provider/openai/apiKey"_s;
  const QString legacyEmbeddingProviderKey = u"ai/embeddings/provider"_s;
  const QString embeddingProviderKey = u"strata/index/embedding_provider"_s;
  const QString automaticIndexingKey = u"strata/index/automatic"_s;
  const QString layerIndexingKey = u"strata/index/enable_layer_indexing"_s;
  const QString privacyMetadataOnlyKey = u"strata/privacy/metadata_only_ack"_s;
  const QString telemetryOptInKey = u"strata/telemetry/opt_in"_s;
  const QString crashOptInKey = u"strata/crash_reporting/metadata_only_opt_in"_s;
  const QString releaseDryRunChecksumKey = u"strata/release/dry_run_checksum"_s;
  const QString releaseDryRunManifestKey = u"strata/release/dry_run_manifest"_s;
  const QString maxToolIterationsKey = u"strata/agent/max_tool_iterations_per_turn"_s;
  const bool hadE5ModelDirEnv = qEnvironmentVariableIsSet( "STRATA_AI_EMBEDDING_MODEL_DIR" );
  const QByteArray savedE5ModelDirEnv = qgetenv( "STRATA_AI_EMBEDDING_MODEL_DIR" );
  const bool hadOpenAiKey = settings.contains( openAiKey );
  const QVariant savedOpenAiKey = settings.value( openAiKey );
  const bool hadLegacyEmbeddingProvider = settings.contains( legacyEmbeddingProviderKey );
  const QVariant savedLegacyEmbeddingProvider = settings.value( legacyEmbeddingProviderKey );
  const bool hadEmbeddingProvider = settings.contains( embeddingProviderKey );
  const QVariant savedEmbeddingProvider = settings.value( embeddingProviderKey );
  const bool hadAutomaticIndexing = settings.contains( automaticIndexingKey );
  const QVariant savedAutomaticIndexing = settings.value( automaticIndexingKey );
  const bool hadLayerIndexing = settings.contains( layerIndexingKey );
  const QVariant savedLayerIndexing = settings.value( layerIndexingKey );
  const bool hadPrivacyMetadataOnly = settings.contains( privacyMetadataOnlyKey );
  const QVariant savedPrivacyMetadataOnly = settings.value( privacyMetadataOnlyKey );
  const bool hadTelemetryOptIn = settings.contains( telemetryOptInKey );
  const QVariant savedTelemetryOptIn = settings.value( telemetryOptInKey );
  const bool hadCrashOptIn = settings.contains( crashOptInKey );
  const QVariant savedCrashOptIn = settings.value( crashOptInKey );
  const bool hadReleaseDryRunChecksum = settings.contains( releaseDryRunChecksumKey );
  const QVariant savedReleaseDryRunChecksum = settings.value( releaseDryRunChecksumKey );
  const bool hadReleaseDryRunManifest = settings.contains( releaseDryRunManifestKey );
  const QVariant savedReleaseDryRunManifest = settings.value( releaseDryRunManifestKey );
  const bool hadMaxToolIterations = settings.contains( maxToolIterationsKey );
  const QVariant savedMaxToolIterations = settings.value( maxToolIterationsKey );
  settings.setValue( openAiKey, u"sk-old-test-key"_s );
  settings.setValue( legacyEmbeddingProviderKey, u"openai"_s );
  settings.remove( embeddingProviderKey );
  settings.setValue( automaticIndexingKey, true );
  settings.setValue( layerIndexingKey, true );
  settings.setValue( privacyMetadataOnlyKey, true );
  settings.remove( maxToolIterationsKey );
  settings.remove( releaseDryRunChecksumKey );
  settings.remove( releaseDryRunManifestKey );
  qputenv( "STRATA_AI_EMBEDDING_MODEL_DIR", QFile::encodeName( QDir( tempDir.path() ).filePath( u"missing-e5"_s ) ) );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  const bool e5ProviderListed = QgsAiEmbeddingProviderRegistry::providerIds().contains( QgsAiE5EmbeddingProvider::staticProviderId() );
  std::unique_ptr<QgsAiEmbeddingProvider> embeddingProvider = QgsAiEmbeddingProviderRegistry::createProviderFromSettings();
  QgsAiWorkspaceIndex workspaceIndex( &contextProvider, embeddingProvider.get() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  manager.setWorkspaceIndex( &workspaceIndex );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  bool inspected = false;
  bool controlsFound = false;
  bool layerIndexingChecked = true;
  bool layerIndexingEnabled = true;
  bool localStatusFound = false;
  bool downloadButtonFound = false;
  bool defaultProviderSelected = false;
  bool e5UiStateFound = false;
  bool onboardingControlsFound = false;
  bool releaseDryRunOk = false;
  bool agentLimitControlFound = false;
  bool claudeControlsFound = false;
  QTimer::singleShot(
    0,
    &dock,
    [&inspected,
     &controlsFound,
     &layerIndexingChecked,
     &layerIndexingEnabled,
     &localStatusFound,
     &downloadButtonFound,
     &defaultProviderSelected,
     &e5UiStateFound,
     &onboardingControlsFound,
     &releaseDryRunOk,
     &agentLimitControlFound,
     &claudeControlsFound,
     e5ProviderListed]() {
      QDialog *settingsDialog = qobject_cast<QDialog *>( QApplication::activeModalWidget() );
      if ( settingsDialog )
      {
        // The dialog opens on the Account section; the visibility checks below
        // need the Indexing page to be the current stacked page.
        QMetaObject::invokeMethod( settingsDialog, "showSection", Qt::DirectConnection, Q_ARG( QString, u"indexing"_s ) );

        QCheckBox *layerIndexing = settingsDialog->findChild<QCheckBox *>( u"aiEnableLayerIndexingCheckBox"_s );
        QComboBox *providerCombo = settingsDialog->findChild<QComboBox *>( u"aiEmbeddingProviderComboBox"_s );
        QLabel *statusLabel = settingsDialog->findChild<QLabel *>( u"aiEmbeddingProviderStatusLabel"_s );
        QPushButton *downloadButton = settingsDialog->findChild<QPushButton *>( u"aiDownloadEmbeddingModelButton"_s );
        controlsFound = layerIndexing
                        && providerCombo
                        && settingsDialog->findChild<QCheckBox *>( u"aiAutomaticIndexingCheckBox"_s )
                        && settingsDialog->findChild<QPushButton *>( u"aiRebuildWorkspaceIndexButton"_s )
                        && settingsDialog->findChild<QPushButton *>( u"aiRebuildLayerIndexButton"_s );
        if ( layerIndexing )
        {
          layerIndexingChecked = layerIndexing->isChecked();
          layerIndexingEnabled = layerIndexing->isEnabled();
        }
        defaultProviderSelected = providerCombo && providerCombo->currentData().toString() == QgsAiEmbeddingProviderRegistry::defaultProviderId();
        if ( providerCombo )
        {
          const int e5Row = providerCombo->findData( QgsAiE5EmbeddingProvider::staticProviderId() );
          const QModelIndex e5Index = providerCombo->model()->index( e5Row, 0 );
          e5UiStateFound = e5Row >= 0 && e5Index.isValid() && static_cast<bool>( providerCombo->model()->flags( e5Index ) & Qt::ItemIsEnabled ) == e5ProviderListed;
        }
        localStatusFound = e5ProviderListed ? statusLabel && statusLabel->text().contains( u"E5"_s, Qt::CaseInsensitive ) && statusLabel->text().contains( u"not installed"_s, Qt::CaseInsensitive )
                                            : statusLabel && statusLabel->text().contains( u"MinHash"_s, Qt::CaseInsensitive ) && statusLabel->text().contains( u"available"_s, Qt::CaseInsensitive );
        downloadButtonFound = downloadButton && downloadButton->isVisible() == e5ProviderListed;
        QLabel *onboardingStatus = settingsDialog->findChild<QLabel *>( u"aiOnboardingStatusLabel"_s );
        QPushButton *releaseDryRunButton = settingsDialog->findChild<QPushButton *>( u"aiReleaseDryRunButton"_s );
        QLabel *releaseDryRunStatus = settingsDialog->findChild<QLabel *>( u"aiReleaseDryRunStatusLabel"_s );
        onboardingControlsFound = onboardingStatus
                                  && onboardingStatus->text().contains( u"Plan login"_s )
                                  && settingsDialog->findChild<QCheckBox *>( u"aiPrivacyMetadataOnlyCheckBox"_s )
                                  && settingsDialog->findChild<QCheckBox *>( u"aiTelemetryOptInCheckBox"_s )
                                  && settingsDialog->findChild<QCheckBox *>( u"aiCrashReportOptInCheckBox"_s )
                                  && settingsDialog->findChild<QPushButton *>( u"aiCreateDemoProjectButton"_s )
                                  && releaseDryRunButton
                                  && releaseDryRunStatus;
        if ( releaseDryRunButton && releaseDryRunStatus )
        {
          releaseDryRunButton->click();
          releaseDryRunOk = releaseDryRunStatus->text().contains( u"checksum"_s, Qt::CaseInsensitive ) && !releaseDryRunStatus->property( "checksum" ).toString().isEmpty();
        }
        QSpinBox *maxToolIterations = settingsDialog->findChild<QSpinBox *>( u"aiMaxToolIterationsPerTurnSpinBox"_s );
        agentLimitControlFound = maxToolIterations
                                 && maxToolIterations->minimum() == QgsAiAgentBehaviorSettings::MIN_TOOL_CALL_PAUSE_LIMIT
                                 && maxToolIterations->maximum() == QgsAiAgentBehaviorSettings::MAX_TOOL_CALL_PAUSE_LIMIT
                                 && maxToolIterations->value() == QgsAiAgentBehaviorSettings::DEFAULT_TOOL_CALL_PAUSE_LIMIT;
        claudeControlsFound = settingsDialog->findChild<QLabel *>( u"aiClaudeLoginStatus"_s )
                              && settingsDialog->findChild<QPushButton *>( u"aiClaudeConnectButton"_s )
                              && settingsDialog->findChild<QPushButton *>( u"aiClaudeCloudButton"_s )
                              && settingsDialog->findChild<QLineEdit *>( u"aiClaudeApiKeyLineEdit"_s )
                              && !settingsDialog->findChild<QLabel *>( u"aiClaudeSuspensionNotice"_s )
                              && !settingsDialog->findChild<QLineEdit *>( u"aiClaudeManualTokenLineEdit"_s );
        settingsDialog->reject();
      }
      inspected = true;
    }
  );

  const bool invoked = QMetaObject::invokeMethod( &dock, "openProviderSettings", Qt::DirectConnection );

  if ( hadOpenAiKey )
    settings.setValue( openAiKey, savedOpenAiKey );
  else
    settings.remove( openAiKey );
  if ( hadLegacyEmbeddingProvider )
    settings.setValue( legacyEmbeddingProviderKey, savedLegacyEmbeddingProvider );
  else
    settings.remove( legacyEmbeddingProviderKey );
  if ( hadEmbeddingProvider )
    settings.setValue( embeddingProviderKey, savedEmbeddingProvider );
  else
    settings.remove( embeddingProviderKey );
  if ( hadAutomaticIndexing )
    settings.setValue( automaticIndexingKey, savedAutomaticIndexing );
  else
    settings.remove( automaticIndexingKey );
  if ( hadLayerIndexing )
    settings.setValue( layerIndexingKey, savedLayerIndexing );
  else
    settings.remove( layerIndexingKey );
  if ( hadPrivacyMetadataOnly )
    settings.setValue( privacyMetadataOnlyKey, savedPrivacyMetadataOnly );
  else
    settings.remove( privacyMetadataOnlyKey );
  if ( hadTelemetryOptIn )
    settings.setValue( telemetryOptInKey, savedTelemetryOptIn );
  else
    settings.remove( telemetryOptInKey );
  if ( hadCrashOptIn )
    settings.setValue( crashOptInKey, savedCrashOptIn );
  else
    settings.remove( crashOptInKey );
  if ( hadReleaseDryRunChecksum )
    settings.setValue( releaseDryRunChecksumKey, savedReleaseDryRunChecksum );
  else
    settings.remove( releaseDryRunChecksumKey );
  if ( hadReleaseDryRunManifest )
    settings.setValue( releaseDryRunManifestKey, savedReleaseDryRunManifest );
  else
    settings.remove( releaseDryRunManifestKey );
  if ( hadMaxToolIterations )
    settings.setValue( maxToolIterationsKey, savedMaxToolIterations );
  else
    settings.remove( maxToolIterationsKey );
  qunsetenv( "STRATA_AI_EMBEDDING_MODEL_DIR" );
  if ( hadE5ModelDirEnv )
    qputenv( "STRATA_AI_EMBEDDING_MODEL_DIR", savedE5ModelDirEnv );

  QVERIFY( invoked );
  QVERIFY( inspected );
  QVERIFY( controlsFound );
  QVERIFY( localStatusFound );
  QVERIFY( downloadButtonFound );
  QVERIFY( defaultProviderSelected );
  QVERIFY( e5UiStateFound );
  QVERIFY( onboardingControlsFound );
  QVERIFY( releaseDryRunOk );
  QVERIFY( agentLimitControlFound );
  QVERIFY( claudeControlsFound );
  QCOMPARE( layerIndexingChecked, !e5ProviderListed );
  QCOMPARE( layerIndexingEnabled, !e5ProviderListed );
}

void TestQgsAiChatDockWidget::historyMenuPromptsForSavedProjectWhenUnsaved()
{
  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( QString {} );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );

  QToolButton *historyButton = dock.findChild<QToolButton *>( u"aiHistoryButton"_s );
  QVERIFY( historyButton );
  QVERIFY( historyButton->menu() );

  QVERIFY( QMetaObject::invokeMethod( &dock, "rebuildHistoryMenu", Qt::DirectConnection ) );

  const QList<QAction *> actions = historyButton->menu()->actions();
  QCOMPARE( actions.size(), 1 );
  QVERIFY( actions.first()->text().contains( u"QGIS project"_s, Qt::CaseInsensitive ) );
  QVERIFY( !actions.first()->isEnabled() );
}

void TestQgsAiChatDockWidget::historyMenuDoesNotShowWorkspaceHistoryForUnsavedProject()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiChatHistoryStore store( &contextProvider );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  manager.setHistoryStore( &store );

  QVERIFY( store.createSession( u"legacy-workspace-chat"_s, u"Legacy workspace chat"_s, u"planner"_s ) );
  manager.resetProjectChatHistoryScope();

  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  QToolButton *historyButton = dock.findChild<QToolButton *>( u"aiHistoryButton"_s );
  QVERIFY( historyButton );
  QVERIFY( historyButton->menu() );

  QVERIFY( QMetaObject::invokeMethod( &dock, "rebuildHistoryMenu", Qt::DirectConnection ) );

  const QList<QAction *> actions = historyButton->menu()->actions();
  QCOMPARE( actions.size(), 1 );
  QVERIFY( actions.first()->text().contains( u"QGIS project"_s, Qt::CaseInsensitive ) );
  QVERIFY( !actions.first()->text().contains( u"Legacy workspace chat"_s ) );
  QVERIFY( !actions.first()->isEnabled() );
}

void TestQgsAiChatDockWidget::historyMenuShowsOnlyCurrentProjectSessions()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiChatHistoryStore store( &contextProvider );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  manager.setHistoryStore( &store );

  const QString projectScope1 = QgsAiAgentSessionManager::chatHistoryScopeKeyForProjectFile( tempDir.filePath( u"one.qgz"_s ) );
  const QString projectScope2 = QgsAiAgentSessionManager::chatHistoryScopeKeyForProjectFile( tempDir.filePath( u"two.qgz"_s ) );

  manager.setProjectChatHistoryScopeKey( projectScope1 );
  manager.sendUserMessage( u"project alpha"_s );
  manager.setProjectChatHistoryScopeKey( projectScope2 );
  manager.sendUserMessage( u"project beta"_s );

  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  QToolButton *historyButton = dock.findChild<QToolButton *>( u"aiHistoryButton"_s );
  QVERIFY( historyButton );
  QVERIFY( historyButton->menu() );

  QVERIFY( QMetaObject::invokeMethod( &dock, "rebuildHistoryMenu", Qt::DirectConnection ) );
  QStringList projectTwoLabels;
  for ( QAction *action : historyButton->menu()->actions() )
    projectTwoLabels << action->text();
  const QString projectTwoMenu = projectTwoLabels.join( '\n' );
  QVERIFY( projectTwoMenu.contains( u"project beta"_s ) );
  QVERIFY( !projectTwoMenu.contains( u"project alpha"_s ) );

  manager.setProjectChatHistoryScopeKey( projectScope1 );
  QVERIFY( QMetaObject::invokeMethod( &dock, "rebuildHistoryMenu", Qt::DirectConnection ) );
  QStringList projectOneLabels;
  for ( QAction *action : historyButton->menu()->actions() )
    projectOneLabels << action->text();
  const QString projectOneMenu = projectOneLabels.join( '\n' );
  QVERIFY( projectOneMenu.contains( u"project alpha"_s ) );
  QVERIFY( !projectOneMenu.contains( u"project beta"_s ) );
}

void TestQgsAiChatDockWidget::dropLocalFileCreatesAttachmentChip()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  const QString filePath = tempDir.filePath( u"notes.txt"_s );
  QFile file( filePath );
  QVERIFY( file.open( QIODevice::WriteOnly ) );
  file.write( "sample attachment" );
  file.close();

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  dock.resize( 400, 520 );
  dock.show();
  QApplication::processEvents();

  QgsAiChatPromptEdit *input = dock.findChild<QgsAiChatPromptEdit *>( u"aiPromptInput"_s );
  QWidget *chipRow = dock.findChild<QWidget *>( u"aiAttachmentChipRow"_s );
  QVERIFY( input );
  QVERIFY( chipRow );
  QVERIFY( !chipRow->isVisible() );

  auto mime = std::make_unique<QMimeData>();
  mime->setUrls( QList<QUrl>() << QUrl::fromLocalFile( filePath ) );
  input->insertFromMimeData( mime.get() );
  QApplication::processEvents();

  QVERIFY( chipRow->isVisible() );
  QVERIFY( input->toPlainText().isEmpty() );
  QVERIFY( !dock.findChildren<QWidget *>( u"aiAttachmentChip"_s ).isEmpty() );
  QLabel *stateLabel = dock.findChild<QLabel *>( u"aiAttachmentStateLabel"_s );
  QToolButton *knowledgeButton = dock.findChild<QToolButton *>( u"aiAttachmentKnowledgeButton"_s );
  QVERIFY( stateLabel );
  QCOMPARE( stateLabel->text(), u"needs consent"_s );
  QVERIFY( knowledgeButton );
  QCOMPARE( knowledgeButton->text(), u"Add to Knowledge Base"_s );
}

void TestQgsAiChatDockWidget::dropDoesNotInsertFileUriText()
{
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );

  const QString filePath = tempDir.filePath( u"screenshot.png"_s );
  QImage image( 4, 4, QImage::Format_ARGB32 );
  image.fill( Qt::red );
  QVERIFY( image.save( filePath, "PNG" ) );

  QgsSettings settings;
  settings.setValue( u"strata/visual_context/image_send_consent"_s, true );

  QgsAiModelRouter router;
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  dock.resize( 400, 520 );
  dock.show();
  QApplication::processEvents();

  QgsAiChatPromptEdit *input = dock.findChild<QgsAiChatPromptEdit *>( u"aiPromptInput"_s );
  QVERIFY( input );

  auto mime = std::make_unique<QMimeData>();
  mime->setText( QUrl::fromLocalFile( filePath ).toString() );
  input->insertFromMimeData( mime.get() );
  QApplication::processEvents();

  QVERIFY( !input->toPlainText().contains( u"file://"_s ) );
  QWidget *chipRow = dock.findChild<QWidget *>( u"aiAttachmentChipRow"_s );
  QVERIFY( chipRow );
  QVERIFY( chipRow->isVisible() );
  QVERIFY( !dock.findChildren<QWidget *>( u"aiAttachmentChip"_s ).isEmpty() );

  settings.remove( u"strata/visual_context/image_send_consent"_s );
  settings.remove( u"geoai/visual_context/image_send_consent"_s );
}

void TestQgsAiChatDockWidget::emptyModelMenuOffersCloudSignIn()
{
  const auto guard = isolatePlanModelPickerState();
  const QByteArray savedOAuthToken = qgetenv( "CLAUDE_CODE_OAUTH_TOKEN" );
  qunsetenv( "CLAUDE_CODE_OAUTH_TOKEN" );
  QgsAiSecretStore::removeSecret( u"ai/provider/claude/subscriptionToken"_s );
  const auto restoreEnv = qScopeGuard( [savedOAuthToken]() {
    if ( !savedOAuthToken.isEmpty() )
      qputenv( "CLAUDE_CODE_OAUTH_TOKEN", savedOAuthToken );
  } );

  // The primary setup path is Cloud sign-in, without a terminal.
  QgsAiModelRouter router;
  QTemporaryDir tempDir;
  QVERIFY( tempDir.isValid() );
  QgsAiFileContextProvider contextProvider( tempDir.path() );
  QgsAiReviewPatchEngine reviewEngine;
  QgsAiAgentSessionManager manager( nullptr, &contextProvider, &reviewEngine );
  QgsAiChatDockWidget dock( &manager, &router, &reviewEngine );
  QApplication::processEvents();

  QToolButton *modelPill = dock.findChild<QToolButton *>( u"aiModelPill"_s );
  QVERIFY( modelPill );
  QMenu *menu = modelPill->menu();
  QVERIFY( menu );
  const QStringList menuTexts = modelMenuTexts( menu );
  QVERIFY2( menuTexts.contains( u"No AI providers configured"_s ), qPrintable( menuTexts.join( " | "_L1 ) ) );
  const int connectIndex = menuTexts.indexOf( u"Sign in to Strata Cloud…"_s );
  const int settingsIndex = menuTexts.indexOf( u"Open provider settings…"_s );
  QVERIFY( connectIndex >= 0 );
  QVERIFY( settingsIndex > connectIndex );
  QVERIFY( !menu->findChild<QAction *>( u"aiConnectClaudeCodeAction"_s ) );
}

QGSTEST_MAIN( TestQgsAiChatDockWidget )
#include "testqgsaichatdockwidget.moc"
