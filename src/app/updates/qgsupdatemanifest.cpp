#include "qgsupdatemanifest.h"
#include "update-public-key.h"
#include <QtCrypto>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QVersionNumber>
#include <QSysInfo>
#include <QCoreApplication>
using namespace Qt::StringLiterals;
namespace
{
  bool fail( QString *error, const QString &message )
  {
    if ( error )
      *error = message;
    return false;
  }
} //namespace
bool QgsUpdateManifest::isReleaseUrl( const QUrl &url )
{
#ifdef STRATA_ENABLE_UPDATE_TEST_FEED
  if ( url.scheme() == "http"_L1 && url.host() == "127.0.0.1"_L1 && url.userInfo().isEmpty() )
    return true;
#endif
  return url.scheme() == "https"_L1
         && url.host() == "github.com"_L1
         && url.userInfo().isEmpty()
         && ( url.port() == -1 || url.port() == 443 )
         && url.query().isEmpty()
         && url.fragment().isEmpty()
         && url.path().startsWith( "/francemazzi/strata/releases/download/strata-v"_L1 )
         && !url.path().contains( ".."_L1 );
}
bool QgsUpdateManifest::verify( const QByteArray &manifest, const QByteArray &signature, QString *error )
{
  if ( manifest.size() > 1024 * 1024 || signature.size() > 8192 )
    return fail( error, u"Update metadata is too large."_s );
  static QCA::Initializer crypto;
  QCA::ConvertResult result;
  QCA::PublicKey key = QCA::PublicKey::fromPEM( QString::fromLatin1( STRATA_UPDATE_PUBLIC_KEY ), &result );
  if ( result != QCA::ConvertGood || key.isNull() || !key.verifyMessage( QCA::MemoryRegion( manifest ), QByteArray::fromBase64( signature.trimmed() ), QCA::EMSA3_SHA256 ) )
    return fail( error, u"The update signature is invalid. Nothing was installed."_s );
  const auto object = QJsonDocument::fromJson( manifest ).object();
  if ( object.value( u"schemaVersion"_s ).toInt() != 1 || object.value( u"keyId"_s ).toString() != QString::fromLatin1( STRATA_UPDATE_KEY_ID ) )
    return fail( error, u"Unsupported update manifest."_s );
  return true;
}
bool QgsUpdateManifest::select( const QByteArray &manifest, const QString &current, const QString &platform, const QString &architecture, QgsUpdatePackage &package, QString *error )
{
  const QJsonObject root = QJsonDocument::fromJson( manifest ).object();
  const QString version = root.value( u"version"_s ).toString();
  static const QRegularExpression semver( u"^[0-9]+\\.[0-9]+\\.[0-9]+$"_s );
  if ( !semver.match( version ).hasMatch() || !semver.match( current ).hasMatch() || QVersionNumber::fromString( version ) <= QVersionNumber::fromString( current ) )
    return fail( error, u"No newer stable update is available."_s );
  for ( const QJsonValue &entry : root.value( u"packages"_s ).toArray() )
  {
    const QJsonObject item = entry.toObject();
    if ( item.value( u"platform"_s ).toString() != platform || item.value( u"architecture"_s ).toString() != architecture )
      continue;
    package
      = { version, platform, architecture, item.value( u"format"_s ).toString(), item.value( u"sha256"_s ).toString(), QUrl( item.value( u"url"_s ).toString() ), item.value( u"size"_s ).toInteger(), root };
    if (
      !isReleaseUrl( package.url )
      || !package.url.path().contains( u"/strata-v%1/"_s.arg( version ) )
      || package.size <= 0
      || package.size > 16LL * 1024 * 1024 * 1024
      || !QRegularExpression( u"^[a-f0-9]{64}$"_s ).match( package.sha256 ).hasMatch()
    )
      return fail( error, u"Invalid update package metadata."_s );
    const QString minimum = item.value( u"minimumOsVersion"_s ).toString();
    if ( !minimum.isEmpty() && QVersionNumber::fromString( QSysInfo::productVersion() ) < QVersionNumber::fromString( minimum ) )
      return fail( error, u"This update requires a newer operating system."_s );
    const QString expected = platform == "macos"_L1 ? u"dmg"_s : platform == "linux"_L1 ? u"AppImage"_s : platform == "windows-portable"_L1 ? u"zip"_s : u"exe"_s;
    if ( package.format != expected )
      return fail( error, u"Unsupported update format."_s );
    return true;
  }
  return fail( error, u"No compatible update package is available for this installation."_s );
}
bool QgsUpdateManifest::verifyFile( const QString &path, const QgsUpdatePackage &package, QString *error )
{
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) || file.size() != package.size )
    return fail( error, u"Update download is incomplete."_s );
  QCryptographicHash hash( QCryptographicHash::Sha256 );
  if ( !hash.addData( &file ) || QString::fromLatin1( hash.result().toHex() ) != package.sha256 )
    return fail( error, u"Update checksum verification failed."_s );
  return true;
}
QString QgsUpdateManifest::platform()
{
#ifdef Q_OS_WIN
  return QFile::exists( QCoreApplication::applicationDirPath() + u"/../Uninstall.exe"_s ) ? u"windows"_s : u"windows-portable"_s;
#elif defined( Q_OS_MACOS )
  return u"macos"_s;
#else
  return u"linux"_s;
#endif
}
