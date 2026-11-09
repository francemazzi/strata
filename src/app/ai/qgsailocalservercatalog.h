/***************************************************************************
    qgsailocalservercatalog.h
    ---------------------
    begin                : November 2026
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

#ifndef QGSAILOCALSERVERCATALOG_H
#define QGSAILOCALSERVERCATALOG_H

#include "qgis_app.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

/**
 * Detects a local inference server and lists the models it can serve to the
 * agent loop.
 *
 * Strata never starts, installs or configures the server: it only probes the
 * configured base URL. For Ollama the probe is `GET /api/version`, followed by
 * `GET /api/tags` and one `POST /api/show` per installed model (cached by
 * digest) to read its capabilities and native context length. Only chat models
 * that advertise tool calling are offered, because a model without tools
 * breaks the agent loop.
 */
class APP_EXPORT QgsAiLocalServerCatalog : public QObject
{
    Q_OBJECT

  public:
    enum class ServerState
    {
      Unknown,   //!< Never probed
      Running,   //!< The last probe reached the server
      NotRunning //!< The last probe failed
    };
    Q_ENUM( ServerState )

    struct ModelInfo
    {
        QString id;
        QString parameterSize; //!< e.g. "27B", empty when the server does not report it
        int contextLength = 0; //!< Native context window in tokens, 0 when unknown
        bool supportsTools = false;
        bool supportsThinking = false;
        bool supportsVision = false;

        //! One-line label for menus and combo boxes: id, size and context window.
        QString displayLabel() const;
    };

    explicit QgsAiLocalServerCatalog( QObject *parent = nullptr );

    //! Default Ollama base URL: honors \c OLLAMA_HOST, otherwise http://127.0.0.1:11434.
    static QString defaultOllamaBaseUrl();

    /**
     * Normalizes a user or environment supplied host into a base URL without a
     * trailing slash: adds the http scheme and the default port when missing and
     * maps the 0.0.0.0 bind address to loopback.
     */
    static QString normalizedBaseUrl( const QString &value, int defaultPort );

    //! Returns true when \a baseUrl points at this machine.
    static bool isLoopbackUrl( const QString &baseUrl );

    /**
     * Parses a `POST /api/show` body into \a info. Returns false when the body
     * is not a JSON object. Public for unit tests.
     */
    static bool parseOllamaShow( const QByteArray &body, ModelInfo &info );

    QString baseUrl() const { return mBaseUrl; }
    //! Sets the server base URL. A different URL resets the known state.
    void setBaseUrl( const QString &baseUrl );

    ServerState state() const { return mState; }
    QString version() const { return mVersion; }

    //! Tool-capable chat models, sorted by id. Empty unless the server is running.
    QList<ModelInfo> models() const { return mModels; }
    //! All models installed on the server, including those hidden from models().
    int installedModelCount() const { return mInstalledCount; }
    //! Details for \a id, or a default-constructed info when unknown.
    ModelInfo model( const QString &id ) const;

    /**
     * Probes the server. Calls made less than five seconds after the previous
     * probe are ignored unless \a force is set. changed() is emitted once the
     * probe settles and the state, version or model list differ.
     */
    void refresh( bool force = false );

    //! Marks the server unreachable after a failed request, without a new probe.
    void markUnreachable();

  signals:
    void changed();

  private:
    void fetchTags( int generation );
    void fetchShow( int generation, const QString &name, const QString &digest );
    void finishProbe( int generation );
    void applyResult( ServerState state, const QString &version, const QList<ModelInfo> &models, int installedCount );

    struct InstalledModel
    {
        QString name;
        QString digest;
        QString parameterSize;
    };

    QString mBaseUrl;
    ServerState mState = ServerState::Unknown;
    QString mVersion;
    QList<ModelInfo> mModels;
    int mInstalledCount = 0;

    int mGeneration = 0;
    bool mProbing = false;
    qint64 mLastProbeMs = 0;
    QString mPendingVersion;
    QList<InstalledModel> mPendingInstalled;
    int mPendingShows = 0;
    QHash<QString, ModelInfo> mShowCache; // digest → capabilities
};

#endif // QGSAILOCALSERVERCATALOG_H
