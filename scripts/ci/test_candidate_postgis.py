"""Launch an unmodified desktop package against a disposable PostGIS database."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("desktop", type=Path)
parser.add_argument("--asset", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parents[2] / "tests/src/python/test_provider_postgres_crs.py"
script = Path(__file__).with_name("run_candidate_postgis.py").resolve()
assert os.environ.get("STRATA_CRS_TEST_DB"), "Set the disposable test database"
env = os.environ.copy()
for key in (
    "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "PYTHONPATH",
    "PYTHONHOME", "QGIS_PREFIX_PATH", "QT_PLUGIN_PATH", "PROJ_DATA", "PROJ_LIB", "GDAL_DATA",
):
    env.pop(key, None)
output = args.output.resolve()
with tempfile.TemporaryDirectory(prefix="strata-candidate-postgis-") as directory:
    org = "getstrata.org" if sys.platform == "darwin" else "Strata"
    settings = Path(directory) / "profiles" / "crs-test" / org / "Strata.ini"
    settings.parent.mkdir(parents=True)
    settings.write_text(
        "[authentication]\nuse-password-helper=false\n"
        "generate-random-password-for-keychain=false\n"
    )
    env.update(
        QT_QPA_PLATFORM="xcb" if sys.platform.startswith("linux") else "offscreen",
        QTWEBENGINE_DISABLE_SANDBOX="1",
        STRATA_CRS_CANDIDATE_OUT=str(output), STRATA_CRS_TEST_SOURCE=str(source),
        APPIMAGE_EXTRACT_AND_RUN="1",
    )
    version = subprocess.check_output([str(args.desktop.resolve()), "--version"], env=env, text=True, timeout=180)
    assert "Strata 1.6.3 (based on QGIS " in version, version
    subprocess.run([
        str(args.desktop.resolve()), "--profiles-path", directory, "--profile", "crs-test",
        "--noplugins", "--noversioncheck", "--nologo", "--code", str(script),
    ], env=env, timeout=300, check=True)
report = json.loads(output.read_text())
report["application_version"] = version.strip()
assert report["success"] and report["tests_run"] == 5 and report["skipped"] == 0
with args.asset.open("rb") as asset:
    report["asset_sha256"] = hashlib.file_digest(asset, "sha256").hexdigest()
output.write_text(json.dumps(report, indent=2) + "\n")
print("Candidate PostGIS regression passed:", output)
