/***************************************************************************
    qgsaioauthcallbackpage.h
    ---------------------
    begin                : September 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************/

#ifndef QGSAIOAUTHCALLBACKPAGE_H
#define QGSAIOAUTHCALLBACKPAGE_H

#include "qgis_app.h"

#include <QByteArray>
#include <QString>

class APP_EXPORT QgsAiOAuthCallbackPage
{
  public:
    enum class State
    {
      Success,
      Error,
      RateLimited
    };

    static QByteArray render( State state, const QString &title, const QString &message, const QString &detail = QString() );
    static QByteArray httpResponse( int status, const QByteArray &body );
};

#endif // QGSAIOAUTHCALLBACKPAGE_H
