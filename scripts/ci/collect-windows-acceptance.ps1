param(
  [Parameter(Mandatory)][string]$ManifestPath,
  [Parameter(Mandatory)][string]$Installer,
  [Parameter(Mandatory)][string]$Zip,
  [Parameter(Mandatory)][string]$InstalledRoot,
  [Parameter(Mandatory)][string]$PortableRoot,
  [Parameter(Mandatory)][datetime]$TestStartedAt,
  [string]$ReportPath = 'windows-acceptance.json'
)
$ErrorActionPreference = 'Stop'
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
$sac = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState
if ($build -lt 22000 -or $sac -ne 1) { throw 'Acceptance requires Windows 11 and Smart App Control On.' }
$packages = @(@($Installer, $Zip) | ForEach-Object {
  $file = Get-Item -LiteralPath $_
  $hash = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()
  $match = @($manifest.artifacts | Where-Object { $_.name -eq $file.Name -and $_.sha256 -eq $hash })
  if ($match.Count -ne 1) { throw "File differs from the sealed candidate: $($file.Name)" }
  @{ name = $file.Name; sha256 = $hash }
})
if ($TestStartedAt -ge [datetime]::Now) { throw 'TestStartedAt must precede the completed test session.' }
if (-not (Get-WinEvent -ListLog 'Microsoft-Windows-CodeIntegrity/Operational').IsEnabled) { throw 'Code Integrity event logging is disabled.' }
try {
  $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3077; StartTime = $TestStartedAt })
} catch {
  if ($_.FullyQualifiedErrorId -notmatch 'NoMatchingEventsFound') { throw }
  $events = @()
}
# NSIS may load a plugin or the uninstaller from a temporary path, and event
# paths can use device names. A clean acceptance session must have no 3077
# events; filtering only the installation paths would miss these blocks.
if ($events.Count) { $events | Format-List TimeCreated, Message; throw 'Code Integrity enforcement blocks occurred during this test session. Investigate and repeat on a clean test machine.' }
$scenarios = [ordered]@{}
foreach ($name in @('install', 'launch', 'restart', 'portable', 'uninstall', 'upgrade-preserves-data', 'project', 'raster-vector', 'processing', 'pyqgis', 'opencl', 'opencl-gpu', 'opencl-no-platform')) {
  if ((Read-Host "Scenario '$name': completed successfully on this exact candidate? Enter yes") -ne 'yes') {
    throw "Acceptance incomplete: $name"
  }
  $scenarios[$name] = 'passed'
}
$tester = Read-Host 'Tester name'
if ([string]::IsNullOrWhiteSpace($tester)) { throw 'Tester name is required.' }
if ((Read-Host 'Has the affected colleague confirmed the issue is resolved with these package hashes? Enter yes') -ne 'yes') {
  throw 'Affected colleague confirmation is still pending.'
}
[ordered]@{
  tag = $manifest.tag; sourceSha = $manifest.sourceSha
  tester = $tester; testedAt = [datetime]::UtcNow.ToString('o'); testStartedAt = $TestStartedAt.ToUniversalTime().ToString('o')
  windowsBuild = $build; smartAppControl = 'On'; strataCodeIntegrityBlocks = 0
  installedRoot = $InstalledRoot; portableRoot = $PortableRoot; codeIntegrityEnforcementEvents = 0
  packages = $packages; scenarios = $scenarios; colleagueConfirmed = $true
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding utf8
Write-Host "Acceptance evidence saved to $ReportPath. No Windows security settings were changed."
