/***************************************************************************
  qgsaiiconbutton.h — palette-aware desktop actions
  SPDX-License-Identifier: GPL-2.0-or-later
 ***************************************************************************/
#ifndef QGSAIICONBUTTON_H
#define QGSAIICONBUTTON_H

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>

namespace QgsAiIconButton
{
  enum class Symbol
  {
    Copy,
    Edit,
    NewChat,
    History,
    More,
    Check
  };

  class Engine final : public QIconEngine
  {
    public:
      explicit Engine( Symbol symbol )
        : mSymbol( symbol )
      {}
      QIconEngine *clone() const override { return new Engine( mSymbol ); }
      void paint( QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State ) override
      {
        const QColor color = QApplication::palette().color( mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active, QPalette::WindowText );
        QString shape;
        switch ( mSymbol )
        {
          case Symbol::Copy:
            shape = QStringLiteral( "<rect x='8' y='8' width='12' height='13' rx='2'/><path d='M16 8V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h3'/>" );
            break;
          case Symbol::Edit:
            shape = QStringLiteral( "<path d='m4 16-1 5 5-1L21 7l-4-4ZM14 6l4 4'/>" );
            break;
          case Symbol::NewChat:
            shape = QStringLiteral( "<path d='M12 4H5a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2h13a2 2 0 0 0 2-2v-7M15 3h6m-3-3v6M8 10h4m-4 5h8'/>" );
            break;
          case Symbol::History:
            shape = QStringLiteral( "<path d='M3 10a9 9 0 1 1 2 8M3 4v6h6m3-4v6l4 2'/>" );
            break;
          case Symbol::More:
            shape = QStringLiteral( "<circle cx='4' cy='12' r='1'/><circle cx='12' cy='12' r='1'/><circle cx='20' cy='12' r='1'/>" );
            break;
          case Symbol::Check:
            shape = QStringLiteral( "<path d='m4 12 5 5L20 6'/>" );
            break;
        }
        QSvgRenderer svg( QStringLiteral( "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%1' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>%2</svg>" )
                            .arg( color.name(), shape )
                            .toUtf8() );
        svg.render( painter, QRectF( rect ) );
      }
      QPixmap pixmap( const QSize &size, QIcon::Mode mode, QIcon::State state ) override
      {
        QPixmap result( size );
        result.fill( Qt::transparent );
        QPainter painter( &result );
        paint( &painter, QRect( QPoint(), size ), mode, state );
        return result;
      }

    private:
      Symbol mSymbol;
  };

  inline QIcon icon( Symbol symbol )
  {
    return QIcon( new Engine( symbol ) );
  }

  inline void configure( QToolButton *button, Symbol symbol, const QString &label, const QString &tip = QString() )
  {
    button->setIcon( icon( symbol ) );
    button->setText( label );
    button->setAccessibleName( label );
    button->setToolTip( tip.isEmpty() ? label : tip );
    button->setToolButtonStyle( Qt::ToolButtonIconOnly );
    button->setIconSize( QSize( 18, 18 ) );
    button->setMinimumSize( 28, 28 );
    button->setFocusPolicy( Qt::StrongFocus );
    button->setAutoRaise( true );
    button->setStyleSheet( QStringLiteral(
      "QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 4px; background: transparent; }"
      "QToolButton:hover, QToolButton:pressed { background: palette(alternate-base); }"
      "QToolButton:focus { border-color: palette(highlight); }"
      "QToolButton::menu-indicator { image: none; width: 0; }"
    ) );
  }

  inline void copied( QToolButton *button )
  {
    const int generation = button->property( "copyGeneration" ).toInt() + 1;
    button->setProperty( "copyGeneration", generation );
    button->setIcon( icon( Symbol::Check ) );
    QTimer::singleShot( 1400, button, [button, generation]() {
      if ( button->property( "copyGeneration" ).toInt() == generation )
        button->setIcon( icon( Symbol::Copy ) );
    } );
  }
} //namespace QgsAiIconButton
#endif
