/***************************************************************************
  qgsaiworkspacecloud.cpp — scoped, non-blocking workspace transfer presentation
  SPDX-License-Identifier: GPL-2.0-or-later
 ***************************************************************************/
#include "qgsaisettingsdialog.h"
#include "qgsaiaccountwidget.h"
#include "qgsaiagentsessionmanager.h"
#include "qgsaifilecontextprovider.h"
#include "qgsaiworkspacetrust.h"
#include "qgsproject.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

QString QgsAiSettingsDialog::cloudConfiguration() const
{
  const QJsonArray values {
    mWorkspaceRoot->text().trimmed(),
    mAccountWidget->planEndpoint(),
    mAccountWidget->planAuthConfigId(),
    mAccountWidget->manualSessionToken(),
    mEmbeddingProvider->currentData().toString(),
    mRemoteEmbeddingModel->text(),
    mAutomaticIndexing->isChecked(),
    mEnableLayerIndexing->isChecked(),
    mIndexRemoteLayers->isChecked(),
    mExcludedIndexFolders->text(),
    mMaxIndexedFiles->value(),
    mCloudContextOptIn->isChecked()
  };
  return QString::fromLatin1( QCryptographicHash::hash( QJsonDocument( values ).toJson( QJsonDocument::Compact ), QCryptographicHash::Sha256 ).toHex() );
}

bool QgsAiSettingsDialog::cloudSettingsPending() const
{
  return cloudConfiguration() != mInitialCloudConfiguration || ( !mTrustRootForCheckbox.isEmpty() && mTrustWorkspace->isChecked() != QgsAiWorkspaceTrust::isTrusted( mTrustRootForCheckbox ) );
}

QString QgsAiSettingsDialog::cloudScopeKey() const
{
  if ( !mSessionManager || !mModelRouter )
    return QString();
  const auto behavior = mSessionManager->agentBehaviorSettings();
  const QJsonArray values {
    mSessionManager->workspaceRoot(),
    QgsProject::instance()->fileName(),
    mModelRouter->planSessionToken(),
    mModelRouter->planCredentialScope(),
    mModelRouter->cachedPlanAccountId(),
    cloudConfiguration(),
    mTrustWorkspace->isChecked(),
    QgsAiWorkspaceTrust::isTrusted( mSessionManager->workspaceRoot() ),
    behavior.rulesPath,
    behavior.skillsPath,
    behavior.loadWorkspaceRules,
    behavior.loadWorkspaceSkills
  };
  return QString::fromLatin1( QCryptographicHash::hash( QJsonDocument( values ).toJson( QJsonDocument::Compact ), QCryptographicHash::Sha256 ).toHex() );
}

void QgsAiSettingsDialog::initializeCloudWorkspace()
{
  mInitialCloudConfiguration = cloudConfiguration();
  const auto refresh = [this]() { refreshCloudWorkspaceState(); };
  connect( mWorkspaceRoot, &QLineEdit::textChanged, this, refresh );
  connect( mTrustWorkspace, &QCheckBox::toggled, this, refresh );
  connect( mCloudContextOptIn, &QCheckBox::toggled, this, refresh );
  connect( mAccountWidget, &QgsAiAccountWidget::accountInfoChanged, this, refresh );
  // Advanced account and indexing edits must disable transfers before the next click.
  for ( QLineEdit *edit : findChildren<QLineEdit *>() )
    connect( edit, &QLineEdit::textChanged, this, refresh );
  if ( mSessionManager && mSessionManager->fileContextProvider() )
    connect( mSessionManager->fileContextProvider(), &QgsAiFileContextProvider::workspaceRootChanged, this, [this]() {
      refreshTrustWorkspace();
      refreshCloudWorkspaceState();
    } );
  connect( mSidebarList, &QListWidget::currentRowChanged, this, [this]() {
    if ( mSidebarList->currentItem() && mSidebarList->currentItem()->data( Qt::UserRole ).toString() == "workspace"_L1 )
      refreshCloudIndexStatusLabel();
  } );
  if ( mSessionManager && mSessionManager->workspaceIndex() )
    connect( mSessionManager->workspaceIndex(), &QgsAiWorkspaceIndex::loaded, this, &QgsAiSettingsDialog::refreshCloudIndexStatusLabel );
  QTimer *stateTimer = new QTimer( this );
  stateTimer->setInterval( 500 );
  connect( stateTimer, &QTimer::timeout, this, refresh );
  connect( this, &QDialog::finished, this, [this, stateTimer]() {
    stateTimer->stop();
    ++mCloudPreviewGeneration;
    if ( mCloudTransfer )
    {
      mCloudTransfer->setProperty( "cloudTransferInvalidated", true );
      disconnect( mCloudTransfer, nullptr, this, nullptr );
      mCloudTransfer->deleteLater();
      mCloudTransfer = nullptr;
    }
  } );
  stateTimer->start();
  refreshCloudWorkspaceState();
}

void QgsAiSettingsDialog::refreshCloudWorkspaceState()
{
  if ( !mCloudTransferStatusLabel )
    return;
  if ( QWidget *row = findChild<QWidget *>( u"aiRulesCloudRow"_s ) )
    for ( QLabel *label : row->findChildren<QLabel *>() )
      if ( label->property( "aiRole" ).toString() == "rowDescription"_L1 )
        label->setText( tr( "%1 rules · %2 skills on this computer" ).arg( mRulesListWidget->count() ).arg( mSkillsListWidget->count() ) );
  const QString scope = cloudScopeKey();
  if ( scope != mObservedCloudScope )
  {
    const bool interrupted = !mCloudTransfer.isNull();
    mObservedCloudScope = scope;
    ++mCloudPreviewGeneration;
    mCloudPreviewRunning = false;
    mPreviewCloudScope.clear();
    mCloudPreviewItems.clear();
    if ( mCloudTransfer )
    {
      mCloudTransfer->setProperty( "cloudTransferInvalidated", true );
      disconnect( mCloudTransfer, nullptr, this, nullptr );
      mCloudTransfer->deleteLater();
      mCloudTransfer = nullptr;
    }
    mRulesSkillsCloudStatusLabel->clear();
    mCloudTransferStatusLabel->setText( interrupted ? tr( "Context changed. Further transfer steps stopped; items already sent may remain in the previous Cloud workspace." ) : QString() );
    mCloudIndexStatusLabel->setText( tr( "Refresh the preview to see the current workspace content." ) );
  }
  const QString root = mSessionManager ? mSessionManager->workspaceRoot() : QString();
  const bool projectFolder = !QgsProject::instance()->homePath().isEmpty();
  mEffectiveWorkspaceLabel->setText(
    root.isEmpty() ? tr( "No active folder. Choose an alternative folder below." )
                   : ( projectFolder ? tr( "Active project folder: %1" ) : tr( "Active alternative folder: %1" ) ).arg( QDir::toNativeSeparators( root ) )
  );
  const bool signedIn = mModelRouter && !mModelRouter->planSessionToken().trimmed().isEmpty();
  mWorkspaceAccountLabel->setText(
    signedIn ? tr( "Signed in: %1" ).arg( mAccountWidget->accountEmail().isEmpty() ? tr( "Strata Cloud account" ) : mAccountWidget->accountEmail() ) : tr( "Not signed in" )
  );
  QStringList prerequisites;
  if ( root.isEmpty() )
    prerequisites << tr( "Choose a local folder." );
  if ( !signedIn )
    prerequisites << tr( "Sign in to Strata Cloud." );
  if ( cloudSettingsPending() )
    prerequisites.prepend( tr( "Save the changed settings with OK, then reopen Workspace and Cloud before transferring." ) );
  if ( !root.isEmpty() && !QgsAiWorkspaceTrust::isTrusted( root ) )
    prerequisites << tr( "Trust this workspace to send or import rules and skills." );
  if ( !mCloudContextOptIn->isChecked() )
    prerequisites << tr( "Allow sending AI context to enable its transfer." );
  mCloudPrerequisitesLabel->setText( prerequisites.isEmpty() ? tr( "Ready. Choose the content to transfer below." ) : tr( "Next: %1" ).arg( prerequisites.first() ) );
  const bool ready = !root.isEmpty() && signedIn && !cloudSettingsPending() && !mCloudTransfer && !mAccountWidget->isBusy();
  mSyncRulesSkillsCloudButton->setEnabled( ready && QgsAiWorkspaceTrust::isTrusted( root ) );
  mImportRulesSkillsCloudButton->setEnabled( ready && QgsAiWorkspaceTrust::isTrusted( root ) );
  mSyncCloudContextButton->setEnabled( ready && mCloudContextOptIn->isChecked() && !mCloudPreviewRunning && mPreviewCloudScope == scope && !mCloudPreviewItems.isEmpty() );
}

void QgsAiSettingsDialog::beginCloudTransfer( QObject *client )
{
  mCloudTransfer = client;
  client->setProperty( "cloudScope", cloudScopeKey() );
  refreshCloudWorkspaceState();
}

bool QgsAiSettingsDialog::cloudTransferCurrent( QObject *client )
{
  refreshCloudWorkspaceState();
  return mCloudTransfer == client && !cloudSettingsPending() && client->property( "cloudScope" ).toString() == cloudScopeKey();
}

void QgsAiSettingsDialog::finishCloudTransfer( QObject *client )
{
  if ( mCloudTransfer == client )
    mCloudTransfer = nullptr;
  refreshCloudWorkspaceState();
}

void QgsAiSettingsDialog::refreshCloudIndexStatusLabel()
{
  refreshCloudWorkspaceState();
  if ( mCloudPreviewRunning || !mSessionManager || cloudSettingsPending() )
    return;
  const QString root = mSessionManager->workspaceRoot();
  if ( root.isEmpty() )
    return;
  QList<QgsAiWorkspaceIndex::Chunk> chunks;
  if ( auto *index = mSessionManager->workspaceIndex() )
  {
    index->requestLoad();
    if ( !index->tryCloudSnapshot( root, chunks ) )
    {
      mCloudIndexStatusLabel->setText( tr( "Waiting for the local index. Refresh the preview when indexing is ready." ) );
      mPreviewCloudScope.clear();
      refreshCloudWorkspaceState();
      return;
    }
  }
  const auto behavior = mSessionManager->agentBehaviorSettings();
  const QString scope = cloudScopeKey();
  const quint64 generation = ++mCloudPreviewGeneration;
  mCloudPreviewRunning = true;
  mPreviewCloudScope.clear();
  mCloudIndexStatusLabel->setText( tr( "Preparing the content preview…" ) );
  refreshCloudWorkspaceState();
  using Items = QList<QgsAiCloudIndexClient::ContextItem>;
  auto *watcher = new QFutureWatcher<Items>( this );
  connect( watcher, &QFutureWatcher<Items>::finished, this, [this, watcher, scope, generation]() {
    const Items items = watcher->result();
    watcher->deleteLater();
    if ( generation != mCloudPreviewGeneration || scope != cloudScopeKey() )
      return;
    mCloudPreviewRunning = false;
    QString error;
    if ( !items.isEmpty() && !QgsAiCloudIndexClient::validateContextItems( items, &error ) )
      mCloudIndexStatusLabel->setText( tr( "Transfer blocked: %1" ).arg( error ) );
    else
    {
      mCloudPreviewItems = items;
      mPreviewCloudScope = scope;
      int layers = 0, rules = 0, skills = 0, documents = 0;
      for ( const auto &item : items )
      {
        if ( item.sourceType == "layer"_L1 )
          ++layers;
        else if ( item.sourceType == "rule"_L1 )
          ++rules;
        else if ( item.sourceType == "skill"_L1 )
          ++skills;
        else
          ++documents;
      }
      mCloudIndexStatusLabel->setText(
        items.isEmpty() ? tr( "No shareable AI context yet. Configure local indexing or add workspace instructions." )
                        : tr( "Preview: %1 items — %2 layer summaries, %3 rules, %4 skills, %5 document excerpts." ).arg( items.size() ).arg( layers ).arg( rules ).arg( skills ).arg( documents )
      );
    }
    refreshCloudWorkspaceState();
  } );
  watcher->setFuture( QtConcurrent::run( [root, behavior, chunks]() {
    Items items = QgsAiCloudIndexClient::contextItemsFromChunks( chunks );
    items += QgsAiCloudIndexClient::contextItemsFromWorkspaceFolders( root, behavior.loadWorkspaceRules ? behavior.rulesPath : QString(), behavior.loadWorkspaceSkills ? behavior.skillsPath : QString() );
    return QgsAiCloudIndexClient::deduplicateContextItems( items );
  } ) );
}

void QgsAiSettingsDialog::syncCloudContext()
{
  refreshCloudWorkspaceState();
  if ( !mSyncCloudContextButton->isEnabled() )
    return;
  auto *client = new QgsAiCloudIndexClient( this );
  beginCloudTransfer( client );
  mCloudTransferStatusLabel->setText( tr( "Sending AI context to Strata Cloud…" ) );
  connect( client, &QgsAiCloudIndexClient::contextSynced, this, [this, client]( const QgsAiCloudIndexClient::SyncResult &result ) {
    if ( !cloudTransferCurrent( client ) )
      return;
    mCloudTransferStatusLabel->setText(
      tr( "Sent %1 items; %2 queued for Cloud indexing. Workspace: %3. Queued items are not yet confirmed indexed." ).arg( result.upserted ).arg( result.queued ).arg( result.workspaceId )
    );
    finishCloudTransfer( client );
    client->deleteLater();
  } );
  connect( client, &QgsAiCloudIndexClient::requestFailed, this, [this, client]( const QString &message ) {
    if ( !cloudTransferCurrent( client ) )
      return;
    mCloudTransferStatusLabel->setText( tr( "Transfer failed: %1. Earlier batches may already have been sent. You can retry this transfer." ).arg( message ) );
    finishCloudTransfer( client );
    client->deleteLater();
  } );
  const QString root = mSessionManager->workspaceRoot();
  client->syncWorkspaceContext( mAccountWidget->planEndpoint(), mModelRouter->planSessionToken(), root, QFileInfo( root ).fileName(), mCloudPreviewItems, true );
}
