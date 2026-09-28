/***************************************************************************
    qgsaiclaudeoautherror.h
    ---------------------
    begin                : September 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************/

#ifndef QGSAICLAUDEOAUTHERROR_H
#define QGSAICLAUDEOAUTHERROR_H

#include "qgis_app.h"

#include <QByteArray>
#include <QMetaType>
#include <QString>

struct APP_EXPORT QgsAiClaudeOAuthError
{
    enum class Category
    {
      None,
      AuthorizationDenied,
      StateMismatch,
      RateLimited,
      Network,
      TokenExchange,
      InvalidResponse,
      CredentialStorage,
      Timeout,
      BrowserLaunch,
      InvalidRequest
    };

    Category category = Category::None;
    QString userMessage;
    QString technicalDetail;
    QString providerCode;
    int httpStatus = 0;
    qint64 retryAfterSeconds = -1;
};

Q_DECLARE_METATYPE( QgsAiClaudeOAuthError )

class APP_EXPORT QgsAiClaudeOAuthErrorParser
{
  public:
    static QgsAiClaudeOAuthError fromTokenReply( int httpStatus, const QByteArray &body, const QByteArray &retryAfter, bool timedOut, const QString &networkDetail );
    static qint64 parseRetryAfter( const QByteArray &value );
    static QString safeDetail( const QString &value );
};

#endif // QGSAICLAUDEOAUTHERROR_H
