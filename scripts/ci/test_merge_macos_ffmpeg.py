"""Exercise the macOS merge with real thin and one-slice Universal files."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from merge_macos_ffmpeg import (
    ARCHITECTURES,
    FRAMEWORKS,
    LIBRARIES,
    PLUGIN,
    check_architectures,
    merge_ffmpeg,
)


class TestMergeMacosFfmpeg(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name).resolve()
        self.intel, self.arm, self.universal = [
            root / f"{name}.app" for name in ("intel", "arm", "universal")
        ]
        source = root / "fixture.c"
        source.write_text("int fixture(void) { return 1; }\n")
        for app, arch in zip((self.intel, self.arm), ARCHITECTURES):
            frameworks = app / FRAMEWORKS
            frameworks.mkdir(parents=True)
            (app / PLUGIN).parent.mkdir(parents=True)
            self.compile_fixture(source, app / PLUGIN, arch, "-bundle")
            for stem in LIBRARIES:
                filename = f"{stem}.1.2.3.dylib"
                binary = frameworks / filename
                self.compile_fixture(
                    source,
                    binary,
                    arch,
                    "-dynamiclib",
                    "-install_name",
                    f"@rpath/{filename}",
                    "-current_version",
                    "1.2.3",
                )
                if arch == "x86_64":
                    # lipo -create on a single input emits a Universal header.
                    container = binary.with_suffix(".fat")
                    subprocess.run(
                        ["/usr/bin/lipo", "-create", binary, "-output", container],
                        check=True,
                    )
                    container.replace(binary)
                    self.assertEqual(binary.read_bytes()[:4], b"\xca\xfe\xba\xbe")
                (frameworks / f"{stem}.1.dylib").symlink_to(filename)
                (frameworks / f"{stem}.dylib").symlink_to(f"{stem}.1.dylib")
        shutil.copytree(self.intel, self.universal, symlinks=True)

    def compile_fixture(self, source, target, arch, *options):
        subprocess.run(
            ["/usr/bin/clang", "-arch", arch, *options, source, "-o", target],
            check=True,
        )

    def test_merges_single_slice_containers_and_restores_relative_aliases(self):
        merge_ffmpeg(self.intel, self.arm, self.universal)
        check_architectures(self.universal / PLUGIN, ARCHITECTURES)
        for stem in LIBRARIES:
            binary = self.universal / FRAMEWORKS / f"{stem}.1.2.3.dylib"
            check_architectures(binary, ARCHITECTURES)
            for name in (f"{stem}.1.dylib", f"{stem}.dylib"):
                alias = binary.with_name(name)
                self.assertEqual(alias.readlink(), Path(binary.name))
                self.assertEqual(alias.resolve(strict=True), binary)

    def test_missing_input_leaves_output_unmodified(self):
        (self.arm / FRAMEWORKS / "libswscale.1.2.3.dylib").unlink()
        with self.assertRaises(FileNotFoundError):
            merge_ffmpeg(self.intel, self.arm, self.universal)
        check_architectures(self.universal / PLUGIN, ("x86_64",))
        check_architectures(
            self.universal / FRAMEWORKS / "libavformat.1.2.3.dylib", ("x86_64",)
        )

    def test_rejects_different_dylib_versions(self):
        library = self.arm / FRAMEWORKS / "libavformat.1.2.3.dylib"
        subprocess.run(
            [
                "/usr/bin/install_name_tool",
                "-id",
                "@rpath/libavformat.2.dylib",
                library,
            ],
            check=True,
        )
        with self.assertRaisesRegex(ValueError, "mismatched dylib identities"):
            merge_ffmpeg(self.intel, self.arm, self.universal)


if __name__ == "__main__":
    unittest.main()
