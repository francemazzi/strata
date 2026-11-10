"""Exercise the CRS catalog through the packaged desktop, in an isolated profile.

Pass the macOS bundle executable or the Linux AppImage. Windows uses the same
assertions in test-windows-runtime.ps1. This never publishes an artifact.

The desktop runs this file as its --code script, in the mode chosen by
STRATA_CRS_RUNTIME_IN_DESKTOP: it transforms known points with the bundled PROJ,
then runs smoke_pyqgis.py. The transform checks live here, not in
smoke_pyqgis.py, because build-appimage.yml repackages older tags with this
file taken from the current commit and smoke_pyqgis.py from the tag.
"""

import argparse
import json
import os
import runpy
import subprocess
import sys
import tempfile
import traceback
from pathlib import Path

IN_DESKTOP = "STRATA_CRS_RUNTIME_IN_DESKTOP"
SMOKE_SCRIPT = "STRATA_PYQGIS_SMOKE_SCRIPT"

# Expected coordinates come from PROJ's cs2cs, independently of QGIS. A transform
# PROJ cannot build leaves the point where it was while both CRSs still look valid:
# on 5 November 2026 a reprojection kept lon/lat values in an EPSG:32632 layer.
# For EPSG:3003 the expected value uses "Monte Mario to WGS 84 (4)". Without a datum
# choice in the project, PROJ may use its ballpark offset instead, about 78 m away,
# so this case only proves that the point reached Monte Mario coordinates.
TRANSFORM_CASES = (
    ("EPSG:4326", "EPSG:32632", (10.78, 45.05), (640169.1529, 4990045.9721), 0.01),
    ("EPSG:32632", "EPSG:4326", (640169.1529, 4990045.9721), (10.78, 45.05), 1e-7),
    ("EPSG:4326", "EPSG:3003", (11.8768, 45.4064), (1725136.5327, 5032218.5950), 150.0),
)
REPROJECT_CASE = (
    "EPSG:4326",
    "EPSG:32632",
    (10.63, 45.38),
    (627616.7159, 5026457.2466),
    0.01,
)
# Datum operations are named from proj.db. Without it PROJ still projects with the
# definitions QGIS keeps in srs.db, so the points above can pass, but it lists
# only a "Ballpark ... from unknown to unknown" operation.
DATABASE_CASE = ("EPSG:4326", "EPSG:3003", "Monte Mario to WGS 84")


def near(actual, expected, tolerance):
    return actual is not None and all(
        abs(a - e) <= tolerance for a, e in zip(actual, expected)
    )


def check_transforms_in_desktop():
    """Transform known points inside the desktop, as users' layers are."""
    from qgis.analysis import QgsNativeAlgorithms
    from qgis.core import (
        QgsApplication,
        QgsCoordinateReferenceSystem,
        QgsCoordinateTransform,
        QgsCsException,
        QgsDatumTransform,
        QgsFeature,
        QgsGeometry,
        QgsPointXY,
        QgsProcessingContext,
        QgsProcessingFeedback,
        QgsProcessingUtils,
        QgsProject,
        QgsProjUtils,
        QgsVectorLayer,
    )

    checks = []
    for source, target, point, expected, tolerance in TRANSFORM_CASES:
        transform = QgsCoordinateTransform(
            QgsCoordinateReferenceSystem(source),
            QgsCoordinateReferenceSystem(target),
            QgsProject.instance(),
        )
        actual, error = None, None
        if transform.isValid():
            try:
                result = transform.transform(QgsPointXY(*point))
                actual = [result.x(), result.y()]
            except QgsCsException as exception:
                error = str(exception)
        checks.append(
            {
                "operation": f"{source} -> {target}",
                "valid": transform.isValid(),
                "input": list(point),
                "expected": list(expected),
                "actual": actual,
                "error": error,
                "ok": near(actual, expected, tolerance),
            }
        )

    registry = QgsApplication.processingRegistry()
    if not registry.providerById("native"):
        registry.addProvider(QgsNativeAlgorithms(registry))
    source, target, point, expected, tolerance = REPROJECT_CASE
    layer = QgsVectorLayer(f"Point?crs={source}", "reprojection smoke", "memory")
    feature = QgsFeature()
    feature.setGeometry(QgsGeometry.fromPointXY(QgsPointXY(*point)))
    layer.dataProvider().addFeature(feature)
    context = QgsProcessingContext()
    context.setProject(QgsProject.instance())
    algorithm = registry.createAlgorithmById("native:reprojectlayer")
    results, ran = algorithm.run(
        {
            "INPUT": layer,
            "TARGET_CRS": QgsCoordinateReferenceSystem(target),
            "OUTPUT": "TEMPORARY_OUTPUT",
        },
        context,
        QgsProcessingFeedback(),
    )
    output = (
        QgsProcessingUtils.mapLayerFromString(results["OUTPUT"], context)
        if ran
        else None
    )
    actual = None
    if output:
        moved = next(output.getFeatures()).geometry().asPoint()
        actual = [moved.x(), moved.y()]
    labelled = bool(output) and output.crs().authid() == target
    checks.append(
        {
            "operation": f"native:reprojectlayer {source} -> {target}",
            "valid": labelled,
            "input": list(point),
            "expected": list(expected),
            "actual": actual,
            "error": None if ran else "the algorithm did not run",
            "ok": labelled and near(actual, expected, tolerance),
        }
    )
    source, target, expected_name = DATABASE_CASE
    operations = [
        operation.name
        for operation in QgsDatumTransform.operations(
            QgsCoordinateReferenceSystem(source), QgsCoordinateReferenceSystem(target)
        )
    ]
    database = {
        "operation": f"{source} -> {target}",
        "operations": operations,
        "ok": any(expected_name in name for name in operations),
    }
    search_paths = QgsProjUtils.searchPaths()
    return {
        "ok": all(check["ok"] for check in checks) and database["ok"],
        "proj": f"{QgsProjUtils.projVersionMajor()}.{QgsProjUtils.projVersionMinor()}",
        "proj_search_paths": search_paths,
        "proj_db_found_in": [
            path
            for path in search_paths
            if os.path.isfile(os.path.join(path, "proj.db"))
        ],
        "proj_database": database,
        "checks": checks,
    }


def run_in_desktop():
    try:
        evidence = check_transforms_in_desktop()
    except Exception:
        evidence = {"ok": False, "error": traceback.format_exc()}
    Path(os.environ[IN_DESKTOP]).write_text(json.dumps(evidence, indent=2) + "\n")
    # The PyQGIS smoke writes its own evidence and exits the desktop.
    runpy.run_path(os.environ[SMOKE_SCRIPT], run_name="__main__")


def transform_failures(evidence):
    if evidence.get("error"):
        return evidence["error"]
    lines = [
        f"{check['operation']}: expected {check['expected']}, got {check['actual']}"
        + (f" ({check['error']})" if check.get("error") else "")
        for check in evidence.get("checks", [])
        if not check.get("ok")
    ]
    database = evidence.get("proj_database", {})
    if not database.get("ok"):
        lines.append(
            f"PROJ database not usable: {database.get('operation')} lists only {database.get('operations')}"
        )
    lines.append(
        f"PROJ {evidence.get('proj')} search paths: {evidence.get('proj_search_paths')}, "
        f"proj.db found in: {evidence.get('proj_db_found_in')}"
    )
    return "\n".join(lines)


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
        transforms = Path(temporary) / "transforms.json"
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
            # The Linux AppImage ships xcb, not the optional offscreen plugin.
            # Run it under Xvfb in CI, using the same platform plugin as users.
            QT_QPA_PLATFORM="xcb" if sys.platform.startswith("linux") else "offscreen",
            QTWEBENGINE_DISABLE_SANDBOX="1",
            # Importing modules must not add bytecode to a signed app bundle.
            PYTHONDONTWRITEBYTECODE="1",
            STRATA_PYQGIS_SMOKE_OUT=str(result),
            **{IN_DESKTOP: str(transforms), SMOKE_SCRIPT: str(script)},
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
                str(Path(__file__).resolve()),
            ],
            env=env,
            timeout=120,
            check=False,
        )
        if not transforms.is_file():
            raise SystemExit("The packaged desktop did not run the transform checks")
        transform_evidence = json.loads(transforms.read_text())
        if not transform_evidence.get("ok"):
            raise SystemExit(
                "The packaged desktop does not transform coordinates correctly:\n"
                + transform_failures(transform_evidence)
            )
        if completed.returncode or not result.is_file():
            raise SystemExit("The packaged desktop did not complete the CRS checks")
        evidence = json.loads(result.read_text())
        evidence["runtime_mode"] = (
            "development" if args.development_runtime else "package"
        )
        evidence["transform_checks"] = transform_evidence
        if (
            not evidence.get("ok")
            or not evidence.get("crs_checks")
            or not evidence.get("custom_crs")
        ):
            raise SystemExit("Incomplete packaged CRS evidence")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n")
        print("Desktop CRS and transform checks passed:", args.output)


if __name__ == "__main__":
    if os.environ.get(IN_DESKTOP):
        run_in_desktop()
    else:
        main()
