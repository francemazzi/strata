#!/usr/bin/env bash
# Complete an unpublished AppImage's Python runtime without recompiling its
# application sources. The caller verifies the source artifact and draft tag.
set -euo pipefail

input=$(realpath "$1")
version=$2
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
test ! -e AppDir
chmod +x "$input"
"$input" --appimage-extract > /dev/null
mv squashfs-root AppDir
sha256sum AppDir/usr/bin/Strata > application-before.sha256

python_version=$(/usr/bin/python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
site="$PWD/AppDir/usr/lib/python${python_version}/site-packages"
test -d "$site/PyQt6"
for module in osgeo numpy; do
  test -d "/usr/lib/python3/dist-packages/$module"
  rm -rf "${site:?}/$module"
  cp -a "/usr/lib/python3/dist-packages/$module" "$site/"
done
find "$site/osgeo" "$site/numpy" -type d -name __pycache__ -prune -exec rm -rf {} +
dpkg-query -W python3-gdal python3-numpy libgdal34t64 > python-runtime-packages.txt

mkdir -p .tools
curl --fail --location --retry 3 https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage -o .tools/linuxdeploy-x86_64.AppImage
chmod +x .tools/linuxdeploy-x86_64.AppImage
sha256sum .tools/linuxdeploy-x86_64.AppImage > packaging-tool.sha256
libraries=()
while IFS= read -r -d '' library; do
  libraries+=(--library "$library")
done < <(find "$site/osgeo" "$site/numpy" -type f -name '*.so' -print0)

# Resolve the newly included Python extensions and their native dependencies.
# The existing Qt deployment and the compiled Strata executable stay intact.
export APPIMAGE_EXTRACT_AND_RUN=1
export LD_LIBRARY_PATH="$PWD/AppDir/usr/lib:$PWD/AppDir/usr/lib/qgis:$PWD/AppDir/usr/lib/x86_64-linux-gnu"
export OUTPUT="Strata-${version}-x86_64.AppImage"
.tools/linuxdeploy-x86_64.AppImage --appdir AppDir --output appimage "${libraries[@]}"
sha256sum --check application-before.sha256

PYTHONHOME="$PWD/AppDir/usr" PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1 \
  PYTHONPATH="$site" /usr/bin/python3 - <<'PYCODE'
import json
from pathlib import Path
import numpy
from osgeo import gdal, gdal_array, ogr, osr
array = numpy.array([[1, 2]], dtype=numpy.uint8)
dataset = gdal_array.OpenArray(array)
assert dataset.ReadAsArray().tolist() == [[1, 2]]
assert Path(gdal.__file__).resolve().is_relative_to(Path('AppDir').resolve())
assert Path(numpy.__file__).resolve().is_relative_to(Path('AppDir').resolve())
Path('python-runtime-repair.json').write_text(json.dumps({
    'gdal': gdal.VersionInfo(), 'numpy': numpy.__version__,
    'bundled_modules': True, 'gdal_numpy_array_roundtrip': True,
    'application_executable_unchanged': True,
}, indent=2) + '\n')
PYCODE
unset LD_LIBRARY_PATH
xvfb-run -a python3 scripts/ci/verify_crs_runtime.py "./$OUTPUT" --output crs-runtime-linux.json
