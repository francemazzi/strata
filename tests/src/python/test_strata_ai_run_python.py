"""QGIS unit tests for strata_ai.run_python session helpers."""

import threading
import unittest

import processing
import processing.gui.AlgorithmExecutor as algorithm_executor
from processing.core.Processing import Processing
from processing.tools.general import runAndLoadResults
from qgis.core import (
    QgsFeature,
    QgsGeometry,
    QgsPointXY,
    QgsProcessing,
    QgsProcessingException,
    QgsProcessingFeedback,
    QgsProject,
    QgsVectorLayer,
)
from qgis.PyQt.QtCore import QTimer
from qgis.PyQt.sip import unwrapinstance
from qgis.testing import QgisTestCase, start_app
from strata_ai.run_python import RunPythonTimeout, session

start_app()


def _memory_points(count, name="pts"):
    layer = QgsVectorLayer("Point?crs=EPSG:4326&field=id:integer", name, "memory")
    features = []
    for i in range(count):
        feature = QgsFeature(layer.fields())
        feature.setGeometry(QgsGeometry.fromPointXY(QgsPointXY(i, i)))
        feature.setAttribute("id", i)
        features.append(feature)
    layer.dataProvider().addFeatures(features)
    return layer


class TestStrataAiRunPython(QgisTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        Processing.initialize()

    def test_processing_run_returns_vector_layer_and_pumps_events(self):
        layer = _memory_points(8)
        QgsProject.instance().addMapLayer(layer)
        ran = {"ok": False}
        timer = QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(lambda: ran.__setitem__("ok", True))
        timer.start(0)
        with session(0, 30):
            result = processing.run(
                "native:buffer",
                {
                    "INPUT": layer,
                    "DISTANCE": 1,
                    "OUTPUT": QgsProcessing.TEMPORARY_OUTPUT,
                },
            )
        self.assertTrue(ran["ok"])
        self.assertIn("OUTPUT", result)
        self.assertIsInstance(result["OUTPUT"], QgsVectorLayer)
        QgsProject.instance().removeMapLayer(layer.id())

    def test_run_and_load_results_adds_layer(self):
        layer = _memory_points(4, "load-src")
        QgsProject.instance().addMapLayer(layer)
        before = set(QgsProject.instance().mapLayers().keys())
        with session(0, 30):
            result = runAndLoadResults(
                "native:buffer",
                {
                    "INPUT": layer,
                    "DISTANCE": 1,
                    "OUTPUT": QgsProcessing.TEMPORARY_OUTPUT,
                },
            )
        after = set(QgsProject.instance().mapLayers().keys())
        self.assertTrue(after - before)
        self.assertIn("OUTPUT", result)
        QgsProject.instance().removeMapLayer(layer.id())

    def test_worker_thread_uses_original_execute(self):
        layer = _memory_points(3, "worker-src")
        seen = []
        original = algorithm_executor.execute

        def tracking(*args, **kwargs):
            seen.append(threading.current_thread().name)
            return original(*args, **kwargs)

        algorithm_executor.execute = tracking
        errors = []

        def worker():
            try:
                with session(0, 30):
                    processing.run(
                        "native:buffer",
                        {
                            "INPUT": layer,
                            "DISTANCE": 1,
                            "OUTPUT": QgsProcessing.TEMPORARY_OUTPUT,
                        },
                    )
            except Exception as exc:  # pylint: disable=broad-except
                errors.append(str(exc))

        thread = threading.Thread(target=worker, name="strata-ai-worker")
        thread.start()
        thread.join(timeout=30)
        algorithm_executor.execute = original
        self.assertFalse(errors)
        self.assertIn("strata-ai-worker", seen)

    def test_no_threading_algorithm_uses_original_execute(self):
        points = _memory_points(3, "sel-pts")
        polys = QgsVectorLayer("Polygon?crs=EPSG:4326", "sel-poly", "memory")
        poly = QgsFeature(polys.fields())
        poly.setGeometry(
            QgsGeometry.fromWkt("Polygon(( -1 -1, 10 -1, 10 10, -1 10, -1 -1))")
        )
        polys.dataProvider().addFeatures([poly])
        seen = []
        original = algorithm_executor.execute

        def tracking(*args, **kwargs):
            seen.append("original")
            return original(*args, **kwargs)

        algorithm_executor.execute = tracking
        with session(0, 30):
            processing.run(
                "native:selectbylocation",
                {
                    "INPUT": points,
                    "PREDICATE": [0],
                    "INTERSECT": polys,
                    "METHOD": 0,
                },
            )
        algorithm_executor.execute = original
        self.assertIn("original", seen)
        self.assertEqual(points.selectedFeatureCount(), 3)

    def test_canceled_bridge_raises(self):
        layer = _memory_points(400, "cancel-src")
        feedback = QgsProcessingFeedback()
        QTimer.singleShot(0, feedback.cancel)
        with self.assertRaises((QgsProcessingException, RunPythonTimeout)):
            with session(unwrapinstance(feedback), 30):
                processing.run(
                    "native:buffer",
                    {
                        "INPUT": layer,
                        "DISTANCE": 1,
                        "SEGMENTS": 8,
                        "OUTPUT": QgsProcessing.TEMPORARY_OUTPUT,
                    },
                )

    def test_timeout_interrupts_python_loop(self):
        with self.assertRaises(RunPythonTimeout):
            with session(0, 1):
                while True:
                    pass

    def test_execute_restored_after_session(self):
        original = algorithm_executor.execute
        with session(0, 30):
            self.assertIsNot(algorithm_executor.execute, original)
        self.assertIs(algorithm_executor.execute, original)


if __name__ == "__main__":
    unittest.main()
