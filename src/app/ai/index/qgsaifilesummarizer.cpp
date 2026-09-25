/***************************************************************************
    qgsaifilesummarizer.cpp
    -----------------------
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

#include "qgsaifilesummarizer.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QDate>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

using namespace Qt::StringLiterals;

namespace
{
  //! Values seen in one column (or property), to describe its type and range.
  struct SummaryColumnStats
  {
      int values = 0;
      int empty = 0;
      int integers = 0;
      int numbers = 0;
      int dates = 0;
      int booleans = 0;
      double minimum = std::numeric_limits<double>::max();
      double maximum = std::numeric_limits<double>::lowest();
      QStringList samples;

      void add( const QString &raw, bool commaDecimal )
      {
        const QString value = raw.trimmed();
        if ( value.isEmpty() )
        {
          ++empty;
          return;
        }
        ++values;
        bool ok = false;
        const QLocale c = QLocale::c();
        c.toLongLong( value, &ok );
        if ( ok )
          ++integers;
        const double number = c.toDouble( commaDecimal ? QString( value ).replace( ',', '.' ) : value, &ok );
        if ( ok && std::isfinite( number ) )
        {
          ++numbers;
          minimum = std::min( minimum, number );
          maximum = std::max( maximum, number );
          return;
        }
        if ( QDate::fromString( value.left( 10 ), Qt::ISODate ).isValid() )
          ++dates;
        if ( value.compare( "true"_L1, Qt::CaseInsensitive ) == 0 || value.compare( "false"_L1, Qt::CaseInsensitive ) == 0 )
          ++booleans;
        if ( samples.size() < 3 && !samples.contains( value ) )
          samples << ( value.size() > 40 ? value.left( 40 ) + u"…"_s : value );
      }

      QString describe() const
      {
        if ( values == 0 )
          return u"empty"_s;
        const auto range = [this]() { return u"%1 to %2"_s.arg( QLocale::c().toString( minimum, 'g', 10 ), QLocale::c().toString( maximum, 'g', 10 ) ); };
        QString text;
        if ( integers == values )
          text = u"integer, %1"_s.arg( range() );
        else if ( numbers == values )
          text = u"decimal, %1"_s.arg( range() );
        else if ( dates == values )
          text = u"date, e.g. %1"_s.arg( samples.join( ", "_L1 ) );
        else if ( booleans == values )
          text = u"boolean"_s;
        else
          text = u"text, e.g. %1"_s.arg( samples.join( ", "_L1 ) );
        if ( empty > 0 )
          text += u", %1 empty"_s.arg( empty );
        return text;
      }
  };

  //! Splits one CSV line on \a delimiter, honouring double quotes.
  QStringList summarySplitCsvLine( const QString &line, QChar delimiter )
  {
    QStringList fields;
    QString current;
    bool quoted = false;
    for ( int i = 0; i < line.size(); ++i )
    {
      const QChar ch = line.at( i );
      if ( ch == '"'_L1 )
      {
        if ( quoted && i + 1 < line.size() && line.at( i + 1 ) == '"'_L1 )
        {
          current += ch;
          ++i;
        }
        else
        {
          quoted = !quoted;
        }
      }
      else if ( ch == delimiter && !quoted )
      {
        fields << current;
        current.clear();
      }
      else
      {
        current += ch;
      }
    }
    fields << current;
    return fields;
  }

  QString summaryFileKind( const QString &relativePath )
  {
    return QFileInfo( relativePath ).suffix().toLower();
  }

  //! Extends [xmin, ymin, xmax, ymax] with the positions in a GeoJSON coordinates array.
  void summaryExtendBounds( const QJsonValue &coordinates, double bounds[4], int depth = 0 )
  {
    if ( !coordinates.isArray() || depth > 6 )
      return;
    const QJsonArray array = coordinates.toArray();
    if ( array.size() >= 2 && array.at( 0 ).isDouble() && array.at( 1 ).isDouble() )
    {
      const double x = array.at( 0 ).toDouble();
      const double y = array.at( 1 ).toDouble();
      bounds[0] = std::min( bounds[0], x );
      bounds[1] = std::min( bounds[1], y );
      bounds[2] = std::max( bounds[2], x );
      bounds[3] = std::max( bounds[3], y );
      return;
    }
    for ( const QJsonValue &item : array )
      summaryExtendBounds( item, bounds, depth + 1 );
  }

  void summaryCollectGeometry( const QJsonObject &geometry, QMap<QString, int> &types, double bounds[4] )
  {
    const QString type = geometry.value( "type"_L1 ).toString();
    if ( type.isEmpty() )
      return;
    types[type]++;
    if ( type == "GeometryCollection"_L1 )
    {
      for ( const QJsonValue &part : geometry.value( "geometries"_L1 ).toArray() )
        summaryCollectGeometry( part.toObject(), types, bounds );
      return;
    }
    summaryExtendBounds( geometry.value( "coordinates"_L1 ), bounds );
  }
} // namespace

QString QgsAiFileSummarizer::summarize( const QString &relativePath, const QString &content, bool truncated )
{
  const QString kind = summaryFileKind( relativePath );
  if ( kind == "csv"_L1 || kind == "tsv"_L1 )
    return csvSummary( relativePath, content, truncated );
  if ( kind == "geojson"_L1 || kind == "json"_L1 )
    return truncated ? QString() : geoJsonSummary( relativePath, content );
  if ( kind == "qgs"_L1 )
    return qgisProjectSummary( relativePath, content );
  if ( kind == "xml"_L1 || kind == "qml"_L1 )
    return xmlSummary( relativePath, content );
  return QString();
}

QString QgsAiFileSummarizer::csvSummary( const QString &relativePath, const QString &content, bool truncated )
{
  QStringList lines = content.split( '\n' );
  for ( QString &line : lines )
  {
    if ( line.endsWith( '\r' ) )
      line.chop( 1 );
  }
  // The last line of a partly read file is cut.
  if ( truncated && !lines.isEmpty() )
    lines.removeLast();
  lines.erase( std::remove_if( lines.begin(), lines.end(), []( const QString &line ) { return line.trimmed().isEmpty(); } ), lines.end() );
  if ( lines.isEmpty() )
    return QString();

  QChar delimiter = ',';
  if ( summaryFileKind( relativePath ) == "tsv"_L1 )
  {
    delimiter = '\t';
  }
  else
  {
    int best = lines.first().count( ',' );
    for ( const QChar candidate : { QChar( ';' ), QChar( '\t' ), QChar( '|' ) } )
    {
      const int count = static_cast<int>( lines.first().count( candidate ) );
      if ( count > best )
      {
        best = count;
        delimiter = candidate;
      }
    }
  }
  const bool commaDecimal = delimiter == ';'_L1;

  const QStringList header = summarySplitCsvLine( lines.first(), delimiter );
  QList<SummaryColumnStats> columns( header.size() );
  for ( int row = 1; row < lines.size(); ++row )
  {
    const QStringList fields = summarySplitCsvLine( lines.at( row ), delimiter );
    for ( int column = 0; column < columns.size(); ++column )
      columns[column].add( column < fields.size() ? fields.at( column ) : QString(), commaDecimal );
  }

  const int rows = static_cast<int>( lines.size() ) - 1;
  const QString delimiterName = delimiter == '\t'_L1 ? u"tab"_s : u"\"%1\""_s.arg( delimiter );
  QString summary = u"Table file %1 (delimiter %2): %3 rows%4, %5 columns.\nColumns:\n"_s.arg( relativePath, delimiterName )
                      .arg( rows )
                      .arg( truncated ? u" in the part read (the file is longer)"_s : QString() )
                      .arg( header.size() );
  for ( int column = 0; column < header.size(); ++column )
    summary += u"- %1: %2\n"_s.arg( header.at( column ).trimmed(), columns.at( column ).describe() );
  summary += "First rows:\n"_L1;
  for ( int row = 0; row < std::min<qsizetype>( lines.size(), SAMPLE_ROWS + 1 ); ++row )
    summary += lines.at( row ).left( 300 ) + '\n';
  return summary;
}

QString QgsAiFileSummarizer::geoJsonSummary( const QString &relativePath, const QString &content )
{
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson( content.toUtf8(), &error );
  if ( error.error != QJsonParseError::NoError || !document.isObject() )
    return QString();

  const QJsonObject root = document.object();
  const QString type = root.value( "type"_L1 ).toString();
  QJsonArray features;
  if ( type == "FeatureCollection"_L1 )
    features = root.value( "features"_L1 ).toArray();
  else if ( type == "Feature"_L1 )
    features.append( root );
  else
    return QString(); // Plain JSON: indexed as it is.

  QMap<QString, int> geometryTypes;
  double bounds[4] = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest() };
  QStringList propertyNames;
  QHash<QString, SummaryColumnStats> properties;
  QStringList samples;
  for ( const QJsonValue &value : std::as_const( features ) )
  {
    const QJsonObject feature = value.toObject();
    summaryCollectGeometry( feature.value( "geometry"_L1 ).toObject(), geometryTypes, bounds );
    const QJsonObject featureProperties = feature.value( "properties"_L1 ).toObject();
    for ( auto it = featureProperties.constBegin(); it != featureProperties.constEnd(); ++it )
    {
      if ( !properties.contains( it.key() ) )
        propertyNames << it.key();
      const QJsonValue property = it.value();
      properties[it.key()].add( property.isString() ? property.toString() : property.isNull() ? QString() : property.toVariant().toString(), false );
    }
    if ( samples.size() < SAMPLE_ROWS )
      samples << QString::fromUtf8( QJsonDocument( featureProperties ).toJson( QJsonDocument::Compact ) ).left( 300 );
  }

  QStringList typeList;
  for ( auto it = geometryTypes.constBegin(); it != geometryTypes.constEnd(); ++it )
    typeList << u"%1 (%2)"_s.arg( it.key() ).arg( it.value() );

  QString summary = u"GeoJSON file %1: %2 features, geometry %3.\n"_s.arg( relativePath ).arg( features.size() ).arg( typeList.isEmpty() ? u"none"_s : typeList.join( ", "_L1 ) );
  if ( bounds[0] <= bounds[2] )
    summary += u"Extent: %1, %2 to %3, %4\n"_s.arg( bounds[0] ).arg( bounds[1] ).arg( bounds[2] ).arg( bounds[3] );
  if ( const QString crs = root.value( "crs"_L1 ).toObject().value( "properties"_L1 ).toObject().value( "name"_L1 ).toString(); !crs.isEmpty() )
    summary += u"CRS: %1\n"_s.arg( crs );
  summary += "Properties:\n"_L1;
  for ( const QString &name : std::as_const( propertyNames ) )
    summary += u"- %1: %2\n"_s.arg( name, properties.value( name ).describe() );
  summary += u"First features:\n"_s + samples.join( '\n' ) + '\n';
  return summary;
}

QString QgsAiFileSummarizer::redactDataSource( const QString &dataSource )
{
  QString redacted = dataSource;
  static const QRegularExpression
    keyValue( u"\\b(password|passwd|pwd|user|username|authcfg|token|access_token|apikey|api_key|key|secret)=('[^']*'|\"[^\"]*\"|[^\\s&;|]*)"_s, QRegularExpression::CaseInsensitiveOption );
  redacted.replace( keyValue, u"\\1=***"_s );
  static const QRegularExpression urlCredentials( u"://[^/@\\s]+@"_s );
  redacted.replace( urlCredentials, u"://***@"_s );
  return redacted;
}

QString QgsAiFileSummarizer::qgisProjectSummary( const QString &relativePath, const QString &content )
{
  struct Layer
  {
      QString id;
      QString name;
      QString type;
      QString geometry;
      QString provider;
      QString source;
      QString crs;
  };

  QXmlStreamReader xml( content );
  QString title;
  QString projectName;
  QString projectCrs;
  QList<Layer> layers;
  QStringList layouts;
  QStringList path;
  Layer current;
  bool inLayer = false;
  while ( !xml.atEnd() )
  {
    const QXmlStreamReader::TokenType token = xml.readNext();
    if ( token == QXmlStreamReader::StartElement )
    {
      const QString name = xml.name().toString();
      path << name;
      if ( name == "qgis"_L1 && path.size() == 1 )
        projectName = xml.attributes().value( "projectname"_L1 ).toString();
      else if ( name == "maplayer"_L1 )
      {
        inLayer = true;
        current = Layer();
        current.type = xml.attributes().value( "type"_L1 ).toString();
        current.geometry = xml.attributes().value( "geometry"_L1 ).toString();
      }
      else if ( name == "Layout"_L1 )
        layouts << xml.attributes().value( "name"_L1 ).toString();
    }
    else if ( token == QXmlStreamReader::EndElement )
    {
      if ( xml.name() == "maplayer"_L1 && inLayer )
      {
        inLayer = false;
        layers << current;
      }
      if ( !path.isEmpty() )
        path.removeLast();
    }
    else if ( token == QXmlStreamReader::Characters && !xml.isWhitespace() && !path.isEmpty() )
    {
      const QString text = xml.text().toString().trimmed();
      const QString &element = path.constLast();
      if ( element == "title"_L1 && path.size() == 2 )
        title = text;
      else if ( element == "authid"_L1 && path.contains( "projectCrs"_L1 ) && !inLayer )
        projectCrs = text;
      else if ( inLayer )
      {
        if ( element == "id"_L1 && path.at( path.size() - 2 ) == "maplayer"_L1 )
          current.id = text;
        else if ( element == "layername"_L1 )
          current.name = text;
        else if ( element == "provider"_L1 )
          current.provider = text;
        else if ( element == "datasource"_L1 )
          current.source = redactDataSource( text ).left( 200 );
        else if ( element == "authid"_L1 && path.contains( "srs"_L1 ) )
          current.crs = text;
      }
    }
  }
  if ( layers.isEmpty() && title.isEmpty() && projectCrs.isEmpty() )
    return QString();

  QString summary = u"QGIS project %1"_s.arg( relativePath );
  if ( !title.isEmpty() || !projectName.isEmpty() )
    summary += u": \"%1\""_s.arg( title.isEmpty() ? projectName : title );
  summary += u", CRS %1, %2 layers.\n"_s.arg( projectCrs.isEmpty() ? u"unknown"_s : projectCrs ).arg( layers.size() );
  if ( !layouts.isEmpty() )
    summary += u"Print layouts: %1\n"_s.arg( layouts.join( ", "_L1 ) );
  summary += "Layers:\n"_L1;
  for ( const Layer &layer : std::as_const( layers ) )
  {
    QStringList details { layer.type };
    if ( !layer.geometry.isEmpty() )
      details << layer.geometry;
    if ( !layer.provider.isEmpty() )
      details << layer.provider;
    if ( !layer.crs.isEmpty() )
      details << layer.crs;
    summary += u"- %1 (%2): %3\n"_s.arg( layer.name.isEmpty() ? layer.id : layer.name, details.join( ", "_L1 ), layer.source );
  }
  return summary;
}

QString QgsAiFileSummarizer::xmlSummary( const QString &relativePath, const QString &content )
{
  QXmlStreamReader xml( content );
  QString root;
  QMap<QString, int> elementCounts;
  QStringList names;
  QStringList texts;
  while ( !xml.atEnd() )
  {
    const QXmlStreamReader::TokenType token = xml.readNext();
    if ( token == QXmlStreamReader::StartElement )
    {
      const QString name = xml.name().toString();
      if ( root.isEmpty() )
        root = name;
      elementCounts[name]++;
      for ( const char *attribute : { "name", "label", "title" } )
      {
        const QString value = xml.attributes().value( QLatin1String( attribute ) ).toString().trimmed();
        if ( !value.isEmpty() && value.size() <= 80 && names.size() < 40 && !names.contains( value ) )
          names << value;
      }
    }
    else if ( token == QXmlStreamReader::Characters && !xml.isWhitespace() )
    {
      const QString text = xml.text().toString().simplified();
      if ( !text.isEmpty() && text.size() <= 80 && texts.size() < 40 && !texts.contains( text ) )
        texts << text;
    }
  }
  if ( root.isEmpty() )
    return QString();

  QList<QPair<int, QString>> counts;
  for ( auto it = elementCounts.constBegin(); it != elementCounts.constEnd(); ++it )
    counts.append( { it.value(), it.key() } );
  std::sort( counts.begin(), counts.end(), []( const auto &a, const auto &b ) { return a.first > b.first || ( a.first == b.first && a.second < b.second ); } );
  QStringList elements;
  for ( int i = 0; i < std::min<qsizetype>( counts.size(), 25 ); ++i )
    elements << u"%1 (%2)"_s.arg( counts.at( i ).second ).arg( counts.at( i ).first );

  QString summary = u"XML file %1, root element <%2>.\nElements: %3\n"_s.arg( relativePath, root, elements.join( ", "_L1 ) );
  if ( !names.isEmpty() )
    summary += u"Names: %1\n"_s.arg( names.join( "; "_L1 ) );
  if ( !texts.isEmpty() )
    summary += u"Text: %1\n"_s.arg( texts.join( "; "_L1 ) );
  return summary;
}
