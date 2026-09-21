/***************************************************************************
    qgsaisecretstore.h
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

#ifndef QGSAISECRETSTORE_H
#define QGSAISECRETSTORE_H

#include <functional>

#include "qgis_app.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

class QObject;

/**
 * Secure AI credentials with asynchronous OS-keychain persistence.
 * Synchronous reads use the session cache or legacy storage during migration.
 * The separate chat/index encryption key remains in the QGIS authentication vault.
 */
class APP_EXPORT QgsAiSecretStore
{
  public:
    struct EncryptionResult
    {
        QString value;
        bool ok = false;
        bool encrypted = false;
        QString errorMessage;
    };

    struct BlobEncryptionResult
    {
        QByteArray value;
        bool ok = false;
        bool encrypted = false;
        QString errorMessage;
    };

    enum class StorageState
    {
      Missing,
      Persistent,
      SessionOnly,
      MigrationPending,
      Failed
    };
    struct SecretResult
    {
        StorageState state = StorageState::Failed;
        QString error;
        bool sessionAllowed = true;
        bool ok() const { return state == StorageState::Persistent || state == StorageState::SessionOnly; }
    };
    using SecretCallback = std::function<void( const SecretResult & )>;
    enum class Operation
    {
      Read,
      Write,
      Remove
    };
    struct BackendResult
    {
        bool ok = false;
        bool notFound = false;
        QString value;
    };
    using BackendCallback = std::function<void( const BackendResult & )>;
    using Backend = std::function<void( Operation, const QString &, const QString &, BackendCallback )>;

    static bool vaultUsable();
    static QString flagKey( const QString &secretKey );
    static QString readSecret( const QString &key, const QStringList &envFallbacks = QStringList() );
    //! Compatibility for blocking OAuth clients; UI saves must use writeSecretAsync.
    static bool writeSecret( const QString &key, const QString &value );
    static void writeSecretAsync( const QString &key, const QString &value, QObject *context, SecretCallback callback );
    static void useForSession( const QString &key, const QString &value );
    static void removeSecret( const QString &key );
    static bool hasSecret( const QString &key );
    static StorageState storageState( const QString &key );
    static void migrateLegacySecrets();
    static void loadSecretsAsync( QObject *context, std::function<void()> callback, bool retry = false );
    static bool secretsLoaded();
    static bool migrationPending();
    //! Injected asynchronous backend: tests never access the user's actual keychain.
    static void setBackendForTesting( Backend backend );
    static void resetCredentialCacheForTesting();
    static QString keychainKey( const QString &key );

    // --- Encryption-at-rest helpers for the AI data stores (RAG index, chat history) ---

    /**
     * Returns the AES-256 data key used to encrypt the AI stores, generating and
     * persisting it in the vault on first use. The key is 24 random bytes encoded
     * as base64 (exactly 32 characters, since QgsAuthCrypto uses the UTF-8 bytes
     * of the passphrase directly as the AES-256 key). Empty when the vault or QCA
     * is unavailable — callers then fall back to plaintext storage.
     */
    static QString dataEncryptionKey();

    //! True when encryptValue/decryptValue can actually encrypt (QCA + vault + data key).
    static bool storageEncryptionAvailable();

    /**
     * Encrypts \a plain with the data key and a per-value random IV. Format:
     * `enc1:<ivHex>:<cipherHex>`. Returns \a plain unchanged when encryption is
     * unavailable, and an empty string for empty input.
     */
    static QString encryptValue( const QString &plain );

    //! Encrypts \a plain and reports whether encryption actually succeeded.
    static EncryptionResult tryEncryptValue( const QString &plain );

    /**
     * Decrypts a value produced by encryptValue(). Values without the `enc1:`
     * prefix are returned as-is (legacy plaintext rows, mixed-mode DBs). Returns
     * an empty string when decryption fails (missing/rotated key), with one
     * warning per session.
     */
    static QString decryptValue( const QString &stored );

    //! Blob variants: base64-encode, then encryptValue. Pass-through for non-encrypted blobs.
    static QByteArray encryptBlob( const QByteArray &blob );
    static BlobEncryptionResult tryEncryptBlob( const QByteArray &blob );
    static QByteArray decryptBlob( const QByteArray &stored );

    //! Logs the "AI data stored unencrypted" warning once per session.
    static void warnPlaintextStorageOnce();

    //! Clears the cached data key (unit tests only).
    static void resetDataKeyCacheForTesting();
};

#endif // QGSAISECRETSTORE_H
