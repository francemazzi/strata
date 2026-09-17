# Strata 1.4.5

This hotfix is the 1.4.4 Windows signing candidate with a CI-only correction:
Azure Artifact Signing metadata is written as UTF-8 without a BOM. Windows
PowerShell 5.1 was emitting `0xEF`, which `Azure.CodeSigning.Dlib` rejects
before any Authenticode signature can be produced.

The application, QGIS version, installer identity and user project formats
are unchanged from 1.4.4. Existing profiles and projects are preserved.

Downloads include Windows x64 EXE/ZIP with Public Trust Authenticode
signatures, the signed and notarized universal macOS DMG, and Linux x86_64
AppImage. SHA-256 checksums and Sigstore bundles accompany the final
artifacts. The release manifest binds the files to the source commit.

New files may still show SmartScreen reputation warnings. Organization-specific
application-control rules can require IT authorization. No Windows security
exclusions are required by the supported acceptance procedure.
