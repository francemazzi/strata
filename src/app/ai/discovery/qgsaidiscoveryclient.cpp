// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaidiscoveryclient.h"

#include <memory>

#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <QUuid>

#include "moc_qgsaidiscoveryclient.cpp"

using namespace Qt::StringLiterals;

QgsAiDiscoveryClient::QgsAiDiscoveryClient( QNetworkAccessManager *network, Auth auth, QObject *parent )
  : QObject( parent )
  , mNetwork( network )
  , mAuth( std::move( auth ) )
{
  mTimer.setInterval( 2000 );
  connect( &mTimer, &QTimer::timeout, this, &QgsAiDiscoveryClient::poll );
}
void QgsAiDiscoveryClient::setScope( const QUrl &base, const QString &settingsKey )
{
  if ( mScope == settingsKey && mBase == base )
    return;
  pause();
  mScope = settingsKey;
  mBase = base;
  const auto state = QJsonDocument::fromJson( QSettings().value( mScope ).toByteArray() ).object();
  mPending = state.value( u"pending"_s ).toObject();
  mWatched = state.value( u"watched"_s ).toObject();
  mSnapshots = state.value( u"snapshots"_s ).toObject();
}
void QgsAiDiscoveryClient::save()
{
  if ( !mScope.isEmpty() )
    QSettings().setValue( mScope, QJsonDocument( QJsonObject { { u"pending"_s, mPending }, { u"watched"_s, mWatched }, { u"snapshots"_s, mSnapshots } } ).toJson( QJsonDocument::Compact ) );
}
QString QgsAiDiscoveryClient::submit( const QString &path, const QJsonObject &body )
{
  const QString key = QUuid::createUuid().toString( QUuid::WithoutBraces );
  mPending.insert( key, QJsonObject { { u"path"_s, path }, { u"body"_s, body } } );
  mSnapshots.insert( key, QJsonObject { { u"requestId"_s, key }, { u"status"_s, u"pending"_s } } );
  save();
  mTimer.start();
  request( key, path, body, true );
  return key;
}
void QgsAiDiscoveryClient::watch( const QString &kind, const QString &id )
{
  if ( ( kind != "plans"_L1 && kind != "runs"_L1 ) || !QRegularExpression( u"^[A-Za-z0-9_-]{1,100}$"_s ).match( id ).hasMatch() )
    return;
  mWatched.insert( id, kind );
  mFailures.remove( id );
  save();
  mTimer.start();
  poll();
}
bool QgsAiDiscoveryClient::discardUnsent( const QString &id )
{
  if ( !mPending.contains( id ) || mInFlight.contains( id ) || mPending.value( id ).toObject().value( u"attempts"_s ).toInt() > 0 )
    return false;
  mPending.remove( id );
  mSnapshots.insert( id, QJsonObject { { u"requestId"_s, id }, { u"status"_s, u"CANCELLED"_s } } ); // # spellok: API protocol status.
  save();
  return true;
}
QJsonObject QgsAiDiscoveryClient::snapshot( const QString &id ) const
{
  return mSnapshots.value( id ).toObject();
}
void QgsAiDiscoveryClient::resume()
{
  for ( auto it = mPending.begin(); it != mPending.end(); ++it )
  {
    auto item = it.value().toObject();
    item.remove( u"attempts"_s );
    item.remove( u"retryAt"_s );
    it.value() = item;
  }
  save();
  mTimer.start();
  poll();
}
void QgsAiDiscoveryClient::pause()
{
  mTimer.stop();
  ++mGeneration;
  mInFlight.clear();
}
void QgsAiDiscoveryClient::poll()
{
  const QJsonObject pending = mPending;
  for ( auto i = pending.begin(); i != pending.end(); ++i )
  {
    const auto item = i.value().toObject();
    if ( item.value( u"attempts"_s ).toInt() >= 3 || item.value( u"retryAt"_s ).toInteger() > QDateTime::currentMSecsSinceEpoch() )
      continue;
    request( i.key(), item.value( u"path"_s ).toString(), item.value( u"body"_s ).toObject(), true );
  }
  const QJsonObject watched = mWatched;
  for ( auto i = watched.begin(); i != watched.end(); ++i )
    request( i.key(), u"/v1/discovery/%1/%2"_s.arg( i.value().toString(), i.key() ), {}, false );
}
void QgsAiDiscoveryClient::request( const QString &key, const QString &path, const QJsonObject &body, bool post )
{
  if ( mInFlight.contains( key ) || mScope.isEmpty() )
    return;
  QUrl url = mBase;
  url.setPath( path );
  url.setQuery( QString() );
  url.setFragment( QString() );
  QNetworkRequest req( url );
  req.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy );
  req.setTransferTimeout( 30000 );
  req.setHeader( QNetworkRequest::ContentTypeHeader, u"application/json"_s );
  if ( !mAuth( req ) )
  {
    mTimer.stop();
    emit failed( key, tr( "Accesso Strata richiesto." ) );
    return;
  }
  if ( post )
    req.setRawHeader( "Idempotency-Key", key.toUtf8() );
  if ( post )
  {
    auto pending = mPending.value( key ).toObject();
    const int attempts = pending.value( u"attempts"_s ).toInt() + 1;
    pending.insert( u"attempts"_s, attempts );
    pending.insert( u"retryAt"_s, QDateTime::currentMSecsSinceEpoch() + ( 1000LL << attempts ) );
    mPending.insert( key, pending );
    save();
  }
  mInFlight.insert( key );
  QNetworkReply *reply = post ? mNetwork->post( req, QJsonDocument( body ).toJson( QJsonDocument::Compact ) ) : mNetwork->get( req );
  const int generation = mGeneration;
  auto bytes = std::make_shared<QByteArray>();
  connect( reply, &QIODevice::readyRead, this, [reply, bytes]() {
    bytes->append( reply->readAll() );
    if ( bytes->size() > 8 * 1024 * 1024 )
      reply->abort();
  } );
  QTimer::singleShot( 30000, reply, [reply]() {
    if ( reply->isRunning() )
      reply->abort();
  } );
  connect( reply, &QNetworkReply::finished, this, [this, reply, key, path, post, bytes, generation]() {
    reply->deleteLater();
    if ( generation != mGeneration )
      return;
    mInFlight.remove( key );
    bytes->append( reply->readAll() );
    const int status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
    QJsonObject result = QJsonDocument::fromJson( *bytes ).object();
    if ( reply->error() != QNetworkReply::NoError || status < 200 || status >= 300 || bytes->size() > 8 * 1024 * 1024 || result.isEmpty() )
    {
      // Authentication and permanent errors need user action; transient requests retain their original key and body.
      if ( path.endsWith( "/discovery-resolve"_L1 ) || ( status >= 400 && status < 500 && status != 429 ) )
      {
        mPending.remove( key );
        mWatched.remove( key );
        save();
      }
      if ( status == 401 || status == 403 )
        mTimer.stop();
      if ( !post && ++mFailures[key] >= 3 )
      {
        mWatched.remove( key );
        save();
      }
      emit failed( key, result.value( u"message"_s ).toString( reply->errorString() ) );
      return;
    }
    mPending.remove( key );
    mFailures.remove( key );
    const QString id = result.value( u"id"_s ).toString( key );
    mSnapshots.insert( key, result );
    mSnapshots.insert( id, result );
    QString kind = path.contains( "/plans"_L1 ) ? u"plans"_s : path.contains( "/runs"_L1 ) ? u"runs"_s : u"resolve"_s;
    const QString state = result.value( u"status"_s ).toString();
    if ( kind != "resolve"_L1 && ( state == "RUNNING"_L1 || state == "QUEUED"_L1 ) )
      mWatched.insert( id, kind );
    else
      mWatched.remove( id );
    save();
    result.insert( u"requestId"_s, key );
    emit updated( path.endsWith( "/reviews"_L1 ) ? u"review"_s : kind, result );
    if ( mPending.isEmpty() && mWatched.isEmpty() )
      mTimer.stop();
  } );
}
