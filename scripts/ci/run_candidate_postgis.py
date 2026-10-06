"""Run the tagged PostGIS regression suite using only the candidate's PyQGIS."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import traceback
import unittest

from qgis.core import Qgis, QgsApplication
import qgis.testing

output = Path(os.environ["STRATA_CRS_CANDIDATE_OUT"])
source = Path(os.environ["STRATA_CRS_TEST_SOURCE"])
report = {
    "tag": "strata-v1.6.3",
    "source_sha": "01ee92498993e2d8e3e0b375262d5f66f56a3a51",
    "test_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "platform": sys.platform,
    "qgis_version": Qgis.QGIS_VERSION,
    "qgis_prefix": QgsApplication.prefixPath(),
    "runtime_mode": "package",
    "success": False,
}
try:
    if QgsApplication.instance():
        qgis.testing.QGISAPP = QgsApplication.instance()
    else:
        qgis.testing.start_app(cleanup=False)
    report["qgis_prefix"] = QgsApplication.prefixPath()
    spec = importlib.util.spec_from_file_location("candidate_postgis", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(module.TestPostgresCrs)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report.update(
        tests_run=result.testsRun,
        failures=len(result.failures),
        errors=len(result.errors),
        skipped=len(result.skipped),
        success=result.wasSuccessful() and result.testsRun == 5 and not result.skipped,
    )
except BaseException:
    traceback.print_exc()
finally:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report), flush=True)
    sys.stdout.flush()
    sys.stderr.flush()
    # Qt/Python teardown is outside this provider regression check.
    os._exit(0 if report["success"] else 1)
