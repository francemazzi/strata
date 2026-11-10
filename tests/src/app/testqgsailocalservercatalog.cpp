/***************************************************************************
  testqgsailocalservercatalog.cpp
  -------------------------------
  begin                : November 2026
***************************************************************************/

#include "ai/qgsailocalservercatalog.h"
#include "qgsaitestloopbackserver.h"
#include "qgsapplication.h"
#include "qgstest.h"

#include <QHostAddress>
#include <QSignalSpy>
#include <QString>
#include <QTcpServer>

using namespace Qt::StringLiterals;

namespace
{
  const QByteArray TAGS_FIXTURE = QByteArrayLiteral(
    "{\"models\":["
    "{\"name\":\"qwen3:14b\",\"digest\":\"sha-qwen\",\"details\":{\"parameter_size\":\"14.8B\"}},"
    "{\"name\":\"nomic-embed-text:latest\",\"digest\":\"sha-embed\",\"details\":{\"parameter_size\":\"137M\"}},"
    "{\"name\":\"llava:latest\",\"digest\":\"sha-llava\",\"details\":{\"parameter_size\":\"7B\"}},"
    "{\"name\":\"gpt-oss:20b\",\"digest\":\"sha-gpt\",\"details\":{\"parameter_size\":\"20.9B\"}}"
    "]}"
  );

  // Answers the three Ollama endpoints the catalog uses, whatever their order.
  void serveOllama( QgsAiTestLoopbackServer &server )
  {
    server.requestHandler = []( const QByteArray &rawRequest, const QByteArray &body, QgsAiTestLoopbackServer::ScriptedResponse &response ) {
      if ( rawRequest.startsWith( "GET /api/version" ) )
        response = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"version\":\"0.40.14\"}" ) );
      else if ( rawRequest.startsWith( "GET /api/tags" ) )
        response = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", TAGS_FIXTURE );
      else if ( rawRequest.startsWith( "POST /api/show" ) && body.contains( "qwen3:14b" ) )
        response = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"capabilities\":[\"completion\",\"tools\",\"thinking\"],\"model_info\":{\"qwen3.context_length\":40960}}" ) );
      else if ( rawRequest.startsWith( "POST /api/show" ) && body.contains( "nomic-embed-text" ) )
        response = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"capabilities\":[\"embedding\"],\"model_info\":{\"nomic-bert.context_length\":2048}}" ) );
      else if ( rawRequest.startsWith( "POST /api/show" ) && body.contains( "llava" ) )
        response = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"capabilities\":[\"completion\",\"vision\"],\"model_info\":{\"llama.context_length\":32768}}" ) );
      else if ( rawRequest.startsWith( "POST /api/show" ) && body.contains( "gpt-oss" ) )
        response = QgsAiTestLoopbackServer::jsonResponse( 200, "OK", QByteArrayLiteral( "{\"capabilities\":[\"completion\",\"tools\",\"vision\"],\"model_info\":{\"gptoss.context_length\":131072}}" ) );
      else
        response = QgsAiTestLoopbackServer::jsonResponse( 404, "Not Found", QByteArrayLiteral( "{\"error\":\"unknown endpoint\"}" ) );
      return true;
    };
  }

  int countRequests( const QgsAiTestLoopbackServer &server, const QByteArray &requestLinePrefix )
  {
    int count = 0;
    for ( const QByteArray &raw : server.rawRequests )
    {
      if ( raw.startsWith( requestLinePrefix ) )
        ++count;
    }
    return count;
  }
} //namespace

class TestQgsAiLocalServerCatalog : public QObject
{
    Q_OBJECT

  private slots:
    void normalizesBaseUrls();
    void recognizesLoopbackUrls();
    void parsesShowCapabilities();
    void listsOnlyToolCapableChatModels();
    void reusesModelDetailsByDigest();
    void stoppedServerIsReportedNotRunning();
    void changingServerForgetsItsModels();
};

void TestQgsAiLocalServerCatalog::normalizesBaseUrls()
{
  // The shapes OLLAMA_HOST takes in practice.
  QCOMPARE( QgsAiLocalServerCatalog::normalizedBaseUrl( u"127.0.0.1:11434"_s, 11434 ), u"http://127.0.0.1:11434"_s );
  QCOMPARE( QgsAiLocalServerCatalog::normalizedBaseUrl( u"0.0.0.0"_s, 11434 ), u"http://127.0.0.1:11434"_s );
  QCOMPARE( QgsAiLocalServerCatalog::normalizedBaseUrl( u"0.0.0.0:11500"_s, 11434 ), u"http://127.0.0.1:11500"_s );
  QCOMPARE( QgsAiLocalServerCatalog::normalizedBaseUrl( u"http://gpu-box:11434/"_s, 11434 ), u"http://gpu-box:11434"_s );
  QCOMPARE( QgsAiLocalServerCatalog::normalizedBaseUrl( u"https://ollama.example.org"_s, 11434 ), u"https://ollama.example.org"_s );
  QCOMPARE( QgsAiLocalServerCatalog::normalizedBaseUrl( u"  "_s, 11434 ), QString() );
}

void TestQgsAiLocalServerCatalog::recognizesLoopbackUrls()
{
  QVERIFY( QgsAiLocalServerCatalog::isLoopbackUrl( u"http://127.0.0.1:11434"_s ) );
  QVERIFY( QgsAiLocalServerCatalog::isLoopbackUrl( u"http://localhost:11434"_s ) );
  QVERIFY( QgsAiLocalServerCatalog::isLoopbackUrl( u"http://[::1]:11434"_s ) );
  QVERIFY( !QgsAiLocalServerCatalog::isLoopbackUrl( u"http://192.168.1.20:11434"_s ) );
  QVERIFY( !QgsAiLocalServerCatalog::isLoopbackUrl( u"https://ollama.example.org"_s ) );
}

void TestQgsAiLocalServerCatalog::parsesShowCapabilities()
{
  QgsAiLocalServerCatalog::ModelInfo tools;
  QVERIFY(
    QgsAiLocalServerCatalog::
      parseOllamaShow( QByteArrayLiteral( "{\"capabilities\":[\"completion\",\"tools\",\"thinking\",\"vision\"],\"details\":{\"parameter_size\":\"27B\"},\"model_info\":{\"general.architecture\":\"qwen3\",\"qwen3.context_length\":262144}}" ), tools )
  );
  QVERIFY( tools.supportsTools );
  QVERIFY( tools.supportsThinking );
  QVERIFY( tools.supportsVision );
  QCOMPARE( tools.contextLength, 262144 );
  QCOMPARE( tools.parameterSize, u"27B"_s );

  // Tool calling without chat completion (or the reverse) cannot drive the agent loop.
  QgsAiLocalServerCatalog::ModelInfo embedding;
  QVERIFY( QgsAiLocalServerCatalog::parseOllamaShow( QByteArrayLiteral( "{\"capabilities\":[\"embedding\"]}" ), embedding ) );
  QVERIFY( !embedding.supportsTools );
  QgsAiLocalServerCatalog::ModelInfo chatOnly;
  QVERIFY( QgsAiLocalServerCatalog::parseOllamaShow( QByteArrayLiteral( "{\"capabilities\":[\"completion\"]}" ), chatOnly ) );
  QVERIFY( !chatOnly.supportsTools );

  // A server too old to report capabilities must not end up with an empty picker.
  QgsAiLocalServerCatalog::ModelInfo legacy;
  QVERIFY( QgsAiLocalServerCatalog::parseOllamaShow( QByteArrayLiteral( "{\"model_info\":{\"llama.context_length\":8192}}" ), legacy ) );
  QVERIFY( legacy.supportsTools );
  QCOMPARE( legacy.contextLength, 8192 );

  QgsAiLocalServerCatalog::ModelInfo garbage;
  QVERIFY( !QgsAiLocalServerCatalog::parseOllamaShow( QByteArrayLiteral( "not json" ), garbage ) );
}

void TestQgsAiLocalServerCatalog::listsOnlyToolCapableChatModels()
{
  QgsAiTestLoopbackServer server;
  serveOllama( server );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiLocalServerCatalog catalog;
  catalog.setBaseUrl( u"http://127.0.0.1:%1"_s.arg( server.serverPort() ) );
  QCOMPARE( catalog.state(), QgsAiLocalServerCatalog::ServerState::Unknown );

  catalog.refresh( true );
  QTRY_COMPARE_WITH_TIMEOUT( catalog.state(), QgsAiLocalServerCatalog::ServerState::Running, 10000 );

  QCOMPARE( catalog.version(), u"0.40.14"_s );
  QCOMPARE( catalog.installedModelCount(), 4 );
  // The embedding model and the chat model without tools are left out; the rest is sorted.
  const QList<QgsAiLocalServerCatalog::ModelInfo> models = catalog.models();
  QCOMPARE( models.size(), 2 );
  QCOMPARE( models.at( 0 ).id, u"gpt-oss:20b"_s );
  QCOMPARE( models.at( 0 ).contextLength, 131072 );
  QVERIFY( models.at( 0 ).supportsVision );
  QCOMPARE( models.at( 1 ).id, u"qwen3:14b"_s );
  QCOMPARE( models.at( 1 ).contextLength, 40960 );
  QVERIFY( models.at( 1 ).supportsThinking );
  QCOMPARE( models.at( 1 ).displayLabel(), u"qwen3:14b — 14.8B, 40k ctx"_s );

  QCOMPARE( catalog.model( u"qwen3:14b"_s ).contextLength, 40960 );
  QVERIFY( catalog.model( u"llava:latest"_s ).id.isEmpty() );
}

void TestQgsAiLocalServerCatalog::reusesModelDetailsByDigest()
{
  QgsAiTestLoopbackServer server;
  serveOllama( server );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiLocalServerCatalog catalog;
  catalog.setBaseUrl( u"http://127.0.0.1:%1"_s.arg( server.serverPort() ) );
  catalog.refresh( true );
  QTRY_COMPARE_WITH_TIMEOUT( catalog.state(), QgsAiLocalServerCatalog::ServerState::Running, 10000 );
  QCOMPARE( countRequests( server, "POST /api/show" ), 4 );

  // An unforced refresh right after a probe is throttled.
  catalog.refresh();
  QTest::qWait( 200 );
  QCOMPARE( countRequests( server, "GET /api/version" ), 1 );

  // A forced one re-reads the model list but not the per-model details.
  QSignalSpy changedSpy( &catalog, &QgsAiLocalServerCatalog::changed );
  catalog.refresh( true );
  QTRY_COMPARE_WITH_TIMEOUT( countRequests( server, "GET /api/tags" ), 2, 10000 );
  QTest::qWait( 200 );
  QCOMPARE( countRequests( server, "POST /api/show" ), 4 );
  QCOMPARE( catalog.models().size(), 2 );
  // Nothing differs, so listeners are not asked to rebuild their menus.
  QCOMPARE( changedSpy.count(), 0 );
}

void TestQgsAiLocalServerCatalog::stoppedServerIsReportedNotRunning()
{
  // Reserve a port, then free it: nothing listens there any more.
  QTcpServer placeholder;
  QVERIFY( placeholder.listen( QHostAddress::LocalHost, 0 ) );
  const quint16 freePort = placeholder.serverPort();
  placeholder.close();

  QgsAiLocalServerCatalog catalog;
  catalog.setBaseUrl( u"http://127.0.0.1:%1"_s.arg( freePort ) );
  QSignalSpy changedSpy( &catalog, &QgsAiLocalServerCatalog::changed );
  catalog.refresh( true );

  QTRY_COMPARE_WITH_TIMEOUT( catalog.state(), QgsAiLocalServerCatalog::ServerState::NotRunning, 10000 );
  QVERIFY( catalog.models().isEmpty() );
  QCOMPARE( changedSpy.count(), 1 );
}

void TestQgsAiLocalServerCatalog::changingServerForgetsItsModels()
{
  QgsAiTestLoopbackServer server;
  serveOllama( server );
  QVERIFY( server.listen( QHostAddress::LocalHost, 0 ) );

  QgsAiLocalServerCatalog catalog;
  catalog.setBaseUrl( u"http://127.0.0.1:%1"_s.arg( server.serverPort() ) );
  catalog.refresh( true );
  QTRY_COMPARE_WITH_TIMEOUT( catalog.models().size(), 2, 10000 );

  // The models of the previous server must not be offered for the new one.
  catalog.setBaseUrl( u"http://127.0.0.1:1"_s );
  QCOMPARE( catalog.state(), QgsAiLocalServerCatalog::ServerState::Unknown );
  QVERIFY( catalog.models().isEmpty() );

  // A request that finds the server gone takes its models off the list at once.
  catalog.setBaseUrl( u"http://127.0.0.1:%1"_s.arg( server.serverPort() ) );
  catalog.refresh( true );
  QTRY_COMPARE_WITH_TIMEOUT( catalog.models().size(), 2, 10000 );
  catalog.markUnreachable();
  QCOMPARE( catalog.state(), QgsAiLocalServerCatalog::ServerState::NotRunning );
  QVERIFY( catalog.models().isEmpty() );
}

QGSTEST_MAIN( TestQgsAiLocalServerCatalog )
#include "testqgsailocalservercatalog.moc"
