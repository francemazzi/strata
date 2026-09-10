# Strata 1.4.4

This hotfix addresses Windows application-control blocks while loading bundled
libraries such as `OpenCL.dll` (error `0xc0e90002`). It is based on the previously
released application, with changes limited to signing, packaging and release handling.

Windows installers, their uninstaller and bundled executable components require
valid Authenticode signatures. The portable ZIP contains the same verified runtime
payload. Installer names display Strata 1.4.4; the underlying QGIS version and user
project formats remain unchanged. Existing profiles and projects are preserved.

Downloads include Windows x64 EXE/ZIP, the signed and notarized universal macOS DMG,
and Linux x86_64 AppImage. SHA-256 checksums and Sigstore bundles accompany the final
artifacts. The release manifest binds the files to the source commit, and the
Windows verification and acceptance reports describe the tested package hashes.

New files may still show SmartScreen reputation warnings. Organization-specific
application-control rules can require IT authorization. No Windows security
exclusions are required by the supported acceptance procedure.
