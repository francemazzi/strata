/***************************************************************************
    qgsaifilesummarizer.h
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

#ifndef QGSAIFILESUMMARIZER_H
#define QGSAIFILESUMMARIZER_H

#include "qgis_app.h"

#include <QString>

/**
 * Describes structured workspace files for the index instead of their raw text.
 *
 * Raw CSV rows, GeoJSON coordinates and project XML are long in tokens and poor in meaning:
 * the embedding model reads the first 512 tokens and the rest is lost. A summary (columns
 * and their types, row count, a few sample rows; feature count, geometry types and extent;
 * project layers and CRS) is shorter and answers the questions users ask about their data.
 */
class APP_EXPORT QgsAiFileSummarizer
{
  public:
    //! Rows or features quoted as samples in a summary.
    static constexpr int SAMPLE_ROWS = 5;

    /**
     * Summary of \a content, the text of the file at \a relativePath, or an empty string when the
     * file is indexed as it is. \a truncated tells that \a content is only the start of the file.
     */
    static QString summarize( const QString &relativePath, const QString &content, bool truncated );

    //! Summary of a CSV or TSV file.
    static QString csvSummary( const QString &relativePath, const QString &content, bool truncated );
    //! Summary of a GeoJSON document; empty if \a content is not complete GeoJSON.
    static QString geoJsonSummary( const QString &relativePath, const QString &content );
    //! Summary of a QGIS project (.qgs). Credentials in data sources are left out.
    static QString qgisProjectSummary( const QString &relativePath, const QString &content );
    //! Summary of another XML document: its elements and the names and texts it holds.
    static QString xmlSummary( const QString &relativePath, const QString &content );

    //! \a dataSource without passwords, user names and authentication ids.
    static QString redactDataSource( const QString &dataSource );
};

#endif // QGSAIFILESUMMARIZER_H
