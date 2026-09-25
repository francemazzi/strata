"""Scenario steps executed inside Strata by scripts/ai/run_scenarios.py.

Started with ``Strata --code scenario_inner.py --py-args CONFIG.json --``. It schedules the
scenario with timers (the interface keeps running), collects the "AI/Perf" and "AI/Index"
message log lines, writes them to the results file and quits Strata.
"""

import json
import os
import subprocess
import sys
import time

from qgis.core import (
    Qgis,
    QgsApplication,
    QgsMapRendererParallelJob,
    QgsProject,
    QgsRectangle,
    QgsVectorLayer,
)
from qgis.PyQt.QtCore import QObject, QSize, QTimer
from qgis.PyQt.QtWidgets import (
    QApplication,
    QLabel,
    QMessageBox,
    QPushButton,
    QTextEdit,
)
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
    elif tag == "AI" and message.startswith("Tool call: name="):
        _event("tool_call", tool=message.split("name=", 1)[1].split(" ", 1)[0])
        return
    elif tag == "AI" and message.startswith("Tool call finished: name="):
        _event(
            "tool_finished",
            tool=message.split("name=", 1)[1].split(" ", 1)[0],
            success=message.rstrip().endswith("success=1"),
        )
        return
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


def _chat(text, marker, then, mode="Ask before edits"):
    """Sends text through the chat panel in the given mode and calls then(reply_ms) once a
    reply containing marker is shown."""
    prompt = _widget("aiPromptInput")
    send = _widget("aiSendButton")
    if prompt is None or send is None:
        _quit("chat_widgets_missing")
        return
    # mode=None keeps the mode a new profile starts with.
    if mode is not None:
        pill = _widget("aiModePill")
        modes = [
            a
            for a in (pill.menu().actions() if pill is not None and pill.menu() else [])
            if a.text() == mode
        ]
        if not modes:
            _quit("chat_mode_missing")
            return
        modes[0].trigger()
    prompt.setPlainText(text)
    _event("chat_sent", marker=marker)
    sent_ms = _now_ms()
    replied = {"value": False}

    def answer_trust_question():
        # The first message in a workspace asks whether to trust it, in a modal message box.
        for widget in QApplication.topLevelWidgets():
            if isinstance(widget, QMessageBox) and widget.isVisible():
                _event("trust_question_answered")
                widget.reject()
                return
        if not replied["value"] and _now_ms() - sent_ms < 10000:
            _later(200, answer_trust_question)

    _later(200, answer_trust_question)
    send.click()

    def poll():
        if _STATE["done"] or marker is None:
            return
        texts = [w.text() for w in iface.mainWindow().findChildren(QLabel)]
        texts += [
            w.toPlainText()
            for w in iface.mainWindow().findChildren(QTextEdit)
            if w is not prompt
        ]
        if any(marker in text for text in texts):
            replied["value"] = True
            reply_ms = _now_ms() - sent_ms
            _event("chat_reply_shown", after_ms=reply_ms)
            then(reply_ms)
        elif _now_ms() - sent_ms > 180000:
            _quit("chat_reply_timeout")
        else:
            _later(200, poll)

    _later(200, poll)


def _send_chat_message():
    """The loopback provider answers with a search_workspace call, then with SCENARIO-REPLY."""
    _chat(
        "Which layers describe land use parcels?",
        "SCENARIO-REPLY",
        lambda reply_ms: _later(3000, lambda: _quit("done", chat_reply_ms=reply_ms)),
    )


def _first_prompt_buffer():
    """Phase 4: a new profile, no settings touched: the first prompt acts, and Undo this turn
    takes the result back."""
    pill = _widget("aiModePill")
    mode = pill.text().replace("▾", "").strip() if pill is not None else ""
    before = len(QgsProject.instance().mapLayers())
    _event("first_prompt", mode=mode, layers=before)

    def undo(reply_ms):
        after_buffer = len(QgsProject.instance().mapLayers())
        buttons = [
            b
            for b in iface.mainWindow().findChildren(QPushButton, "aiUndoTurnButton")
            if not b.isHidden()
        ]
        if not buttons:
            _quit(
                "undo_turn_missing",
                first_prompt_mode=mode,
                layers_before=before,
                layers_after_buffer=after_buffer,
            )
            return
        buttons[-1].click()

        def check():
            _quit(
                "done",
                first_prompt_mode=mode,
                layers_before=before,
                layers_after_buffer=after_buffer,
                layers_after_undo=len(QgsProject.instance().mapLayers()),
                chat_reply_ms=reply_ms,
            )

        _later(1500, check)

    _chat(
        "Buffer strato_00 by 100 m and add the result to the map.",
        "SCENARIO-BUFFER-DONE",
        lambda reply_ms: _later(1000, lambda: undo(reply_ms)),
        mode=None,
    )


def _tool_tour():
    """Phase 3: the loopback provider calls one tool after another on the dataset (layers, map,
    files, a new layer, a field calculation), then a slow calculation that the user stops."""

    def stop_slow_tool():
        send = _widget("aiSendButton")
        seen = {"value": False}

        def wait_for_tool():
            if _STATE["done"]:
                return
            calls = [
                e
                for e in _STATE["events"]
                if e["event"] == "tool_call" and e.get("tool") == "calculate_field"
            ]
            if len(calls) >= 2 and not seen["value"]:
                seen["value"] = True
                _later(300, press_stop)
            elif not seen["value"]:
                _later(50, wait_for_tool)

        def press_stop():
            _event("stop_pressed", running=send.text())
            pressed_ms = _now_ms()
            send.click()

            def wait_idle():
                if send.text() != "↑" and _now_ms() - pressed_ms < 30000:
                    _later(20, wait_idle)
                    return
                stop_ms = _now_ms() - pressed_ms
                _event("stopped", after_ms=stop_ms)
                finished = [
                    e for e in _STATE["events"] if e["event"] == "tool_finished"
                ]
                # Stopped for real only if the running tool ended without success.
                stopped_tool_succeeded = finished[-1]["success"] if finished else None
                _later(
                    2000,
                    lambda: _quit(
                        "done",
                        stop_ms=stop_ms,
                        stopped_tool_succeeded=stopped_tool_succeeded,
                    ),
                )

            _later(20, wait_idle)

        _chat(
            "Compute the buffered area of every boundary.",
            None,
            lambda _ms: None,
            mode="Agent",
        )
        _later(50, wait_for_tool)

    _chat(
        "Take a tour of the project tools.",
        "SCENARIO-TOOLS-DONE",
        lambda reply_ms: _later(1000, stop_slow_tool),
        mode="Agent",
    )


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
            # capture_map_canvas asks once whether map images may go to the provider: yes, as a
            # user trying the tool would answer.
            if "send images to vision-capable" in widget.text():
                yes = widget.button(QMessageBox.StandardButton.Yes)
                if yes is not None:
                    _event("image_consent_given")
                    yes.click()
    _STATE["message_boxes"] = shown
    if not _STATE["done"]:
        _later(250, _watch_message_boxes)


def _rss_mb():
    """Resident memory of Strata now (ps), in MB."""
    output = subprocess.check_output(
        ["ps", "-o", "rss=", "-p", str(os.getpid())], text=True
    )
    return round(int(output.split()[0]) / 1024)


def _count_perf(prefix, since=0):
    return sum(1 for _, line in _STATE["perf"][since:] if line.startswith(prefix))


INDEXING_TASKS = ("Index AI workspace", "Index AI layers")


def _watch_indexing():
    """Records when an indexing task first runs, when the chat shows it, and the CPU used meanwhile."""
    events = [e["event"] for e in _STATE["events"]]
    running = any(
        task.description() in INDEXING_TASKS
        for task in QgsApplication.taskManager().activeTasks()
    )
    if running and "indexing_task_started" not in events:
        _event("indexing_task_started")
        _STATE["cpu_at_start"] = (time.process_time(), time.monotonic())
    # The chat panel may be closed (and is, offscreen): what counts is the indicator itself.
    indicator = _widget("aiIndexingIndicator")
    if (
        indicator is not None
        and not indicator.isHidden()
        and "indicator_shown" not in events
    ):
        _event("indicator_shown", text=_widget("aiIndexingStatusButton").text())
    if not _STATE["done"]:
        _later(100, _watch_indexing)


def _indexing_cpu():
    """CPU used by Strata since indexing started, in percent of one core and of the whole computer."""
    if "cpu_at_start" not in _STATE:
        return {}
    cpu0, wall0 = _STATE["cpu_at_start"]
    busy = (time.process_time() - cpu0) / max(0.001, time.monotonic() - wall0) * 100
    return {
        "indexing_cpu_pct": round(busy),
        "indexing_cpu_total_pct": round(busy / (os.cpu_count() or 1), 1),
    }


def _time_map_refreshes(label, count, then):
    """Draws the project map count times, panning each time, and records the median time.

    A render job of fixed size, not the canvas widget: an offscreen canvas may have no size and
    never draw.
    """
    canvas = iface.mapCanvas()
    settings = canvas.mapSettings()
    settings.setOutputSize(QSize(1280, 800))
    settings.setLayers(QgsProject.instance().layerTreeRoot().checkedLayers())
    extent = QgsProject.instance().viewSettings().fullExtent()
    if extent.isEmpty():
        extent = canvas.extent()
    settings.setDestinationCrs(QgsProject.instance().crs())
    times = []
    jobs = []

    def one(index):
        shift = extent.width() * 0.05 * (1 if index % 2 else -1)
        settings.setExtent(
            QgsRectangle(
                extent.xMinimum() + shift,
                extent.yMinimum(),
                extent.xMaximum() + shift,
                extent.yMaximum(),
            )
        )
        job = QgsMapRendererParallelJob(settings)
        jobs.append(job)
        started = time.monotonic()
        done = {"value": False}

        def finished():
            if done["value"]:
                return
            done["value"] = True
            times.append(round((time.monotonic() - started) * 1000))
            if index + 1 < count:
                _later(150, lambda: one(index + 1))
            else:
                median = sorted(times)[len(times) // 2]
                _event(label, median_ms=median, all_ms=times)
                then(median)

        def too_slow():
            if not done["value"]:
                job.cancelWithoutBlocking()
                _event(label + "_render_timeout", index=index)
                finished()

        job.finished.connect(finished)
        job.start()
        _later(60000, too_slow)

    one(0)


def _run():
    scenario = _CONFIG["scenario"]
    _event("start", scenario=scenario)
    _watch_message_boxes()
    _watch_indexing()
    if scenario == "startup":
        _later(_CONFIG.get("wait_s", 25) * 1000, lambda: _quit("done"))
    elif scenario == "open_project":

        def step():
            started = _now_ms()
            _open_project()
            _wait_for_idle(started, lambda: _quit("done", **_indexing_cpu()))

        _later(2000, step)
    elif scenario == "reopen_project":
        # Opening the same unchanged project again must embed nothing.
        def step():
            started = _now_ms()
            _open_project()

            def reopen():
                _event(
                    "first_open_indexed",
                    layer_embeds=_count_perf("index_task embed_layer"),
                    file_embeds=_count_perf("index_task embed_files"),
                )
                QgsProject.instance().clear()
                _event("project_closed")
                mark = len(_STATE["perf"])
                reopened = _now_ms()
                _open_project()

                def done():
                    _quit(
                        "done",
                        reopen_layer_embeds=_count_perf("index_task embed_layer", mark),
                        reopen_file_embeds=_count_perf("index_task embed_files", mark),
                        reopen_unchanged_layers=_count_perf(
                            "index_task layer_unchanged", mark
                        ),
                    )

                _wait_for_idle(reopened, done)

            _wait_for_idle(started, reopen)

        _later(2000, step)
    elif scenario == "idle_memory":
        # After indexing, the model is released once idle (strata/index/model_idle_unload_s).
        def step():
            started = _now_ms()
            _open_project()
            rss_opened = _rss_mb()
            _event("rss", stage="project_opened", mb=rss_opened)

            def indexed():
                rss_indexed = _rss_mb()
                _event("rss", stage="indexed", mb=rss_indexed)

                def idle():
                    rss_idle = _rss_mb()
                    _event("rss", stage="idle", mb=rss_idle)
                    _quit(
                        "done",
                        rss_opened_mb=rss_opened,
                        rss_indexed_mb=rss_indexed,
                        rss_idle_mb=rss_idle,
                    )

                _later((_CONFIG.get("model_idle_s", 20) + 20) * 1000, idle)

            _wait_for_idle(started, indexed)

        _later(2000, step)
    elif scenario == "map_during_indexing":
        # Drawing the map while indexing runs, compared with drawing it once indexing is done.
        def step():
            started = _now_ms()
            _open_project()

            def during():
                def after(median_during):
                    def idle():
                        _time_map_refreshes(
                            "map_idle",
                            7,
                            lambda median_idle: _quit(
                                "done",
                                map_during_ms=median_during,
                                map_idle_ms=median_idle,
                            ),
                        )

                    _wait_for_idle(started, idle)

                _time_map_refreshes("map_during_indexing", 7, after)

            _when_indexing(during)

        _later(2000, step)
    elif scenario == "first_prompt_buffer":

        def step():
            _open_project()
            _later(3000, _first_prompt_buffer)

        _later(2000, step)
    elif scenario == "tools_on_dataset":

        def step():
            _open_project()
            _later(3000, _tool_tour)

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
