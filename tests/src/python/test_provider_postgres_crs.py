"""CRS regression matrix. STRATA_CRS_TEST_DB must name a disposable PostGIS DB.

This suite temporarily changes catalog grants, and deliberately never uses the
ordinary QGIS_PGTEST_DB setting. Run sequentially against its own container.
SPDX-License-Identifier: GPL-2.0-or-later
"""

import os
import tempfile
import unittest
import uuid

from qgis.core import (
    Qgis,
    QgsApplication,
    QgsCoordinateReferenceSystem,
    QgsDataSourceUri,
    QgsProject,
    QgsProviderRegistry,
    QgsVectorLayer,
)
from qgis.testing import start_app

APP = start_app()


@unittest.skipUnless(
    os.environ.get("STRATA_CRS_TEST_DB"), "Requires an isolated STRATA_CRS_TEST_DB"
)
class TestPostgresCrs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.uri = os.environ["STRATA_CRS_TEST_DB"]
        cls.md = QgsProviderRegistry.instance().providerMetadata("postgres")
        cls.conn = cls.md.createConnection(cls.uri, {})
        cls.schema = "crs_" + uuid.uuid4().hex
        # Never overwrite a pre-existing definition, even in a test database.
        if cls.conn.executeSql(
            "SELECT srid FROM spatial_ref_sys WHERE srid BETWEEN 990051 AND 990057"
        ):
            raise RuntimeError(
                "CRS test SRIDs already exist; use a fresh disposable database"
            )
        cls.conn.executeSql(f"CREATE SCHEMA {cls.schema}")
        cls.custom = QgsCoordinateReferenceSystem.fromProj(
            "+proj=tmerc +lat_0=0 +lon_0=9.123456 +k=0.9996 +x_0=500000 +y_0=0 +ellps=GRS80 +units=m"
        )
        wkt = cls.custom.toWkt(Qgis.CrsWktVariant.Preferred).replace("'", "''")
        proj = cls.custom.toProj().replace("'", "''")
        cls.conn.executeSql(
            f"INSERT INTO spatial_ref_sys VALUES (990051, 'LOCAL', 990051, '{wkt}', '{proj}')"
        )
        cls.conn.executeSql(
            f"INSERT INTO spatial_ref_sys VALUES (990057, 'LOCAL', 990057, '', '{proj}')"
        )
        for srid in [990052, 990053]:
            cls.conn.executeSql(
                f"INSERT INTO spatial_ref_sys SELECT {srid},auth_name,auth_srid,srtext,proj4text "
                "FROM spatial_ref_sys WHERE srid=32632"
            )
        cls.conn.executeSql(
            "INSERT INTO spatial_ref_sys VALUES (990054,'LOCAL',990054,'invalid','invalid')"
        )
        esri = QgsCoordinateReferenceSystem("ESRI:102100")
        cls.conn.executeSql(
            "INSERT INTO spatial_ref_sys VALUES (990056,'ESRI',102100,'"
            + esri.toWkt().replace("'", "''")
            + "','"
            + esri.toProj().replace("'", "''")
            + "')"
        )

    @classmethod
    def tearDownClass(cls):
        cls.conn.executeSql(f"DROP SCHEMA {cls.schema} CASCADE")
        cls.conn.executeSql(
            "DELETE FROM spatial_ref_sys WHERE srid BETWEEN 990051 AND 990057"
        )
        cls.conn = None

    def create_table(self, name, srid):
        self.conn.executeSql(
            f"CREATE TABLE {self.schema}.{name}(id integer PRIMARY KEY,geom geometry(Point,{srid}))"
        )
        self.conn.executeSql(
            f"INSERT INTO {self.schema}.{name} VALUES (1,ST_SetSRID(ST_MakePoint(9,45),{srid}))"
        )

    def layer(self, name, uri=None):
        source = QgsDataSourceUri(uri or self.uri)
        source.setDataSource(self.schema, name, "geom", "", "id")
        options = QgsVectorLayer.LayerOptions()
        options.skipCrsValidation = True
        layer = QgsVectorLayer(source.uri(False), name, "postgres", options)
        self.assertTrue(layer.isValid())
        self.assertEqual(layer.featureCount(), 1)
        return layer

    def test_standard_custom_and_remapped(self):
        for srid in [
            4326,
            3003,
            3004,
            32632,
            32633,
            25832,
            25833,
            6706,
            6707,
            6708,
            7791,
            7792,
            990051,
            990052,
            990056,
            990057,
        ]:
            with self.subTest(srid=srid):
                name = f"matrix_{srid}"
                self.create_table(name, srid)
                layer = self.layer(name)
                prop = self.conn.table(self.schema, name)
                self.assertTrue(layer.crs().isValid())
                self.assertEqual(prop.geometryColumnTypes()[0].crs, layer.crs())
                info = prop.info()["crs_details"]
                self.assertEqual(info[0]["source_srid"], srid)
                self.assertEqual(info[0]["status"], "valid")
                self.assertEqual(
                    layer.dataProvider().property("crsResolution")["source_srid"], srid
                )
                if srid == 990051:
                    self.assertEqual(layer.crs().authid(), "")
                    self.assertEqual(layer.crs(), self.custom)
                if srid == 990052:
                    self.assertEqual(layer.crs().authid(), "EPSG:32632")
                if srid == 990056:
                    self.assertEqual(layer.crs().authid(), "ESRI:102100")
                if srid == 990057:
                    self.assertEqual(layer.crs(), self.custom)
                # Test both automatic detection and Browser-style explicit SRID.
                explicit = QgsDataSourceUri(layer.source())
                explicit.setSrid(str(srid))
                explicit_layer = QgsVectorLayer(explicit.uri(False), name, "postgres")
                self.assertEqual(explicit_layer.crs(), layer.crs())
                with tempfile.TemporaryDirectory() as tmp:
                    project = QgsProject()
                    project.addMapLayer(explicit_layer)
                    path = os.path.join(tmp, "roundtrip.qgz")
                    self.assertTrue(project.write(path))
                    project.clear()
                    self.assertTrue(project.read(path))
                    self.assertEqual(
                        next(iter(project.mapLayers().values())).crs(), layer.crs()
                    )
                    project.clear()

    def test_missing_invalid_and_recovery(self):
        for name, srid, code in [
            ("unset", 0, "srid_unspecified"),
            ("invalid", 990054, "definition_invalid"),
            ("recover", 990055, "definition_missing"),
        ]:
            self.create_table(name, srid)
            layer = self.layer(name)
            self.assertFalse(layer.crs().isValid())
            diagnostic = layer.dataProvider().property("crsResolution")["diagnostic"]
            self.assertEqual(diagnostic["code"], code)
            self.assertIn(diagnostic["message"], layer.dataProvider().htmlMetadata())
        self.conn.executeSql(
            "INSERT INTO spatial_ref_sys SELECT 990055,auth_name,auth_srid,srtext,proj4text FROM spatial_ref_sys WHERE srid=32632"
        )
        self.assertEqual(self.layer("recover").crs().authid(), "EPSG:32632")
        self.assertTrue(
            self.conn.table(self.schema, "recover")
            .geometryColumnTypes()[0]
            .crs.isValid()
        )

    def test_equivalent_srids_and_nonspatial(self):
        self.conn.executeSql(
            f"CREATE TABLE {self.schema}.mixed(id integer PRIMARY KEY,geom geometry)"
        )
        self.conn.executeSql(
            f"INSERT INTO {self.schema}.mixed VALUES (1,ST_SetSRID(ST_MakePoint(0,0),990052)),(2,ST_SetSRID(ST_MakePoint(1,1),990053))"
        )
        prop = self.conn.table(self.schema, "mixed")
        self.assertEqual(
            {v["source_srid"] for v in prop.info()["crs_details"]}, {990052, 990053}
        )
        self.assertTrue(
            all(v["crs"].authid() == "EPSG:32632" for v in prop.info()["crs_details"])
        )
        self.assertEqual(self.conn.fields(self.schema, "mixed").names(), ["id", "geom"])
        for detail in prop.info()["crs_details"]:
            source = QgsDataSourceUri(self.uri)
            source.setDataSource(self.schema, "mixed", "geom", "", "id")
            source.setSrid(str(detail["source_srid"]))
            source.setWkbType(Qgis.WkbType(detail["wkb_type"]))
            selected = QgsVectorLayer(source.uri(False), "one SRID", "postgres")
            self.assertTrue(selected.isValid())
            self.assertEqual(selected.featureCount(), 1)
        self.conn.executeSql(
            f"CREATE TABLE {self.schema}.dual(id integer PRIMARY KEY, geom geometry(Point,4326), other geometry(LineString,3003))"
        )
        pairs = {
            (p.geometryColumn(), d["source_srid"])
            for p in self.conn.tables(self.schema)
            if p.tableName() == "dual"
            for d in p.info()["crs_details"]
        }
        self.assertEqual(pairs, {("geom", 4326), ("other", 3003)})
        self.assertEqual(
            {
                (d["geometry_column"], d["source_srid"])
                for d in self.conn.table(self.schema, "dual").info()["crs_details"]
            },
            pairs,
        )
        self.conn.executeSql(
            f"CREATE TABLE {self.schema}.plain(id integer PRIMARY KEY)"
        )
        self.assertTrue(
            all(
                v["status"] == "not_applicable"
                for v in self.conn.table(self.schema, "plain").info()["crs_details"]
            )
        )

    def test_catalog_unavailable(self):
        self.create_table("catalog_hidden", 32631)
        # Dedicated disposable DB only. Restore even when an assertion fails.
        self.conn.executeSql(
            "ALTER TABLE spatial_ref_sys RENAME TO crs_test_hidden_catalog"
        )
        try:
            layer = self.layer("catalog_hidden")
            self.assertFalse(layer.crs().isValid())
            self.assertEqual(
                layer.dataProvider().property("crsResolution")["diagnostic"]["code"],
                "catalog_unavailable",
            )
        finally:
            self.conn.executeSql(
                "ALTER TABLE crs_test_hidden_catalog RENAME TO spatial_ref_sys"
            )
        self.assertEqual(self.layer("catalog_hidden").crs().authid(), "EPSG:32631")

    def test_permission_recovery(self):
        name = "restricted"
        self.create_table(name, 32632)
        role = "crs_reader_" + uuid.uuid4().hex
        self.conn.executeSql(f"CREATE ROLE {role}")
        try:
            self.conn.executeSql(f"GRANT USAGE ON SCHEMA {self.schema} TO {role}")
            self.conn.executeSql(f"GRANT SELECT ON {self.schema}.{name} TO {role}")
            self.conn.executeSql("REVOKE SELECT ON spatial_ref_sys FROM PUBLIC")
            source = QgsDataSourceUri(self.uri)
            source.setParam("session_role", role)
            restricted = self.layer(name, source.uri(False))
            restricted_conn = self.md.createConnection(source.uri(False), {})
            self.assertFalse(restricted.crs().isValid())
            self.assertEqual(
                restricted_conn.table(self.schema, name).info()["crs_details"][0][
                    "diagnostic"
                ]["code"],
                "permission_denied",
            )
            self.assertEqual(
                restricted.dataProvider().property("crsResolution")["diagnostic"][
                    "code"
                ],
                "permission_denied",
            )
            self.conn.executeSql(f"GRANT SELECT ON spatial_ref_sys TO {role}")
            recovered = self.layer(name, source.uri(False))
            self.assertEqual(recovered.crs().authid(), "EPSG:32632")
            self.assertEqual(
                restricted_conn.table(self.schema, name)
                .info()["crs_details"][0]["crs"]
                .authid(),
                "EPSG:32632",
            )
            self.assertNotIn(
                "diagnostic", recovered.dataProvider().property("crsResolution")
            )
        finally:
            self.conn.executeSql("GRANT SELECT ON spatial_ref_sys TO PUBLIC")
            self.conn.executeSql(f"DROP OWNED BY {role}")
            self.conn.executeSql(f"DROP ROLE {role}")


if __name__ == "__main__":
    unittest.main()
