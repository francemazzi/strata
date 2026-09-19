@echo off
REM NSIS !uninstfinalize invokes this via system(). Keep the NSI command to
REM `"<this-script>" "%1"`; Bundle.cmake escapes the CPack serialization layer.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign-nsis-uninstaller.ps1" -Path "%~1"
exit /b %ERRORLEVEL%
