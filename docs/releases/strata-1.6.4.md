# Strata 1.6.4

The map-context chip above the assistant prompt now reads correctly: its text is struck through only while the map context is left out of the next message.

## Changes

- PostGIS layers whose `spatial_ref_sys` row does not resolve through its declared EPSG/ESRI authority no longer appear as a custom CRS when the stored WKT declares one. PostGIS ships EPSG:3003 (and other rows) as WKT with an embedded TOWGS84 datum shift; PROJ reads it as a BoundCRS that QGIS never matches to a code. The provider now uses the authority declared by the definition (`definition: legacy_bound` in the layer's CRS snapshot) and resolves declared authorities through QGIS's own catalogue instead of GDAL, which needs PROJ's `proj.db` and has no fallback. Layer properties and the assistant's `crs_details` report when PROJ cannot open its database, and on Windows the application now points `PROJ_DATA` at its bundled `share/proj`, so a machine-wide `PROJ_LIB`/`PROJ_DATA` left by other installers (PostGIS bundle) no longer breaks CRS resolution. Decision recorded in `docs/implementation/postgis-crs.md`: the embedded Helmert shift is not used for datum transformations. The customer confirmation of the original incident is still pending.
- The chip ("<active layer> · 1:<scale>") showed its text struck through even while the map view, the active layer and its selection were being sent. The context itself was always sent as the tooltip said; only the display was wrong. Qt style sheets resolve font properties such as `text-decoration` once, without the `:checked` state, so the `:!checked` rule applied permanently. The strikeout is now set on the chip font and follows the chip state.

## Validation

PostGIS CRS definitions (macOS development build, 2026-10-08): the new `test_provider_postgrescrsdefinition` suite passes (10 checks: declared authority, lowercase authority, legacy WKT without authority mapped to EPSG:3003, bound CRS kept by the core WKT path, PROJ-string definition, modern WKT, custom definition kept custom, invalid definition, PROJ database probe). `PyQgsPostgresCrs` passes its 6 methods against a disposable PostGIS 3.4.3 container, including the new legacy-row method (rows with NULL authority and with EPSG declared, both resolving to EPSG:3003). `test_app_aicrsmetadata` passes 8 of 8 with the same database. No Windows run of the `PROJ_DATA` startup change yet; it follows the existing macOS branch.

A macOS development build (RelWithDebInfo, Qt 6.11.1) of the application and of `test_app_aichatdockwidget` compiled. The dock suite passed, including a new check that the chip font is struck out only after the chip is clicked off and restored after the next click.

No packaged build has been verified yet for this version.

## Downloads and remaining acceptance

Application source tag: `strata-v1.6.4`. This is a draft: the platform packages (macOS universal DMG, Windows x64 installer and portable ZIP, Linux x86_64 AppImage), their verification records and SHA-256 checksums are produced and sealed by the existing workflows from the tag.

Before publication the same gates as 1.6.3 apply: platform regression methods on the exact packages, Windows signing and runtime checks, macOS code-signature, Gatekeeper and notarization checks, plus the manual acceptance checks still pending from 1.6.3 (Windows 11 Smart App Control field acceptance, interactive GUI check of the final macOS DMG). The cloud backend is unchanged.
