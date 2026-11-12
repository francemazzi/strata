/***************************************************************************
    qgsaiprovidermodelcatalog.h
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

#ifndef QGSAIPROVIDERMODELCATALOG_H
#define QGSAIPROVIDERMODELCATALOG_H

#include "qgis_app.h"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QComboBox;
class QJsonArray;
class QNetworkReply;
class QProcess;
class QWidget;

class APP_EXPORT QgsAiProviderModelCatalog : public QObject
{
    Q_OBJECT
  public:
    enum class Source
    {
      Codex,
      ClaudeCode,
      Anthropic,
      OpenAi
    };

    struct Model
    {
        QString id;
        QString label;
    };

    explicit QgsAiProviderModelCatalog( Source source, QObject *parent = nullptr );
    QList<Model> models() const { return mModels; }
    bool loading() const { return mLoading; }
    QString status() const { return mStatus; }
    void refresh( const QString &apiKey = QString(), bool force = false );
    void reset();

    QWidget *createPicker( QComboBox *&picker, const QString &value, const QString &docsUrl, std::function<void()> refresh, QWidget *parent, const QString &automaticLabel = QString() );
    static QString pickerValue( const QComboBox *picker );

  signals:
    void changed();

  private:
    static QList<Model> parseModels( Source source, const QJsonArray &entries );
    static void fillPicker( QComboBox *picker, const QList<Model> &models, const QString &automaticLabel );
    void refreshClaudeCode();
    void finish( const QList<Model> &models, const QString &error = QString() );

    Source mSource;
    QList<Model> mModels;
    QString mStatus;
    QElapsedTimer mLastAttempt;
    QByteArray mCredentialFingerprint;
    QPointer<QNetworkReply> mReply;
    QProcess *mProcess = nullptr;
    bool mLoading = false;
    int mGeneration = 0;
    quint64 mSessionRevision = 0;
};

#endif
