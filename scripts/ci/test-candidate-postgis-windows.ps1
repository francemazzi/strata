$ErrorActionPreference = 'Stop'
$tag = 'strata-v1.6.3'
$sourceSha = '01ee92498993e2d8e3e0b375262d5f66f56a3a51'
$work = Join-Path $env:RUNNER_TEMP 'strata-postgis-candidate'
New-Item -ItemType Directory -Force $work | Out-Null
$release = gh release view $tag --repo francemazzi/strata --json assets | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Cannot read release' }
$name = 'Strata-1.6.3-win64.zip'
$asset = @($release.assets | Where-Object name -eq $name)
if ($asset.Count -ne 1) { throw 'Candidate asset is missing' }
gh release download $tag --repo francemazzi/strata --dir $work --pattern $name
if ($LASTEXITCODE -ne 0) { throw 'Candidate download failed' }
$archive = Join-Path $work $name
$digest = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($asset[0].digest -ne "sha256:$digest") { throw 'Candidate digest mismatch' }
$candidate = Join-Path $work 'candidate'
7z x $archive "-o$candidate" -y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Candidate extraction failed' }
$python = @(Get-ChildItem $candidate -Recurse -File -Filter python.exe | Where-Object { $_.Directory.Name -eq 'bin' })
if ($python.Count -ne 1) { throw 'Cannot locate bundled Python' }
$root = $python[0].Directory.Parent.FullName
$qgis = @(Get-ChildItem $root -Recurse -File -Filter '_core*.pyd' | Where-Object { $_.Directory.Name -eq 'qgis' })
if ($qgis.Count -ne 1) { throw 'Cannot locate bundled PyQGIS' }
$pgRoot = 'C:\Program Files\PostgreSQL\14'
$pgBin = Join-Path $pgRoot 'bin'
if (-not (Test-Path "$pgBin\initdb.exe")) { throw 'PostgreSQL 14 is unavailable' }
Get-Service | Where-Object Name -Like 'postgresql*' | Stop-Service -Force
$postgis = Join-Path $work 'postgis.zip'
Invoke-WebRequest 'https://download.osgeo.org/postgis/windows/pg14/postgis-bundle-pg14-3.6.1x64.zip' -OutFile $postgis
if ((Get-FileHash $postgis -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'd95f1e0ed333c3b0729210fb6573d4d7e07bc7eba61868d51fb1eebd338b1a97') { throw 'PostGIS download digest mismatch' }
7z x $postgis "-o$work\postgis" -y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'PostGIS extraction failed' }
Copy-Item "$work\postgis\postgis-bundle-pg14-3.6.1x64\*" $pgRoot -Recurse -Force
$data = Join-Path $work 'database'
& "$pgBin\initdb.exe" -D $data -U postgres -A trust --encoding=UTF8
if ($LASTEXITCODE -ne 0) { throw 'initdb failed' }
& "$pgBin\pg_ctl.exe" -D $data -l "$work\postgres.log" -o '-p 55013 -h 127.0.0.1' -w start
if ($LASTEXITCODE -ne 0) { throw 'PostgreSQL start failed' }
try {
  & "$pgBin\createdb.exe" -h 127.0.0.1 -p 55013 -U postgres strata_crs_candidate
  if ($LASTEXITCODE -ne 0) { throw 'Database creation failed' }
  & "$pgBin\psql.exe" -h 127.0.0.1 -p 55013 -U postgres -d strata_crs_candidate -v ON_ERROR_STOP=1 -c 'CREATE EXTENSION postgis; SELECT postgis_full_version();'
  if ($LASTEXITCODE -ne 0) { throw 'PostGIS initialization failed' }
  $env:PATH = "$root\bin;$env:SystemRoot\System32;$env:SystemRoot"
  $env:PYTHONHOME = "$root\bin"
  $pythonRoot = $qgis[0].Directory.Parent.FullName
  $env:PYTHONPATH = "$pythonRoot;$pythonRoot\plugins"
  $env:PYTHONNOUSERSITE = '1'
  $env:QT_QPA_PLATFORM = 'offscreen'
  $env:QT_PLUGIN_PATH = "$root\bin\Qt6\plugins"
  $env:QGIS_PREFIX_PATH = $root
  $env:STRATA_CRS_TEST_DB = "host=127.0.0.1 port=55013 dbname=strata_crs_candidate user=postgres sslmode=disable"
  $env:STRATA_CRS_TEST_SOURCE = Join-Path $env:GITHUB_WORKSPACE 'tests/src/python/test_provider_postgres_crs.py'
  $env:STRATA_CRS_CANDIDATE_OUT = Join-Path $env:GITHUB_WORKSPACE 'postgis-candidate-windows.json'
  foreach ($key in @('PROJ_DATA', 'PROJ_LIB', 'GDAL_DATA')) { [Environment]::SetEnvironmentVariable($key, $null) }
  $desktop = Join-Path $root "bin\Strata.exe"
  $version = (& $desktop --version | Out-String).Trim()
  if ($LASTEXITCODE -ne 0 -or $version -notmatch "Strata 1\.6\.3 \(based on QGIS ") { throw "Candidate application version mismatch: $version" }
  & $python[0].FullName (Join-Path $PSScriptRoot 'run_candidate_postgis.py')
  if ($LASTEXITCODE -ne 0) { throw 'Candidate PostGIS regression failed' }
  $result = Get-Content $env:STRATA_CRS_CANDIDATE_OUT -Raw | ConvertFrom-Json
  if (-not $result.success -or $result.source_sha -ne $sourceSha) { throw 'Invalid candidate receipt' }
  $result | Add-Member -NotePropertyName application_version -NotePropertyValue $version
  $result | Add-Member -NotePropertyName asset_sha256 -NotePropertyValue $digest
  $result | ConvertTo-Json -Depth 8 | Set-Content $env:STRATA_CRS_CANDIDATE_OUT -Encoding utf8
} finally {
  & "$pgBin\pg_ctl.exe" -D $data -m fast -w stop
}
