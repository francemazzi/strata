@echo off
REM NSIS !uninstfinalize invokes this via system(). Keep the NSI command to
REM `<this-script> "%1"` so CMake/CPack never nest quotes around powershell.exe.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign-windows-artifacts.ps1" -Mode staging -Path "%~1"
exit /b %ERRORLEVEL%
