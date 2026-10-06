# Strata 1.6.3

Strata now recognizes valid custom coordinate reference systems even when they
have no EPSG identifier. The assistant, database catalog and layer properties
use consistent CRS metadata and distinguish layer coordinates from the map view.

- Local PostGIS SRIDs resolve through their connection's `spatial_ref_sys`,
  including mappings to EPSG/ESRI and definitions provided as WKT or PROJ.
  Original SRIDs and all geometry-column/SRID associations are preserved.
- Missing definitions, inaccessible catalogs, insufficient catalog permissions
  and uninterpretable definitions have specific diagnostics. The layer remains
  available; Strata does not silently assign a replacement CRS. Reopening retries
  failed resolutions after a definition or permission has been corrected.
- Custom CRS definitions remain available to the assistant and survive project
  save/reopen. A valid custom project CRS is labeled “Custom CRS” in the status bar.
- Remote layer indexing includes existing CRS, geometry-type and field metadata
  without automatically reading features. Existing layer chunks are refreshed
  through the normal indexing coordinator, respecting pause and disable settings.

This change does not alter customer databases, privileges or the cloud backend.
Candidate validation is in progress. See [implementation and validation](../implementation/postgis-crs.md)
for evidence and the outstanding package/customer acceptance gates.
