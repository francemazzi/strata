#include "qgsupdatewidget.h"
#include "qgsupdateservice.h"
#include "qgsconfig.h"
#include "qgssettings.h"
#include "qgisapp.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QProgressBar>
#include <QPlainTextEdit>
using namespace Qt::StringLiterals;
QgsUpdateWidget::QgsUpdateWidget( QWidget *parent )
  : QgsOptionsPageWidget( parent )
{
  setObjectName( u"strataUpdates"_s );
  auto *layout = new QVBoxLayout( this );
  layout->addWidget( new QLabel( tr( "Installed version: %1" ).arg( QString::fromUtf8( STRATA_VERSION ) ), this ) );
  auto *automatic = new QCheckBox( tr( "Check for updates at startup" ), this );
  automatic->setChecked( QgsSettings().value( u"strata/updates/checkAutomatically"_s, true ).toBool() );
  connect( automatic, &QCheckBox::toggled, this, []( bool enabled ) { QgsSettings().setValue( u"strata/updates/checkAutomatically"_s, enabled ); } );
  layout->addWidget( automatic );
  auto *status = new QLabel( this );
  status->setWordWrap( true );
  status->setTextFormat( Qt::PlainText );
  layout->addWidget( status );
  auto *notes = new QPlainTextEdit( this );
  notes->setReadOnly( true );
  layout->addWidget( notes );
  auto *progress = new QProgressBar( this );
  layout->addWidget( progress );
  auto *check = new QPushButton( tr( "Check for updates" ), this );
  layout->addWidget( check );
  auto *download = new QPushButton( tr( "Update" ), this );
  layout->addWidget( download );
  auto *cancel = new QPushButton( tr( "Cancel download" ), this );
  layout->addWidget( cancel );
  auto *install = new QPushButton( tr( "Install and restart" ), this );
  layout->addWidget( install );
  auto *service = QgsUpdateService::instance();
  const auto refresh = [=]() {
    status->setText( service->status() + ( service->size() > 0 ? tr( "\nDownload size: %1 MiB" ).arg( service->size() / 1024 / 1024 ) : QString() ) );
    notes->setPlainText( service->notes() );
    const auto state = service->state();
    check->setEnabled( state == "idle"_L1 || state == "error"_L1 || state == "current"_L1 || state == "available"_L1 );
    download->setVisible( state == "available"_L1 );
    install->setVisible( state == "ready"_L1 );
    cancel->setVisible( state == "downloading"_L1 || state == "checking"_L1 );
    progress->setVisible( state == "downloading"_L1 || state == "verifying"_L1 );
  };
  connect( service, &QgsUpdateService::changed, this, refresh );
  connect( service, &QgsUpdateService::progress, this, [progress]( qint64 current, qint64 total ) {
    progress->setRange( 0, total > 0 ? 100 : 0 );
    if ( total > 0 )
      progress->setValue( static_cast<int>( 100 * current / total ) );
  } );
  connect( check, &QPushButton::clicked, service, [service]() { service->check(); } );
  connect( download, &QPushButton::clicked, service, &QgsUpdateService::download );
  connect( cancel, &QPushButton::clicked, service, &QgsUpdateService::cancel );
  connect( install, &QPushButton::clicked, this, []() {
    if ( QgisApp::instance() )
      QgisApp::instance()->installStrataUpdate();
  } );
  refresh();
}
