/***************************************************************************
  qgsaitestbackgroundprobe.h
  --------------------------
  begin                : September 2026
***************************************************************************/

#ifndef QGSAITESTBACKGROUNDPROBE_H
#define QGSAITESTBACKGROUNDPROBE_H

#include <algorithm>
#include <functional>

#include "ai/tools/qgsaitaskrunner.h"

#include <QString>
#include <QThread>

/**
 * Listens on the AI background progress channel while alive.
 *
 * Background scans report progress through the runner's nested event loop, and that progress
 * reaches the GUI thread before the completion that ends the loop. So maxLoopLevel() >= 1 proves
 * the tool waited in a pumping event loop instead of blocking the GUI thread, and onFirstProgress
 * runs deterministically while the scan is still pending: use it to press Stop, edit or remove
 * the layer mid-run. Timers are no substitute, since a fast scan can finish before they fire.
 */
class QgsAiTestBackgroundProbe
{
  public:
    explicit QgsAiTestBackgroundProbe( std::function<void()> onFirstProgress = {} )
      : mOnFirstProgress( std::move( onFirstProgress ) )
    {
      qgsAiSetBackgroundToolProgressHandler( [this]( const QString &, double ) {
        mMaxLoopLevel = std::max( mMaxLoopLevel, QThread::currentThread()->loopLevel() );
        if ( mOnFirstProgress )
        {
          const std::function<void()> action = std::move( mOnFirstProgress );
          mOnFirstProgress = nullptr;
          action();
        }
      } );
    }

    ~QgsAiTestBackgroundProbe() { qgsAiSetBackgroundToolProgressHandler( {} ); }

    QgsAiTestBackgroundProbe( const QgsAiTestBackgroundProbe & ) = delete;
    QgsAiTestBackgroundProbe &operator=( const QgsAiTestBackgroundProbe & ) = delete;

    //! Deepest event loop level at which progress reached the GUI thread, or -1 if none did.
    int maxLoopLevel() const { return mMaxLoopLevel; }

  private:
    std::function<void()> mOnFirstProgress;
    int mMaxLoopLevel = -1;
};

#endif // QGSAITESTBACKGROUNDPROBE_H
