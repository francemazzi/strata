import json
import os
import sys
import tempfile

import console
import qgis.core
import qgis.gui
from qgis.PyQt import Qsci

# This script runs after application initialization in the packaged desktop.
# An import alone does not prove that the PROJ catalog can resolve layer CRSs.
crs_checks = {}
for epsg in (4326, 3003, 3004, 32632, 32633, 25832, 6707, 7791, 7792):
    crs = qgis.core.QgsCoordinateReferenceSystem.fromEpsgId(epsg)
    assert crs.isValid() and crs.authid() == f"EPSG:{epsg}" and crs.toWkt(), epsg
    crs_checks[str(epsg)] = crs.authid()

custom = qgis.core.QgsCoordinateReferenceSystem.fromProj(
    "+proj=tmerc +lat_0=0 +lon_0=9.123456 +k=0.9996 +x_0=500000 +ellps=GRS80 +units=m"
)
assert custom.isValid() and not custom.authid() and custom.toWkt()
layer = qgis.core.QgsVectorLayer("Point", "custom CRS smoke", "memory")
layer.setCrs(custom)
assert layer.isValid() and layer.crs().isValid() and layer.crs().toWkt()
project = qgis.core.QgsProject()
project.addMapLayer(layer)
with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, "crs.qgz")
    assert project.write(path)
    project.clear()
    assert project.read(path)
    assert next(iter(project.mapLayers().values())).crs() == custom
project.clear()

out_path = os.environ.get("STRATA_PYQGIS_SMOKE_OUT")
if out_path:
    with open(out_path, "w", encoding="utf-8") as out_file:
        json.dump(
            {
                "ok": True,
                "python": sys.version.split()[0],
                "qgis_core": qgis.core.Qgis.version(),
                "qsci": bool(Qsci),
                "console": bool(console),
                "crs_checks": crs_checks,
                "custom_crs": True,
            },
            out_file,
            sort_keys=True,
        )

# The AppImage smoke test only needs to prove that the bundled PyQGIS runtime
# imports and resolves the bundled CRS catalog successfully. Avoid Qt/QGIS teardown in CI, which can abort after the
# success marker has already been written.
os._exit(0)
