"""Verify FFmpeg on both architectures from a read-only, signed macOS DMG."""

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from contextlib import contextmanager
from pathlib import Path

from merge_macos_ffmpeg import ARCHITECTURES, verify_ffmpeg


@contextmanager
def mounted_dmg(dmg, log):
    # A failed detach must never let TemporaryDirectory recurse into the volume.
    mount = Path(tempfile.mkdtemp(prefix="strata-dmg-")).resolve()
    disk = "/usr/bin/hdiutil"
    try:
        subprocess.run(
            [disk, "attach", dmg, "-readonly", "-nobrowse", "-mountpoint", mount],
            stdout=log,
            stderr=log,
            check=True,
        )
        try:
            yield mount
        finally:
            for flags in ([], ["-force"]):
                result = subprocess.run(
                    [disk, "detach", mount, *flags], stdout=log, stderr=log, check=False
                )
                if result.returncode == 0:
                    break
            else:
                raise RuntimeError(f"Could not detach {dmg} from {mount}")
    finally:
        try:
            mount.rmdir()
        except OSError:
            # Keep the mount point available for manual detach on failure.
            pass


def verify_dmg(dmg, evidence, source_sha, require_notarization):
    evidence.mkdir(parents=True, exist_ok=True)
    receipt = evidence / "verification.json"
    for name in ("verification.json", "signatures.log", "x86_64.log", "arm64.log"):
        (evidence / name).unlink(missing_ok=True)
    with dmg.open("rb") as stream:
        checksum = hashlib.file_digest(stream, "sha256").hexdigest()
    report = {
        "source_sha": source_sha,
        "dmg": dmg.name,
        "dmg_sha256": checksum,
        "notarization_checked": require_notarization,
        "architectures": {},
    }
    with (
        tempfile.TemporaryDirectory(prefix="strata-video-") as directory,
        (evidence / "signatures.log").open("w") as log,
    ):
        temporary = Path(directory)
        signature = ["/usr/bin/codesign", "--verify", "--strict", "--verbose=2"]
        subprocess.run([*signature, dmg], stdout=log, stderr=log, check=True)
        if require_notarization:
            subprocess.run(
                ["/usr/bin/xcrun", "stapler", "validate", dmg],
                stdout=log,
                stderr=log,
                check=True,
            )
        with mounted_dmg(dmg, log) as mount:
            apps = list(mount.glob("*.app"))
            if len(apps) != 1:
                raise ValueError(f"Expected one app in {dmg}, found {len(apps)}")
            app = apps[0]
            subprocess.run(
                [*signature, "--all-architectures", app],
                stdout=log,
                stderr=log,
                check=True,
            )
            verify_ffmpeg(app)

            contents = app / "Contents"
            interpreters = [
                path
                for path in (contents / "MacOS").glob("python3.*")
                if path.is_file() and re.fullmatch(r"python3\.\d+", path.name)
            ]
            if len(interpreters) != 1:
                raise ValueError(
                    f"Expected one bundled Python interpreter: {interpreters}"
                )
            smoke = Path(__file__).with_name("smoke_macos_video.py").resolve()
            for arch in ARCHITECTURES:
                profile = temporary / arch
                profile.mkdir()
                # Keep Qt and Python on the mounted bundle, with a fresh home
                # and no inherited paths or media backend overrides.
                env = {
                    "HOME": str(profile),
                    "PATH": "/usr/bin:/bin",
                    "TMPDIR": str(profile),
                    "PYTHONHOME": str(contents / "Frameworks"),
                    "QT_PLUGIN_PATH": str(contents / "PlugIns"),
                    "QT_QPA_PLATFORM": "offscreen",
                    "QT_LOGGING_RULES": "qt.multimedia.ffmpeg=true",
                    "PYTHONUNBUFFERED": "1",
                }
                media_log = evidence / f"{arch}.log"
                with media_log.open("w") as output:
                    result = subprocess.run(
                        [
                            "/usr/bin/arch",
                            f"-{arch}",
                            interpreters[0],
                            "-s",
                            "-B",
                            smoke,
                            arch,
                            source_sha,
                            profile,
                        ],
                        cwd=profile,
                        env=env,
                        stdout=output,
                        stderr=output,
                        # A cold Rosetta launch translates the bundled Qt and
                        # PyQGIS libraries before either video timer starts.
                        timeout=180,
                        check=False,
                    )
                output = media_log.read_text(errors="replace")
                if result.returncode or not re.search(
                    r"^qt\.multimedia\.ffmpeg: Using Qt multimedia with FFmpeg version ",
                    output,
                    re.MULTILINE,
                ):
                    raise ValueError(f"{arch}: FFmpeg smoke test failed\n{output}")
                report["architectures"][arch] = {"backend": "ffmpeg", "frames": 3}
                print(
                    f"{arch}: FFmpeg exported and decoded 3 MP4/H.264 frames",
                    flush=True,
                )
    receipt.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Verified {dmg.name} (SHA-256 {checksum})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dmg", type=Path)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument(
        "--skip-notarization", action="store_true", help="Local signed test DMG only"
    )
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
        parser.error("--source-sha must be a full Git commit SHA")
    try:
        verify_dmg(
            args.dmg.resolve(strict=True),
            args.evidence.resolve(),
            args.source_sha,
            not args.skip_notarization,
        )
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\nSee logs under {args.evidence}\n")


if __name__ == "__main__":
    main()
