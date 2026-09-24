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

## Packages

Built on tag by `release-strata.yml`, `build-macos-qt6.yml`, `windows-qt6.yml`,
`build-appimage.yml` and `sign-release-assets.yml`. Expected assets match 1.4.13:
installer EXE and portable ZIP, `Strata-Installer.dmg`, `*-x86_64.AppImage`,
platform receipts, checksums, Sigstore bundles and `windows-verification.json`.
