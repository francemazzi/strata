"""Run a tag's PostGIS CRS regression suite with the candidate package's own PyQGIS.

Started inside the packaged desktop (--code) or with its bundled Python. Writes a
JSON receipt to STRATA_CRS_CANDIDATE_OUT and exits 0 only when every test of the
suite ran and passed, so a skipped database test cannot pass silently.

Env: STRATA_CRS_CANDIDATE_OUT, STRATA_CRS_TEST_SOURCE (the tag's
tests/src/python/test_provider_postgres_crs.py), STRATA_CRS_TEST_DB,
STRATA_CANDIDATE_TAG, STRATA_CANDIDATE_SHA.
"""

import hashlib
import importlib.util
import json
import os
import platform
import sys
import traceback
import unittest
from pathlib import Path

from qgis.core import Qgis, QgsApplication
import qgis.testing

output = Path(os.environ["STRATA_CRS_CANDIDATE_OUT"])
source = Path(os.environ["STRATA_CRS_TEST_SOURCE"])
report = {
    "tag": os.environ.get("STRATA_CANDIDATE_TAG", ""),
    "source_sha": os.environ.get("STRATA_CANDIDATE_SHA", ""),
    "test_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "platform": sys.platform,
    "architecture": platform.machine(),
    "qgis_module": str(Path(sys.modules["qgis.core"].__file__).resolve()),
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
    expected = suite.countTestCases()
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report.update(
        tests_expected=expected,
        tests_run=result.testsRun,
        failures=len(result.failures),
        errors=len(result.errors),
        skipped=len(result.skipped),
        success=(
            result.wasSuccessful()
            and expected > 0
            and result.testsRun == expected
            and not result.skipped
        ),
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
