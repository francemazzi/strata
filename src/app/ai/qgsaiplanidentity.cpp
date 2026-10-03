// SPDX-License-Identifier: GPL-2.0-or-later
#include "qgsaimodelrouter.h"
#include "qgsaiplanclient.h"
#include "qgsnetworkaccessmanager.h"
#include <QEventLoop>
#include <QNetworkReply>
#include <QTimer>
#include <QScopedValueRollback>
#include <QCryptographicHash>
#include <QDateTime>
#include <QNetworkRequest>
using namespace Qt::StringLiterals;

QString QgsAiModelRouter::planCredentialScope() const
{
  const auto settings = providerSettings( Provider::Plan );
  return QString::fromLatin1( QCryptographicHash::hash( ( settings.endpoint + '\n' + settings.authConfigId + '\n' + planSessionToken() ).toUtf8(), QCryptographicHash::Sha256 ).toHex() );
}

QString QgsAiModelRouter::cachedPlanAccountId() const
{
  return planCredentialScope() == mPlanIdentityScope && QDateTime::currentMSecsSinceEpoch() < mPlanIdentityExpires ? mPlanAccountId : QString();
}

QString QgsAiModelRouter::verifiedPlanAccountId( bool refresh )
{
  if ( mPlanIdentityFetching )
    return {};
  QScopedValueRollback<bool> fetching( mPlanIdentityFetching, true );
  const QString scope = planCredentialScope();
  if ( !refresh && scope == mPlanIdentityScope && QDateTime::currentMSecsSinceEpoch() < mPlanIdentityExpires )
    return mPlanAccountId;
  mPlanAccountId.clear();
  mPlanIdentityExpires = 0;
  mPlanIdentityScope = scope;
  if ( !isProviderUsable( Provider::Plan ) )
    return {};
  QNetworkRequest request( QUrl( QgsAiPlanClient::apiBaseForChatEndpoint( providerSettings( Provider::Plan ).endpoint ) + u"/v1/auth/me"_s ) );
  request.setTransferTimeout( 10000 );
  request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy );
  if ( !applyAuthentication( Provider::Plan, request ) )
    return {};
  QNetworkReply *reply = QgsNetworkAccessManager::instance()->get( request );
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot( true );
  QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
  QObject::connect( &deadline, &QTimer::timeout, &loop, &QEventLoop::quit );
  deadline.start( 10000 );
  if ( !reply->isFinished() )
    loop.exec( QEventLoop::ExcludeUserInputEvents );
  const bool valid = reply->isFinished() && reply->error() == QNetworkReply::NoError && reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt() == 200 && scope == planCredentialScope();
  if ( valid )
    mPlanAccountId = QgsAiPlanClient::parseMeJson( reply->readAll() ).id;
  if ( !reply->isFinished() )
    reply->abort();
  reply->deleteLater();
  mPlanIdentityExpires = mPlanAccountId.isEmpty() ? 0 : QDateTime::currentMSecsSinceEpoch() + 60000;
  return mPlanAccountId;
}
