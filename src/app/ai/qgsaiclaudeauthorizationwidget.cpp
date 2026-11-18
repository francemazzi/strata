/***************************************************************************
    qgsaiclaudeauthorizationwidget.cpp
    --------------------------------
    begin                : November 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************/
/*                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsaiclaudeauthorizationwidget.h"
#include "qgsaiclaudecodeauth.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>
#include <utility>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <shellapi.h>
#endif

#include "moc_qgsaiclaudeauthorizationwidget.cpp"

using namespace Qt::StringLiterals;

namespace
{
  bool openAuthorizationUrl( const QUrl &url )
  {
    // The Qt Windows and Unix URL handlers can log the entire URL on failure.
#if defined( Q_OS_WIN )
    const QString encoded = url.toString( QUrl::FullyEncoded );
    SHELLEXECUTEINFOW request {};
    request.cbSize = sizeof( request );
    request.fMask = SEE_MASK_FLAG_NO_UI;
    request.lpVerb = L"open";
    request.lpFile = reinterpret_cast<const wchar_t *>( encoded.utf16() );
    request.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW( &request ) != FALSE;
#elif defined( Q_OS_MACOS )
    return QDesktopServices::openUrl( url );
#else
    QProcess opener;
    opener.setProgram( u"xdg-open"_s );
    opener.setArguments( { url.toString( QUrl::FullyEncoded ) } );
    opener.setStandardOutputFile( QProcess::nullDevice() );
    opener.setStandardErrorFile( QProcess::nullDevice() );
    return opener.startDetached();
#endif
  }
} //namespace

QgsAiClaudeAuthorizationWidget::QgsAiClaudeAuthorizationWidget( QgsAiClaudeCodeAuth *auth, QWidget *parent, std::function<bool( const QUrl & )> openUrl )
  : QWidget( parent )
{
  if ( !openUrl )
    openUrl = openAuthorizationUrl;

  auto *layout = new QVBoxLayout( this );
  layout->setContentsMargins( 0, 0, 0, 0 );
  auto *hint = new QLabel( this );
  hint->setObjectName( u"aiClaudeAuthorizationHint"_s );
  hint->setWordWrap( true );
  layout->addWidget( hint );

  auto *link = new QLineEdit( this );
  link->setObjectName( u"aiClaudeAuthorizationLink"_s );
  link->setAccessibleName( tr( "Claude authorization link" ) );
  link->setReadOnly( true );
  layout->addWidget( link );

  auto *actions = new QHBoxLayout;
  auto *open = new QPushButton( tr( "Open authorization page" ), this );
  open->setObjectName( u"aiClaudeOpenAuthorizationButton"_s );
  open->setAutoDefault( false );
  actions->addWidget( open );
  auto *copy = new QPushButton( tr( "Copy link" ), this );
  copy->setObjectName( u"aiClaudeCopyAuthorizationButton"_s );
  copy->setAutoDefault( false );
  actions->addWidget( copy );
  actions->addStretch();
  layout->addLayout( actions );

  auto *codeRow = new QHBoxLayout;
  auto *code = new QLineEdit( this );
  code->setObjectName( u"aiClaudeAuthorizationCode"_s );
  code->setAccessibleName( tr( "Claude authorization code" ) );
  code->setPlaceholderText( tr( "Paste the authorization code if shown" ) );
  code->setEchoMode( QLineEdit::Password );
  code->setMaxLength( 4096 );
  codeRow->addWidget( code, 1 );
  auto *submit = new QPushButton( tr( "Continue sign-in" ), this );
  submit->setObjectName( u"aiClaudeSubmitAuthorizationButton"_s );
  submit->setAutoDefault( false );
  submit->setEnabled( false );
  codeRow->addWidget( submit );
  layout->addLayout( codeRow );

  connect( auth, &QgsAiClaudeCodeAuth::authorizationUrlChanged, this, [this, hint, link, code]( const QUrl &url ) {
    mUrl = url;
    link->setText( url.toString( QUrl::FullyEncoded ) );
    link->setCursorPosition( 0 );
    code->clear();
    code->setEnabled( !url.isEmpty() );
    hint->setText( tr( "If the browser did not open, open or copy this link. Paste the code below if Claude shows one." ) );
    setVisible( !url.isEmpty() );
  } );
  connect( open, &QPushButton::clicked, this, [this, hint, openUrl = std::move( openUrl )]() {
    if ( !mUrl.isEmpty() && !openUrl( mUrl ) )
      hint->setText( tr( "Could not open the browser. Copy the link and open it manually." ) );
  } );
  connect( copy, &QPushButton::clicked, this, [this, hint, link]() {
    if ( mUrl.isEmpty() )
      return;
    QApplication::clipboard()->setText( link->text() );
    hint->setText( tr( "Link copied. Paste it into your browser." ) );
  } );
  connect( code, &QLineEdit::textChanged, this, [submit]( const QString &text ) { submit->setEnabled( !text.trimmed().isEmpty() ); } );
  connect( code, &QLineEdit::returnPressed, submit, &QPushButton::click );
  connect( submit, &QPushButton::clicked, auth, [auth, code, hint]() {
    if ( auth->submitAuthorizationCode( code->text() ) )
    {
      code->clear();
      code->setEnabled( false );
      hint->setText( tr( "Code sent to Claude Code. Waiting for sign-in to finish…" ) );
    }
    else
    {
      hint->setText( tr( "Could not submit the code. Check that the sign-in is still active and paste the code shown by Claude." ) );
    }
  } );
  hide();
}
