# Strata 1.5.1

Tag `strata-v1.5.1`. Packages are produced by the same GitHub Actions pipeline as
1.4.13: Windows Authenticode, notarized macOS DMG, Linux AppImage, SHA-256
checksums and Sigstore sealing. The sealed draft stays unpublished until the
signed assets and receipts match this tag.

## Changes

- AI tools no longer freeze the desktop: field edits, add-layer context scans,
  and `run_python` Processing calls run off the GUI with Stop and progress.
- Sidecar files (`.qmd`, `.qml`) are refused as map layers; the error names the
  sibling dataset and explains that `.qml` is a style.
- `run_python` times out after a configurable budget (default 120 s) and hints
  at slow feature loops instead of refusing them.
- Empty HTTP 2xx assistant replies still recover; pre-dispatch configuration
  failures keep the error path.
- Stop also interrupts web search, web fetch, MCP calls, downloads, DataHub
  extraction and tree detection within about a second. A Stop or a new chat
  during a tool no longer leaves the chat locked or cancels the next request.
- Quitting Strata while an AI tool works in the background no longer crashes.
- A Processing algorithm that fails before it starts reports its error to the
  assistant, which can retry, instead of ending the turn as if Stop was pressed.
- `run_python` restores Processing after every run, and its time budget can no
  longer hang a run that is waiting on Processing. Loop hints only appear for
  code that edits features one by one.
- Dropping only sidecar files explains why nothing opened, and plugins still
  receive `.qml` and `.qmd` drops.

## Known limitations

- `add_layer_from_file` and `add_layer_from_service` still create and validate
  the layer on the interface thread, so a large GeoJSON or CSV file, or a slow
  service, can pause the window briefly.
- `calculate_field` and `batch_update_attributes` compute values in the
  background, but writing them into the layer and saving still happen on the
  interface thread.
- `run_python` cannot interrupt a single long C++ call; the time budget applies
  when control returns to Python.

## Packages

Built on tag by `release-strata.yml`, `build-macos-qt6.yml`, `windows-qt6.yml`,
`build-appimage.yml` and `sign-release-assets.yml`. Expected assets match 1.4.13:
installer EXE and portable ZIP, `Strata-Installer.dmg`, `*-x86_64.AppImage`,
platform receipts, checksums, Sigstore bundles and `windows-verification.json`.
