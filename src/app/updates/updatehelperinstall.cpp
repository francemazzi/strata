#include "updatehelper.h"
#include "qgsupdatemanifest.h"
#include <QJsonDocument>
#include <QSysInfo>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QThread>
#include <QTemporaryDir>
#include <QUuid>
#ifndef Q_OS_WIN
#include <stdio.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif
using namespace Qt::StringLiterals;
namespace
{
  void result( const QString &path, const QString &text )
  {
    QSaveFile file( path + u".result"_s );
    if ( file.open( QIODevice::WriteOnly ) )
    {
      file.write( text.toUtf8() );
      file.commit();
    }
  }
  bool move( const QString &from, const QString &to )
  {
    return QDir().rename( from, to );
  }
} //namespace
bool UpdateHelper::install( const QJsonObject &job, const QString &jobPath, QString *error )
{
  const QString target = job.value( u"target"_s ).toString();
  const QString package = job.value( u"package"_s ).toString();
  const QString platform = job.value( u"platform"_s ).toString();
  const QString executable = job.value( u"executable"_s ).toString();
  if ( target.isEmpty() || !QDir::isAbsolutePath( target ) || QFileInfo( target ).isSymLink() || !QFileInfo( target ).exists() )
  {
    *error = u"Invalid installed application path."_s;
    return false;
  }
  if ( platform == "linux"_L1 && ( !QFileInfo( target ).isFile() || !target.endsWith( ".AppImage"_L1, Qt::CaseInsensitive ) || executable != target ) )
  {
    *error = u"Invalid AppImage installation."_s;
    return false;
  }
  if ( platform == "macos"_L1 && ( !QFileInfo( target ).isDir() || !target.endsWith( ".app"_L1 ) ) )
  {
    *error = u"Invalid macOS installation."_s;
    return false;
  }
  if ( platform == "windows"_L1 )
  {
#ifdef Q_OS_WIN
    if ( !verifyNative( package, executable, error ) )
      return false;
    const QString args = u"/D=%1"_s.arg( target );
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof( info );
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = reinterpret_cast<LPCWSTR>( package.utf16() );
    info.lpParameters = reinterpret_cast<LPCWSTR>( args.utf16() );
    info.nShow = SW_SHOWNORMAL;
    if ( !ShellExecuteExW( &info ) || !info.hProcess )
    {
      *error = u"Installer was not started or elevation was cancelled."_s;
      return false;
    }
    WaitForSingleObject( info.hProcess, INFINITE );
    DWORD exitCode = 1;
    GetExitCodeProcess( info.hProcess, &exitCode );
    CloseHandle( info.hProcess );
    if ( exitCode != 0 )
    {
      *error = u"The Windows installer did not complete successfully."_s;
      return false;
    }
    if ( !QProcess::startDetached( executable, {} ) )
    {
      *error = u"Installed; restart Strata to verify the new version."_s;
      return false;
    }
    result( jobPath, u"Installer finished; waiting for version verification."_s );
    return true;
#else
    *error = u"Invalid platform."_s;
    return false;
#endif
  }
  const QString suffix = QUuid::createUuid().toString( QUuid::WithoutBraces );
  const QString stage = target + u".update-"_s + suffix;
  const QString backup = target + u".previous-"_s + suffix;
  if ( platform == "linux"_L1 )
  {
    if ( !QFile::copy( package, stage ) || !QFile::setPermissions( stage, QFileInfo( target ).permissions() | QFile::ExeOwner ) )
    {
      *error = u"Cannot stage AppImage."_s;
      return false;
    }
  }
  else if ( platform == "macos"_L1 )
  {
#ifdef Q_OS_MACOS
    QTemporaryDir mount;
    QProcess attach;
    attach.start( u"/usr/bin/hdiutil"_s, { u"attach"_s, u"-readonly"_s, u"-nobrowse"_s, u"-mountpoint"_s, mount.path(), package } );
    if ( !attach.waitForFinished( 120000 ) || attach.exitCode() != 0 )
    {
      *error = u"Cannot mount the update disk image."_s;
      return false;
    }
    const QString app = mount.filePath( u"Strata.app"_s );
    const bool valid = verifyNative( app, target, error ) && copyTree( app, stage, error );
    QProcess::execute( u"/usr/bin/hdiutil"_s, { u"detach"_s, mount.path() } );
    if ( !valid )
      return false;
    if ( !verifyNative( stage, target, error ) )
      return false;
#else
    *error = u"Invalid platform."_s;
    return false;
#endif
  }
  else if ( platform == "windows-portable"_L1 )
  {
    QTemporaryDir extracted( QFileInfo( target ).absolutePath() + u"/.strata-extract-XXXXXX"_s );
    if ( !extracted.isValid() || !extractZip( package, extracted.path(), error ) )
      return false;
    QString root = extracted.path();
    const auto directories = QDir( root ).entryList( QDir::Dirs | QDir::NoDotAndDotDot );
    if ( !QFileInfo::exists( root + u"/bin/strata.exe"_s ) && directories.size() == 1 )
      root += '/' + directories.first();
    const QString relative = QDir( target ).relativeFilePath( executable );
    if ( relative.startsWith( "../"_L1 ) || !verifyNative( root + '/' + relative, executable, error ) || !copyTree( target, stage, error ) || !copyTree( root, stage, error, true ) )
      return false;
  }
  else
  {
    *error = u"Unsupported installation."_s;
    return false;
  }
  if ( platform == "linux"_L1 )
  {
    QgsUpdatePackage selected;
    const QByteArray manifest = QByteArray::fromBase64( job.value( u"manifest"_s ).toString().toUtf8() );
    if ( !QgsUpdateManifest::select( manifest, job.value( u"currentVersion"_s ).toString(), platform, QSysInfo::buildCpuArchitecture(), selected, error ) || !QgsUpdateManifest::verifyFile( stage, selected, error ) )
      return false;
  }
  bool replacedAtomically = false;
#ifdef Q_OS_LINUX
  // The application path must always exist, even if power is lost during replacement.
  if ( platform == "linux"_L1 )
  {
    if ( !QFile::copy( target, backup ) || ::rename( QFile::encodeName( stage ).constData(), QFile::encodeName( target ).constData() ) != 0 )
    {
      *error = u"Cannot atomically replace the AppImage. Previous application retained."_s;
      return false;
    }
    replacedAtomically = true;
  }
#elif defined( Q_OS_MACOS )
  if ( platform == "macos"_L1 )
  {
    if ( ::renamex_np( QFile::encodeName( stage ).constData(), QFile::encodeName( target ).constData(), RENAME_SWAP ) != 0 )
    {
      *error = u"Cannot atomically exchange the application bundles."_s;
      return false;
    }
    // After the atomic exchange the previous application resides at stage.
    if ( !move( stage, backup ) )
    {
      ::renamex_np( QFile::encodeName( stage ).constData(), QFile::encodeName( target ).constData(), RENAME_SWAP );
      *error = u"Cannot retain the application backup. Update cancelled."_s;
      return false;
    }
    replacedAtomically = true;
  }
#endif
  if ( !replacedAtomically && !move( target, backup ) )
  {
    *error = u"Cannot move the current application. No update was installed."_s;
    return false;
  }
  if ( !replacedAtomically && !move( stage, target ) )
  {
    move( backup, target );
    *error = u"Cannot install update; previous version restored."_s;
    return false;
  }
  qint64 newPid = 0;
  const bool launched = QProcess::startDetached( executable, {}, QFileInfo( executable ).absolutePath(), &newPid );
  if ( !launched )
  {
    move( target, stage );
    move( backup, target );
    *error = u"The new application could not start. Previous version restored."_s;
    return false;
  }
  result( jobPath, u"Application replaced; waiting for startup confirmation. Backup: %1"_s.arg( backup ) );
  for ( int i = 0; i < 180; ++i )
  {
    if ( QFileInfo::exists( jobPath + u".ack"_s ) )
    {
      // Preserve backup for explicit recovery; never delete unknown application/user files.
      result( jobPath, u"Update confirmed. Previous application retained at %1"_s.arg( backup ) );
      return true;
    }
    if ( !parentAlive( newPid ) )
    {
      if ( move( target, stage ) && move( backup, target ) )
        *error = u"The new application exited before startup confirmation. Previous version restored."_s;
      else
        *error = u"Startup failed. Restore the retained backup at %1."_s.arg( backup );
      return false;
    }
    QThread::sleep( 1 );
  }
  *error = u"Startup was not confirmed. Backup retained at %1. Close Strata before restoring it."_s.arg( backup );
  return false;
}
