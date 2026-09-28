#include "qgstest.h"
#include "updates/qgsupdatemanifest.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
using namespace Qt::StringLiterals;
class TestQgsUpdateManifest : public QObject
{
    Q_OBJECT
  private:
    QByteArray manifest( const QString &version = u"1.6.2"_s, const QString &url = u"https://github.com/francemazzi/strata/releases/download/strata-v1.6.2/Strata.AppImage"_s )
    {
      return QJsonDocument(
               QJsonObject {
                 { u"version"_s, version },
                 { u"packages"_s,
                   QJsonArray { QJsonObject {
                     { u"platform"_s, u"linux"_s },
                     { u"architecture"_s, u"x86_64"_s },
                     { u"format"_s, u"AppImage"_s },
                     { u"size"_s, 3 },
                     { u"sha256"_s, QString::fromLatin1( QCryptographicHash::hash( "abc", QCryptographicHash::Sha256 ).toHex() ) },
                     { u"url"_s, url }
                   } } }
               }
      ).toJson();
    }
  private slots:
    void verifiesPublishedTrustRoot()
    {
      QFile data( QStringLiteral( TEST_DATA_DIR ) + u"/updates/valid-manifest.json"_s );
      QFile signature( QStringLiteral( TEST_DATA_DIR ) + u"/updates/valid-manifest.sig"_s );
      QVERIFY( data.open( QIODevice::ReadOnly ) );
      QVERIFY( signature.open( QIODevice::ReadOnly ) );
      const QByteArray bytes = data.readAll(), sig = signature.readAll();
      QString error;
      QVERIFY2( QgsUpdateManifest::verify( bytes, sig, &error ), qPrintable( error ) );
      QVERIFY( !QgsUpdateManifest::verify( bytes + " ", sig, &error ) );
    }
    void rejectsUntrustedSignatures()
    {
      QString error;
      QVERIFY( !QgsUpdateManifest::verify( manifest(), "invalid", &error ) );
      QVERIFY( !error.isEmpty() );
    }
    void selectsOnlyNewCompatibleStablePackages()
    {
      QgsUpdatePackage package;
      QString error;
      QVERIFY( QgsUpdateManifest::select( manifest(), u"1.6.1"_s, u"linux"_s, u"x86_64"_s, package, &error ) );
      QCOMPARE( package.version, u"1.6.2"_s );
      for ( const auto &version : { u"1.6.2"_s, u"1.6.3"_s } )
        QVERIFY( !QgsUpdateManifest::select( manifest(), version, u"linux"_s, u"x86_64"_s, package, &error ) );
      QVERIFY( !QgsUpdateManifest::select( manifest( u"1.6.2-rc1"_s ), u"1.6.1"_s, u"linux"_s, u"x86_64"_s, package, &error ) );
      QVERIFY( !QgsUpdateManifest::select( manifest(), u"1.6.1"_s, u"linux"_s, u"arm64"_s, package, &error ) );
    }
    void rejectsForeignOrMismatchedAssetLocations()
    {
      QgsUpdatePackage package;
      QString error;
      for ( const auto &url :
            { u"http://github.com/francemazzi/strata/releases/download/strata-v1.6.2/a"_s,
              u"https://example.org/a"_s,
              u"https://github.com/other/repo/releases/download/strata-v1.6.2/a"_s,
              u"https://github.com/francemazzi/strata/releases/download/strata-v1.6.1/a"_s,
              u"https://user@github.com/francemazzi/strata/releases/download/strata-v1.6.2/a"_s } )
        QVERIFY( !QgsUpdateManifest::select( manifest( u"1.6.2"_s, url ), u"1.6.1"_s, u"linux"_s, u"x86_64"_s, package, &error ) );
    }
    void verifiesExactBytesAndLength()
    {
      QgsUpdatePackage package;
      QString error;
      QVERIFY( QgsUpdateManifest::select( manifest(), u"1.6.1"_s, u"linux"_s, u"x86_64"_s, package, &error ) );
      QTemporaryDir dir;
      QFile file( dir.filePath( u"package"_s ) );
      QVERIFY( file.open( QIODevice::WriteOnly ) );
      file.write( "abc" );
      file.close();
      QVERIFY( QgsUpdateManifest::verifyFile( file.fileName(), package, &error ) );
      QVERIFY( file.open( QIODevice::WriteOnly ) );
      file.write( "bad" );
      file.close();
      QVERIFY( !QgsUpdateManifest::verifyFile( file.fileName(), package, &error ) );
      QVERIFY( file.open( QIODevice::WriteOnly ) );
      file.write( "ab" );
      file.close();
      QVERIFY( !QgsUpdateManifest::verifyFile( file.fileName(), package, &error ) );
    }
};
QGSTEST_MAIN( TestQgsUpdateManifest )
#include "testqgsupdatemanifest.moc"
