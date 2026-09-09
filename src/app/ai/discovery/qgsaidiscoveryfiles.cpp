// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaidiscoveryfiles.h"

#include <memory>
#include <zip.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>

using namespace Qt::StringLiterals;
namespace QgsAiDiscoveryFiles
{
  bool validDigest( const QString &digest )
  {
    return QRegularExpression( u"^[a-fA-F0-9]{64}$"_s ).match( digest ).hasMatch();
  }
  bool safeUrl( const QUrl &url )
  {
    const bool local = url.host() == "localhost"_L1 || url.host() == "127.0.0.1"_L1 || url.host() == "::1"_L1;
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && ( url.scheme() == "https"_L1 || ( local && url.scheme() == "http"_L1 ) );
  }
  bool safeName( const QString &name )
  {
    if ( name.isEmpty() || name.size() > 512 || name.startsWith( '/' ) || name.contains( '\\' ) || name.contains( ':' ) || name.contains( QChar::Null ) )
      return false;
    for ( const auto &part : name.split( '/' ) )
      if ( part.isEmpty() || part == "."_L1 || part == ".."_L1 || part.endsWith( ' ' ) || part.endsWith( '.' ) || part.contains( QRegularExpression( u"[\\x00-\\x1f]"_s ) ) )
        return false;
    return true;
  }
  QString destinationError( const QString &root, const QString &destination )
  {
    const QString base = QDir( root ).absolutePath();
    const QString target = QDir::cleanPath( destination );
    if ( root.isEmpty() || !QDir::isAbsolutePath( destination ) || !target.startsWith( base + '/' ) )
      return u"Destinazione esterna al workspace."_s;
    QFileInfo ancestor( target );
    while ( ancestor.absoluteFilePath() != base )
    {
      if ( ancestor.isSymLink() )
        return u"La destinazione attraversa un collegamento simbolico."_s;
      ancestor.setFile( ancestor.absolutePath() );
    }
    if ( QFileInfo( base ).isSymLink() )
      return u"Workspace simbolico non supportato per il download."_s;
    return {};
  }
  static QByteArray readEntry( zip_t *archive, zip_uint64_t index, qint64 limit )
  {
    zip_stat_t info;
    zip_stat_init( &info );
    if ( zip_stat_index( archive, index, 0, &info ) || info.size > static_cast<quint64>( limit ) )
      return {};
    std::unique_ptr<zip_file_t, decltype( &zip_fclose )> file( zip_fopen_index( archive, index, 0 ), zip_fclose );
    if ( !file )
      return {};
    QByteArray result;
    char chunk[65536];
    zip_int64_t count;
    while ( ( count = zip_fread( file.get(), chunk, sizeof( chunk ) ) ) > 0 )
    {
      result.append( chunk, count );
      if ( result.size() > limit )
        return {};
    }
    return count < 0 || result.size() != static_cast<qint64>( info.size ) ? QByteArray() : result;
  }
  static bool approvedLayer( const QJsonObject &layer, const QJsonArray &approved )
  {
    for ( const auto &value : approved )
    {
      const auto c = value.toObject();
      const QString key = c.value( u"key"_s ).toString();
      if ( key.isEmpty() )
        continue;
      if ( layer.value( u"mode"_s ) != c.value( u"mode"_s ) )
        continue;
      if ( c.value( u"mode"_s ) == "file"_L1 && layer.value( u"id"_s ).toString().startsWith( key + ':' ) && layer.value( u"path"_s ).toString().startsWith( u"layers/%1/"_s.arg( key ) ) )
        return true;
      const auto d = c.value( u"distribution"_s ).toObject();
      if ( c.value( u"mode"_s ) == "service"_L1
           && layer.value( u"id"_s ) == key
           && layer.value( u"url"_s ) == d.value( u"url"_s )
           && layer.value( u"protocol"_s ) == d.value( u"protocol"_s )
           && layer.value( u"typeName"_s ) == d.value( u"typeName"_s ) )
        return true;
    }
    return false;
  }
  Result unpack( const QString &zipPath, const QString &directory, const QString &expectedHash, const QJsonArray &approved )
  {
    auto failure = [&]( const QString &error ) -> Result { return { {}, error }; };
    QFile input( zipPath );
    QCryptographicHash digest( QCryptographicHash::Sha256 );
    if ( !validDigest( expectedHash )
         || input.size() > MaxKitBytes
         || !input.open( QIODevice::ReadOnly )
         || !digest.addData( &input )
         || QString::fromLatin1( digest.result().toHex() ) != expectedHash.toLower() )
      return failure( u"Hash del kit non valido."_s );
    input.close();
    int error = 0;
    std::unique_ptr<zip_t, decltype( &zip_discard )> zip( zip_open( QFile::encodeName( zipPath ).constData(), ZIP_RDONLY | ZIP_CHECKCONS, &error ), zip_discard );
    if ( !zip )
      return failure( u"Archivio corrotto."_s );
    const auto count = zip_get_num_entries( zip.get(), 0 );
    if ( count < 1 || count > 4096 )
      return failure( u"Numero di file non consentito."_s );
    QSet<QString> names;
    quint64 expanded = 0;
    for ( zip_int64_t i = 0; i < count; ++i )
    {
      zip_stat_t stat;
      zip_stat_init( &stat );
      if ( zip_stat_index( zip.get(), i, 0, &stat ) )
        return failure( u"Indice ZIP illeggibile."_s );
      const QString name = QString::fromUtf8( stat.name );
      zip_uint8_t opsys;
      zip_uint32_t attrs;
      if ( zip_file_get_external_attributes( zip.get(), i, 0, &opsys, &attrs ) )
        return failure( u"Attributi ZIP illeggibili."_s );
      if ( !safeName( name )
           || names.contains( name.toCaseFolded() )
           || stat.encryption_method != ZIP_EM_NONE
           || ( opsys == ZIP_OPSYS_UNIX && ( ( attrs >> 16 ) & 0170000 ) == 0120000 )
           || stat.size > MaxKitBytes )
        return failure( u"Percorso, collisione o struttura ZIP non consentiti."_s );
      expanded += stat.size;
      if ( expanded > static_cast<quint64>( MaxKitBytes ) )
        return failure( u"Il kit decompresso supera 2 GiB."_s );
      names.insert( name.toCaseFolded() );
    }
    const auto manifestIndex = zip_name_locate( zip.get(), "manifest.json", 0 );
    const auto manifest = QJsonDocument::fromJson( readEntry( zip.get(), manifestIndex, 1048576 ) ).object();
    if ( manifest.value( u"schemaVersion"_s ) != "strata-discovery/1"_L1 )
      return failure( u"Manifest assente o versione non supportata."_s );
    QMap<QString, QJsonObject> files;
    for ( const auto &value : manifest.value( u"files"_s ).toArray() )
    {
      const auto file = value.toObject();
      const QString name = file.value( u"path"_s ).toString();
      if ( !safeName( name ) || files.contains( name ) || !validDigest( file.value( u"sha256"_s ).toString() ) )
        return failure( u"Manifest con file non validi."_s );
      bool selected = false;
      for ( const auto &v : approved )
        if ( v.toObject().value( u"mode"_s ) == "file"_L1 && name.startsWith( u"layers/%1/"_s.arg( v.toObject().value( u"key"_s ).toString() ) ) )
          selected = true;
      if ( !selected || !QRegularExpression( u"\\.(geojson|gpkg|shp|shx|dbf|prj|cpg|qix|sbn|sbx|tif|tiff)$"_s, QRegularExpression::CaseInsensitiveOption ).match( name ).hasMatch() )
        return failure( u"File non autorizzato nel manifest."_s );
      files.insert( name, file );
    }
    const auto layers = manifest.value( u"layers"_s ).toArray();
    if ( layers.isEmpty() )
      return failure( u"Nessun layer nel kit."_s );
    for ( const auto &value : layers )
    {
      const auto layer = value.toObject();
      if ( !approvedLayer( layer, approved ) || ( layer.value( u"mode"_s ) == "file"_L1 && !files.contains( layer.value( u"path"_s ).toString() ) ) )
        return failure( u"Il kit contiene layer esterni alla selezione approvata."_s );
    }
    if ( names.size() != files.size() + 2 || !names.contains( u"readme.txt"_s ) )
      return failure( u"File estranei al manifest."_s );
    // Recover an interrupted import only if all persisted files still match the freshly verified kit.
    if ( QFileInfo::exists( directory ) )
    {
      for ( auto it = files.begin(); it != files.end(); ++it )
      {
        const QString path = QDir( directory ).filePath( it.key() );
        if ( !destinationError( directory, path ).isEmpty() )
          return failure( u"Percorso di ripresa non sicuro."_s );
        QFile existing( path );
        QCryptographicHash hash( QCryptographicHash::Sha256 );
        if ( existing.size() != it.value().value( u"sizeBytes"_s ).toInteger()
             || !existing.open( QIODevice::ReadOnly )
             || !hash.addData( &existing )
             || hash.result().toHex() != it.value().value( u"sha256"_s ).toString().toLatin1().toLower() )
          return failure( u"La cartella esistente non corrisponde al kit; scegliere una nuova destinazione."_s ); // # spellok: Italian UI text.
      }
      return { manifest, {} };
    }
    if ( QFileInfo::exists( directory ) || !QDir().mkpath( directory ) )
      return failure( u"Cartella di importazione già esistente o non scrivibile."_s );
    // Only a new directory is ever written. Stream bounded chunks; CRC is checked by libzip at EOF.
    auto writeFailure = [&]( const QString &message ) {
      QDir( directory ).removeRecursively();
      return failure( message );
    };
    for ( auto it = files.begin(); it != files.end(); ++it )
    {
      std::unique_ptr<zip_file_t, decltype( &zip_fclose )> file( zip_fopen( zip.get(), it.key().toUtf8().constData(), 0 ), zip_fclose );
      const QString path = QDir( directory ).filePath( it.key() );
      if ( !file || !QDir().mkpath( QFileInfo( path ).absolutePath() ) )
        return writeFailure( u"File del manifest assente."_s );
      QSaveFile out( path );
      if ( !out.open( QIODevice::WriteOnly ) )
        return writeFailure( out.errorString() );
      QCryptographicHash hash( QCryptographicHash::Sha256 );
      char chunk[65536];
      zip_int64_t n;
      qint64 bytes = 0;
      while ( ( n = zip_fread( file.get(), chunk, sizeof( chunk ) ) ) > 0 )
      {
        bytes += n;
        if ( bytes > MaxKitBytes || bytes > it.value().value( u"sizeBytes"_s ).toInteger() || out.write( chunk, n ) != n )
          return writeFailure( u"Dimensione file non valida."_s );
        hash.addData( QByteArrayView( chunk, n ) );
      }
      if ( n < 0 || bytes != it.value().value( u"sizeBytes"_s ).toInteger() || hash.result().toHex() != it.value().value( u"sha256"_s ).toString().toLatin1().toLower() || !out.commit() )
        return writeFailure( u"Hash, CRC o dimensione file non validi."_s );
    }
    QSaveFile saved( QDir( directory ).filePath( u"manifest.json"_s ) );
    if ( !saved.open( QIODevice::WriteOnly ) || saved.write( QJsonDocument( manifest ).toJson() ) < 0 || !saved.commit() )
      return writeFailure( u"Manifest non salvato."_s );
    return { manifest, {} };
  }
} //namespace QgsAiDiscoveryFiles
