/***************************************************************************
    qgsaisettingsdialog.cpp
    ---------------------
    begin                : July 2026
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

#include "qgsaisettingsdialog.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include "ai/index/qgsaicloudindexclient.h"
#include "ai/index/qgsaiembeddingprovider.h"
#include "ai/index/qgsaiindexingscheduler.h"
#include "ai/index/qgsaiindexingthrottle.h"
#include "ai/index/qgsailayerindexcoordinator.h"
#include "ai/index/qgsaiworkspaceindex.h"
#include "ai/tools/qgsaitaskrunner.h"
#include "qgsaiaccountwidget.h"
#include "qgsaiagentsessionmanager.h"
#include "qgsaichatdockwidget.h"
#include "qgsaiclaudeconnectwidget.h"
#include "qgsaicredentialdialog.h"
#include "qgsaigallerycloudclient.h"
#include "qgsaigissuggestionengine.h"
#include "qgsaimessagelogbuffer.h"
#include "qgsaimodelrouter.h"
#include "qgsaiopenroutermodelcatalog.h"
#include "qgsaiplanclient.h"
#include "qgsairulesskillscloudclient.h"
#include "qgsairulesskillsstore.h"
#include "qgsaisecretstore.h"
#include "qgsaisettingsutils.h"
#include "qgsaiworkspacetrust.h"
#include "qgsapplication.h"
#include "qgscollapsiblegroupbox.h"
#include "qgsfeature.h"
#include "qgsfeedback.h"
#include "qgsgeometry.h"
#include "qgsnetworkaccessmanager.h"
#include "qgsproject.h"
#include "qgsscrollarea.h"
#include "qgssettings.h"
#include "qgstaskmanager.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QString>
#include <QTableWidget>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

#include "moc_qgsaisettingsdialog.cpp"

using namespace Qt::StringLiterals;

using QgsAiSettingsUtils::humanBytes;
using QgsAiSettingsUtils::sectionHeader;
using QgsAiSettingsUtils::settingRow;
using QgsAiSettingsUtils::settingRowFullWidth;
using QgsAiSettingsUtils::settingValueWithLegacy;

namespace
{
  class ManualWorkspaceIndexTask final : public QgsTask
  {
    public:
      ManualWorkspaceIndexTask( QgsAiWorkspaceIndex *index, const QString &workspaceRoot )
        : QgsTask( QObject::tr( "Rebuild AI workspace index" ), QgsTask::CanCancel | QgsTask::CancelWithoutPrompt )
        , mIndex( index )
        , mWorkspaceRoot( workspaceRoot )
      {}

      QString errorMessage() const { return mErrorMessage; }

      void cancel() override
      {
        mFeedback.cancel();
        QgsTask::cancel();
      }

    protected:
      bool run() override
      {
        if ( !mIndex )
        {
          mErrorMessage = QObject::tr( "Workspace index is unavailable." );
          return false;
        }

        const QMetaObject::Connection conn = connect(
          mIndex.data(),
          &QgsAiWorkspaceIndex::progress,
          this,
          [this]( int current, int total, const QString & ) {
            if ( total > 0 )
              setProgress( 100.0 * static_cast<double>( current ) / static_cast<double>( total ) );
          },
          Qt::DirectConnection
        );

        // The folder walk runs here too: a large or network workspace never holds the dialog.
        QString error;
        QList<QgsAiWorkspaceIndex::WorkspaceFileSnapshot> snapshot;
        bool ok = QgsAiWorkspaceIndex::scanWorkspaceFileSnapshot( mWorkspaceRoot, QgsAiIndexingScheduler::maxFiles(), snapshot, &error, &mFeedback );
        if ( ok )
          ok = mIndex->reindex( snapshot, mWorkspaceRoot, &error, &mFeedback );
        disconnect( conn );
        mIndex->closeDatabaseConnectionForCurrentThread();
        if ( !ok )
          mErrorMessage = error;
        return ok && !isCanceled();
      }

    private:
      QPointer<QgsAiWorkspaceIndex> mIndex;
      QString mWorkspaceRoot;
      QgsFeedback mFeedback;
      QString mErrorMessage;
  };

  class ManualLayerIndexTask final : public QgsTask
  {
    public:
      ManualLayerIndexTask( QgsAiWorkspaceIndex *index, const QgsAiWorkspaceIndex::WorkspaceLayerSnapshot &snapshot )
        : QgsTask( QObject::tr( "Rebuild AI layer index" ), QgsTask::CanCancel | QgsTask::CancelWithoutPrompt )
        , mIndex( index )
        , mSnapshot( snapshot )
      {}

      QString errorMessage() const { return mErrorMessage; }

    protected:
      bool run() override
      {
        if ( !mIndex )
        {
          mErrorMessage = QObject::tr( "Workspace index is unavailable." );
          return false;
        }

        QString error;
        const bool ok = mIndex->reindexLayerSnapshot( mSnapshot, &error );
        mIndex->closeDatabaseConnectionForCurrentThread();
        if ( !ok )
          mErrorMessage = error;
        return ok && !isCanceled();
      }

    private:
      QPointer<QgsAiWorkspaceIndex> mIndex;
      QgsAiWorkspaceIndex::WorkspaceLayerSnapshot mSnapshot;
      QString mErrorMessage;
  };

  bool downloadEmbeddingModelFile( const QgsAiEmbeddingModelDownloadFile &file, const QString &baseDir, QProgressDialog &progress, qint64 &completedBytes, QString *errorMessage )
  {
    const QString destPath = QDir( baseDir ).filePath( file.relativePath );
    if ( QFileInfo::exists( destPath ) )
    {
      QString hashError;
      if ( QgsAiE5EmbeddingProvider::fileMatchesSha256( destPath, file.sha256, &hashError ) )
      {
        completedBytes += file.size;
        progress.setValue( static_cast<int>( std::min<qint64>( completedBytes, progress.maximum() ) ) );
        return true;
      }
    }

    QFileInfo destInfo( destPath );
    if ( !destInfo.dir().exists() && !destInfo.dir().mkpath( u"."_s ) )
    {
      if ( errorMessage )
        *errorMessage = QObject::tr( "Cannot create model directory: %1" ).arg( destInfo.dir().absolutePath() );
      return false;
    }

    QgsNetworkAccessManager *nam = QgsNetworkAccessManager::instance();
    if ( !nam )
    {
      if ( errorMessage )
        *errorMessage = QObject::tr( "Network manager is not available." );
      return false;
    }

    const QString partPath = destPath + u".part"_s;
    QFile::remove( partPath );
    QFile outFile( partPath );
    if ( !outFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
      if ( errorMessage )
        *errorMessage = QObject::tr( "Cannot write model file: %1" ).arg( partPath );
      return false;
    }

    QNetworkRequest request( QUrl( file.url ) );
    request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy );
    request.setTransferTimeout( 30000 );

    QNetworkReply *reply = nam->get( request );
    if ( !reply )
    {
      outFile.close();
      QFile::remove( partPath );
      if ( errorMessage )
        *errorMessage = QObject::tr( "Unable to start model download." );
      return false;
    }

    QCryptographicHash hash( QCryptographicHash::Sha256 );
    qint64 fileBytes = 0;
    auto drainReply = [&]() {
      const QByteArray chunk = reply->readAll();
      if ( chunk.isEmpty() )
        return;
      outFile.write( chunk );
      hash.addData( chunk );
      fileBytes += chunk.size();
      progress.setValue( static_cast<int>( std::min<qint64>( completedBytes + fileBytes, progress.maximum() ) ) );
      QApplication::processEvents();
    };

    QObject::connect( reply, &QNetworkReply::readyRead, reply, drainReply );
    QObject::connect( &progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort );

    QEventLoop loop;
    QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
    loop.exec();
    drainReply();

    const bool wasCanceled = progress.wasCanceled();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorString = reply->errorString();
    reply->deleteLater();

    outFile.close();
    if ( wasCanceled )
    {
      QFile::remove( partPath );
      if ( errorMessage )
        *errorMessage = QObject::tr( "Model download canceled." );
      return false;
    }

    if ( networkError != QNetworkReply::NoError )
    {
      QFile::remove( partPath );
      if ( errorMessage )
        *errorMessage = QObject::tr( "Model download failed: %1" ).arg( networkErrorString );
      return false;
    }

    if ( fileBytes != file.size )
    {
      QFile::remove( partPath );
      if ( errorMessage )
        *errorMessage = QObject::tr( "Model download size mismatch for %1: expected %2, got %3." ).arg( file.relativePath ).arg( file.size ).arg( fileBytes );
      return false;
    }

    const QString actualSha = QString::fromLatin1( hash.result().toHex() );
    if ( actualSha.compare( file.sha256, Qt::CaseInsensitive ) != 0 )
    {
      QFile::remove( partPath );
      if ( errorMessage )
        *errorMessage = QObject::tr( "Model download hash mismatch for %1." ).arg( file.relativePath );
      return false;
    }

    QFile::remove( destPath );
    if ( !QFile::rename( partPath, destPath ) )
    {
      QFile::remove( partPath );
      if ( errorMessage )
        *errorMessage = QObject::tr( "Cannot move verified model file into place: %1" ).arg( destPath );
      return false;
    }

    completedBytes += file.size;
    progress.setValue( static_cast<int>( std::min<qint64>( completedBytes, progress.maximum() ) ) );
    return true;
  }

  bool downloadEmbeddingModelWithConsent( QWidget *parent, QString *errorMessage )
  {
    const QString destination = QgsAiE5EmbeddingProvider::userModelDirectory();
    const QString question = QObject::tr(
                               "Strata will download the local multilingual E5 embedding model from Hugging Face.\n\n"
                               "Source: https://huggingface.co/intfloat/multilingual-e5-small\n"
                               "License: MIT\n"
                               "Revision: 614241f622f53c4eeff9890bdc4f31cfecc418b3\n"
                               "Size: %1\n"
                               "Destination: %2\n\n"
                               "The model is used locally for workspace indexing and is not bundled with this release.\n\n"
                               "Download now?"
    )
                               .arg( humanBytes( QgsAiE5EmbeddingProvider::downloadSize() ), destination );

    if ( QMessageBox::question( parent, QObject::tr( "Download local embedding model" ), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No ) != QMessageBox::Yes )
    {
      if ( errorMessage )
        *errorMessage = QObject::tr( "Model download was not approved." );
      return false;
    }

    QProgressDialog progress( QObject::tr( "Downloading local embedding model..." ), QObject::tr( "Cancel" ), 0, static_cast<int>( QgsAiE5EmbeddingProvider::downloadSize() ), parent );
    progress.setWindowModality( Qt::WindowModal );
    progress.setMinimumDuration( 0 );
    progress.setValue( 0 );

    qint64 completedBytes = 0;
    for ( const QgsAiEmbeddingModelDownloadFile &file : QgsAiE5EmbeddingProvider::downloadFiles() )
    {
      progress.setLabelText( QObject::tr( "Downloading %1" ).arg( file.relativePath ) );
      QString fileError;
      if ( !downloadEmbeddingModelFile( file, destination, progress, completedBytes, &fileError ) )
      {
        if ( errorMessage )
          *errorMessage = fileError;
        return false;
      }
    }

    QJsonArray files;
    for ( const QgsAiEmbeddingModelDownloadFile &file : QgsAiE5EmbeddingProvider::downloadFiles() )
    {
      QJsonObject item;
      item.insert( u"relative_path"_s, file.relativePath );
      item.insert( u"sha256"_s, file.sha256 );
      item.insert( u"size"_s, static_cast<double>( file.size ) );
      files.append( item );
    }

    QJsonObject manifest;
    manifest.insert( u"model_id"_s, QgsAiE5EmbeddingProvider::modelName() );
    manifest.insert( u"model_revision"_s, QgsAiE5EmbeddingProvider::pinnedModelRevision() );
    manifest.insert( u"license"_s, u"MIT"_s );
    manifest.insert( u"source"_s, u"https://huggingface.co/intfloat/multilingual-e5-small"_s );
    manifest.insert( u"source_revision"_s, u"614241f622f53c4eeff9890bdc4f31cfecc418b3"_s );
    manifest.insert( u"files"_s, files );

    const QString manifestPath = QDir( destination ).filePath( u"manifest.json"_s );
    const QString manifestPartPath = manifestPath + u".part"_s;
    QFile::remove( manifestPartPath );
    QFile manifestFile( manifestPartPath );
    if ( !manifestFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    {
      if ( errorMessage )
        *errorMessage = QObject::tr( "Cannot write model manifest: %1" ).arg( manifestPartPath );
      return false;
    }
    const QByteArray manifestJson = QJsonDocument( manifest ).toJson( QJsonDocument::Indented );
    if ( manifestFile.write( manifestJson ) != manifestJson.size() )
    {
      manifestFile.close();
      QFile::remove( manifestPartPath );
      if ( errorMessage )
        *errorMessage = QObject::tr( "Cannot write complete model manifest: %1" ).arg( manifestPartPath );
      return false;
    }
    manifestFile.close();
    QFile::remove( manifestPath );
    if ( !QFile::rename( manifestPartPath, manifestPath ) )
    {
      QFile::remove( manifestPartPath );
      if ( errorMessage )
        *errorMessage = QObject::tr( "Cannot move verified model manifest into place: %1" ).arg( manifestPath );
      return false;
    }

    progress.setValue( progress.maximum() );
    return true;
  }

  QString remoteEmbeddingModelSettingKey( const QString &providerId )
  {
    if ( providerId.compare( u"strata-cloud"_s, Qt::CaseInsensitive ) == 0 )
      return u"ai/embeddings/strata-cloud/model"_s;
    return providerId.compare( u"openrouter"_s, Qt::CaseInsensitive ) == 0 ? u"ai/embeddings/openrouter/model"_s : u"ai/embeddings/openai/model"_s;
  }

  QString remoteEmbeddingModelDefault( const QString &providerId )
  {
    if ( providerId.compare( u"strata-cloud"_s, Qt::CaseInsensitive ) == 0 )
      return u"strata-embedding-384"_s;
    return providerId.compare( u"openrouter"_s, Qt::CaseInsensitive ) == 0 ? u"openai/text-embedding-3-small"_s : u"text-embedding-3-small"_s;
  }

  QString gisProjectSettingsKey()
  {
    const QgsProject *project = QgsProject::instance();
    const QString projectFile = project ? project->fileName() : QString();
    return QgsAiGisSuggestionEngine::projectEnabledSettingsKey( projectFile );
  }

  bool hasByokProvider( const QgsAiModelRouter *modelRouter )
  {
    if ( !modelRouter )
      return false;
    return modelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::OpenAi )
           || modelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::OpenRouter )
           || modelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::Claude )
           || modelRouter->hasStoredOAuthRefreshToken( QgsAiModelRouter::Provider::Codex )
           || modelRouter->hasStoredOAuthRefreshToken( QgsAiModelRouter::Provider::Claude )
           || !qEnvironmentVariable( "OPENAI_API_KEY" ).trimmed().isEmpty()
           || !qEnvironmentVariable( "OPENROUTER_API_KEY" ).trimmed().isEmpty()
           || !qEnvironmentVariable( "ANTHROPIC_API_KEY" ).trimmed().isEmpty();
  }

  bool demoProjectReady()
  {
    QgsSettings settings;
    return settings.value( u"strata/onboarding/demo_project_seen"_s, false ).toBool() || ( QgsProject::instance() && !QgsProject::instance()->mapLayers().isEmpty() );
  }
} // namespace

QgsAiSettingsDialog::QgsAiSettingsDialog( QgsAiAgentSessionManager *sessionManager, QgsAiModelRouter *modelRouter, QgsAiLayerIndexCoordinator *layerIndexCoordinator, QWidget *parent )
  : QDialog( parent )
  , mSessionManager( sessionManager )
  , mModelRouter( modelRouter )
  , mLayerIndexCoordinator( layerIndexCoordinator )
{
  QgsAiPerfScope perf( u"settings"_s, u"dialog_open"_s, 100 );
  setWindowTitle( tr( "AI Settings" ) );
  setObjectName( u"aiSettingsDialog"_s );

  QVBoxLayout *rootLayout = new QVBoxLayout( this );
  rootLayout->setContentsMargins( 0, 0, 0, 0 );
  rootLayout->setSpacing( 0 );

  QWidget *body = new QWidget( this );
  QHBoxLayout *bodyLayout = new QHBoxLayout( body );
  bodyLayout->setContentsMargins( 0, 0, 0, 0 );
  bodyLayout->setSpacing( 0 );

  // Sidebar: mini account header on top, flat section list below.
  QWidget *sidebarPane = new QWidget( body );
  sidebarPane->setObjectName( u"aiSettingsSidebarPane"_s );
  sidebarPane->setFixedWidth( 200 );
  QVBoxLayout *sidebarLayout = new QVBoxLayout( sidebarPane );
  sidebarLayout->setContentsMargins( 0, 12, 0, 12 );
  sidebarLayout->setSpacing( 8 );

  mSidebarHeader = new QWidget( sidebarPane );
  mSidebarHeader->setCursor( Qt::PointingHandCursor );
  mSidebarHeader->setToolTip( tr( "Open the Account section" ) );
  QHBoxLayout *headerLayout = new QHBoxLayout( mSidebarHeader );
  headerLayout->setContentsMargins( 14, 0, 8, 0 );
  headerLayout->setSpacing( 8 );
  mSidebarAvatar = new QLabel( mSidebarHeader );
  mSidebarAvatar->setObjectName( u"aiSettingsSidebarAvatar"_s );
  mSidebarAvatar->setFixedSize( 28, 28 );
  mSidebarAvatar->setAlignment( Qt::AlignCenter );
  mSidebarEmailLabel = new QLabel( mSidebarHeader );
  headerLayout->addWidget( mSidebarAvatar );
  headerLayout->addWidget( mSidebarEmailLabel, 1 );
  mSidebarHeader->installEventFilter( this );
  sidebarLayout->addWidget( mSidebarHeader );

  mSidebarList = new QListWidget( sidebarPane );
  mSidebarList->setObjectName( u"aiSettingsSidebar"_s );
  mSidebarList->setFrameShape( QFrame::NoFrame );
  mSidebarList->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
  sidebarLayout->addWidget( mSidebarList, 1 );

  // Content: stacked pages inside a single scroll area.
  QgsScrollArea *scrollArea = new QgsScrollArea( body );
  scrollArea->setWidgetResizable( true );
  scrollArea->setFrameShape( QFrame::NoFrame );
  mStack = new QStackedWidget( scrollArea );
  mStack->setObjectName( u"aiSettingsStack"_s );
  scrollArea->setWidget( mStack );

  bodyLayout->addWidget( sidebarPane );
  bodyLayout->addWidget( scrollArea, 1 );
  rootLayout->addWidget( body, 1 );

  QFrame *buttonSeparator = new QFrame( this );
  buttonSeparator->setFrameShape( QFrame::HLine );
  buttonSeparator->setFrameShadow( QFrame::Sunken );
  rootLayout->addWidget( buttonSeparator );

  QDialogButtonBox *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
  connect( buttons, &QDialogButtonBox::accepted, this, &QDialog::accept );
  connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
  // Enter inside the auth (and other) line edits must submit the form action,
  // not fire the dialog-default OK button and close the whole dialog.
  const QList<QAbstractButton *> boxButtons = buttons->buttons();
  for ( QAbstractButton *boxButton : boxButtons )
  {
    if ( QPushButton *pushButton = qobject_cast<QPushButton *>( boxButton ) )
    {
      pushButton->setAutoDefault( false );
      pushButton->setDefault( false );
    }
  }
  QWidget *buttonRow = new QWidget( this );
  QHBoxLayout *buttonRowLayout = new QHBoxLayout( buttonRow );
  buttonRowLayout->setContentsMargins( 12, 8, 12, 8 );
  buttonRowLayout->addWidget( buttons );
  rootLayout->addWidget( buttonRow );

  // Build order matters: later pages capture widgets created by earlier ones
  // (cloud sync reads the Account endpoint, onboarding reads indexing/privacy).
  addSection( u"account"_s, tr( "Account" ), buildAccountPage() );
  addSection( u"providers"_s, tr( "Models & Providers" ), buildProvidersPage() );
  addSection( u"agent"_s, tr( "Agent" ), buildAgentPage() );
  addSection( u"rules"_s, tr( "Rules & Skills" ), buildRulesSkillsPage() );
  addSection( u"gallery"_s, tr( "Gallery & Connectors" ), buildGalleryConnectorsPage() );
  addSection( u"indexing"_s, tr( "Indexing & Docs" ), buildIndexingPage() );
  addSection( u"workspace"_s, tr( "Workspace" ), buildWorkspacePage() );
  addSection( u"privacy"_s, tr( "Privacy & Telemetry" ), buildPrivacyPage() );
  addSection( u"onboarding"_s, tr( "Onboarding & Release" ), buildOnboardingPage() );

  connect( mSidebarList, &QListWidget::currentRowChanged, this, [this]( int ) {
    if ( mSidebarList->currentItem() && mSidebarList->currentItem()->data( Qt::UserRole ).toString() == "gallery"_L1 )
      refreshGalleryAndConnectors();
  } );
  connect( mSidebarList, &QListWidget::currentRowChanged, mStack, &QStackedWidget::setCurrentIndex );
  mSidebarList->setCurrentRow( 0 );

  connect( mAccountWidget, &QgsAiAccountWidget::accountInfoChanged, this, &QgsAiSettingsDialog::refreshSidebarAccountHeader );
  connect( mAccountWidget, &QgsAiAccountWidget::authStateChanged, this, [this]() {
    refreshSidebarAccountHeader();
    refreshOnboardingStatus();
    const bool hasPlanSession = mModelRouter && !mModelRouter->planSessionToken().trimmed().isEmpty();
    mSyncCloudContextButton->setEnabled( mSessionManager && mSessionManager->workspaceIndex() && hasPlanSession );
    if ( mSyncRulesSkillsCloudButton )
      mSyncRulesSkillsCloudButton->setEnabled( hasPlanSession );
    if ( mImportRulesSkillsCloudButton )
      mImportRulesSkillsCloudButton->setEnabled( hasPlanSession );
    emit planAuthStateChanged();
  } );
  // Model enable/disable toggles should rebuild the chat model menu the same way an auth change does.
  connect( mAccountWidget, &QgsAiAccountWidget::modelPreferencesChanged, this, &QgsAiSettingsDialog::planAuthStateChanged );
  refreshSidebarAccountHeader();

  setStyleSheet( uR"css(
QWidget#aiSettingsSidebarPane { background: palette(alternate-base); border-right: 1px solid palette(mid); }
QListWidget#aiSettingsSidebar { background: transparent; border: none; outline: none; }
QListWidget#aiSettingsSidebar::item { padding: 7px 12px; border-radius: 6px; margin: 1px 8px; color: palette(window-text); }
QListWidget#aiSettingsSidebar::item:hover { background: palette(midlight); }
QListWidget#aiSettingsSidebar::item:selected { background: palette(midlight); color: palette(window-text); }
QLabel#aiSettingsSidebarAvatar { background: palette(highlight); color: palette(highlighted-text); border-radius: 14px; font-weight: 600; }
QLabel[aiRole="pageTitle"] { font-weight: 700; }
QLabel[aiRole="sectionHeader"] { font-weight: 600; margin-top: 8px; }
QLabel[aiRole="rowDescription"] { color: palette(dark); }
)css"_s );

  setMinimumSize( 720, 480 );
  if ( const QScreen *screen = QApplication::primaryScreen() )
  {
    const QSize availableSize = screen->availableGeometry().size();
    const int dialogWidth = std::min( 920, std::max( 720, availableSize.width() - 100 ) );
    const int dialogHeight = std::min( 640, std::max( 480, availableSize.height() - 120 ) );
    resize( dialogWidth, dialogHeight );
  }
  else
  {
    resize( 880, 620 );
  }
}

void QgsAiSettingsDialog::accept()
{
  if ( mSavingSecrets )
    return;
  // Login is a two-step async flow; closing mid-flight would orphan the
  // freshly minted desktop token and leave the user apparently signed out.
  if ( mAccountWidget && mAccountWidget->isBusy() )
  {
    QMessageBox::information( this, tr( "Account request in progress" ), tr( "Wait for the running login or account request to finish before closing the settings." ) );
    return;
  }
  QMap<QString, QString> credentials;
  for ( const auto &pair :
        { qMakePair( u"ai/provider/openai/apiKey"_s, mOpenAiKey->text().trimmed() ),
          qMakePair( u"ai/provider/openrouter/apiKey"_s, mOpenRouterKey->text().trimmed() ),
          qMakePair( u"ai/provider/claude/apiKey"_s, mClaudeConnectWidget->pendingApiKey() ),
          qMakePair( u"ai/provider/plan/token"_s, mAccountWidget->manualSessionToken() ) } )
    if ( !pair.second.isEmpty() )
      credentials.insert( pair.first, pair.second );
  mSavingSecrets = true;
  mStack->setEnabled( false );
  QgsAiCredentialDialog::save( this, credentials, [this]( bool saved ) {
    mSavingSecrets = false;
    mStack->setEnabled( true );
    QgsAiPerfScope perf( u"settings"_s, u"dialog_accept"_s, 100 );
    if ( !saved || !applySettings() )
      return;
    if ( mRequestedProvider )
    {
      if ( !mModelRouter->isProviderUsable( *mRequestedProvider ) )
      {
        QMessageBox::information( this, tr( "Configure a provider" ), tr( "Enter an API key before using this provider." ) );
        return;
      }
      mModelRouter->setActiveProvider( *mRequestedProvider );
      emit planAuthStateChanged();
    }
    QDialog::accept();
  } );
}

void QgsAiSettingsDialog::reject()
{
  if ( mSavingSecrets || ( mAccountWidget && mAccountWidget->isBusy() ) )
    return;
  QDialog::reject();
}

void QgsAiSettingsDialog::showSection( const QString &key )
{
  for ( int i = 0; i < mSidebarList->count(); ++i )
  {
    if ( mSidebarList->item( i )->data( Qt::UserRole ).toString() == key )
    {
      mSidebarList->setCurrentRow( i );
      return;
    }
  }
}

bool QgsAiSettingsDialog::eventFilter( QObject *watched, QEvent *event )
{
  if ( watched == mSidebarHeader && event->type() == QEvent::MouseButtonRelease )
  {
    showSection( u"account"_s );
    return true;
  }
  return QDialog::eventFilter( watched, event );
}

QWidget *QgsAiSettingsDialog::createPage( const QString &title, const QString &subtitle, QVBoxLayout *&contentLayout )
{
  QWidget *page = new QWidget();
  QVBoxLayout *outer = new QVBoxLayout( page );
  outer->setContentsMargins( 24, 20, 24, 24 );
  outer->setSpacing( 4 );

  QLabel *titleLabel = new QLabel( title, page );
  titleLabel->setProperty( "aiRole", u"pageTitle"_s );
  QFont titleFont = titleLabel->font();
  titleFont.setBold( true );
  titleFont.setPointSize( titleFont.pointSize() + 3 );
  titleLabel->setFont( titleFont );
  outer->addWidget( titleLabel );

  if ( !subtitle.isEmpty() )
  {
    QLabel *subtitleLabel = new QLabel( subtitle, page );
    subtitleLabel->setProperty( "aiRole", u"rowDescription"_s );
    subtitleLabel->setWordWrap( true );
    outer->addWidget( subtitleLabel );
  }

  QWidget *content = new QWidget( page );
  content->setMaximumWidth( 680 );
  contentLayout = new QVBoxLayout( content );
  contentLayout->setContentsMargins( 0, 12, 0, 0 );
  contentLayout->setSpacing( 8 );
  outer->addWidget( content );
  outer->addStretch( 1 );
  return page;
}

void QgsAiSettingsDialog::addSection( const QString &key, const QString &label, QWidget *page )
{
  QListWidgetItem *item = new QListWidgetItem( label, mSidebarList );
  item->setData( Qt::UserRole, key );
  mStack->addWidget( page );
}

QWidget *QgsAiSettingsDialog::buildAccountPage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Account" ), tr( "Sign in to Strata Cloud to use managed models and the cloud agent." ), contentLayout );
  mAccountWidget = new QgsAiAccountWidget( mModelRouter, mSessionManager, page );
  contentLayout->addWidget( mAccountWidget );
  return page;
}

QWidget *QgsAiSettingsDialog::buildProvidersPage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Models & Providers" ), tr( "Bring-your-own-key providers used when the Plan account is not active." ), contentLayout );

  auto *active
    = new QLabel( tr( "Active provider: %1 · %2" ).arg( mModelRouter->providerDisplayName( mModelRouter->activeProvider() ), mModelRouter->credentialStatus( mModelRouter->activeProvider() ) ), page );
  active->setWordWrap( true );
  contentLayout->addWidget( active );
  auto *migration = new QLabel( page );
  migration->setWordWrap( true );
  auto *retry = new QPushButton( tr( "Retry credential protection" ), page );
  const auto updateProtection = [this, active, migration, retry]() {
    const bool pending = QgsAiSecretStore::migrationPending();
    const bool unavailable = QgsAiSecretStore::unavailableCredentials();
    migration->setText(
      pending       ? tr( "Protection incomplete: some existing credentials could not yet be moved to the system keychain. They have been preserved." )
      : unavailable ? tr( "Unlock the system keychain, then retry. Your saved credentials have not been changed." )
                    : QString()
    );
    migration->setVisible( pending || unavailable );
    retry->setVisible( pending || unavailable );
    active->setText( tr( "Active provider: %1 · %2" ).arg( mModelRouter->providerDisplayName( mModelRouter->activeProvider() ), mModelRouter->credentialStatus( mModelRouter->activeProvider() ) ) );
  };
  updateProtection();
  contentLayout->addWidget( migration );
  contentLayout->addWidget( retry );
  connect( retry, &QPushButton::clicked, this, [this, retry, updateProtection]() {
    retry->setEnabled( false );
    QgsAiSecretStore::loadSecretsAsync(
      this,
      [retry, updateProtection]() {
        retry->setEnabled( true );
        updateProtection();
      },
      true
    );
  } );
  QgsAiSecretStore::loadSecretsAsync( this, updateProtection );

  // ---- Claude ----
  // Subscription login finishes in the browser. The API key stays an advanced option.
  contentLayout->addWidget( sectionHeader( tr( "Claude" ), page ) );
  mClaudeEndpoint = new QLineEdit( mModelRouter->providerSettings( QgsAiModelRouter::Provider::Claude ).endpoint, page );
  mClaudeConnectWidget = new QgsAiClaudeConnectWidget( mModelRouter, page );
  contentLayout->addWidget( mClaudeConnectWidget );
  connect( mClaudeConnectWidget, &QgsAiClaudeConnectWidget::cloudRequested, this, [this]() { showSection( u"account"_s ); } );
  connect( mClaudeConnectWidget, &QgsAiClaudeConnectWidget::useRequested, this, [this]() {
    mRequestedProvider = QgsAiModelRouter::Provider::Claude;
    accept();
  } );

  // ---- OpenAI ----
  contentLayout->addWidget( sectionHeader( tr( "OpenAI" ), page ) );
  mOpenAiEndpoint = new QLineEdit( mModelRouter->providerSettings( QgsAiModelRouter::Provider::OpenAi ).endpoint, page );
  mOpenAiModel = new QLineEdit( mModelRouter->providerSettings( QgsAiModelRouter::Provider::OpenAi ).model, page );
  const auto addUseButton = [this, page, contentLayout]( QgsAiModelRouter::Provider provider, const QString &name ) {
    auto *use = new QPushButton( tr( "Use in this chat" ), page );
    use->setObjectName( name );
    contentLayout->addWidget( use );
    connect( use, &QPushButton::clicked, this, [this, provider]() {
      mRequestedProvider = provider;
      accept();
    } );
  };
  mOpenAiKey = new QLineEdit( page );
  mOpenAiKey->setObjectName( u"aiOpenAiKeyLineEdit"_s );
  mOpenAiKey->setEchoMode( QLineEdit::Password );
  mOpenAiKey->setPlaceholderText( mModelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::OpenAi ) ? tr( "Saved locally — enter a new key only to replace it" ) : tr( "sk-..." ) );
  contentLayout->addWidget( settingRow( tr( "Model" ), QString(), mOpenAiModel, page ) );
  contentLayout->addWidget( settingRow( tr( "API key" ), tr( "Stored locally. Leave empty to keep the saved key." ), mOpenAiKey, page ) );

  addUseButton( QgsAiModelRouter::Provider::OpenAi, u"aiOpenAiUseButton"_s );

  // ---- OpenRouter ----
  contentLayout->addWidget( sectionHeader( tr( "OpenRouter" ), page ) );
  mOpenRouterEndpoint = new QLineEdit( mModelRouter->providerSettings( QgsAiModelRouter::Provider::OpenRouter ).endpoint, page );

  // Editable, searchable model picker fed by the async OpenRouter catalog
  // (tool-capable models with context size and pricing). Free text stays valid:
  // the current text is what gets persisted.
  mOpenRouterModel = new QComboBox( page );
  mOpenRouterModel->setObjectName( u"aiOpenRouterModelComboBox"_s );
  mOpenRouterModel->setEditable( true );
  mOpenRouterModel->setInsertPolicy( QComboBox::NoInsert );
  mOpenRouterModel->lineEdit()->setPlaceholderText( u"anthropic/claude-sonnet-4.6"_s );
  const QString configuredOpenRouterModel = mModelRouter->providerSettings( QgsAiModelRouter::Provider::OpenRouter ).model;
  mOpenRouterModel->setEditText( configuredOpenRouterModel );

  QgsAiOpenRouterModelCatalog *openRouterCatalog = new QgsAiOpenRouterModelCatalog( this );
  connect( openRouterCatalog, &QgsAiOpenRouterModelCatalog::modelsReady, this, [this]( const QList<QgsAiOpenRouterModelCatalog::ModelInfo> &models, bool ) {
    const QString currentText = mOpenRouterModel->currentText();
    mOpenRouterModel->clear();
    for ( const QgsAiOpenRouterModelCatalog::ModelInfo &model : models )
      mOpenRouterModel->addItem( model.displayLabel(), model.id );
    QCompleter *completer = new QCompleter( mOpenRouterModel->model(), mOpenRouterModel );
    completer->setFilterMode( Qt::MatchContains );
    completer->setCaseSensitivity( Qt::CaseInsensitive );
    mOpenRouterModel->setCompleter( completer );
    mOpenRouterModel->setEditText( currentText );
  } );
  // Selecting a catalog entry replaces the display label with the model id.
  connect( mOpenRouterModel, qOverload<int>( &QComboBox::activated ), this, [this]( int index ) {
    const QString modelId = mOpenRouterModel->itemData( index ).toString();
    if ( !modelId.isEmpty() )
      mOpenRouterModel->setEditText( modelId );
  } );
  openRouterCatalog->refresh();

  mOpenRouterKey = new QLineEdit( page );
  mOpenRouterKey->setEchoMode( QLineEdit::Password );
  mOpenRouterKey->setPlaceholderText( mModelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::OpenRouter ) ? tr( "Saved locally — enter a new key only to replace it" ) : tr( "sk-or-..." ) );

  // Connection test: validates the pending (or stored) key against GET /key and
  // shows the credits summary inline.
  QPushButton *openRouterTestButton = new QPushButton( tr( "Test connection" ), page );
  openRouterTestButton->setObjectName( u"aiOpenRouterTestConnectionButton"_s );
  QLabel *openRouterTestResult = new QLabel( page );
  openRouterTestResult->setObjectName( u"aiOpenRouterTestConnectionLabel"_s );
  openRouterTestResult->setWordWrap( true );
  connect( openRouterCatalog, &QgsAiOpenRouterModelCatalog::keyInfoReady, this, [openRouterTestButton, openRouterTestResult]( const QString &summary ) {
    openRouterTestButton->setEnabled( true );
    openRouterTestResult->setText( summary );
  } );
  connect( openRouterCatalog, &QgsAiOpenRouterModelCatalog::keyInfoFailed, this, [openRouterTestButton, openRouterTestResult]( const QString &errorMessage ) {
    openRouterTestButton->setEnabled( true );
    openRouterTestResult->setText( errorMessage );
  } );
  connect( openRouterTestButton, &QPushButton::clicked, this, [this, openRouterCatalog, openRouterTestButton, openRouterTestResult]() {
    QString key = mOpenRouterKey->text().trimmed();
    if ( key.isEmpty() )
      key = QgsAiSecretStore::readSecret( u"ai/provider/openrouter/apiKey"_s, { u"OPENROUTER_API_KEY"_s } );
    openRouterTestButton->setEnabled( false );
    openRouterTestResult->setText( tr( "Testing…" ) );
    openRouterCatalog->fetchKeyInfo( key );
  } );

  mOpenRouterAutoRouting = new QCheckBox( page );
  mOpenRouterAutoRouting->setChecked( mModelRouter->providerSettings( QgsAiModelRouter::Provider::OpenRouter ).autoRouting );

  contentLayout->addWidget( settingRow( tr( "Model" ), QString(), mOpenRouterModel, page ) );
  contentLayout->addWidget( settingRow( tr( "API key" ), tr( "Stored locally. Leave empty to keep the saved key." ), mOpenRouterKey, page ) );
  contentLayout->addWidget(
    settingRow( tr( "Automatic routing" ), tr( "Adds OpenRouter provider routing preferences (tool-capable providers, price/throughput sorting) and falls back to the Auto router if the pinned model is unavailable." ), mOpenRouterAutoRouting, page )
  );
  contentLayout->addWidget( settingRow( tr( "Connection" ), QString(), openRouterTestButton, page ) );
  contentLayout->addWidget( openRouterTestResult );

  addUseButton( QgsAiModelRouter::Provider::OpenRouter, u"aiOpenRouterUseButton"_s );

  QPushButton *clearAiCacheButton = new QPushButton( tr( "Clear AI cache" ), page );
  clearAiCacheButton->setObjectName( u"aiClearOpenRouterCacheButton"_s );
  connect( clearAiCacheButton, &QPushButton::clicked, this, [this]() {
    const QString text = tr(
      "This clears downloaded Strata Cloud model and policy catalogs and starts a new OpenRouter prompt-cache session. Chat history, model choices, credentials, and projects are kept. OpenRouter "
      "does not provide a global prompt-cache deletion API.\n\nContinue?"
    );
    if ( QMessageBox::question( this, tr( "Clear AI cache" ), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No ) != QMessageBox::Yes )
      return;
    QgsAiPlanClient::clearNetworkCaches();
    if ( mModelRouter )
      mModelRouter->resetPlanPromptCacheSession();
    emit planAuthStateChanged();
    QMessageBox::information( this, tr( "AI cache cleared" ), tr( "The next managed request starts a fresh OpenRouter prompt-cache session." ) );
  } );
  contentLayout->addWidget(
    settingRow( tr( "AI cache" ), tr( "Clears local cloud catalog cache and resets the OpenRouter prompt-cache session. It does not delete chat history or model preferences." ), clearAiCacheButton, page )
  );

  QCheckBox *diagnosticsJsonl = new QCheckBox( page );
  diagnosticsJsonl->setChecked( QgsSettings().value( u"strata/ai/diagnostics_jsonl_enabled"_s, true ).toBool() );
  connect( diagnosticsJsonl, &QCheckBox::toggled, this, []( bool enabled ) { QgsAiMessageLogBuffer::setDiagnosticsFileEnabled( enabled ); } );
  contentLayout->addWidget(
    settingRow( tr( "Diagnostic log" ), tr( "Writes local QGIS diagnostics to strata_ai_diagnostics.jsonl. This replaces the unavailable QGIS_LOG_FILE workflow and never uploads logs." ), diagnosticsJsonl, page )
  );

  // ---- Codex ----
  contentLayout->addWidget( sectionHeader( tr( "Codex" ), page ) );
  mCodexEndpoint = new QLineEdit( mModelRouter->providerSettings( QgsAiModelRouter::Provider::Codex ).endpoint, page );
  mCodexModel = new QLineEdit( mModelRouter->providerSettings( QgsAiModelRouter::Provider::Codex ).model, page );
  mCodexModel->setPlaceholderText( u"gpt-5.4"_s );
  mCodexStatus = new QLabel( mModelRouter->hasStoredOAuthRefreshToken( QgsAiModelRouter::Provider::Codex ) ? tr( "Signed in" ) : tr( "Not signed in" ), page );
  QPushButton *codexRequestCodeButton = new QPushButton( tr( "Get device code" ), page );
  QPushButton *codexCompleteLoginButton = new QPushButton( tr( "Complete login" ), page );
  QPushButton *codexLogoutButton = new QPushButton( tr( "Log out" ), page );
  QWidget *codexButtons = new QWidget( page );
  QHBoxLayout *codexButtonsLayout = new QHBoxLayout( codexButtons );
  codexButtonsLayout->setContentsMargins( 0, 0, 0, 0 );
  codexButtonsLayout->addWidget( codexRequestCodeButton );
  codexButtonsLayout->addWidget( codexCompleteLoginButton );
  codexButtonsLayout->addWidget( codexLogoutButton );

  contentLayout->addWidget( settingRow( tr( "Model" ), QString(), mCodexModel, page ) );
  contentLayout->addWidget( settingRow( tr( "Status" ), tr( "Codex uses a device-code OAuth login." ), mCodexStatus, page ) );
  contentLayout->addWidget( settingRow( tr( "Account" ), QString(), codexButtons, page ) );

  connect( codexRequestCodeButton, &QPushButton::clicked, this, [this]() {
    QString error;
    if ( !QgsAiCodexOAuthClient::requestDeviceCode( mCodexDeviceCode, &error ) )
    {
      QMessageBox::warning( this, tr( "Codex login failed" ), error );
      return;
    }

    mCodexStatus->setText( tr( "Open %1 and enter code %2" ).arg( mCodexDeviceCode.verificationUrl, mCodexDeviceCode.userCode ) );
    QDesktopServices::openUrl( QUrl( mCodexDeviceCode.verificationUrl ) );
    QMessageBox::information( this, tr( "Codex device code" ), tr( "Open %1 and enter this code:\n\n%2\n\nThen click Complete login." ).arg( mCodexDeviceCode.verificationUrl, mCodexDeviceCode.userCode ) );
  } );

  connect( codexCompleteLoginButton, &QPushButton::clicked, this, [this]() {
    if ( mCodexDeviceCode.deviceAuthId.isEmpty() )
    {
      QMessageBox::information( this, tr( "Codex login" ), tr( "Request a Codex device code first." ) );
      return;
    }

    QString error;
    QString refreshToken;
    mSavingSecrets = true;
    if ( !QgsAiCodexOAuthClient::completeDeviceCodeLogin( mCodexDeviceCode, &error, &refreshToken ) )
    {
      mSavingSecrets = false;
      QMessageBox::warning( this, tr( "Codex login failed" ), error );
      return;
    }
    QgsAiCredentialDialog::save( this, { { QgsAiCodexOAuthClient::refreshTokenSettingKey(), refreshToken } }, [this]( bool saved ) {
      mSavingSecrets = false;
      if ( saved )
        mCodexStatus->setText( tr( "Configured" ) );
    } );
  } );

  connect( codexLogoutButton, &QPushButton::clicked, this, [this]() {
    QString error;
    if ( !QgsAiCodexOAuthClient::clearRefreshToken( &error ) )
    {
      QMessageBox::warning( this, tr( "Codex logout failed" ), error );
      return;
    }
    mCodexStatus->setText( tr( "Not signed in" ) );
  } );

  auto *protectCodex = new QPushButton( tr( "Complete credential protection" ), page );
  protectCodex->setVisible( !QgsAiCodexOAuthClient::credentialAwaitingProtection().isEmpty() );
  contentLayout->addWidget( protectCodex );
  connect( protectCodex, &QPushButton::clicked, this, [this, protectCodex]() {
    mSavingSecrets = true;
    mStack->setEnabled( false );
    QgsAiCredentialDialog::save( this, { { QgsAiCodexOAuthClient::refreshTokenSettingKey(), QgsAiCodexOAuthClient::credentialAwaitingProtection() } }, [this, protectCodex]( bool saved ) {
      mSavingSecrets = false;
      mStack->setEnabled( true );
      if ( saved )
      {
        protectCodex->hide();
        mCodexStatus->setText( mModelRouter->credentialStatus( QgsAiModelRouter::Provider::Codex ) );
      }
    } );
  } );
  addUseButton( QgsAiModelRouter::Provider::Codex, u"aiCodexUseButton"_s );

  // ---- Advanced endpoints ----
  QgsCollapsibleGroupBox *advancedEndpoints = new QgsCollapsibleGroupBox( tr( "Advanced endpoints" ), page );
  QFormLayout *endpointsForm = new QFormLayout( advancedEndpoints );
  endpointsForm->addRow( tr( "OpenAI endpoint" ), mOpenAiEndpoint );
  endpointsForm->addRow( tr( "OpenRouter endpoint" ), mOpenRouterEndpoint );
  endpointsForm->addRow( tr( "Codex endpoint" ), mCodexEndpoint );
  endpointsForm->addRow( tr( "Claude endpoint" ), mClaudeEndpoint );
  advancedEndpoints->setCollapsed( true );
  contentLayout->addWidget( advancedEndpoints );

  return page;
}

QWidget *QgsAiSettingsDialog::buildAgentPage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Agent" ), tr( "How the assistant is allowed to act on your project." ), contentLayout );

  const QgsAiAgentBehaviorSettings currentBehavior = mSessionManager ? mSessionManager->agentBehaviorSettings() : QgsAiAgentBehaviorSettings();

  mAllowCustomActions = new QCheckBox( page );
  mAllowCustomActions->setChecked( currentBehavior.allowCustomActions );
  contentLayout->addWidget(
    settingRow( tr( "Allow custom agent actions" ), tr( "When enabled, the agent can call tools like read_file, propose_edit, run_python. Destructive tools still require confirmation in their dedicated review dialogs." ), mAllowCustomActions, page )
  );

  mRememberPythonApprovalsForSession = new QCheckBox( page );
  mRememberPythonApprovalsForSession->setChecked( currentBehavior.rememberPythonApprovalsForSession );
  contentLayout->addWidget(
    settingRow( tr( "Remember Python approvals for this session" ), tr( "After you approve one safe Python execution, Strata can run subsequent low-risk Python snippets in this app session without asking again. High-risk code still asks." ), mRememberPythonApprovalsForSession, page )
  );

  mRunPythonTimeoutSeconds = new QSpinBox( page );
  mRunPythonTimeoutSeconds->setObjectName( u"aiRunPythonTimeoutSecondsSpinBox"_s );
  mRunPythonTimeoutSeconds->setRange( QgsAiAgentBehaviorSettings::MIN_RUN_PYTHON_TIMEOUT_SECONDS, QgsAiAgentBehaviorSettings::MAX_RUN_PYTHON_TIMEOUT_SECONDS );
  mRunPythonTimeoutSeconds->setValue( currentBehavior.runPythonTimeoutSeconds );
  contentLayout->addWidget(
    settingRow( tr( "Python timeout (s)" ), tr( "Stop run_python after this many seconds of Python. Time spent inside Processing algorithms is excluded." ), mRunPythonTimeoutSeconds, page )
  );

  mMaxToolIterationsPerTurn = new QSpinBox( page );
  mMaxToolIterationsPerTurn->setObjectName( u"aiMaxToolIterationsPerTurnSpinBox"_s );
  mMaxToolIterationsPerTurn->setRange( QgsAiAgentBehaviorSettings::MIN_TOOL_CALL_PAUSE_LIMIT, QgsAiAgentBehaviorSettings::MAX_TOOL_CALL_PAUSE_LIMIT );
  mMaxToolIterationsPerTurn->setValue( currentBehavior.maxToolIterationsPerTurn );
  contentLayout->addWidget(
    settingRow( tr( "Maximum tool calls before pause" ), tr( "The agent pauses after this many tool-use rounds and shows a Continue button before running another block." ), mMaxToolIterationsPerTurn, page )
  );

  mMaxTotalToolIterationsPerTurn = new QSpinBox( page );
  mMaxTotalToolIterationsPerTurn->setObjectName( u"aiMaxTotalToolIterationsPerTurnSpinBox"_s );
  mMaxTotalToolIterationsPerTurn->setRange( QgsAiAgentBehaviorSettings::MIN_TOTAL_TOOL_CALL_LIMIT, QgsAiAgentBehaviorSettings::MAX_TOTAL_TOOL_CALL_LIMIT );
  mMaxTotalToolIterationsPerTurn->setValue( currentBehavior.maxTotalToolIterationsPerTurn );
  contentLayout->addWidget(
    settingRow( tr( "Maximum tool calls per user turn" ), tr( "Hard cumulative limit across every Continue block. Reaching it stops the turn instead of allowing an unbounded loop." ), mMaxTotalToolIterationsPerTurn, page )
  );

  mAutoContinueToolBlocks = new QCheckBox( page );
  mAutoContinueToolBlocks->setObjectName( u"aiAutoContinueToolBlocksCheckBox"_s );
  mAutoContinueToolBlocks->setChecked( currentBehavior.autoContinueToolBlocks );
  contentLayout->addWidget(
    settingRow( tr( "Continue tool blocks automatically" ), tr( "Continue at each pause without asking, while still enforcing the cumulative per-turn limit and retry safeguards." ), mAutoContinueToolBlocks, page )
  );

  QgsSettings gisToggleSettings;
  mGisSuggestionsEnabled = new QCheckBox( page );
  mGisSuggestionsEnabled->setObjectName( u"aiGisGlobalEnableCheckBox"_s );
  mGisSuggestionsEnabled->setChecked( gisToggleSettings.value( QgsAiGisSuggestionEngine::globalEnabledSettingsKey(), true ).toBool() );
  contentLayout->addWidget(
    settingRow( tr( "Enable GIS suggestions" ), tr( "Rule-based project health suggestions: shown as a card in the chat and injected into the model context." ), mGisSuggestionsEnabled, page )
  );

  mGisSuggestionsProjectEnabled = new QCheckBox( page );
  mGisSuggestionsProjectEnabled->setObjectName( u"aiGisProjectEnableCheckBox"_s );
  mGisSuggestionsProjectEnabled->setChecked( gisToggleSettings.value( gisProjectSettingsKey(), true ).toBool() );
  contentLayout->addWidget( settingRow( tr( "Enable GIS suggestions for this project" ), QString(), mGisSuggestionsProjectEnabled, page ) );

  return page;
}

QWidget *QgsAiSettingsDialog::buildRulesSkillsPage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Rules & Skills" ), tr( "Standing instructions and reusable playbooks the agent can draw on, per workspace." ), contentLayout );

  const QgsAiAgentBehaviorSettings currentBehavior = mSessionManager ? mSessionManager->agentBehaviorSettings() : QgsAiAgentBehaviorSettings();
  mRulesRelativeDirForList = currentBehavior.rulesPath;
  mSkillsRelativeDirForList = currentBehavior.skillsPath;

  mRulesSkillsTrustBanner = new QLabel( page );
  mRulesSkillsTrustBanner->setWordWrap( true );
  mRulesSkillsTrustBanner->setProperty( "aiRole", u"rowDescription"_s );
  contentLayout->addWidget( mRulesSkillsTrustBanner );

  // ---- Rules ----
  contentLayout->addWidget( sectionHeader( tr( "Rules" ), page ) );
  QLabel *rulesHint = new QLabel( tr( "Rules are Markdown files stored in the workspace. The title comes from frontmatter name, the first Markdown heading, or the first non-empty line." ), page );
  rulesHint->setWordWrap( true );
  rulesHint->setProperty( "aiRole", u"rowDescription"_s );
  contentLayout->addWidget( rulesHint );

  mRulesListWidget = new QListWidget( page );
  mRulesListWidget->setFixedHeight( 110 );
  contentLayout->addWidget( mRulesListWidget );

  mRuleNewButton = new QPushButton( tr( "+ New rule" ), page );
  contentLayout->addWidget( mRuleNewButton );

  mRuleEditorWidget = new QWidget( page );
  QVBoxLayout *ruleEditorLayout = new QVBoxLayout( mRuleEditorWidget );
  ruleEditorLayout->setContentsMargins( 0, 4, 0, 4 );

  mRuleBodyEdit = new QTextEdit( mRuleEditorWidget );
  mRuleBodyEdit->setAcceptRichText( false );
  mRuleBodyEdit->setPlaceholderText( tr( "# Rule title\n\nWrite the rule in Markdown. Optional frontmatter like alwaysApply, enabled, description, and globs is preserved." ) );
  mRuleBodyEdit->setFixedHeight( 190 );
  ruleEditorLayout->addWidget( settingRowFullWidth( tr( "Markdown" ), QString(), mRuleBodyEdit, mRuleEditorWidget ) );

  QHBoxLayout *ruleButtonsLayout = new QHBoxLayout();
  mRuleSaveButton = new QPushButton( tr( "Save" ), mRuleEditorWidget );
  mRuleDuplicateButton = new QPushButton( tr( "Duplicate" ), mRuleEditorWidget );
  mRuleDeleteButton = new QPushButton( tr( "Delete" ), mRuleEditorWidget );
  ruleButtonsLayout->addWidget( mRuleSaveButton );
  ruleButtonsLayout->addWidget( mRuleDuplicateButton );
  ruleButtonsLayout->addWidget( mRuleDeleteButton );
  ruleButtonsLayout->addStretch( 1 );
  ruleEditorLayout->addLayout( ruleButtonsLayout );

  contentLayout->addWidget( mRuleEditorWidget );

  connect( mRulesListWidget, &QListWidget::currentItemChanged, this, [this]( QListWidgetItem *current, QListWidgetItem * ) {
    if ( current )
      selectRuleInEditor( current->data( Qt::UserRole ).value<QgsAiRuleInfo>() );
  } );
  connect( mRuleNewButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::newRule );
  connect( mRuleDuplicateButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::duplicateSelectedRule );
  connect( mRuleDeleteButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::deleteSelectedRule );
  connect( mRuleSaveButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::saveCurrentRule );

  // ---- Skills ----
  contentLayout->addWidget( sectionHeader( tr( "Skills" ), page ) );
  QLabel *skillsHint = new QLabel( tr( "Skills are always listed by name/description only; the agent reads the full SKILL.md content once it decides a skill applies." ), page );
  skillsHint->setWordWrap( true );
  skillsHint->setProperty( "aiRole", u"rowDescription"_s );
  contentLayout->addWidget( skillsHint );

  mSkillsListWidget = new QListWidget( page );
  mSkillsListWidget->setFixedHeight( 110 );
  contentLayout->addWidget( mSkillsListWidget );

  mSkillNewButton = new QPushButton( tr( "+ New skill" ), page );
  contentLayout->addWidget( mSkillNewButton );

  mSkillEditorWidget = new QWidget( page );
  QVBoxLayout *skillEditorLayout = new QVBoxLayout( mSkillEditorWidget );
  skillEditorLayout->setContentsMargins( 0, 4, 0, 4 );

  skillEditorLayout->addWidget( sectionHeader( tr( "Properties" ), mSkillEditorWidget ) );

  QWidget *skillPropertiesWidget = new QWidget( mSkillEditorWidget );
  mSkillPropertiesLayout = new QVBoxLayout( skillPropertiesWidget );
  mSkillPropertiesLayout->setContentsMargins( 0, 0, 0, 0 );
  mSkillPropertiesLayout->setSpacing( 6 );
  skillEditorLayout->addWidget( skillPropertiesWidget );

  mSkillAddPropertyButton = new QPushButton( tr( "+ Add property" ), mSkillEditorWidget );
  skillEditorLayout->addWidget( mSkillAddPropertyButton );

  mSkillBodyEdit = new QTextEdit( mSkillEditorWidget );
  mSkillBodyEdit->setAcceptRichText( false );
  mSkillBodyEdit->setPlaceholderText( tr( "# Skill title\n\nWrite detailed Markdown instructions here." ) );
  mSkillBodyEdit->setFixedHeight( 190 );
  skillEditorLayout->addWidget( settingRowFullWidth( tr( "Markdown body" ), QString(), mSkillBodyEdit, mSkillEditorWidget ) );

  QHBoxLayout *skillButtonsLayout = new QHBoxLayout();
  mSkillSaveButton = new QPushButton( tr( "Save" ), mSkillEditorWidget );
  mSkillDuplicateButton = new QPushButton( tr( "Duplicate" ), mSkillEditorWidget );
  mSkillDeleteButton = new QPushButton( tr( "Delete" ), mSkillEditorWidget );
  skillButtonsLayout->addWidget( mSkillSaveButton );
  skillButtonsLayout->addWidget( mSkillDuplicateButton );
  skillButtonsLayout->addWidget( mSkillDeleteButton );
  skillButtonsLayout->addStretch( 1 );
  skillEditorLayout->addLayout( skillButtonsLayout );

  contentLayout->addWidget( mSkillEditorWidget );

  connect( mSkillsListWidget, &QListWidget::currentItemChanged, this, [this]( QListWidgetItem *current, QListWidgetItem * ) {
    if ( current )
      selectSkillInEditor( current->data( Qt::UserRole ).value<QgsAiSkillInfo>() );
  } );
  connect( mSkillNewButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::newSkill );
  connect( mSkillDuplicateButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::duplicateSelectedSkill );
  connect( mSkillDeleteButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::deleteSelectedSkill );
  connect( mSkillSaveButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::saveCurrentSkill );
  connect( mSkillAddPropertyButton, &QPushButton::clicked, this, [this]() { addSkillPropertyRow( QString(), QStringList(), false, false ); } );

  refreshRulesList();
  refreshSkillsList();
  refreshRulesSkillsTrustState();

  QWidget *cloudButtons = new QWidget( page );
  QHBoxLayout *cloudButtonsLayout = new QHBoxLayout( cloudButtons );
  cloudButtonsLayout->setContentsMargins( 0, 0, 0, 0 );
  mSyncRulesSkillsCloudButton = new QPushButton( tr( "Push to Strata Cloud" ), cloudButtons );
  mSyncRulesSkillsCloudButton->setObjectName( u"aiSyncRulesSkillsCloudButton"_s );
  mSyncRulesSkillsCloudButton->setEnabled( mModelRouter && !mModelRouter->planSessionToken().trimmed().isEmpty() );
  mImportRulesSkillsCloudButton = new QPushButton( tr( "Import from Strata Cloud…" ), cloudButtons );
  mImportRulesSkillsCloudButton->setObjectName( u"aiImportRulesSkillsCloudButton"_s );
  mImportRulesSkillsCloudButton->setEnabled( mModelRouter && !mModelRouter->planSessionToken().trimmed().isEmpty() );
  cloudButtonsLayout->addWidget( mSyncRulesSkillsCloudButton );
  cloudButtonsLayout->addWidget( mImportRulesSkillsCloudButton );
  contentLayout->addWidget( settingRow( tr( "Cloud copy (opt-in)" ), tr( "Explicit copy/upsert only: no automatic merge, tombstones, or deletion of local/cloud-only items." ), cloudButtons, page ) );
  mRulesSkillsCloudStatusLabel = new QLabel( page );
  mRulesSkillsCloudStatusLabel->setWordWrap( true );
  mRulesSkillsCloudStatusLabel->setProperty( "aiRole", u"rowDescription"_s );
  contentLayout->addWidget( mRulesSkillsCloudStatusLabel );
  connect( mSyncRulesSkillsCloudButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::syncRulesSkillsToCloud );
  connect( mImportRulesSkillsCloudButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::importRulesSkillsFromCloud );

  return page;
}

QgsAiRulesSkillsStore QgsAiSettingsDialog::rulesSkillsStore() const
{
  return QgsAiRulesSkillsStore( mSessionManager ? mSessionManager->fileContextProvider() : nullptr );
}

bool QgsAiSettingsDialog::rulesSkillsWritable() const
{
  if ( !mSessionManager )
    return false;
  const QString root = mSessionManager->workspaceRoot();
  return !root.isEmpty() && QgsAiWorkspaceTrust::isTrusted( root );
}

void QgsAiSettingsDialog::refreshRulesSkillsTrustState()
{
  if ( !mRulesSkillsTrustBanner )
    return;
  const bool writable = rulesSkillsWritable();
  mRulesSkillsTrustBanner->setVisible( !writable );
  if ( !writable )
  {
    mRulesSkillsTrustBanner->setText(
      mSessionManager && !mSessionManager->workspaceRoot().isEmpty() ? tr( "Trust this workspace (Workspace section) to create, edit or delete rules and skills." )
                                                                     : tr( "Configure a workspace (Workspace section) to create rules and skills." )
    );
  }
  if ( mRuleNewButton )
    mRuleNewButton->setEnabled( writable );
  if ( mSkillNewButton )
    mSkillNewButton->setEnabled( writable );
  if ( mRuleBodyEdit )
    mRuleBodyEdit->setEnabled( writable );
  if ( mRuleSaveButton )
    mRuleSaveButton->setEnabled( writable );
  if ( mRuleDuplicateButton )
    mRuleDuplicateButton->setEnabled( writable && !mCurrentRuleSlug.isEmpty() );
  if ( mRuleDeleteButton )
    mRuleDeleteButton->setEnabled( writable && !mCurrentRuleSlug.isEmpty() );
  for ( SkillPropertyRow &row : mSkillPropertyRows )
  {
    if ( row.keyEdit )
      row.keyEdit->setEnabled( writable && !row.required );
    if ( row.valueEdit )
      row.valueEdit->setEnabled( writable );
    if ( row.removeButton )
      row.removeButton->setEnabled( writable && !row.required );
  }
  if ( mSkillAddPropertyButton )
    mSkillAddPropertyButton->setEnabled( writable );
  if ( mSkillBodyEdit )
    mSkillBodyEdit->setEnabled( writable );
  if ( mSkillSaveButton )
    mSkillSaveButton->setEnabled( writable );
  if ( mSkillDuplicateButton )
    mSkillDuplicateButton->setEnabled( writable && !mCurrentSkillSlug.isEmpty() );
  if ( mSkillDeleteButton )
    mSkillDeleteButton->setEnabled( writable && !mCurrentSkillSlug.isEmpty() );
}

void QgsAiSettingsDialog::refreshRulesList()
{
  if ( !mRulesListWidget )
    return;

  mRulesListWidget->blockSignals( true );
  mRulesListWidget->clear();
  const QList<QgsAiRuleInfo> rules = rulesSkillsStore().listRules( mRulesRelativeDirForList );
  for ( const QgsAiRuleInfo &rule : rules )
  {
    QString label = rule.name;
    label += rule.alwaysApply ? tr( "  \u00b7  Always" ) : tr( "  \u00b7  Manual" );
    if ( !rule.enabled )
      label += tr( "  \u00b7  disabled" );
    QListWidgetItem *item = new QListWidgetItem( label, mRulesListWidget );
    item->setData( Qt::UserRole, QVariant::fromValue( rule ) );
  }
  mRulesListWidget->blockSignals( false );

  if ( mRulesListWidget->count() == 0 )
  {
    clearRuleEditor();
    return;
  }

  int rowToSelect = 0;
  for ( int i = 0; i < mRulesListWidget->count(); ++i )
  {
    if ( !mCurrentRuleSlug.isEmpty() && mRulesListWidget->item( i )->data( Qt::UserRole ).value<QgsAiRuleInfo>().slug == mCurrentRuleSlug )
    {
      rowToSelect = i;
      break;
    }
  }
  mRulesListWidget->setCurrentRow( rowToSelect );
  selectRuleInEditor( mRulesListWidget->item( rowToSelect )->data( Qt::UserRole ).value<QgsAiRuleInfo>() );
}

void QgsAiSettingsDialog::selectRuleInEditor( const QgsAiRuleInfo &rule )
{
  mCurrentRuleSlug = rule.slug;
  mCurrentRulePath = rule.path;
  mRuleBodyEdit->setPlainText( rulesSkillsStore().readRuleMarkdown( rule ) );

  const bool writable = rulesSkillsWritable();
  mRuleBodyEdit->setEnabled( writable );
  mRuleSaveButton->setEnabled( writable );
  mRuleDuplicateButton->setEnabled( writable );
  mRuleDeleteButton->setEnabled( writable );
}

void QgsAiSettingsDialog::clearRuleEditor()
{
  mCurrentRuleSlug.clear();
  mCurrentRulePath.clear();
  mRuleBodyEdit->clear();

  const bool writable = rulesSkillsWritable();
  mRuleBodyEdit->setEnabled( writable );
  mRuleSaveButton->setEnabled( writable );
  mRuleDuplicateButton->setEnabled( false );
  mRuleDeleteButton->setEnabled( false );
}

void QgsAiSettingsDialog::newRule()
{
  mRulesListWidget->setCurrentRow( -1 );
  clearRuleEditor();
  mRuleBodyEdit->setFocus();
}

void QgsAiSettingsDialog::duplicateSelectedRule()
{
  if ( mCurrentRuleSlug.isEmpty() || !rulesSkillsWritable() )
    return;

  QStringList existingSlugs;
  for ( int i = 0; i < mRulesListWidget->count(); ++i )
    existingSlugs << mRulesListWidget->item( i )->data( Qt::UserRole ).value<QgsAiRuleInfo>().slug;

  const QString markdown = mRuleBodyEdit->toPlainText();
  const QString title = QgsAiRulesSkillsStore::titleFromMarkdown( markdown, mCurrentRuleSlug );
  const QString baseSlug = QgsAiRulesSkillsStore::slugify( title + u"-copy"_s );
  QString candidate = baseSlug;
  int suffix = 2;
  while ( existingSlugs.contains( candidate ) )
    candidate = u"%1-%2"_s.arg( baseSlug ).arg( suffix++ );

  QString error;
  if ( !rulesSkillsStore().writeRuleMarkdown( mRulesRelativeDirForList, candidate, markdown, &error ) )
  {
    QMessageBox::warning( this, tr( "Could not duplicate rule" ), error );
    return;
  }
  mCurrentRuleSlug = candidate;
  refreshRulesList();
}

void QgsAiSettingsDialog::deleteSelectedRule()
{
  if ( mCurrentRuleSlug.isEmpty() )
    return;
  const QString title = QgsAiRulesSkillsStore::titleFromMarkdown( mRuleBodyEdit->toPlainText(), mCurrentRuleSlug );
  if ( QMessageBox::question( this, tr( "Delete rule" ), tr( "Delete rule \u201c%1\u201d? This cannot be undone." ).arg( title ) ) != QMessageBox::Yes )
    return;

  QgsAiRuleInfo info;
  info.slug = mCurrentRuleSlug;
  info.path = mCurrentRulePath;
  QString error;
  if ( !rulesSkillsStore().deleteRule( info, &error ) )
  {
    QMessageBox::warning( this, tr( "Could not delete rule" ), error );
    return;
  }
  mCurrentRuleSlug.clear();
  mCurrentRulePath.clear();
  refreshRulesList();
}

void QgsAiSettingsDialog::saveCurrentRule()
{
  if ( !rulesSkillsWritable() )
  {
    QMessageBox::warning( this, tr( "Workspace not trusted" ), tr( "Trust this workspace (Workspace section) before creating or editing rules." ) );
    return;
  }
  const QString markdown = mRuleBodyEdit->toPlainText().trimmed();
  if ( markdown.isEmpty() )
  {
    QMessageBox::warning( this, tr( "Content required" ), tr( "Write the rule Markdown before saving." ) );
    return;
  }

  QString slug = mCurrentRuleSlug;
  if ( slug.isEmpty() )
  {
    QStringList existingSlugs;
    for ( int i = 0; i < mRulesListWidget->count(); ++i )
      existingSlugs << mRulesListWidget->item( i )->data( Qt::UserRole ).value<QgsAiRuleInfo>().slug;

    const QString title = QgsAiRulesSkillsStore::titleFromMarkdown( markdown, u"untitled"_s );
    const QString baseSlug = QgsAiRulesSkillsStore::slugify( title );
    slug = baseSlug;
    int suffix = 2;
    while ( existingSlugs.contains( slug ) )
      slug = u"%1-%2"_s.arg( baseSlug ).arg( suffix++ );
  }

  QString error;
  if ( !rulesSkillsStore().writeRuleMarkdown( mRulesRelativeDirForList, slug, markdown, &error ) )
  {
    QMessageBox::warning( this, tr( "Could not save rule" ), error );
    return;
  }
  mCurrentRuleSlug = slug;
  refreshRulesList();
}

void QgsAiSettingsDialog::refreshSkillsList()
{
  if ( !mSkillsListWidget )
    return;

  mSkillsListWidget->blockSignals( true );
  mSkillsListWidget->clear();
  const QList<QgsAiSkillInfo> skills = rulesSkillsStore().listSkills( mSkillsRelativeDirForList );
  for ( const QgsAiSkillInfo &skill : skills )
  {
    QString label = skill.name;
    if ( !skill.enabled )
      label += tr( "  \u00b7  disabled" );
    QListWidgetItem *item = new QListWidgetItem( label, mSkillsListWidget );
    item->setData( Qt::UserRole, QVariant::fromValue( skill ) );
  }
  mSkillsListWidget->blockSignals( false );

  if ( mSkillsListWidget->count() == 0 )
  {
    clearSkillEditor();
    return;
  }

  int rowToSelect = 0;
  for ( int i = 0; i < mSkillsListWidget->count(); ++i )
  {
    if ( !mCurrentSkillSlug.isEmpty() && mSkillsListWidget->item( i )->data( Qt::UserRole ).value<QgsAiSkillInfo>().slug == mCurrentSkillSlug )
    {
      rowToSelect = i;
      break;
    }
  }
  mSkillsListWidget->setCurrentRow( rowToSelect );
  selectSkillInEditor( mSkillsListWidget->item( rowToSelect )->data( Qt::UserRole ).value<QgsAiSkillInfo>() );
}

void QgsAiSettingsDialog::selectSkillInEditor( const QgsAiSkillInfo &skill )
{
  mCurrentSkillSlug = skill.slug;
  setSkillDocumentInEditor( QgsAiRulesSkillsStore::parseMarkdownDocument( rulesSkillsStore().readSkillMarkdown( skill ) ) );

  const bool writable = rulesSkillsWritable();
  for ( SkillPropertyRow &row : mSkillPropertyRows )
  {
    row.keyEdit->setEnabled( writable && !row.required );
    row.valueEdit->setEnabled( writable );
    if ( row.removeButton )
      row.removeButton->setEnabled( writable && !row.required );
  }
  if ( mSkillAddPropertyButton )
    mSkillAddPropertyButton->setEnabled( writable );
  mSkillBodyEdit->setEnabled( writable );
  mSkillSaveButton->setEnabled( writable );
  mSkillDuplicateButton->setEnabled( writable );
  mSkillDeleteButton->setEnabled( writable );
}

void QgsAiSettingsDialog::clearSkillEditor()
{
  mCurrentSkillSlug.clear();
  QgsAiMarkdownDocument document;
  document.properties.append( { u"name"_s, QStringList(), false } );
  document.properties.append( { u"description"_s, QStringList(), false } );
  document.properties.append( { u"references"_s, QStringList(), true } );
  setSkillDocumentInEditor( document );

  const bool writable = rulesSkillsWritable();
  for ( SkillPropertyRow &row : mSkillPropertyRows )
  {
    row.keyEdit->setEnabled( writable && !row.required );
    row.valueEdit->setEnabled( writable );
    if ( row.removeButton )
      row.removeButton->setEnabled( writable && !row.required );
  }
  if ( mSkillAddPropertyButton )
    mSkillAddPropertyButton->setEnabled( writable );
  mSkillBodyEdit->setEnabled( writable );
  mSkillSaveButton->setEnabled( writable );
  mSkillDuplicateButton->setEnabled( false );
  mSkillDeleteButton->setEnabled( false );
}

void QgsAiSettingsDialog::clearSkillPropertyRows()
{
  for ( const SkillPropertyRow &row : std::as_const( mSkillPropertyRows ) )
    delete row.rowWidget;
  mSkillPropertyRows.clear();
}

void QgsAiSettingsDialog::addSkillPropertyRow( const QString &key, const QStringList &values, bool isList, bool required )
{
  if ( !mSkillPropertiesLayout )
    return;

  QWidget *rowWidget = new QWidget( mSkillEditorWidget );
  QHBoxLayout *rowLayout = new QHBoxLayout( rowWidget );
  rowLayout->setContentsMargins( 0, 0, 0, 0 );
  rowLayout->setSpacing( 6 );

  QLineEdit *keyEdit = new QLineEdit( rowWidget );
  keyEdit->setPlaceholderText( tr( "property" ) );
  keyEdit->setText( key );
  keyEdit->setFixedWidth( 145 );
  keyEdit->setEnabled( rulesSkillsWritable() && !required );

  QTextEdit *valueEdit = new QTextEdit( rowWidget );
  valueEdit->setAcceptRichText( false );
  valueEdit->setPlaceholderText( isList ? tr( "One value per line" ) : tr( "Value" ) );
  valueEdit->setFixedHeight( key.compare( u"description"_s, Qt::CaseInsensitive ) == 0 ? 76 : 54 );
  valueEdit->setPlainText( isList ? values.join( '\n' ) : ( values.isEmpty() ? QString() : values.first() ) );
  valueEdit->setEnabled( rulesSkillsWritable() );

  QPushButton *removeButton = new QPushButton( tr( "Remove" ), rowWidget );
  removeButton->setEnabled( rulesSkillsWritable() && !required );
  removeButton->setVisible( !required );

  rowLayout->addWidget( keyEdit );
  rowLayout->addWidget( valueEdit, 1 );
  rowLayout->addWidget( removeButton );
  mSkillPropertiesLayout->addWidget( rowWidget );

  SkillPropertyRow row;
  row.rowWidget = rowWidget;
  row.keyEdit = keyEdit;
  row.valueEdit = valueEdit;
  row.removeButton = removeButton;
  row.isList = isList;
  row.required = required;
  mSkillPropertyRows << row;

  connect( removeButton, &QPushButton::clicked, this, [this, rowWidget]() {
    for ( int i = 0; i < mSkillPropertyRows.size(); ++i )
    {
      if ( mSkillPropertyRows.at( i ).rowWidget == rowWidget )
      {
        SkillPropertyRow row = mSkillPropertyRows.takeAt( i );
        delete row.rowWidget;
        return;
      }
    }
  } );
}

QgsAiMarkdownDocument QgsAiSettingsDialog::skillDocumentFromEditor() const
{
  QgsAiMarkdownDocument document;
  document.hasFrontmatter = true;
  for ( const SkillPropertyRow &row : mSkillPropertyRows )
  {
    const QString key = row.keyEdit ? row.keyEdit->text().trimmed() : QString();
    if ( key.isEmpty() )
      continue;

    const QString rawValue = row.valueEdit ? row.valueEdit->toPlainText().trimmed() : QString();
    const QStringList lines = rawValue.split( '\n', Qt::SkipEmptyParts );
    QStringList values;
    for ( const QString &line : lines )
    {
      const QString value = line.trimmed();
      if ( !value.isEmpty() )
        values << value;
    }

    const bool requiredScalar = key.compare( u"name"_s, Qt::CaseInsensitive ) == 0 || key.compare( u"description"_s, Qt::CaseInsensitive ) == 0;
    const bool isList = !requiredScalar && ( row.isList || key.compare( u"references"_s, Qt::CaseInsensitive ) == 0 || values.size() > 1 );
    if ( !isList && !rawValue.isEmpty() )
      values = QStringList { rawValue };
    document.properties.append( { key, values, isList } );
  }
  document.body = mSkillBodyEdit ? mSkillBodyEdit->toPlainText() : QString();
  return document;
}

void QgsAiSettingsDialog::setSkillDocumentInEditor( const QgsAiMarkdownDocument &document )
{
  clearSkillPropertyRows();

  auto addPropertyByKey = [this, &document]( const QString &key, bool required, bool defaultList = false ) {
    const QStringList values = document.values( key );
    bool isList = defaultList;
    for ( const QgsAiFrontmatterProperty &property : document.properties )
    {
      if ( property.key.compare( key, Qt::CaseInsensitive ) == 0 )
      {
        isList = property.isList;
        break;
      }
    }
    addSkillPropertyRow( key, values, isList, required );
  };

  addPropertyByKey( u"name"_s, true );
  addPropertyByKey( u"description"_s, true );
  addPropertyByKey( u"references"_s, false, true );

  for ( const QgsAiFrontmatterProperty &property : document.properties )
  {
    if ( property.key.compare( u"name"_s, Qt::CaseInsensitive ) == 0
         || property.key.compare( u"description"_s, Qt::CaseInsensitive ) == 0
         || property.key.compare( u"references"_s, Qt::CaseInsensitive ) == 0 )
      continue;
    addSkillPropertyRow( property.key, property.values, property.isList, false );
  }

  if ( mSkillBodyEdit )
    mSkillBodyEdit->setPlainText( document.body );
}

void QgsAiSettingsDialog::newSkill()
{
  mSkillsListWidget->setCurrentRow( -1 );
  clearSkillEditor();
  if ( !mSkillPropertyRows.isEmpty() && mSkillPropertyRows.first().valueEdit )
    mSkillPropertyRows.first().valueEdit->setFocus();
}

void QgsAiSettingsDialog::duplicateSelectedSkill()
{
  if ( mCurrentSkillSlug.isEmpty() || !rulesSkillsWritable() )
    return;

  QStringList existingSlugs;
  for ( int i = 0; i < mSkillsListWidget->count(); ++i )
    existingSlugs << mSkillsListWidget->item( i )->data( Qt::UserRole ).value<QgsAiSkillInfo>().slug;

  QgsAiMarkdownDocument document = skillDocumentFromEditor();
  const QString name = document.value( u"name"_s, mCurrentSkillSlug ).trimmed();
  const QString copiedName = name + tr( " (copy)" );
  bool updatedName = false;
  for ( QgsAiFrontmatterProperty &property : document.properties )
  {
    if ( property.key.compare( u"name"_s, Qt::CaseInsensitive ) == 0 )
    {
      property.values = QStringList { copiedName };
      property.isList = false;
      updatedName = true;
      break;
    }
  }
  if ( !updatedName )
    document.properties.prepend( { u"name"_s, QStringList { copiedName }, false } );

  const QString baseSlug = QgsAiRulesSkillsStore::slugify( copiedName );
  QString candidate = baseSlug;
  int suffix = 2;
  while ( existingSlugs.contains( candidate ) )
    candidate = u"%1-%2"_s.arg( baseSlug ).arg( suffix++ );

  QString error;
  if ( !rulesSkillsStore().writeSkillMarkdown( mSkillsRelativeDirForList, candidate, QgsAiRulesSkillsStore::serializeMarkdownDocument( document ), &error ) )
  {
    QMessageBox::warning( this, tr( "Could not duplicate skill" ), error );
    return;
  }
  mCurrentSkillSlug = candidate;
  refreshSkillsList();
}

void QgsAiSettingsDialog::deleteSelectedSkill()
{
  if ( mCurrentSkillSlug.isEmpty() )
    return;
  const QString name = skillDocumentFromEditor().value( u"name"_s, mCurrentSkillSlug );
  if ( QMessageBox::question( this, tr( "Delete skill" ), tr( "Delete skill \u201c%1\u201d? This cannot be undone." ).arg( name ) ) != QMessageBox::Yes )
    return;

  QgsAiSkillInfo info;
  info.slug = mCurrentSkillSlug;
  const QList<QgsAiSkillInfo> skills = rulesSkillsStore().listSkills( mSkillsRelativeDirForList );
  for ( const QgsAiSkillInfo &skill : skills )
  {
    if ( skill.slug == mCurrentSkillSlug )
    {
      info = skill;
      break;
    }
  }
  QString error;
  if ( !rulesSkillsStore().deleteSkill( info, &error ) )
  {
    QMessageBox::warning( this, tr( "Could not delete skill" ), error );
    return;
  }
  mCurrentSkillSlug.clear();
  refreshSkillsList();
}

void QgsAiSettingsDialog::saveCurrentSkill()
{
  if ( !rulesSkillsWritable() )
  {
    QMessageBox::warning( this, tr( "Workspace not trusted" ), tr( "Trust this workspace (Workspace section) before creating or editing skills." ) );
    return;
  }
  const QgsAiMarkdownDocument document = skillDocumentFromEditor();
  const QString name = document.value( u"name"_s ).trimmed();
  if ( name.isEmpty() )
  {
    QMessageBox::warning( this, tr( "Name required" ), tr( "Give the skill a name before saving." ) );
    return;
  }
  const QString description = document.value( u"description"_s ).trimmed();
  if ( description.isEmpty() )
  {
    QMessageBox::warning( this, tr( "Description required" ), tr( "Describe when the agent should use this skill before saving." ) );
    return;
  }

  QString slug = mCurrentSkillSlug;
  if ( slug.isEmpty() )
  {
    QStringList existingSlugs;
    for ( int i = 0; i < mSkillsListWidget->count(); ++i )
      existingSlugs << mSkillsListWidget->item( i )->data( Qt::UserRole ).value<QgsAiSkillInfo>().slug;

    const QString baseSlug = QgsAiRulesSkillsStore::slugify( name );
    slug = baseSlug;
    int suffix = 2;
    while ( existingSlugs.contains( slug ) )
      slug = u"%1-%2"_s.arg( baseSlug ).arg( suffix++ );
  }

  QString error;
  if ( !rulesSkillsStore().writeSkillMarkdown( mSkillsRelativeDirForList, slug, QgsAiRulesSkillsStore::serializeMarkdownDocument( document ), &error ) )
  {
    QMessageBox::warning( this, tr( "Could not save skill" ), error );
    return;
  }
  mCurrentSkillSlug = slug;
  refreshSkillsList();
}

void QgsAiSettingsDialog::syncRulesSkillsToCloud()
{
  if ( !mSessionManager || !mModelRouter || !mAccountWidget )
    return;

  const QString token = mModelRouter->planSessionToken().trimmed();
  if ( token.isEmpty() )
  {
    QMessageBox::information( this, tr( "Strata Cloud sync" ), tr( "Sign in to Plan Account before syncing rules and skills." ) );
    return;
  }
  const QString workspaceRoot = mSessionManager->workspaceRoot();
  if ( workspaceRoot.trimmed().isEmpty() )
  {
    QMessageBox::warning( this, tr( "Strata Cloud sync" ), tr( "Workspace root is unset." ) );
    return;
  }
  if ( !QgsAiWorkspaceTrust::isTrusted( workspaceRoot ) )
  {
    QMessageBox::warning( this, tr( "Strata Cloud push" ), tr( "Trust this workspace before writing its Rules & Skills to Strata Cloud." ) );
    return;
  }

  const QgsAiRulesSkillsStore store = rulesSkillsStore();
  const QList<QgsAiRuleInfo> rules = store.listRules( mRulesRelativeDirForList );
  const QList<QgsAiSkillInfo> skills = store.listSkills( mSkillsRelativeDirForList );
  if ( rules.isEmpty() && skills.isEmpty() )
  {
    QMessageBox::information( this, tr( "Strata Cloud sync" ), tr( "No local rules or skills to sync yet." ) );
    return;
  }

  auto localRules = std::make_shared<QList<QgsAiRulesSkillsCloudClient::RemoteRule>>();
  localRules->reserve( rules.size() );
  for ( const QgsAiRuleInfo &rule : rules )
    *localRules << QgsAiRulesSkillsCloudClient::toRemoteRule( rule, store.readRuleMarkdown( rule ) );

  auto localSkills = std::make_shared<QList<QgsAiRulesSkillsCloudClient::RemoteSkill>>();
  localSkills->reserve( skills.size() );
  for ( const QgsAiSkillInfo &skill : skills )
    *localSkills << QgsAiRulesSkillsCloudClient::toRemoteSkill( skill, store.readSkillMarkdown( skill ) );

  const int totalExpected = localRules->size() + localSkills->size();
  auto progress = std::make_shared<int>( 0 );
  auto failures = std::make_shared<int>( 0 );
  auto requestsStarted = std::make_shared<bool>( false );
  auto cancelled = std::make_shared<bool>( false ); //#spellok
  auto rulesFetched = std::make_shared<bool>( false );
  auto skillsFetched = std::make_shared<bool>( false );
  auto cloudRules = std::make_shared<QList<QgsAiRulesSkillsCloudClient::RemoteRule>>();
  auto cloudSkills = std::make_shared<QList<QgsAiRulesSkillsCloudClient::RemoteSkill>>();

  mSyncRulesSkillsCloudButton->setEnabled( false );
  mImportRulesSkillsCloudButton->setEnabled( false );
  mRulesSkillsCloudStatusLabel->setText( tr( "Resolving cloud records before pushing %1 rule(s) and %2 skill(s)…" ).arg( localRules->size() ).arg( localSkills->size() ) );

  QgsAiRulesSkillsCloudClient *client = new QgsAiRulesSkillsCloudClient( this );

  auto maybeFinish = [this, client, progress, failures, totalExpected, cancelled]() { //#spellok
    if ( *progress < totalExpected )
      return;
    *cancelled = true; //#spellok
    mSyncRulesSkillsCloudButton->setEnabled( true );
    mImportRulesSkillsCloudButton->setEnabled( true );
    mRulesSkillsCloudStatusLabel->setText(
      *failures == 0 ? tr( "Pushed %1 item(s) to Strata Cloud." ).arg( totalExpected ) : tr( "Push completed with %1 error(s); see the warning dialog." ).arg( *failures )
    );
    client->deleteLater();
  };

  auto maybeStartPush = std::make_shared<std::function<void()>>();
  *maybeStartPush =
    [client, localRules, localSkills, cloudRules, cloudSkills, rulesFetched, skillsFetched, requestsStarted, cancelled, apiBase = mAccountWidget->planEndpoint(), token, maybeFinish, this]() { //#spellok
      if ( *cancelled || !*rulesFetched || !*skillsFetched ) //#spellok
        return;
      for ( QgsAiRulesSkillsCloudClient::RemoteRule &local : *localRules )
      {
        const auto match = std::find_if( cloudRules->cbegin(), cloudRules->cend(), [&local]( const auto &remote ) { return remote.slug == local.slug; } );
        if ( match != cloudRules->cend() )
          local.id = match->id;
      }
      for ( QgsAiRulesSkillsCloudClient::RemoteSkill &local : *localSkills )
      {
        const auto match = std::find_if( cloudSkills->cbegin(), cloudSkills->cend(), [&local]( const auto &remote ) { return remote.slug == local.slug; } );
        if ( match != cloudSkills->cend() )
          local.id = match->id;
      }
      *requestsStarted = true;
      mRulesSkillsCloudStatusLabel->setText( tr( "Pushing local copies; cloud-only items will remain untouched…" ) );
      for ( const auto &rule : *localRules )
        client->pushRule( apiBase, token, client->property( "workspaceId" ).toString(), rule );
      for ( const auto &skill : *localSkills )
        client->pushSkill( apiBase, token, client->property( "workspaceId" ).toString(), skill );
      if ( localRules->isEmpty() && localSkills->isEmpty() )
        maybeFinish();
    };

  connect( client, &QgsAiRulesSkillsCloudClient::ruleSynced, this, [progress, maybeFinish]( const QgsAiRulesSkillsCloudClient::RemoteRule & ) {
    ++( *progress );
    maybeFinish();
  } );
  connect( client, &QgsAiRulesSkillsCloudClient::skillSynced, this, [progress, maybeFinish]( const QgsAiRulesSkillsCloudClient::RemoteSkill & ) {
    ++( *progress );
    maybeFinish();
  } );
  connect( client, &QgsAiRulesSkillsCloudClient::requestFailed, this, [this, client, progress, failures, requestsStarted, cancelled, totalExpected, maybeFinish]( const QString &message ) { //#spellok
    if ( *cancelled )                                                                                                                                                                        //#spellok
      return;
    if ( !*requestsStarted )
    {
      *cancelled = true; //#spellok
      mSyncRulesSkillsCloudButton->setEnabled( true );
      mImportRulesSkillsCloudButton->setEnabled( true );
      mRulesSkillsCloudStatusLabel->setText( tr( "Strata Cloud push failed before any item was written." ) );
      QMessageBox::warning( this, tr( "Strata Cloud push failed" ), message );
      client->deleteLater();
      return;
    }
    ++( *progress );
    ++( *failures );
    if ( *progress == totalExpected )
      QMessageBox::warning( this, tr( "Strata Cloud push" ), tr( "Some items failed to push. Last error: %1" ).arg( message ) );
    maybeFinish();
  } );
  connect( client, &QgsAiRulesSkillsCloudClient::workspaceReady, this, [client, apiBase = mAccountWidget->planEndpoint(), token]( const QString &workspaceId ) {
    client->setProperty( "workspaceId", workspaceId );
    client->fetchRules( apiBase, token, workspaceId );
    client->fetchSkills( apiBase, token, workspaceId );
  } );
  connect( client, &QgsAiRulesSkillsCloudClient::rulesFetched, this, [cloudRules, rulesFetched, maybeStartPush]( const auto &items ) {
    *cloudRules = items;
    *rulesFetched = true;
    ( *maybeStartPush )();
  } );
  connect( client, &QgsAiRulesSkillsCloudClient::skillsFetched, this, [cloudSkills, skillsFetched, maybeStartPush]( const auto &items ) {
    *cloudSkills = items;
    *skillsFetched = true;
    ( *maybeStartPush )();
  } );

  client->ensureWorkspace( mAccountWidget->planEndpoint(), token, workspaceRoot, QFileInfo( workspaceRoot ).fileName() );
}

void QgsAiSettingsDialog::importRulesSkillsFromCloud()
{
  if ( !mSessionManager || !mModelRouter || !mAccountWidget )
    return;
  const QString token = mModelRouter->planSessionToken().trimmed();
  const QString workspaceRoot = mSessionManager->workspaceRoot();
  if ( token.isEmpty() || workspaceRoot.trimmed().isEmpty() )
  {
    QMessageBox::information( this, tr( "Strata Cloud import" ), tr( "Sign in to Plan Account and configure a workspace before importing." ) );
    return;
  }
  if ( !QgsAiWorkspaceTrust::isTrusted( workspaceRoot ) )
  {
    QMessageBox::warning( this, tr( "Strata Cloud import" ), tr( "Trust this workspace before importing files from Strata Cloud." ) );
    return;
  }

  const QgsAiRulesSkillsStore store = rulesSkillsStore();
  QHash<QString, QString> localRuleMarkdown;
  for ( const QgsAiRuleInfo &rule : store.listRules( mRulesRelativeDirForList ) )
    localRuleMarkdown.insert( rule.slug, store.readRuleMarkdown( rule ) );
  QHash<QString, QString> localSkillMarkdown;
  for ( const QgsAiSkillInfo &skill : store.listSkills( mSkillsRelativeDirForList ) )
    localSkillMarkdown.insert( skill.slug, store.readSkillMarkdown( skill ) );

  auto cloudRules = std::make_shared<QList<QgsAiRulesSkillsCloudClient::RemoteRule>>();
  auto cloudSkills = std::make_shared<QList<QgsAiRulesSkillsCloudClient::RemoteSkill>>();
  auto rulesFetched = std::make_shared<bool>( false );
  auto skillsFetched = std::make_shared<bool>( false );
  auto finished = std::make_shared<bool>( false );
  QgsAiRulesSkillsCloudClient *client = new QgsAiRulesSkillsCloudClient( this );
  mSyncRulesSkillsCloudButton->setEnabled( false );
  mImportRulesSkillsCloudButton->setEnabled( false );
  mRulesSkillsCloudStatusLabel->setText( tr( "Downloading Rules & Skills for import preview…" ) );

  auto showPreview = std::make_shared<std::function<void()>>();
  *showPreview = [this, client, cloudRules, cloudSkills, rulesFetched, skillsFetched, finished, localRuleMarkdown, localSkillMarkdown, workspaceRoot]() {
    if ( *finished || !*rulesFetched || !*skillsFetched )
      return;
    *finished = true;

    QDialog preview( this );
    preview.setWindowTitle( tr( "Import from Strata Cloud" ) );
    preview.resize( 760, 430 );
    QVBoxLayout *layout = new QVBoxLayout( &preview );
    QLabel *intro = new QLabel( tr( "Review every cloud item. Conflicts keep the local file unless you explicitly choose Replace local; local-only files are never deleted." ), &preview );
    intro->setWordWrap( true );
    layout->addWidget( intro );
    QTableWidget *table = new QTableWidget( cloudRules->size() + cloudSkills->size(), 4, &preview );
    table->setHorizontalHeaderLabels( { tr( "Type" ), tr( "Slug" ), tr( "State" ), tr( "Action" ) } );
    table->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );
    table->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::Stretch );
    table->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::ResizeToContents );
    table->horizontalHeader()->setSectionResizeMode( 3, QHeaderView::ResizeToContents );
    table->verticalHeader()->setVisible( false );
    table->setSelectionMode( QAbstractItemView::NoSelection );
    layout->addWidget( table, 1 );

    struct PreviewRow
    {
        bool skill = false;
        int remoteIndex = -1;
        QComboBox *action = nullptr;
    };
    QList<PreviewRow> rows;
    auto addRow = [table, &rows]( int row, bool skill, int remoteIndex, const QString &slug, QgsAiRulesSkillsCloudClient::RemoteComparison comparison ) {
      auto fixedItem = []( const QString &value ) {
        QTableWidgetItem *item = new QTableWidgetItem( value );
        item->setFlags( item->flags() & ~Qt::ItemIsEditable );
        return item;
      };
      table->setItem( row, 0, fixedItem( skill ? tr( "Skill" ) : tr( "Rule" ) ) );
      table->setItem( row, 1, fixedItem( slug ) );
      const QString state = comparison == QgsAiRulesSkillsCloudClient::RemoteComparison::RemoteOnly   ? tr( "Remote only" )
                            : comparison == QgsAiRulesSkillsCloudClient::RemoteComparison::Equivalent ? tr( "Equivalent" )
                                                                                                      : tr( "Conflict" );
      table->setItem( row, 2, fixedItem( state ) );
      QComboBox *action = new QComboBox( table );
      if ( comparison == QgsAiRulesSkillsCloudClient::RemoteComparison::RemoteOnly )
      {
        action->addItem( tr( "Import" ), u"import"_s );
        action->addItem( tr( "Skip" ), u"skip"_s );
      }
      else if ( comparison == QgsAiRulesSkillsCloudClient::RemoteComparison::Equivalent )
      {
        action->addItem( tr( "Skip" ), u"skip"_s );
        action->setEnabled( false );
      }
      else
      {
        action->addItem( tr( "Keep local" ), u"keep"_s );
        action->addItem( tr( "Replace local" ), u"replace"_s );
      }
      table->setCellWidget( row, 3, action );
      rows << PreviewRow { skill, remoteIndex, action };
    };

    int row = 0;
    for ( int i = 0; i < cloudRules->size(); ++i, ++row )
    {
      const auto &remote = cloudRules->at( i );
      const QString markdown = QgsAiRulesSkillsCloudClient::markdownForRemoteRule( remote );
      const bool exists = localRuleMarkdown.contains( remote.slug );
      addRow( row, false, i, remote.slug, QgsAiRulesSkillsCloudClient::classifyRemote( exists, localRuleMarkdown.value( remote.slug ), markdown ) );
    }
    for ( int i = 0; i < cloudSkills->size(); ++i, ++row )
    {
      const auto &remote = cloudSkills->at( i );
      const QString markdown = QgsAiRulesSkillsCloudClient::markdownForRemoteSkill( remote );
      const bool exists = localSkillMarkdown.contains( remote.slug );
      addRow( row, true, i, remote.slug, QgsAiRulesSkillsCloudClient::classifyRemote( exists, localSkillMarkdown.value( remote.slug ), markdown ) );
    }

    QDialogButtonBox *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &preview );
    buttons->button( QDialogButtonBox::Ok )->setText( tr( "Apply selected" ) );
    connect( buttons, &QDialogButtonBox::accepted, &preview, &QDialog::accept );
    connect( buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject );
    layout->addWidget( buttons );

    int imported = 0;
    QStringList errors;
    if ( preview.exec() == QDialog::Accepted )
    {
      if ( !QgsAiWorkspaceTrust::isTrusted( workspaceRoot ) )
        errors << tr( "Workspace trust was revoked before applying the import." );
      else
      {
        const QgsAiRulesSkillsStore destination = rulesSkillsStore();
        for ( const PreviewRow &previewRow : rows )
        {
          const QString action = previewRow.action->currentData().toString();
          if ( action != "import"_L1 && action != "replace"_L1 )
            continue;
          QString error;
          bool ok = false;
          if ( previewRow.skill )
          {
            const auto &remote = cloudSkills->at( previewRow.remoteIndex );
            ok = destination.writeSkillMarkdown( mSkillsRelativeDirForList, remote.slug, QgsAiRulesSkillsCloudClient::markdownForRemoteSkill( remote ), &error );
          }
          else
          {
            const auto &remote = cloudRules->at( previewRow.remoteIndex );
            ok = destination.writeRuleMarkdown( mRulesRelativeDirForList, remote.slug, QgsAiRulesSkillsCloudClient::markdownForRemoteRule( remote ), &error );
          }
          if ( ok )
            ++imported;
          else
            errors << error;
        }
      }
      refreshRulesList();
      refreshSkillsList();
    }
    mSyncRulesSkillsCloudButton->setEnabled( true );
    mImportRulesSkillsCloudButton->setEnabled( true );
    if ( errors.isEmpty() )
      mRulesSkillsCloudStatusLabel->setText(
        preview.result() == QDialog::Accepted ? tr( "Imported %1 item(s); skipped items and local-only files were left untouched." ).arg( imported )
                                              : tr( "Cloud import cancelled; no local files were changed." ) //#spellok
      );
    else
      QMessageBox::warning( this, tr( "Strata Cloud import" ), errors.join( '\n'_L1 ) );
    client->deleteLater();
  };

  connect( client, &QgsAiRulesSkillsCloudClient::workspaceReady, this, [client, apiBase = mAccountWidget->planEndpoint(), token]( const QString &workspaceId ) {
    client->fetchRules( apiBase, token, workspaceId );
    client->fetchSkills( apiBase, token, workspaceId );
  } );
  connect( client, &QgsAiRulesSkillsCloudClient::rulesFetched, this, [cloudRules, rulesFetched, showPreview]( const auto &items ) {
    *cloudRules = items;
    *rulesFetched = true;
    ( *showPreview )();
  } );
  connect( client, &QgsAiRulesSkillsCloudClient::skillsFetched, this, [cloudSkills, skillsFetched, showPreview]( const auto &items ) {
    *cloudSkills = items;
    *skillsFetched = true;
    ( *showPreview )();
  } );
  connect( client, &QgsAiRulesSkillsCloudClient::requestFailed, this, [this, client, finished]( const QString &message ) {
    if ( *finished )
      return;
    *finished = true;
    mSyncRulesSkillsCloudButton->setEnabled( true );
    mImportRulesSkillsCloudButton->setEnabled( true );
    mRulesSkillsCloudStatusLabel->setText( tr( "Strata Cloud import failed; no local files were changed." ) );
    QMessageBox::warning( this, tr( "Strata Cloud import failed" ), message );
    client->deleteLater();
  } );
  client->ensureWorkspace( mAccountWidget->planEndpoint(), token, workspaceRoot, QFileInfo( workspaceRoot ).fileName() );
}

QWidget *QgsAiSettingsDialog::buildGalleryConnectorsPage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Gallery & Connectors" ), tr( "Install curated plugin packs into this workspace and enable Plan MCP connectors." ), contentLayout );

  contentLayout->addWidget( sectionHeader( tr( "Plugin packs" ), page ) );
  mGalleryPacksList = new QListWidget( page );
  mGalleryPacksList->setMinimumHeight( 140 );
  contentLayout->addWidget( mGalleryPacksList );
  mImportGalleryPackButton = new QPushButton( tr( "Install selected pack" ), page );
  connect( mImportGalleryPackButton, &QPushButton::clicked, this, &QgsAiSettingsDialog::importSelectedGalleryPack );
  contentLayout->addWidget( mImportGalleryPackButton );
  mGalleryStatusLabel = new QLabel( page );
  mGalleryStatusLabel->setWordWrap( true );
  mGalleryStatusLabel->setProperty( "aiRole", u"rowDescription"_s );
  contentLayout->addWidget( mGalleryStatusLabel );

  contentLayout->addWidget( sectionHeader( tr( "MCP connectors" ), page ) );
  mMcpConnectorsList = new QListWidget( page );
  mMcpConnectorsList->setMinimumHeight( 120 );
  connect( mMcpConnectorsList, &QListWidget::itemChanged, this, [this]( QListWidgetItem *item ) {
    if ( !item || !mAccountWidget || !mModelRouter )
      return;
    const QString token = mModelRouter->planSessionToken().trimmed();
    const QString apiBase = QgsAiPlanClient::apiBaseForChatEndpoint( mAccountWidget->planEndpoint() );
    if ( token.isEmpty() || apiBase.isEmpty() )
      return;
    const QString slug = item->data( Qt::UserRole ).toString();
    const bool enabled = item->checkState() == Qt::Checked;
    QgsAiGalleryCloudClient *client = new QgsAiGalleryCloudClient( this );
    connect( client, &QgsAiGalleryCloudClient::mcpServerUpdated, this, [this, client]( const QgsAiGalleryCloudClient::McpServer & ) {
      QgsAiPlanClient *policyClient = new QgsAiPlanClient( this );
      connect( policyClient, &QgsAiPlanClient::agentPolicyReady, this, [this, policyClient]( const QgsAiManagedAgentPolicy &policy, bool ) {
        if ( mSessionManager )
          mSessionManager->setManagedAgentPolicy( policy );
        policyClient->deleteLater();
      } );
      policyClient->refreshAgentPolicy( mAccountWidget->planEndpoint(), mModelRouter->planSessionToken() );
      client->deleteLater();
    } );
    connect( client, &QgsAiGalleryCloudClient::requestFailed, this, [this, client]( const QString &message ) {
      mConnectorsStatusLabel->setText( message );
      client->deleteLater();
    } );
    client->setMcpServerEnabled( apiBase, token, slug, enabled );
  } );
  contentLayout->addWidget( mMcpConnectorsList );
  mConnectorsStatusLabel = new QLabel( page );
  mConnectorsStatusLabel->setWordWrap( true );
  mConnectorsStatusLabel->setProperty( "aiRole", u"rowDescription"_s );
  contentLayout->addWidget( mConnectorsStatusLabel );
  return page;
}

void QgsAiSettingsDialog::refreshGalleryAndConnectors()
{
  if ( !mGalleryPacksList || !mMcpConnectorsList || !mAccountWidget )
    return;

  const QString apiBase = QgsAiPlanClient::apiBaseForChatEndpoint( mAccountWidget->planEndpoint() );
  const QString token = mModelRouter ? mModelRouter->planSessionToken().trimmed() : QString();
  if ( apiBase.isEmpty() )
  {
    mGalleryStatusLabel->setText( tr( "Configure the Plan endpoint to load the gallery." ) );
    return;
  }

  QgsAiGalleryCloudClient *client = new QgsAiGalleryCloudClient( this );
  connect( client, &QgsAiGalleryCloudClient::packsFetched, this, [this, client]( const QList<QgsAiGalleryCloudClient::PackSummary> &packs ) {
    mGalleryPacksList->clear();
    for ( const QgsAiGalleryCloudClient::PackSummary &pack : packs )
    {
      QListWidgetItem *item = new QListWidgetItem( pack.name, mGalleryPacksList );
      item->setToolTip( pack.description );
      item->setData( Qt::UserRole, pack.slug );
    }
    mGalleryStatusLabel->setText( packs.isEmpty() ? tr( "No curated packs are published yet." ) : tr( "%n pack(s) available.", nullptr, packs.size() ) );
    client->deleteLater();
  } );
  connect( client, &QgsAiGalleryCloudClient::requestFailed, this, [this, client]( const QString &message ) {
    mGalleryStatusLabel->setText( message );
    client->deleteLater();
  } );
  client->fetchPacks( apiBase, token );

  if ( token.isEmpty() )
  {
    mMcpConnectorsList->clear();
    mConnectorsStatusLabel->setText( tr( "Sign in to Plan Account to enable MCP connectors." ) );
    return;
  }

  QgsAiGalleryCloudClient *mcpClient = new QgsAiGalleryCloudClient( this );
  connect( mcpClient, &QgsAiGalleryCloudClient::mcpServersFetched, this, [this, mcpClient]( const QList<QgsAiGalleryCloudClient::McpServer> &servers ) {
    mMcpConnectorsList->blockSignals( true );
    mMcpConnectorsList->clear();
    for ( const QgsAiGalleryCloudClient::McpServer &server : servers )
    {
      QListWidgetItem *item = new QListWidgetItem( server.name, mMcpConnectorsList );
      item->setFlags( item->flags() | Qt::ItemIsUserCheckable );
      item->setCheckState( server.enabled ? Qt::Checked : Qt::Unchecked );
      item->setToolTip( server.description );
      item->setData( Qt::UserRole, server.slug );
    }
    mMcpConnectorsList->blockSignals( false );
    mConnectorsStatusLabel->setText( servers.isEmpty() ? tr( "No curated MCP connectors are available." ) : tr( "Mutating connectors still require local approval in Ask before edits." ) );
    mcpClient->deleteLater();
  } );
  connect( mcpClient, &QgsAiGalleryCloudClient::requestFailed, this, [this, mcpClient]( const QString &message ) {
    mConnectorsStatusLabel->setText( message );
    mcpClient->deleteLater();
  } );
  mcpClient->fetchMcpServers( apiBase, token );
}

void QgsAiSettingsDialog::importSelectedGalleryPack()
{
  if ( !mGalleryPacksList || !mSessionManager || !mAccountWidget )
    return;
  QListWidgetItem *selected = mGalleryPacksList->currentItem();
  if ( !selected )
  {
    QMessageBox::information( this, tr( "Install pack" ), tr( "Select a pack first." ) );
    return;
  }
  const QString workspaceRoot = mSessionManager->workspaceRoot();
  if ( workspaceRoot.trimmed().isEmpty() || !QgsAiWorkspaceTrust::isTrusted( workspaceRoot ) )
  {
    QMessageBox::warning( this, tr( "Install pack" ), tr( "Trust this workspace before installing gallery files." ) );
    return;
  }

  const QString apiBase = QgsAiPlanClient::apiBaseForChatEndpoint( mAccountWidget->planEndpoint() );
  const QString token = mModelRouter ? mModelRouter->planSessionToken().trimmed() : QString();
  if ( apiBase.isEmpty() )
    return;

  mImportGalleryPackButton->setEnabled( false );
  mGalleryStatusLabel->setText( tr( "Downloading pack…" ) );
  QgsAiGalleryCloudClient *client = new QgsAiGalleryCloudClient( this );
  connect( client, &QgsAiGalleryCloudClient::packImportReady, this, [this, client]( const QgsAiGalleryCloudClient::PackImport &packImport ) {
    QHash<QString, QString> localRuleMarkdown;
    for ( const QgsAiRuleInfo &rule : rulesSkillsStore().listRules( mRulesRelativeDirForList ) )
      localRuleMarkdown.insert( rule.slug, rulesSkillsStore().readRuleMarkdown( rule ) );
    QHash<QString, QString> localSkillMarkdown;
    for ( const QgsAiSkillInfo &skill : rulesSkillsStore().listSkills( mSkillsRelativeDirForList ) )
      localSkillMarkdown.insert( skill.slug, rulesSkillsStore().readSkillMarkdown( skill ) );

    QDialog preview( this );
    preview.setWindowTitle( tr( "Install gallery pack" ) );
    preview.resize( 760, 430 );
    QVBoxLayout *layout = new QVBoxLayout( &preview );
    QLabel *intro = new QLabel( tr( "Review pack children before writing them into this workspace. Conflicts keep the local file unless you choose Replace local." ), &preview );
    intro->setWordWrap( true );
    layout->addWidget( intro );
    QTableWidget *table = new QTableWidget( packImport.skills.size() + packImport.rules.size(), 4, &preview );
    table->setHorizontalHeaderLabels( { tr( "Type" ), tr( "Slug" ), tr( "State" ), tr( "Action" ) } );
    table->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );
    table->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::Stretch );
    table->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::ResizeToContents );
    table->horizontalHeader()->setSectionResizeMode( 3, QHeaderView::ResizeToContents );
    table->verticalHeader()->setVisible( false );
    layout->addWidget( table, 1 );

    struct PreviewRow
    {
        bool skill = false;
        int remoteIndex = -1;
        QComboBox *action = nullptr;
    };
    QList<PreviewRow> rows;
    auto addRow = [table, &rows]( int row, bool skill, int remoteIndex, const QString &slug, QgsAiRulesSkillsCloudClient::RemoteComparison comparison ) {
      auto fixedItem = []( const QString &value ) {
        QTableWidgetItem *item = new QTableWidgetItem( value );
        item->setFlags( item->flags() & ~Qt::ItemIsEditable );
        return item;
      };
      table->setItem( row, 0, fixedItem( skill ? tr( "Skill" ) : tr( "Rule" ) ) );
      table->setItem( row, 1, fixedItem( slug ) );
      const QString state = comparison == QgsAiRulesSkillsCloudClient::RemoteComparison::RemoteOnly   ? tr( "Remote only" )
                            : comparison == QgsAiRulesSkillsCloudClient::RemoteComparison::Equivalent ? tr( "Equivalent" )
                                                                                                      : tr( "Conflict" );
      table->setItem( row, 2, fixedItem( state ) );
      QComboBox *action = new QComboBox( table );
      if ( comparison == QgsAiRulesSkillsCloudClient::RemoteComparison::RemoteOnly )
      {
        action->addItem( tr( "Import" ), u"import"_s );
        action->addItem( tr( "Skip" ), u"skip"_s );
      }
      else if ( comparison == QgsAiRulesSkillsCloudClient::RemoteComparison::Equivalent )
      {
        action->addItem( tr( "Skip" ), u"skip"_s );
        action->setEnabled( false );
      }
      else
      {
        action->addItem( tr( "Keep local" ), u"keep"_s );
        action->addItem( tr( "Replace local" ), u"replace"_s );
      }
      table->setCellWidget( row, 3, action );
      rows << PreviewRow { skill, remoteIndex, action };
    };

    int row = 0;
    for ( int i = 0; i < packImport.rules.size(); ++i, ++row )
    {
      QgsAiRulesSkillsCloudClient::RemoteRule remote;
      remote.slug = packImport.rules.at( i ).slug;
      remote.name = packImport.rules.at( i ).name;
      remote.description = packImport.rules.at( i ).description;
      remote.content = packImport.rules.at( i ).content;
      const QString markdown = QgsAiRulesSkillsCloudClient::markdownForRemoteRule( remote );
      addRow( row, false, i, remote.slug, QgsAiRulesSkillsCloudClient::classifyRemote( localRuleMarkdown.contains( remote.slug ), localRuleMarkdown.value( remote.slug ), markdown ) );
    }
    for ( int i = 0; i < packImport.skills.size(); ++i, ++row )
    {
      QgsAiRulesSkillsCloudClient::RemoteSkill remote;
      remote.slug = packImport.skills.at( i ).slug;
      remote.name = packImport.skills.at( i ).name;
      remote.description = packImport.skills.at( i ).description;
      remote.content = packImport.skills.at( i ).content;
      const QString markdown = QgsAiRulesSkillsCloudClient::markdownForRemoteSkill( remote );
      addRow( row, true, i, remote.slug, QgsAiRulesSkillsCloudClient::classifyRemote( localSkillMarkdown.contains( remote.slug ), localSkillMarkdown.value( remote.slug ), markdown ) );
    }

    QDialogButtonBox *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &preview );
    buttons->button( QDialogButtonBox::Ok )->setText( tr( "Apply selected" ) );
    connect( buttons, &QDialogButtonBox::accepted, &preview, &QDialog::accept );
    connect( buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject );
    layout->addWidget( buttons );

    int imported = 0;
    QStringList errors;
    if ( preview.exec() == QDialog::Accepted )
    {
      const QgsAiRulesSkillsStore destination = rulesSkillsStore();
      for ( const PreviewRow &previewRow : rows )
      {
        const QString action = previewRow.action->currentData().toString();
        if ( action != "import"_L1 && action != "replace"_L1 )
          continue;
        QString error;
        bool ok = false;
        if ( previewRow.skill )
        {
          QgsAiRulesSkillsCloudClient::RemoteSkill remote;
          remote.slug = packImport.skills.at( previewRow.remoteIndex ).slug;
          remote.name = packImport.skills.at( previewRow.remoteIndex ).name;
          remote.description = packImport.skills.at( previewRow.remoteIndex ).description;
          remote.content = packImport.skills.at( previewRow.remoteIndex ).content;
          ok = destination.writeSkillMarkdown( mSkillsRelativeDirForList, remote.slug, QgsAiRulesSkillsCloudClient::markdownForRemoteSkill( remote ), &error );
        }
        else
        {
          QgsAiRulesSkillsCloudClient::RemoteRule remote;
          remote.slug = packImport.rules.at( previewRow.remoteIndex ).slug;
          remote.name = packImport.rules.at( previewRow.remoteIndex ).name;
          remote.description = packImport.rules.at( previewRow.remoteIndex ).description;
          remote.content = packImport.rules.at( previewRow.remoteIndex ).content;
          ok = destination.writeRuleMarkdown( mRulesRelativeDirForList, remote.slug, QgsAiRulesSkillsCloudClient::markdownForRemoteRule( remote ), &error );
        }
        if ( ok )
          ++imported;
        else if ( !error.isEmpty() )
          errors << error;
      }

      const QString token = mModelRouter ? mModelRouter->planSessionToken().trimmed() : QString();
      const QString apiBase = QgsAiPlanClient::apiBaseForChatEndpoint( mAccountWidget->planEndpoint() );
      if ( !token.isEmpty() && !apiBase.isEmpty() )
      {
        for ( const QgsAiGalleryCloudClient::Connector &connector : packImport.pack.connectors )
        {
          QgsAiGalleryCloudClient *toggle = new QgsAiGalleryCloudClient( this );
          toggle->setMcpServerEnabled( apiBase, token, connector.slug, connector.enabled );
          connect( toggle, &QgsAiGalleryCloudClient::mcpServerUpdated, toggle, &QObject::deleteLater );
          connect( toggle, &QgsAiGalleryCloudClient::requestFailed, toggle, &QObject::deleteLater );
        }
      }
    }

    refreshRulesList();
    refreshSkillsList();
    refreshGalleryAndConnectors();
    mImportGalleryPackButton->setEnabled( true );
    mGalleryStatusLabel->setText( errors.isEmpty() ? tr( "Installed %n item(s) from the pack.", nullptr, imported ) : errors.join( '\n'_L1 ) );
    client->deleteLater();
  } );
  connect( client, &QgsAiGalleryCloudClient::requestFailed, this, [this, client]( const QString &message ) {
    mImportGalleryPackButton->setEnabled( true );
    mGalleryStatusLabel->setText( message );
    client->deleteLater();
  } );
  client->fetchPackForImport( apiBase, token, selected->data( Qt::UserRole ).toString() );
}

QWidget *QgsAiSettingsDialog::buildIndexingPage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Indexing & Docs" ), tr( "Local retrieval index (RAG) over workspace files and layers." ), contentLayout );

  QgsSettings indexSettings;
  const bool canUseEmbeddings = mSessionManager && mSessionManager->workspaceIndex() && mSessionManager->workspaceIndex()->embeddingProviderAvailable();

  mEmbeddingProvider = new QComboBox( page );
  mEmbeddingProvider->setObjectName( u"aiEmbeddingProviderComboBox"_s );
  for ( const QgsAiEmbeddingProviderUiEntry &entry : QgsAiEmbeddingProviderRegistry::providerUiEntries() )
  {
    mEmbeddingProvider->addItem( entry.displayName, entry.providerId );
    const int row = mEmbeddingProvider->count() - 1;
    if ( !entry.unavailableReason.isEmpty() )
      mEmbeddingProvider->setItemData( row, entry.unavailableReason, Qt::ToolTipRole );
    if ( !entry.selectable )
    {
      if ( QStandardItemModel *itemModel = qobject_cast<QStandardItemModel *>( mEmbeddingProvider->model() ) )
      {
        if ( QStandardItem *item = itemModel->item( row ) )
        {
          item->setFlags( item->flags() & ~Qt::ItemIsEnabled );
          item->setToolTip( entry.unavailableReason );
        }
      }
    }
  }
  const QString configuredEmbeddingProvider = QgsAiEmbeddingProviderRegistry::configuredProviderId();
  const int embeddingProviderIndex = mEmbeddingProvider->findData( configuredEmbeddingProvider );
  mEmbeddingProvider->setCurrentIndex( embeddingProviderIndex >= 0 ? embeddingProviderIndex : 0 );
  contentLayout->addWidget( settingRow( tr( "Embedding provider" ), QString(), mEmbeddingProvider, page ) );

  mEmbeddingStatusLabel = new QLabel( page );
  mEmbeddingStatusLabel->setObjectName( u"aiEmbeddingProviderStatusLabel"_s );
  mEmbeddingStatusLabel->setWordWrap( true );
  mDownloadEmbeddingModelButton = new QPushButton( tr( "Download local E5 model" ), page );
  mDownloadEmbeddingModelButton->setObjectName( u"aiDownloadEmbeddingModelButton"_s );

  // Remote embedding model id, shown only when a remote provider is selected.
  mRemoteEmbeddingModel = new QLineEdit( page );
  mRemoteEmbeddingModel->setObjectName( u"aiRemoteEmbeddingModelLineEdit"_s );
  mRemoteEmbeddingModelRow = settingRow( tr( "Remote embedding model" ), QString(), mRemoteEmbeddingModel, page );
  contentLayout->addWidget( mRemoteEmbeddingModelRow );
  contentLayout->addWidget( mEmbeddingStatusLabel );
  contentLayout->addWidget( settingRow( tr( "Local model" ), QString(), mDownloadEmbeddingModelButton, page ) );

  refreshEmbeddingStatusLabel();
  refreshRemoteEmbeddingModelField();
  connect( mEmbeddingProvider, &QComboBox::currentIndexChanged, this, [this]( int ) {
    refreshEmbeddingStatusLabel();
    refreshRemoteEmbeddingModelField();
  } );
  connect( mDownloadEmbeddingModelButton, &QPushButton::clicked, this, [this]() {
    QString error;
    if ( !downloadEmbeddingModelWithConsent( this, &error ) )
    {
      if ( !error.contains( tr( "not approved" ), Qt::CaseInsensitive ) )
        QMessageBox::warning( this, tr( "Embedding model download failed" ), error.isEmpty() ? tr( "Unknown error." ) : error );
      refreshEmbeddingStatusLabel();
      return;
    }

    emit embeddingProviderSettingsChanged();
    refreshEmbeddingStatusLabel();
    QMessageBox::information( this, tr( "Embedding model downloaded" ), tr( "The local multilingual E5 embedding model is ready. Existing MinHash indexes will rebuild automatically when E5 is selected." ) );
  } );

  mAutomaticIndexing = new QCheckBox( page );
  mAutomaticIndexing->setObjectName( u"aiAutomaticIndexingCheckBox"_s );
  // Default ON only when an embedding provider is actually available; if the user
  // already chose a value, keep it. The toggle is disabled when no provider is available.
  const bool hasAutomaticSetting = indexSettings.contains( u"strata/index/automatic"_s );
  mAutomaticIndexing->setChecked( hasAutomaticSetting ? indexSettings.value( u"strata/index/automatic"_s, true ).toBool() : canUseEmbeddings );
  mAutomaticIndexing->setEnabled( canUseEmbeddings );
  contentLayout->addWidget(
    settingRow( tr( "Index workspace automatically" ), tr( "Strata refreshes the local retrieval index in the background after opening or changing the project. The task is cancellable." ), mAutomaticIndexing, page )
  );

  const bool hasLayerIndexingSetting = indexSettings.contains( u"strata/index/enable_layer_indexing"_s )
                                       || indexSettings.contains( u"geoai/index/enable_layer_indexing"_s )
                                       || indexSettings.contains( u"qgis_ai/index/enable_layer_indexing"_s );
  const bool defaultLayerIndexingEnabled = mAutomaticIndexing->isChecked();
  const bool requestedLayerIndexing
    = hasLayerIndexingSetting
        ? settingValueWithLegacy( indexSettings, u"strata/index/enable_layer_indexing"_s, QStringList { u"geoai/index/enable_layer_indexing"_s, u"qgis_ai/index/enable_layer_indexing"_s }, false ).toBool()
        : defaultLayerIndexingEnabled;
  const bool layerIndexingEnabled = requestedLayerIndexing && canUseEmbeddings;

  mEnableLayerIndexing = new QCheckBox( page );
  mEnableLayerIndexing->setObjectName( u"aiEnableLayerIndexingCheckBox"_s );
  mEnableLayerIndexing->setChecked( layerIndexingEnabled );
  mEnableLayerIndexing->setEnabled( canUseEmbeddings );
  contentLayout->addWidget(
    settingRow( tr( "Enable layer indexing" ), tr( "Layer attributes and bounding boxes are embedded with the selected indexing provider and indexed so the assistant can ground its answers on actual layer data. Reindexes on layer add/remove/edit." ), mEnableLayerIndexing, page )
  );

  // How gently indexing runs, and what it reads.
  mIndexingSpeed = new QComboBox( page );
  mIndexingSpeed->setObjectName( u"aiIndexingSpeedComboBox"_s );
  mIndexingSpeed->addItem( tr( "Low (one core, long pauses)" ), u"low"_s );
  mIndexingSpeed->addItem( tr( "Normal (two cores, short pauses)" ), u"normal"_s );
  mIndexingSpeed->addItem( tr( "High (up to four cores)" ), u"high"_s );
  const int speedIndex = mIndexingSpeed->findData( indexSettings.value( QgsAiIndexingThrottle::speedSettingsKey(), u"normal"_s ).toString() );
  mIndexingSpeed->setCurrentIndex( speedIndex >= 0 ? speedIndex : 1 );
  contentLayout->addWidget(
    settingRow( tr( "Indexing speed" ), tr( "Background indexing leaves the rest of the computer to you; a higher speed finishes sooner but uses more of the processor." ), mIndexingSpeed, page )
  );

  mPauseIndexingOnBattery = new QCheckBox( page );
  mPauseIndexingOnBattery->setObjectName( u"aiPauseIndexingOnBatteryCheckBox"_s );
  mPauseIndexingOnBattery->setChecked( indexSettings.value( QgsAiIndexingThrottle::pauseOnBatterySettingsKey(), true ).toBool() );
  contentLayout->addWidget( settingRow( tr( "Pause indexing on battery" ), tr( "Background indexing waits for mains power." ), mPauseIndexingOnBattery, page ) );

  mIndexRemoteLayers = new QCheckBox( page );
  mIndexRemoteLayers->setObjectName( u"aiIndexRemoteLayersCheckBox"_s );
  mIndexRemoteLayers->setChecked( indexSettings.value( u"strata/index/include_remote_layers"_s, false ).toBool() );
  contentLayout->addWidget(
    settingRow( tr( "Read features of remote layers" ), tr( "PostGIS, WFS and other services are indexed from their metadata unless this is on: reading their features means network traffic at every indexing." ), mIndexRemoteLayers, page )
  );

  mExcludedIndexFolders = new QLineEdit( indexSettings.value( u"strata/index/excluded_folders"_s ).toStringList().join( ", "_L1 ), page );
  mExcludedIndexFolders->setObjectName( u"aiExcludedIndexFoldersLineEdit"_s );
  mExcludedIndexFolders->setPlaceholderText( tr( "e.g. archive, old_*, tiles" ) );
  contentLayout->addWidget(
    settingRow( tr( "Folders not to index" ), tr( "Folder names, with * as wildcard, left out at any depth. Version control, build, cache and virtual environment folders are always left out." ), mExcludedIndexFolders, page )
  );

  mMaxIndexedFiles = new QSpinBox( page );
  mMaxIndexedFiles->setObjectName( u"aiMaxIndexedFilesSpinBox"_s );
  mMaxIndexedFiles->setRange( 10, 20000 );
  mMaxIndexedFiles->setValue( QgsAiIndexingScheduler::maxFiles() );
  contentLayout->addWidget( settingRow( tr( "Workspace files indexed at most" ), QString(), mMaxIndexedFiles, page ) );

  mIndexStatusLabel = new QLabel( page );
  mIndexStatusLabel->setObjectName( u"aiIndexStatusLabel"_s );
  refreshIndexStatusLabel();
  contentLayout->addWidget( mIndexStatusLabel );

  contentLayout->addWidget( sectionHeader( tr( "Strata Cloud sync" ), page ) );

  mCloudContextOptIn = new QCheckBox( page );
  mCloudContextOptIn->setObjectName( u"aiCloudContextOptInCheckBox"_s );
  mCloudContextOptIn->setChecked( indexSettings.value( u"strata/index/cloud_context_opt_in"_s, false ).toBool() );
  contentLayout->addWidget(
    settingRow( tr( "Sync safe RAG context to Strata Cloud" ), tr( "Only metadata-safe layer summaries and opted-in rules/skills/PDF/image text are sent. Geometry, coordinates, WKT and datasource URIs are blocked before upload." ), mCloudContextOptIn, page )
  );

  mCloudIndexStatusLabel = new QLabel( page );
  mCloudIndexStatusLabel->setObjectName( u"aiCloudIndexStatusLabel"_s );
  mCloudIndexStatusLabel->setWordWrap( true );
  refreshCloudIndexStatusLabel();
  contentLayout->addWidget( mCloudIndexStatusLabel );

  mSyncCloudContextButton = new QPushButton( tr( "Sync safe context now" ), page );
  mSyncCloudContextButton->setObjectName( u"aiSyncCloudContextButton"_s );
  mSyncCloudContextButton->setEnabled( mSessionManager && mSessionManager->workspaceIndex() && mModelRouter && !mModelRouter->planSessionToken().trimmed().isEmpty() );
  contentLayout->addWidget( settingRow( tr( "Cloud sync" ), QString(), mSyncCloudContextButton, page ) );

  connect( mCloudContextOptIn, &QCheckBox::toggled, this, [this]( bool ) { refreshCloudIndexStatusLabel(); } );

  connect( mSyncCloudContextButton, &QPushButton::clicked, this, [this]() {
    if ( !mSessionManager || !mSessionManager->workspaceIndex() || !mModelRouter )
      return;

    if ( !mCloudContextOptIn->isChecked() )
    {
      QMessageBox::information( this, tr( "Cloud context sync" ), tr( "Enable the Strata Cloud context sync opt-in before uploading context." ) );
      return;
    }

    const QString token = mModelRouter->planSessionToken().trimmed();
    if ( token.isEmpty() )
    {
      QMessageBox::information( this, tr( "Cloud context sync" ), tr( "Sign in to Plan Account before syncing cloud context." ) );
      return;
    }

    auto buildCloudContextItems = [this]() {
      QList<QgsAiCloudIndexClient::ContextItem> items;
      if ( mSessionManager && mSessionManager->workspaceIndex() )
      {
        mSessionManager->workspaceIndex()->ensureLoaded();
        items += QgsAiCloudIndexClient::contextItemsFromChunks( mSessionManager->workspaceIndex()->chunks() );
      }
      if ( mSessionManager )
      {
        const QgsAiAgentBehaviorSettings behavior = mSessionManager->agentBehaviorSettings();
        items += QgsAiCloudIndexClient::
          contextItemsFromWorkspaceFolders( mSessionManager->workspaceRoot(), behavior.loadWorkspaceRules ? behavior.rulesPath : QString(), behavior.loadWorkspaceSkills ? behavior.skillsPath : QString() );
      }
      return QgsAiCloudIndexClient::deduplicateContextItems( items );
    };

    const QList<QgsAiCloudIndexClient::ContextItem> items = buildCloudContextItems();
    QString validationError;
    if ( !QgsAiCloudIndexClient::validateContextItems( items, &validationError ) )
    {
      QMessageBox::warning( this, tr( "Cloud context sync blocked" ), validationError );
      refreshCloudIndexStatusLabel();
      return;
    }

    const QString workspaceRoot = mSessionManager->workspaceRoot();
    if ( workspaceRoot.trimmed().isEmpty() )
    {
      QMessageBox::warning( this, tr( "Cloud context sync" ), tr( "Workspace root is unset." ) );
      return;
    }

    mSyncCloudContextButton->setEnabled( false );
    mCloudIndexStatusLabel->setText( tr( "Cloud sync running..." ) );
    QgsAiCloudIndexClient *client = new QgsAiCloudIndexClient( this );
    connect( client, &QgsAiCloudIndexClient::contextSynced, this, [this, client]( const QgsAiCloudIndexClient::SyncResult &result ) {
      mSyncCloudContextButton->setEnabled( true );
      mCloudIndexStatusLabel->setText( tr( "Cloud sync queued %1 context items for workspace %2." ).arg( result.queued ).arg( result.workspaceId ) );
      refreshCloudIndexStatusLabel();
      client->deleteLater();
    } );
    connect( client, &QgsAiCloudIndexClient::requestFailed, this, [this, client]( const QString &message ) {
      mSyncCloudContextButton->setEnabled( true );
      mCloudIndexStatusLabel->setText( tr( "Cloud sync failed." ) );
      QMessageBox::warning( this, tr( "Cloud context sync failed" ), message );
      client->deleteLater();
    } );
    client->syncWorkspaceContext( mAccountWidget->planEndpoint(), token, workspaceRoot, QFileInfo( workspaceRoot ).fileName(), items, true );
  } );

  contentLayout->addWidget( sectionHeader( tr( "Maintenance" ), page ) );

  // Rebuilding can be expensive: heavy local CPU usage, or remote API cost and data egress.
  // Always confirm before starting, with a message tailored to the selected provider.
  mRebuildWorkspaceIndexButton = new QPushButton( tr( "Rebuild now" ), page );
  mRebuildWorkspaceIndexButton->setObjectName( u"aiRebuildWorkspaceIndexButton"_s );
  mRebuildWorkspaceIndexButton->setEnabled( mSessionManager && mSessionManager->workspaceIndex() );
  contentLayout->addWidget( settingRow( tr( "File/workspace index" ), QString(), mRebuildWorkspaceIndexButton, page ) );

  mRebuildLayerIndexButton = new QPushButton( tr( "Rebuild now" ), page );
  mRebuildLayerIndexButton->setObjectName( u"aiRebuildLayerIndexButton"_s );
  mRebuildLayerIndexButton->setEnabled( mSessionManager && mSessionManager->workspaceIndex() );
  contentLayout->addWidget( settingRow( tr( "Layer index" ), QString(), mRebuildLayerIndexButton, page ) );

  mClearIndexButton = new QPushButton( tr( "Clear index" ), page );
  mClearIndexButton->setObjectName( u"aiClearIndexButton"_s );
  mClearIndexButton->setEnabled( mSessionManager && mSessionManager->workspaceIndex() );
  mIndexSizeLabel = new QLabel( page );
  mIndexSizeLabel->setObjectName( u"aiIndexSizeLabel"_s );
  const auto refreshIndexSize = [this]() {
    const qint64 bytes = mSessionManager && mSessionManager->workspaceIndex() ? mSessionManager->workspaceIndex()->databaseSizeBytes() : 0;
    mIndexSizeLabel->setText( tr( "Index of this workspace on disk: %1" ).arg( QLocale().formattedDataSize( bytes ) ) );
  };
  refreshIndexSize();
  contentLayout->addWidget( mIndexSizeLabel );
  contentLayout->addWidget( settingRow( tr( "Index of this workspace" ), tr( "Deletes what was indexed; it is built again as you work." ), mClearIndexButton, page ) );
  connect( mClearIndexButton, &QPushButton::clicked, this, [this, refreshIndexSize]() {
    if ( !mSessionManager || !mSessionManager->workspaceIndex() )
      return;
    if ( QMessageBox::
           question( this, tr( "Clear index" ), tr( "Delete the index of this workspace? The assistant finds files and layers again once they are indexed anew." ), QMessageBox::Yes | QMessageBox::No, QMessageBox::No )
         != QMessageBox::Yes )
      return;
    mSessionManager->workspaceIndex()->clear();
    refreshIndexSize();
    refreshIndexStatusLabel();
  } );

  connect( mRebuildWorkspaceIndexButton, &QPushButton::clicked, this, [this]() {
    if ( !mSessionManager || !mSessionManager->workspaceIndex() )
      return;

    if ( !ensureEmbeddingProvider() )
      return;

    if ( !confirmRebuild( tr( "file/workspace index" ) ) )
      return;

    QString err;
    const QString workspaceRoot = mSessionManager->workspaceIndex()->workspaceRoot();
    if ( workspaceRoot.isEmpty() )
    {
      QMessageBox::warning( this, tr( "Workspace reindex failed" ), tr( "AI workspace root is unset. Save the QGIS project or configure the AI workspace root." ) );
      return;
    }

    QgsTaskManager *taskManager = QgsApplication::taskManager();
    if ( !taskManager )
    {
      QApplication::setOverrideCursor( Qt::WaitCursor );
      const bool ok = mSessionManager->workspaceIndex()->reindex( QgsAiIndexingScheduler::maxFiles(), &err );
      QApplication::restoreOverrideCursor();
      if ( !ok )
      {
        QMessageBox::warning( this, tr( "Workspace reindex failed" ), err.isEmpty() ? tr( "Unknown error." ) : err );
        return;
      }
      refreshIndexStatusLabel();
      const auto status = mSessionManager->workspaceIndex()->status();
      QMessageBox::information( this, tr( "Workspace reindex" ), tr( "Done — %1 file chunks indexed." ).arg( status.fileChunkCount ) );
      return;
    }

    mRebuildWorkspaceIndexButton->setEnabled( false );
    ManualWorkspaceIndexTask *task = new ManualWorkspaceIndexTask( mSessionManager->workspaceIndex(), workspaceRoot );
    connect( task, &QgsTask::taskCompleted, this, [this]() {
      mRebuildWorkspaceIndexButton->setEnabled( mSessionManager && mSessionManager->workspaceIndex() );
      refreshIndexStatusLabel();
      if ( mSessionManager && mSessionManager->workspaceIndex() )
      {
        const auto status = mSessionManager->workspaceIndex()->status();
        QMessageBox::information( this, tr( "Workspace reindex" ), tr( "Done — %1 file chunks indexed." ).arg( status.fileChunkCount ) );
      }
    } );
    connect( task, &QgsTask::taskTerminated, this, [this, task]() {
      mRebuildWorkspaceIndexButton->setEnabled( true );
      const QString taskError = task->errorMessage();
      if ( !taskError.isEmpty() )
        QMessageBox::warning( this, tr( "Workspace reindex failed" ), taskError );
    } );
    taskManager->addTask( task, 1 );
  } );

  connect( mRebuildLayerIndexButton, &QPushButton::clicked, this, [this]() {
    if ( !mSessionManager || !mSessionManager->workspaceIndex() )
      return;

    if ( !ensureEmbeddingProvider() )
      return;

    if ( !confirmRebuild( tr( "layer index" ) ) )
      return;

    QString err;
    QgsAiWorkspaceIndex::WorkspaceLayerSnapshot snapshot;
    if ( !mSessionManager->workspaceIndex()->createWorkspaceLayerSnapshot( snapshot, &err ) )
    {
      QMessageBox::warning( this, tr( "Layer reindex failed" ), err.isEmpty() ? tr( "Unknown error." ) : err );
      return;
    }

    QgsTaskManager *taskManager = QgsApplication::taskManager();
    if ( !taskManager )
    {
      QApplication::setOverrideCursor( Qt::WaitCursor );
      const bool ok = mSessionManager->workspaceIndex()->reindexLayerSnapshot( snapshot, &err );
      QApplication::restoreOverrideCursor();
      if ( !ok )
      {
        QMessageBox::warning( this, tr( "Layer reindex failed" ), err.isEmpty() ? tr( "Unknown error." ) : err );
        return;
      }
      refreshIndexStatusLabel();
      const auto status = mSessionManager->workspaceIndex()->status();
      QMessageBox::information( this, tr( "Layer reindex" ), tr( "Done — %1 layer chunks indexed." ).arg( status.layerChunkCount ) );
      return;
    }

    mRebuildLayerIndexButton->setEnabled( false );
    ManualLayerIndexTask *task = new ManualLayerIndexTask( mSessionManager->workspaceIndex(), snapshot );
    connect( task, &QgsTask::taskCompleted, this, [this]() {
      mRebuildLayerIndexButton->setEnabled( mSessionManager && mSessionManager->workspaceIndex() );
      refreshIndexStatusLabel();
      if ( mSessionManager && mSessionManager->workspaceIndex() )
      {
        const auto status = mSessionManager->workspaceIndex()->status();
        QMessageBox::information( this, tr( "Layer reindex" ), tr( "Done — %1 layer chunks indexed." ).arg( status.layerChunkCount ) );
      }
    } );
    connect( task, &QgsTask::taskTerminated, this, [this, task]() {
      mRebuildLayerIndexButton->setEnabled( true );
      const QString taskError = task->errorMessage();
      if ( !taskError.isEmpty() )
        QMessageBox::warning( this, tr( "Layer reindex failed" ), taskError );
    } );
    taskManager->addTask( task, 1 );
  } );

  return page;
}

QWidget *QgsAiSettingsDialog::buildWorkspacePage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Workspace" ), tr( "Where the assistant reads and writes files." ), contentLayout );

  QgsSettings workspaceSettings;
  mWorkspaceRoot
    = new QLineEdit( settingValueWithLegacy( workspaceSettings, u"strata/workspace/root"_s, QStringList { u"geoai/workspace/root"_s, u"qgis_ai/workspace/root"_s }, QString() ).toString(), page );
  mWorkspaceRoot->setObjectName( u"aiWorkspaceRootLineEdit"_s );
  mWorkspaceRoot->setPlaceholderText( tr( "Used when the QGIS project is unsaved" ) );
  QPushButton *browseWorkspaceRoot = new QPushButton( tr( "Browse..." ), page );
  QWidget *workspaceRootWidget = new QWidget( page );
  QHBoxLayout *workspaceRootLayout = new QHBoxLayout( workspaceRootWidget );
  workspaceRootLayout->setContentsMargins( 0, 0, 0, 0 );
  workspaceRootLayout->addWidget( mWorkspaceRoot, 1 );
  workspaceRootLayout->addWidget( browseWorkspaceRoot );
  contentLayout->addWidget( settingRowFullWidth( tr( "AI workspace root" ), tr( "Used only when the current QGIS project has no home path." ), workspaceRootWidget, page ) );

  // Workspace trust: gates rules/skills loading and the risky tools.
  mTrustWorkspace = new QCheckBox( page );
  mTrustWorkspace->setObjectName( u"aiTrustWorkspaceCheckBox"_s );
  contentLayout->addWidget( settingRow( tr( "Trust this workspace" ), tr( "Enables rules/skills files and the run_python, install_python_package and download_file tools." ), mTrustWorkspace, page ) );
  refreshTrustWorkspace();

  connect( browseWorkspaceRoot, &QPushButton::clicked, this, [this]() {
    const QString dir = QFileDialog::getExistingDirectory( this, tr( "Choose AI workspace root" ), mWorkspaceRoot->text().trimmed() );
    if ( !dir.isEmpty() )
      mWorkspaceRoot->setText( QDir::cleanPath( dir ) );
  } );
  connect( mWorkspaceRoot, &QLineEdit::textChanged, this, [this]( const QString & ) { refreshTrustWorkspace(); } );

  return page;
}

QWidget *QgsAiSettingsDialog::buildPrivacyPage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Privacy & Telemetry" ), tr( "All sharing is opt-in and metadata-only, never payloads." ), contentLayout );

  QLabel *encryptionStatus = new QLabel( page );
  encryptionStatus->setObjectName( u"aiCredentialEncryptionStatusLabel"_s );
  encryptionStatus->setWordWrap( true );
  encryptionStatus->setText(
    tr( "New credentials use the system keychain. Session-only credentials are forgotten when Strata closes. Existing credentials are removed from legacy storage only after a verified migration." )
  );
  contentLayout->addWidget( encryptionStatus );

  QgsSettings productSettings;
  mPrivacyMetadataOnly = new QCheckBox( page );
  mPrivacyMetadataOnly->setObjectName( u"aiPrivacyMetadataOnlyCheckBox"_s );
  mPrivacyMetadataOnly->setChecked( productSettings.value( u"strata/privacy/metadata_only_ack"_s, false ).toBool() );
  contentLayout->addWidget(
    settingRow( tr( "Acknowledge managed cloud privacy boundary" ), tr( "Managed cloud features only ever receive metadata, never your data payloads." ), mPrivacyMetadataOnly, page )
  );

  mTelemetryOptIn = new QCheckBox( page );
  mTelemetryOptIn->setObjectName( u"aiTelemetryOptInCheckBox"_s );
  mTelemetryOptIn->setChecked( productSettings.value( u"strata/telemetry/opt_in"_s, false ).toBool() );
  contentLayout->addWidget( settingRow( tr( "Share product telemetry" ), tr( "Metadata-only usage signals." ), mTelemetryOptIn, page ) );

  mCrashReportOptIn = new QCheckBox( page );
  mCrashReportOptIn->setObjectName( u"aiCrashReportOptInCheckBox"_s );
  mCrashReportOptIn->setChecked( productSettings.value( u"strata/crash_reporting/metadata_only_opt_in"_s, false ).toBool() );
  contentLayout->addWidget( settingRow( tr( "Share crash reports" ), tr( "Metadata-only crash signatures." ), mCrashReportOptIn, page ) );

  QLabel *storageNote = new QLabel(
    tr(
      "AI credentials are saved in the system keychain or, when explicitly chosen, kept only for the current session. Legacy credentials are migrated after verification. Leave API key fields empty "
      "to keep the current saved value. Agent rules and skills are stored locally in application "
      "settings."
    ),
    page
  );
  storageNote->setProperty( "aiRole", u"rowDescription"_s );
  storageNote->setWordWrap( true );
  contentLayout->addWidget( storageNote );

  return page;
}

QWidget *QgsAiSettingsDialog::buildOnboardingPage()
{
  QVBoxLayout *contentLayout = nullptr;
  QWidget *page = createPage( tr( "Onboarding & Release" ), tr( "First-run checklist and release readiness." ), contentLayout );

  mOnboardingStatusLabel = new QLabel( page );
  mOnboardingStatusLabel->setObjectName( u"aiOnboardingStatusLabel"_s );
  mOnboardingStatusLabel->setWordWrap( true );
  contentLayout->addWidget( settingRowFullWidth( tr( "First-run checklist" ), QString(), mOnboardingStatusLabel, page ) );

  mReleaseDryRunStatus = new QLabel( page );
  mReleaseDryRunStatus->setObjectName( u"aiReleaseDryRunStatusLabel"_s );
  mReleaseDryRunStatus->setWordWrap( true );
  QgsSettings productSettings;
  const QString savedReleaseChecksum = productSettings.value( u"strata/release/dry_run_checksum"_s ).toString();
  mReleaseDryRunStatus->setText( savedReleaseChecksum.isEmpty() ? tr( "Release dry-run: not run." ) : tr( "Release dry-run checksum: %1." ).arg( savedReleaseChecksum ) );

  QPushButton *releaseDryRunButton = new QPushButton( tr( "Run release dry-run" ), page );
  releaseDryRunButton->setObjectName( u"aiReleaseDryRunButton"_s );
  QPushButton *createDemoProjectButton = new QPushButton( tr( "Create demo project" ), page );
  createDemoProjectButton->setObjectName( u"aiCreateDemoProjectButton"_s );

  contentLayout->addWidget( settingRow( tr( "Demo project" ), tr( "Creates a small in-memory sample project to try the assistant." ), createDemoProjectButton, page ) );
  // A check for whoever prepares a release, not for users.
  const bool developerMode = productSettings.value( u"strata/developer_mode"_s, false ).toBool() || qEnvironmentVariableIsSet( "STRATA_DEVELOPER" );
  QWidget *releaseDryRunRow = settingRow( tr( "Release dry-run" ), tr( "Stores only a local readiness manifest checksum." ), releaseDryRunButton, page );
  releaseDryRunRow->setVisible( developerMode );
  mReleaseDryRunStatus->setVisible( developerMode );
  contentLayout->addWidget( releaseDryRunRow );
  contentLayout->addWidget( mReleaseDryRunStatus );

  refreshOnboardingStatus();

  connect( mPrivacyMetadataOnly, &QCheckBox::toggled, this, [this]( bool ) { refreshOnboardingStatus(); } );
  connect( mAutomaticIndexing, &QCheckBox::toggled, this, [this]( bool ) { refreshOnboardingStatus(); } );
  connect( mEnableLayerIndexing, &QCheckBox::toggled, this, [this]( bool ) { refreshOnboardingStatus(); } );
  connect( mCloudContextOptIn, &QCheckBox::toggled, this, [this]( bool ) { refreshOnboardingStatus(); } );

  connect( createDemoProjectButton, &QPushButton::clicked, this, [this]() {
    QgsProject *project = QgsProject::instance();
    if ( !project )
      return;

    project->clear();
    QgsVectorLayer *demoLayer = new QgsVectorLayer( u"Point?field=name:string&field=kind:string&crs=EPSG:4326"_s, tr( "Strata demo points" ), u"memory"_s );
    if ( demoLayer->isValid() && demoLayer->dataProvider() )
    {
      QList<QgsFeature> features;
      QgsFeature rome( demoLayer->fields() );
      rome.setAttributes( QVariantList { tr( "Rome sample" ), tr( "point of interest" ) } );
      rome.setGeometry( QgsGeometry::fromWkt( u"POINT(12.4924 41.8902)"_s ) );
      features << rome;
      QgsFeature milan( demoLayer->fields() );
      milan.setAttributes( QVariantList { tr( "Milan sample" ), tr( "point of interest" ) } );
      milan.setGeometry( QgsGeometry::fromWkt( u"POINT(9.1900 45.4642)"_s ) );
      features << milan;
      demoLayer->dataProvider()->addFeatures( features );
      demoLayer->updateExtents();
      project->addMapLayer( demoLayer );
    }
    else
    {
      delete demoLayer;
    }
    project->setTitle( tr( "Strata demo project" ) );

    QgsSettings settings;
    settings.setValue( u"strata/onboarding/demo_project_seen"_s, true );
    refreshOnboardingStatus();
    emit demoProjectCreated();
  } );

  connect( releaseDryRunButton, &QPushButton::clicked, this, [this]() {
    QJsonObject manifest;
    manifest.insert( u"app"_s, u"Strata"_s );
    manifest.insert( u"plan_ready"_s, mModelRouter && mModelRouter->isProviderAvailable( QgsAiModelRouter::Provider::Plan ) );
    manifest.insert( u"byok_ready"_s, hasByokProvider( mModelRouter ) );
    manifest.insert( u"privacy_metadata_only_ack"_s, mPrivacyMetadataOnly && mPrivacyMetadataOnly->isChecked() );
    manifest.insert( u"telemetry_opt_in"_s, mTelemetryOptIn && mTelemetryOptIn->isChecked() );
    manifest.insert( u"crash_metadata_only_opt_in"_s, mCrashReportOptIn && mCrashReportOptIn->isChecked() );
    manifest.insert( u"automatic_indexing"_s, mAutomaticIndexing && mAutomaticIndexing->isChecked() );
    manifest.insert( u"layer_indexing"_s, mEnableLayerIndexing && mEnableLayerIndexing->isChecked() );
    manifest.insert( u"cloud_context_opt_in"_s, mCloudContextOptIn && mCloudContextOptIn->isChecked() );
    manifest.insert( u"demo_project_ready"_s, demoProjectReady() );
    manifest.insert( u"active_provider"_s, mModelRouter ? mModelRouter->providerDisplayName( mModelRouter->resolveProvider() ) : QString() );
    manifest.insert( u"workspace_root_configured"_s, mSessionManager && !mSessionManager->workspaceRoot().trimmed().isEmpty() );

    const QByteArray payload = QJsonDocument( manifest ).toJson( QJsonDocument::Compact );
    const QString checksum = QString::fromLatin1( QCryptographicHash::hash( payload, QCryptographicHash::Sha256 ).toHex().left( 16 ) );
    mReleaseDryRunStatus->setProperty( "checksum", checksum );
    mReleaseDryRunStatus->setText( tr( "Release dry-run OK. Metadata checksum: %1." ).arg( checksum ) );

    QgsSettings settings;
    settings.setValue( u"strata/release/dry_run_checksum"_s, checksum );
    settings.setValue( u"strata/release/dry_run_manifest"_s, QString::fromUtf8( payload ) );
  } );

  return page;
}

void QgsAiSettingsDialog::refreshSidebarAccountHeader()
{
  const bool signedIn = mAccountWidget->isSignedIn();
  const QString email = mAccountWidget->accountEmail();
  mSidebarAvatar->setText( email.isEmpty() ? ( signedIn ? u"•"_s : u"?"_s ) : email.left( 1 ).toUpper() );
  mSidebarEmailLabel->setText( signedIn ? ( email.isEmpty() ? tr( "Signed in" ) : email ) : tr( "Not signed in" ) );
  mSidebarEmailLabel->setToolTip( email );
}

void QgsAiSettingsDialog::refreshTrustWorkspace()
{
  auto trustRoot = [this]() {
    if ( QgsProject::instance() )
    {
      const QString projectHome = QgsProject::instance()->homePath().trimmed();
      if ( !projectHome.isEmpty() )
        return QDir( projectHome ).absolutePath();
    }

    const QString requestedWorkspaceRoot = mWorkspaceRoot->text().trimmed();
    return requestedWorkspaceRoot.isEmpty() ? QString() : QDir( requestedWorkspaceRoot ).absolutePath();
  };

  mTrustRootForCheckbox = trustRoot();
  const bool hasRoot = !mTrustRootForCheckbox.isEmpty();
  mTrustWorkspace->setEnabled( hasRoot );
  mTrustWorkspace->setChecked( hasRoot && QgsAiWorkspaceTrust::isTrusted( mTrustRootForCheckbox ) );
  mTrustWorkspace->setToolTip( hasRoot ? tr( "Current workspace: %1" ).arg( mTrustRootForCheckbox ) : tr( "No workspace configured." ) );
}

void QgsAiSettingsDialog::refreshEmbeddingStatusLabel()
{
  const QString providerId = mEmbeddingProvider->currentData().toString();
  const bool e5Compiled = QgsAiEmbeddingProviderRegistry::providerIds().contains( QgsAiE5EmbeddingProvider::staticProviderId() );
  if ( QgsAiEmbeddingProviderRegistry::isRemoteProviderId( providerId ) )
  {
    mEmbeddingStatusLabel->setText( tr( "Remote workspace indexing will use %1 only because it is explicitly selected here. Saved provider keys do not switch indexing by themselves." )
                                      .arg( QgsAiEmbeddingProviderRegistry::displayNameForProviderId( providerId ) ) );
    mDownloadEmbeddingModelButton->setVisible( false );
  }
  else if ( providerId == QgsAiE5EmbeddingProvider::staticProviderId() )
  {
    if ( !e5Compiled )
    {
      mEmbeddingStatusLabel->setText(
        tr( "Local multilingual E5 is not available in this build because ONNX Runtime and SentencePiece support were not found at compile time. Use MinHash or rebuild Strata with those dependencies." )
      );
      mDownloadEmbeddingModelButton->setVisible( false );
      mDownloadEmbeddingModelButton->setEnabled( false );
      return;
    }
    QString filesError;
    const QString modelDir = QgsAiE5EmbeddingProvider::activeModelDirectory();
    const bool filesAvailable = QgsAiE5EmbeddingProvider::modelFilesAvailable( modelDir, &filesError );
    const QString developerDir = QgsAiE5EmbeddingProvider::developerModelDirectory();
    mDownloadEmbeddingModelButton->setVisible( true );
    mDownloadEmbeddingModelButton->setEnabled( developerDir.isEmpty() );
    mDownloadEmbeddingModelButton->setToolTip(
      developerDir.isEmpty() ? tr( "Download the pinned multilingual E5 ONNX model and SentencePiece tokenizer to the local Strata model cache." )
                             : tr( "STRATA_AI_EMBEDDING_MODEL_DIR is set; unset it to use the downloaded cache from this dialog." )
    );
    mEmbeddingStatusLabel->setText(
      filesAvailable ? tr( "Local multilingual E5 model files are installed in %1. Indexing runs on this computer without an API key." ).arg( modelDir )
                     : tr( "Local multilingual E5 model is not installed or not usable: %1\nDownload size: %2. Developers can set STRATA_AI_EMBEDDING_MODEL_DIR." )
                         .arg( filesError, humanBytes( QgsAiE5EmbeddingProvider::downloadSize() ) )
    );
  }
  else
  {
    mEmbeddingStatusLabel->setText(
      e5Compiled ? tr( "Local MinHash fallback is available and runs on this computer without an API key. Semantic quality is lower than multilingual E5." )
                 : tr( "Local MinHash fallback is available and runs on this computer without an API key. Multilingual E5 requires ONNX Runtime and SentencePiece support in the Strata build." )
    );
    mDownloadEmbeddingModelButton->setVisible( false );
  }
}

void QgsAiSettingsDialog::refreshRemoteEmbeddingModelField()
{
  const QString providerId = mEmbeddingProvider->currentData().toString();
  const bool remote = QgsAiEmbeddingProviderRegistry::isRemoteProviderId( providerId );
  mRemoteEmbeddingModelRow->setVisible( remote );
  if ( remote )
  {
    QgsSettings settings;
    mRemoteEmbeddingModel->setText( settings.value( remoteEmbeddingModelSettingKey( providerId ), remoteEmbeddingModelDefault( providerId ) ).toString() );
    mRemoteEmbeddingModel->setPlaceholderText( remoteEmbeddingModelDefault( providerId ) );
  }
}

void QgsAiSettingsDialog::refreshIndexStatusLabel()
{
  if ( mSessionManager && mSessionManager->workspaceIndex() )
  {
    QgsAiWorkspaceIndex *index = mSessionManager->workspaceIndex();
    // Opening the settings never waits on the index: it loads in the background and the
    // label refreshes once it is ready.
    const auto status = index->status();
    if ( status.loading )
    {
      mIndexStatusLabel->setText( tr( "Indexed: loading…" ) );
      connect( index, &QgsAiWorkspaceIndex::loaded, this, &QgsAiSettingsDialog::refreshIndexStatusLabel, Qt::SingleShotConnection );
      index->requestLoad();
      return;
    }
    mIndexStatusLabel->setText( tr( "Indexed: %1 file chunks, %2 layer chunks (last sync: %3)" )
                                  .arg( status.fileChunkCount )
                                  .arg( status.layerChunkCount )
                                  .arg( status.lastSync.isValid() ? status.lastSync.toLocalTime().toString( Qt::ISODate ) : tr( "never" ) ) );
  }
  else
  {
    mIndexStatusLabel->setText( tr( "Indexed: (workspace index unavailable)" ) );
  }
}

void QgsAiSettingsDialog::refreshCloudIndexStatusLabel()
{
  QList<QgsAiCloudIndexClient::ContextItem> items;
  if ( mSessionManager && mSessionManager->workspaceIndex() )
  {
    mSessionManager->workspaceIndex()->ensureLoaded();
    items += QgsAiCloudIndexClient::contextItemsFromChunks( mSessionManager->workspaceIndex()->chunks() );
  }
  if ( mSessionManager )
  {
    const QgsAiAgentBehaviorSettings behavior = mSessionManager->agentBehaviorSettings();
    items += QgsAiCloudIndexClient::
      contextItemsFromWorkspaceFolders( mSessionManager->workspaceRoot(), behavior.loadWorkspaceRules ? behavior.rulesPath : QString(), behavior.loadWorkspaceSkills ? behavior.skillsPath : QString() );
  }
  items = QgsAiCloudIndexClient::deduplicateContextItems( items );

  if ( items.isEmpty() )
  {
    mCloudIndexStatusLabel->setText( tr( "Cloud sync preview: no safe context items yet." ) );
    return;
  }
  QString validationError;
  if ( !QgsAiCloudIndexClient::validateContextItems( items, &validationError ) )
  {
    mCloudIndexStatusLabel->setText( tr( "Cloud sync preview blocked: %1" ).arg( validationError ) );
    return;
  }
  int layerItems = 0;
  int ruleItems = 0;
  int skillItems = 0;
  int documentItems = 0;
  for ( const QgsAiCloudIndexClient::ContextItem &item : items )
  {
    if ( item.sourceType == "layer"_L1 )
      ++layerItems;
    else if ( item.sourceType == "rule"_L1 )
      ++ruleItems;
    else if ( item.sourceType == "skill"_L1 )
      ++skillItems;
    else if ( item.sourceType == "pdf"_L1 || item.sourceType == "image"_L1 )
      ++documentItems;
  }
  mCloudIndexStatusLabel->setText(
    tr( "Cloud sync preview: %1 items (%2 layer, %3 rule, %4 skill, %5 document)." ).arg( items.size() ).arg( layerItems ).arg( ruleItems ).arg( skillItems ).arg( documentItems )
  );
}

QString QgsAiSettingsDialog::onboardingStatusText() const
{
  const bool planReady = mModelRouter && mModelRouter->isProviderAvailable( QgsAiModelRouter::Provider::Plan );
  const bool byokReady = hasByokProvider( mModelRouter );
  const bool modelReady = mModelRouter && mModelRouter->isProviderUsable( mModelRouter->resolveProvider() );
  const bool indexingReady = ( mAutomaticIndexing && mAutomaticIndexing->isChecked() )
                             || ( mEnableLayerIndexing && mEnableLayerIndexing->isChecked() )
                             || ( mCloudContextOptIn && mCloudContextOptIn->isChecked() );
  QStringList lines;
  lines << tr( "Plan login: %1" ).arg( planReady ? tr( "ready" ) : tr( "not configured" ) );
  lines << tr( "BYOK fallback: %1" ).arg( byokReady ? tr( "ready" ) : tr( "optional" ) );
  lines << tr( "Privacy boundary: %1" ).arg( mPrivacyMetadataOnly && mPrivacyMetadataOnly->isChecked() ? tr( "acknowledged" ) : tr( "needs review" ) );
  lines << tr( "Model route: %1" ).arg( modelReady ? tr( "ready" ) : tr( "choose Plan or BYOK model" ) );
  lines << tr( "Indexing: %1" ).arg( indexingReady ? tr( "enabled" ) : tr( "off" ) );
  lines << tr( "Demo project: %1" ).arg( demoProjectReady() ? tr( "ready" ) : tr( "not created" ) );
  return lines.join( '\n' );
}

void QgsAiSettingsDialog::refreshOnboardingStatus()
{
  if ( mOnboardingStatusLabel )
    mOnboardingStatusLabel->setText( onboardingStatusText() );
}

bool QgsAiSettingsDialog::ensureEmbeddingProvider()
{
  if ( mSessionManager && mSessionManager->workspaceIndex() && mSessionManager->workspaceIndex()->embeddingProviderAvailable() )
    return true;

  QMessageBox::information( this, tr( "Workspace indexing unavailable" ), tr( "Workspace indexing requires the selected embedding provider to be available. Chat with Claude/Codex works without indexing." ) );
  return false;
}

bool QgsAiSettingsDialog::confirmRebuild( const QString &what )
{
  const QString providerId = mEmbeddingProvider->currentData().toString();
  const QString message = QgsAiEmbeddingProviderRegistry::isRemoteProviderId( providerId )
                            ? tr( "Rebuilding the %1 will re-embed content with the remote provider %2. This sends data to that service and may incur API costs.\n\nProceed?" )
                                .arg( what, QgsAiEmbeddingProviderRegistry::displayNameForProviderId( providerId ) )
                            : tr( "Rebuilding the %1 will re-embed content on this computer and may use significant CPU for a while.\n\nProceed?" ).arg( what );
  return QMessageBox::question( this, tr( "Rebuild index" ), message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No ) == QMessageBox::Yes;
}

bool QgsAiSettingsDialog::applySettings()
{
  if ( !mModelRouter )
    return false;

  const QString pendingOpenAiKey = mOpenAiKey->text().trimmed();
  const QString pendingOpenRouterKey = mOpenRouterKey->text().trimmed();
  const QString pendingClaudeKey = mClaudeConnectWidget->pendingApiKey();
  const QString pendingPlanToken = mAccountWidget->manualSessionToken();

  QString errorMessages;
  QString error;

  if ( !pendingOpenAiKey.isEmpty() && !mModelRouter->storeApiKey( QgsAiModelRouter::Provider::OpenAi, pendingOpenAiKey, &error ) )
    errorMessages += error + '\n';
  if ( !pendingOpenRouterKey.isEmpty() && !mModelRouter->storeApiKey( QgsAiModelRouter::Provider::OpenRouter, pendingOpenRouterKey, &error ) )
    errorMessages += error + '\n';
  if ( !pendingClaudeKey.isEmpty() && !mModelRouter->storeApiKey( QgsAiModelRouter::Provider::Claude, pendingClaudeKey, &error ) )
    errorMessages += error + '\n';
  if ( !pendingPlanToken.isEmpty() && !mModelRouter->setPlanSessionToken( pendingPlanToken, &error ) )
    errorMessages += error + '\n';

  if ( !errorMessages.isEmpty() )
  {
    QMessageBox::warning( this, tr( "Credentials not saved" ), errorMessages.trimmed() );
    return false;
  }

  QgsAiModelRouter::ProviderSettings openAiSettings = mModelRouter->providerSettings( QgsAiModelRouter::Provider::OpenAi );
  openAiSettings.endpoint = mOpenAiEndpoint->text().trimmed();
  openAiSettings.model = mOpenAiModel->text().trimmed();
  openAiSettings.enabled = !pendingOpenAiKey.isEmpty() || mModelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::OpenAi ) || !qEnvironmentVariable( "OPENAI_API_KEY" ).trimmed().isEmpty();
  mModelRouter->setProviderSettings( QgsAiModelRouter::Provider::OpenAi, openAiSettings );

  QgsAiModelRouter::ProviderSettings openRouterSettings = mModelRouter->providerSettings( QgsAiModelRouter::Provider::OpenRouter );
  openRouterSettings.endpoint = mOpenRouterEndpoint->text().trimmed();
  openRouterSettings.model = mOpenRouterModel->currentText().trimmed();
  openRouterSettings.autoRouting = mOpenRouterAutoRouting->isChecked();
  openRouterSettings.enabled = !pendingOpenRouterKey.isEmpty()
                               || mModelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::OpenRouter )
                               || !qEnvironmentVariable( "OPENROUTER_API_KEY" ).trimmed().isEmpty();
  mModelRouter->setProviderSettings( QgsAiModelRouter::Provider::OpenRouter, openRouterSettings );

  QgsAiModelRouter::ProviderSettings codexSettings = mModelRouter->providerSettings( QgsAiModelRouter::Provider::Codex );
  codexSettings.endpoint = mCodexEndpoint->text().trimmed();
  codexSettings.model = mCodexModel->text().trimmed();
  codexSettings.credentialMode = QgsAiModelRouter::CredentialMode::OAuth;
  codexSettings.enabled = QgsAiCodexOAuthClient::hasRefreshToken();
  mModelRouter->setProviderSettings( QgsAiModelRouter::Provider::Codex, codexSettings );

  QgsAiModelRouter::ProviderSettings planSettings = mModelRouter->providerSettings( QgsAiModelRouter::Provider::Plan );
  planSettings.endpoint = mAccountWidget->planEndpoint();
  planSettings.authConfigId = mAccountWidget->planAuthConfigId();
  mModelRouter->setProviderSettings( QgsAiModelRouter::Provider::Plan, planSettings );
  mModelRouter->setPlanAuthConfigId( mAccountWidget->planAuthConfigId() );


  {
    QgsSettings settings;
    const QString requestedWorkspaceRoot = mWorkspaceRoot->text().trimmed();
    const QString configuredWorkspaceRoot = requestedWorkspaceRoot.isEmpty() ? QString() : QDir::cleanPath( requestedWorkspaceRoot );
    if ( configuredWorkspaceRoot.isEmpty() )
    {
      settings.remove( u"strata/workspace/root"_s );
      settings.remove( u"geoai/workspace/root"_s );
      settings.remove( u"qgis_ai/workspace/root"_s );
    }
    else
    {
      settings.setValue( u"strata/workspace/root"_s, QDir::cleanPath( configuredWorkspaceRoot ) );
      settings.remove( u"geoai/workspace/root"_s );
      settings.remove( u"qgis_ai/workspace/root"_s );
    }

    if ( mSessionManager && QgsProject::instance() && QgsProject::instance()->homePath().isEmpty() )
      mSessionManager->setWorkspaceRoot( configuredWorkspaceRoot );

    // Persist the trust decision only for the workspace represented by the
    // checkbox. Changing the root in this dialog recalculates the checkbox state,
    // so trust is never inherited from a previous root.
    if ( !mTrustRootForCheckbox.isEmpty() )
      QgsAiWorkspaceTrust::setState( mTrustRootForCheckbox, mTrustWorkspace->isChecked() ? QgsAiWorkspaceTrust::State::Trusted : QgsAiWorkspaceTrust::State::Untrusted );

    // Workspace content goes to a remote embedding service only with the user's consent, asked
    // once per service. Declining keeps indexing on this computer.
    const QString embeddingProviderId = mEmbeddingProvider->currentData().toString();
    if ( QgsAiEmbeddingProviderRegistry::isRemoteProviderId( embeddingProviderId ) && !QgsAiEmbeddingProviderRegistry::remoteEmbeddingConsented( embeddingProviderId ) )
    {
      const QString service = QgsAiEmbeddingProviderRegistry::displayNameForProviderId( embeddingProviderId );
      const bool agreed = QMessageBox::question(
                            this,
                            tr( "Send workspace content to %1?" ).arg( service ),
                            tr(
                              "Indexing with %1 sends to that service:\n"
                              "• the text of the files in the AI workspace (up to 500 files: notes, tables, JSON, SQL, project files);\n"
                              "• sampled attribute values and bounding boxes of the project layers;\n"
                              "• your chat questions, to search the index.\n\n"
                              "The index itself stays on this computer. The local model sends nothing.\n\n"
                              "Send workspace content to %1?"
                            )
                              .arg( service ),
                            QMessageBox::Yes | QMessageBox::No,
                            QMessageBox::No
                          )
                          == QMessageBox::Yes;
      QgsAiEmbeddingProviderRegistry::setRemoteEmbeddingConsent( embeddingProviderId, agreed );
      if ( !agreed )
      {
        const int local = mEmbeddingProvider->findData( QgsAiE5EmbeddingProvider::staticProviderId() );
        if ( local >= 0 )
          mEmbeddingProvider->setCurrentIndex( local );
      }
    }

    QgsAiEmbeddingProviderRegistry::setConfiguredProviderId( mEmbeddingProvider->currentData().toString() );
    if ( QgsAiEmbeddingProviderRegistry::isRemoteProviderId( mEmbeddingProvider->currentData().toString() ) )
    {
      const QString embeddingModelKey = remoteEmbeddingModelSettingKey( mEmbeddingProvider->currentData().toString() );
      const QString embeddingModelValue = mRemoteEmbeddingModel->text().trimmed();
      if ( embeddingModelValue.isEmpty() )
        settings.remove( embeddingModelKey );
      else
        settings.setValue( embeddingModelKey, embeddingModelValue );
    }
    settings.setValue( u"strata/index/automatic"_s, mAutomaticIndexing->isChecked() );
    settings.setValue( QgsAiIndexingThrottle::speedSettingsKey(), mIndexingSpeed->currentData().toString() );
    settings.setValue( QgsAiIndexingThrottle::pauseOnBatterySettingsKey(), mPauseIndexingOnBattery->isChecked() );
    settings.setValue( u"strata/index/include_remote_layers"_s, mIndexRemoteLayers->isChecked() );
    QStringList excludedFolders;
    for ( const QString &folder : mExcludedIndexFolders->text().split( ',' ) )
    {
      if ( !folder.trimmed().isEmpty() )
        excludedFolders << folder.trimmed();
    }
    settings.setValue( u"strata/index/excluded_folders"_s, excludedFolders );
    settings.setValue( u"strata/index/max_files"_s, mMaxIndexedFiles->value() );
    settings.setValue( u"strata/index/cloud_context_opt_in"_s, mCloudContextOptIn->isChecked() );
    settings.setValue( u"strata/privacy/metadata_only_ack"_s, mPrivacyMetadataOnly->isChecked() );
    settings.setValue( u"strata/telemetry/opt_in"_s, mTelemetryOptIn->isChecked() );
    settings.setValue( u"strata/crash_reporting/metadata_only_opt_in"_s, mCrashReportOptIn->isChecked() );
  }

  emit embeddingProviderSettingsChanged();

  QgsAiModelRouter::ProviderSettings claudeSettings = mModelRouter->providerSettings( QgsAiModelRouter::Provider::Claude );
  claudeSettings.endpoint = mClaudeEndpoint->text().trimmed();
  claudeSettings.model = mClaudeConnectWidget->modelText();
  const bool claudeSubscription = pendingClaudeKey.isEmpty() && mModelRouter->hasStoredOAuthRefreshToken( QgsAiModelRouter::Provider::Claude );
  claudeSettings.credentialMode = claudeSubscription ? QgsAiModelRouter::CredentialMode::OAuth : QgsAiModelRouter::CredentialMode::ApiKey;
  claudeSettings.enabled = !pendingClaudeKey.isEmpty() || mModelRouter->hasStoredApiKey( QgsAiModelRouter::Provider::Claude ) || claudeSubscription;
  mModelRouter->setProviderSettings( QgsAiModelRouter::Provider::Claude, claudeSettings );

  QgsSettings gisSaveSettings;
  gisSaveSettings.setValue( QgsAiGisSuggestionEngine::globalEnabledSettingsKey(), mGisSuggestionsEnabled->isChecked() );
  gisSaveSettings.setValue( gisProjectSettingsKey(), mGisSuggestionsProjectEnabled->isChecked() );

  if ( mSessionManager )
  {
    QgsAiAgentBehaviorSettings behaviorSettings = mSessionManager->agentBehaviorSettings();
    behaviorSettings.allowCustomActions = mAllowCustomActions->isChecked();
    behaviorSettings.rememberPythonApprovalsForSession = mRememberPythonApprovalsForSession->isChecked();
    behaviorSettings.runPythonTimeoutSeconds = mRunPythonTimeoutSeconds->value();
    behaviorSettings.maxToolIterationsPerTurn = mMaxToolIterationsPerTurn->value();
    behaviorSettings.maxTotalToolIterationsPerTurn = mMaxTotalToolIterationsPerTurn->value();
    behaviorSettings.autoContinueToolBlocks = mAutoContinueToolBlocks->isChecked();
    behaviorSettings.rulesPath = mRulesRelativeDirForList;
    behaviorSettings.skillsPath = mSkillsRelativeDirForList;
    mSessionManager->setAgentBehaviorSettings( behaviorSettings );

    const bool canUseEmbeddings = mSessionManager->workspaceIndex() && mSessionManager->workspaceIndex()->embeddingProviderAvailable();
    bool layerIndexingChoice = mEnableLayerIndexing->isChecked() && canUseEmbeddings;

    if ( mEnableLayerIndexing->isChecked() && !canUseEmbeddings )
    {
      errorMessages += tr( "Layer indexing requires the selected embedding provider to be available." ) + '\n';
      mEnableLayerIndexing->setChecked( false );
    }

    QgsSettings layerSettings;
    layerSettings.setValue( u"strata/index/enable_layer_indexing"_s, layerIndexingChoice );
    layerSettings.remove( u"geoai/index/enable_layer_indexing"_s );
    layerSettings.remove( u"qgis_ai/index/enable_layer_indexing"_s );
    // Enabling schedules every layer by itself; keeping it on must not re-embed them all.
    if ( mLayerIndexCoordinator )
      mLayerIndexCoordinator->setEnabled( layerIndexingChoice && canUseEmbeddings );
  }

  if ( !errorMessages.isEmpty() )
    QMessageBox::warning( this, tr( "Provider configuration warnings" ), errorMessages.trimmed() );
  return errorMessages.isEmpty();
}
