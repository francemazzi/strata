// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

namespace QgsAiDiscoveryPresentation
{
  inline QString runResults( const QJsonObject &run )
  {
    using namespace Qt::StringLiterals;
    QStringList lines;
    for ( const auto &value : run.value( u"results"_s ).toArray() )
    {
      const auto result = value.toObject();
      const auto content = result.value( u"content"_s ).toObject();
      lines << QObject::tr( "%1: %2 · %3 MiB ricevuti · %4 crediti" )
                 .arg( result.value( u"title"_s ).toString(), result.value( u"status"_s ).toString() )
                 .arg( result.value( u"transferredBytes"_s ).toDouble() / 1048576., 0, 'f', 2 )
                 .arg( result.value( u"creditsCharged"_s ).toInt() );
      if ( !content.isEmpty() )
        lines << QObject::tr( "Contenuto acquisito: %1 · %2" ).arg( content.value( u"kind"_s ).toString(), content.value( u"reason"_s ).toString() );
      for ( const auto &issue : result.value( u"issues"_s ).toArray() )
        lines << issue.toString();
      for ( const auto &inspection : result.value( u"inspections"_s ).toArray() )
      {
        const auto layer = inspection.toObject();
        lines << QObject::tr( "Verifica GIS: %1 · CRS %2" ).arg( layer.value( u"path"_s ).toString(), layer.value( u"nativeCrs"_s ).toString() );
      }
    }
    if ( !run.value( u"error"_s ).toString().isEmpty() )
      lines << run.value( u"error"_s ).toString();
    return lines.join( '\n' );
  }
} //namespace QgsAiDiscoveryPresentation
