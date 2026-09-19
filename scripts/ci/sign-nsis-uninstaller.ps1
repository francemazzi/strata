param([Parameter(Mandatory = $true)][string]$Path)
$ErrorActionPreference = 'Stop'
$original = Get-Item -LiteralPath $Path
if ($original.PSIsContainer) { throw 'NSIS finalizer requires a file.' }
$workspace = Join-Path ([System.IO.Path]::GetTempPath()) ('strata-uninstaller-' + [guid]::NewGuid().ToString('N'))
try {
  New-Item -ItemType Directory -Path $workspace | Out-Null
  # NSIS gives !uninstfinalize a .tmp file. Keep the general PE extension policy
  # strict and use an EXE working copy for SignTool and Authenticode validation.
  $executable = Join-Path $workspace 'uninstall.exe'
  Copy-Item -LiteralPath $original.FullName -Destination $executable
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode staging -Path $executable
  # Replace NSIS's temporary bytes only after the signer has verified its result.
  Copy-Item -LiteralPath $executable -Destination $original.FullName -Force
} finally {
  if (Test-Path -LiteralPath $workspace) { Remove-Item -LiteralPath $workspace -Recurse -Force }
}
