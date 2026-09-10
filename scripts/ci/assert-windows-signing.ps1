$ErrorActionPreference = 'Stop'
$required = @('AZURE_CLIENT_ID', 'AZURE_TENANT_ID', 'AZURE_SUBSCRIPTION_ID',
  'AZURE_ARTIFACT_SIGNING_ENDPOINT', 'AZURE_ARTIFACT_SIGNING_ACCOUNT_NAME', 'AZURE_ARTIFACT_SIGNING_CERT_PROFILE_NAME')
$missing = @($required | Where-Object { [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($_)) })
if ($missing.Count) { throw "Windows release signing is mandatory. Missing configuration: $($missing -join ', ')" }
if ($env:AZURE_ARTIFACT_SIGNING_ENDPOINT -notmatch '^https://[a-z0-9]+\.codesigning\.azure\.net/?$') {
  throw 'Unexpected Azure Artifact Signing endpoint.'
}
Write-Host 'Windows signing configuration is present; Azure authentication and a real signature are checked next.'
