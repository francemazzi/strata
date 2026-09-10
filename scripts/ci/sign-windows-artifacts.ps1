param(
  [ValidateSet('staging', 'package', 'verify')][string]$Mode = 'staging',
  [string]$Path,
  [string[]]$PackageFiles,
  [string]$PackageManifest,
  [switch]$Recurse,
  [switch]$VerifyOnly,
  [string]$ReportPath,
  [string]$SignToolPath = $env:STRATA_SIGNTOOL_PATH,
  [string]$DlibPath = $env:STRATA_AZURE_CODESIGN_DLIB_PATH,
  [string]$MetadataPath = $env:STRATA_AZURE_CODESIGN_METADATA_PATH
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/windows-signing-files.ps1"
$script:Records = [System.Collections.Generic.List[object]]::new()
$script:SignTool = Resolve-RequiredFile $SignToolPath 'SignTool'
$onlyVerify = $VerifyOnly -or $Mode -eq 'verify'
if (-not $onlyVerify) {
  $script:Dlib = Resolve-RequiredFile $DlibPath 'Azure signing library'
  $script:Metadata = Resolve-RequiredFile $MetadataPath 'Azure signing metadata'
}
$result = 'failed'
$failure = $null
try {
  if ($Mode -eq 'package') {
    $files = @($PackageFiles | Where-Object { $_ })
    if ($PackageManifest) { $files += Get-Content -LiteralPath $PackageManifest | Where-Object { $_.Trim() } }
    if ($files.Count -eq 0) { throw 'Package mode requires package files.' }
    foreach ($file in $files) {
      $fullName = Resolve-RequiredFile $file 'Package'
      switch ([System.IO.Path]::GetExtension($fullName).ToLowerInvariant()) {
        '.zip' { Invoke-ZipVerification $fullName }
        '.exe' { Invoke-SignablePath $fullName -OnlyVerify:$onlyVerify }
        default { throw "Unsupported package: $file" }
      }
    }
  } else {
    if (-not $Path) { throw "Mode $Mode requires -Path." }
    Invoke-SignablePath $Path -Recursive:$Recurse -OnlyVerify:$onlyVerify
  }
  if ($script:Records.Count -eq 0) { throw 'No Windows PE files were found.' }
  $result = 'passed'
} catch {
  $failure = $_.Exception.Message
  throw
} finally {
  if ($ReportPath) {
    $report = [ordered]@{ result = $result; error = $failure; files = @($script:Records.ToArray()) }
    $parent = Split-Path ([System.IO.Path]::GetFullPath($ReportPath)) -Parent
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding utf8
  }
}
Write-Host "Windows signature verification passed: $($script:Records.Count) PE files."
