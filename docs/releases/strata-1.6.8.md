# Strata 1.6.8

Strata 1.6.8 is the 1.6.7 release with a Windows package. The `strata-v1.6.7` tag produced the macOS DMG and the Linux AppImage, but its Windows build failed while signing the installer: the federated Azure access token from the start of the job expired after 730 signed files. No application source changed.

## Changes

- Windows release signing: the workflows clear the Azure CLI token cache before the refresh login that precedes packaging, and `scripts/ci/windows-signing-files.ps1` renews the federated login with a fresh GitHub OIDC token every 40 minutes and once more after a SignTool failure. See `docs/releases/windows-signing.md`, "Token lifetime during packaging".
- Everything in 1.6.7: the Windows vcpkg triplet provides a Fortran compiler for `lapack-reference`, plus the 1.6.6 and 1.6.5 desktop work (distinct managed AI error messages, simplified chat actions and workspace cloud transfers, PostGIS CRS resolution of legacy `spatial_ref_sys` definitions with PROJ database diagnostics and bundled `PROJ_DATA` on Windows).

## Validation

- Signing policy tests: `scripts/ci/test-windows-signing-files.ps1` passes 16 scenarios with PowerShell 7 on macOS, including the renewed login after a SignTool failure, the scheduled renewal, and no renewal without the OIDC request variables.
- PostGIS CRS definitions (macOS development build, 2026-10-08): `test_provider_postgrescrsdefinition` 10 of 10, `PyQgsPostgresCrs` 6 of 6 against PostGIS 3.4.3, `test_app_aicrsmetadata` 8 of 8.
- The Windows fixes are validated by the release build of this tag: a successful `Windows Qt6` run on `strata-v1.6.8` whose package step signs the full installer and uploads `strata-sdk-x64-windows`.

## Downloads and remaining acceptance

Application source tag: `strata-v1.6.8`. Platform packages (macOS universal DMG, Windows x64 installer and portable ZIP, Linux x86_64 AppImage), verification records, SHA-256 checksums and Sigstore bundles are produced by the existing workflows from this tag and sealed with the platform-only seal, as for 1.6.3. The release stays a draft until the seal passes.

The same gates as 1.6.3 apply: Windows signing and runtime checks, macOS code-signature, Gatekeeper and notarization checks, plus the manual acceptance still pending (Windows 11 Smart App Control field acceptance, in-app updater acceptance, interactive GUI check of the final macOS DMG). The customer confirmation of the PostGIS CRS incident on the Windows installer of this release is the next field check.
