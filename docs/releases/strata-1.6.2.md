# Strata 1.6.2

Strata 1.6.2 improves recovery when the managed AI provider temporarily refuses
requests, and validates the geographic position of ArcGIS raster snapshots.
Install this desktop update to enable the new recovery behavior; the compatible
backend changes are already deployed.

## Changes

- Temporary managed-provider budget contention is distinguished from a user's
  Strata credit balance. Recovery stays on the selected model, respects the
  provider's wait, and allows up to three retries within ten minutes.
- Automatic retry is limited to requests that produced no model content or tool
  calls. Completed tools are preserved. Changes to account, project, chat, mode
  or model cancel pending recovery.
- Interrupted chats retain a local checkpoint. After restart, Resume verifies
  the account, project and affected resources before continuing. Deleting chat
  history also deletes its recovery checkpoint.
- Tool execution, applied changes and geographic verification have separate
  outcomes. Resume rechecks failed resource checks and records corrections
  without rewriting the original history or replaying mutations.
- Discovery resolves identity through the authenticated account endpoint and
  supports the normal opaque desktop token.
- ArcGIS MapServer layers support live mode and explicit GeoTIFF snapshots.
  Snapshots use the dimensions, extent and CRS actually returned by the service,
  preserve transparency, and are reopened and checked before use.
- CRS and source changes refresh project suggestions; obsolete asynchronous
  findings are discarded.

## Validation and distribution

The code passed 224 targeted Qt tests (four live-provider cases remained behind
their existing opt-in guards), complete macOS Intel/Apple Silicon builds and a
Windows build before tagging. Release packages and their platform receipts must
all match the immutable `strata-v1.6.2` source commit.

Windows package verification is distinct from manual Windows 11 Smart App
Control acceptance. Actual recovery of an existing GIS project also requires
checking that project's resources; passing automated tests does not certify a
previously misreferenced raster or an unavailable project.
