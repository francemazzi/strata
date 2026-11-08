# Strata 1.8.2

Strata 1.8.2 gives the assistant its product name, fixes two problems that kept AI assistant tools from running, and aligns Strata with QGIS master as of 7 November 2026. It uses the same Strata Cloud endpoint as 1.8.1, `https://api.getstrata.org`.

## AI assistant

- **Strata Assistant.** The chat panel, its menu action, the Ctrl+Shift+K action, the onboarding bar, the quit prompt and the approval dialogs say "Strata Assistant" instead of "AI Assistant" or "AI:". Saved window layouts still restore the panel, and API keys stored in the system keychain are kept.
- **Strata Cloud jobs in Ask before edits.** In Ask before edits the assistant could not use DataHub extracts (`datahub_extract`), tree detection (`trees_detect`) or the cancellation of a discovery job (`discovery_cancel`). They are now offered, and each call asks for confirmation first, because it starts or stops a job on Strata Cloud; extracts and tree detection use the plan quota. In Agent mode they run without a dialog, as before. The plans that include these tools do not change.
- **Downloads with raw Overpass queries.** Overpass or Nominatim URLs with `[`, `]` or `"` in the query failed with "URL is not valid". `download_file` now percent-encodes the query and the fragment when needed and leaves existing escapes as they are. Scheme, host and path are not changed, and the HTTPS, localhost, credential and destination checks work as before.
- The Strata Auto theme draws menu separators.

## QGIS

Strata now includes QGIS master as of 7 November 2026. Among the changes:

- copying and pasting features between two QGIS instances is improved, and the feature clipboard works on Wayland;
- 3D maps: keyboard control works, picking works with the orthographic projection, and point clouds can be identified, highlighted and edited when the layer and map CRS differ;
- when editing in transaction groups, features are no longer committed if the geometry checks fail;
- key/value and list widgets can be used on text fields;
- vector tile styles: sprites chosen from field values, line marker opacity, interpolated fill translation and geometry-type filters;
- the "New Empty Project" template is back, the welcome screen closes when a new project is created, and Linux sessions with Wayland only start correctly;
- PostgreSQL: browser tooltips of stored projects need a single query, interval values are written correctly, and the extent of TopoGeometry columns is no longer estimated;
- crash fixes in layer properties and in GeoPackage relation items.

## Build

- Dependencies follow the vcpkg baseline of QGIS master (`8450b13`). Windows builds use a dependency SDK rebuilt on that baseline, available until 5 February 2027.
- The vcpkg setup fetches the pcre2 sources with a token, so a GitHub API rate limit no longer stops the macOS dependency build.
- A manual workflow, `strata-candidate-postgis.yml`, runs the PostGIS coordinate reference system tests with the packaged Linux AppImage and Windows zip of a release, each against a disposable PostGIS database. It is one of the release checks.

## Updating

From 1.7.1 or newer, install 1.8.2 over the existing installation. From 1.6.8 or older, follow the 1.7.1 notes: profiles that use the retired Google Cloud Run addresses switch to `https://api.getstrata.org` automatically after the update.

## Validation

- Pull request builds:
  - the QGIS synchronization of 8 November built on macOS (arm64 and x86_64, with the DMG) and Windows. It contains the synchronization of 6 November, whose macOS jobs had stopped while downloading pcre2 before the token fix;
  - the Overpass and Strata Assistant changes built on macOS (arm64 and x86_64) and Windows;
  - the Ask before edits change built on macOS arm64 and Windows.
- Local macOS development build of master with the Ask before edits change (2026-11-08):
  - 37 of 39 AI assistant test targets pass. The two exceptions also fail without these changes: `test_app_aiincidentrecovery::transferTimeoutExplainsTheAbort`, which waits 45 seconds for a network timeout message and needed about half a second more, and `test_app_aidiscoveryworkflow::previewSelectionDownloadImport`, where the local test server receives one more request than expected after a cancellation.
  - A headless start against `https://api.getstrata.org` with a new profile loads the GDAL, OGR, PostgreSQL and SpatiaLite data providers, the GDAL, QGIS, native and PDAL Processing providers and the Strata Assistant panel, with one Window menu and one "Import QGIS Profile" action. No action still says "AI Assistant".

## Release verification

The release candidate is built from the immutable `strata-v1.8.2` tag. Windows, macOS, and Linux packages must be produced from this source revision. Checksums, platform verification records, signing, and the release seal are required before the candidate is published. A draft is not an installer release.
