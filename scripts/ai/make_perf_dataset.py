#!/usr/bin/env python3
"""Generate the dataset used by scripts/ai/run_scenarios.py.

Layout written under OUT_DIR (the project folder, which is also the AI workspace root):

    project_100.qgz       100 vector layers: 60 GeoPackage layers, 39 shapefiles, 1 heavy layer
    data/layers.gpkg      60 point, line and polygon layers with attributes
    data/shp/*.shp        39 shapefiles
    data/heavy.gpkg       polygons with many vertices (a costly layer to read)
    extra/*.shp           50 more shapefiles for the "add 50 layers" scenario
    docs/                 500 text files: CSV, GeoJSON, .qgs-like XML and Italian prose

Run it with the Python and PyQGIS of a Strata build, for example:

    QGIS_PREFIX_PATH=$BUILD/output PYTHONPATH=$BUILD/output/python \\
        python3 scripts/ai/make_perf_dataset.py /path/to/dataset
"""

import json
import math
import os
import random
import sys

from qgis.core import (
    QgsApplication,
    QgsCoordinateReferenceSystem,
    QgsCoordinateTransformContext,
    QgsFeature,
    QgsField,
    QgsFields,
    QgsGeometry,
    QgsPointXY,
    QgsProject,
    QgsVectorFileWriter,
    QgsVectorLayer,
    QgsWkbTypes,
)
from qgis.PyQt.QtCore import QMetaType

CRS = QgsCoordinateReferenceSystem("EPSG:3857")
FEATURES_PER_LAYER = 2000
PROSE = (
    "La relazione tecnica descrive l'analisi dei vincoli paesaggistici e idrogeologici del comune, "
    "con attenzione alle aree boscate, ai corsi d'acqua tutelati e alle fasce di rispetto. "
    "Il censimento del verde urbano riporta specie, diametro, altezza e stato fitosanitario degli alberi. "
)


def fields():
    out = QgsFields()
    out.append(QgsField("id", QMetaType.Type.Int))
    out.append(QgsField("nome", QMetaType.Type.QString))
    out.append(QgsField("categoria", QMetaType.Type.QString))
    out.append(QgsField("valore", QMetaType.Type.Double))
    return out


def geometry(kind, rng, index, vertices=5):
    x = 1_000_000 + rng.uniform(0, 50_000)
    y = 5_600_000 + rng.uniform(0, 50_000)
    if kind == QgsWkbTypes.Type.Point:
        return QgsGeometry.fromPointXY(QgsPointXY(x, y))
    if kind == QgsWkbTypes.Type.LineString:
        return QgsGeometry.fromPolylineXY(
            [QgsPointXY(x + i * 10, y + math.sin(i) * 10) for i in range(vertices)]
        )
    ring = [
        QgsPointXY(x + 100 * math.cos(a), y + 100 * math.sin(a))
        for a in (2 * math.pi * i / vertices for i in range(vertices))
    ]
    ring.append(ring[0])
    return QgsGeometry.fromPolygonXY([ring])


def write_layer(
    path, layer_name, kind, count, rng, vertices=5, driver="GPKG", append=False
):
    layer = QgsVectorLayer(
        f"{QgsWkbTypes.displayString(kind)}?crs=EPSG:3857", layer_name, "memory"
    )
    provider = layer.dataProvider()
    provider.addAttributes(fields())
    layer.updateFields()
    features = []
    categories = ["parco", "strada", "scuola", "fiume", "bosco"]
    for i in range(count):
        feature = QgsFeature(layer.fields())
        feature.setAttributes(
            [
                i,
                f"{layer_name}_{i}",
                categories[i % len(categories)],
                rng.uniform(0, 1000),
            ]
        )
        feature.setGeometry(geometry(kind, rng, i, vertices))
        features.append(feature)
    provider.addFeatures(features)

    options = QgsVectorFileWriter.SaveVectorOptions()
    options.driverName = driver
    options.layerName = layer_name
    if append:
        options.actionOnExistingFile = (
            QgsVectorFileWriter.ActionOnExistingFile.CreateOrOverwriteLayer
        )
    error = QgsVectorFileWriter.writeAsVectorFormatV3(
        layer, path, QgsCoordinateTransformContext(), options
    )
    if error[0] != QgsVectorFileWriter.WriterError.NoError:
        raise RuntimeError(f"Cannot write {path}:{layer_name}: {error}")


def write_docs(docs_dir, rng):
    os.makedirs(docs_dir, exist_ok=True)
    for i in range(200):
        with open(os.path.join(docs_dir, f"tabella_{i:03d}.csv"), "w") as f:
            f.write("id;codice_istat;superficie_mq;x;y;anno\n")
            for row in range(60):
                f.write(
                    f"{row};017{row:03d};{rng.uniform(100, 99999):.2f};"
                    f"{rng.uniform(1.5e6, 1.6e6):.1f};{rng.uniform(5.0e6, 5.1e6):.1f};20{row % 25:02d}\n"
                )
    for i in range(150):
        collection = {
            "type": "FeatureCollection",
            "features": [
                {
                    "type": "Feature",
                    "properties": {
                        "id": j,
                        "nome": f"albero {j}",
                        "specie": "Tilia cordata",
                    },
                    "geometry": {
                        "type": "Point",
                        "coordinates": [rng.uniform(9, 10), rng.uniform(45, 46)],
                    },
                }
                for j in range(30)
            ],
        }
        with open(os.path.join(docs_dir, f"alberi_{i:03d}.geojson"), "w") as f:
            json.dump(collection, f)
    for i in range(100):
        with open(os.path.join(docs_dir, f"relazione_{i:03d}.md"), "w") as f:
            f.write(f"# Relazione {i}\n\n" + PROSE * 20)
    for i in range(50):
        with open(os.path.join(docs_dir, f"progetto_{i:03d}.xml"), "w") as f:
            f.write("<qgis>\n")
            for j in range(20):
                f.write(
                    f'<maplayer type="vector" geometry="Polygon"><id>particelle_{i}_{j}</id>'
                    f"<datasource>./dati/particelle.gpkg|layername=particelle_{j}</datasource>"
                    '<srs><spatialrefsys><wkt>PROJCRS["RDN2008 / UTM zone 32N"]</wkt></spatialrefsys></srs></maplayer>\n'
                )
            f.write("</qgis>\n")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out_dir = os.path.abspath(sys.argv[1])
    os.makedirs(os.path.join(out_dir, "data", "shp"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "extra"), exist_ok=True)

    app = QgsApplication([], False)
    app.initQgis()
    rng = random.Random(42)
    kinds = [
        QgsWkbTypes.Type.Point,
        QgsWkbTypes.Type.LineString,
        QgsWkbTypes.Type.Polygon,
    ]

    gpkg = os.path.join(out_dir, "data", "layers.gpkg")
    for i in range(60):
        write_layer(
            gpkg, f"strato_{i:02d}", kinds[i % 3], FEATURES_PER_LAYER, rng, append=i > 0
        )
    for i in range(39):
        write_layer(
            os.path.join(out_dir, "data", "shp", f"shape_{i:02d}.shp"),
            f"shape_{i:02d}",
            kinds[i % 3],
            FEATURES_PER_LAYER,
            rng,
            driver="ESRI Shapefile",
        )
    # Few features with many vertices each: slow to read and to turn into WKT.
    write_layer(
        os.path.join(out_dir, "data", "heavy.gpkg"),
        "confini_dettagliati",
        QgsWkbTypes.Type.Polygon,
        200,
        rng,
        vertices=20000,
    )
    for i in range(50):
        write_layer(
            os.path.join(out_dir, "extra", f"extra_{i:02d}.shp"),
            f"extra_{i:02d}",
            kinds[i % 3],
            500,
            rng,
            driver="ESRI Shapefile",
        )
    write_docs(os.path.join(out_dir, "docs"), rng)

    project = QgsProject.instance()
    project.setCrs(CRS)
    layers = [
        QgsVectorLayer(f"{gpkg}|layername=strato_{i:02d}", f"strato_{i:02d}", "ogr")
        for i in range(60)
    ]
    layers += [
        QgsVectorLayer(
            os.path.join(out_dir, "data", "shp", f"shape_{i:02d}.shp"),
            f"shape_{i:02d}",
            "ogr",
        )
        for i in range(39)
    ]
    layers.append(
        QgsVectorLayer(
            os.path.join(out_dir, "data", "heavy.gpkg")
            + "|layername=confini_dettagliati",
            "confini_dettagliati",
            "ogr",
        )
    )
    invalid = [layer.name() for layer in layers if not layer.isValid()]
    if invalid:
        raise RuntimeError(f"Invalid layers: {invalid}")
    project.addMapLayers(layers)
    if not project.write(os.path.join(out_dir, "project_100.qgz")):
        raise RuntimeError("Cannot write project_100.qgz")
    project.clear()
    app.exitQgis()
    print(f"Dataset written to {out_dir}")


if __name__ == "__main__":
    main()
