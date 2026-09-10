$ErrorActionPreference = 'Stop'
$root = Join-Path $env:RUNNER_TEMP ('strata-authenticode-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root | Out-Null
$tool = Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/signtool.exe" | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $tool) { throw 'Windows SDK SignTool not installed.' }
$env:STRATA_SIGNTOOL_PATH = $tool.FullName
$certificate = $null
function Assert-Fails {
  param([scriptblock]$Action, [string]$Expected)
  try { & $Action } catch {
    if ($_.Exception.Message -like "*$Expected*") { return }
    throw
  }
  throw "Expected failure: $Expected"
}
try {
  Write-Host "Compile unsigned PE fixture"
  New-Item -ItemType Directory "$root/payload" | Out-Null
  $library = "$root/payload/OpenCL.dll"
  Add-Type -TypeDefinition 'public class StrataSignatureFixture { public static int Value() { return 42; } }' -OutputAssembly $library
  Assert-Fails { & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode verify -Path $library } 'Unsigned or invalid'
  Write-Host "Create ephemeral test certificate"
  $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Strata CI test $([guid]::NewGuid())" -CertStoreLocation Cert:\CurrentUser\My
  Export-Certificate -Cert $certificate -FilePath "$root/test.cer" | Out-Null
  # LocalMachine avoids the interactive CurrentUser root-store consent dialog.
  # GitHub hosted Windows runners are isolated administrators; cleanup is below.
  Import-Certificate -FilePath "$root/test.cer" -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
  Write-Host "Sign fixture and verify installed trust"
  Set-AuthenticodeSignature -FilePath $library -Certificate $certificate -HashAlgorithm SHA256 | Out-Null
  Copy-Item $library "$root/payload/module.pyd"
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode verify -Path "$root/payload" -Recurse -ReportPath "$root/report.json"
  $report = Get-Content "$root/report.json" -Raw | ConvertFrom-Json
  if ($report.result -ne 'passed' -or $report.files.Count -ne 2) { throw 'Incomplete Authenticode report.' }
  $original = (Get-FileHash $library).Hash
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Path $library -DlibPath $library -MetadataPath "$root/report.json"
  if ((Get-FileHash $library).Hash -ne $original) { throw 'Valid third-party signature was replaced.' }
  Compress-Archive "$root/payload/*" "$root/test.zip"
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode package -PackageFiles "$root/test.zip" -VerifyOnly
  $bytes = [System.IO.File]::ReadAllBytes($library)
  $bytes[64] = $bytes[64] -bxor 1
  [System.IO.File]::WriteAllBytes($library, $bytes)
  Assert-Fails { & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode verify -Path $library } 'Unsigned or invalid'
  Assert-Fails { & "$PSScriptRoot/sign-windows-artifacts.ps1" -Path $library -DlibPath $library -MetadataPath "$root/report.json" } 'Existing signature is invalid'
  Write-Host 'Real Windows Authenticode integration tests passed.'
} finally {
  if ($certificate) {
    Remove-Item "Cert:\LocalMachine\Root\$($certificate.Thumbprint)" -ErrorAction SilentlyContinue
    Remove-Item "Cert:\CurrentUser\My\$($certificate.Thumbprint)" -ErrorAction SilentlyContinue
  }
  Remove-Item -LiteralPath $root -Recurse -Force
}
