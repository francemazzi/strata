"""Merge the FFmpeg runtime from the two staged macOS apps before signing."""

import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ARCHITECTURES = ("x86_64", "arm64")
FRAMEWORKS = Path("Contents/Frameworks")
PLUGIN = Path("Contents/PlugIns/multimedia/libffmpegmediaplugin.dylib")
LIBRARIES = (
    "libavcodec",
    "libavdevice",
    "libavfilter",
    "libavformat",
    "libavutil",
    "libswresample",
    "libswscale",
)


def tool(name, *args):
    result = subprocess.run(
        [f"/usr/bin/{name}", *args], capture_output=True, text=True, check=True
    )
    return result.stdout


def check_architectures(path, expected):
    actual = set(tool("lipo", "-archs", path).split())
    if actual != set(expected):
        raise ValueError(f"{path}: expected {sorted(expected)}, found {sorted(actual)}")


def bundled_binary(path):
    target = path.resolve(strict=True)
    if target.parent != path.parent or not target.is_file():
        raise ValueError(f"{path}: expected a binary in {path.parent}, found {target}")
    return target


def library_names(directory, stem):
    pattern = re.compile(rf"{stem}(?:\.\d+)*\.dylib")
    return {path.name for path in directory.iterdir() if pattern.fullmatch(path.name)}


def library_identity(binary, arch, stem):
    # The dylib's first entry from otool -L is its own install name and versions.
    _, _, entries = tool("otool", "-arch", arch, "-L", binary).partition("\n")
    identity = entries.split("\n", 1)[0].strip()
    if not re.fullmatch(
        rf"@rpath/{stem}(?:\.\d+)*\.dylib "
        r"\(compatibility version [\d.]+, current version [\d.]+\)",
        identity,
    ):
        raise ValueError(f"{binary} ({arch}): unexpected dylib identity {identity!r}")
    return identity


def plugin_dependencies(binary, arch):
    # Compare Qt and FFmpeg ABI versions. System library versions can differ
    # between the SDKs used for each architecture.
    dependencies = set()
    for line in tool("otool", "-arch", arch, "-L", binary).splitlines()[1:]:
        dependency = line.strip()
        name = Path(dependency.split(" (", 1)[0]).name
        if name.startswith("libQt6") or name.split(".", 1)[0] in LIBRARIES:
            dependencies.add(dependency)
    return dependencies


def native_library(directory, stem, arch):
    anchor = directory / f"{stem}.dylib"
    binary = bundled_binary(anchor)
    check_architectures(binary, (arch,))
    names = library_names(directory, stem)
    for name in names:
        alias = directory / name
        if bundled_binary(alias) != binary:
            raise ValueError(f"{alias}: expected an alias of {binary.name}")
    return binary, names, library_identity(binary, arch, stem)


def verify_ffmpeg(app):
    app = app.resolve(strict=True)
    frameworks = app / FRAMEWORKS
    plugin = bundled_binary(app / PLUGIN)
    check_architectures(plugin, ARCHITECTURES)
    if plugin_dependencies(plugin, "x86_64") != plugin_dependencies(plugin, "arm64"):
        raise ValueError(
            f"{plugin}: architectures link different Qt or FFmpeg versions"
        )
    for stem in LIBRARIES:
        binary = bundled_binary(frameworks / f"{stem}.dylib")
        check_architectures(binary, ARCHITECTURES)
        identity = library_identity(binary, "x86_64", stem)
        if library_identity(binary, "arm64", stem) != identity:
            raise ValueError(f"{binary}: architectures have different dylib identities")
        install_name = Path(identity.split(" (", 1)[0]).name
        for name in library_names(frameworks, stem) | {install_name}:
            alias = frameworks / name
            if bundled_binary(alias) != binary or (
                alias.is_symlink() and alias.readlink().is_absolute()
            ):
                raise ValueError(f"{alias}: expected a relative alias of {binary.name}")


def merge_ffmpeg(intel, arm, output):
    sources = (intel.resolve(strict=True), arm.resolve(strict=True))
    output = output.resolve(strict=True)
    if output in sources:
        raise ValueError("The merged app must be separate from the native inputs")
    frameworks = output / FRAMEWORKS
    plugins = [bundled_binary(app / PLUGIN) for app in sources]
    for plugin, arch in zip(plugins, ARCHITECTURES):
        check_architectures(plugin, (arch,))
    if plugin_dependencies(plugins[0], "x86_64") != plugin_dependencies(
        plugins[1], "arm64"
    ):
        raise ValueError(
            "The macOS FFmpeg plugins link different Qt or FFmpeg versions"
        )

    binaries = {output / PLUGIN: plugins}
    aliases = {}
    for stem in LIBRARIES:
        x64, x64_names, x64_identity = native_library(
            sources[0] / FRAMEWORKS, stem, "x86_64"
        )
        arm64, arm64_names, arm64_identity = native_library(
            sources[1] / FRAMEWORKS, stem, "arm64"
        )
        if x64_identity != arm64_identity:
            raise ValueError(
                f"{stem}: mismatched dylib identities: {x64_identity} / {arm64_identity}"
            )
        destination = frameworks / x64.name
        binaries[destination] = (x64, arm64)
        install_name = Path(x64_identity.split(" (", 1)[0]).name
        for name in x64_names | arm64_names | {install_name}:
            if name != destination.name:
                aliases[frameworks / name] = destination.name

    # Stage all replacements first so a missing input or failed merge leaves
    # the output untouched. Replacing paths also avoids writing through symlinks.
    with tempfile.TemporaryDirectory(dir=output.parent) as directory:
        stage = Path(directory)
        for destination, inputs in binaries.items():
            merged = stage / destination.name
            tool("lipo", "-create", *inputs, "-output", merged)
            check_architectures(merged, ARCHITECTURES)
        for alias, target in aliases.items():
            (stage / alias.name).symlink_to(target)
        for path in (*binaries, *aliases):
            (stage / path.name).replace(path)
    verify_ffmpeg(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("intel_app", type=Path)
    parser.add_argument("arm_app", type=Path)
    parser.add_argument("universal_app", type=Path)
    args = parser.parse_args()
    try:
        merge_ffmpeg(args.intel_app, args.arm_app, args.universal_app)
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"{error}\n{error.stderr}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print("Merged the FFmpeg plugin and seven libraries for x86_64 and arm64")


if __name__ == "__main__":
    main()
