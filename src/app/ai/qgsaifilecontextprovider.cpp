/***************************************************************************
    qgsaifilecontextprovider.cpp
    ---------------------
    begin                : April 2026
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

#include "qgsaifilecontextprovider.h"

#include <algorithm>

#include "qgsapplication.h"
#include "qgsfeedback.h"
#include "qgsproject.h"
#include "qgssettings.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QString>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "moc_qgsaifilecontextprovider.cpp"

using namespace Qt::StringLiterals;

QString QgsAiFileContextProvider::resolveWorkspaceRoot()
{
  const QString projectHome = QgsProject::instance()->homePath().trimmed();
  if ( !projectHome.isEmpty() )
    return QDir( projectHome ).absolutePath();

  QgsSettings settings;
  const QString settingsKey = u"strata/workspace/root"_s;
  const QString geoAiLegacyKey = u"geoai/workspace/root"_s;
  const QString qgisAiLegacyKey = u"qgis_ai/workspace/root"_s;
  QString configured = settings.value( settingsKey ).toString().trimmed();
  if ( configured.isEmpty() )
    configured = settings.value( geoAiLegacyKey ).toString().trimmed();
  if ( configured.isEmpty() )
    configured = settings.value( qgisAiLegacyKey ).toString().trimmed();
  if ( !configured.isEmpty() )
  {
    settings.setValue( settingsKey, QDir( configured ).absolutePath() );
    settings.remove( geoAiLegacyKey );
    settings.remove( qgisAiLegacyKey );
    return QDir( configured ).absolutePath();
  }

  const QString defaultRoot = QDir( QgsApplication::qgisSettingsDirPath() ).filePath( u"ai_workspace"_s );
  QDir().mkpath( defaultRoot );
  const QString absoluteDefaultRoot = QDir( defaultRoot ).absolutePath();
  settings.setValue( settingsKey, absoluteDefaultRoot );
  settings.remove( geoAiLegacyKey );
  settings.remove( qgisAiLegacyKey );
  return absoluteDefaultRoot;
}

QgsAiFileContextProvider::QgsAiFileContextProvider( const QString &workspaceRoot, QObject *parent )
  : QObject( parent )
  , mWorkspaceRoot( workspaceRoot.trimmed().isEmpty() ? QString() : QDir( workspaceRoot ).absolutePath() )
{}

void QgsAiFileContextProvider::setWorkspaceRoot( const QString &workspaceRoot )
{
  const QString normalizedRoot = workspaceRoot.trimmed().isEmpty() ? QString() : QDir( workspaceRoot ).absolutePath();
  if ( mWorkspaceRoot == normalizedRoot )
    return;

  mWorkspaceRoot = normalizedRoot;
  emit workspaceRootChanged( mWorkspaceRoot );
}

bool QgsAiFileContextProvider::isInWorkspace( const QString &absolutePath ) const
{
  if ( absolutePath.isEmpty() || mWorkspaceRoot.isEmpty() )
    return false;

  const QString relativePath = QDir( mWorkspaceRoot ).relativeFilePath( absolutePath );
  return relativePath == "."_L1 || ( !relativePath.startsWith( "../"_L1 ) && relativePath != ".."_L1 && !QDir::isAbsolutePath( relativePath ) );
}

QString QgsAiFileContextProvider::normalizePath( const QString &filePath, bool allowExternal ) const
{
  if ( filePath.isEmpty() )
    return QString();

  QFileInfo info( filePath );
  if ( mWorkspaceRoot.isEmpty() && ( !allowExternal || !info.isAbsolute() ) )
    return QString();

  const QString absolutePath = info.isAbsolute() ? info.absoluteFilePath() : QDir( mWorkspaceRoot ).absoluteFilePath( filePath );
  const QString cleanPath = QDir::cleanPath( absolutePath );

  if ( !allowExternal && !isInWorkspace( cleanPath ) )
    return QString();

  return cleanPath;
}

QString QgsAiFileContextProvider::resolveWorkspaceFile( const QString &filePath ) const
{
  const QString normalizedPath = normalizePath( filePath );
  if ( normalizedPath.isEmpty() )
    return QString();

  QFileInfo info( normalizedPath );
  if ( !info.exists() || !info.isFile() )
    return QString();

  return normalizedPath;
}

namespace
{
  // Workspace folders of the Strata source tree itself, excluded at the root only (a user's
  // "build" or "external" folder deeper in a project can hold real data).
  const QStringList FILE_CONTEXT_ROOT_ONLY_EXCLUSIONS {
    u"build/"_s,
    u"external/"_s,
    u"i18n/"_s,
    u"tests/testdata/"_s,
    u"vcpkg/"_s,
  };

  bool fileContextIsCloudPlaceholder( const QString &absolutePath )
  {
#ifdef Q_OS_WIN
    // OneDrive "Files On-Demand": reading these files downloads them.
    const DWORD attributes = GetFileAttributesW( reinterpret_cast<const wchar_t *>( QDir::toNativeSeparators( absolutePath ).utf16() ) );
    if ( attributes == INVALID_FILE_ATTRIBUTES )
      return false;
    constexpr DWORD recallOnDataAccess = 0x00400000; // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
    constexpr DWORD recallOnOpen = 0x00040000;       // FILE_ATTRIBUTE_RECALL_ON_OPEN
    return ( attributes & ( recallOnDataAccess | recallOnOpen | FILE_ATTRIBUTE_OFFLINE ) ) != 0;
#else
    Q_UNUSED( absolutePath )
    return false;
#endif
  }
} // namespace

bool QgsAiFileContextProvider::isExcludedFolderName( const QString &folderName )
{
  static const QSet<QString> excluded {
    u".git"_s,
    u".svn"_s,
    u".hg"_s,
    u"node_modules"_s,
    u"__pycache__"_s,
    u"__MACOSX"_s,
    u".venv"_s,
    u"venv"_s,
    u".tox"_s,
    u".mypy_cache"_s,
    u".pytest_cache"_s,
    u".cache"_s,
    u".Trash"_s,
    u"$RECYCLE.BIN"_s,
    u"System Volume Information"_s,
  };
  return excluded.contains( folderName );
}

bool QgsAiFileContextProvider::isNetworkPath( const QString &path )
{
  if ( path.startsWith( "\\\\"_L1 ) || path.startsWith( "//"_L1 ) )
    return true;
  const QStorageInfo storage( path );
  if ( !storage.isValid() )
    return false;
  static const QSet<QByteArray> networkFileSystems { "smbfs", "cifs", "smb2", "smb3", "nfs", "nfs4", "afpfs", "webdav", "davfs", "fuse.sshfs", "9p" };
  return networkFileSystems.contains( storage.fileSystemType().toLower() );
}

QgsAiFileContextProvider::WorkspaceScanResult QgsAiFileContextProvider::scanWorkspace( const QString &workspaceRoot, const WorkspaceScanOptions &options, QgsFeedback *feedback )
{
  WorkspaceScanResult result;
  if ( workspaceRoot.isEmpty() || !QFileInfo( workspaceRoot ).isDir() )
    return result;

  const QDir rootDir( workspaceRoot );
  const QString query = options.query.trimmed();
  QList<QRegularExpression> userExclusions;
  for ( const QString &pattern : options.excludedFolders )
  {
    if ( !pattern.trimmed().isEmpty() )
      userExclusions << QRegularExpression( QRegularExpression::wildcardToRegularExpression( pattern.trimmed() ), QRegularExpression::CaseInsensitiveOption );
  }
  const auto userExcluded = [&userExclusions]( const QString &name ) {
    return std::any_of( userExclusions.cbegin(), userExclusions.cend(), [&name]( const QRegularExpression &exclusion ) { return exclusion.match( name ).hasMatch(); } );
  };
  QElapsedTimer clock;
  clock.start();

  // Breadth-first, so a truncated walk still covers the top of the tree.
  QStringList pendingDirs { rootDir.absolutePath() };
  while ( !pendingDirs.isEmpty() )
  {
    if ( feedback && feedback->isCanceled() )
    {
      result.truncated = true;
      break;
    }
    if ( options.timeBudgetMs > 0 && clock.elapsed() > options.timeBudgetMs )
    {
      result.truncated = true;
      result.timedOut = true;
      break;
    }

    const QDir dir( pendingDirs.takeFirst() );
    const QFileInfoList entries = dir.entryInfoList( QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name );
    for ( const QFileInfo &entry : entries )
    {
      if ( ++result.visitedEntries > options.maxEntries )
      {
        result.truncated = true;
        return result;
      }

      const QString absolutePath = QDir::cleanPath( entry.absoluteFilePath() );
      const QString relativePath = rootDir.relativeFilePath( absolutePath );
      if ( entry.isDir() )
      {
        const QString relativeDir = relativePath + '/';
        const bool rootOnlyExcluded = std::any_of( FILE_CONTEXT_ROOT_ONLY_EXCLUSIONS.cbegin(), FILE_CONTEXT_ROOT_ONLY_EXCLUSIONS.cend(), [&relativeDir]( const QString &excluded ) {
          return relativeDir == excluded;
        } );
        if ( !rootOnlyExcluded && !isExcludedFolderName( entry.fileName() ) && !userExcluded( entry.fileName() ) )
          pendingDirs.append( absolutePath );
        continue;
      }

      if ( !query.isEmpty() && !relativePath.contains( query, Qt::CaseInsensitive ) )
        continue;
      if ( fileContextIsCloudPlaceholder( absolutePath ) )
        continue;

      result.files.append( { relativePath, absolutePath, entry.size(), entry.lastModified().toMSecsSinceEpoch() } );
      if ( options.maxResults > 0 && result.files.size() >= options.maxResults )
      {
        result.truncated = !pendingDirs.isEmpty() || &entry != &entries.constLast();
        return result;
      }
    }
  }
  return result;
}

QStringList QgsAiFileContextProvider::workspaceFileCandidates( const QString &query, int maxResults ) const
{
  QStringList candidates;
  if ( maxResults <= 0 || mWorkspaceRoot.isEmpty() )
    return candidates;

  WorkspaceScanOptions options;
  options.maxResults = maxResults;
  options.query = query;
  // Callers still run on the interface thread: never walk a huge tree for long.
  options.timeBudgetMs = 3000;
  const WorkspaceScanResult result = scanWorkspace( mWorkspaceRoot, options );
  candidates.reserve( result.files.size() );
  for ( const WorkspaceFile &file : result.files )
    candidates << file.relativePath;

  candidates.sort( Qt::CaseInsensitive );
  return candidates;
}

QgsAiFileContext QgsAiFileContextProvider::buildContext( const QString &filePath, const QString &selectedText, int maxBytes, bool allowExternal ) const
{
  QgsAiFileContext context;
  context.selectedText = selectedText;

  const QString normalizedPath = normalizePath( filePath, allowExternal );
  if ( normalizedPath.isEmpty() )
    return context;

  context.filePath = normalizedPath;
  context.fileSize = QFileInfo( normalizedPath ).size();
  QFile file( normalizedPath );
  if ( !file.open( QIODevice::ReadOnly ) )
    return context;

  QByteArray content = file.read( std::max( 0, maxBytes ) + 1 );
  context.truncated = content.size() > maxBytes;
  if ( context.truncated )
    content.truncate( maxBytes );

  context.binary = content.contains( '\0' );
  if ( context.binary )
    return context;

  context.fileSnippet = QString::fromUtf8( content );
  return context;
}

QStringList QgsAiFileContextProvider::searchInFile( const QString &filePath, const QString &needle, int maxMatches ) const
{
  QStringList matches;
  if ( needle.isEmpty() || maxMatches <= 0 )
    return matches;

  const QgsAiFileContext context = buildContext( filePath, QString(), 512 * 1024 );
  if ( context.filePath.isEmpty() || context.fileSnippet.isEmpty() )
    return matches;

  const QStringList lines = context.fileSnippet.split( '\n' );
  for ( int lineNumber = 0; lineNumber < lines.size() && matches.size() < maxMatches; ++lineNumber )
  {
    const QString line = lines.at( lineNumber );
    if ( line.contains( needle, Qt::CaseInsensitive ) )
      matches << u"%1:%2"_s.arg( lineNumber + 1 ).arg( line.trimmed() );
  }

  return matches;
}

QString QgsAiFileContextProvider::diffPreview( const QString &beforeText, const QString &afterText ) const
{
  if ( beforeText == afterText )
    return u"No changes."_s;

  QString preview;
  preview += "--- before\n"_L1;
  preview += beforeText.left( 2000 );
  preview += "\n+++ after\n"_L1;
  preview += afterText.left( 2000 );
  return preview;
}
