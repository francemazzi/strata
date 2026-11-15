/***************************************************************************
    qgsbundledqcaprovider_p.h
    -------------------------
    SPDX-License-Identifier: GPL-2.0-or-later
 ***************************************************************************/

#ifndef QGSBUNDLEDQCAPROVIDER_P_H
#define QGSBUNDLEDQCAPROVIDER_P_H

#include <QCoreApplication>
#include <QDir>
#include <QPluginLoader>
#include <QtCrypto>
#include <qcaprovider.h>

namespace QgsBundledQcaProvider
{
  inline QString path()
  {
    using namespace Qt::StringLiterals;
    return QDir::cleanPath( QCoreApplication::applicationDirPath() + u"/../PlugIns/crypto/libqca-ossl.dylib"_s );
  }

  inline bool registerProvider( const QString &providerPath, QString &error )
  {
    using namespace Qt::StringLiterals;
    // QCA owns the provider. Its plugin must remain loaded through QCA teardown.
    QPluginLoader loader( providerPath );
    loader.setLoadHints( QLibrary::PreventUnloadHint );
    if ( !loader.load() )
    {
      error = u"Could not load bundled QCA OpenSSL provider: %1"_s.arg( loader.errorString() );
      return false;
    }

    auto *plugin = qobject_cast< QCAPlugin * >( loader.instance() );
    if ( !plugin )
    {
      error = u"Bundled QCA OpenSSL provider does not implement QCAPlugin"_s;
      return false;
    }

    QCA::Provider *provider = plugin->createProvider();
    if ( !provider || !QCA::insertProvider( provider, 0 ) )
    {
      delete provider;
      error = u"Could not register bundled QCA OpenSSL provider"_s;
      return false;
    }
    return true;
  }
} // namespace QgsBundledQcaProvider

#endif
