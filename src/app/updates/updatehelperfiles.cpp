#include "updatehelper.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <zip.h>
#ifdef Q_OS_MACOS
#include <QDirIterator>
#include <QProcess>
#include <filesystem>
#endif
using namespace Qt::StringLiterals;
#ifdef Q_OS_MACOS
namespace
{
  bool remainsInside( const QDir &root, const QString &path )
  {
    const QString relative = root.relativeFilePath( path );
    return !path.isEmpty() && relative != ".."_L1 && !relative.startsWith( "../"_L1 ) && !QDir::isAbsolutePath( relative );
  }

  bool isSafeBundleLink( const QFileInfo &entry, const QDir &packageRoot )
  {
    const QString resolved = entry.canonicalFilePath();
    if ( !remainsInside( packageRoot, resolved ) )
      return false;
    std::error_code error;
    const auto link = std::filesystem::read_symlink( QFile::encodeName( entry.filePath() ).constData(), error );
    if ( error || link.is_absolute() )
      return false;
    const QString lexicalTarget = QDir::cleanPath( entry.absolutePath() + u'/' + QFile::decodeName( link.c_str() ) );
    return remainsInside( packageRoot, lexicalTarget );
  }
} // namespace
#endif

bool UpdateHelper::copyTree( const QString &source, const QString &target, QString *error, bool overwrite )
{
#ifdef Q_OS_MACOS
  Q_UNUSED( overwrite )
  const QString canonicalSource = QFileInfo( source ).canonicalFilePath();
  if ( canonicalSource.isEmpty() )
  {
    *error = u"Cannot locate update bundle."_s;
    return false;
  }
  const QDir bundle( canonicalSource );
  QDirIterator entries( canonicalSource, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories );
  while ( entries.hasNext() )
  {
    const QFileInfo entry = entries.nextFileInfo();
    if ( entry.isSymLink() && !isSafeBundleLink( entry, bundle ) )
    {
      *error = u"Unsafe package symlink."_s;
      return false;
    }
  }
  // Signatures on scripts are stored in extended attributes. Preserve them
  // along with the original symlinks before verifying the staged bundle.
  QProcess copy;
  copy.start( u"/usr/bin/ditto"_s, { u"--extattr"_s, canonicalSource, target } );
  if ( !copy.waitForFinished( 120000 ) || copy.exitStatus() != QProcess::NormalExit || copy.exitCode() != 0 )
  {
    *error = u"Cannot stage the signed macOS bundle."_s;
    return false;
  }
#else
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
#endif
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
