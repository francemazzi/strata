#include "updatehelper.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <zip.h>
using namespace Qt::StringLiterals;
bool UpdateHelper::copyTree( const QString &source, const QString &target, QString *error, bool overwrite )
{
  if ( !QDir().mkpath( target ) )
  {
    *error = u"Cannot create staging directory."_s;
    return false;
  }
  const QDir directory( source );
  for ( const auto &entry : directory.entryInfoList( QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System ) )
  {
    const QString destination = target + '/' + entry.fileName();
    if ( entry.isSymLink() )
    {
      const QString resolved = entry.canonicalFilePath();
      // Framework symlinks are preserved only when their target remains inside the package tree.
      const QString relative = directory.relativeFilePath( resolved );
      if ( relative.startsWith( "../"_L1 ) || resolved.isEmpty() || !QFile::link( relative, destination ) )
      {
        *error = u"Unsafe package symlink."_s;
        return false;
      }
    }
    else if ( entry.isDir() )
    {
      if ( !copyTree( entry.filePath(), destination, error, overwrite ) )
        return false;
    }
    else
    {
      if ( overwrite && QFileInfo::exists( destination ) && !QFile::remove( destination ) )
      {
        *error = u"Cannot replace a staged file."_s;
        return false;
      }
      if ( !QFile::copy( entry.filePath(), destination ) )
      {
        *error = u"Cannot stage package files."_s;
        return false;
      }
      QFile::setPermissions( destination, entry.permissions() );
    }
  }
  return true;
}
bool UpdateHelper::extractZip( const QString &source, const QString &target, QString *error )
{
  int code = 0;
  zip_t *archive = zip_open( source.toUtf8().constData(), ZIP_RDONLY, &code );
  if ( !archive )
  {
    *error = u"Cannot open portable update archive."_s;
    return false;
  }
  bool ok = true;
  qint64 total = 0;
  const auto count = zip_get_num_entries( archive, 0 );
  for ( zip_int64_t i = 0; i < count && ok; ++i )
  {
    zip_stat_t stat;
    if ( zip_stat_index( archive, i, 0, &stat ) != 0 )
    {
      ok = false;
      break;
    }
    const QString name = QString::fromUtf8( stat.name );
    const QString clean = QDir::cleanPath( name );
    zip_uint8_t os;
    zip_uint32_t attributes;
    zip_file_get_external_attributes( archive, i, 0, &os, &attributes );
    const bool symlink = os == ZIP_OPSYS_UNIX && ( ( attributes >> 16 ) & 0170000 ) == 0120000;
    if ( symlink || QDir::isAbsolutePath( name ) || name.contains( '\\' ) || name.contains( ':' ) || clean == ".."_L1 || clean.startsWith( "../"_L1 ) || ( total += stat.size ) > 32LL * 1024 * 1024 * 1024 )
    {
      ok = false;
      break;
    }
    const QString path = target + '/' + clean;
    if ( name.endsWith( '/' ) )
    {
      ok = QDir().mkpath( path );
      continue;
    }
    ok = QDir().mkpath( QFileInfo( path ).absolutePath() );
    QFile output( path );
    zip_file_t *input = zip_fopen_index( archive, i, 0 );
    if ( !input || !output.open( QIODevice::WriteOnly | QIODevice::NewOnly ) )
    {
      if ( input )
        zip_fclose( input );
      ok = false;
      break;
    }
    char buffer[65536];
    zip_int64_t length;
    while ( ( length = zip_fread( input, buffer, sizeof( buffer ) ) ) > 0 )
      if ( output.write( buffer, length ) != length )
      {
        ok = false;
        break;
      }
    if ( length < 0 || static_cast<zip_uint64_t>( output.size() ) != stat.size )
      ok = false;
    zip_fclose( input );
  }
  zip_close( archive );
  if ( !ok )
    *error = u"Portable archive is unsafe, damaged, or could not be extracted."_s;
  return ok;
}
