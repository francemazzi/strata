"""QGIS unit tests for strata_ai.run_python session helpers."""

import threading
import time
import unittest

import processing
import processing.core.Processing as processing_module
import processing.gui.AlgorithmExecutor as algorithm_executor
import strata_ai.run_python as run_python
from processing.core.Processing import Processing
from processing.tools.general import runAndLoadResults
from qgis.core import (
    QgsApplication,
    QgsFeature,
    QgsGeometry,
    QgsPointXY,
    QgsProcessing,
    QgsProcessingAlgorithm,
    QgsProcessingException,
    QgsProcessingFeedback,
    QgsProcessingParameterNumber,
    QgsProcessingProvider,
    QgsProject,
    QgsVectorLayer,
)
from qgis.PyQt.QtCore import QTimer
from qgis.PyQt.sip import unwrapinstance
from qgis.testing import QgisTestCase, start_app
from strata_ai.run_python import RunPythonTimeout, session

start_app()


class _SlowAlgorithm(QgsProcessingAlgorithm):
    """Sleeps for DURATION seconds in small steps, honoring cancellation."""

    def name(self):
        return "slow"

    def displayName(self):
        return "Slow"

    def createInstance(self):
        return _SlowAlgorithm()

    def initAlgorithm(self, config=None):
        self.addParameter(
            QgsProcessingParameterNumber(
                "DURATION",
                "Duration",
                type=QgsProcessingParameterNumber.Type.Double,
                defaultValue=30,
            )
        )

    def processAlgorithm(self, parameters, context, feedback):
        deadline = time.monotonic() + self.parameterAsDouble(
            parameters, "DURATION", context
        )
        while time.monotonic() < deadline:
            if feedback.isCanceled():
                break
            time.sleep(0.02)
        return {}


class _FailingPrepareAlgorithm(QgsProcessingAlgorithm):
    """Fails in prepareAlgorithm(), which the runner task calls in its constructor."""

    def name(self):
        return "failingprepare"

    def displayName(self):
        return "Failing prepare"

    def createInstance(self):
        return _FailingPrepareAlgorithm()

    def initAlgorithm(self, config=None):
        pass

    def prepareAlgorithm(self, parameters, context, feedback):
        raise QgsProcessingException("prepare boom")

    def processAlgorithm(self, parameters, context, feedback):
        return {}


class _TestProvider(QgsProcessingProvider):
    def id(self):
        return "strataaitest"

    def name(self):
        return "Strata AI test"

    def loadAlgorithms(self):
        self.addAlgorithm(_SlowAlgorithm())
        self.addAlgorithm(_FailingPrepareAlgorithm())


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


def _buffer_parameters(layer):
    return {
        "INPUT": layer,
        "DISTANCE": 1,
        "OUTPUT": QgsProcessing.TEMPORARY_OUTPUT,
    }


class TestStrataAiRunPython(QgisTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        Processing.initialize()
        cls.provider = _TestProvider()
        QgsApplication.processingRegistry().addProvider(cls.provider)

    def _track_original_execute(self):
        """Replaces the stock execute with a spy; the session picks it up as the original."""
        seen = []
        original = algorithm_executor.execute

        def tracking(*args, **kwargs):
            seen.append(threading.current_thread().name)
            return original(*args, **kwargs)

        algorithm_executor.execute = tracking
        processing_module.execute = tracking
        self.addCleanup(setattr, algorithm_executor, "execute", original)
        self.addCleanup(setattr, processing_module, "execute", original)
        return seen

    def test_processing_run_returns_vector_layer_and_pumps_events(self):
        layer = _memory_points(8)
        QgsProject.instance().addMapLayer(layer)
        ran = {"ok": False}
        timer = QTimer()
        timer.setSingleShot(True)
        timer.timeout.connect(lambda: ran.__setitem__("ok", True))
        timer.start(0)
        with session(0, 30):
            result = processing.run("native:buffer", _buffer_parameters(layer))
        self.assertTrue(ran["ok"])
        self.assertIn("OUTPUT", result)
        self.assertIsInstance(result["OUTPUT"], QgsVectorLayer)
        QgsProject.instance().removeMapLayer(layer.id())

    def test_run_and_load_results_adds_layer(self):
        layer = _memory_points(4, "load-src")
        QgsProject.instance().addMapLayer(layer)
        before = set(QgsProject.instance().mapLayers().keys())
        with session(0, 30):
            result = runAndLoadResults("native:buffer", _buffer_parameters(layer))
        added = set(QgsProject.instance().mapLayers().keys()) - before
        self.assertTrue(added)
        self.assertIn("OUTPUT", result)
        QgsProject.instance().removeMapLayers(list(added) + [layer.id()])

    def test_worker_thread_uses_original_execute(self):
        layer = _memory_points(3, "worker-src")
        seen = self._track_original_execute()
        errors = []

        def worker():
            try:
                with session(0, 30):
                    processing.run("native:buffer", _buffer_parameters(layer))
            except Exception as exc:  # pylint: disable=broad-except
                errors.append(str(exc))

        thread = threading.Thread(target=worker, name="strata-ai-worker")
        thread.start()
        thread.join(timeout=30)
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
        seen = self._track_original_execute()
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
        self.assertTrue(seen)
        self.assertEqual(points.selectedFeatureCount(), 3)

    def test_feedback_subclass_uses_original_execute(self):
        class RecordingFeedback(QgsProcessingFeedback):
            pass

        layer = _memory_points(3, "subclass-src")
        seen = self._track_original_execute()
        with session(0, 30):
            processing.run(
                "native:buffer", _buffer_parameters(layer), feedback=RecordingFeedback()
            )
        # Its Python overrides must not be called from a worker thread.
        self.assertTrue(seen)

    def test_nested_main_thread_call_uses_original_execute(self):
        layer = _memory_points(3, "nested-src")
        seen = self._track_original_execute()
        nested = {}

        def run_nested():
            nested["result"] = processing.run(
                "native:buffer", _buffer_parameters(layer)
            )

        # While the slow run waits in its event loop, other main-thread code calls
        # processing.run: that call stays synchronous instead of nesting another wait.
        QTimer.singleShot(100, run_nested)
        with session(0, 30):
            processing.run("strataaitest:slow", {"DURATION": 1.0})
        self.assertEqual(len(seen), 1)
        self.assertIn("OUTPUT", nested["result"])

    def test_bridge_cancel_stops_a_slow_algorithm(self):
        bridge = QgsProcessingFeedback()
        QTimer.singleShot(300, bridge.cancel)
        started = time.monotonic()
        with self.assertRaises((QgsProcessingException, RunPythonTimeout)):
            with session(unwrapinstance(bridge), 30):
                processing.run("strataaitest:slow", {"DURATION": 30})
        self.assertLess(time.monotonic() - started, 10)
        self.assertTrue(bridge.isCanceled())

    def test_prepare_failure_raises_without_hanging(self):
        started = time.monotonic()
        with self.assertRaises(QgsProcessingException) as raised:
            with session(0, 30):
                processing.run("strataaitest:failingprepare", {})
        self.assertIn("prepare boom", str(raised.exception))
        self.assertLess(time.monotonic() - started, 10)

    def test_timeout_interrupts_python_loop(self):
        with self.assertRaises(RunPythonTimeout):
            with session(0, 1):
                while True:
                    pass

    def test_watchdog_fires_again_after_a_swallowed_interrupt(self):
        caught = 0
        started = time.monotonic()
        with session(0, 1):
            while caught < 2 and time.monotonic() - started < 20:
                try:
                    while True:
                        pass
                except RunPythonTimeout:
                    caught += 1
        self.assertEqual(caught, 2)

    def test_processing_time_does_not_count_against_the_budget(self):
        # 0.8 s of the 1 s budget in Python, then a 1.5 s Processing run: the run must
        # finish normally, since an interrupt inside its wait would hang it.
        started = time.monotonic()
        with session(0, 1):
            deadline = time.monotonic() + 0.8
            while time.monotonic() < deadline:
                pass
            processing.run("strataaitest:slow", {"DURATION": 1.5})
        self.assertLess(time.monotonic() - started, 10)

    def test_execute_restored_after_session(self):
        original_executor = algorithm_executor.execute
        original_processing = processing_module.execute
        with session(0, 30):
            self.assertIsNot(algorithm_executor.execute, original_executor)
            self.assertIsNot(processing_module.execute, original_processing)
        self.assertIs(algorithm_executor.execute, original_executor)
        self.assertIs(processing_module.execute, original_processing)

    def test_execute_restored_after_an_exception_inside_the_session(self):
        original_executor = algorithm_executor.execute
        original_processing = processing_module.execute
        with self.assertRaises(ValueError):
            with session(0, 30):
                raise ValueError("boom")
        self.assertIs(algorithm_executor.execute, original_executor)
        self.assertIs(processing_module.execute, original_processing)

    def test_stale_reference_to_patched_execute_uses_original(self):
        with session(0, 30):
            grabbed = processing_module.execute
        self.assertIs(grabbed, run_python._patched_execute)
        # Code that kept the patched function must still run Processing after the session.
        layer = _memory_points(2, "stale-src")
        algorithm = QgsApplication.processingRegistry().createAlgorithmById(
            "native:buffer"
        )
        ok, results = grabbed(algorithm, _buffer_parameters(layer), None, None, False)
        self.assertTrue(ok)
        self.assertIn("OUTPUT", results)


if __name__ == "__main__":
    unittest.main()
