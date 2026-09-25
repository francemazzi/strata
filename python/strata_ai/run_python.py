"""Off-thread Processing.execute patch, Stop bridge, and run_python timeout."""

from __future__ import annotations

import ctypes
import threading
import time
from contextlib import contextmanager

from qgis.core import (
    Qgis,
    QgsApplication,
    QgsMapLayer,
    QgsProcessingAlgorithm,
    QgsProcessingAlgRunnerTask,
    QgsProcessingException,
    QgsProcessingFeedback,
    QgsProcessingModelAlgorithm,
    QgsProject,
    QgsTask,
)
from qgis.PyQt import sip
from qgis.PyQt.QtCore import QEventLoop, QThread

# Seconds between two interrupts while the snippet stays over budget: a bare
# ``except:`` in the snippet swallows one, the next one lands anyway.
REFIRE_INTERVAL_S = 1.0

# Guards the watchdog decision (session active, not inside Processing) and the
# interrupt itself, so an interrupt never lands inside a Processing wait, e.g. in
# the callback that ends its event loop.
_lock = threading.Lock()
_session_active = False
_execute_depth = 0
_main_thread_id = None
_active_bridge = None
_interrupt_message = ""

# The functions each session replaced, restored when it ends. Never reset to None:
# code that grabbed the patched function during a session keeps calling it, and it
# must still reach a real execute afterwards.
_original_algorithm_executor_execute = None
_original_processing_execute = None

# Processing runs abandoned when Strata quits: their C++ task still references the
# context and feedback, which must outlive the Python frames that created them.
_abandoned_runs = []


class RunPythonInterrupted(BaseException):
    """Stops a run_python snippet. A BaseException, so ``except Exception`` lets it through."""


class RunPythonTimeout(RunPythonInterrupted):
    """Raised when run_python exceeds its budget or Stop is pressed outside Processing."""

    def __init__(self, message=None):
        super().__init__(message or _interrupt_message or "Python execution timed out.")


class RunPythonLayersRemoved(RunPythonInterrupted):
    """Raised when project layers were removed while processing.run was running.

    The snippet may still hold Python references to them, and sip does not notice
    when C++ deletes a layer it created: using one would crash Strata.
    """


def _wrap_bridge(bridge_ptr):
    if not bridge_ptr:
        return None
    return sip.wrapinstance(int(bridge_ptr), QgsProcessingFeedback)


def _algorithm_is_no_threading(alg):
    flags = alg.flags()
    processing_flags = getattr(Qgis, "ProcessingAlgorithmFlag", None)
    no_threading = (
        getattr(processing_flags, "NoThreading", None)
        if processing_flags is not None
        else None
    )
    if no_threading is not None and flags & no_threading:
        return True
    flag = getattr(QgsProcessingAlgorithm.Flag, "FlagNoThreading", None)
    return flag is not None and bool(flags & flag)


# Providers shipped with Strata whose algorithms the Toolbox already runs in the
# background. Script and plugin algorithms, and algorithms defined in the snippet,
# stay on the synchronous path like in the Python console: written on the fly, they
# often touch the GUI or the project without declaring NoThreading.
BACKGROUND_PROVIDERS = {
    "native",
    "3d",
    "pdal",
    "gdal",
    "qgis",
    "grass",
    "model",
    "project",
}


def _runs_in_background(alg):
    provider = alg.provider()
    if provider is None or provider.id() not in BACKGROUND_PROVIDERS:
        return False
    if isinstance(alg, QgsProcessingModelAlgorithm):
        # A model runs its children on its own worker: each one must be trusted too.
        for child in alg.childAlgorithms().values():
            child_alg = child.algorithm()
            if child_alg is None or not _runs_in_background(child_alg):
                return False
    return True


def _raise_async(thread_id, message):
    global _interrupt_message
    _interrupt_message = message
    set_async_exc = ctypes.pythonapi.PyThreadState_SetAsyncExc
    changed = set_async_exc(
        ctypes.c_ulong(thread_id), ctypes.py_object(RunPythonTimeout)
    )
    if changed > 1:
        # Documented CPython contract: more than one thread state was hit, undo.
        set_async_exc(ctypes.c_ulong(thread_id), None)


def _clear_async(thread_id):
    if thread_id is not None:
        ctypes.pythonapi.PyThreadState_SetAsyncExc(ctypes.c_ulong(thread_id), None)


def _run_off_thread(alg, parameters, context, feedback, catch_exceptions):
    """Runs alg in a QgsProcessingAlgRunnerTask while a nested event loop keeps the GUI alive."""
    bridge = _active_bridge
    if bridge is not None and bridge.isCanceled():
        raise QgsProcessingException("Canceled.")

    flags = (
        QgsTask.Flag.CanCancel | QgsTask.Flag.CancelWithoutPrompt | QgsTask.Flag.Silent
    )
    task = QgsProcessingAlgRunnerTask(alg, parameters, context, feedback, flags)
    # The snippet may hold Python references to any project layer, not only the
    # inputs, and sip does not notice when C++ deletes a layer it created. While the
    # run pumps events, QGIS refuses to remove the layers a task depends on or to
    # close the project.
    project = QgsProject.instance()
    task.setDependentLayers(list(project.mapLayers().values()))
    if task.isCanceled():
        # prepare() failed in the constructor: the task never runs and never emits
        # executed, so report the failure now instead of waiting forever.
        if not catch_exceptions:
            raise QgsProcessingException(
                feedback.textLog().strip() or "Processing task failed to start."
            )
        return False, {}

    loop = QEventLoop()
    outcome = {"done": False, "ok": False, "results": {}}
    errors = []

    def on_executed(ok, results):
        outcome["done"] = True
        outcome["ok"] = bool(ok)
        outcome["results"] = results
        loop.quit()

    def on_error(text, _fatal):
        errors.append(text)

    def on_bridge_canceled():
        feedback.cancel()
        if not sip.isdeleted(task):
            task.cancel()

    # A plugin can still remove layers from code: the task manager then cancels the
    # task, and the snippet must not go on with its references.
    removed = []

    def on_layers_will_be_removed(items):
        for item in items:
            layer = item if isinstance(item, QgsMapLayer) else project.mapLayer(item)
            removed.append(layer.name() if layer is not None else str(item))

    task.executed.connect(on_executed)
    feedback.errorReported.connect(on_error)
    project.layersWillBeRemoved.connect(on_layers_will_be_removed)
    if bridge is not None:
        bridge.canceled.connect(on_bridge_canceled)
        feedback.progressChanged.connect(bridge.setProgress)
    try:
        QgsApplication.taskManager().addTask(task)
        loop.exec()
    finally:
        # The caller may reuse its feedback for later runs: leave no connection behind.
        connections = [
            (feedback.errorReported, on_error),
            (project.layersWillBeRemoved, on_layers_will_be_removed),
        ]
        if bridge is not None:
            connections += [
                (bridge.canceled, on_bridge_canceled),
                (feedback.progressChanged, bridge.setProgress),
            ]
        for signal, slot in connections:
            try:
                signal.disconnect(slot)
            except (TypeError, RuntimeError):
                pass

    if not outcome["done"]:
        # Every event loop was stopped (Strata is quitting) while the task still runs.
        _abandoned_runs.append((task, context, feedback))
        if not sip.isdeleted(task):
            task.taskCompleted.connect(_forget_abandoned_runs)
            task.taskTerminated.connect(_forget_abandoned_runs)
            task.cancel()
        raise QgsProcessingException(
            "Strata is closing; the Processing run was stopped."
        )

    if removed:
        raise RunPythonLayersRemoved(
            "Project layers were removed while processing.run was running: "
            + ", ".join(removed)
            + "."
        )

    if not outcome["ok"] and not catch_exceptions:
        message = errors[-1] if errors else ""
        raise QgsProcessingException(
            message or "There were errors executing the algorithm."
        )
    return outcome["ok"], outcome["results"] or {}


def _forget_abandoned_runs():
    _abandoned_runs[:] = [run for run in _abandoned_runs if not sip.isdeleted(run[0])]


def _patched_execute(
    alg, parameters, context=None, feedback=None, catch_exceptions=True
):
    global _execute_depth
    if (
        not _session_active
        or _execute_depth > 0
        or QThread.currentThread() != QgsApplication.instance().thread()
        or (feedback is not None and type(feedback) is not QgsProcessingFeedback)
        or _algorithm_is_no_threading(alg)
        or not _runs_in_background(alg)
    ):
        # Outside a run_python session, nested in a Processing wait, off the main
        # thread (scripts and models calling processing.run from their worker), with
        # a Python feedback subclass whose overrides must not run on a worker, for
        # algorithms that must stay on the main thread, or for script and plugin
        # algorithms: the stock synchronous path.
        return _original_algorithm_executor_execute(
            alg, parameters, context, feedback, catch_exceptions
        )

    if feedback is None:
        feedback = QgsProcessingFeedback()
    if context is None:
        from processing.tools import dataobjects

        context = dataobjects.createContext(feedback)

    with _lock:
        _execute_depth += 1
        # An interrupt fired just before Processing started must not land in its wait:
        # the watchdog fires again after the run if the budget is still exceeded.
        _clear_async(_main_thread_id)
    try:
        return _run_off_thread(alg, parameters, context, feedback, catch_exceptions)
    finally:
        with _lock:
            _execute_depth -= 1


def _install_execute_patch():
    global _original_algorithm_executor_execute, _original_processing_execute
    import processing.core.Processing as processing_module
    import processing.gui.AlgorithmExecutor as algorithm_executor

    # Remember whatever is installed now (possibly another wrapper), unless it is ours.
    if algorithm_executor.execute is not _patched_execute:
        _original_algorithm_executor_execute = algorithm_executor.execute
    current_processing_execute = getattr(processing_module, "execute", None)
    if current_processing_execute is not _patched_execute:
        _original_processing_execute = current_processing_execute
    algorithm_executor.execute = _patched_execute
    processing_module.execute = _patched_execute


def _restore_execute_patch():
    import processing.core.Processing as processing_module
    import processing.gui.AlgorithmExecutor as algorithm_executor

    if _original_algorithm_executor_execute is not None:
        algorithm_executor.execute = _original_algorithm_executor_execute
    if _original_processing_execute is not None:
        processing_module.execute = _original_processing_execute


@contextmanager
def session(bridge_ptr, timeout_s=120):
    """Patch Processing.execute, honor Stop, and interrupt Python after timeout_s seconds.

    Time spent inside a patched Processing run does not count: the GUI stays
    responsive there and Stop cancels the run itself.
    """
    global _active_bridge, _interrupt_message, _main_thread_id, _session_active

    bridge = _wrap_bridge(bridge_ptr)
    main_thread_id = threading.get_ident()
    budget = max(1.0, float(timeout_s))
    stop_watchdog = threading.Event()

    def watchdog():
        used = 0.0
        last = time.monotonic()
        next_fire = 0.0
        while not stop_watchdog.wait(0.05):
            now = time.monotonic()
            with _lock:
                if not _session_active:
                    return
                if _execute_depth > 0:
                    last = now
                    continue
                used += now - last
                last = now
                canceled = bridge is not None and bridge.isCanceled()
                if (canceled or used >= budget) and now >= next_fire:
                    message = (
                        "Canceled."
                        if canceled
                        else f"stopped after {int(timeout_s)} s of Python"
                    )
                    _raise_async(main_thread_id, message)
                    next_fire = now + REFIRE_INTERVAL_S

    with _lock:
        _active_bridge = bridge
        _interrupt_message = ""
        _main_thread_id = main_thread_id
        _session_active = True
    watcher = threading.Thread(
        target=watchdog, name="strata-ai-run-python-timeout", daemon=True
    )
    try:
        _install_execute_patch()
        watcher.start()
        if bridge is not None and bridge.isCanceled():
            raise RunPythonTimeout("Canceled.")
        yield
    finally:
        # Order matters: first no new interrupts, then drop a pending one, and only
        # then restore Processing, so the restore itself cannot be interrupted.
        with _lock:
            _session_active = False
        stop_watchdog.set()
        _clear_async(main_thread_id)
        try:
            _restore_execute_patch()
        finally:
            _active_bridge = None
            if watcher.is_alive():
                watcher.join(timeout=1.0)
