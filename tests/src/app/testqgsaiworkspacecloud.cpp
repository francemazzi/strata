/***************************************************************************
  testqgsaiworkspacecloud.cpp
  SPDX-License-Identifier: GPL-2.0-or-later
 ***************************************************************************/
#include "ai/qgsaisettingsdialog.h"
#include "ai/qgsairulesskillscloudclient.h"
#include "ai/qgsaiagentsessionmanager.h"
#include "ai/qgsaifilecontextprovider.h"
#include "ai/qgsaiworkspacetrust.h"
#include "ai/qgsaireviewpatchengine.h"
#include "qgsaisecretstoretestutils.h"
#include "qgsaitestloopbackserver.h"
#include "qgsproject.h"
#include "qgssettings.h"
#include "qgstest.h"
#include <QCheckBox>
#include <QComboBox>
#include <QTableWidget>
#include <QTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>

using namespace Qt::StringLiterals;

class TestQgsAiWorkspaceCloud : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
      QgsApplication::init();
      QgsApplication::initQgis();
    }
    void init()
    {
      installTestSecretBackend();
      QgsSettings().clear();
      QgsProject::instance()->clear();
    }
    void cleanupTestCase() { QgsApplication::exitQgis(); }

    void workspaceOwnsTransfersAndReportsDrafts()
    {
      QTemporaryDir root;
      QgsAiModelRouter router;
      QgsAiFileContextProvider files( root.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      dialog.showSection( u"workspace"_s );
      QVERIFY( dialog.mEffectiveWorkspaceLabel->text().contains( root.path() ) );
      QVERIFY( !dialog.mSyncCloudContextButton->isEnabled() );
      QVERIFY( !dialog.mImportRulesSkillsCloudButton->isEnabled() );
      QVERIFY( dialog.mCloudPrerequisitesLabel->text().contains( u"Sign in"_s ) );
      QCOMPARE( dialog.mSidebarList->currentItem()->data( Qt::UserRole ).toString(), u"workspace"_s );
      QVERIFY( !dialog.mSyncCloudContextButton->isHidden() );
      const QString screenshots = qEnvironmentVariable( "STRATA_UI_SCREENSHOTS" );
      if ( !screenshots.isEmpty() )
      {
        dialog.resize( 1000, 1000 );
        dialog.show();
        QTest::qWait( 100 );
        QVERIFY( dialog.grab().save( screenshots + u"/workspace.png"_s ) );
        const QPalette original = QApplication::palette();
        QPalette dark = original;
        dark.setColor( QPalette::Window, QColor( 30, 30, 32 ) );
        dark.setColor( QPalette::Base, QColor( 40, 40, 43 ) );
        dark.setColor( QPalette::AlternateBase, QColor( 48, 48, 51 ) );
        dark.setColor( QPalette::Button, QColor( 48, 48, 51 ) );
        dark.setColor( QPalette::Text, QColor( 235, 235, 238 ) );
        dark.setColor( QPalette::WindowText, QColor( 235, 235, 238 ) );
        dark.setColor( QPalette::ButtonText, QColor( 235, 235, 238 ) );
        dark.setColor( QPalette::Mid, QColor( 130, 130, 135 ) );
        dark.setColor( QPalette::Midlight, QColor( 65, 65, 69 ) );
        QApplication::setPalette( dark );
        QgsAiSettingsDialog darkDialog( &manager, &router, nullptr );
        darkDialog.showSection( u"workspace"_s );
        darkDialog.resize( 1000, 1000 );
        darkDialog.show();
        QTest::qWait( 100 );
        const bool darkSaved = darkDialog.grab().save( screenshots + u"/workspace-dark.png"_s );
        QApplication::setPalette( original );
        QVERIFY( darkSaved );
      }
      dialog.mWorkspaceRoot->setText( root.path() + u"/other"_s );
      QVERIFY( dialog.cloudSettingsPending() );
      QVERIFY( dialog.mCloudPrerequisitesLabel->text().contains( u"Save"_s ) );
      QVERIFY( dialog.mEffectiveWorkspaceLabel->text().contains( root.path() ) );
      QVERIFY( !dialog.mEffectiveWorkspaceLabel->text().contains( u"/other"_s ) );
      dialog.showSection( u"updates"_s );
      QCOMPARE( dialog.mSidebarList->currentItem()->data( Qt::UserRole ).toString(), u"updates"_s );
    }

    void absentAndProjectWorkspaceAreExplained()
    {
      QgsAiModelRouter router;
      QgsAiFileContextProvider files( QString {} );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      QVERIFY( dialog.mEffectiveWorkspaceLabel->text().contains( u"No active folder"_s ) );
      QVERIFY( !dialog.mSyncRulesSkillsCloudButton->isEnabled() );
      QTemporaryDir project;
      QgsProject::instance()->setFileName( project.filePath( u"synthetic.qgz"_s ) );
      manager.setWorkspaceRoot( project.path() );
      dialog.refreshCloudWorkspaceState();
      QVERIFY( dialog.mEffectiveWorkspaceLabel->text().contains( u"Active project folder"_s ) );
      QVERIFY( dialog.mEffectiveWorkspaceLabel->text().contains( project.path() ) );
      QgsProject::instance()->clear();
    }

    void importConflictKeepsLocalAndRejectsChangedWorkspace_data()
    {
      QTest::addColumn<bool>( "changeWorkspace" );
      QTest::newRow( "keep-local-default" ) << false;
      QTest::newRow( "workspace-changed-in-preview" ) << true;
    }
    void importConflictKeepsLocalAndRejectsChangedWorkspace()
    {
      QFETCH( bool, changeWorkspace );
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      auto delayed = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", "{}" );
      delayed.responseDelayMs = 10000;
      server.responses << delayed;
      QTemporaryDir first, second;
      QgsAiWorkspaceTrust::setState( first.path(), QgsAiWorkspaceTrust::State::Trusted );
      QgsAiWorkspaceTrust::setState( second.path(), QgsAiWorkspaceTrust::State::Trusted );
      QgsAiModelRouter router;
      auto settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
      settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( server.serverPort() );
      router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
      QgsAiFileContextProvider files( first.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      const QString markdown = u"---\nname: Synthetic rule\n---\nKeep this local content.\n"_s;
      QgsAiFileContextProvider originalFiles( first.path() ), otherFiles( second.path() );
      QgsAiRulesSkillsStore store( &originalFiles );
      QVERIFY( store.writeRuleMarkdown( manager.agentBehaviorSettings().rulesPath, u"example"_s, markdown ) );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      QVERIFY( router.setPlanSessionToken( u"synthetic-token"_s ) );
      dialog.refreshCloudWorkspaceState();
      dialog.importRulesSkillsFromCloud();
      auto *client = dialog.findChild<QgsAiRulesSkillsCloudClient *>();
      QVERIFY( client );
      QgsAiRulesSkillsCloudClient::RemoteRule rule;
      rule.slug = u"example"_s;
      rule.name = u"Synthetic rule"_s;
      rule.content = u"Different cloud content."_s;
      client->rulesFetched( { rule } );
      bool sawConflict = false;
      bool keptLocalByDefault = false;
      QTimer::singleShot( 0, &dialog, [&]() {
        auto *preview = qobject_cast<QDialog *>( QApplication::activeModalWidget() );
        if ( !preview )
          return;
        auto *table = preview->findChild<QTableWidget *>();
        if ( table && table->rowCount() == 1 )
        {
          sawConflict = table->item( 0, 2 )->text() == u"Conflict"_s;
          auto *action = qobject_cast<QComboBox *>( table->cellWidget( 0, 3 ) );
          keptLocalByDefault = action && action->currentData().toString() == u"keep"_s;
          if ( changeWorkspace && action )
          {
            action->setCurrentIndex( action->findData( u"replace"_s ) );
            manager.setWorkspaceRoot( second.path() );
          }
        }
        preview->accept();
      } );
      client->skillsFetched( {} ); // Opens the real modal import preview.
      QVERIFY( sawConflict );
      QVERIFY( keptLocalByDefault );
      const auto rules = store.listRules( manager.agentBehaviorSettings().rulesPath );
      QCOMPARE( rules.size(), 1 );
      QCOMPARE( store.readRuleMarkdown( rules.first() ), markdown );
      QVERIFY( QgsAiRulesSkillsStore( &otherFiles ).listRules( manager.agentBehaviorSettings().rulesPath ).isEmpty() );
      QVERIFY( dialog.mCloudTransfer.isNull() );
      if ( !changeWorkspace )
        QVERIFY( dialog.mRulesSkillsCloudStatusLabel->text().contains( u"Imported 0"_s ) );
    }

    void pendingConfigurationDisablesTransfers_data()
    {
      QTest::addColumn<QString>( "field" );
      for ( const char *field : { "folder", "endpoint", "consent", "trust", "indexing" } )
        QTest::newRow( field ) << QString::fromLatin1( field );
    }
    void pendingConfigurationDisablesTransfers()
    {
      QFETCH( QString, field );
      QTemporaryDir root;
      QgsSettings().setValue( u"strata/index/cloud_context_opt_in"_s, true );
      QgsAiWorkspaceTrust::setState( root.path(), QgsAiWorkspaceTrust::State::Trusted );
      QgsAiModelRouter router;
      QgsAiFileContextProvider files( root.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      QVERIFY( router.setPlanSessionToken( u"synthetic-token"_s ) );
      dialog.refreshCloudWorkspaceState();
      QgsAiCloudIndexClient::ContextItem item;
      item.sourceType = u"rule"_s;
      item.path = u"rules/test.md"_s;
      item.text = u"Synthetic instructions"_s;
      dialog.mCloudPreviewItems = { item };
      dialog.mPreviewCloudScope = dialog.cloudScopeKey();
      dialog.refreshCloudWorkspaceState();
      QVERIFY( dialog.mSyncCloudContextButton->isEnabled() );
      QVERIFY( dialog.mSyncRulesSkillsCloudButton->isEnabled() );
      if ( field == "folder"_L1 )
        dialog.mWorkspaceRoot->setText( root.path() + u"/pending"_s );
      if ( field == "endpoint"_L1 )
        dialog.findChild<QLineEdit *>( u"aiPlanEndpointLineEdit"_s )->setText( u"http://127.0.0.1:9/ai/messages"_s );
      if ( field == "consent"_L1 )
        dialog.mCloudContextOptIn->setChecked( false );
      if ( field == "trust"_L1 )
        dialog.mTrustWorkspace->setChecked( false );
      if ( field == "indexing"_L1 )
        dialog.mAutomaticIndexing->toggle();
      dialog.refreshCloudWorkspaceState();
      QVERIFY( dialog.cloudSettingsPending() );
      QVERIFY( !dialog.mSyncCloudContextButton->isEnabled() );
      QVERIFY( !dialog.mSyncRulesSkillsCloudButton->isEnabled() );
      QVERIFY( !dialog.mImportRulesSkillsCloudButton->isEnabled() );
      QVERIFY( dialog.mCloudPrerequisitesLabel->text().contains( u"Save"_s ) );
    }

    void previewDoesNotOverwriteTransferOutcome()
    {
      QTemporaryDir root;
      QDir( root.path() ).mkpath( u".strata/rules"_s );
      QFile file( root.filePath( u".strata/rules/example.md"_s ) );
      QVERIFY( file.open( QIODevice::WriteOnly ) );
      file.write( "Use clear synthetic examples." );
      file.close();
      QgsAiModelRouter router;
      QgsAiFileContextProvider files( root.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      auto behavior = manager.agentBehaviorSettings();
      behavior.rulesPath = u".strata/rules"_s;
      behavior.loadWorkspaceRules = true;
      manager.setAgentBehaviorSettings( behavior );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      dialog.mCloudTransferStatusLabel->setText( u"Queued, not indexed"_s );
      dialog.refreshCloudIndexStatusLabel();
      QTRY_VERIFY( !dialog.mCloudPreviewRunning );
      QCOMPARE( dialog.mCloudPreviewItems.size(), 1 );
      QCOMPARE( dialog.mCloudTransferStatusLabel->text(), u"Queued, not indexed"_s );
      QCOMPARE( dialog.mCloudPreviewItems.first().sourceType, u"rule"_s );
      QVERIFY( !dialog.mSyncCloudContextButton->isEnabled() ); // Preview never grants consent or signs in.
    }

    void accountAndWorkspaceChangesInvalidateTransfers()
    {
      QTemporaryDir first, second;
      QgsAiModelRouter router;
      QgsAiFileContextProvider files( first.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      auto *operation = new QObject( &dialog );
      dialog.beginCloudTransfer( operation );
      QVERIFY( dialog.cloudTransferCurrent( operation ) );
      manager.setWorkspaceRoot( second.path() );
      QVERIFY( !dialog.cloudTransferCurrent( operation ) );
      QVERIFY( dialog.mCloudTransfer.isNull() );
      QVERIFY( dialog.mCloudTransferStatusLabel->text().contains( u"Context changed"_s ) );
      QVERIFY( dialog.mCloudPreviewItems.isEmpty() );
      router.setPlanSessionToken( u"synthetic-account-one"_s );
      dialog.refreshCloudWorkspaceState();
      operation = new QObject( &dialog );
      dialog.beginCloudTransfer( operation );
      router.setPlanSessionToken( u"synthetic-account-two"_s );
      QVERIFY( !dialog.cloudTransferCurrent( operation ) );
      QVERIFY( dialog.mCloudTransfer.isNull() );
    }

    void stalePreviewCannotReplaceNewWorkspace()
    {
      QTemporaryDir first, second;
      QgsAiModelRouter router;
      QgsAiFileContextProvider files( first.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      dialog.refreshCloudIndexStatusLabel();
      manager.setWorkspaceRoot( second.path() );
      dialog.refreshCloudWorkspaceState();
      QTest::qWait( 100 );
      QVERIFY( dialog.mPreviewCloudScope.isEmpty() );
      QVERIFY( dialog.mCloudPreviewItems.isEmpty() );
      QVERIFY( dialog.mEffectiveWorkspaceLabel->text().contains( second.path() ) );
    }

    void contextTransferReportsQueueAndPartialFailure_data()
    {
      QTest::addColumn<QString>( "outcome" );
      QTest::newRow( "queued" ) << u"queued"_s;
      QTest::newRow( "error" ) << u"error"_s;
      QTest::newRow( "partial" ) << u"partial"_s;
    }
    void contextTransferReportsQueueAndPartialFailure()
    {
      QFETCH( QString, outcome );
      QgsSettings().setValue( u"strata/index/cloud_context_opt_in"_s, true );
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", "{\"id\":\"synthetic-workspace\"}" );
      if ( outcome != "error"_L1 )
        server.responses << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", "{\"upserted\":1,\"queued\":1}" );
      if ( outcome != "queued"_L1 )
        server.responses << QgsAiTestLoopbackServer::jsonResponse( 400, "Bad Request", "{\"message\":\"Synthetic failure\"}" );
      QTemporaryDir root;
      QgsAiModelRouter router;
      auto settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
      settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( server.serverPort() );
      router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
      QgsAiFileContextProvider files( root.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      QVERIFY( router.setPlanSessionToken( u"synthetic-cloud-token"_s ) );
      dialog.refreshCloudWorkspaceState();
      const int count = outcome == "partial"_L1 ? 129 : 1;
      for ( int i = 0; i < count; ++i )
      {
        QgsAiCloudIndexClient::ContextItem item;
        item.sourceType = u"rule"_s;
        item.path = u"rules/example-%1.md"_s.arg( i );
        item.text = u"Synthetic instructions %1"_s.arg( i );
        dialog.mCloudPreviewItems << item;
      }
      dialog.mPreviewCloudScope = dialog.cloudScopeKey();
      dialog.syncCloudContext();
      QTRY_VERIFY( dialog.mCloudTransfer.isNull() );
      QCOMPARE( server.requestCount, outcome == "partial"_L1 ? 3 : 2 );
      const QString result = dialog.mCloudTransferStatusLabel->text();
      if ( outcome == "queued"_L1 )
        QVERIFY( result.contains( u"not yet confirmed indexed"_s ) );
      else
      {
        QVERIFY( result.contains( u"Transfer failed"_s ) );
        QVERIFY( result.contains( u"Earlier batches may already have been sent"_s ) );
      }
      dialog.refreshCloudIndexStatusLabel();
      QTRY_VERIFY( !dialog.mCloudPreviewRunning );
      QCOMPARE( dialog.mCloudTransferStatusLabel->text(), result );
    }

    void lateCloudReplyCannotReportSuccessForAnotherWorkspace()
    {
      QgsSettings().setValue( u"strata/index/cloud_context_opt_in"_s, true );
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      auto delayed = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", "{\"id\":\"old-workspace\"}" );
      delayed.responseDelayMs = 200;
      server.responses << delayed;
      QTemporaryDir first, second;
      QgsAiModelRouter router;
      auto settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
      settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( server.serverPort() );
      router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
      QgsAiFileContextProvider files( first.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      QVERIFY( router.setPlanSessionToken( u"synthetic-cloud-token"_s ) );
      dialog.refreshCloudWorkspaceState();
      QgsAiCloudIndexClient::ContextItem item;
      item.sourceType = u"rule"_s;
      item.path = u"rules/example.md"_s;
      item.text = u"Use synthetic examples."_s;
      dialog.mCloudPreviewItems = { item };
      dialog.mPreviewCloudScope = dialog.cloudScopeKey();
      dialog.syncCloudContext();
      auto *client = dialog.findChild<QgsAiCloudIndexClient *>();
      QVERIFY( client );
      QTRY_COMPARE( server.requestCount, 1 );
      manager.setWorkspaceRoot( second.path() );
      dialog.refreshCloudWorkspaceState();
      client->contextSynced( { u"old-workspace"_s, 1, 1 } );
      QVERIFY( !dialog.mCloudTransferStatusLabel->text().contains( u"old-workspace"_s ) );
      QVERIFY( dialog.mCloudTransferStatusLabel->text().contains( u"Context changed"_s ) );
      QTest::qWait( 300 );
      QCOMPARE( server.requestCount, 1 ); // No context POST after the delayed workspace response.
    }

    void openingAndPreviewingNeverWritesCloud()
    {
      QgsAiTestLoopbackServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      server.responses << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", "{}" );
      QTemporaryDir root;
      QgsAiModelRouter router;
      auto settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
      settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( server.serverPort() );
      router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
      QgsAiFileContextProvider files( root.path() );
      QgsAiReviewPatchEngine review;
      QgsAiAgentSessionManager manager( nullptr, &files, &review );
      QgsAiSettingsDialog dialog( &manager, &router, nullptr );
      dialog.showSection( u"workspace"_s );
      QTRY_VERIFY( !dialog.mCloudPreviewRunning );
      QTest::qWait( 100 );
      for ( const auto &request : server.rawRequests )
        QVERIFY2( request.startsWith( "GET " ), request.left( request.indexOf( '\n' ) ).constData() );
      QVERIFY( dialog.findChildren<QgsAiCloudIndexClient *>().isEmpty() );
      QVERIFY( dialog.findChildren<QgsAiRulesSkillsCloudClient *>().isEmpty() );
    }
};
QGSTEST_MAIN( TestQgsAiWorkspaceCloud )
#include "testqgsaiworkspacecloud.moc"
