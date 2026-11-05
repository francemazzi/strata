#ifndef STRATA_TRANSPORT_QGSTEST_H
#define STRATA_TRANSPORT_QGSTEST_H
#include <QtTest>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QScopeGuard>
#include "qgssettings.h"
using namespace Qt::StringLiterals;
#define QGSTEST_MAIN( TestObject )                                                    \
  int main( int argc, char **argv )                                                   \
  {                                                                                   \
    QCoreApplication::setOrganizationName( u"Strata-Transport-Test"_s );              \
    QCoreApplication::setApplicationName( u"Claude-Transport"_s );                    \
    QTemporaryDir profile;                                                            \
    qputenv( "QGIS_CUSTOM_CONFIG_PATH", profile.path().toUtf8() );                    \
    QSettings::setDefaultFormat( QSettings::IniFormat );                              \
    QSettings::setPath( QSettings::IniFormat, QSettings::UserScope, profile.path() ); \
    QCoreApplication app( argc, argv );                                               \
    TestObject tc;                                                                    \
    return QTest::qExec( &tc, argc, argv );                                           \
  }
#endif
