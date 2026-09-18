$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/windows-signing-files.ps1"
$source = Join-Path ${env:ProgramFiles(x86)} 'NSIS'
if (-not (Test-Path "$source/makensis.exe")) { throw 'NSIS installation was not found.' }
$target = Join-Path $env:RUNNER_TEMP 'strata-signed-nsis'
if (Test-Path $target) { throw "NSIS signing workspace already exists: $target" }
Copy-Item -LiteralPath $source -Destination $target -Recurse
# makensis loads plugin DLLs from NSISDIR at compile time. Sign plugins and the
# compiler binaries; skip NSIS helper PE files with non-standard extensions (.bin).
foreach ($compilerPath in @("$target/makensis.exe", "$target/Bin/makensis.exe")) {
  if (Test-Path -LiteralPath $compilerPath) {
    & "$PSScriptRoot/sign-windows-artifacts.ps1" -Path $compilerPath
  }
}
if (Test-Path -LiteralPath "$target/Bin") {
  Get-ChildItem -LiteralPath "$target/Bin" -File -Filter '*.exe' | ForEach-Object {
    & "$PSScriptRoot/sign-windows-artifacts.ps1" -Path $_.FullName
  }
}
& "$PSScriptRoot/sign-windows-artifacts.ps1" -Path "$target/Plugins" -Recurse -ReportPath "$env:RUNNER_TEMP/nsis-signatures.json"
Add-CiEnvironment -Name 'STRATA_NSIS_EXECUTABLE' -Value "$target\makensis.exe"
Add-CiEnvironment -Name 'NSISDIR' -Value $target
