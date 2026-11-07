"""Launch an unmodified desktop package against a disposable PostGIS database.

Usage: test_candidate_postgis.py <AppImage or app executable>
         --version X.Y.Z --test-source <tag's test_provider_postgres_crs.py>
         --asset <downloaded release asset> --output <receipt.json>
Env:   STRATA_CRS_TEST_DB (disposable database), STRATA_CANDIDATE_TAG,
       STRATA_CANDIDATE_SHA (recorded in the receipt).
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("desktop", type=Path)
parser.add_argument("--version", required=True, help="Expected Strata version, e.g. 1.8.1")
parser.add_argument("--test-source", type=Path, required=True)
parser.add_argument("--asset", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
assert os.environ.get("STRATA_CRS_TEST_DB"), "Set the disposable test database"

script = Path(__file__).with_name("run_candidate_postgis.py").resolve()
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
    # No credentials are needed; never prompt the CI keychain.
    settings.write_text(
        "[authentication]\nuse-password-helper=false\n"
        "generate-random-password-for-keychain=false\n"
    )
    env.update(
        # The Linux AppImage ships xcb, not the optional offscreen plugin: run it
        # under Xvfb with the same platform plugin as users.
        QT_QPA_PLATFORM="xcb" if sys.platform.startswith("linux") else "offscreen",
        QTWEBENGINE_DISABLE_SANDBOX="1",
        PYTHONDONTWRITEBYTECODE="1",
        STRATA_CRS_CANDIDATE_OUT=str(output),
        STRATA_CRS_TEST_SOURCE=str(args.test_source.resolve(strict=True)),
        APPIMAGE_EXTRACT_AND_RUN="1",
    )
    desktop = str(args.desktop.resolve(strict=True))
    version = subprocess.check_output([desktop, "--version"], env=env, text=True, timeout=180)
    assert f"Strata {args.version} (based on QGIS " in version, version
    subprocess.run(
        [
            desktop, "--profiles-path", directory, "--profile", "crs-test",
            "--noplugins", "--noversioncheck", "--nologo", "--code", str(script),
        ],
        env=env, timeout=300, check=True,
    )
report = json.loads(output.read_text())
report["application_version"] = version.strip()
assert report["success"], report
with args.asset.open("rb") as asset:
    report["asset_sha256"] = hashlib.file_digest(asset, "sha256").hexdigest()
output.write_text(json.dumps(report, indent=2) + "\n")
print("Candidate PostGIS regression passed:", output)
