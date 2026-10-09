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

`crsFromCatalogDefinition()` interprets the row without the connection, in this
order: the declared EPSG/ESRI authority through `createFromOgcWmsCrs()` (PROJ
database, then the bundled `srs.db`), the WKT through `createFromWkt()`, then the
PROJ string. `createFromUserInput()` is only a second attempt, because it goes
through GDAL, needs `proj.db` and has no fallback. The snapshot records how the
row was read in `definition`:

| `definition` | Meaning |
| --- | --- |
| `authority` | Declared EPSG/ESRI code resolved |
| `wkt` | WKT definition, identified by PROJ when possible |
| `proj` | PROJ string definition |
| `legacy_bound` | WKT with an embedded TOWGS84 datum shift, as PostGIS ships for EPSG:3003 (3.4 included). PROJ reads it as a BoundCRS that the WKT path never identifies; the authority declared by the definition itself is used and recorded in `definition_authid` |

When the declared authority of a bound definition cannot be loaded, the bound CRS is
kept and `identified_authid` records the equivalent code. `proj_database` is added
whenever PROJ cannot open its own database; `projDatabaseStatus()` probes it once
per process, because `fromEpsgId()` succeeds through `srs.db` even then.

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
| `unresolved` / `local_catalog_unavailable` | Definition failed and PROJ cannot open its database; `proj_database` carries the PROJ error |
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
- The GUI chat interaction passed with a loopback-only simulated provider. The
  actual `list_project_layers` payload contained custom WKT/source SRID 990091,
  EPSG:3003 and the SRID 0 diagnostic; the checked response was visibly rendered.
  This is development GUI evidence, distinct from package acceptance.

The frozen `postgis-crs-evidence.json` records the initial development run; its pending statuses are historical. The final candidate results and build recipes are recorded in `strata-1.6.3-package-evidence.json`.

## Startup correction found during candidate acceptance

The Linux desktop runtime check exposed an existing ABI mismatch. The AI feature
compile definition was private to `qgis_app`, while the separately compiled
`main.cpp` allocates `QgisApp`, whose header conditionally contains AI members.
The local compiler measured 3,976 bytes without the definition and 4,096 with it.
Publishing the definition to all library consumers gives the executable and the
library the same layout. Windows compiles `main.cpp` inside the library and
already had a consistent layout. The rebuilt local desktop passes the CRS smoke. Native Linux Memcheck also
confirms out-of-bounds writes in the original `QgisApp` constructor, immediately
after the 3,920-byte allocation made by its executable (platform layouts differ).
For that diagnostic only, malformed optional Qt-plugin debug metadata was
removed from a disposable extraction; the main executable was unchanged.

The immutable 1.6.3 tag is retained. Its Linux/macOS rebuilds use the corrected
compile-definition propagation as an explicit build-configuration input; the
workflow preserves `candidate-build-configuration.patch` with the artifacts.
The tagged application `.cpp`/`.h` files remain unchanged. Reproducing those
packages requires the recorded workflow recipe in addition to the source tag.

## Packaging corrections found during acceptance

The Linux AppImage originally omitted the `osgeo` Python bindings. The runtime
now includes the distribution's matched GDAL/NumPy pair and verifies a raster
array round trip before testing the desktop. The final 1.6.3 AppImage is completed
from the corrected native artifact, retaining the exact Strata executable hash.
Both its packaged CRS smoke and five PostGIS methods pass. Native Memcheck
confirms that the corrected constructor no longer performs the original
out-of-bounds writes; this is not a claim that every third-party diagnostic is
clean.

The macOS package records the Strata version in both bundle version fields.
Launch Services receives `PYTHONDONTWRITEBYTECODE=1` through `LSEnvironment`, so
normal Finder launches do not add Python cache files to the signed resources.
Direct command-line launches of the bundle executable must also set this
variable, as the package verification helpers do. See Apple's
[Launch Services environment documentation](https://developer.apple.com/library/archive/documentation/General/Reference/InfoPlistKeyReference/Articles/LaunchServicesKeys.html).
The workflow verifies the bundle seal again after its packaged runtime check.

## Final package validation, 2026-10-06

The exact 1.6.3 Windows portable ZIP, Linux AppImage and macOS universal DMG pass
all five synthetic PostGIS methods, without failures, errors or skips. The macOS
DMG was exercised as both arm64 and x86_64 using Rosetta. Each receipt records the
package SHA-256, source tag, test-file digest, architecture and bundled PyQGIS
module path. Host Python/library/data overrides were removed from these checks.

Windows native CI additionally verifies both installer and portable signing and
runtime behavior. The macOS app/DMG signatures, Gatekeeper assessments, accepted
Apple notarization and stapled DMG ticket pass. The bundled CRS smoke verifies
validity, identifiers and WKT; a valid layer alone is never the acceptance test.
Linux also passes the GDAL/NumPy array round trip and the constructor-allocation
regression described above.

See [final package evidence](strata-1.6.3-package-evidence.json) for exact hashes,
CI run URLs, build recipes and the deployment receipt. The immutable source tag
is `01ee92498993e2d8e3e0b375262d5f66f56a3a51`; the application C++/headers remain
unchanged. Linux/macOS use the corrected build configuration at
`2df694fc5a7422201ade0d917fd5e1307e00b12c`, plus the separately recorded final
packaging revisions. The release records distinguish these inputs explicitly.
No result from the pre-existing 1.6.2 package validates this release.

The GUI Browser → properties → assistant → project reopen check passed earlier
on the modified development build, using synthetic data and a loopback provider.
An additional interactive check on the final DMG could not start because the Mac
was locked and computer use required manual unlock. The final DMG's headless
PostGIS and signed-runtime checks passed; a Finder-launch/post-GUI signature
result is not claimed. Manual Windows 11 Smart App Control field acceptance also
remains pending. Distribution uses the existing installer channel, without an
in-app updater feed.

Closing the original customer incident additionally requires their Strata
version/platform, source SRID, connection characteristics and the affected layer.
No customer data has been used in this regression suite.

## Legacy catalog definitions and PROJ database, 2026-10-07

A customer layer (Windows, SRID 3003, loaded from the Browser) still showed a custom
CRS after 1.6.3: a valid BoundCRS "Monte Mario / Italy zone 1" with the TOWGS84 shift
-104.1, -49.1, -9.9, 0.971, -2.917, 0.714, -11.68 and `ID["EPSG",3003]` inside its
source CRS. PROJ 9 does not export TOWGS84 for EPSG:3003, but the PostGIS catalog
`srtext` still carries it (verified on PostGIS 3.4.3), so that CRS is the WKT fallback
of `spatial_ref_sys`, reached only when the declared authority was not resolved. The
1.6.3 diagnostics only cover invalid CRSs; this CRS was valid with an empty
authority, so no diagnostic fired, and the regression suite only exercised rows whose
EPSG authority resolves.

Two conditions lead there, and the correction covers both:

- The row declares no EPSG/ESRI authority. The WKT path gives the bound CRS and
  `setWktString()` skips BoundCRS candidates during identification (only the PROJ
  string path uses `FlagMatchBoundCrsToUnderlyingSourceCrs`). The provider now uses
  the authority declared inside the definition (`legacy_bound`), consistently with
  what QGIS already does for `+towgs84` PROJ strings and with the existing behaviour
  for rows that declare EPSG, where the TOWGS84 of the WKT was already ignored.
  Decision recorded here: the embedded Helmert shift is not used for datum
  transformations; QGIS selects transformations from the PROJ database as for any
  EPSG:3003 layer. Keeping the bound CRS instead is a one-line change in
  `crsFromCatalogDefinition()`.
- PROJ cannot open `proj.db` in the Strata process. `createFromUserInput("EPSG:3003")`
  goes through GDAL and fails, while the CRS dialog and the project CRS still show
  EPSG names through the `srs.db` fallback. On Windows the usual cause is a
  machine-wide `PROJ_LIB`/`PROJ_DATA` variable left by other installers (the PostGIS
  Stack Builder bundle, see PostGIS tickets 4766 and 5689) pointing to a `proj.db` of
  another PROJ version, which PROJ rejects ("It comes from another PROJ
  installation"). The authority is now resolved through `createFromOgcWmsCrs()`, the
  probe result is attached as `proj_database`, and on Windows `QgsApplication::init()`
  exports the bundled `share/proj` as `PROJ_DATA` and puts it first in the PROJ
  search paths, as the macOS build already did.

Local reproduction without a database: `PROJ_DATA` pointing to a copy of `proj.db`
whose `DATABASE.LAYOUT.VERSION.MINOR` metadata was lowered makes `projinfo EPSG:3003`
fail with the PROJ error above and `--identify` of the legacy WKT return no match.
