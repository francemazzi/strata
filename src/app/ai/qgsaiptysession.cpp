/***************************************************************************
    qgsaiptysession.cpp
    ---------------------
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

#include "qgsaiptysession.h"

#include <QFile>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <vector>

#if defined( Q_OS_UNIX )
// Plain POSIX pty API (posix_openpt & co.) instead of forkpty(): it needs no
// libutil and no <util.h>, which src/core/pal/util.h would shadow anyway.
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "moc_qgsaiptysession.cpp"

using namespace Qt::StringLiterals;

namespace
{
  // After the child closes its side of the terminal, wait this many exit polls
  // (250 ms each) before assuming it hung and killing it.
  constexpr int SLAVE_CLOSED_GRACE_POLLS = 12;
  constexpr int EXIT_POLL_INTERVAL_MS = 250;
} // namespace

QgsAiPtySession::QgsAiPtySession( QObject *parent )
  : QObject( parent )
{
}

QgsAiPtySession::~QgsAiPtySession()
{
#if defined( Q_OS_UNIX )
  if ( isRunning() )
  {
    const pid_t pid = static_cast<pid_t>( mPid );
    if ( ::kill( -pid, SIGKILL ) != 0 )
      ::kill( pid, SIGKILL );
    int status = 0;
    ::waitpid( pid, &status, 0 );
    mPid = -1;
  }
#endif
  closeMaster();
}

bool QgsAiPtySession::isSupported()
{
#if defined( Q_OS_UNIX )
  return true;
#else
  return false;
#endif
}

bool QgsAiPtySession::isRunning() const
{
  return mPid > 0;
}

bool QgsAiPtySession::start( const QString &program, const QStringList &arguments, const QProcessEnvironment &environment, int columns, int rows, QString *errorMessage )
{
#if defined( Q_OS_UNIX )
  if ( isRunning() )
  {
    if ( errorMessage )
      *errorMessage = tr( "A terminal session is already running." );
    return false;
  }
  if ( program.trimmed().isEmpty() || !QFile::exists( program ) )
  {
    if ( errorMessage )
      *errorMessage = tr( "Program not found: %1" ).arg( program );
    return false;
  }

  // Everything the child needs is prepared BEFORE forking: after forkpty() the
  // child may only call async-signal-safe functions (execve, _exit).
  const QByteArray programBytes = QFile::encodeName( program );
  QList<QByteArray> argvStorage;
  argvStorage.append( programBytes );
  for ( const QString &argument : arguments )
    argvStorage.append( argument.toLocal8Bit() );
  std::vector<char *> argv;
  argv.reserve( argvStorage.size() + 1 );
  for ( QByteArray &entry : argvStorage )
    argv.push_back( entry.data() );
  argv.push_back( nullptr );

  QList<QByteArray> envStorage;
  const QStringList keys = environment.keys();
  for ( const QString &key : keys )
    envStorage.append( ( key + u'=' + environment.value( key ) ).toLocal8Bit() );
  std::vector<char *> envp;
  envp.reserve( envStorage.size() + 1 );
  for ( QByteArray &entry : envStorage )
    envp.push_back( entry.data() );
  envp.push_back( nullptr );

  struct winsize windowSize;
  std::memset( &windowSize, 0, sizeof( windowSize ) );
  windowSize.ws_col = static_cast<unsigned short>( std::max( 20, columns ) );
  windowSize.ws_row = static_cast<unsigned short>( std::max( 5, rows ) );

  const int masterFd = ::posix_openpt( O_RDWR | O_NOCTTY );
  if ( masterFd < 0 || ::grantpt( masterFd ) != 0 || ::unlockpt( masterFd ) != 0 )
  {
    if ( errorMessage )
      *errorMessage = tr( "Unable to allocate a pseudo-terminal: %1" ).arg( QString::fromLocal8Bit( std::strerror( errno ) ) );
    if ( masterFd >= 0 )
      ::close( masterFd );
    return false;
  }
  // The master must not leak into the child (or any other spawned process).
  ::fcntl( masterFd, F_SETFD, FD_CLOEXEC );

  char slavePath[PATH_MAX];
  std::memset( slavePath, 0, sizeof( slavePath ) );
  const char *slaveName = ::ptsname( masterFd );
  if ( !slaveName || std::strlen( slaveName ) >= sizeof( slavePath ) )
  {
    if ( errorMessage )
      *errorMessage = tr( "Unable to resolve the pseudo-terminal slave device." );
    ::close( masterFd );
    return false;
  }
  std::strncpy( slavePath, slaveName, sizeof( slavePath ) - 1 );

  const pid_t pid = ::fork();
  if ( pid < 0 )
  {
    if ( errorMessage )
      *errorMessage = tr( "Unable to start the program: %1" ).arg( QString::fromLocal8Bit( std::strerror( errno ) ) );
    ::close( masterFd );
    return false;
  }
  if ( pid == 0 )
  {
    // Child: only async-signal-safe calls from here on. New session, the pty
    // slave becomes the controlling terminal and stdin/stdout/stderr.
    ::setsid();
    const int slaveFd = ::open( slavePath, O_RDWR );
    if ( slaveFd < 0 )
      ::_exit( 126 );
#if defined( TIOCSCTTY )
    ::ioctl( slaveFd, TIOCSCTTY, 0 );
#endif
    ::ioctl( slaveFd, TIOCSWINSZ, &windowSize );
    ::dup2( slaveFd, STDIN_FILENO );
    ::dup2( slaveFd, STDOUT_FILENO );
    ::dup2( slaveFd, STDERR_FILENO );
    if ( slaveFd > STDERR_FILENO )
      ::close( slaveFd );
    ::close( masterFd );
    ::execve( programBytes.constData(), argv.data(), envp.data() );
    ::_exit( 127 );
  }

  mPid = pid;
  mMasterFd = masterFd;
  mSlaveClosed = false;
  mSlaveClosedPolls = 0;
  mFinishedEmitted = false;

  const int flags = ::fcntl( mMasterFd, F_GETFL, 0 );
  if ( flags >= 0 )
    ::fcntl( mMasterFd, F_SETFL, flags | O_NONBLOCK );

  mNotifier = new QSocketNotifier( mMasterFd, QSocketNotifier::Read, this );
  connect( mNotifier, &QSocketNotifier::activated, this, &QgsAiPtySession::onReadyRead );

  mExitPollTimer = new QTimer( this );
  mExitPollTimer->setInterval( EXIT_POLL_INTERVAL_MS );
  connect( mExitPollTimer, &QTimer::timeout, this, &QgsAiPtySession::pollChildExit );
  mExitPollTimer->start();
  return true;
#else
  Q_UNUSED( program )
  Q_UNUSED( arguments )
  Q_UNUSED( environment )
  Q_UNUSED( columns )
  Q_UNUSED( rows )
  if ( errorMessage )
    *errorMessage = tr( "Pseudo-terminal sessions are not supported on this platform." );
  return false;
#endif
}

qint64 QgsAiPtySession::write( const QByteArray &data )
{
#if defined( Q_OS_UNIX )
  if ( mMasterFd < 0 || data.isEmpty() )
    return 0;
  qint64 written = 0;
  int retries = 0;
  while ( written < data.size() )
  {
    const ssize_t count = ::write( mMasterFd, data.constData() + written, static_cast<size_t>( data.size() - written ) );
    if ( count > 0 )
    {
      written += count;
      continue;
    }
    if ( count < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) && retries++ < 50 )
    {
      ::usleep( 2000 );
      continue;
    }
    break;
  }
  return written;
#else
  Q_UNUSED( data )
  return 0;
#endif
}

void QgsAiPtySession::sendInterrupt()
{
  write( QByteArray( "\x03", 1 ) );
}

void QgsAiPtySession::terminate()
{
#if defined( Q_OS_UNIX )
  if ( !isRunning() )
    return;
  sendInterrupt();
  QTimer::singleShot( 500, this, [this]() { signalChild( SIGTERM ); } );
  QTimer::singleShot( 2500, this, [this]() { signalChild( SIGKILL ); } );
#endif
}

void QgsAiPtySession::kill()
{
#if defined( Q_OS_UNIX )
  signalChild( SIGKILL );
#endif
}

void QgsAiPtySession::signalChild( int signal )
{
#if defined( Q_OS_UNIX )
  if ( !isRunning() )
    return;
  // forkpty() made the child a session leader, so its pid is also its process
  // group: signal the whole group to take helpers (browser openers, sleeps) along.
  const pid_t pid = static_cast<pid_t>( mPid );
  if ( ::kill( -pid, signal ) != 0 )
    ::kill( pid, signal );
#else
  Q_UNUSED( signal )
#endif
}

void QgsAiPtySession::onReadyRead()
{
  drainOutput();
}

void QgsAiPtySession::drainOutput()
{
#if defined( Q_OS_UNIX )
  if ( mMasterFd < 0 )
    return;
  char buffer[4096];
  for ( ;; )
  {
    const ssize_t count = ::read( mMasterFd, buffer, sizeof( buffer ) );
    if ( count > 0 )
    {
      emit outputReceived( QByteArray( buffer, static_cast<int>( count ) ) );
      continue;
    }
    if ( count < 0 )
    {
      if ( errno == EINTR )
        continue;
      if ( errno == EAGAIN || errno == EWOULDBLOCK )
        return;
      // Linux reports EIO once the slave side is closed (child exited or closed its tty).
    }
    // EOF (or EIO): the child released the terminal. Stop the notifier so a
    // closed descriptor cannot spin the event loop, then let the exit poll reap it.
    if ( mNotifier )
      mNotifier->setEnabled( false );
    mSlaveClosed = true;
    pollChildExit();
    return;
  }
#endif
}

void QgsAiPtySession::pollChildExit()
{
#if defined( Q_OS_UNIX )
  if ( !isRunning() )
    return;
  int status = 0;
  const pid_t result = ::waitpid( static_cast<pid_t>( mPid ), &status, WNOHANG );
  if ( result == static_cast<pid_t>( mPid ) )
  {
    handleExit( status );
    return;
  }
  if ( result < 0 && errno != EINTR )
  {
    // The child is gone but could not be reaped (ECHILD): report a failure exit.
    handleExit( -1 );
    return;
  }
  if ( mSlaveClosed && ++mSlaveClosedPolls > SLAVE_CLOSED_GRACE_POLLS )
    signalChild( SIGKILL );
#endif
}

void QgsAiPtySession::handleExit( int status )
{
#if defined( Q_OS_UNIX )
  if ( mExitPollTimer )
    mExitPollTimer->stop();
  if ( mNotifier )
    mNotifier->setEnabled( false );
  // Flush whatever the child printed right before exiting.
  if ( mMasterFd >= 0 )
  {
    char buffer[4096];
    for ( ;; )
    {
      const ssize_t count = ::read( mMasterFd, buffer, sizeof( buffer ) );
      if ( count > 0 )
      {
        emit outputReceived( QByteArray( buffer, static_cast<int>( count ) ) );
        continue;
      }
      if ( count < 0 && errno == EINTR )
        continue;
      break;
    }
  }

  bool crashed = false;
  int exitCode = -1;
  if ( status >= 0 )
  {
    if ( WIFEXITED( status ) )
      exitCode = WEXITSTATUS( status );
    else if ( WIFSIGNALED( status ) )
    {
      crashed = true;
      exitCode = 128 + WTERMSIG( status );
    }
  }
  mPid = -1;
  closeMaster();
  if ( !mFinishedEmitted )
  {
    mFinishedEmitted = true;
    emit finished( exitCode, crashed );
  }
#else
  Q_UNUSED( status )
#endif
}

void QgsAiPtySession::closeMaster()
{
  if ( mNotifier )
  {
    mNotifier->setEnabled( false );
    mNotifier->deleteLater();
    mNotifier = nullptr;
  }
  if ( mExitPollTimer )
  {
    mExitPollTimer->stop();
    mExitPollTimer->deleteLater();
    mExitPollTimer = nullptr;
  }
#if defined( Q_OS_UNIX )
  if ( mMasterFd >= 0 )
  {
    ::close( mMasterFd );
    mMasterFd = -1;
  }
#endif
}
