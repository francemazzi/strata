/***************************************************************************
    qgsaigallerycloudclient.h
    ---------------------
    begin                : September 2026
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

#ifndef QGSAIGALLERYCLOUDCLIENT_H
#define QGSAIGALLERYCLOUDCLIENT_H

#include "qgis_app.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

class APP_EXPORT QgsAiGalleryCloudClient : public QObject
{
    Q_OBJECT

  public:
    struct Connector
    {
        QString slug;
        bool enabled = true;
    };

    struct PackSummary
    {
        QString slug;
        QString name;
        QString description;
        QStringList skillSlugs;
        QStringList ruleSlugs;
        QList<Connector> connectors;
    };

    struct PackSkill
    {
        QString slug;
        QString name;
        QString description;
        QString content;
    };

    struct PackRule
    {
        QString slug;
        QString name;
        QString description;
        QString content;
    };

    struct PackImport
    {
        PackSummary pack;
        QList<PackSkill> skills;
        QList<PackRule> rules;
    };

    struct McpServer
    {
        QString slug;
        QString name;
        QString description;
        bool mutating = false;
        bool enabled = true;
        QString scanStatus;
    };

    explicit QgsAiGalleryCloudClient( QObject *parent = nullptr );

    void fetchPacks( const QString &apiBase, const QString &sessionToken );
    void fetchPackForImport( const QString &apiBase, const QString &sessionToken, const QString &slug );
    void fetchMcpServers( const QString &apiBase, const QString &sessionToken );
    void setMcpServerEnabled( const QString &apiBase, const QString &sessionToken, const QString &slug, bool enabled );

  signals:
    void packsFetched( const QList<QgsAiGalleryCloudClient::PackSummary> &packs );
    void packImportReady( const QgsAiGalleryCloudClient::PackImport &pack );
    void mcpServersFetched( const QList<QgsAiGalleryCloudClient::McpServer> &servers );
    void mcpServerUpdated( const QgsAiGalleryCloudClient::McpServer &server );
    void requestFailed( const QString &message );
};

Q_DECLARE_METATYPE( QgsAiGalleryCloudClient::PackSummary )
Q_DECLARE_METATYPE( QgsAiGalleryCloudClient::PackImport )
Q_DECLARE_METATYPE( QgsAiGalleryCloudClient::McpServer )

#endif // QGSAIGALLERYCLOUDCLIENT_H
