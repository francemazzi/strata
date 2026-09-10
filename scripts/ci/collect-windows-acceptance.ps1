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
$packages = @($Installer, $Zip | ForEach-Object {
  $file = Get-Item -LiteralPath $_
  $hash = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()
  $match = @($manifest.artifacts | Where-Object { $_.name -eq $file.Name -and $_.sha256 -eq $hash })
  if ($match.Count -ne 1) { throw "File differs from the sealed candidate: $($file.Name)" }
  @{ name = $file.Name; sha256 = $hash }
})
try {
  $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3077; StartTime = $TestStartedAt })
} catch {
  if ($_.FullyQualifiedErrorId -notmatch 'NoMatchingEventsFound') { throw }
  $events = @()
}
$paths = [regex]::Escape($InstalledRoot) + '|' + [regex]::Escape($PortableRoot) + '|' + [regex]::Escape((Split-Path $Installer -Leaf))
$blocks = @($events | Where-Object { $_.Message -match $paths })
if ($blocks.Count) { $blocks | Format-List TimeCreated, Message; throw 'Candidate binaries were blocked by Code Integrity.' }
$scenarios = [ordered]@{}
foreach ($name in @('install', 'launch', 'restart', 'portable', 'uninstall', 'upgrade-preserves-data', 'project', 'raster-vector', 'processing', 'pyqgis', 'opencl')) {
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
  packages = $packages; scenarios = $scenarios; colleagueConfirmed = $true
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding utf8
Write-Host "Acceptance evidence saved to $ReportPath. No Windows security settings were changed."
