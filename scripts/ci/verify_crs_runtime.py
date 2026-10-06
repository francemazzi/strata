"""Exercise the CRS catalog through the packaged desktop, in an isolated profile.

Pass the macOS bundle executable or the Linux AppImage. Windows uses the same
assertions in test-windows-runtime.ps1. This never publishes an artifact.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("desktop", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--development-runtime",
        action="store_true",
        help="Allow host libraries for an unbundled developer build; this does not validate a package",
    )
    args = parser.parse_args()
    desktop = args.desktop.resolve(strict=True)
    script = Path(__file__).with_name("smoke_pyqgis.py").resolve()
    with tempfile.TemporaryDirectory(prefix="strata-crs-smoke-") as temporary:
        result = Path(temporary) / "result.json"
        organization = "getstrata.org" if sys.platform == "darwin" else "Strata"
        settings = (
            Path(temporary) / "profiles" / "crs-test" / organization / "Strata.ini"
        )
        settings.parent.mkdir(parents=True)
        # Runtime checks need no credentials and must not prompt the CI keychain.
        settings.write_text(
            "[authentication]\nuse-password-helper=false\n"
            "generate-random-password-for-keychain=false\n"
        )
        env = os.environ.copy()
        if not args.development_runtime:
            for key in (
                "LD_LIBRARY_PATH",
                "DYLD_LIBRARY_PATH",
                "DYLD_FRAMEWORK_PATH",
                "PYTHONPATH",
                "PYTHONHOME",
                "QGIS_PREFIX_PATH",
                "QT_PLUGIN_PATH",
                "PROJ_DATA",
                "PROJ_LIB",
                "GDAL_DATA",
            ):
                env.pop(key, None)
        env.update(
            QT_QPA_PLATFORM="offscreen",
            QTWEBENGINE_DISABLE_SANDBOX="1",
            STRATA_PYQGIS_SMOKE_OUT=str(result),
        )
        completed = subprocess.run(
            [
                str(desktop),
                "--profiles-path",
                temporary,
                "--profile",
                "crs-test",
                "--noplugins",
                "--noversioncheck",
                "--nologo",
                "--code",
                str(script),
            ],
            env=env,
            timeout=120,
            check=False,
        )
        if completed.returncode or not result.is_file():
            raise SystemExit("The packaged desktop did not complete the CRS checks")
        evidence = json.loads(result.read_text())
        evidence["runtime_mode"] = (
            "development" if args.development_runtime else "package"
        )
        if (
            not evidence.get("ok")
            or not evidence.get("crs_checks")
            or not evidence.get("custom_crs")
        ):
            raise SystemExit("Incomplete packaged CRS evidence")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n")
        print("Desktop CRS checks passed:", args.output)


if __name__ == "__main__":
    main()
