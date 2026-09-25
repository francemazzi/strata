/***************************************************************************
    qgsaiindexingactivity.cpp
    -------------------------
    begin                : September 2026
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

#include "qgsaiindexingactivity.h"

#include <cmath>

#include "qgsaiindexingscheduler.h"
#include "qgsaiindexingthrottle.h"
#include "qgsailayerindexcoordinator.h"
#include "qgsaiworkspaceindex.h"
#include "qgssettings.h"

#include <QString>
#include <QStringList>

#include "moc_qgsaiindexingactivity.cpp"

using namespace Qt::StringLiterals;

QgsAiIndexingActivity::QgsAiIndexingActivity( QgsAiWorkspaceIndex *index, QgsAiIndexingScheduler *scheduler, QgsAiLayerIndexCoordinator *coordinator, QObject *parent )
  : QObject( parent )
  , mIndex( index )
  , mScheduler( scheduler )
  , mCoordinator( coordinator )
{
  if ( mScheduler )
  {
    connect( mScheduler, &QgsAiIndexingScheduler::passStarted, this, [this]() {
      mFilePercent = 0;
      mLastError.clear();
      emit changed();
    } );
    connect( mScheduler, &QgsAiIndexingScheduler::passProgress, this, [this]( double percent ) {
      // The label shows whole percents: skip the updates that do not change it.
      if ( std::floor( percent ) == std::floor( mFilePercent ) )
        return;
      mFilePercent = percent;
      emit changed();
    } );
    connect( mScheduler, &QgsAiIndexingScheduler::passFinished, this, [this]( bool, const QString &error ) {
      mFilePercent = -1;
      mLastError = error;
      emit changed();
    } );
  }
  if ( mCoordinator )
  {
    connect( mCoordinator, &QgsAiLayerIndexCoordinator::queueChanged, this, &QgsAiIndexingActivity::onLayerQueueChanged );
    connect( mCoordinator, &QgsAiLayerIndexCoordinator::reindexFinished, this, [this]( const QString &, bool success, const QString &error ) {
      ++mLayersDone;
      if ( success )
        mLastError.clear();
      else if ( !mPaused && !error.contains( "cancel"_L1, Qt::CaseInsensitive ) )
        mLastError = error;
      emit changed();
    } );
  }
  // Power and the provider can change without a signal: look again from time to time.
  connect( &mRefreshTimer, &QTimer::timeout, this, &QgsAiIndexingActivity::refresh );
  mRefreshTimer.start( 10000 );
  refresh();
}

void QgsAiIndexingActivity::onLayerQueueChanged()
{
  // A new wave of layers starts counting from zero.
  if ( mCoordinator && mCoordinator->pendingLayerCount() == 0 && !mCoordinator->isRunning() )
    mLayersDone = 0;
  emit changed();
}

void QgsAiIndexingActivity::setPaused( bool paused )
{
  if ( mPaused == paused )
    return;
  mPaused = paused;
  if ( mScheduler )
    mScheduler->setPaused( paused );
  if ( mCoordinator )
    mCoordinator->setPaused( paused );
  emit changed();
}

void QgsAiIndexingActivity::refresh()
{
  const bool indexingWanted = ( mScheduler && mScheduler->automaticEnabled() ) || ( mCoordinator && mCoordinator->isEnabled() );
  const QString problem = indexingWanted && mIndex && !mIndex->embeddingProviderAvailable() ? mIndex->unavailableReason() : QString();
  if ( problem != mAvailabilityProblem )
    mAvailabilityProblem = problem;
  emit changed();
}

QgsAiIndexingActivity::State QgsAiIndexingActivity::state() const
{
  State s;
  s.paused = mPaused;
  const bool filesRunning = mScheduler && mScheduler->isRunning();
  const bool layersRunning = mCoordinator && mCoordinator->isRunning();
  s.filePercent = filesRunning ? std::max( 0.0, mFilePercent ) : -1;
  const int pending = mCoordinator ? mCoordinator->pendingLayerCount() : 0;
  if ( pending > 0 || layersRunning )
  {
    s.layersDone = mLayersDone;
    s.layersTotal = mLayersDone + pending;
  }
  s.currentLayer = mCoordinator ? mCoordinator->runningLayerName() : QString();
  s.active = !mPaused && ( filesRunning || layersRunning );
  s.waitingForPower = s.active && QgsSettings().value( QgsAiIndexingThrottle::pauseOnBatterySettingsKey(), true ).toBool() && QgsAiIndexingThrottle::onBatteryPower();
  s.problem = !mAvailabilityProblem.isEmpty() ? mAvailabilityProblem : mLastError;
  return s;
}

QString QgsAiIndexingActivity::summaryText( const State &state )
{
  if ( state.paused )
  {
    const int waiting = state.layersTotal - state.layersDone;
    return waiting > 0 ? tr( "Indexing paused · %n layer(s) waiting", nullptr, waiting ) : tr( "Indexing paused" );
  }
  if ( state.waitingForPower )
    return tr( "Indexing waits for mains power" );
  if ( state.active )
  {
    QStringList parts { tr( "Indexing" ) };
    if ( state.filePercent >= 0 )
      parts << tr( "files %1%" ).arg( static_cast<int>( state.filePercent ) );
    if ( state.layersTotal > 0 )
      parts << tr( "layer %1 of %2" ).arg( std::min( state.layersDone + 1, state.layersTotal ) ).arg( state.layersTotal );
    return parts.join( u" · "_s );
  }
  if ( !state.problem.isEmpty() )
    return tr( "Index unavailable" );
  return QString();
}

QString QgsAiIndexingActivity::detailText( const State &state )
{
  QStringList lines;
  if ( state.active || state.paused )
  {
    lines << tr( "Strata indexes the workspace files and the project layers on this computer, so the assistant can find them." );
    if ( state.filePercent >= 0 )
      lines << tr( "Workspace files: %1%" ).arg( static_cast<int>( state.filePercent ) );
    if ( state.layersTotal > 0 )
      lines << tr( "Layers: %1 of %2 done" ).arg( state.layersDone ).arg( state.layersTotal );
    if ( !state.currentLayer.isEmpty() )
      lines << tr( "Now: %1" ).arg( state.currentLayer );
  }
  if ( state.waitingForPower )
    lines << tr( "The computer runs on battery: indexing resumes on mains power (see the AI settings)." );
  if ( !state.problem.isEmpty() )
    lines << state.problem;
  lines << tr( "Click to open the indexing settings." );
  return lines.join( '\n' );
}
