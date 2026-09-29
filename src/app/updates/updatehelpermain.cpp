#include "updatehelper.h"
#include "qgsupdatemanifest.h"
#include <QtCrypto>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSysInfo>
#include <QThread>
#include <QProcess>
using namespace Qt::StringLiterals;
int main( int argc, char **argv )
{
  QCoreApplication app( argc, argv );
  QCA::Initializer crypto;
  if ( app.arguments().size() != 2 )
    return 2;
  const QString path = app.arguments().at( 1 );
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) || file.size() > 2 * 1024 * 1024 )
    return 2;
  const QJsonObject job = QJsonDocument::fromJson( file.readAll() ).object();
  file.close();
  QLockFile lock( path + u".lock"_s );
  if ( !lock.tryLock() )
    return 2;
  const auto fail = [&path]( const QString &error ) {
    QSaveFile result( path + u".result"_s );
    if ( result.open( QIODevice::WriteOnly ) )
    {
      result.write( error.toUtf8() );
      result.commit();
    }
    return 1;
  };
  const QByteArray manifest = QByteArray::fromBase64( job.value( u"manifest"_s ).toString().toUtf8() );
  QgsUpdatePackage package;
  QString error;
  if (
    !QgsUpdateManifest::verify( manifest, job.value( u"signature"_s ).toString().toUtf8(), &error )
    || !QgsUpdateManifest::select( manifest, job.value( u"currentVersion"_s ).toString(), job.value( u"platform"_s ).toString(), QSysInfo::buildCpuArchitecture(), package, &error )
    || !QgsUpdateManifest::verifyFile( job.value( u"package"_s ).toString(), package, &error )
  )
    return fail( error );
#ifdef Q_OS_WIN
  // Run from a private copy: Windows cannot replace loaded executables or DLLs.
  // Copying is performed by the helper, never on the application's UI thread.
  const QString target = QFileInfo( job.value( u"target"_s ).toString() ).canonicalFilePath();
  const QString runtime = QFileInfo( path ).absolutePath() + u"/helper-runtime"_s;
  const QString relocated = runtime + u"/bin/strata-update-helper.exe"_s;
  if ( QDir::cleanPath( QCoreApplication::applicationFilePath() ).compare( QDir::cleanPath( relocated ), Qt::CaseInsensitive ) != 0 )
  {
    const QString installed = QFileInfo( QCoreApplication::applicationDirPath() + u"/.."_s ).canonicalFilePath();
    if ( target.isEmpty() || target.compare( installed, Qt::CaseInsensitive ) != 0 )
      return fail( u"The update target is not this helper's installation."_s );
    if ( QFileInfo::exists( runtime ) || !UpdateHelper::copyTree( target, runtime, &error ) )
      return fail( error.isEmpty() ? u"A helper runtime already exists; prepare a new update."_s : error );
    lock.unlock();
    if ( !QProcess::startDetached( relocated, { path }, QFileInfo( relocated ).absolutePath() ) )
      return fail( u"Cannot start the isolated update helper."_s );
    return 0;
  }
#endif
  const qint64 pid = job.value( u"parentPid"_s ).toInteger();
  if ( pid <= 1 || pid == QCoreApplication::applicationPid() )
    return fail( u"Invalid parent process."_s );
  for ( int i = 0; i < 120 && UpdateHelper::parentAlive( pid ); ++i )
    QThread::sleep( 1 );
  if ( UpdateHelper::parentAlive( pid ) )
    return fail( u"Strata did not close. Update cancelled."_s );
  if ( !QgsUpdateManifest::verifyFile( job.value( u"package"_s ).toString(), package, &error ) )
    return fail( error );
  if ( !UpdateHelper::install( job, path, &error ) )
  {
    const int status = fail( error );
    // Never open a second instance if startup is merely slow; that case retains a backup.
    if ( !error.startsWith( u"Startup was not confirmed"_s ) )
      QProcess::startDetached( job.value( u"executable"_s ).toString(), {} );
    return status;
  }
  return 0;
}
