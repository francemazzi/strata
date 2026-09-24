"""Off-thread Processing.execute patch, Stop bridge, and run_python timeout."""

from __future__ import annotations

import ctypes
import threading
import time
from contextlib import contextmanager

from qgis.core import (
    Qgis,
    QgsApplication,
    QgsProcessingAlgorithm,
    QgsProcessingAlgRunnerTask,
    QgsProcessingException,
    QgsProcessingFeedback,
)
from qgis.PyQt.QtCore import QEventLoop, QThread

_original_algorithm_executor_execute = None
_original_processing_execute = None
_in_execute = threading.Event()
_interrupt_message = ""
_active_bridge = None


class RunPythonTimeout(BaseException):
    """Raised when run_python exceeds its budget or Stop is pressed outside Processing."""

    def __init__(self, message=None):
        super().__init__(message or _interrupt_message or "Python execution timed out.")


def _wrap_bridge(bridge_ptr):
    if not bridge_ptr:
        return None
    from qgis.PyQt.sip import wrapinstance

    return wrapinstance(int(bridge_ptr), QgsProcessingFeedback)


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


def _raise_async(thread_id, message):
    global _interrupt_message
    _interrupt_message = message
    ctypes.pythonapi.PyThreadState_SetAsyncExc(
        ctypes.c_ulong(thread_id), ctypes.py_object(RunPythonTimeout)
    )


def _clear_async(thread_id):
    ctypes.pythonapi.PyThreadState_SetAsyncExc(ctypes.c_ulong(thread_id), None)


def _patched_execute(
    alg, parameters, context=None, feedback=None, catch_exceptions=True
):
    if (
        QThread.currentThread() != QgsApplication.instance().thread()
        or _algorithm_is_no_threading(alg)
    ):
        return _original_algorithm_executor_execute(
            alg, parameters, context, feedback, catch_exceptions
        )

    bridge = _active_bridge
    if bridge is not None and bridge.isCanceled():
        raise QgsProcessingException("Canceled.")

    if feedback is None:
        feedback = QgsProcessingFeedback()
    if context is None:
        from processing.tools import dataobjects

        context = dataobjects.createContext(feedback)

    connections = []
    if bridge is not None:
        connections.append(bridge.canceled.connect(feedback.cancel))
        connections.append(feedback.progressChanged.connect(bridge.setProgress))

    _in_execute.set()
    try:
        task = QgsProcessingAlgRunnerTask(alg, parameters, context, feedback)
        if task.isCanceled():
            if not catch_exceptions:
                raise QgsProcessingException(
                    feedback.textLog() or "Processing task failed to start."
                )
            return False, {}

        loop = QEventLoop()
        outcome = {"ok": False, "results": {}}

        def on_executed(ok, results):
            outcome["ok"] = bool(ok)
            outcome["results"] = results
            loop.quit()

        task.executed.connect(on_executed)
        QgsApplication.taskManager().addTask(task)
        loop.exec()

        if not outcome["ok"] and not catch_exceptions:
            raise QgsProcessingException(
                feedback.textLog() or "There were errors executing the algorithm."
            )
        return outcome["ok"], outcome.get("results") or {}
    finally:
        _in_execute.clear()
        if bridge is not None:
            try:
                bridge.canceled.disconnect(feedback.cancel)
            except (TypeError, RuntimeError):
                pass
            try:
                feedback.progressChanged.disconnect(bridge.setProgress)
            except (TypeError, RuntimeError):
                pass
        del connections


def _install_execute_patch():
    global _original_algorithm_executor_execute, _original_processing_execute
    import processing.core.Processing as processing_module
    import processing.gui.AlgorithmExecutor as algorithm_executor

    if _original_algorithm_executor_execute is None:
        _original_algorithm_executor_execute = algorithm_executor.execute
        _original_processing_execute = getattr(processing_module, "execute", None)
    algorithm_executor.execute = _patched_execute
    processing_module.execute = _patched_execute


def _restore_execute_patch():
    global _original_algorithm_executor_execute, _original_processing_execute
    import processing.core.Processing as processing_module
    import processing.gui.AlgorithmExecutor as algorithm_executor

    if _original_algorithm_executor_execute is not None:
        algorithm_executor.execute = _original_algorithm_executor_execute
        if _original_processing_execute is not None:
            processing_module.execute = _original_processing_execute
    _original_algorithm_executor_execute = None
    _original_processing_execute = None


@contextmanager
def session(bridge_ptr, timeout_s=120):
    """Patch Processing.execute, honor Stop, and interrupt Python after timeout_s seconds."""
    global _active_bridge, _interrupt_message

    bridge = _wrap_bridge(bridge_ptr)
    _active_bridge = bridge
    _interrupt_message = ""
    _install_execute_patch()

    stop_watchdog = threading.Event()
    main_tid = threading.get_ident()
    budget = max(1.0, float(timeout_s))

    def watchdog():
        remaining = budget
        while not stop_watchdog.is_set() and remaining > 0:
            if bridge is not None and bridge.isCanceled() and not _in_execute.is_set():
                _raise_async(main_tid, "Canceled.")
                return
            if _in_execute.is_set():
                time.sleep(0.05)
                continue
            time.sleep(0.05)
            remaining -= 0.05
        if not stop_watchdog.is_set() and remaining <= 0:
            _raise_async(main_tid, f"stopped after {int(timeout_s)} s of Python")

    watcher = threading.Thread(
        target=watchdog, name="strata-ai-run-python-timeout", daemon=True
    )
    watcher.start()
    try:
        if bridge is not None and bridge.isCanceled():
            raise RunPythonTimeout("Canceled.")
        yield
    finally:
        stop_watchdog.set()
        watcher.join(timeout=1.0)
        _clear_async(main_tid)
        _restore_execute_patch()
        _active_bridge = None
        _in_execute.clear()
