# Strata 1.5.0

Local-built release. GitHub Actions minutes are exhausted, so packages are
produced on the developer machine and uploaded to GitHub. They are not the
CI-sealed 1.4.13 set (no Azure Authenticode, no Linux AppImage builder, no
GitHub OIDC Sigstore identity).

## Changes

- Plan Account treats an HTTP 200 with neither text nor tool calls as a failure
  and retries with backoff, honoring `retry_after` when the gateway sends it.
- Dropped shapefile sidecars (`.prj`, `.cpg`, `.shx`, …) are skipped instead of
  being opened as layers.
- Processing algorithm outputs are loaded into the project like the QGIS Toolbox.
- After a tool run, the assistant narrates what happened instead of leaving an
  empty turn.

The matching `strata-be` change writes an SSE error event when OpenRouter fails
after the gateway has already sent HTTP 200, so the desktop can surface the
upstream limit instead of a blank reply.

## Packages

Build locally with `-DSTRATA_VERSION=1.5.0`. Do not dispatch the tag-triggered
GitHub Actions workflows. Attach SHA-256 checksums next to each uploaded file.
Windows EXE/ZIP and the Linux AppImage require the CI runners used for 1.4.13
and are omitted until those runners are available again.
