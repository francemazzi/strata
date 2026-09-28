/***************************************************************************
    qgsaiclaudeoautherror.cpp
    ---------------------
    begin                : September 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************/

#include "qgsaiclaudeoautherror.h"

#include <algorithm>

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

QString QgsAiClaudeOAuthErrorParser::safeDetail( const QString &value )
{
  QString result = value;
  result.replace( QRegularExpression( u"[\\x00-\\x1f\\x7f]+"_s ), u" "_s );
  result.replace( QRegularExpression( u"sk-ant-[A-Za-z0-9_-]+"_s ), u"[REDACTED]"_s );
  result.replace( QRegularExpression( u"(Bearer\\s+)[^\\s]+"_s, QRegularExpression::CaseInsensitiveOption ), u"\\1[REDACTED]"_s );
  result = result.simplified();
  return result.left( 240 );
}

qint64 QgsAiClaudeOAuthErrorParser::parseRetryAfter( const QByteArray &value )
{
  bool secondsOk = false;
  const qint64 seconds = value.trimmed().toLongLong( &secondsOk );
  if ( secondsOk )
    return std::max<qint64>( 0, seconds );

  const QDateTime date = QDateTime::fromString( QString::fromLatin1( value ), Qt::RFC2822Date );
  if ( !date.isValid() )
    return -1;
  return std::max<qint64>( 0, QDateTime::currentDateTimeUtc().secsTo( date.toUTC() ) );
}

QgsAiClaudeOAuthError QgsAiClaudeOAuthErrorParser::fromTokenReply( int httpStatus, const QByteArray &body, const QByteArray &retryAfter, bool timedOut, const QString &networkDetail )
{
  QgsAiClaudeOAuthError result;
  result.httpStatus = httpStatus;
  result.retryAfterSeconds = parseRetryAfter( retryAfter );

  const QJsonDocument document = QJsonDocument::fromJson( body );
  const QJsonObject root = document.isObject() ? document.object() : QJsonObject();
  const QJsonObject nested = root.value( u"error"_s ).toObject();
  result.providerCode = safeDetail( nested.value( u"type"_s ).toString() );
  if ( result.providerCode.isEmpty() )
    result.providerCode = safeDetail( root.value( u"error"_s ).toString() );

  QString providerMessage = nested.value( u"message"_s ).toString();
  if ( providerMessage.isEmpty() )
    providerMessage = root.value( u"error_description"_s ).toString();
  providerMessage = safeDetail( providerMessage );

  if ( httpStatus == 429 || result.providerCode == "rate_limit_error"_L1 )
  {
    result.category = QgsAiClaudeOAuthError::Category::RateLimited;
    result.userMessage = QObject::tr( "Claude is temporarily limiting login attempts. Return to Strata and try again later." );
  }
  else if ( timedOut )
  {
    result.category = QgsAiClaudeOAuthError::Category::Timeout;
    result.userMessage = QObject::tr( "Claude did not complete the connection in time. Try again from Strata." );
  }
  else if ( httpStatus == 0 )
  {
    result.category = QgsAiClaudeOAuthError::Category::Network;
    result.userMessage = QObject::tr( "Strata could not reach Claude to finish the connection." );
  }
  else
  {
    result.category = QgsAiClaudeOAuthError::Category::TokenExchange;
    result.userMessage = QObject::tr( "Claude approved the request, but Strata could not finish the connection." );
  }

  QStringList detailParts;
  if ( httpStatus > 0 )
    detailParts << u"HTTP %1"_s.arg( httpStatus );
  if ( !result.providerCode.isEmpty() )
    detailParts << result.providerCode;
  if ( !providerMessage.isEmpty() )
    detailParts << providerMessage;
  if ( detailParts.isEmpty() && !networkDetail.isEmpty() )
    detailParts << safeDetail( networkDetail );
  result.technicalDetail = detailParts.join( u" · "_s );
  return result;
}
