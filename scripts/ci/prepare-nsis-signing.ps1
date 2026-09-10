$ErrorActionPreference = 'Stop'
$source = Join-Path ${env:ProgramFiles(x86)} 'NSIS'
if (-not (Test-Path "$source/makensis.exe")) { throw 'NSIS installation was not found.' }
$target = Join-Path $env:RUNNER_TEMP 'strata-signed-nsis'
if (Test-Path $target) { throw "NSIS signing workspace already exists: $target" }
Copy-Item -LiteralPath $source -Destination $target -Recurse
# The copied compiler loads its own Plugins directory. Sign those DLLs before
# makensis embeds them; signing only the final installer does not cover them.
& "$PSScriptRoot/sign-windows-artifacts.ps1" -Path "$target/Plugins" -Recurse -ReportPath "$env:RUNNER_TEMP/nsis-signatures.json"
"STRATA_NSIS_EXECUTABLE=$target\makensis.exe" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
"NSISDIR=$target" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
