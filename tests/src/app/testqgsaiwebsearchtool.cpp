/***************************************************************************
  testqgsaiwebsearchtool.cpp
  --------------------------
  begin                : September 2026
***************************************************************************/

#include "qgsaimodelrouter.h"
#include "qgsaisecretstore.h"
#include "qgsaisecretstoretestutils.h"
#include "qgsaitaskrunner.h"
#include "qgsaitestloopbackserver.h"
#include "qgsaiwebsearchtool.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <QTimer>

using namespace Qt::StringLiterals;

namespace
{
  bool configurePlanForLoopback( QgsAiModelRouter &router, quint16 port )
  {
    QString error;
    if ( !router.setPlanSessionToken( u"strata-plan-web-search-test-token"_s, &error ) )
      return false;

    QgsAiModelRouter::ProviderSettings settings = router.providerSettings( QgsAiModelRouter::Provider::Plan );
    settings.endpoint = u"http://127.0.0.1:%1/ai/messages"_s.arg( port );
    settings.model = u"managed-plan"_s;
    settings.enabled = true;
    router.setProviderSettings( QgsAiModelRouter::Provider::Plan, settings );
    return true;
  }

  void clearPlanSettings()
  {
    QgsSettings().remove( u"ai/provider/plan"_s );
    QgsAiSecretStore::removeSecret( u"ai/provider/plan/token"_s );
  }
} // namespace

class TestQgsAiWebSearchTool : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void returnsSearchResults();
    void stopDuringPendingSearchReturnsCanceled();
};

void TestQgsAiWebSearchTool::init()
{
  installTestSecretBackend();
  clearPlanSettings();
}

void TestQgsAiWebSearchTool::cleanup()
{
  clearPlanSettings();
}

void TestQgsAiWebSearchTool::returnsSearchResults()
{
  QgsAiTestLoopbackServer server;
  server.responses << QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"results\":[{\"title\":\"Roads\"}]}" ) );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiWebSearchTool tool( &router );
  const QgsAiToolResult result = tool.execute( QJsonObject { { u"query"_s, u"roads padova"_s } } );
  QVERIFY2( result.success, qPrintable( result.errorMessage ) );
  QVERIFY( !result.canceled );
  QVERIFY( server.rawRequests.at( 0 ).startsWith( "POST /v1/tools/web-search HTTP/1.1\r\n" ) );
}

void TestQgsAiWebSearchTool::stopDuringPendingSearchReturnsCanceled()
{
  QgsAiTestLoopbackServer server;
  QgsAiTestLoopbackServer::ScriptedResponse pending = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"results\":[]}" ) );
  pending.responseDelayMs = 60000;
  server.responses << pending;
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiModelRouter router;
  QVERIFY( configurePlanForLoopback( router, server.serverPort() ) );
  QgsAiWebSearchTool tool( &router );
  QTimer::singleShot( 200, []() { qgsAiCancelActiveBackgroundTool(); } );
  QElapsedTimer elapsed;
  elapsed.start();
  const QgsAiToolResult result = tool.execute( QJsonObject { { u"query"_s, u"roads padova"_s } } );
  QVERIFY( !result.success );
  QVERIFY( result.canceled );
  // Well under the search timeout: Stop aborted the pending request.
  QVERIFY2( elapsed.elapsed() < 10000, "Stop did not interrupt the pending search" );
  QVERIFY( !qgsAiHasActiveBackgroundTool() );
}

QGSTEST_MAIN( TestQgsAiWebSearchTool )
#include "testqgsaiwebsearchtool.moc"
