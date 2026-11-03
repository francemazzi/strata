# Strata 1.8.0

Strata 1.8.0 gives the AI assistant new tools for reports and print layouts and aligns Strata with QGIS master as of 3 November 2026. It uses the same Strata Cloud endpoint as 1.7.1, `https://api.getstrata.org`.

## AI assistant

- **Reports from earlier chats.** The assistant can list the other chats of the current project and read them turn by turn: prompts, replies and tool arguments in full, long results as previews it can open in detail. The tool is read-only and the reviewer can use it too. With your own provider keys it is available right away; on Strata Cloud it becomes available when the service update for this release is rolled out.
- **Markdown to PDF.** `export_map` turns a markdown file of the workspace into an A4 PDF, with images scaled to the page, tables with a grid and long code lines wrapped. The result lists missing images and the page count.
- **Print layouts that stay readable.**
  - New layouts take a page size and orientation, keep the map in its frame with the current map extent, and can frame a whole layer instead of the current view.
  - Legend, scale bar and north arrow go in a side panel; the legend shows only map layers, with readable names, and a footer note can be added. Later edits update the panel in place.
  - An attribute table can be added to the panel, with chosen columns or expressions, a filter, sorting and a row limit. Columns and filter are checked before the layout changes, and the result says whether all rows fit.
  - The assistant is told which items fall outside the page or overlap, and undo restores the previous layout.
- **Exports.** `export_map` can replace an existing file, keeping a copy so undo restores it, and shows the assistant an image of the first page when image sharing is allowed.
- **Processing on part of a layer.** Processing inputs accept the selected features only, a filter expression or a feature limit, like the option in the Processing Toolbox.
- **Graduated styles.** The classification method follows QGIS (Jenks by default), with color ramp, inverted ramp and label precision; every class is returned with its feature count. Tables without geometry give an error instead of a crash.
- **Web pages.** The assistant knows that a fetched page can lose its download links and saves the page HTML to find them.

## QGIS

Strata now includes QGIS master as of 3 November 2026, together with the compatibility fixes that let the Linux AppImage build against the Qt 6.4 and SIP of Ubuntu 24.04.

## Build

The CI pins the vcpkg tool to the 2026-10-29 release, so a new vcpkg release no longer empties the macOS dependency cache.

## Updating

From 1.7.1, install 1.8.0 over the existing installation. From 1.6.8 or older, follow the 1.7.1 notes: profiles that use the retired Google Cloud Run addresses switch to `https://api.getstrata.org` automatically after the update.

## Validation

- Pull request builds: the QGIS synchronization built on macOS (arm64 and x86_64), Windows and Linux AppImage; the AI assistant changes built on macOS and Windows (the x86_64 macOS job compiled and failed only while uploading its artifact, on a GitHub network timeout). Master built on Windows and macOS after the merges.
- Local macOS development build of master (2026-11-03):
  - 39 of 40 AI assistant and application test targets pass. The exception is `test_app_aiincidentrecovery::transferTimeoutExplainsTheAbort`, which checks the wording of a network timeout message and was already failing before this release.
  - A headless start against `https://api.getstrata.org` with a new profile loads the GDAL, QGIS and native Processing providers and the AI assistant panel, with one Window menu and one "Import QGIS Profile" action.

## Release verification

The release candidate is built from the immutable `strata-v1.8.0` tag. Windows, macOS, and Linux packages must be produced from this source revision. Checksums, platform verification records, signing, and the release seal are required before the candidate is published. A draft is not an installer release.
