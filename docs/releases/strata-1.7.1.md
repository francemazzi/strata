# Strata 1.7.1

Strata Cloud now uses `https://api.getstrata.org`. This release updates the desktop default and automatically resolves saved profiles using the two retired production Google Cloud Run addresses to the new endpoint. Custom backend URLs and explicit `STRATA_PLAN_ENDPOINT` overrides remain supported. The production launcher and website feedback form also use the new API origin.

## Updating an existing installation

After installing 1.7.1, restart Strata. Profiles using the former production endpoint switch automatically. Your account, password, projects, and managed session are unchanged.

For an older desktop version, open the assistant settings, then **Account → Advanced → Backend endpoint**, enter:

```text
https://api.getstrata.org/ai/messages
```

Save the settings and restart Strata. Sign in again with your existing account only if requested. The previous Google `run.app` URLs are retired.

The web console remains at https://app.getstrata.org and drawing conversion at https://dwg.getstrata.org.

## Why 1.7.1 replaces 1.7.0

1.7.0 was never published. Its Linux AppImage did not build, because QGIS code merged in the upstream sync used a curve type that only exists from GEOS 3.15, while the AppImage builds against the GEOS of Ubuntu 24.04. 1.7.1 is 1.7.0 plus that build fix, so all three platforms come from one source revision as the release verification requires.

## Other changes since 1.6.8

This version includes the changes already merged into `master`: authenticated remote desktop runner support, the QGIS upstream synchronization, and the updated dependency baselines and build compatibility fixes. Remote execution and customer acceptance remain subject to the documented product gates; this release does not certify workflows that have not been validated.

## Release verification

The release candidate is built from the immutable `strata-v1.7.1` tag. Windows, macOS, and Linux packages must be produced from this source revision. Checksums, platform verification records, signing, and the release seal are required before the candidate is published. A draft is not an installer release.
