"""Scenario steps executed inside Strata by scripts/ai/run_scenarios.py.

Started with ``Strata --code scenario_inner.py --py-args CONFIG.json --``. It schedules the
scenario with timers (the interface keeps running), collects the "AI/Perf" and "AI/Index"
message log lines, writes them to the results file and quits Strata.
"""

import json
import os
import sys
import time

from qgis.core import Qgis, QgsApplication, QgsProject, QgsVectorLayer
from qgis.PyQt.QtCore import QObject, QTimer
from qgis.PyQt.QtWidgets import QApplication, QLabel, QMessageBox, QTextEdit
from qgis.utils import iface

_CONFIG = json.load(open(sys.argv[-1]))
_STATE = {
    "t0": time.monotonic(),
    "last_ai_message": time.monotonic(),
    "perf": [],
    "index": [],
    "events": [],
    "done": False,
}
_TIMERS = []


def _now_ms():
    return round((time.monotonic() - _STATE["t0"]) * 1000)


def _event(name, **extra):
    entry = {"at_ms": _now_ms(), "event": name}
    entry.update(extra)
    _STATE["events"].append(entry)


def _on_message(message, tag, level, *_rest):
    if tag == "AI/Perf":
        _STATE["perf"].append([_now_ms(), message])
    elif tag == "AI/Index":
        _STATE["index"].append([_now_ms(), message])
    else:
        return
    _STATE["last_ai_message"] = time.monotonic()


QgsApplication.messageLog().messageReceivedWithFormat.connect(_on_message)


def _later(ms, callback):
    timer = QTimer()
    timer.setSingleShot(True)
    timer.timeout.connect(callback)
    timer.start(ms)
    _TIMERS.append(timer)


def _write_results(**extra):
    results = {
        "scenario": _CONFIG["scenario"],
        "elapsed_ms": _now_ms(),
        "events": _STATE["events"],
        "perf": _STATE["perf"],
        "index": _STATE["index"],
    }
    results.update(extra)
    tmp = _CONFIG["results"] + ".tmp"
    with open(tmp, "w") as f:
        json.dump(results, f, indent=1)
    os.replace(tmp, _CONFIG["results"])


def _quit(reason, **extra):
    if _STATE["done"]:
        return
    _STATE["done"] = True
    _event("quit_requested", reason=reason)
    _write_results(quit_requested_wall=time.time(), **extra)
    QgsProject.instance().setDirty(False)
    iface.actionExit().trigger()


def _wait_for_idle(started_ms, then):
    """Calls then() once indexing looks finished, or when the scenario time budget runs out."""
    idle_s = _CONFIG.get("idle_s", 10)
    max_s = _CONFIG.get("max_s", 240)

    def poll():
        if _STATE["done"]:
            return
        quiet_for = time.monotonic() - _STATE["last_ai_message"]
        running = QgsApplication.taskManager().countActiveTasks()
        elapsed_s = (_now_ms() - started_ms) / 1000
        # The layer coordinator waits up to 15 s (bulk debounce) before its first flush.
        if elapsed_s > 20 and quiet_for > idle_s and running == 0:
            _event("idle", after_s=round(elapsed_s, 1))
            then()
        elif elapsed_s > max_s:
            _event("max_time_reached", after_s=round(elapsed_s, 1))
            then()
        else:
            _later(500, poll)

    _later(500, poll)


def _open_project():
    _event("open_project")
    iface.addProject(os.path.join(_CONFIG["dataset"], "project_100.qgz"))
    _event("project_opened", layers=len(QgsProject.instance().mapLayers()))


def _add_layers():
    extra = os.path.join(_CONFIG["dataset"], "extra")
    _event("add_layers")
    for name in sorted(os.listdir(extra)):
        if name.endswith(".shp"):
            layer = QgsVectorLayer(os.path.join(extra, name), name[:-4], "ogr")
            QgsProject.instance().addMapLayer(layer)
    _event("layers_added", layers=len(QgsProject.instance().mapLayers()))


def _widget(name):
    return iface.mainWindow().findChild(QObject, name)


def _when_indexing(then):
    """Calls then() once background indexing has started embedding."""

    def poll():
        if _STATE["done"]:
            return
        embedding = any("index_task" in line for _, line in _STATE["perf"])
        tasks_running = QgsApplication.taskManager().countActiveTasks() > 0
        if embedding or (tasks_running and _now_ms() > 20000):
            _event("indexing_seen")
            then()
        elif _now_ms() > _CONFIG.get("max_s", 240) * 1000:
            _quit("no_indexing_seen")
        else:
            _later(200, poll)

    _later(200, poll)


def _send_chat_message():
    """Sends a chat message through the chat panel; the loopback provider answers with a
    search_workspace call, then with a text reply containing SCENARIO-REPLY."""
    prompt = _widget("aiPromptInput")
    send = _widget("aiSendButton")
    if prompt is None or send is None:
        _quit("chat_widgets_missing")
        return
    # New profiles start in Plan mode, which runs no tools: pick a mode that runs read-only ones.
    pill = _widget("aiModePill")
    modes = [
        a
        for a in (pill.menu().actions() if pill is not None and pill.menu() else [])
        if a.text() == "Ask before edits"
    ]
    if not modes:
        _quit("chat_mode_missing")
        return
    modes[0].trigger()
    prompt.setPlainText("Which layers describe land use parcels?")
    _event("chat_sent")
    sent_ms = _now_ms()

    def answer_trust_question():
        # The first message in a workspace asks whether to trust it, in a modal message box.
        for widget in QApplication.topLevelWidgets():
            if isinstance(widget, QMessageBox) and widget.isVisible():
                _event("trust_question_answered")
                widget.reject()
                return
        if (
            "chat_reply_shown" not in [e["event"] for e in _STATE["events"]]
            and _now_ms() - sent_ms < 10000
        ):
            _later(200, answer_trust_question)

    _later(200, answer_trust_question)
    send.click()

    def poll():
        if _STATE["done"]:
            return
        texts = [w.text() for w in iface.mainWindow().findChildren(QLabel)]
        texts += [
            w.toPlainText()
            for w in iface.mainWindow().findChildren(QTextEdit)
            if w is not prompt
        ]
        if any("SCENARIO-REPLY" in text for text in texts):
            reply_ms = _now_ms() - sent_ms
            _event("chat_reply_shown", after_ms=reply_ms)
            _later(3000, lambda: _quit("done", chat_reply_ms=reply_ms))
        elif _now_ms() - sent_ms > 120000:
            _quit("chat_reply_timeout")
        else:
            _later(200, poll)

    _later(200, poll)


def _accept_ai_settings():
    """Opens the AI settings from the chat panel and presses OK."""
    button = _widget("aiProviderSettingsButton")
    if button is None:
        _quit("settings_button_missing")
        return
    _event("settings_open")
    # The dialog runs its own event loop: click from a timer so this callback returns first.
    _later(0, button.click)
    opened_ms = _now_ms()

    def poll():
        if _STATE["done"]:
            return
        dialogs = [
            w
            for w in QApplication.topLevelWidgets()
            if w.objectName() == "aiSettingsDialog" and w.isVisible()
        ]
        # OK asks for consent to layer indexing: answer as a user keeping the defaults would.
        for box in QApplication.topLevelWidgets():
            if (
                isinstance(box, QMessageBox)
                and box.isVisible()
                and box.button(QMessageBox.StandardButton.Yes)
            ):
                _event("settings_question_answered", text=box.text()[:120])
                box.button(QMessageBox.StandardButton.Yes).click()
        if dialogs and "settings_shown" not in [e["event"] for e in _STATE["events"]]:
            _event("settings_shown", after_ms=_now_ms() - opened_ms)
            _later(1000, dialogs[0].accept)
            _later(200, poll)
        elif not dialogs and "settings_shown" in [e["event"] for e in _STATE["events"]]:
            _event("settings_accepted")
            _later(10000, lambda: _quit("done"))
        elif _now_ms() - opened_ms > 60000:
            _quit("settings_timeout")
        else:
            _later(200, poll)

    _later(200, poll)


def _watch_message_boxes():
    """Records every message box shown during the scenario: a modal one would stall it."""
    shown = set()
    for widget in QApplication.topLevelWidgets():
        if isinstance(widget, QMessageBox) and widget.isVisible():
            shown.add(widget.text())
            if widget.text() not in _STATE.setdefault("message_boxes", set()):
                _event(
                    "message_box", title=widget.windowTitle(), text=widget.text()[:300]
                )
    _STATE["message_boxes"] = shown
    if not _STATE["done"]:
        _later(250, _watch_message_boxes)


def _run():
    scenario = _CONFIG["scenario"]
    _event("start", scenario=scenario)
    _watch_message_boxes()
    if scenario == "startup":
        _later(_CONFIG.get("wait_s", 25) * 1000, lambda: _quit("done"))
    elif scenario == "open_project":

        def step():
            started = _now_ms()
            _open_project()
            _wait_for_idle(started, lambda: _quit("done"))

        _later(2000, step)
    elif scenario == "add_layers":

        def step():
            started = _now_ms()
            _add_layers()
            _wait_for_idle(started, lambda: _quit("done"))

        _later(2000, step)
    elif scenario == "quit_during_indexing":

        def step():
            _open_project()
            _when_indexing(lambda: _quit("quit_during_indexing"))

        _later(2000, step)
    elif scenario == "chat_during_indexing":

        def step():
            _open_project()
            _when_indexing(_send_chat_message)

        _later(2000, step)
    elif scenario == "ai_settings_ok":

        def step():
            _open_project()
            _when_indexing(_accept_ai_settings)

        _later(2000, step)
    else:
        _quit(f"unknown scenario {scenario}")

    # Hard stop if the scenario never ends on its own.
    _later(_CONFIG.get("max_s", 240) * 1000 + 30000, lambda: _quit("hard_timeout"))


_later(0, _run)
