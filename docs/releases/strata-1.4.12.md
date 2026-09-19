# Strata 1.4.12

Strata 1.4.12 is the complete desktop release candidate following 1.4.3. It
includes the product improvements prepared for 1.4.9 and the subsequent Windows
release-pipeline corrections. Tags 1.4.4 through 1.4.11 remain unpublished draft
candidates and are not supported downloads.

## Highlights

- **Claude subscription via Claude Code**: connect a Claude Pro or Max
  subscription from AI settings or the chat model menu. Strata stores the setup
  token locally and uses the Claude Code CLI for chat sessions.
- **Windows x64 Public Trust signing**: Azure Artifact Signing covers the
  installer, uninstaller, portable package and bundled executable payload. The
  packaging callback now signs NSIS's temporary uninstaller safely even when
  tool paths contain spaces.
- **Reliable cross-platform builds**: internal namespace collisions under Unity
  Build are resolved, release uploads retry transient GitHub service failures,
  and the QGIS test matrix uses an immutable MinIO image.
- **Verifiable release assets**: every platform publishes a receipt tied to the
  source commit. The final manifest, SHA-256 checksums and Sigstore bundles are
  generated only after all platform receipts agree.

Application data, existing profiles, projects and the installer identity are
preserved. The QGIS project format is unchanged from 1.4.3.

## Downloads and release gate

The completed release contains Windows x64 EXE and portable ZIP, a signed and
notarized universal macOS DMG, and a Linux x86_64 AppImage. It remains a draft
until the exact Windows EXE and ZIP hashes pass the documented Windows 11 x64
Smart App Control acceptance procedure, including confirmation on the affected
colleague's PC.

Newly signed files can still show a SmartScreen reputation warning, and managed
organizations can impose additional application-control policy. The supported
procedure does not disable Windows security or require local exclusions.
