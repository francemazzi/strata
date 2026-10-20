# Strata 1.6.7

Strata 1.6.7 is the 1.6.6 desktop release with a working Windows build. It is the first tag since 1.6.4 to ship platform packages: 1.6.5 and 1.6.6 never produced any, because GitHub had suspended the fork's workflows, and the Windows build then failed once the prebuilt dependency SDK had expired.

## Changes

- Windows dependency build: `vcpkg/triplets/x64-windows-release.cmake` now sets `VCPKG_PROVIDED_FORTRAN ON`, as upstream QGIS does since 2026-08-10 (qgis/QGIS#67012). Since July 2026 vcpkg's `vcpkg_find_fortran` uses its bundled MinGW gfortran only when the triplet asks for it; without the flag `lapack-reference`, required by `py-numpy`, fails to configure with "No CMAKE_Fortran_COMPILER could be found". The prebuilt `strata-sdk-x64-windows` artifact that masked this expired after the 1.6.4 build.
- Everything in 1.6.6 and 1.6.5: distinct managed AI error messages, simplified desktop chat actions and workspace cloud transfers, and the PostGIS CRS resolution fix (legacy `spatial_ref_sys` definitions with an embedded TOWGS84 shift resolve to their declared EPSG code, PROJ database diagnostics in layer properties and assistant payloads, bundled `PROJ_DATA` preferred on Windows). See `strata-1.6.5.md` and `strata-1.6.6.md`.

## Validation

- PostGIS CRS definitions (macOS development build, 2026-10-08): `test_provider_postgrescrsdefinition` 10 of 10, `PyQgsPostgresCrs` 6 of 6 against a disposable PostGIS 3.4.3 container, `test_app_aicrsmetadata` 8 of 8.
- The 1.6.6 incident-recovery checks (`tests/src/app/testqgsaiincidentrecovery.cpp`) were still not executed in a local build before this tag.
- The Windows fix is validated by the release build of this tag itself: a successful `Windows Qt6` run on `strata-v1.6.7`, with `lapack-reference` built from source and a fresh `strata-sdk-x64-windows` artifact uploaded by that run.

## Downloads and remaining acceptance

Application source tag: `strata-v1.6.7`. Platform packages (macOS universal DMG, Windows x64 installer and portable ZIP, Linux x86_64 AppImage), verification records, SHA-256 checksums and Sigstore bundles are produced by the existing workflows from this tag and sealed with the platform-only seal, as for 1.6.3. The release stays a draft until the seal passes.

The same gates as 1.6.3 apply: Windows signing and runtime checks, macOS code-signature, Gatekeeper and notarization checks, plus the manual acceptance still pending (Windows 11 Smart App Control field acceptance, in-app updater acceptance, interactive GUI check of the final macOS DMG). The customer confirmation of the PostGIS CRS incident on the Windows installer of this release is the next field check.
