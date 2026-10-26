#include "strataremoteprotocol.h"

#include <QJsonObject>
#include <QtTest>

class TestStrataRemoteProtocol : public QObject
{
    Q_OBJECT
  private slots:
    void roundTrip()
    {
      QByteArray buffer = StrataRemoteProtocol::encode( { { QStringLiteral( "task" ), QStringLiteral( "one" ) } } );
      QJsonObject message;
      QString error;
      QVERIFY( StrataRemoteProtocol::takeFrame( buffer, message, error ) );
      QCOMPARE( message.value( QStringLiteral( "task" ) ).toString(), QStringLiteral( "one" ) );
      QVERIFY( buffer.isEmpty() );
      QVERIFY( error.isEmpty() );
    }

    void rejectsOversizedFrame()
    {
      QByteArray buffer( StrataRemoteProtocol::MaxFrameBytes + 1, 'x' );
      QJsonObject message;
      QString error;
      QVERIFY( !StrataRemoteProtocol::takeFrame( buffer, message, error ) );
      QVERIFY( !error.isEmpty() );
      QVERIFY( buffer.isEmpty() );
    }
};

QTEST_MAIN( TestStrataRemoteProtocol )
#include "test_strataremoteprotocol.moc"
