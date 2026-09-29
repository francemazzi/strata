#include "qgsupdateservice.h"
#include "qgssettings.h"
#include "qgsconfig.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QProcess>
#include <QStandardPaths>
using namespace Qt::StringLiterals;
namespace
{
  bool write( const QString &path, const QByteArray &data )
  {
    QSaveFile file( path );
    return file.open( QIODevice::WriteOnly ) && file.write( data ) == data.size() && file.commit();
  }
} //namespace
bool QgsUpdateService::prepareInstall( QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return false;
  };
  if ( mState != "ready"_L1 )
    return fail( tr( "Download and verify an update first." ) );
  QString target, executable = QCoreApplication::applicationFilePath();
#ifdef Q_OS_MACOS
  target = QDir::cleanPath( QCoreApplication::applicationDirPath() + u"/../.."_s );
  if ( !target.endsWith( ".app"_L1 ) || target.startsWith( "/Volumes/"_L1 ) )
    return fail( tr( "Install Strata in Applications before updating." ) );
#elif defined( Q_OS_WIN )
  target = QDir::cleanPath( QCoreApplication::applicationDirPath() + u"/.."_s );
#else
  target = qEnvironmentVariable( "APPIMAGE" );
  if ( target.isEmpty() )
    return fail( tr( "Use your package manager to update this installation. In-app updates require the official AppImage." ) );
  executable = target;
#endif
  if ( !QFileInfo( target ).exists() )
    return fail( tr( "The installed application could not be located." ) );
  if ( mPackage.platform != "windows"_L1 && !QFileInfo( QFileInfo( target ).absolutePath() ).isWritable() )
    return fail( tr( "This installation is managed or not writable. Ask its administrator to update it." ) );
  const QString helper = QCoreApplication::applicationDirPath()
                         + u"/strata-update-helper"_s
#ifdef Q_OS_WIN
                         + u".exe"_s
#endif
    ;
  if ( !QFileInfo::exists( helper ) )
    return fail( tr( "The update helper is missing. Repair the installation before updating." ) );
  mJobPath = mDirectory.filePath( u"install.json"_s );
  const QJsonObject job {
    { u"manifest"_s, QString::fromUtf8( mManifest.toBase64() ) },
    { u"signature"_s, QString::fromLatin1( mSignature ) },
    { u"package"_s, mPackagePath },
    { u"target"_s, QFileInfo( target ).canonicalFilePath() },
    { u"executable"_s, executable },
    { u"currentVersion"_s, QString::fromUtf8( STRATA_VERSION ) },
    { u"parentPid"_s, QCoreApplication::applicationPid() },
    { u"platform"_s, mPackage.platform }
  };
  if ( !write( mJobPath, QJsonDocument( job ).toJson() ) )
    return fail( tr( "Cannot write the update transaction." ) );
  return true;
}
bool QgsUpdateService::launchInstaller( QString *error )
{
  if ( mJobPath.isEmpty() )
  {
    if ( error )
      *error = tr( "Update was not prepared." );
    return false;
  }
  const QString helper = QCoreApplication::applicationDirPath()
                         + u"/strata-update-helper"_s
#ifdef Q_OS_WIN
                         + u".exe"_s
#endif
    ;
  mDirectory.setAutoRemove( false );
#ifdef Q_OS_LINUX
  const bool started = QProcess::startDetached( qEnvironmentVariable( "APPIMAGE" ), { u"--strata-apply-update"_s, mJobPath } );
#else
  const bool started = QProcess::startDetached( helper, { mJobPath } );
#endif
  if ( !started )
  {
    mDirectory.setAutoRemove( true );
    if ( error )
      *error = tr( "Unable to start the update helper." );
    return false;
  }
  QgsSettings().setValue( u"strata/updates/pendingJob"_s, mJobPath );
  return true;
}
void QgsUpdateService::acknowledgeLaunch()
{
  QgsSettings settings;
  const QString path = settings.value( u"strata/updates/pendingJob"_s ).toString();
  if ( path.isEmpty() )
    return;
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
    return;
  const auto job = QJsonDocument::fromJson( file.readAll() ).object();
  const auto manifest = QJsonDocument::fromJson( QByteArray::fromBase64( job.value( u"manifest"_s ).toString().toUtf8() ) ).object();
  if ( manifest.value( u"version"_s ).toString() == QString::fromUtf8( STRATA_VERSION ) )
  {
    write( path + u".ack"_s, QByteArray( STRATA_VERSION ) );
    settings.remove( u"strata/updates/pendingJob"_s );
    setState( u"current"_s, tr( "Strata was updated successfully to %1." ).arg( QString::fromUtf8( STRATA_VERSION ) ) );
  }
  else
  {
    QFile result( path + u".result"_s );
    if ( result.open( QIODevice::ReadOnly ) )
      setState( u"error"_s, tr( "Update did not complete: %1" ).arg( QString::fromUtf8( result.read( 4096 ) ) ) );
  }
}
