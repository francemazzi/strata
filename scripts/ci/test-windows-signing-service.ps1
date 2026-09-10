$ErrorActionPreference = 'Stop'
$probe = Join-Path $env:RUNNER_TEMP ('strata-signing-probe-' + [guid]::NewGuid().ToString('N') + '.dll')
try {
  Add-Type -TypeDefinition 'public class StrataSigningProbe { public static int Value() { return 1; } }' -OutputAssembly $probe
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Path $probe -ReportPath "$env:RUNNER_TEMP/signing-probe.json"
  & "$PSScriptRoot/sign-windows-artifacts.ps1" -Mode verify -Path $probe
} finally {
  if (Test-Path $probe) { Remove-Item -LiteralPath $probe }
}
