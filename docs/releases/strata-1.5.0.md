# Strata 1.5.0

Tag `strata-v1.5.0`. Packages are produced by the same GitHub Actions pipeline as
1.4.13: Windows Authenticode, notarized macOS DMG, Linux AppImage, SHA-256
checksums and Sigstore sealing. The sealed draft stays unpublished until the
signed assets and receipts match this tag.

## Changes

- Plan Account treats an HTTP 200 with neither text nor tool calls as a failure
  and retries with backoff, honoring `retry_after` when the gateway sends it.
- Dropped shapefile sidecars (`.prj`, `.cpg`, `.shx`, …) are skipped instead of
  being opened as layers.
- Processing algorithm outputs are loaded into the project like the QGIS Toolbox.
- After a tool run, the assistant narrates what happened instead of leaving an
  empty turn.

The matching `strata-be` change writes an SSE error event when OpenRouter fails
after the gateway has already sent HTTP 200, so the desktop can surface the
upstream limit instead of a blank reply.

## Packages

Built on tag by `release-strata.yml`, `build-macos-qt6.yml`, `windows-qt6.yml`,
`build-appimage.yml` and `sign-release-assets.yml`. Expected assets match 1.4.13:
installer EXE and portable ZIP, `Strata-Installer.dmg`, `*-x86_64.AppImage`,
platform receipts, checksums, Sigstore bundles and `windows-verification.json`.
