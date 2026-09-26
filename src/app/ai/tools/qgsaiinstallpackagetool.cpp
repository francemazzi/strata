/***************************************************************************
    qgsaiinstallpackagetool.cpp
    ---------------------------
    begin                : May 2026
    copyright            : (C) 2026 by Francesco Mazzi
    email                : francemazzi at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsaiinstallpackagetool.h"

#include "qgsaiauditlog.h"
#include "qgsaipipinstallapprovaldialog.h"
#include "qgsaipythonruntime.h"
#include "qgsaitaskrunner.h"
#include "qgsaitoolschemautil.h"
#include "qgsaiworkspacetrust.h"
#include "qgsmessagelog.h"
#include "qgspythonrunner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace
{
  QString truncatePipOutput( const QString &text, int maxBytes )
  {
    const QByteArray utf8 = text.toUtf8();
    if ( utf8.size() <= maxBytes )
      return text;
    return QString::fromUtf8( utf8.left( maxBytes ) ) + u"\n…[truncated]"_s;
  }

  bool isAllowedSpec( const QString &spec )
  {
    static const QRegularExpression re( uR"(^[A-Za-z0-9_.\-]+([<>=!~]+[A-Za-z0-9_.\-]+)?$)"_s );
    return re.match( spec ).hasMatch();
  }
} // namespace

QgsAiInstallPythonPackageTool::QgsAiInstallPythonPackageTool( QWidget *dialogParent )
  : mDialogParent( dialogParent )
{}

bool QgsAiInstallPythonPackageTool::isAvailable() const
{
  return QgsPythonRunner::isValid() && QgsAiWorkspaceTrust::isCurrentWorkspaceTrusted();
}

QString QgsAiInstallPythonPackageTool::availabilityReason() const
{
  if ( !QgsPythonRunner::isValid() )
  {
    const QString reason = QgsPythonRunner::unavailableReason();
    return reason.isEmpty()
             ? u"Python package installation is not available because the QGIS Python runner is unavailable. Start QGIS with Python enabled (do not use --nopython), build with WITH_BINDINGS, and verify that the qgispython support library loads."_s
             : u"Python package installation is not available: %1"_s.arg( reason );
  }
  return u"install_python_package is disabled because this workspace is not trusted. Trust the workspace from the AI provider settings to enable package installation."_s;
}

QString QgsAiInstallPythonPackageTool::description() const
{
  return QStringLiteral(
    "Installs one or more Python packages into an isolated directory in the active QGIS "
    "profile using the bundled Python interpreter and 'pip install --target'. The user must approve the exact list of "
    "packages via a modal dialog before anything is installed; refusal returns 'user_rejected'. "
    "Use this only when the requested module is not already importable. Bundled packages such as "
    "DuckDB are never reinstalled. Successful installs are cached to avoid duplicate pip calls. "
    "After installation, call run_python to use the library; its isolated path is bootstrapped automatically. "
    "PEP 668 externally-managed errors are terminal and must not be retried. "
    "Each spec must be a plain pip requirement of the form name[<op>version]. URLs, git refs, "
    "and '-r requirements.txt' are not accepted. Maximum 10 packages per call."
  );
}

QJsonObject QgsAiInstallPythonPackageTool::schema() const
{
  QJsonObject packagesProp;
  packagesProp.insert( u"type"_s, u"array"_s );
  packagesProp.insert( u"description"_s, u"Pip specs to install. Each item must match ^[A-Za-z0-9_.\\-]+([<>=!~]+[A-Za-z0-9_.\\-]+)?$ — for example 'geopy', 'osmnx==1.9.3', 'requests>=2.31'."_s );
  QJsonObject itemSchema;
  itemSchema.insert( u"type"_s, u"string"_s );
  packagesProp.insert( u"items"_s, itemSchema );
  packagesProp.insert( u"minItems"_s, 1 );
  packagesProp.insert( u"maxItems"_s, MAX_PACKAGES );

  QJsonObject properties;
  properties.insert( u"packages"_s, packagesProp );
  properties.insert( u"reason"_s, prop( u"string"_s, u"Short human-readable explanation of why these packages are needed. Shown in the approval dialog."_s ) );
  return schemaObject( properties, QJsonArray { u"packages"_s } );
}

namespace
{
  //! The pip-enabled interpreter found by the first install of the session.
  QString &installToolKnownPython()
  {
    static QString path;
    return path;
  }

  //! Adds \a specs to the "successful_specs" of the install cache, as the wrapper does.
  void installToolRecordInstalled( const QString &cachePath, const QStringList &specs )
  {
    QSet<QString> successful;
    QFile existing( cachePath );
    if ( existing.open( QIODevice::ReadOnly ) )
    {
      for ( const QJsonValue &spec : QJsonDocument::fromJson( existing.readAll() ).object().value( u"successful_specs"_s ).toArray() )
        successful.insert( spec.toString() );
      existing.close();
    }
    for ( const QString &spec : specs )
      successful.insert( spec );
    QStringList sorted( successful.cbegin(), successful.cend() );
    sorted.sort();
    QJsonObject cache;
    cache.insert( u"version"_s, 1 );
    cache.insert( u"successful_specs"_s, QJsonArray::fromStringList( sorted ) );
    QDir().mkpath( QFileInfo( cachePath ).absolutePath() );
    QSaveFile file( cachePath );
    if ( file.open( QIODevice::WriteOnly ) )
    {
      file.write( QJsonDocument( cache ).toJson( QJsonDocument::Compact ) );
      file.commit();
    }
  }
} // namespace

QgsAiToolResult QgsAiInstallPythonPackageTool::execute( const QJsonObject &args )
{
  const QJsonValue packagesValue = args.value( u"packages"_s );
  if ( !packagesValue.isArray() )
    return QgsAiToolResult::error( u"Argument 'packages' is required and must be an array of strings."_s );

  const QJsonArray packagesArray = packagesValue.toArray();
  if ( packagesArray.isEmpty() )
    return QgsAiToolResult::error( u"Argument 'packages' must contain at least one entry."_s );
  if ( packagesArray.size() > MAX_PACKAGES )
    return QgsAiToolResult::error( u"Refusing to install more than %1 packages in a single call (got %2)."_s.arg( MAX_PACKAGES ).arg( packagesArray.size() ) );

  QStringList packages;
  packages.reserve( packagesArray.size() );
  for ( const QJsonValue &value : packagesArray )
  {
    if ( !value.isString() )
      return QgsAiToolResult::error( u"Every entry in 'packages' must be a string."_s );
    const QString spec = value.toString().trimmed();
    if ( spec.isEmpty() )
      return QgsAiToolResult::error( u"Empty pip spec is not allowed."_s );
    if ( spec.size() > MAX_SPEC_CHARS )
      return QgsAiToolResult::error( u"Pip spec exceeds %1 characters: '%2'."_s.arg( MAX_SPEC_CHARS ).arg( spec.left( 60 ) ) );
    if ( !isAllowedSpec( spec ) )
      return QgsAiToolResult::error( u"Pip spec '%1' is not in the allowed form name[<op>version]. URLs, git refs and '-r' are blocked."_s.arg( spec ) );
    packages << spec;
  }

  if ( !QgsPythonRunner::isValid() )
    return QgsAiToolResult::error( availabilityReason() );

  QgsAiPipInstallApprovalDialog dialog( packages, args.value( u"reason"_s ).toString(), mDialogParent );
  if ( dialog.exec() != QDialog::Accepted )
  {
    QgsMessageLog::logMessage( u"install_python_package rejected by user (packages=%1)"_s.arg( packages.size() ), u"AI/Pip"_s, Qgis::MessageLevel::Info, false );
    QJsonObject output;
    output.insert( u"status"_s, u"user_rejected"_s );
    return QgsAiToolResult::ok( output );
  }

  const QString uniqueId = QUuid::createUuid().toString( QUuid::WithoutBraces );
  const QDir temporaryDirectory( QDir::tempPath() );
  const QString argumentsPath = temporaryDirectory.filePath( u"qgsai_pipargs_%1.json"_s.arg( uniqueId ) );
  const QString outputPath = temporaryDirectory.filePath( u"qgsai_pipout_%1.json"_s.arg( uniqueId ) );
  const QString wrapperPath = temporaryDirectory.filePath( u"qgsai_pipwrapper_%1.py"_s.arg( uniqueId ) );

  QJsonArray jsonPackages;
  for ( const QString &package : packages )
    jsonPackages.append( package );
  // The wrapper only prepares the pip command (quick); pip itself runs below, in the background.
  // The interpreter found once is reused for the session instead of being probed again.
  QJsonObject wrapperArguments;
  wrapperArguments.insert( u"packages"_s, jsonPackages );
  wrapperArguments.insert( u"known_python"_s, installToolKnownPython() );
  wrapperArguments.insert( u"prepare_only"_s, true );
  QFile argumentsFile( argumentsPath );
  if ( !argumentsFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    return QgsAiToolResult::error( u"Cannot write temp pip-args file: %1"_s.arg( argumentsPath ) );
  argumentsFile.write( QJsonDocument( wrapperArguments ).toJson( QJsonDocument::Compact ) );
  argumentsFile.close();

  QgsMessageLog::logMessage( u"install_python_package: executing approved install (packages=%1)"_s.arg( packages.size() ), u"AI/Pip"_s, Qgis::MessageLevel::Info, false );
  QgsAiAuditLog::append( u"install_python_package"_s, packages.join( ' '_L1 ) );

  constexpr int TIMEOUT_SECONDS = 300;
  const QString targetPath = QgsAiPythonRuntime::packageTargetPath();
  const QString wrapper = QgsAiPythonRuntime::pipWrapperSource( outputPath, argumentsPath, targetPath, QgsAiPythonRuntime::installCachePath(), TIMEOUT_SECONDS );
  QFile wrapperFile( wrapperPath );
  if ( !wrapperFile.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
  {
    QFile::remove( argumentsPath );
    return QgsAiToolResult::error( u"Cannot write temp pip wrapper file: %1"_s.arg( wrapperPath ) );
  }
  wrapperFile.write( wrapper.toUtf8() );
  wrapperFile.close();

  QString runnerError;
  const bool ranOk = QgsPythonRunner::runFileCaptureError( wrapperPath, runnerError );

  QJsonObject runtimeOutput;
  QFile outputFile( outputPath );
  if ( outputFile.open( QIODevice::ReadOnly ) )
  {
    const QJsonDocument document = QJsonDocument::fromJson( outputFile.readAll() );
    if ( document.isObject() )
      runtimeOutput = document.object();
  }

  QFile::remove( argumentsPath );
  QFile::remove( outputPath );
  QFile::remove( wrapperPath );

  if ( !ranOk )
  {
    const QString innerError = runtimeOutput.value( u"error"_s ).toString();
    const QString detail = !runnerError.isEmpty() ? runnerError : innerError;
    QgsMessageLog::logMessage( u"install_python_package: Python wrapper failed."_s, u"AI/Pip"_s, Qgis::MessageLevel::Warning, false );
    return QgsAiToolResult::error( u"pip wrapper failed to execute. %1"_s.arg( detail ) );
  }

  // pip runs as a separate process watched by an event loop: Strata stays responsive during the
  // download and build, and Stop ends it.
  const QJsonArray command = runtimeOutput.value( u"command"_s ).toArray();
  if ( !command.isEmpty() && runtimeOutput.value( u"error"_s ).toString().isEmpty() )
  {
    installToolKnownPython() = runtimeOutput.value( u"python_used"_s ).toString();
    QStringList arguments;
    for ( int i = 1; i < command.size(); ++i )
      arguments << command.at( i ).toString();
    const QgsAiProcessResult process
      = qgsAiRunProcess( u"Installing Python packages"_s, command.at( 0 ).toString(), arguments, TIMEOUT_SECONDS * 1000, { u"PIP_BREAK_SYSTEM_PACKAGES"_s, u"PYTHONUSERBASE"_s } );
    if ( process.canceled )
      return QgsAiToolResult::canceledResult( u"Package installation was stopped; nothing may have been installed."_s );
    runtimeOutput.insert( u"stdout"_s, process.standardOutput );
    runtimeOutput.insert( u"stderr"_s, process.standardError );
    runtimeOutput.insert( u"returncode"_s, process.exitCode );
    if ( !process.started )
      runtimeOutput.insert( u"error"_s, u"pip could not start: %1"_s.arg( process.error ) );
    else if ( process.timedOut )
      runtimeOutput.insert( u"error"_s, u"pip install timed out after %1 seconds"_s.arg( TIMEOUT_SECONDS ) );
    else if ( process.exitCode == 0 )
    {
      const QJsonArray pending = runtimeOutput.value( u"pending"_s ).toArray();
      runtimeOutput.insert( u"installed"_s, pending );
      QStringList installed;
      for ( const QJsonValue &spec : pending )
        installed << spec.toString();
      installToolRecordInstalled( QgsAiPythonRuntime::installCachePath(), installed );
      // New modules become importable in the session.
      QgsPythonRunner::run( u"import importlib; importlib.invalidate_caches()"_s );
    }
  }

  const int returnCode = runtimeOutput.value( u"returncode"_s ).toInt( -1 );
  const QString innerError = runtimeOutput.value( u"error"_s ).toString();
  const QString standardError = runtimeOutput.value( u"stderr"_s ).toString();
  const bool success = innerError.isEmpty() && returnCode == 0;
  QString errorCode = runtimeOutput.value( u"error_code"_s ).toString();
  bool retryable = runtimeOutput.value( u"retryable"_s ).toBool( true );
  if ( QgsAiPythonRuntime::isExternallyManagedError( standardError ) )
  {
    errorCode = u"externally_managed_environment"_s;
    retryable = false;
  }

  QgsMessageLog::
    logMessage( u"install_python_package: completed (returncode=%1, error=%2)"_s.arg( returnCode ).arg( innerError.isEmpty() ? u"none"_s : u"yes"_s ), u"AI/Pip"_s, success ? Qgis::MessageLevel::Info : Qgis::MessageLevel::Warning, false );

  QJsonObject output;
  output.insert( u"status"_s, success ? u"ok"_s : u"error"_s );
  output.insert( u"exit_code"_s, returnCode );
  output.insert( u"stdout"_s, truncatePipOutput( runtimeOutput.value( u"stdout"_s ).toString(), MAX_CAPTURE_BYTES ) );
  output.insert( u"stderr"_s, truncatePipOutput( standardError, MAX_CAPTURE_BYTES ) );
  output.insert( u"installed"_s, runtimeOutput.value( u"installed"_s ).toArray() );
  output.insert( u"already_available"_s, runtimeOutput.value( u"already_available"_s ).toArray() );
  output.insert( u"cached"_s, runtimeOutput.value( u"cached"_s ).toArray() );
  output.insert( u"target_path"_s, targetPath );
  output.insert( u"retryable"_s, retryable );
  if ( !runtimeOutput.value( u"python_used"_s ).toString().isEmpty() )
    output.insert( u"python_used"_s, runtimeOutput.value( u"python_used"_s ) );
  if ( !runtimeOutput.value( u"python_candidates"_s ).toArray().isEmpty() )
    output.insert( u"python_candidates"_s, runtimeOutput.value( u"python_candidates"_s ) );
  if ( !errorCode.isEmpty() )
    output.insert( u"error_code"_s, errorCode );
  if ( !innerError.isEmpty() )
    output.insert( u"error"_s, truncatePipOutput( innerError, MAX_CAPTURE_BYTES ) );
  return QgsAiToolResult::ok( output );
}
