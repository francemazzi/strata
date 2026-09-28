/***************************************************************************
    testqgsaiclaudeoauthhelpers.cpp
    ---------------------
    begin                : September 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************/

#include "ai/qgsaiclaudeoautherror.h"
#include "ai/qgsaioauthcallbackpage.h"
#include "qgstest.h"

using namespace Qt::StringLiterals;

class TestQgsAiClaudeOAuthHelpers : public QObject
{
    Q_OBJECT

  private slots:
    void parsesRetryAfterAndInvalidBodies()
    {
      QCOMPARE( QgsAiClaudeOAuthErrorParser::parseRetryAfter( "12" ), 12 );
      QCOMPARE( QgsAiClaudeOAuthErrorParser::parseRetryAfter( "not-a-date" ), -1 );
      const QgsAiClaudeOAuthError rateLimited
        = QgsAiClaudeOAuthErrorParser::fromTokenReply( 429, QByteArrayLiteral( R"({"error":{"type":"rate_limit_error","message":"Rate limited"}})" ), {}, false, QString() );
      QCOMPARE( rateLimited.category, QgsAiClaudeOAuthError::Category::RateLimited );
      QCOMPARE( rateLimited.retryAfterSeconds, -1 );
      const QgsAiClaudeOAuthError error = QgsAiClaudeOAuthErrorParser::fromTokenReply( 503, QByteArrayLiteral( "not-json secret-value" ), {}, false, QString() );
      QCOMPARE( error.category, QgsAiClaudeOAuthError::Category::TokenExchange );
      QVERIFY( !error.technicalDetail.contains( u"secret-value"_s ) );
      QCOMPARE( QgsAiClaudeOAuthErrorParser::safeDetail( u"Bearer abc123 sk-ant-api03-secret"_s ), u"Bearer [REDACTED] [REDACTED]"_s );
    }

    void callbackPageEscapesContentAndSetsSecurityHeaders()
    {
      const QByteArray body = QgsAiOAuthCallbackPage::render( QgsAiOAuthCallbackPage::State::Error, u"<title>"_s, u"<message>"_s, u"<detail>"_s );
      QVERIFY( body.contains( "&lt;title&gt;" ) );
      QVERIFY( body.contains( "&lt;message&gt;" ) );
      QVERIFY( body.contains( "&lt;detail&gt;" ) );
      QVERIFY( !body.contains( "setTimeout" ) );
      const QByteArray response = QgsAiOAuthCallbackPage::httpResponse( 400, body );
      QVERIFY( response.contains( "Cache-Control: no-store" ) );
      QVERIFY( response.contains( "Content-Security-Policy: default-src 'none'" ) );
      QVERIFY( response.contains( "img-src data:" ) );
      QVERIFY( response.contains( "X-Content-Type-Options: nosniff" ) );
    }
};

QGSTEST_MAIN( TestQgsAiClaudeOAuthHelpers )
#include "testqgsaiclaudeoauthhelpers.moc"
