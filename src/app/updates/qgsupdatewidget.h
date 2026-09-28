#pragma once
#include "qgsoptionswidgetfactory.h"
#include "qgis_app.h"
class APP_EXPORT QgsUpdateWidget : public QgsOptionsPageWidget
{
  public:
    explicit QgsUpdateWidget( QWidget *parent = nullptr );
    void apply() override {}
};
class QgsUpdateOptionsFactory : public QgsOptionsWidgetFactory
{
  public:
    QgsUpdateOptionsFactory()
      : QgsOptionsWidgetFactory( QObject::tr( "Updates" ), QIcon(), QStringLiteral( "strataUpdates" ) )
    {}
    QString pagePositionHint() const override { return QString(); }
    QgsOptionsPageWidget *createWidget( QWidget *parent = nullptr ) const override { return new QgsUpdateWidget( parent ); }
    QIcon icon() const override { return QIcon(); }
};
