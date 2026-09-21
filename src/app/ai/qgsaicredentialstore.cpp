/***************************************************************************
    qgsaicredentialstore.cpp
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

#include <memory>
#include <utility>

#include "qgsaisecretstore.h"
#include "qgsapplication.h"
#include "qgsauthmanager.h"
#include "qgssettings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QTimer>
#include <qt6keychain/keychain.h>

using namespace Qt::StringLiterals;
namespace QgsAiCredentialStoreInternal
{
  using Store = QgsAiSecretStore;
  using State = Store::StorageState;
  struct Entry
  {
      QString value;
      State state = State::Missing;
      int generation = 0;
      bool writing = false;
  };
  QMutex cacheMutex;
  QHash<QString, Entry> cache;
  Store::Backend testBackend;
  bool loaded = false;
  bool loading = false;
  QList<std::function<void()>> waiters;
  const QStringList keys = { u"ai/provider/openai/apiKey"_s, u"ai/provider/openrouter/apiKey"_s, u"ai/provider/claude/apiKey"_s, u"ai/provider/codex/oauth/refreshToken"_s, u"ai/provider/plan/token"_s };
  QString presenceKey( const QString &key )
  {
    return key + u"_inKeychain"_s;
  }
  QString removedKey( const QString &key )
  {
    return key + u"_removed"_s;
  }
  bool retired( const QString &key )
  {
    return key == "ai/provider/claude/subscriptionToken"_L1 || key.startsWith( "ai/provider/claude/oauth/"_L1 );
  }

  void performBackend( Store::Operation operation, const QString &key, const QString &value, Store::BackendCallback callback )
  {
    if ( testBackend )
    {
      testBackend( operation, Store::keychainKey( key ), value, std::move( callback ) );
      return;
    }
    if ( qEnvironmentVariable( "QGIS_CONTINUOUS_INTEGRATION_RUN" ) == "true"_L1 )
    {
      QTimer::singleShot( 0, qApp, [callback]() { callback( {} ); } );
      return;
    }
    QKeychain::Job *job = nullptr;
    if ( operation == Store::Operation::Read )
      job = new QKeychain::ReadPasswordJob( u"Strata AI"_s, qApp );
    else if ( operation == Store::Operation::Write )
    {
      auto *write = new QKeychain::WritePasswordJob( u"Strata AI"_s, qApp );
      write->setTextData( value );
      job = write;
    }
    else
      job = new QKeychain::DeletePasswordJob( u"Strata AI"_s, qApp );
    job->setKey( Store::keychainKey( key ) );
    job->setInsecureFallback( false );
    const auto completed = std::make_shared<bool>( false );
    QObject::connect( job, &QKeychain::Job::finished, qApp, [callback, completed, operation]( QKeychain::Job *finished ) {
      if ( std::exchange( *completed, true ) )
        return;
      Store::BackendResult result;
      result.ok = finished->error() == QKeychain::NoError;
      result.notFound = finished->error() == QKeychain::EntryNotFound;
      if ( result.ok && operation == Store::Operation::Read )
        result.value = static_cast<QKeychain::ReadPasswordJob *>( finished )->textData();
      callback( result );
    } );
    // A write must finish before Retry/session-only is offered: an artificial
    // timeout could report failure while the OS is still persisting the secret.
    job->start();
  }

  // Serialize OS operations for each credential, including disconnect during a save.
  QHash<QString, QQueue<std::function<void()>>> operations;
  void backend( Store::Operation operation, const QString &key, const QString &value, Store::BackendCallback callback )
  {
    auto &queue = operations[key];
    queue.enqueue( [operation, key, value, callback]() {
      performBackend( operation, key, value, [key, callback]( const Store::BackendResult &result ) {
        callback( result );
        auto &pending = operations[key];
        pending.dequeue();
        if ( pending.isEmpty() )
          operations.remove( key );
        else
        {
          const auto next = pending.head();
          next();
        }
      } );
    } );
    if ( queue.size() == 1 )
    {
      const auto start = queue.head();
      start();
    }
  }

  QString legacyValue( const QString &key )
  {
    if ( Store::vaultUsable() )
    {
      auto *auth = QgsApplication::authManager();
      if ( auth->existsAuthSetting( key ) )
        return auth->authSetting( key, QVariant(), true ).toString();
    }
    return QgsSettings().value( key ).toString();
  }

  void eraseLegacy( const QString &key )
  {
    QgsSettings settings;
    settings.remove( key );
    auto *auth = QgsApplication::authManager();
    if ( auth && !auth->isDisabled() )
    {
      if ( !auth->existsAuthSetting( key ) || auth->removeAuthSetting( key ) )
        settings.remove( Store::flagKey( key ) );
    }
  }

  bool legacyExists( const QString &key )
  {
    QgsSettings settings;
    auto *auth = QgsApplication::authManager();
    return settings.contains( key ) || settings.value( Store::flagKey( key ), false ).toBool() || ( auth && !auth->isDisabled() && auth->existsAuthSetting( key ) );
  }

  void loadNext( int index )
  {
    if ( index == keys.size() )
    {
      loaded = true;
      loading = false;
      const auto callbacks = std::exchange( waiters, {} );
      for ( const auto &callback : callbacks )
        callback();
      return;
    }
    const QString key = keys.at( index );
    if ( QgsSettings().value( removedKey( key ), false ).toBool() || ( !QgsSettings().value( presenceKey( key ), false ).toBool() && !legacyExists( key ) ) )
    {
      loadNext( index + 1 );
      return;
    }
    int generation;
    {
      QMutexLocker lock( &cacheMutex );
      if ( cache[key].writing || cache[key].state == State::SessionOnly )
      {
        lock.unlock();
        loadNext( index + 1 );
        return;
      }
      generation = cache[key].generation;
    }
    backend( Store::Operation::Read, key, {}, [key, index, generation]( const Store::BackendResult &result ) {
      {
        QMutexLocker lock( &cacheMutex );
        if ( cache[key].generation != generation )
        {
          lock.unlock();
          loadNext( index + 1 );
          return;
        }
        if ( result.ok && !result.value.isEmpty() )
        {
          cache[key].value = result.value;
          cache[key].state = State::Persistent;
          QgsSettings().setValue( presenceKey( key ), true );
          lock.unlock();
          // An earlier migration may have been interrupted after verification.
          if ( legacyExists( key ) && legacyValue( key ) == result.value )
            eraseLegacy( key );
          loadNext( index + 1 );
          return;
        }
      }
      const QString old = legacyValue( key );
      if ( !old.isEmpty() && ( result.notFound || !QgsSettings().value( presenceKey( key ), false ).toBool() ) )
      {
        {
          QMutexLocker lock( &cacheMutex );
          cache[key].value = old;
          cache[key].state = State::MigrationPending;
        }
        Store::writeSecretAsync( key, old, qApp, [index]( const Store::SecretResult & ) { loadNext( index + 1 ); } );
        return;
      }
      {
        QMutexLocker lock( &cacheMutex );
        cache[key].state = legacyExists( key ) ? State::MigrationPending : result.notFound ? State::Missing : State::Failed;
        if ( cache[key].state == State::Missing )
          QgsSettings().remove( presenceKey( key ) );
      }
      loadNext( index + 1 );
    } );
  }
} //namespace QgsAiCredentialStoreInternal

QString QgsAiSecretStore::keychainKey( const QString &key )
{
  using namespace QgsAiCredentialStoreInternal;
  const QByteArray profile = QDir::cleanPath( QgsApplication::qgisSettingsDirPath() ).toUtf8();
  return QString::fromLatin1( QCryptographicHash::hash( profile, QCryptographicHash::Sha256 ).toHex() ) + u'/' + key;
}

void QgsAiSecretStore::writeSecretAsync( const QString &key, const QString &value, QObject *context, SecretCallback callback )
{
  using namespace QgsAiCredentialStoreInternal;
  QPointer<QObject> guard( context );
  QMetaObject::invokeMethod(
    qApp,
    [key, value, guard, callback]() {
      if ( !guard )
        return;
      const auto finish = [guard, callback]( const SecretResult &result ) {
        if ( guard )
          callback( result );
      };
      if ( retired( key ) || value.trimmed().isEmpty() || ( key == "ai/provider/claude/apiKey"_L1 && value.startsWith( "sk-ant-oat"_L1 ) ) )
      {
        finish( { State::Failed, QObject::tr( "This credential cannot be saved." ), false } );
        return;
      }
      int generation;
      {
        QMutexLocker lock( &cacheMutex );
        if ( cache[key].writing )
        {
          lock.unlock();
          finish( { State::Failed, QObject::tr( "A save is already in progress. Please retry." ), false } );
          return;
        }
        if ( cache[key].value == value && cache[key].state == State::Persistent )
        {
          const auto state = cache[key].state;
          lock.unlock();
          finish( { state, {} } );
          return;
        }
        cache[key].writing = true;
        generation = ++cache[key].generation;
      }
      const auto current = [key, generation]() {
        QMutexLocker lock( &cacheMutex );
        return cache[key].generation == generation;
      };
      const auto complete = [key, generation, finish]( const SecretResult &result ) {
        {
          QMutexLocker lock( &cacheMutex );
          if ( cache[key].generation == generation )
            cache[key].writing = false;
        }
        finish( result );
      };
      const SecretResult unavailable { State::Failed, QObject::tr( "The system keychain is unavailable. Retry or use this credential for this session only." ) };
      // Snapshot the existing secure value before replacing it. A failed read-back
      // restores that value; legacy storage is removed only after verification.
      backend( Operation::Read, key, {}, [key, value, current, complete, unavailable]( const BackendResult &previous ) {
        if ( !current() || ( !previous.ok && !previous.notFound ) )
        {
          complete( unavailable );
          return;
        }
        backend( Operation::Write, key, value, [key, value, previous, current, complete, unavailable]( const BackendResult &written ) {
          if ( !current() )
          {
            complete( unavailable );
            return;
          }
          const auto verify = [key, value, previous, current, complete, unavailable]( const BackendResult &result ) {
            if ( !current() )
            {
              complete( unavailable );
              return;
            }
            if ( result.ok && result.value == value )
            {
              {
                QMutexLocker lock( &cacheMutex );
                cache[key].value = value;
                cache[key].state = State::Persistent;
              }
              QgsSettings().setValue( presenceKey( key ), true );
              QgsSettings().remove( removedKey( key ) );
              eraseLegacy( key );
              complete( { State::Persistent, {} } );
              return;
            }
            const Operation undo = previous.ok ? Operation::Write : Operation::Remove;
            backend( undo, key, previous.value, [key, previous, current, complete, unavailable]( const BackendResult & ) {
              if ( !current() )
              {
                complete( unavailable );
                return;
              }
              backend( Operation::Read, key, {}, [previous, complete, unavailable]( const BackendResult &restored ) {
                if ( ( previous.ok && restored.ok && restored.value == previous.value ) || ( previous.notFound && restored.notFound ) )
                  complete( unavailable );
                else
                  complete( { State::Failed, QObject::tr( "The keychain could not confirm or undo this save. Retry to complete secure storage." ), false } );
              } );
            } );
          };
          if ( !written.ok )
          {
            verify( {} );
            return;
          }
          backend( Operation::Read, key, {}, verify );
        } );
      } );
    },
    Qt::QueuedConnection
  );
}

bool QgsAiSecretStore::writeSecret( const QString &key, const QString &value )
{
  using namespace QgsAiCredentialStoreInternal;
  {
    QMutexLocker lock( &cacheMutex );
    const Entry entry = cache.value( key );
    if ( !retired( key ) && entry.value == value && !value.isEmpty() && ( entry.state == State::Persistent || entry.state == State::SessionOnly ) )
      return true;
  }
  // Compatibility for existing blocking OAuth exchanges. Event processing continues;
  // all interactive credential saves use the asynchronous API instead.
  QEventLoop loop;
  bool ok = false;
  writeSecretAsync( key, value, &loop, [&loop, &ok]( const SecretResult &result ) {
    ok = result.ok();
    loop.quit();
  } );
  loop.exec();
  return ok;
}

void QgsAiSecretStore::useForSession( const QString &key, const QString &value )
{
  using namespace QgsAiCredentialStoreInternal;
  if ( retired( key ) || value.isEmpty() || ( key == "ai/provider/claude/apiKey"_L1 && value.startsWith( "sk-ant-oat"_L1 ) ) )
    return;
  QMutexLocker lock( &cacheMutex );
  auto &entry = cache[key];
  if ( entry.writing )
    return;
  ++entry.generation;
  entry.value = value;
  entry.state = State::SessionOnly;
  entry.writing = false;
}

QString QgsAiSecretStore::readSecret( const QString &key, const QStringList &envFallbacks )
{
  using namespace QgsAiCredentialStoreInternal;
  if ( retired( key ) )
    return {};
  {
    QMutexLocker lock( &cacheMutex );
    const auto entry = cache.value( key );
    if ( !entry.value.isEmpty() )
      return entry.value;
  }
  if ( !QgsSettings().value( removedKey( key ), false ).toBool() && !QgsSettings().value( presenceKey( key ), false ).toBool() )
  {
    const QString old = legacyValue( key );
    if ( !old.isEmpty() )
      return old;
  }
  for ( const QString &env : envFallbacks )
  {
    const QString value = qEnvironmentVariable( env.toUtf8().constData() ).trimmed();
    if ( !value.isEmpty() )
      return value;
  }
  return {};
}

bool QgsAiSecretStore::hasSecret( const QString &key )
{
  using namespace QgsAiCredentialStoreInternal;
  if ( retired( key ) )
    return false;
  {
    QMutexLocker lock( &cacheMutex );
    if ( !cache.value( key ).value.isEmpty() )
      return true;
  }
  return !QgsSettings().value( removedKey( key ), false ).toBool() && ( QgsSettings().value( presenceKey( key ), false ).toBool() || legacyExists( key ) );
}

void QgsAiSecretStore::removeSecret( const QString &key )
{
  using namespace QgsAiCredentialStoreInternal;
  {
    QMutexLocker lock( &cacheMutex );
    const int generation = cache[key].generation + 1;
    cache[key] = Entry();
    cache[key].generation = generation;
  }
  // A failed OS deletion must never reconnect the user at the next launch.
  QgsSettings().setValue( removedKey( key ), true );
  QgsSettings().remove( presenceKey( key ) );
  eraseLegacy( key );
  QMetaObject::invokeMethod( qApp, [key]() { backend( Operation::Remove, key, {}, []( const BackendResult & ) {} ); }, Qt::QueuedConnection );
}

QgsAiSecretStore::StorageState QgsAiSecretStore::storageState( const QString &key )
{
  using namespace QgsAiCredentialStoreInternal;
  QMutexLocker lock( &cacheMutex );
  return cache.value( key ).state;
}
bool QgsAiSecretStore::migrationPending()
{
  using namespace QgsAiCredentialStoreInternal;
  for ( const QString &key : keys )
    if ( legacyExists( key ) )
      return true;
  return false;
}
bool QgsAiSecretStore::secretsLoaded()
{
  using namespace QgsAiCredentialStoreInternal;
  return loaded;
}
void QgsAiSecretStore::loadSecretsAsync( QObject *context, std::function<void()> callback, bool retry )
{
  using namespace QgsAiCredentialStoreInternal;
  QPointer<QObject> guard( context );
  QMetaObject::invokeMethod(
    qApp,
    [guard, callback, retry]() {
      const auto finish = [guard, callback]() {
        if ( guard )
          callback();
      };
      if ( loaded && !retry )
      {
        finish();
        return;
      }
      waiters << finish;
      if ( loading )
        return;
      loading = true;
      loadNext( 0 );
    },
    Qt::QueuedConnection
  );
}
void QgsAiSecretStore::migrateLegacySecrets()
{
  using namespace QgsAiCredentialStoreInternal;
  loadSecretsAsync( qApp, []() {} );
}
void QgsAiSecretStore::setBackendForTesting( Backend backendFunction )
{
  using namespace QgsAiCredentialStoreInternal;
  testBackend = std::move( backendFunction );
  resetCredentialCacheForTesting();
}
void QgsAiSecretStore::resetCredentialCacheForTesting()
{
  using namespace QgsAiCredentialStoreInternal;
  QMutexLocker lock( &cacheMutex );
  cache.clear();
  loaded = false;
  loading = false;
  waiters.clear();
}
