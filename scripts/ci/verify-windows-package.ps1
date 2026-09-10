param([Parameter(Mandatory)][string]$BuildDirectory, [Parameter(Mandatory)][string]$ReportPath)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/windows-signing-files.ps1"
. "$PSScriptRoot/windows-process.ps1"
$packages = @(Get-ChildItem $BuildDirectory -File | Where-Object { $_.Name -match '-win64\.(exe|zip)$' })
if ($packages.Count -ne 2) { throw 'Exactly one Windows installer and one ZIP are required.' }
$installer = @($packages | Where-Object Extension -eq '.exe')
$zip = @($packages | Where-Object Extension -eq '.zip')
if ($installer.Count -ne 1 -or $zip.Count -ne 1) { throw 'Installer/ZIP pair missing.' }
$workspace = Join-Path $env:RUNNER_TEMP ('strata-package-' + [guid]::NewGuid().ToString('N'))
$installed = Join-Path $workspace 'installed'
$expanded = Join-Path $workspace 'portable'
New-Item -ItemType Directory -Path $workspace | Out-Null


try {
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode package -PackageFiles $packages.FullName -VerifyOnly -ReportPath "$workspace/packages.json"
  Expand-Archive $zip[0].FullName $expanded
  $roots = @(Get-ChildItem $expanded -Recurse -Filter OpenCL.dll | Where-Object { $_.Directory.Name -eq 'bin' })
  if ($roots.Count -ne 1) { throw 'ZIP must contain exactly one bin/OpenCL.dll.' }
  $portable = $roots[0].Directory.Parent.FullName
  if ((Get-PeInfo $roots[0].FullName).Machine -ne 0x8664) { throw 'OpenCL.dll must be x64.' }
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode verify -Path $portable -Recurse -ReportPath "$workspace/portable.json"
  & "$PSScriptRoot/test-windows-runtime.ps1" -Root $portable
  Invoke-WindowsProcess $installer[0].FullName "/S /D=$installed"
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode verify -Path $installed -Recurse -ReportPath "$workspace/installed.json"
  $portableReport = Get-Content "$workspace/portable.json" -Raw | ConvertFrom-Json
  $installedReport = Get-Content "$workspace/installed.json" -Raw | ConvertFrom-Json
  foreach ($file in $portableReport.files) {
    $match = @($installedReport.files | Where-Object { $_.path -eq $file.path -and $_.sha256 -eq $file.sha256 })
    if ($match.Count -ne 1) { throw "Installer payload differs from ZIP: $($file.path)" }
  }
  if (-not (Test-Path "$installed/Uninstall.exe")) { throw 'Signed uninstaller is missing.' }
  & "$PSScriptRoot/test-windows-runtime.ps1" -Root $installed
  Invoke-WindowsProcess "$installed/Uninstall.exe" "/S _?=$installed"
  $report = [ordered]@{
    result = 'passed'; sourceSha = (& git rev-parse HEAD).Trim()
    smartAppControlTested = $false
    packages = @($packages | ForEach-Object { @{ name = $_.Name; sha256 = (Get-FileHash $_.FullName).Hash.ToLowerInvariant() } })
    portable = $portableReport.files; installed = $installedReport.files
    scenarios = @('signatures', 'payload-match', 'opencl-x64', 'portable-runtime', 'installed-runtime', 'uninstall')
  }
  $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding utf8
} finally {
  # Keep failed package evidence in the runner workspace; CI uploads it on failure.
  if (Test-Path $ReportPath) { Remove-Item -LiteralPath $workspace -Recurse -Force }
}
