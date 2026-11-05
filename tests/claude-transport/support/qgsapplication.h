#ifndef STRATA_TRANSPORT_QGSAPPLICATION_H
#define STRATA_TRANSPORT_QGSAPPLICATION_H
#include <QDir>
// The production adapter returns the active QGIS profile; unit tests isolate it.
class QgsApplication
{
  public:
    static QString qgisSettingsDirPath() { return QDir( QString::fromUtf8( qgetenv( "QGIS_CUSTOM_CONFIG_PATH" ) ) ).absolutePath() + QDir::separator(); }
};
#endif
