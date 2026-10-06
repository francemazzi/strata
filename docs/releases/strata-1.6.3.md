# Strata 1.6.3

Strata now recognizes valid custom coordinate reference systems even when they have no EPSG identifier. Layer properties, the PostGIS catalog and the assistant describe the same CRS and distinguish it from the map view CRS.

## Changes

- PostGIS SRIDs are resolved through the connected database's `spatial_ref_sys`, including local SRIDs mapped to EPSG/ESRI or defined by WKT/PROJ. Original SRIDs and every geometry/SRID association are preserved.
- Missing or unreadable CRS definitions have specific, credential-free diagnostics. Affected layers remain available without silently assigning another CRS. Reopening retries resolution after metadata or permissions are corrected.
- Valid custom CRS definitions are included in assistant metadata and retained when projects are saved and reopened. A custom project CRS is labeled “Custom CRS” in the status bar.
- Remote layer indexing includes existing CRS, geometry-type and field metadata without automatically reading geometries. Existing chunks refresh through the indexing coordinator, respecting pause and disable settings.
- Corrects a non-Windows build configuration mismatch that could corrupt memory when creating the main application window. The executable and AI library now use the same class layout.
- Completes the Linux Python runtime with compatible GDAL/NumPy bindings. macOS bundle metadata reports the Strata version and normal Finder launches preserve signed Python resources.

## Validation

Seven targeted C++ suites passed, covering metadata, assistant context/tool payloads, database tools and indexing. Synthetic PostGIS tests cover standard Italian CRS, local EPSG/ESRI mappings, custom WKT and PROJ definitions, equivalent SRIDs, multiple geometry columns, unresolved definitions, permission/catalog recovery and project round trips.

A GUI check on the modified development build verified Browser loading, layer properties, attribute access and project reopening. A loopback AI provider received and checked the actual tool payloads; its final answer was displayed in the chat. No external model or customer data was used for that check.

The exact Windows portable package, Linux AppImage and macOS universal DMG passed all five PostGIS regression methods (zero failures, errors or skips), with macOS tested as both arm64 and x86_64. Windows installer/portable signing and runtime checks passed. The macOS app and DMG passed code-signature and Gatekeeper checks, Apple notarization was accepted, and the DMG ticket was validated. CRS checks explicitly verify validity, identifiers and WKT.

The final Linux candidate also passed a native Memcheck regression for the main-window constructor: the earlier out-of-bounds writes are absent. Its GDAL/NumPy array round trip passed, and completing the Python runtime retained the compiled application executable's exact hash.

## Downloads and remaining acceptance

Application source tag: `strata-v1.6.3` (`01ee92498993e2d8e3e0b375262d5f66f56a3a51`). Linux/macOS use the reviewed build recipe at `2df694fc5a7422201ade0d917fd5e1307e00b12c`, which propagates the AI compile definition to the executable. Its explicit `candidate-build-configuration.patch` is retained in the workflow artifacts; build receipts record both the application source and workflow revision. The tag is unchanged; reproducing these packages requires that build recipe. Linux runtime packaging uses `ec643ea759f78c740fcb22b6b6780a5ae79937e3`; macOS final packaging uses `a4a7e0638f8dcce322855d59a9dbccc43d0fbf80`. The sealed platform receipts record these recipes and their CI runs separately from the application source tag.
This distribution follows the existing platform package and Sigstore sealing workflow: macOS universal DMG, Windows x64 installer and portable ZIP, Linux x86_64 AppImage, platform verification records and SHA-256 checksums.

Manual Windows 11 Smart App Control field acceptance remains pending, as in 1.6.2. An additional interactive GUI check of the final macOS DMG is pending because the verification workstation requires manual unlock; the modified-source GUI and packaged headless checks above passed. The original customer's report also remains subject to verification with their layer, Strata version, platform and SRID. Automated and synthetic tests do not substitute for either acceptance check.

Install 1.6.3 from these downloads; an in-app updater feed is not included in this publication. The CRS fixes require the desktop update. The unchanged compatible cloud backend was redeployed and passed production health/readiness checks; this release does not migrate customer databases or change their permissions.
