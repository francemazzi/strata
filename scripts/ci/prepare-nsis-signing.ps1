$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/windows-signing-files.ps1"
$source = Join-Path ${env:ProgramFiles(x86)} 'NSIS'
if (-not (Test-Path "$source/makensis.exe")) { throw 'NSIS installation was not found.' }
$target = Join-Path $env:RUNNER_TEMP 'strata-signed-nsis'
if (Test-Path $target) { throw "NSIS signing workspace already exists: $target" }
Copy-Item -LiteralPath $source -Destination $target -Recurse
# makensis loads plugin DLLs from NSISDIR at compile time. Sign the full toolchain
# (compiler, stubs, and plugins) so CI policy and Smart App Control see one publisher.
& "$PSScriptRoot/sign-windows-artifacts.ps1" -Path $target -Recurse -ReportPath "$env:RUNNER_TEMP/nsis-signatures.json"
Add-CiEnvironment -Name 'STRATA_NSIS_EXECUTABLE' -Value "$target\makensis.exe"
Add-CiEnvironment -Name 'NSISDIR' -Value $target
