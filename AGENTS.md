# AGENTS.md

## Upstream QGIS contribution policy

Strata is an independent fork of QGIS. The QGIS project does not accept LLM-, AI- or
agent-assisted contributions to its repositories
([QEP 408](https://github.com/qgis/QGIS-Enhancement-Proposals/blob/master/qep-408-ai-tool-policy.md)).
Upstream ships that policy as its own root `AGENTS.md`; Strata keeps this file instead,
so the policy is restated here:

- Never open pull requests, issues, comments, reviews or any other interaction on
  `qgis/QGIS` or other QGIS project repositories, and never prepare patches meant to be
  submitted there.
- Merging upstream QGIS into Strata (`scripts/ci/sync-qgis-upstream.sh`) is maintenance
  of this fork, not a contribution to QGIS.
- When an upstream sync conflicts on `AGENTS.md`, keep this file and update this section
  if the upstream policy changed.

## Cursor Cloud specific instructions

Strata is an AI-native fork of **QGIS**: the QGIS desktop app (rebranded `Strata`)
plus an integrated AI assistant under `src/app/ai/`. It is a large C++20 / Qt6 /
CMake project with PyQGIS (SIP) Python bindings. Build-from-source notes live in
`INSTALL.md`; product/AI notes live in `README.md` and `strata_cursor_roadmap.md`.

The dependency-refresh update script only reinstalls `requirements.txt`. Everything
below (system build deps via `apt`, the CMake build, staged Python plugins) is a
one-time setup captured in the VM snapshot — it is **not** re-run automatically, so
re-do it manually if you start from a clean machine. Full `apt` build-dep list is in
`INSTALL.md` (section 3.3) / `.docker/qgis3-ubuntu-qt6-build-deps.dockerfile`; plus
`libspatialindex-dev` which those omit.

### Build / run / test / lint (Linux)

The repo's `scripts/*-strata-*.sh` launchers are **macOS-only** (they expect a
`.app` bundle and run `patch-macos-bundle.sh`); do not use them on Linux. Instead:

- Configure (build dir `build/`, from repo root). Force GCC — see caveat below:
  ```
  cmake -S . -B build -GNinja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DUSE_CCACHE=ON \
    -DWITH_DESKTOP=ON -DWITH_GUI=ON -DWITH_ANALYSIS=ON -DWITH_PYTHON=ON \
    -DWITH_BINDINGS=ON -DENABLE_AI_ASSISTANT=ON -DENABLE_TESTS=ON \
    -DWITH_3D=OFF -DWITH_PDAL=OFF -DWITH_SERVER=OFF
  ```
  (3D/PDAL/Server/Oracle/HANA are disabled to keep the cold build tractable on
  ~4 cores; the cold build compiles ~3576 targets and takes a long time. ccache
  speeds up rebuilds.)
- Build the app: `cmake --build build --target qgis_desktop -j"$(nproc)"`.
  Building only `qgis_desktop` skips Python package staging, so also run:
  `ninja -C build pyutils python/staged-plugins python/console/pyconsole \
   python/pyplugin_installer/pyplugin-installer python/testing/pytesting`.
  Without `pyutils` the `qgis`/`qgis.utils` package is missing and the Processing
  framework / console plugin fail to import.
- Run the binary directly (it reads `output/bin/qgisbuildpath.txt`; no install needed):
  ```
  QGIS_PREFIX_PATH=$PWD/build/output \
  LD_LIBRARY_PATH=$PWD/build/output/lib \
  PYTHONPATH=$PWD/build/output/python \
  build/output/bin/Strata
  ```
  Headless PyQGIS scripts work with `QT_QPA_PLATFORM=offscreen` and the same 3 env vars.
- Tests: `cd build && QT_QPA_PLATFORM=offscreen ctest -R <regex> --output-on-failure`.
  Most C++ test binaries are only built by the default `all` target; the `PyQgs*`
  Python tests run against the built bindings without extra compilation.
- Python lint: `python3 -m ruff check python/` (config `.ruff.toml`; note one
  pre-existing unsorted-import finding in `python/plugins/MetaSearch/plugin.py`).
  Full hooks are in `.pre-commit-config.yaml` (ruff + clang-format + local checks).

### Non-obvious caveats

- **Compiler**: `/usr/bin/cc` and `/usr/bin/c++` resolve to Clang whose libstdc++
  link is broken here (`cannot find -lstdc++`). Always configure with
  `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`.
- **numpy ABI (critical)**: the system GDAL Python bindings (`osgeo`) are built for
  apt `python3-numpy` 1.26. A pip-installed `numpy` 2.x under
  `/usr/local/lib/python3.12/dist-packages` shadows it, giving
  `_ARRAY_API not found` and — when Python plugins autoload in the GUI — heap
  corruption crashes (`corrupted double-linked list` / `malloc(): unaligned tcache
  chunk detected`) on startup. Fix: `sudo pip3 uninstall -y --break-system-packages
  numpy` so apt numpy 1.26 is used (`from osgeo import gdal_array` must import
  cleanly). Do NOT `pip install numpy` (unpinned) into `/usr/local` again.
- **GUI display + keyring**: run the GUI on the TigerVNC display `:1` (used by the
  computer-use tool) or under `xvfb-run`. On first launch the AI secret store
  (QtKeychain) shows a GNOME "Choose password for new keyring" dialog. Enter an
  empty password and click **Continue** (Cancel can abort startup); the app
  otherwise falls back to unencrypted local settings.
- **AI assistant backend**: chat/agent features need a reachable LLM backend —
  Strata Cloud (prod Cloud Run is the default endpoint; local `../strata-be`, a
  separate Node.js service not in this repo, for full local E2E) plus a Strata
  Cloud account, or a BYOK provider key (OpenAI/Anthropic/OpenRouter/Codex/Claude).
  None of this is needed for core GIS work (loading layers, rendering, Processing).
