#include "strataremoteclient.h"
#include "strataremoteprotocol.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int main( int argc, char **argv )
{
  QCoreApplication app( argc, argv );
  QCoreApplication::setApplicationName( QStringLiteral( "strata-runner" ) );
  QCoreApplication::setApplicationVersion( QStringLiteral( "1.0" ) );
  QCoreApplication::setOrganizationName( QStringLiteral( "Strata" ) );

  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOption( { QStringLiteral( "pair" ), QStringLiteral( "One-time pairing code." ), QStringLiteral( "code" ) } );
  parser.addOption( { QStringLiteral( "api-url" ), QStringLiteral( "STRATA API base URL (HTTPS, or loopback for local staging)." ), QStringLiteral( "url" ) } );
  parser.addOption( { QStringLiteral( "fingerprint" ), QStringLiteral( "Print the fingerprint for a local STRATA project and exit." ), QStringLiteral( "project" ) } );
  parser.process( app );

  if ( parser.isSet( QStringLiteral( "fingerprint" ) ) )
  {
    const QString fingerprint = StrataRemoteProtocol::projectFingerprint( parser.value( QStringLiteral( "fingerprint" ) ) );
    if ( fingerprint.isEmpty() )
      return 2;
    QTextStream( stdout ) << fingerprint << Qt::endl;
    return 0;
  }

  StrataRemoteClient client;
  if ( parser.isSet( QStringLiteral( "pair" ) ) )
  {
    const QUrl apiUrl( parser.value( QStringLiteral( "api-url" ) ) );
    if ( !apiUrl.isValid() || apiUrl.isEmpty() )
      parser.showHelp( 2 );
    client.pair( apiUrl, parser.value( QStringLiteral( "pair" ) ) );
  }
  else
  {
    client.start();
  }
  return app.exec();
}
