#ifndef STRATA_TRANSPORT_QGSSETTINGS_H
#define STRATA_TRANSPORT_QGSSETTINGS_H
#include <QSettings>
// Production QgsSettings exposes this QSettings API. Keep real persistent I/O.
class QgsSettings : public QSettings
{};
#endif
