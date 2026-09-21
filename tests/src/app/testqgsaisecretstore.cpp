/***************************************************************************
    testqgsaisecretstore.cpp
    ---------------------
    begin                : June 2026
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

#include "ai/qgsaisecretstore.h"
#include "qgsaisecretstoretestutils.h"
#include "qgsapplication.h"
#include "qgsauthmanager.h"
#include "qgssettings.h"
#include "qgstest.h"

#include <QScopeGuard>
#include <QString>
#include <qt6keychain/keychain.h>

using namespace Qt::StringLiterals;
using Store = QgsAiSecretStore;
namespace
{
  bool unlockTestVault()
  {
    QgsAuthManager *authManager = QgsApplication::authManager();
    if ( !authManager || authManager->isDisabled() )
      return false;
    if ( authManager->masterPasswordIsSet() )
      return true;
    return authManager->setMasterPassword( u"qgsai_test_master"_s, true );
  }
} //namespace
class TestQgsAiSecretStore : public QObject
{
    Q_OBJECT
    std::shared_ptr<QHash<QString, QString>> values;
    const QString key = u"ai/provider/openai/apiKey"_s;
    void load()
    {
      bool done = false;
      Store::loadSecretsAsync( this, [&done]() { done = true; }, true );
      QTRY_VERIFY( done );
    }
  private slots:
    void init()
    {
      values = installTestSecretBackend();
      QgsSettings().remove( u"ai/provider"_s );
    }
    void cleanup() { QCoreApplication::processEvents(); }
    void verifiedWriteHasNoPlaintext()
    {
      bool done = false;
      Store::writeSecretAsync( key, u"private-key"_s, this, [&done]( const Store::SecretResult &result ) {
        QCOMPARE( result.state, Store::StorageState::Persistent );
        done = true;
      } );
      QVERIFY( !done );
      QTRY_VERIFY( done );
      QCOMPARE( Store::readSecret( key ), u"private-key"_s );
      QVERIFY( !QgsSettings().contains( key ) );
      QCOMPARE( values->value( Store::keychainKey( key ) ), u"private-key"_s );
    }
    void failedSavePreservesPreviousCredential()
    {
      QgsSettings().setValue( key, u"previous"_s );
      Store::setBackendForTesting( []( Store::Operation, const QString &, const QString &, Store::BackendCallback done ) { done( {} ); } );
      QVERIFY( !Store::writeSecret( key, u"replacement"_s ) );
      QCOMPARE( QgsSettings().value( key ).toString(), u"previous"_s );
      QCOMPARE( Store::readSecret( key ), u"previous"_s );
    }
    void failedVerificationDoesNotDeleteLegacy()
    {
      QgsSettings().setValue( key, u"legacy"_s );
      Store::setBackendForTesting( []( Store::Operation operation, const QString &, const QString &, Store::BackendCallback done ) {
        done( { true, false, operation == Store::Operation::Read ? u"different"_s : QString() } );
      } );
      QVERIFY( !Store::writeSecret( key, u"legacy"_s ) );
      QCOMPARE( QgsSettings().value( key ).toString(), u"legacy"_s );
    }
    void sessionOnlyDoesNotPersist()
    {
      Store::useForSession( key, u"temporary"_s );
      QCOMPARE( Store::readSecret( key ), u"temporary"_s );
      QCOMPARE( Store::storageState( key ), Store::StorageState::SessionOnly );
      QVERIFY( values->isEmpty() );
      QVERIFY( !QgsSettings().contains( key ) );
      Store::resetCredentialCacheForTesting();
      QVERIFY( Store::readSecret( key ).isEmpty() );
    }
    void migrationSurvivesRestart()
    {
      QgsSettings().setValue( key, u"legacy"_s );
      load();
      QVERIFY( !QgsSettings().contains( key ) );
      QVERIFY( !Store::migrationPending() );
      Store::resetCredentialCacheForTesting();
      load();
      QCOMPARE( Store::readSecret( key ), u"legacy"_s );
      load(); // Idempotent.
      QCOMPARE( Store::readSecret( key ), u"legacy"_s );
    }
    void lockedVaultStaysPending()
    {
      QgsSettings().setValue( Store::flagKey( key ), true );
      load();
      QVERIFY( Store::migrationPending() );
      QVERIFY( QgsSettings().value( Store::flagKey( key ) ).toBool() );
    }
    void deniedBackendDoesNotLoseLegacy()
    {
      QgsSettings().setValue( key, u"legacy"_s );
      Store::setBackendForTesting( []( Store::Operation, const QString &, const QString &, Store::BackendCallback done ) { done( {} ); } );
      load();
      QCOMPARE( Store::storageState( key ), Store::StorageState::MigrationPending );
      QCOMPARE( Store::readSecret( key ), u"legacy"_s );
      QVERIFY( Store::migrationPending() );
    }
    void disconnectDoesNotResurrectAfterFailedDeletion()
    {
      QVERIFY( Store::writeSecret( key, u"saved"_s ) );
      const auto saved = values;
      Store::setBackendForTesting( [saved]( Store::Operation operation, const QString &name, const QString &, Store::BackendCallback done ) {
        done( { operation == Store::Operation::Read, false, saved->value( name ) } );
      } );
      Store::removeSecret( key );
      QCoreApplication::processEvents();
      Store::resetCredentialCacheForTesting();
      load();
      QVERIFY( !Store::hasSecret( key ) );
      QVERIFY( Store::readSecret( key ).isEmpty() );
    }
    void retiredTokensCannotBeReadOrWritten()
    {
      const QString retired = u"ai/provider/claude/subscriptionToken"_s;
      QgsSettings().setValue( retired, u"old"_s );
      QVERIFY( !Store::writeSecret( retired, u"new"_s ) );
      Store::useForSession( retired, u"new"_s );
      QVERIFY( Store::readSecret( retired ).isEmpty() );
      QVERIFY( !Store::hasSecret( retired ) );
    }
    void environmentNotPersisted()
    {
      qputenv( "STRATA_TEST_SECRET", "environment-only" );
      const auto cleanup = qScopeGuard( []() { qunsetenv( "STRATA_TEST_SECRET" ); } );
      QCOMPARE( Store::readSecret( key, { u"STRATA_TEST_SECRET"_s } ), u"environment-only"_s );
      QVERIFY( values->isEmpty() );
      QVERIFY( !QgsSettings().contains( key ) );
    }
    void interruptedMigrationRecoversAllCredentialTypes()
    {
      const QStringList keys { u"ai/provider/openai/apiKey"_s, u"ai/provider/openrouter/apiKey"_s, u"ai/provider/claude/apiKey"_s, u"ai/provider/plan/token"_s, u"ai/provider/codex/oauth/refreshToken"_s };
      for ( const QString &name : keys )
      {
        QgsSettings().setValue( name, u"existing"_s );
        values->insert( Store::keychainKey( name ), u"existing"_s );
      }
      load();
      for ( const QString &name : keys )
      {
        QVERIFY( !QgsSettings().contains( name ) );
        QCOMPARE( Store::readSecret( name ), u"existing"_s );
      }
      QVERIFY( !Store::migrationPending() );
    }
    void failedReadbackRestoresPreviousSecureValue()
    {
      QString secure = u"previous"_s;
      int reads = 0;
      Store::setBackendForTesting( [&secure, &reads]( Store::Operation operation, const QString &, const QString &value, Store::BackendCallback done ) {
        if ( operation == Store::Operation::Write )
        {
          secure = value;
          done( { true, false, {} } );
        }
        else if ( operation == Store::Operation::Read )
        {
          ++reads;
          done( reads == 2 ? Store::BackendResult() : Store::BackendResult { true, false, secure } );
        }
        else
          done( {} );
      } );
      QVERIFY( !Store::writeSecret( key, u"replacement"_s ) );
      QCOMPARE( secure, u"previous"_s );
      Store::useForSession( key, u"replacement"_s );
      QCOMPARE( Store::readSecret( key ), u"replacement"_s );
      QCOMPARE( secure, u"previous"_s ); // The session-only replacement is never left on disk.
    }
    void disconnectDuringPendingWrite()
    {
      QString secure;
      Store::BackendCallback pending;
      Store::setBackendForTesting( [&secure, &pending]( Store::Operation operation, const QString &, const QString &value, Store::BackendCallback done ) {
        if ( operation == Store::Operation::Write )
        {
          secure = value;
          pending = done;
        }
        else if ( operation == Store::Operation::Remove )
        {
          secure.clear();
          done( { true, false, {} } );
        }
        else
          done( { !secure.isEmpty(), secure.isEmpty(), secure } );
      } );
      bool done = false;
      Store::writeSecretAsync( key, u"pending"_s, this, [&done]( const Store::SecretResult &result ) {
        QVERIFY( !result.ok() );
        done = true;
      } );
      QTRY_VERIFY( pending );
      Store::removeSecret( key );
      QCoreApplication::processEvents();
      pending( { true, false, {} } );
      QTRY_VERIFY( done );
      QVERIFY( secure.isEmpty() );
      QVERIFY( !Store::hasSecret( key ) );
    }
    void destroyedContextDoesNotStartSave()
    {
      auto *context = new QObject();
      Store::writeSecretAsync( key, u"cancelled"_s, context, []( const Store::SecretResult & ) { QFAIL( "Destroyed owner received a callback" ); } );
      delete context;
      QCoreApplication::processEvents();
      QVERIFY( values->isEmpty() );
      QVERIFY( !Store::hasSecret( key ) );
    }
    void nativeKeychainRoundTrip()
    {
      if ( qEnvironmentVariable( "STRATA_TEST_NATIVE_KEYCHAIN" ) != "1"_L1 )
        QSKIP( "Opt in with STRATA_TEST_NATIVE_KEYCHAIN=1 on a desktop with an unlocked OS keychain." );
      const auto ci = qgetenv( "QGIS_CONTINUOUS_INTEGRATION_RUN" );
      qunsetenv( "QGIS_CONTINUOUS_INTEGRATION_RUN" );
      const auto cleanup = qScopeGuard( [ci]() { qputenv( "QGIS_CONTINUOUS_INTEGRATION_RUN", ci ); } );
      Store::setBackendForTesting( {} );
      QVERIFY( Store::writeSecret( key, u"strata-disposable-native-keychain-test"_s ) );
      QVERIFY( !QgsSettings().contains( key ) );
      Store::resetCredentialCacheForTesting();
      load();
      QCOMPARE( Store::readSecret( key ), u"strata-disposable-native-keychain-test"_s );
      Store::removeSecret( key );
      bool removed = false;
      const auto inspect = [&removed, this]() {
        auto *read = new QKeychain::ReadPasswordJob( u"Strata AI"_s, this );
        read->setKey( Store::keychainKey( key ) );
        read->setInsecureFallback( false );
        connect( read, &QKeychain::Job::finished, this, [&removed]( QKeychain::Job *job ) { removed = job->error() == QKeychain::EntryNotFound; } );
        read->start();
      };
      QTimer::singleShot( 250, this, inspect );
      QTRY_VERIFY_WITH_TIMEOUT( removed, 30000 );
      QVERIFY( !Store::hasSecret( key ) );
    }
    void dataKeyIsNotMigrated()
    {
      QgsSettings().setValue( u"ai/storage/hasDataKey"_s, true );
      load();
      QVERIFY( QgsSettings().value( u"ai/storage/hasDataKey"_s ).toBool() );
      QVERIFY( !values->contains( Store::keychainKey( u"ai/storage/dataKey"_s ) ) );
      QgsSettings().remove( u"ai/storage/hasDataKey"_s );
    }
    void plaintextPassthroughWithoutVault()
    {
      QgsAuthManager *authManager = QgsApplication::authManager();
      if ( authManager && authManager->masterPasswordIsSet() )
        QSKIP( "Master password already set in this environment: plaintext mode not reachable." );

      QgsAiSecretStore::resetDataKeyCacheForTesting();
      QVERIFY( !QgsAiSecretStore::storageEncryptionAvailable() );

      // Without a data key, encryptValue is a pass-through and decryptValue keeps
      // legacy plaintext readable.
      QCOMPARE( QgsAiSecretStore::encryptValue( u"plain text"_s ), u"plain text"_s );
      QCOMPARE( QgsAiSecretStore::decryptValue( u"plain text"_s ), u"plain text"_s );
      const QByteArray blob( "\x01\x02\x03", 3 );
      QCOMPARE( QgsAiSecretStore::encryptBlob( blob ), blob );
      QCOMPARE( QgsAiSecretStore::decryptBlob( blob ), blob );
    }

    void dataKeyBootstrapAndRoundTrip()
    {
      if ( !unlockTestVault() )
        QSKIP( "Authentication vault unavailable in this environment." );

      QgsSettings settings;
      const auto cleanup = qScopeGuard( [&settings]() {
        QgsAuthManager *authManager = QgsApplication::authManager();
        if ( authManager )
          authManager->removeAuthSetting( u"ai/storage/dataKey"_s );
        settings.remove( u"ai/storage/hasDataKey"_s );
        QgsAiSecretStore::resetDataKeyCacheForTesting();
      } );

      QgsAiSecretStore::resetDataKeyCacheForTesting();

      // First call generates a 32-char base64 key and persists it; later calls are stable.
      const QString key = QgsAiSecretStore::dataEncryptionKey();
      QCOMPARE( key.size(), 32 );
      QVERIFY( settings.value( u"ai/storage/hasDataKey"_s, false ).toBool() );
      QCOMPARE( QgsAiSecretStore::dataEncryptionKey(), key );
      QgsAiSecretStore::resetDataKeyCacheForTesting();
      QCOMPARE( QgsAiSecretStore::dataEncryptionKey(), key );
      QVERIFY( QgsAiSecretStore::storageEncryptionAvailable() );

      // Value round trip with per-call random IVs.
      const QString secretText = u"sensitive layer attribute"_s;
      const QString encryptedA = QgsAiSecretStore::encryptValue( secretText );
      const QString encryptedB = QgsAiSecretStore::encryptValue( secretText );
      QVERIFY( encryptedA.startsWith( "enc1:"_L1 ) );
      QVERIFY( encryptedA != encryptedB ); // different IVs
      QVERIFY( !encryptedA.contains( u"sensitive"_s ) );
      QCOMPARE( QgsAiSecretStore::decryptValue( encryptedA ), secretText );
      QCOMPARE( QgsAiSecretStore::decryptValue( encryptedB ), secretText );

      // Blob round trip (embeddings, WKT).
      QByteArray blob;
      for ( int i = 0; i < 64; ++i )
        blob.append( static_cast<char>( i ) );
      const QByteArray encryptedBlob = QgsAiSecretStore::encryptBlob( blob );
      QVERIFY( encryptedBlob.startsWith( "enc1:" ) );
      QCOMPARE( QgsAiSecretStore::decryptBlob( encryptedBlob ), blob );

      // Legacy plaintext values still pass through.
      QCOMPARE( QgsAiSecretStore::decryptValue( u"legacy plaintext"_s ), u"legacy plaintext"_s );
    }
};
QGSTEST_MAIN( TestQgsAiSecretStore )
#include "testqgsaisecretstore.moc"
