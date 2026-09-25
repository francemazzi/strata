/***************************************************************************
  testqgsaifilesummarizer.cpp
  ---------------------------
  begin                : September 2026
  copyright            : (C) 2026
***************************************************************************/

#include "ai/index/qgsaiembeddingprovider.h"
#include "ai/index/qgsaifilesummarizer.h"
#include "ai/index/qgsaiworkspaceindex.h"
#include "qgstest.h"

#include <QString>
#include <QStringList>

using namespace Qt::StringLiterals;

class TestQgsAiFileSummarizer : public QObject
{
    Q_OBJECT

  private slots:
    void csvSummaryDescribesColumns();
    void semicolonCsvWithDecimalCommas();
    void partlyReadCsvDropsTheCutLine();
    void geoJsonSummaryCountsFeatures();
    void plainJsonIsIndexedAsItIs();
    void projectSummaryListsLayersWithoutCredentials();
    void xmlSummaryKeepsNamesAndTexts();
    void otherFilesAreIndexedAsTheyAre();
    void chunksStayWithinTheTokenLimit();
    void e5ChunksOfStructuredFilesFitTheModel();
};

void TestQgsAiFileSummarizer::csvSummaryDescribesColumns()
{
  const QString csv = u"id,nome,area,rilievo\n1,Parco A,12.5,2024-01-01\n2,Parco B,3.25,2024-02-01\n3,\"Parco, C\",7,2024-03-01\n"_s;
  const QString summary = QgsAiFileSummarizer::summarize( u"dati/parchi.csv"_s, csv, false );
  QVERIFY( summary.contains( u"dati/parchi.csv"_s ) );
  QVERIFY2( summary.contains( u"3 rows"_s ), summary.toUtf8().constData() );
  QVERIFY( summary.contains( u"4 columns"_s ) );
  QVERIFY( summary.contains( u"- id: integer, 1 to 3"_s ) );
  QVERIFY( summary.contains( u"- area: decimal, 3.25 to 12.5"_s ) );
  QVERIFY( summary.contains( u"- rilievo: date"_s ) );
  QVERIFY( summary.contains( u"- nome: text, e.g. Parco A, Parco B, Parco, C"_s ) );
  QVERIFY( summary.contains( u"First rows:\nid,nome,area,rilievo\n1,Parco A"_s ) );
}

void TestQgsAiFileSummarizer::semicolonCsvWithDecimalCommas()
{
  const QString csv = u"codice;quota\nA;12,5\nB;3,1\n"_s;
  const QString summary = QgsAiFileSummarizer::summarize( u"quote.csv"_s, csv, false );
  QVERIFY2( summary.contains( u"delimiter \";\""_s ), summary.toUtf8().constData() );
  QVERIFY( summary.contains( u"- quota: decimal, 3.1 to 12.5"_s ) );
}

void TestQgsAiFileSummarizer::partlyReadCsvDropsTheCutLine()
{
  const QString csv = u"id,valore\n1,10\n2,20\n3,3"_s;
  const QString summary = QgsAiFileSummarizer::summarize( u"grande.csv"_s, csv, true );
  QVERIFY2( summary.contains( u"2 rows in the part read"_s ), summary.toUtf8().constData() );
  QVERIFY( summary.contains( u"- valore: integer, 10 to 20"_s ) );
}

void TestQgsAiFileSummarizer::geoJsonSummaryCountsFeatures()
{
  const QString geojson = uR"({"type":"FeatureCollection","features":[
    {"type":"Feature","properties":{"id":1,"specie":"Tilia cordata"},"geometry":{"type":"Point","coordinates":[9.1,45.4]}},
    {"type":"Feature","properties":{"id":2,"specie":"Quercus robur"},"geometry":{"type":"Point","coordinates":[9.3,45.6]}},
    {"type":"Feature","properties":{"id":3,"specie":"Tilia cordata"},"geometry":{"type":"Polygon","coordinates":[[[9,45],[10,45],[10,46],[9,45]]]}}
  ]})"_s;
  const QString summary = QgsAiFileSummarizer::summarize( u"alberi.geojson"_s, geojson, false );
  QVERIFY2( summary.contains( u"3 features"_s ), summary.toUtf8().constData() );
  QVERIFY( summary.contains( u"Point (2)"_s ) );
  QVERIFY( summary.contains( u"Polygon (1)"_s ) );
  QVERIFY( summary.contains( u"Extent: 9, 45 to 10, 46"_s ) );
  QVERIFY( summary.contains( u"- id: integer, 1 to 3"_s ) );
  QVERIFY( summary.contains( u"- specie: text, e.g. Tilia cordata, Quercus robur"_s ) );
  // No coordinates in the text: they are long in tokens and poor in meaning.
  QVERIFY( !summary.contains( u"45.4"_s ) );
}

void TestQgsAiFileSummarizer::plainJsonIsIndexedAsItIs()
{
  QVERIFY( QgsAiFileSummarizer::summarize( u"config.json"_s, uR"({"name":"strata"})"_s, false ).isEmpty() );
  // A GeoJSON file read only in part cannot be parsed: its text is indexed.
  QVERIFY( QgsAiFileSummarizer::summarize( u"big.geojson"_s, uR"({"type":"FeatureCollection","features":[)"_s, true ).isEmpty() );
}

void TestQgsAiFileSummarizer::projectSummaryListsLayersWithoutCredentials()
{
  const QString qgs = uR"(<!DOCTYPE qgis><qgis projectname="" version="4.3.1-Strata">
  <title>Verde urbano</title>
  <projectCrs><spatialrefsys><authid>EPSG:3003</authid></spatialrefsys></projectCrs>
  <projectlayers>
    <maplayer type="vector" geometry="Point">
      <id>alberi_1234</id>
      <datasource>./alberi.gpkg|layername=alberi</datasource>
      <layername>Alberi</layername>
      <srs><spatialrefsys><authid>EPSG:3003</authid></spatialrefsys></srs>
      <provider encoding="UTF-8">ogr</provider>
    </maplayer>
    <maplayer type="vector" geometry="Polygon">
      <id>parchi_5678</id>
      <datasource>dbname='gis' host=db.example.com user='mario' password='s3cret' table="public"."parchi" (geom)</datasource>
      <layername>Parchi</layername>
      <provider encoding="">postgres</provider>
    </maplayer>
  </projectlayers>
  <Layouts><Layout name="Tavola A3"/></Layouts>
</qgis>)"_s;
  const QString summary = QgsAiFileSummarizer::summarize( u"verde.qgs"_s, qgs, false );
  QVERIFY2( summary.contains( u"\"Verde urbano\""_s ), summary.toUtf8().constData() );
  QVERIFY( summary.contains( u"CRS EPSG:3003, 2 layers"_s ) );
  QVERIFY( summary.contains( u"Print layouts: Tavola A3"_s ) );
  QVERIFY( summary.contains( u"- Alberi (vector, Point, ogr, EPSG:3003): ./alberi.gpkg|layername=alberi"_s ) );
  QVERIFY( summary.contains( u"- Parchi (vector, Polygon, postgres)"_s ) );
  QVERIFY( !summary.contains( u"s3cret"_s ) );
  QVERIFY( !summary.contains( u"mario"_s ) );
  QVERIFY( summary.contains( u"password=***"_s ) );

  QCOMPARE( QgsAiFileSummarizer::redactDataSource( u"https://user:pw@example.com/wfs?token=abc&typename=a"_s ), u"https://***@example.com/wfs?token=***&typename=a"_s );
}

void TestQgsAiFileSummarizer::xmlSummaryKeepsNamesAndTexts()
{
  const QString xml
    = u"<StyledLayerDescriptor><NamedLayer><Name>Parchi</Name><UserStyle><Title>Verde pubblico</Title><Rule name=\"grandi\"/><Rule name=\"piccoli\"/></UserStyle></NamedLayer></StyledLayerDescriptor>"_s;
  const QString summary = QgsAiFileSummarizer::summarize( u"stile.xml"_s, xml, false );
  QVERIFY2( summary.contains( u"root element <StyledLayerDescriptor>"_s ), summary.toUtf8().constData() );
  QVERIFY( summary.contains( u"Rule (2)"_s ) );
  QVERIFY( summary.contains( u"grandi"_s ) );
  QVERIFY( summary.contains( u"Verde pubblico"_s ) );
}

void TestQgsAiFileSummarizer::otherFilesAreIndexedAsTheyAre()
{
  QVERIFY( QgsAiFileSummarizer::summarize( u"README.md"_s, u"# Progetto"_s, false ).isEmpty() );
  QVERIFY( QgsAiFileSummarizer::summarize( u"script.py"_s, u"print(1)"_s, false ).isEmpty() );
}

void TestQgsAiFileSummarizer::chunksStayWithinTheTokenLimit()
{
  // One token per character keeps the arithmetic visible.
  const QgsAiWorkspaceIndex::TokenCounter perCharacter = []( const QString &text ) { return static_cast<int>( text.size() ); };
  QStringList lines;
  for ( int i = 0; i < 40; ++i )
    lines << QString( 99, QChar( 'a' + i % 25 ) );
  lines << QString( 1000, 'z' );
  const QStringList chunks = QgsAiWorkspaceIndex::chunkTextByTokens( lines.join( '\n' ), perCharacter, 450 );
  QVERIFY( chunks.size() > 5 );
  int total = 0;
  for ( const QString &chunk : chunks )
  {
    QVERIFY2( chunk.size() <= 450, QString::number( chunk.size() ).toUtf8().constData() );
    total += static_cast<int>( chunk.count( 'z' ) );
  }
  // The long line is split, not lost.
  QCOMPARE( total, 1000 );

  QCOMPARE( QgsAiWorkspaceIndex::truncateToTokens( QString( 100, 'x' ), perCharacter, 30 ).size(), 30 );
  QCOMPARE( QgsAiWorkspaceIndex::truncateToTokens( u"short"_s, perCharacter, 30 ), u"short"_s );
}

void TestQgsAiFileSummarizer::e5ChunksOfStructuredFilesFitTheModel()
{
  if ( !QgsAiEmbeddingProviderRegistry::providerIds().contains( QgsAiE5EmbeddingProvider::staticProviderId() ) )
    QSKIP( "Local E5 embeddings were not compiled." );
  if ( qgetenv( "STRATA_AI_EMBEDDING_MODEL_DIR" ).trimmed().isEmpty() )
    QSKIP( "STRATA_AI_EMBEDDING_MODEL_DIR is not set." );

  QgsAiE5EmbeddingProvider provider;
  QVERIFY( provider.tokenCount( u"strade comunali"_s ) > 0 );
  // Counting never loads the model.
  QVERIFY( !provider.runtimeLoaded() );

  // Numeric CSV: about two characters per token, so 1200 characters were cut at 512 tokens.
  QStringList rows { u"id,x,y,quota,portata,data"_s };
  for ( int i = 0; i < 400; ++i )
    rows << u"%1,%2,%3,%4,%5,2024-%6-01"_s.arg( i ).arg( 1500000.123 + i ).arg( 5000000.456 + i ).arg( 12.34 + i ).arg( 0.5 * i ).arg( 1 + i % 12, 2, 10, QChar( '0' ) );
  const int maxTokens = provider.maxInputTokens() - QgsAiWorkspaceIndex::CHUNK_TOKEN_MARGIN;
  const QgsAiWorkspaceIndex::TokenCounter counter = [&provider]( const QString &text ) { return provider.tokenCount( text ); };
  const QStringList rawChunks = QgsAiWorkspaceIndex::chunkTextByTokens( rows.join( '\n' ), counter, maxTokens );
  QVERIFY( rawChunks.size() > 1 );
  for ( const QString &chunk : rawChunks )
    QVERIFY2( provider.tokenCount( QgsAiE5EmbeddingProvider::formatInputForRole( chunk, QgsAiEmbeddingRole::Passage ) ) + 2 <= provider.maxInputTokens(), chunk.left( 80 ).toUtf8().constData() );

  // Its summary fits one chunk.
  const QString summary = QgsAiFileSummarizer::summarize( u"misure.csv"_s, rows.join( '\n' ), false );
  QCOMPARE( QgsAiWorkspaceIndex::chunkTextByTokens( summary, counter, maxTokens ).size(), 1 );
}

QGSTEST_MAIN( TestQgsAiFileSummarizer )
#include "testqgsaifilesummarizer.moc"
