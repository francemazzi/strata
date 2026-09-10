param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'
$python = Join-Path $Root 'bin/python.exe'
$qgis = @(Get-ChildItem -LiteralPath $Root -Filter '_core*.pyd' -Recurse -File |
  Where-Object { $_.Directory.Name -eq 'qgis' })
if ($qgis.Count -ne 1 -or -not (Test-Path $python)) { throw 'Cannot locate bundled Python/PyQGIS.' }
$names = @('PATH', 'PYTHONHOME', 'PYTHONPATH', 'PYTHONNOUSERSITE', 'QT_QPA_PLATFORM', 'QT_PLUGIN_PATH', 'QGIS_PREFIX_PATH')
$original = @{}
foreach ($name in $names) { $original[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
  $env:PATH = "$Root\bin;$env:PATH"
  $env:PYTHONHOME = "$Root\bin"
  $pythonRoot = $qgis[0].Directory.Parent.FullName
  $env:PYTHONPATH = "$pythonRoot;$pythonRoot\plugins"
  $env:PYTHONNOUSERSITE = '1'
  $env:QT_QPA_PLATFORM = 'offscreen'
  $env:QT_PLUGIN_PATH = "$Root\bin\Qt6\plugins"
  $env:QGIS_PREFIX_PATH = $Root
  $code = @'
import ctypes, os, tempfile
from qgis.core import QgsApplication, QgsProject, QgsVectorLayer, QgsRasterLayer
from qgis.analysis import QgsNativeAlgorithms
from osgeo import gdal
import processing
app = QgsApplication([], False)
app.setPrefixPath(os.environ['QGIS_PREFIX_PATH'], True)
app.initQgis()
app.processingRegistry().addProvider(QgsNativeAlgorithms())
with tempfile.TemporaryDirectory() as tmp:
    vector = QgsVectorLayer('Point?crs=EPSG:4326', 'test', 'memory')
    assert vector.isValid()
    output = processing.run('native:buffer', {'INPUT': vector, 'DISTANCE': 1, 'SEGMENTS': 5,
        'END_CAP_STYLE': 0, 'JOIN_STYLE': 0, 'MITER_LIMIT': 2, 'DISSOLVE': False, 'OUTPUT': 'memory:'})
    assert output['OUTPUT'].isValid()
    raster_path = os.path.join(tmp, 'smoke.tif')
    dataset = gdal.GetDriverByName('GTiff').Create(raster_path, 2, 2, 1)
    dataset.GetRasterBand(1).Fill(1)
    dataset = None
    raster = QgsRasterLayer(raster_path, 'test')
    assert raster.isValid()
    project = QgsProject()
    project.addMapLayer(vector)
    project.addMapLayer(raster)
    assert project.write(os.path.join(tmp, 'smoke.qgz'))
    assert project.read(os.path.join(tmp, 'smoke.qgz'))
    project.clear()
    del project, vector, raster, output
opencl = ctypes.WinDLL(os.path.join(os.environ['QGIS_PREFIX_PATH'], 'bin', 'OpenCL.dll'))
count = ctypes.c_uint()
status = opencl.clGetPlatformIDs(0, None, ctypes.byref(count))
assert status in (0, -1001), ('OpenCL failed', status)
print('OpenCL platform count:', count.value)
app.exitQgis()
print('Windows runtime smoke passed')
'@
  $scriptPath = Join-Path ([System.IO.Path]::GetTempPath()) ('strata-runtime-' + [guid]::NewGuid().ToString('N') + '.py')
  $code | Set-Content -LiteralPath $scriptPath -Encoding utf8
  try {
    $process = Start-Process $python -ArgumentList ('"' + $scriptPath + '"') -PassThru -NoNewWindow
    if (-not $process.WaitForExit(180000)) { $process.Kill(); throw 'Windows runtime smoke timed out.' }
    if ($process.ExitCode -ne 0) { throw 'Bundled Windows runtime smoke failed.' }
  } finally { Remove-Item -LiteralPath $scriptPath }
} finally {
  foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $original[$name]) }
}
