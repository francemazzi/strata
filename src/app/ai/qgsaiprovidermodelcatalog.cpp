/***************************************************************************
    qgsaiprovidermodelcatalog.cpp
    ---------------------
    begin                : November 2026
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

#include "qgsaiprovidermodelcatalog.h"

#include "qgsaiclaudecodeauth.h"
#include "qgsaicodexoauthclient.h"
#include "qgsnetworkaccessmanager.h"

#include <QCollator>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QSignalBlocker>
#include <QUrlQuery>

#include <algorithm>

#include "moc_qgsaiprovidermodelcatalog.cpp"

using namespace Qt::StringLiterals;

namespace
{
  constexpr int REFRESH_MS = 5 * 60 * 1000;
  constexpr int RETRY_MS = 15 * 1000;
  constexpr int TIMEOUT_MS = 8000;
  constexpr int MAX_BYTES = 4 * 1024 * 1024;
} //namespace

QgsAiProviderModelCatalog::QgsAiProviderModelCatalog( Source source, QObject *parent )
  : QObject( parent )
  , mSource( source )
{}

void QgsAiProviderModelCatalog::reset()
{
  ++mGeneration;
  if ( mReply )
    mReply->abort();
  if ( mProcess )
  {
    mProcess->disconnect( this );
    mProcess->kill();
    mProcess->deleteLater();
    mProcess = nullptr;
  }
  mModels.clear();
  mCredentialFingerprint.clear();
  mStatus.clear();
  mLastAttempt.invalidate();
  mLoading = false;
  emit changed();
}

void QgsAiProviderModelCatalog::refresh( const QString &apiKey, bool force )
{
  if ( mSource == Source::Codex && mSessionRevision != QgsAiCodexOAuthClient::sessionRevision() )
  {
    mSessionRevision = QgsAiCodexOAuthClient::sessionRevision();
    reset();
  }
  const bool api = mSource == Source::OpenAi || mSource == Source::Anthropic;
  const QByteArray fingerprint = api && !apiKey.isEmpty() ? QCryptographicHash::hash( apiKey.toUtf8(), QCryptographicHash::Sha256 ) : QByteArray();
  if ( fingerprint != mCredentialFingerprint )
  {
    reset();
    mCredentialFingerprint = fingerprint;
  }
  if ( ( api && apiKey.isEmpty() ) || ( mSource == Source::Codex && !QgsAiCodexOAuthClient::hasRefreshToken() ) || ( mSource == Source::ClaudeCode && !QgsAiClaudeCodeAuth::isConfigured() ) )
  {
    if ( force )
      finish( {}, api ? tr( "Save an API key to load its model list." ) : tr( "Connect your account to load its model list." ) );
    return;
  }
  if ( mLoading || ( !force && mLastAttempt.isValid() && mLastAttempt.elapsed() < ( mStatus.isEmpty() ? REFRESH_MS : RETRY_MS ) ) )
    return;

  mLoading = true;
  mLastAttempt.start();
  emit changed();
  if ( mSource == Source::ClaudeCode )
  {
    refreshClaudeCode();
    return;
  }

  const int generation = mGeneration;
  QTimer::singleShot( 0, this, [this, generation, apiKey]() {
    if ( generation != mGeneration )
      return;
    QNetworkRequest request;
    request.setTransferTimeout( TIMEOUT_MS );
    request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy );
    request.setAttribute( QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual );
    request.setAttribute( QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual );
    request.setRawHeader( "Accept", "application/json" );
    if ( mSource == Source::Codex )
    {
      const QPointer<QgsAiProviderModelCatalog> guard( this );
      QgsAiCodexOAuthClient::TokenSet tokens;
      const bool authorized = QgsAiCodexOAuthClient::refreshAccessToken( tokens );
      if ( !guard || generation != mGeneration )
        return;
      if ( mSessionRevision != QgsAiCodexOAuthClient::sessionRevision() )
      {
        reset();
        return;
      }
      if ( !authorized )
      {
        finish( {}, tr( "Could not load the ChatGPT model list. Check your Codex connection." ) );
        return;
      }
      // Match the catalog route and client metadata of the Codex inference transport.
      QUrl url( u"https://chatgpt.com/backend-api/codex/models"_s );
      QUrlQuery query;
      query.addQueryItem( u"client_version"_s, QgsAiCodexOAuthClient::protocolVersion() );
      url.setQuery( query );
      request.setUrl( url );
      QgsAiCodexOAuthClient::applySessionHeaders( request, tokens );
    }
    else if ( mSource == Source::Anthropic )
    {
      if ( apiKey.startsWith( "sk-ant-oat"_L1 ) )
      {
        finish( {}, tr( "Use an Anthropic API key to load this catalog." ) );
        return;
      }
      request.setUrl( QUrl( u"https://api.anthropic.com/v1/models?limit=1000"_s ) );
      request.setRawHeader( "x-api-key", apiKey.toUtf8() );
      request.setRawHeader( "anthropic-version", "2026-10-21" );
    }
    else
    {
      request.setUrl( QUrl( u"https://api.openai.com/v1/models"_s ) );
      request.setRawHeader( "Authorization", "Bearer " + apiKey.toUtf8() );
    }

    auto *reply = QgsNetworkAccessManager::instance()->get( request );
    mReply = reply;
    connect( reply, &QNetworkReply::readyRead, reply, [reply]() {
      if ( reply->bytesAvailable() > MAX_BYTES )
        reply->abort();
    } );
    QTimer::singleShot( TIMEOUT_MS, reply, [reply]() {
      if ( reply->isRunning() )
        reply->abort();
    } );
    connect( reply, &QNetworkReply::finished, reply, &QObject::deleteLater );
    connect( reply, &QNetworkReply::finished, this, [this, reply, generation]() {
      if ( generation != mGeneration )
        return;
      mReply = nullptr;
      if ( mSource == Source::Codex && mSessionRevision != QgsAiCodexOAuthClient::sessionRevision() )
      {
        reset();
        return;
      }
      const int code = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
      if ( code < 200 || code >= 300 || reply->error() != QNetworkReply::NoError || reply->bytesAvailable() > MAX_BYTES )
      {
        if ( code == 401 || code == 403 )
          mModels.clear();
        finish( {}, tr( "Could not refresh models. You can still enter a model ID." ) );
        return;
      }
      const QJsonObject response = QJsonDocument::fromJson( reply->readAll() ).object();
      const QJsonValue entries = response.value( mSource == Source::Codex ? u"models"_s : u"data"_s );
      if ( !entries.isArray() )
      {
        finish( {}, tr( "Could not read the model list. You can still enter a model ID." ) );
        return;
      }
      finish( parseModels( mSource, entries.toArray() ) );
    } );
  } );
}

void QgsAiProviderModelCatalog::refreshClaudeCode()
{
  auto *process = new QProcess( this );
  mProcess = process;
  auto *directory = new QTemporaryDir();
  connect( process, &QObject::destroyed, [directory]() { delete directory; } );
  if ( !directory->isValid() )
  {
    mProcess = nullptr;
    process->deleteLater();
    finish( {} );
    return;
  }
  process->setWorkingDirectory( directory->path() );
  process->setProcessEnvironment( QgsAiClaudeCodeAuth::environment() );
  process->setStandardErrorFile( QProcess::nullDevice() );
  const auto complete = [this, process]( const QList<Model> &models ) {
    if ( mProcess != process )
      return;
    mProcess = nullptr;
    process->disconnect( this );
    if ( process->state() != QProcess::NotRunning )
      process->kill();
    process->deleteLater();
    finish( models );
  };

  // Use the same initialize handshake as supportedModels() in the Claude Agent
  // SDK. No user message or project context is written to stdin.
  connect( process, &QProcess::started, this, [process]() {
    process->write(
      R"({"type":"control_request","request_id":"strata-models","request":{"subtype":"initialize"}})"
      "\n"
    );
  } );
  connect( process, &QProcess::readyReadStandardOutput, this, [process, complete]() {
    if ( process->bytesAvailable() > MAX_BYTES )
    {
      complete( {} );
      return;
    }
    while ( process->canReadLine() )
    {
      const QJsonObject event = QJsonDocument::fromJson( process->readLine() ).object();
      const QJsonObject response = event.value( u"response"_s ).toObject();
      if ( event.value( u"type"_s ) != "control_response"_L1 || response.value( u"request_id"_s ) != "strata-models"_L1 )
        continue;
      complete( response.value( u"subtype"_s ) == "success"_L1 ? parseModels( Source::ClaudeCode, response.value( u"response"_s ).toObject().value( u"models"_s ).toArray() ) : QList<Model>() );
      return;
    }
  } );
  connect( process, &QProcess::errorOccurred, this, [complete]( QProcess::ProcessError ) { complete( {} ); } );
  connect( process, qOverload<int, QProcess::ExitStatus>( &QProcess::finished ), this, [complete]( int, QProcess::ExitStatus ) { complete( {} ); } );
  QTimer::singleShot( TIMEOUT_MS, process, [complete]() { complete( {} ); } );
  process->start(
    QgsAiClaudeCodeAuth::executablePath(),
    { u"--print"_s,
      u"--verbose"_s,
      u"--input-format"_s,
      u"stream-json"_s,
      u"--output-format"_s,
      u"stream-json"_s,
      u"--restricted"_s,
      u"--no-chrome"_s,
      u"--no-session-persistence"_s,
      u"--setting-sources"_s,
      QString(),
      u"--tools"_s,
      QString(),
      u"--strict-mcp-config"_s,
      u"--settings"_s,
      uR"({"disableAllHooks":true,"disableClaudeAiMcpServers":true})"_s }
  );
}

void QgsAiProviderModelCatalog::finish( const QList<Model> &models, const QString &error )
{
  mLoading = false;
  if ( error.isEmpty() )
    mModels = models;
  mStatus = !error.isEmpty() ? error : models.isEmpty() ? tr( "No models returned. You can still enter a model ID." ) : QString();
  emit changed();
}

QList<QgsAiProviderModelCatalog::Model> QgsAiProviderModelCatalog::parseModels( Source source, const QJsonArray &entries )
{
  QList<Model> models;
  QSet<QString> seen;
  for ( const auto &entry : entries )
  {
    const QJsonObject model = entry.toObject();
    const QString id = model.value( source == Source::Codex ? u"slug"_s : source == Source::ClaudeCode ? u"value"_s : u"id"_s ).toString().trimmed();
    if ( id.isEmpty() || seen.contains( id ) )
      continue;
    if ( source == Source::Codex && model.value( u"visibility"_s ) != "list"_L1 )
      continue;
    if ( source == Source::Anthropic && ( model.value( u"lifecycle"_s ) == "retired"_L1 || model.value( u"lifecycle"_s ) == "deprecated"_L1 ) )
      continue;
    if ( source == Source::OpenAi )
    {
      // The public API also lists image, audio, realtime and embedding models.
      static const QRegularExpression chatFamily( u"^(gpt-|chatgpt-|o[0-9])"_s );
      static const QRegularExpression otherModality( u"(^|-)(audio|realtime|image|transcribe|instruct|search|tts|live)(-|$)"_s );
      if ( !chatFamily.match( id ).hasMatch() || otherModality.match( id ).hasMatch() )
        continue;
    }
    const QString label = model.value( source == Source::ClaudeCode ? u"displayName"_s : u"display_name"_s ).toString().trimmed();
    models.append( Model { id, label.isEmpty() ? id : label } );
    seen.insert( id );
  }
  if ( source == Source::OpenAi )
  {
    QCollator collator;
    collator.setNumericMode( true );
    std::stable_sort( models.begin(), models.end(), [&collator]( const Model &left, const Model &right ) { return collator.compare( left.id, right.id ) > 0; } );
  }
  return models;
}

QWidget *QgsAiProviderModelCatalog::createPicker( QComboBox *&picker, const QString &value, const QString &docsUrl, std::function<void()> refresh, QWidget *parent, const QString &automaticLabel )
{
  auto *widget = new QWidget( parent );
  auto *layout = new QVBoxLayout( widget );
  layout->setContentsMargins( 0, 0, 0, 0 );
  picker = new QComboBox( widget );
  picker->setEditable( true );
  picker->setInsertPolicy( QComboBox::NoInsert );
  picker->setSizeAdjustPolicy( QComboBox::AdjustToMinimumContentsLengthWithIcon );
  picker->setMinimumContentsLength( 24 );
  picker->setCompleter( nullptr );
  picker->lineEdit()->setPlaceholderText( tr( "Select a model or paste its ID" ) );
  picker->setEditText( value );
  layout->addWidget( picker );
  auto *actions = new QHBoxLayout();
  layout->addLayout( actions );
  auto *button = new QPushButton( tr( "Refresh models" ), widget );
  actions->addWidget( button );
  auto *link = new QLabel( u"<a href=\"%1\">%2</a>"_s.arg( docsUrl, tr( "Model IDs" ) ), widget );
  link->setOpenExternalLinks( true );
  actions->addWidget( link );
  actions->addStretch();
  auto *label = new QLabel( widget );
  label->setWordWrap( true );
  layout->addWidget( label );
  connect( button, &QPushButton::clicked, widget, std::move( refresh ) );
  const auto update = [this, picker, button, label, automaticLabel]() {
    button->setEnabled( !loading() );
    label->setText( loading() ? tr( "Loading models…" ) : status() );
    label->setVisible( !label->text().isEmpty() );
    if ( !loading() )
      fillPicker( picker, models(), automaticLabel );
  };
  connect( this, &QgsAiProviderModelCatalog::changed, widget, update );
  update();
  return widget;
}

void QgsAiProviderModelCatalog::fillPicker( QComboBox *picker, const QList<Model> &models, const QString &automaticLabel )
{
  const QSignalBlocker blocker( picker );
  const QString value = pickerValue( picker );
  picker->clear();
  if ( !automaticLabel.isEmpty() )
    picker->addItem( automaticLabel, QString() );
  for ( const Model &model : models )
    picker->addItem( model.label, model.id );
  const int index = picker->findData( value );
  picker->setCurrentIndex( index );
  if ( index < 0 )
    picker->setEditText( value );
}

QString QgsAiProviderModelCatalog::pickerValue( const QComboBox *picker )
{
  const QString text = picker->currentText().trimmed();
  const int index = picker->currentIndex();
  return index >= 0 && text == picker->itemText( index ) ? picker->itemData( index ).toString() : text;
}
