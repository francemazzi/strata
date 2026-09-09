// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaidiscoverypreview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QVBoxLayout>

#include "moc_qgsaidiscoverypreview.cpp"

using namespace Qt::StringLiterals;
static QString displayLabel( const QString &id )
{
  static const QHash<QString, QString> labels {
    { u"identified"_s, QObject::tr( "Contenuto identificato" ) },
    { u"unverified"_s, QObject::tr( "Da verificare" ) },
    { u"rejected"_s, QObject::tr( "Non importabile" ) },
    { u"table"_s, QObject::tr( "Tabella: richiede collegamento geografico" ) },
    { u"unknown"_s, QObject::tr( "Non disponibile" ) },
    { u"covered"_s, QObject::tr( "Area coperta" ) },
    { u"partial"_s, QObject::tr( "Copertura parziale" ) },
    { u"incompatible"_s, QObject::tr( "Fuori area" ) },
    { u"boundary"_s, QObject::tr( "Confini" ) },
    { u"public_green"_s, QObject::tr( "Verde pubblico" ) },
    { u"trees"_s, QObject::tr( "Censimento arboreo" ) },
    { u"ortho"_s, QObject::tr( "Ortofoto" ) },
    { u"land_cover"_s, QObject::tr( "Uso del suolo / impermeabilità" ) },
    { u"canopy"_s, QObject::tr( "Copertura arborea" ) },
    { u"optical"_s, QObject::tr( "Immagini ottiche" ) },
    { u"thermal"_s, QObject::tr( "Immagini termiche" ) },
    { u"population"_s, QObject::tr( "Popolazione" ) },
    { u"accessible_green"_s, QObject::tr( "Verde accessibile" ) },
    { u"pedestrian"_s, QObject::tr( "Rete pedonale" ) },
    { u"constraints"_s, QObject::tr( "Vincoli" ) },
    { u"cadastre"_s, QObject::tr( "Catasto" ) },
    { u"roads"_s, QObject::tr( "Viabilità" ) },
    { u"basemap"_s, QObject::tr( "Carta di base" ) },
    { u"forest"_s, QObject::tr( "Foreste" ) },
    { u"protected"_s, QObject::tr( "Aree protette" ) },
    { u"publisher"_s, QObject::tr( "Editore" ) },
    { u"license"_s, QObject::tr( "Licenza" ) },
    { u"coverage"_s, QObject::tr( "Copertura" ) },
    { u"protocol"_s, QObject::tr( "Distribuzione" ) },
    { u"freshness"_s, QObject::tr( "Metadati aggiornati" ) }
  };
  return labels.value( id, id );
}
static QString joined( const QJsonArray &values )
{
  QStringList items;
  for ( const auto &v : values )
    items << displayLabel( v.toString() );
  return items.join( ", "_L1 );
}
QgsAiDiscoveryPreview::QgsAiDiscoveryPreview( const QJsonObject &plan, const QString &defaultDestination, QWidget *parent )
  : QWidget( parent )
{
  setObjectName( u"discoveryPreview"_s );
  auto layout = new QVBoxLayout( this );
  auto label = [&]( const QString &text ) {
    auto widget = new QLabel( text, this );
    widget->setTextFormat( Qt::PlainText );
    widget->setWordWrap( true );
    layout->addWidget( widget );
    return widget;
  };
  const auto coverage = plan.value( u"coverage"_s ).toObject();
  const auto profile = plan.value( u"profile"_s ).toObject();
  label( tr( "%1 · versione %2" ).arg( profile.value( u"title"_s ).toString() ).arg( profile.value( u"version"_s ).toInt() ) );
  label( tr( "Area verificata: %1\nRequisiti: %2\nLacune: %3\nRicerca: %4 crediti. Anteprima valida fino a %5." )
           .arg(
             plan.value( u"brief"_s ).toObject().value( u"aoi"_s ).toObject().value( u"name"_s ).toString( u"Extent esplicito EPSG:4326"_s ),
             joined( coverage.value( u"required"_s ).toArray() ),
             joined( coverage.value( u"missing"_s ).toArray() )
           )
           .arg( plan.value( u"budget"_s ).toObject().value( u"creditsCharged"_s ).toInt() )
           .arg( plan.value( u"expiresAt"_s ).toString() ) );
  const auto candidates = plan.value( u"candidates"_s ).toArray();
  const auto diagnostics = plan.value( u"diagnostics"_s ).toObject();
  if ( !diagnostics.isEmpty() )
  {
    int cached = 0;
    for ( const auto &harvest : diagnostics.value( u"harvests"_s ).toArray() )
      if ( harvest.toObject().value( u"result"_s ).toObject().value( u"cached"_s ).toBool() )
        ++cached;
    label(
      tr( "Ricerca: %1 candidati trovati, %2 verificati, %3 ricerche da cache.%4" )
        .arg( diagnostics.value( u"matched"_s ).toInt() )
        .arg( diagnostics.value( u"verified"_s ).toInt() )
        .arg( cached )
        .arg( diagnostics.value( u"catalogsIncomplete"_s ).toBool() || diagnostics.value( u"verificationLimitReached"_s ).toBool() ? tr( " Alcuni cataloghi o controlli sono incompleti." ) : QString() )
    );
  }
  QList<QCheckBox *> checks;
  QList<QComboBox *> modes;
  QList<QCheckBox *> consent;
  QList<QSpinBox *> byteLimits;
  for ( const auto &value : candidates )
  {
    const auto c = value.toObject();
    auto check = new QCheckBox( c.value( u"title"_s ).toString(), this );
    layout->addWidget( check );
    auto mode = new QComboBox( this );
    for ( const auto &m : c.value( u"modes"_s ).toArray() )
      if ( m != "reference"_L1 )
        mode->addItem( m == "file"_L1 ? tr( "Scarica file" ) : tr( "Collega servizio" ), m.toString() );
    check->setEnabled( mode->count() > 0 );
    if ( !mode->count() )
      mode->addItem( tr( "Riferimento da verificare" ) );
    layout->addWidget( mode );
    checks << check;
    modes << mode;
    const auto content = c.value( u"content"_s ).toObject();
    const bool uncertain = content.value( u"status"_s ) == "unverified"_L1 && mode->count() > 0 && check->isEnabled();
    auto allow = new QCheckBox( tr( "Autorizzo un tentativo sul contenuto non verificato" ), this );
    allow->setObjectName( u"discoveryContentConsent"_s );
    allow->setVisible( uncertain );
    layout->addWidget( allow );
    consent << allow;
    auto bytes = new QSpinBox( this );
    bytes->setObjectName( u"discoveryCandidateMiB"_s );
    bytes->setRange( 1, 2048 );
    bytes->setValue( 16 );
    bytes->setSuffix( tr( " MiB massimi per questo tentativo" ) );
    bytes->setVisible( uncertain );
    layout->addWidget( bytes );
    byteLimits << bytes;
    if ( !content.isEmpty() )
      label( tr( "Contenuto: %1 · %2\n%3" ).arg( displayLabel( content.value( u"kind"_s ).toString() ), displayLabel( content.value( u"status"_s ).toString() ), content.value( u"reason"_s ).toString() ) );
    if ( content.value( u"kind"_s ) == "table"_L1 || content.value( u"status"_s ) == "rejected"_L1 || c.value( u"spatialStatus"_s ) == "incompatible"_L1 )
      check->setEnabled( false );
    label( tr( "%1 · %2\n%3\nRequisiti: %4\n%5\nDimensione: %6 · tetto file: %7 crediti" )
             .arg(
               c.value( u"sourceName"_s ).toString(),
               c.value( u"license"_s ).toString( tr( "Licenza da verificare" ) ),
               c.value( u"distribution"_s ).toObject().value( u"url"_s ).toString(),
               joined( c.value( u"requirements"_s ).toArray() ),
               joined( c.value( u"reasons"_s ).toArray() ),
               c.contains( u"estimatedBytes"_s ) ? tr( "%1 MiB" ).arg( c.value( u"estimatedBytes"_s ).toDouble() / 1048576., 0, 'f', 1 ) : tr( "non disponibile" )
             )
             .arg( c.value( u"maxCredits"_s ).toInt() ) );
    const auto period = c.value( u"observationPeriod"_s ).toObject();
    label( tr( "Territorio: %1 · periodo del dato: %2 – %3\nModifica metadati: %4" )
             .arg(
               displayLabel( c.value( u"spatialStatus"_s ).toString( u"unknown"_s ) ),
               period.value( u"start"_s ).toString( tr( "non disponibile" ) ),
               period.value( u"end"_s ).toString( tr( "non disponibile" ) ),
               c.value( u"metadataUpdatedAt"_s ).toString( tr( "non disponibile" ) )
             ) );
    QStringList verification;
    for ( const auto &value : c.value( u"checks"_s ).toArray() )
    {
      const auto check = value.toObject();
      verification << tr( "%1: %2" ).arg( displayLabel( check.value( u"id"_s ).toString() ), check.value( u"passed"_s ) == true ? tr( "verificato" ) : check.value( u"reason"_s ).toString() );
    }
    label( verification.join( u" · "_s ) );
  }
  label( tr( "Destinazione nel workspace. Il limite include tutti i file selezionati; il kit può essere parziale. Nessuna riproiezione o clip raster automatica." ) );
  auto destination = new QLineEdit( defaultDestination, this );
  destination->setObjectName( u"discoveryDestination"_s );
  layout->addWidget( destination );
  auto budget = new QSpinBox( this );
  budget->setObjectName( u"discoveryAcquisitionBudget"_s );
  budget->setRange( 0, 1000000 );
  budget->setValue( 20 );
  budget->setSuffix( tr( " crediti massimi di acquisizione" ) );
  layout->addWidget( budget );
  auto selectedGaps = label( tr( "Nessun candidato selezionato." ) );
  selectedGaps->setObjectName( u"discoverySelectedGaps"_s );
  auto confirm = new QPushButton( tr( "Conferma selezione e acquisisci" ), this );
  confirm->setObjectName( u"discoveryConfirm"_s );
  layout->addWidget( confirm );
  const bool expired = QDateTime::fromString( plan.value( u"expiresAt"_s ).toString(), Qt::ISODateWithMs ) <= QDateTime::currentDateTimeUtc();
  confirm->setEnabled( false );
  auto update = [=]() {
    QSet<QString> covered;
    int count = 0;
    bool consentComplete = true;
    qint64 requestedBytes = 65536;
    double minimumCredits = 0;
    const int price = plan.value( u"budget"_s ).toObject().value( u"prices"_s ).toObject().value( u"datahub_extract"_s ).toInt( 1 );
    for ( int i = 0; i < checks.size(); ++i )
      if ( checks[i]->isChecked() )
      {
        ++count;
        const auto c = candidates[i].toObject();
        const QString mode = modes[i]->currentData().toString();
        const bool uncertain = c.value( u"content"_s ).toObject().value( u"status"_s ) == "unverified"_L1;
        if ( mode == "file"_L1 && uncertain && !consent[i]->isChecked() )
          consentComplete = false;
        if ( mode == "file"_L1 )
        {
          const qint64 bytes = uncertain ? static_cast<qint64>( byteLimits[i]->value() ) * 1048576 : c.value( u"estimatedBytes"_s ).toInteger( 1048576 );
          requestedBytes += bytes + 1024;
          minimumCredits += ( bytes + 1048575 ) / 1048576 * price;
        }
        for ( const auto &v : c.value( u"eligibleRequirementsByMode"_s ).toObject().value( mode ).toArray() )
          covered.insert( v.toString() );
      }
    QStringList missing;
    for ( const auto &v : coverage.value( u"required"_s ).toArray() )
      if ( !covered.contains( v.toString() ) )
        missing << displayLabel( v.toString() );
    selectedGaps->setText( tr( "Selezionati: %1 · lacune della selezione: %2" ).arg( count ).arg( missing.join( ", "_L1 ) ) );
    selectedGaps->setText( selectedGaps->text() + tr( " · stima per i file e i tentativi scelti: %1 crediti" ).arg( minimumCredits ) );
    confirm->setEnabled( count > 0 && !expired && consentComplete && minimumCredits <= budget->value() && requestedBytes <= 2147483648LL );
  };
  for ( auto check : checks )
    connect( check, &QCheckBox::toggled, this, update );
  for ( auto allow : consent )
    connect( allow, &QCheckBox::toggled, this, update );
  for ( auto mode : modes )
    connect( mode, &QComboBox::currentIndexChanged, this, update );
  for ( auto bytes : byteLimits )
    connect( bytes, &QSpinBox::valueChanged, this, update );
  connect( budget, &QSpinBox::valueChanged, this, update );
  connect( confirm, &QPushButton::clicked, this, [=, this]() {
    QJsonArray selection;
    for ( int i = 0; i < checks.size(); ++i )
      if ( checks[i]->isChecked() )
      {
        QJsonObject item { { u"candidateId"_s, candidates[i].toObject().value( u"id"_s ) }, { u"mode"_s, modes[i]->currentData().toString() } };
        if ( candidates[i].toObject().value( u"content"_s ).toObject().value( u"status"_s ) == "unverified"_L1 )
        {
          item.insert( u"allowUnverifiedContent"_s, consent[i]->isChecked() );
          item.insert( u"maxBytes"_s, static_cast<qint64>( byteLimits[i]->value() ) * 1048576 );
        }
        selection.append( item );
      }
    // Disable the whole card before emitting; no second confirmation or mutable selection after start.
    setEnabled( false );
    emit approved( selection, budget->value(), destination->text() );
  } );
  auto cancel = new QPushButton( tr( "Annulla" ), this );
  layout->addWidget( cancel );
  connect( cancel, &QPushButton::clicked, this, [this]() {
    setEnabled( false );
    emit canceled();
  } );
}
