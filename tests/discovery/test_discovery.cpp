// SPDX-License-Identifier: GPL-2.0-or-later
#include <zip.h>

#include "qgsaidiscoveryclient.h"
#include "qgsaidiscoverydownload.h"
#include "qgsaidiscoveryfiles.h"
#include "qgsaidiscoverypresentation.h"
#include "qgsaidiscoverypreview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
class TestDiscovery : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
      QCoreApplication::setOrganizationName( u"StrataSyntheticTests"_s );
      QCoreApplication::setApplicationName( QUuid::createUuid().toString() );
    }
    void cleanupTestCase() { QSettings().clear(); }
    void pathAndDigestGuards()
    {
      using namespace QgsAiDiscoveryFiles;
      QVERIFY( !safeName( u"../escape"_s ) );
      QVERIFY( !safeName( u"layers//file"_s ) );
      QVERIFY( !safeName( u"C:/x"_s ) );
      QVERIFY( !safeUrl( QUrl( u"https://secret@example.com/file"_s ) ) );
      QVERIFY( !safeUrl( QUrl( u"file:///tmp/x"_s ) ) );
      QVERIFY( !validDigest( QString( 64, 'z' ) ) );
      QTemporaryDir root;
      QVERIFY( !destinationError( root.path(), root.path() + u"/x"_s ).size() );
      QVERIFY( destinationError( root.path(), root.path() + u"/../x"_s ).size() );
      QVERIFY( QFile::link( u"/tmp"_s, root.path() + u"/link"_s ) );
      QVERIFY( destinationError( root.path(), root.path() + u"/link/escape"_s ).size() );
    }
    void previewRequiresOneUserSelection()
    {
      const QJsonObject candidate { { u"id"_s, u"candidate"_s }, { u"title"_s, u"Synthetic boundary"_s }, { u"modes"_s, QJsonArray { u"file"_s } } };
      QgsAiDiscoveryPreview preview( { { u"id"_s, u"plan"_s }, { u"expiresAt"_s, u"2099-01-01T00:00:00.000Z"_s }, { u"candidates"_s, QJsonArray { candidate } } }, u"discovery/test"_s );
      QSignalSpy accepted( &preview, &QgsAiDiscoveryPreview::approved );
      auto button = preview.findChild<QPushButton *>( u"discoveryConfirm"_s );
      QVERIFY( !button->isEnabled() );
      preview.findChild<QCheckBox *>()->setChecked( true );
      QVERIFY( button->isEnabled() );
      button->click();
      button->click();
      QCOMPARE( accepted.size(), 1 );
      QCOMPARE( accepted[0][0].toJsonArray()[0].toObject().value( u"candidateId"_s ).toString(), u"candidate"_s );
    }
    void coverageUsesBackendEligibilityForSelectedMode()
    {
      const QJsonObject candidate {
        { u"id"_s, u"service"_s },
        { u"title"_s, u"Synthetic WFS"_s },
        { u"modes"_s, QJsonArray { u"file"_s, u"service"_s } },
        { u"eligibleRequirementsByMode"_s, QJsonObject { { u"file"_s, QJsonArray {} }, { u"service"_s, QJsonArray { u"boundary"_s } } } }
      };
      QgsAiDiscoveryPreview
        preview( { { u"expiresAt"_s, u"2099-01-01T00:00:00.000Z"_s }, { u"candidates"_s, QJsonArray { candidate } }, { u"coverage"_s, QJsonObject { { u"required"_s, QJsonArray { u"boundary"_s } } } } }, u"test"_s );
      preview.findChildren<QCheckBox *>()[0]->setChecked( true );
      auto gaps = preview.findChild<QLabel *>( u"discoverySelectedGaps"_s );
      QVERIFY( gaps->text().contains( u"Confini"_s ) );
      preview.findChild<QComboBox *>()->setCurrentIndex( 1 );
      QVERIFY( !gaps->text().contains( u"Confini"_s ) );
    }
    void unknownContentRequiresExplicitConsent()
    {
      const QJsonObject candidate {
        { u"id"_s, u"uncertain"_s },
        { u"title"_s, u"Synthetic archive"_s },
        { u"modes"_s, QJsonArray { u"file"_s } },
        { u"content"_s, QJsonObject { { u"kind"_s, u"unknown"_s }, { u"status"_s, u"unverified"_s } } },
        { u"eligibleRequirementsByMode"_s, QJsonObject { { u"file"_s, QJsonArray {} } } }
      };
      QgsAiDiscoveryPreview preview( { { u"expiresAt"_s, u"2099-01-01T00:00:00.000Z"_s }, { u"candidates"_s, QJsonArray { candidate } } }, u"test"_s );
      QSignalSpy accepted( &preview, &QgsAiDiscoveryPreview::approved );
      auto button = preview.findChild<QPushButton *>( u"discoveryConfirm"_s );
      preview.findChildren<QCheckBox *>()[0]->setChecked( true );
      QVERIFY( !button->isEnabled() );
      preview.findChild<QCheckBox *>( u"discoveryContentConsent"_s )->setChecked( true );
      QVERIFY( button->isEnabled() );
      preview.findChild<QSpinBox *>( u"discoveryCandidateMiB"_s )->setValue( 8 );
      button->click();
      const auto item = accepted[0][0].toJsonArray()[0].toObject();
      QVERIFY( item.value( u"allowUnverifiedContent"_s ).toBool() );
      QCOMPARE( item.value( u"maxBytes"_s ).toInteger(), 8388608 );
    }
    void acquisitionResultsExposeFailureAndUsage()
    {
      const auto text = QgsAiDiscoveryPresentation::runResults(
        { { u"results"_s, QJsonArray { QJsonObject { { u"title"_s, u"Synthetic table"_s }, { u"status"_s, u"FAILED"_s }, { u"transferredBytes"_s, 1048576 }, { u"creditsCharged"_s, 1 }, { u"issues"_s, QJsonArray { u"No readable GIS layer"_s } } } } } }
      );
      QVERIFY( text.contains( u"Synthetic table"_s ) );
      QVERIFY( text.contains( u"1.00 MiB"_s ) );
      QVERIFY( text.contains( u"No readable GIS layer"_s ) );
    }
    void asyncRequestsResumeWithSameKey()
    {
      QTcpServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      QNetworkAccessManager network;
      QList<QByteArray> requests;
      connect( &server, &QTcpServer::newConnection, this, [&]() {
        auto socket = server.nextPendingConnection();
        auto bytes = std::make_shared<QByteArray>();
        connect( socket, &QTcpSocket::readyRead, socket, [socket, bytes, &requests]() {
          bytes->append( socket->readAll() );
          if ( !bytes->contains( "\r\n\r\n" ) )
            return;
          requests << *bytes;
          const QByteArray body = R"({"id":"csyntheticplan","status":"SUCCEEDED","version":1,"candidates":[]})";
          socket->write( "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number( body.size() ) + "\r\nConnection: close\r\n\r\n" + body );
          socket->disconnectFromHost();
        } );
      } );
      const QUrl base( u"http://127.0.0.1:%1"_s.arg( server.serverPort() ) );
      QString key;
      {
        QgsAiDiscoveryClient first( &network, []( QNetworkRequest & ) { return false; } );
        first.setScope( base, u"synthetic"_s );
        const auto discarded = first.submit( u"/v1/discovery/plans"_s, { { u"maxCredits"_s, 3 } } );
        QVERIFY( first.discardUnsent( discarded ) );
        key = first.submit( u"/v1/discovery/plans"_s, { { u"maxCredits"_s, 2 } } );
      }
      QVERIFY( requests.isEmpty() );
      QgsAiDiscoveryClient resumed( &network, []( QNetworkRequest & ) { return true; } );
      resumed.setScope( base, u"synthetic"_s );
      QSignalSpy changed( &resumed, &QgsAiDiscoveryClient::updated );
      QElapsedTimer timer;
      timer.start();
      resumed.resume();
      QVERIFY( timer.elapsed() < 100 );
      QTRY_COMPARE( changed.size(), 1 );
      QCOMPARE( requests.size(), 1 );
      QVERIFY( requests[0].contains( key.toUtf8() ) );
      QCOMPARE( resumed.snapshot( key ).value( u"id"_s ).toString(), u"csyntheticplan"_s );
      resumed.watch( u"plans"_s, u"csyntheticplan"_s );
      QTRY_COMPARE( changed.size(), 2 );
      QVERIFY( requests[1].startsWith( "GET /v1/discovery/plans/csyntheticplan" ) );
    }
    void interruptedDownloadDoesNotPublishAFile()
    {
      QTemporaryDir root;
      QTcpServer server;
      QVERIFY( server.listen( QHostAddress::LocalHost ) );
      QNetworkAccessManager network;
      QgsAiDiscoveryDownload download( &network );
      QSignalSpy finished( &download, &QgsAiDiscoveryDownload::finished );
      connect( &server, &QTcpServer::newConnection, this, [&]() {
        auto socket = server.nextPendingConnection();
        connect( socket, &QTcpSocket::readyRead, socket, [socket, &download]() {
          socket->readAll();
          socket->write( "HTTP/1.1 200 OK\r\nContent-Length: 10000\r\n\r\npartial" );
          socket->flush();
          QTimer::singleShot( 10, &download, &QgsAiDiscoveryDownload::cancel );
        } );
      } );
      const auto destination = root.filePath( u"canceled.zip"_s );
      download.start( QUrl( u"http://127.0.0.1:%1/kit"_s.arg( server.serverPort() ) ), root.path(), destination, QString( 64, '0' ), 10000 );
      QTRY_COMPARE( finished.size(), 1 );
      QVERIFY( !finished[0][1].toString().isEmpty() );
      QVERIFY( !QFileInfo::exists( destination ) );
    }
    void streamingKitAndCorruption()
    {
      QTemporaryDir temp;
      const QString path = temp.path() + u"/kit.zip"_s;
      const QByteArray data = R"({"type":"FeatureCollection","features":[]})";
      const QString name = u"layers/key/layer.geojson"_s;
      const QJsonArray approved { QJsonObject { { u"key"_s, u"key"_s }, { u"mode"_s, u"file"_s } } };
      const QJsonObject manifest {
        { u"schemaVersion"_s, u"strata-discovery/1"_s },
        { u"files"_s,
          QJsonArray { QJsonObject { { u"path"_s, name }, { u"sizeBytes"_s, data.size() }, { u"sha256"_s, QString::fromLatin1( QCryptographicHash::hash( data, QCryptographicHash::Sha256 ).toHex() ) } } } },
        { u"layers"_s, QJsonArray { QJsonObject { { u"id"_s, u"key:0"_s }, { u"mode"_s, u"file"_s }, { u"path"_s, name } } } }
      };
      const QByteArray metadata = QJsonDocument( manifest ).toJson();
      int error;
      auto archive = zip_open( QFile::encodeName( path ).constData(), ZIP_CREATE, &error );
      QVERIFY( archive );
      zip_file_add( archive, "manifest.json", zip_source_buffer( archive, metadata.constData(), metadata.size(), 0 ), 0 );
      zip_file_add( archive, "README.txt", zip_source_buffer( archive, "test", 4, 0 ), 0 );
      zip_file_add( archive, name.toUtf8().constData(), zip_source_buffer( archive, data.constData(), data.size(), 0 ), 0 );
      QCOMPARE( zip_close( archive ), 0 );
      QFile file( path );
      QVERIFY( file.open( QIODevice::ReadOnly ) );
      const auto hash = QString::fromLatin1( QCryptographicHash::hash( file.readAll(), QCryptographicHash::Sha256 ).toHex() );
      file.close();
      auto valid = QgsAiDiscoveryFiles::unpack( path, temp.path() + u"/valid"_s, hash, approved );
      QVERIFY2( valid.error.isEmpty(), qPrintable( valid.error ) );
      QVERIFY2( QgsAiDiscoveryFiles::unpack( path, temp.path() + u"/valid"_s, hash, approved ).error.isEmpty(), "Resuming verified extraction must be idempotent" );
      QVERIFY( QFileInfo::exists( temp.path() + u"/valid/"_s + name ) );
      QVERIFY( !QgsAiDiscoveryFiles::unpack( path, temp.path() + u"/other"_s, hash, {} ).error.isEmpty() );
      QVERIFY( !QgsAiDiscoveryFiles::unpack( path, temp.path() + u"/bad"_s, QString( 64, '0' ), approved ).error.isEmpty() );
    }
};
QTEST_MAIN( TestDiscovery )
#include "test_discovery.moc"
