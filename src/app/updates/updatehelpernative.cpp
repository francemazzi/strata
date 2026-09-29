#include "updatehelper.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#elif defined( Q_OS_MACOS )
#include <Security/Security.h>
#include <signal.h>
#else
#include <signal.h>
#endif
using namespace Qt::StringLiterals;
bool UpdateHelper::parentAlive( qint64 pid )
{
#ifdef Q_OS_WIN
  HANDLE process = OpenProcess( SYNCHRONIZE, FALSE, static_cast<DWORD>( pid ) );
  if ( !process )
    return false;
  const bool alive = WaitForSingleObject( process, 0 ) == WAIT_TIMEOUT;
  CloseHandle( process );
  return alive;
#else
  return kill( static_cast<pid_t>( pid ), 0 ) == 0;
#endif
}
bool UpdateHelper::verifyNative( const QString &candidate, const QString &installed, QString *error )
{
#ifdef Q_OS_WIN
  Q_UNUSED( installed )
  WINTRUST_FILE_INFO file = {};
  file.cbStruct = sizeof( file );
  file.pcwszFilePath = reinterpret_cast<LPCWSTR>( candidate.utf16() );
  WINTRUST_DATA data = {};
  data.cbStruct = sizeof( data );
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status = WinVerifyTrust( nullptr, &action, &data );
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust( nullptr, &action, &data );
  if ( status != ERROR_SUCCESS )
  {
    *error = u"Windows could not verify the update publisher."_s;
    return false;
  }
#elif defined( Q_OS_MACOS )
  const auto code = []( const QString &path, SecStaticCodeRef *result ) {
    const QByteArray bytes = path.toUtf8();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation( nullptr, reinterpret_cast<const UInt8 *>( bytes.constData() ), bytes.size(), true );
    const OSStatus status = SecStaticCodeCreateWithPath( url, kSecCSDefaultFlags, result );
    CFRelease( url );
    return status;
  };
  SecStaticCodeRef oldCode = nullptr, newCode = nullptr;
  SecRequirementRef requirement = nullptr;
  bool ok = code( installed, &oldCode ) == errSecSuccess && code( candidate, &newCode ) == errSecSuccess;
  if ( ok )
    ok = SecCodeCopyDesignatedRequirement( oldCode, kSecCSDefaultFlags, &requirement ) == errSecSuccess;
  if ( ok )
    ok = SecStaticCodeCheckValidity( newCode, kSecCSCheckAllArchitectures | kSecCSStrictValidate | kSecCSCheckNestedCode, requirement ) == errSecSuccess;
  if ( requirement )
    CFRelease( requirement );
  if ( oldCode )
    CFRelease( oldCode );
  if ( newCode )
    CFRelease( newCode );
  if ( !ok )
  {
    *error = u"The update does not match the installed application's signing identity."_s;
    return false;
  }
#else
  Q_UNUSED( candidate )
  Q_UNUSED( installed )
  Q_UNUSED( error )
#endif
  return true;
}
