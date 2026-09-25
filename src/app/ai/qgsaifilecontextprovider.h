/***************************************************************************
    qgsaifilecontextprovider.h
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

#ifndef QGSAIFILECONTEXTPROVIDER_H
#define QGSAIFILECONTEXTPROVIDER_H

#include "qgis_app.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QgsFeedback;

struct APP_EXPORT QgsAiFileContext
{
    QString filePath;
    QString selectedText;
    QString fileSnippet;
    qint64 fileSize = 0;
    bool truncated = false;
    bool binary = false;

    bool isValid() const { return !filePath.isEmpty() || !selectedText.isEmpty(); }
};

class APP_EXPORT QgsAiFileContextProvider : public QObject
{
    Q_OBJECT

  public:
    //! One file found by scanWorkspace(). Size and modification time come from the directory listing.
    struct WorkspaceFile
    {
        QString relativePath;
        QString absolutePath;
        qint64 size = 0;
        qint64 lastModifiedMs = 0;
    };

    struct WorkspaceScanOptions
    {
        //! Files and folders to visit at most.
        int maxEntries = 50000;
        //! Files to return at most (<= 0: no limit besides maxEntries).
        int maxResults = 0;
        //! Stop the walk after this many milliseconds (<= 0: no time limit).
        int timeBudgetMs = 0;
        //! Keep only files whose relative path contains this text (case-insensitive).
        QString query;
        //! More folder names to leave out at any depth, with * as wildcard (case-insensitive).
        QStringList excludedFolders;
    };

    struct WorkspaceScanResult
    {
        QList<WorkspaceFile> files;
        int visitedEntries = 0;
        //! The walk stopped early: entry, result or time limit reached, or canceled.
        bool truncated = false;
        bool timedOut = false;
    };

    explicit QgsAiFileContextProvider( const QString &workspaceRoot, QObject *parent = nullptr );

    /**
     * Walks \a workspaceRoot for files. Safe on any thread: it only reads the file system.
     *
     * Folders that never hold user data (version control, caches, virtual environments, …)
     * are skipped at any depth instead of being walked and filtered afterwards, hidden entries
     * and symbolic links are skipped, and so are cloud placeholder files that reading would
     * download (OneDrive "Files On-Demand" on Windows).
     */
    static WorkspaceScanResult scanWorkspace( const QString &workspaceRoot, const WorkspaceScanOptions &options, QgsFeedback *feedback = nullptr );

    //! True if a folder with this name is never walked by scanWorkspace().
    static bool isExcludedFolderName( const QString &folderName );

    //! True if \a path is on a network share (SMB, NFS, AFP, WebDAV, UNC path).
    static bool isNetworkPath( const QString &path );

    /**
     * Resolves the AI workspace root: project home path, then strata/workspace/root,
     * then legacy geoai/qgis_ai settings, then a profile-local default.
     */
    static QString resolveWorkspaceRoot();

    QgsAiFileContext buildContext( const QString &filePath, const QString &selectedText = QString(), int maxBytes = 16384, bool allowExternal = false ) const;
    QString resolveWorkspaceFile( const QString &filePath ) const;
    QStringList workspaceFileCandidates( const QString &query, int maxResults = 25 ) const;
    QStringList searchInFile( const QString &filePath, const QString &needle, int maxMatches = 25 ) const;
    QString diffPreview( const QString &beforeText, const QString &afterText ) const;
    QString workspaceRoot() const { return mWorkspaceRoot; }
    void setWorkspaceRoot( const QString &workspaceRoot );

    /**
     * Returns the absolute, cleaned path for \a filePath. If \a allowExternal is false (default)
     * and the resolved path is not inside the workspace root, returns an empty string.
     * Public so tools that write/download into the workspace (e.g. download_file) can validate
     * destination paths against the same boundary used by read tools.
     */
    QString normalizePath( const QString &filePath, bool allowExternal = false ) const;

    /**
     * Returns true iff \a absolutePath is inside the workspace root (or equal to it).
     * Public for the same reason as normalizePath.
     */
    bool isInWorkspace( const QString &absolutePath ) const;

  signals:
    void workspaceRootChanged( const QString &workspaceRoot );

  private:
    QString mWorkspaceRoot;
};

#endif // QGSAIFILECONTEXTPROVIDER_H
