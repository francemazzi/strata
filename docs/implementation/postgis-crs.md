# PostGIS CRS correction

## Contract and implementation

The CRS is valid when `QgsCoordinateReferenceSystem::isValid()` is true. An empty
`authid()` is not evidence that the CRS is absent. Existing JSON `crs` fields keep
their original type and meaning; the project summary's existing CRS object is
enriched in place. Other tool payloads add `crs_details`:

```json
{
  "is_valid": true,
  "authid": "",
  "description": "unknown",
  "status": "valid",
  "label": "Custom CRS (valid)",
  "source_srid": 990051,
  "wkt": "PROJCRS[...]"
}
```

The example WKT is abbreviated for this document only. Actual payloads carry the
complete definition. WKT is included for valid CRSs with no portable identifier,
including USER/QGIS/CUSTOM identifiers. It is CRS metadata, independent of feature
geometry sharing. Labels are sanitized and bounded; data-derived names and
CRS definitions remain untrusted input to the assistant.

`QgsPostgresConn::resolveCrs()` reads authority, authority code, WKT and PROJ from
`spatial_ref_sys` on the same connection. `sridToCrs()` remains available as a
compatibility wrapper. Only successful resolutions enter the per-connection
cache. A per-catalog-operation map avoids repeating failed lookups for each table.
The provider's read-only Qt `crsResolution` property returns the cached diagnostic
snapshot without querying. Layer properties and AI tools consume that snapshot.

`TableProperty::info()["crs_details"]` retains raw SRIDs, geometry columns and
geometry types, including distinct source SRIDs which resolve to equivalent CRSs.
The public `geometryColumnTypes()` list retains its existing deduplication.
Single-table catalog inspection also includes every geometry column. Field URI
reconstruction uses the original SRID, not a resolved EPSG number. Empty/all-null
geometry columns retain catalog metadata when type detection yields no samples.

| Status / diagnostic | Meaning |
| --- | --- |
| `valid` | Resolved CRS; no error diagnostic |
| `missing` / `srid_unspecified` | Source has no usable SRID, including 0 |
| `unresolved` / `definition_missing` | SRID has no catalog row |
| `unresolved` / `permission_denied` | SQLSTATE 42501 reading the CRS catalog |
| `unresolved` / `catalog_unavailable` | Catalog/schema unavailable on this connection |
| `unresolved` / `definition_invalid` | Authority, WKT and PROJ could not resolve |
| `unresolved` / `connection_error` | Connection could not read the definition |
| `unresolved` / `catalog_query_failed` | Other catalog query failure |
| `unresolved` / `local_catalog_unavailable` | Definition failed and local EPSG:4326 resolution also failed |
| `unresolved` / `operation_canceled` | CRS lookup canceled; never cached |
| `not_applicable` | Non-spatial table/layer |

Diagnostics contain fixed, understandable messages and numeric SRIDs, never URI,
credentials or raw SQL errors. The existing missing-CRS indicator stays active.
An unresolved PostGIS layer bypasses the global automatic/prompt CRS-validation
preference, remains valid as a layer and is not assigned a guessed CRS. Explicit
user assignment remains possible. No database migration or permission change is
performed by this feature.

Index snapshots are captured in the layer thread. Workers receive only prepared
metadata and (where allowed) a feature source. PostGIS, including localhost,
continues to have no automatic feature source, count or extent scan. The
`layer-chunks-3` fingerprint includes normalized preferred WKT, so changing
between custom CRSs with empty identifiers replaces stale chunks. The existing
coordinator handles loaded layers without clearing unrelated index entries.

## Automated checks and reproduction

Use a disposable PostGIS database exclusively for this suite. The Python test
intentionally renames `spatial_ref_sys` and changes its grants temporarily. Never
point `STRATA_CRS_TEST_DB` at customer, development or shared databases. The test
uses synthetic data and restores catalog state in cleanup; destroy the disposable
container after use. Run C++ and Python PostGIS tests sequentially.

Example disposable fixture:

```sh
docker run --rm -d --name strata-crs-test \
  -p 127.0.0.1:55011:5432 --tmpfs /var/lib/postgresql/data \
  -e POSTGRES_DB=crs_test -e POSTGRES_HOST_AUTH_METHOD=trust \
  postgis/postgis:16-3.4-alpine
export STRATA_CRS_TEST_DB='host=127.0.0.1 port=55011 dbname=crs_test user=postgres sslmode=disable'
```

After configuring the normal Qt6/PyQGIS build, build `qgis_desktop`,
`provider_postgres`, `test_app_aicrsmetadata`, `test_app_ailayerchunker`,
`test_app_ailayerindexcoordinator`, `test_app_aiagentsessionmanager`,
`test_app_aitoolregistry`, `test_app_aidatabasetools`, `test_app_aiprojecttools`.
Run these binaries with `QT_QPA_PLATFORM=offscreen` and an isolated
`QGIS_CUSTOM_CONFIG_PATH`. Run `tests/src/python/test_provider_postgres_crs.py -v`
with that build's Python bindings and runtime libraries. The new tests skip when
`STRATA_CRS_TEST_DB` is absent. Existing database-tool integration cases additionally
use `QGIS_PGTEST_DB` (same disposable URI) and `PGUSER=postgres` for saved connections.

The tests verify actual tool payloads and the prompt received by a fake provider,
not model-generated assertions. Coverage includes empty/local identifiers,
legacy field types, custom WKT, layer-vs-view CRS, index replacement and pause,
100 equivalent custom source SRIDs exceeding the tool output cap, a locked catalog
canceled in flight, explicit Browser-style SRID URIs, multiple geometry columns,
missing/invalid definitions, catalog denial/unavailability and subsequent recovery.

## Local validation, 2026-10-06

The modified source was compiled on macOS arm64 with Qt 6.11 and Python 3.14.6.
The local OpenEXR/runtime dependency issue was resolved through the build
process's library search path, without changing system symlinks or release
artifacts. This is a development build, not the existing 1.6.2 package.

- Seven C++ suites pass: CRS metadata (8), chunker (12), index coordinator (16),
  agent session manager (78), tool registry (39), database tools (20), project
  tools (8). Qt counts include suite initialization/cleanup.
- Five PyQGIS/PostGIS test methods pass, including 16 standard/custom/remapped
  SRID cases and project round trips. Standard Italian cases include Monte Mario,
  ETRS89 and RDN2008.
- Desktop runtime smoke verifies nine EPSG definitions with validity, identifier
  and WKT plus a custom CRS/project round trip. The receipt explicitly records
  `runtime_mode: development`.
- GUI Browser loading verified custom CRS (local SRID 990091), Monte Mario
  (EPSG:3003) and unresolved SRID 0. The latter remains a valid layer; its
  Information page displays the source SRID and diagnostic and its attribute
  table opens with the synthetic record. Saving/reopening preserves CRS state.
- The final GUI chat interaction with a loopback-only simulated provider is
  pending because the Mac session was locked. The automated fake-provider
  context/tool checks passed; no GUI chat answer is claimed.

See `postgis-crs-evidence.json` alongside this document for compact receipts.

## Package and customer acceptance still required

The package workflows now test CRS validity, identifier and WKT explicitly.
macOS/Linux run the packaged desktop through `scripts/ci/verify_crs_runtime.py`
with an isolated profile and host data/library overrides removed. macOS/Linux
retain the JSON receipt with workflow artifacts. Windows uses the expanded
`scripts/ci/test-windows-runtime.ps1`. `--development-runtime` is only for developer
builds and explicitly cannot validate a self-contained package.

Before distribution, run the PostGIS matrix and the GUI Browser → properties →
assistant → save/reopen flow on freshly built Windows, macOS and Linux candidates
from the correction's commit. Record OS, artifact hash, source commit and results.
The package catalog smoke alone does not validate a PostGIS connection. No
three-platform candidate acceptance, signing, publication or rollout is claimed
by the local evidence. Do not use the pre-existing 1.6.2 package as evidence for
these changes.

Closing the original customer incident additionally requires their Strata
version/platform, source SRID, connection characteristics and the affected layer.
No customer data has been used in this regression suite.
