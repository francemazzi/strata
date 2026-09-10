$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/windows-signing-files.ps1"
$script:Records = [System.Collections.Generic.List[object]]::new()
$script:SignatureStatus = 'Valid'
function Get-AuthenticodeSignature {
  param([string]$LiteralPath)
  return @{ Status = $script:SignatureStatus; SignerCertificate = @{ Subject = 'CN=Original vendor'; Thumbprint = 'abc' } }
}
function Test-AuthenticodeSignature { param([string]$FilePath) return $script:SignatureStatus -eq 'Valid' }
function Invoke-UnsignedFile { param([string]$FilePath) throw 'Unexpected signing call' }
function Assert-Fails {
  param([scriptblock]$Action, [string]$Expected)
  try { & $Action } catch {
    if ($_.Exception.Message -like "*$Expected*") { return }
    throw "Expected '$Expected', got '$($_.Exception.Message)'"
  }
  throw "Expected failure: $Expected"
}
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('strata-signing-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root | Out-Null
try {
  $bytes = [byte[]]::new(512)
  $bytes[0] = 0x4d; $bytes[1] = 0x5a; $bytes[60] = 128
  $bytes[128] = 0x50; $bytes[129] = 0x45
  $bytes[132] = 0x64; $bytes[133] = 0x86
  $bytes[152] = 0x0b; $bytes[153] = 0x02
  $file = Join-Path $root 'OpenCL.dll'
  [System.IO.File]::WriteAllBytes($file, $bytes)
  $original = (Get-FileHash $file).Hash
  Invoke-SignableFile $file 'OpenCL.dll'
  if ($script:Records[0].action -ne 'preserved' -or (Get-FileHash $file).Hash -ne $original) { throw 'Vendor signature changed.' }
  $script:SignatureStatus = 'HashMismatch'
  Assert-Fails { Invoke-SignableFile $file 'OpenCL.dll' } 'Existing signature is invalid'
  $script:SignatureStatus = 'NotSigned'
  Assert-Fails { Invoke-SignableFile $file 'OpenCL.dll' -OnlyVerify } 'Unsigned or invalid'
  $bytes[128 + 24 + 112 + 32] = 1
  [System.IO.File]::WriteAllBytes($file, $bytes)
  Assert-Fails { Invoke-SignableFile $file 'OpenCL.dll' } 'Existing signature is invalid'
  Copy-Item $file "$root/uncovered.bin"
  Assert-Fails { Invoke-SignableFile "$root/uncovered.bin" 'uncovered.bin' } 'Uncovered PE'
  Set-Content $file 'not a library'
  Assert-Fails { Invoke-SignableFile $file 'OpenCL.dll' } 'not a PE'
  [System.IO.File]::WriteAllBytes($file, [byte[]]@(0x4d, 0x5a))
  Assert-Fails { Get-PeInfo $file } 'Truncated PE'
  Set-Content "$root/readme.txt" 'data'
  Compress-Archive "$root/readme.txt" "$root/empty.zip"
  Assert-Fails { Invoke-ZipVerification "$root/empty.zip" } 'no executable code'
  $required = @('AZURE_CLIENT_ID','AZURE_TENANT_ID','AZURE_SUBSCRIPTION_ID','AZURE_ARTIFACT_SIGNING_ENDPOINT','AZURE_ARTIFACT_SIGNING_ACCOUNT_NAME','AZURE_ARTIFACT_SIGNING_CERT_PROFILE_NAME')
  $originalEnv = @{}
  foreach ($name in $required) { $originalEnv[$name] = [Environment]::GetEnvironmentVariable($name); [Environment]::SetEnvironmentVariable($name, $null) }
  try { Assert-Fails { & "$PSScriptRoot/assert-windows-signing.ps1" } 'Missing configuration' }
  finally { foreach ($name in $required) { [Environment]::SetEnvironmentVariable($name, $originalEnv[$name]) } }
  Write-Host 'Windows signing policy tests passed (9 scenarios).'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
