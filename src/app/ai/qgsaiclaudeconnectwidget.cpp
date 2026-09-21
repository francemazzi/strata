/***************************************************************************
    qgsaiclaudeconnectwidget.h
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

#include "qgsaiclaudeconnectwidget.h"

#include "qgsaimodelrouter.h"
#include "qgscollapsiblegroupbox.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

#include "moc_qgsaiclaudeconnectwidget.cpp"

using namespace Qt::StringLiterals;

QgsAiClaudeConnectWidget::QgsAiClaudeConnectWidget( QgsAiModelRouter *router, QWidget *parent )
  : QWidget( parent )
{
  auto *layout = new QVBoxLayout( this );
  layout->setContentsMargins( 0, 0, 0, 0 );
  auto *notice = new QLabel( tr( "Claude subscription connections are temporarily suspended. You can use Strata Cloud or an Anthropic API key." ), this );
  notice->setWordWrap( true );
  notice->setObjectName( u"aiClaudeSuspensionNotice"_s );
  layout->addWidget( notice );
  auto *cloud = new QPushButton( tr( "Sign in to Strata Cloud" ), this );
  cloud->setObjectName( u"aiClaudeCloudButton"_s );
  layout->addWidget( cloud );
  connect( cloud, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::cloudRequested );
  auto *advanced = new QgsCollapsibleGroupBox( tr( "Configure an API key (advanced)" ), this );
  advanced->setSaveCollapsedState( false );
  advanced->setCollapsed( !router->hasStoredApiKey( QgsAiModelRouter::Provider::Claude ) );
  layout->addWidget( advanced );
  auto *form = new QVBoxLayout( advanced );
  auto *billing = new QLabel( tr( "Anthropic API usage is billed separately from a Claude subscription." ), advanced );
  billing->setWordWrap( true );
  form->addWidget( billing );
  mApiKeyEdit = new QLineEdit( advanced );
  mApiKeyEdit->setEchoMode( QLineEdit::Password );
  mApiKeyEdit->setObjectName( u"aiClaudeApiKeyLineEdit"_s );
  mApiKeyEdit->setPlaceholderText( tr( "API key — leave empty to keep the saved key" ) );
  form->addWidget( mApiKeyEdit );
  mModelCombo = new QComboBox( advanced );
  mModelCombo->setEditable( true );
  mModelCombo->addItems( { u"claude-sonnet-5"_s, u"claude-opus-4-8"_s, u"claude-haiku-4-5"_s } );
  mModelCombo->setCurrentText( router->providerSettings( QgsAiModelRouter::Provider::Claude ).model );
  form->addWidget( mModelCombo );
  auto *use = new QPushButton( tr( "Use in this chat" ), advanced );
  use->setObjectName( u"aiClaudeUseButton"_s );
  form->addWidget( use );
  connect( use, &QPushButton::clicked, this, &QgsAiClaudeConnectWidget::useRequested );
}
QString QgsAiClaudeConnectWidget::modelText() const
{
  return mModelCombo->currentText().trimmed();
}
QString QgsAiClaudeConnectWidget::pendingApiKey() const
{
  return mApiKeyEdit->text().trimmed();
}
