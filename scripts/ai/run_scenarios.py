#!/usr/bin/env python3
"""Run AI performance scenarios in a real Strata build and check them against phase thresholds.

Each scenario starts the built Strata with a throw-away profile (never the user's), the local
E5 model and the interface stall monitor, drives it with scripts/ai/scenario_inner.py, and
samples memory and CPU from outside. Results go to OUT_DIR/<scenario>.json and a summary to
OUT_DIR/summary.json.

    python3 scripts/ai/make_perf_dataset.py /tmp/strata-perf-dataset   # once, with PyQGIS
    python3 scripts/ai/run_scenarios.py --build /Volumes/.../build --dataset /tmp/strata-perf-dataset \\
        --model-dir /Volumes/.../multilingual-e5-small-int8 --out /tmp/strata-perf-results --check phase1

Scenarios: startup, open_project, add_layers, ai_settings_ok, chat_during_indexing,
quit_during_indexing. The chat talks to a loopback provider started by this script.
"""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCENARIOS = [
    "startup",
    "open_project",
    "add_layers",
    "ai_settings_ok",
    "chat_during_indexing",
    "quit_during_indexing",
    "reopen_project",
    "idle_memory",
    "map_during_indexing",
    "tools_on_dataset",
    "first_prompt_buffer",
]
#: Extra lines of the [strata] settings group, per scenario.
SCENARIO_SETTINGS = {
    "idle_memory": ["index\\model_idle_unload_s=20"],
}
HERE = os.path.dirname(os.path.abspath(__file__))
STALL_RE = re.compile(r"^gui_stall ms=(\d+) during=(.*)$")

# Pass/fail thresholds per roadmap phase (see roadmap_ottimizzazione/00_ROADMAP.md).
CHECKS = {
    "phase1": {"max_ai_stall_ms": 200, "max_exit_ms": 5000},
    "phase2": {
        "max_ai_stall_ms": 200,
        "max_exit_ms": 5000,
        "max_reopen_embeds": 0,
        "max_idle_rss_growth_mb": 100,
        "max_map_slowdown": 1.2,
        "max_indicator_delay_ms": 1000,
        "max_indexing_cpu_total_pct": 50,
    },
    "phase3": {
        "max_ai_stall_ms": 200,
        "max_exit_ms": 5000,
        "max_stop_ms": 1000,
        "max_tool_errors": 0,
    },
    "phase4": {
        "max_ai_stall_ms": 200,
        "max_exit_ms": 5000,
        "max_tool_errors": 0,
        "first_prompt_mode": "Agent",
    },
}
#: Copy of the heavy layer the tool tour adds and edits, so the dataset itself never changes.
TOUR_LAYER = os.path.join("data", "scenario_tour_copy.gpkg")


def _find_layer_id(content, name):
    """Id of the layer called name in a tool result (list_project_layers, add_layer_from_file)."""
    try:
        data = json.loads(content)
    except ValueError:
        return None
    stack = [data]
    while stack:
        item = stack.pop()
        if isinstance(item, dict):
            if item.get("name") == name or item.get("layer_name") == name:
                found = item.get("id") or item.get("layer_id")
                if found:
                    return found
            stack.extend(item.values())
        elif isinstance(item, list):
            stack.extend(item)
    return None


def tool_tour_reply(server, body):
    """Phase 3 script: one tool per round on the dataset, then a slow one the user stops."""
    messages = body.get("messages", [])

    def prompt_index(text):
        # The map image comes back as a user message too: find the turn by its prompt.
        for i in range(len(messages) - 1, -1, -1):
            if messages[i].get("role") == "user" and text in json.dumps(
                messages[i].get("content")
            ):
                return i
        return None

    second = prompt_index("Compute the buffered area")
    start = second if second is not None else prompt_index("Take a tour") or 0
    results = [
        m.get("content") or "" for m in messages[start + 1 :] if m.get("role") == "tool"
    ]
    results = [r if isinstance(r, str) else json.dumps(r) for r in results]
    if results:
        server.tool_results.append(results[-1])
    state = server.state
    step = len(results)
    if second is None:
        if step == 1:
            state["heavy_id"] = _find_layer_id(results[-1], "confini_dettagliati")
        if step == 6:
            state["copy_id"] = _find_layer_id(results[-1], "confini_copia")
        plan = [
            ("list_project_layers", {}),
            (
                "describe_layer",
                {"layer_id": state.get("heavy_id") or "", "sample_features": 3},
            ),
            ("capture_map_canvas", {}),
            ("search_files", {"query": "alberi"}),
            ("list_files", {}),
            (
                "add_layer_from_file",
                {"path": state["copy_path"], "name": "confini_copia"},
            ),
            (
                "calculate_field",
                {
                    "layer_id": state.get("copy_id") or "",
                    "field_name": "strata_perimeter",
                    "expression": "$perimeter",
                    "create_field": True,
                    "field_type": "double",
                },
            ),
        ]
        if step < len(plan):
            return plan[step]
        return "SCENARIO-TOOLS-DONE: every tool answered."
    if step == 0:
        # Seconds of GEOS work (20 buffers of 200 polygons of 20000 vertices): the user presses Stop.
        return (
            "calculate_field",
            {
                "layer_id": state.get("copy_id") or "",
                "field_name": "strata_buffer_area",
                "expression": "array_sum(array_foreach(generate_series(1, 20), area(buffer($geometry, @element))))",
                "create_field": True,
                "field_type": "double",
            },
        )
    return "SCENARIO-STOP-REPLY: stopped."


def first_prompt_reply(server, body):
    """Phase 4: "buffer a layer by 100 m" from a new profile, answered with the tools it needs."""
    messages = body.get("messages", [])
    results = [m.get("content") or "" for m in messages if m.get("role") == "tool"]
    results = [r if isinstance(r, str) else json.dumps(r) for r in results]
    if results:
        server.tool_results.append(results[-1])
    if len(results) == 0:
        return ("list_project_layers", {})
    if len(results) == 1:
        layer = _find_layer_id(results[0], "strato_00") or ""
        return (
            "run_processing_algorithm",
            {
                "algorithm_id": "native:buffer",
                "parameters": {
                    "INPUT": layer,
                    "DISTANCE": 100,
                    "OUTPUT": "TEMPORARY_OUTPUT",
                },
            },
        )
    return "SCENARIO-BUFFER-DONE: the buffer is on the map."


def search_workspace_reply(server, body):
    """A search_workspace call first, then a text reply."""
    if any(message.get("role") == "tool" for message in body.get("messages", [])):
        return "SCENARIO-REPLY: the parcels layers are indexed."
    return ("search_workspace", {"query": "land use parcels"})


#: Loopback chat script per scenario.
CHAT_SCRIPTS = {
    "tools_on_dataset": tool_tour_reply,
    "first_prompt_buffer": first_prompt_reply,
}


class LoopbackChatHandler(BaseHTTPRequestHandler):
    """OpenAI-compatible chat completions answered by the scenario's script: a tool call as
    (name, arguments), or a text reply."""

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(length) or b"{}")
        self.server.requests += 1
        reply = self.server.script(self.server, body)
        if isinstance(reply, str):
            message = {"role": "assistant", "content": reply}
            finish = "stop"
        else:
            call = {
                "id": f"call_scenario_{self.server.requests}",
                "type": "function",
                "function": {"name": reply[0], "arguments": json.dumps(reply[1])},
            }
            message = {"role": "assistant", "content": None, "tool_calls": [call]}
            finish = "tool_calls"
        payload = json.dumps(
            {
                "id": f"scenario-{self.server.requests}",
                "model": "test/model",
                "choices": [{"index": 0, "message": message, "finish_reason": finish}],
                "usage": {"prompt_tokens": 10, "completion_tokens": 5},
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


def start_loopback_chat(script=search_workspace_reply, state=None):
    server = ThreadingHTTPServer(("127.0.0.1", 0), LoopbackChatHandler)
    server.requests = 0
    server.script = script
    server.state = state or {}
    server.tool_results = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def app_binary(build):
    for candidate in (
        os.path.join(build, "output", "Contents", "MacOS", "Strata"),
        os.path.join(build, "output", "bin", "Strata"),
        os.path.join(build, "output", "bin", "strata"),
    ):
        if os.path.exists(candidate):
            return candidate
    sys.exit(f"No Strata binary under {build}/output")


def sample(pid):
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=,%cpu=", "-p", str(pid)], text=True
        ).split()
        return int(out[0]) / 1024, float(out[1])
    except (subprocess.CalledProcessError, IndexError, ValueError):
        return None


def run_scenario(args, scenario):
    profile_dir = tempfile.mkdtemp(prefix=f"strata-perf-{scenario}-")
    results_path = os.path.join(args.out, f"{scenario}.json")
    if os.path.exists(results_path):
        os.remove(results_path)
    config_path = os.path.join(profile_dir, "scenario.json")
    with open(config_path, "w") as f:
        json.dump(
            {
                "scenario": scenario,
                "dataset": os.path.abspath(args.dataset),
                "results": results_path,
                "max_s": args.max_s,
                "idle_s": args.idle_s,
            },
            f,
        )

    # A fresh profile folder would offer to import the QGIS profiles found on this computer
    # with a modal dialog: record that decision up front.
    os.makedirs(os.path.join(profile_dir, "profiles"), exist_ok=True)
    with open(os.path.join(profile_dir, "profiles", "profiles.ini"), "w") as f:
        f.write("[strata]\nqgisImport\\decisionMade=true\n")
    # Never touch the user's system keychain: a new profile would store a generated master
    # password there, and wait on the keychain without a visible window.
    settings_dir = os.path.join(profile_dir, "profiles", "default", "getstrata.org")
    os.makedirs(settings_dir, exist_ok=True)
    # The chat uses the OpenRouter provider pointed at a loopback server, with a fake key
    # passed through the environment so nothing is stored.
    tour_copy = None
    state = {}
    if scenario == "tools_on_dataset":
        # Inside the workspace, so the file tools accept it; removed afterwards.
        tour_copy = os.path.join(os.path.abspath(args.dataset), TOUR_LAYER)
        shutil.copyfile(
            os.path.join(os.path.abspath(args.dataset), "data", "heavy.gpkg"), tour_copy
        )
        state["copy_path"] = tour_copy
    chat_server = start_loopback_chat(
        CHAT_SCRIPTS.get(scenario, search_workspace_reply), state
    )
    endpoint = (
        f"http://127.0.0.1:{chat_server.server_address[1]}/api/v1/chat/completions"
    )
    with open(os.path.join(settings_dir, "Strata.ini"), "w") as f:
        f.write(
            "[authentication]\nuse-password-helper=false\ngenerate-random-password-for-keychain=false\n"
        )
        f.write(
            "[ai]\nactiveProvider=OpenRouter\n"
            f"provider\\openrouter\\endpoint={endpoint}\n"
            "provider\\openrouter\\model=test/model\n"
            "provider\\openrouter\\enabled=true\n"
            "provider\\openrouter\\defaultModelMigrated_v1=true\n"
            "network\\maxRetries=0\n"
            # New profiles have tools on and start in Agent mode: the scenarios use those defaults.
            "[strata]\n"
        )
        for line in SCENARIO_SETTINGS.get(scenario, []):
            f.write(line + "\n")

    env = dict(os.environ)
    env["STRATA_AI_GUI_STALL_MS"] = str(args.stall_ms)
    env["STRATA_AI_EMBEDDING_MODEL_DIR"] = os.path.abspath(args.model_dir)
    env["STRATA_AI_NO_KEYCHAIN"] = "1"
    env["OPENROUTER_API_KEY"] = "sk-or-scenario-loopback"
    if not args.visible:
        env["QT_QPA_PLATFORM"] = "offscreen"
    command = [
        app_binary(args.build),
        "--profiles-path",
        profile_dir,
        "--profile",
        "default",
        "--nologo",
        "--code",
        os.path.join(HERE, "scenario_inner.py"),
        "--py-args",
        config_path,
        "--",
    ]

    log_path = os.path.join(args.out, f"{scenario}.log")
    started = time.time()
    with open(log_path, "w") as log:
        process = subprocess.Popen(
            command, env=env, stdout=log, stderr=subprocess.STDOUT
        )
        rss_max, cpu_samples, exit_wall, timed_out = 0.0, [], None, False
        deadline = started + args.max_s + 90
        while process.poll() is None:
            measured = sample(process.pid)
            if measured:
                rss_max = max(rss_max, measured[0])
                cpu_samples.append(measured[1])
            if time.time() > deadline:
                timed_out = True
                process.send_signal(signal.SIGKILL)
                break
            time.sleep(0.5)
        process.wait()
        exit_wall = time.time()
    chat_server.shutdown()
    if tour_copy:
        for suffix in ("", "-wal", "-shm"):
            if os.path.exists(tour_copy + suffix):
                os.remove(tour_copy + suffix)

    summary = {
        "scenario": scenario,
        "cpu_count": os.cpu_count(),
        "returncode": process.returncode,
        "timed_out": timed_out,
        "wall_s": round(exit_wall - started, 1),
        "rss_max_mb": round(rss_max),
        "cpu_avg_pct": round(sum(cpu_samples) / len(cpu_samples)) if cpu_samples else 0,
        "log": log_path,
    }
    if not os.path.exists(results_path):
        summary["error"] = "scenario wrote no results (crash or blocking dialog?)"
    else:
        with open(results_path) as f:
            results = json.load(f)
        stalls = []
        for at_ms, line in results["perf"]:
            match = STALL_RE.match(line)
            if match:
                stalls.append(
                    {
                        "at_ms": at_ms,
                        "ms": int(match.group(1)),
                        "during": match.group(2),
                    }
                )
        ai_stalls = [s for s in stalls if s["during"] != "unknown"]
        summary.update(
            {
                "events": [e["event"] for e in results["events"]],
                "quit_reason": next(
                    (
                        e.get("reason")
                        for e in results["events"]
                        if e["event"] == "quit_requested"
                    ),
                    None,
                ),
                "stalls_total": len(stalls),
                "max_stall_ms": max((s["ms"] for s in stalls), default=0),
                "max_ai_stall_ms": max((s["ms"] for s in ai_stalls), default=0),
                "worst_ai_stalls": sorted(ai_stalls, key=lambda s: -s["ms"])[:8],
                "layer_embeds": sum(
                    1
                    for _, line in results["perf"]
                    if line.startswith("index_task embed_layer")
                ),
                "file_embeds": sum(
                    1
                    for _, line in results["perf"]
                    if line.startswith("index_task embed_files")
                ),
                "chat_requests": chat_server.requests,
            }
        )
        if scenario in CHAT_SCRIPTS:
            errors = [
                r[:300] for r in chat_server.tool_results if r.startswith('{"error"')
            ]
            summary["tool_results"] = len(chat_server.tool_results)
            summary["tool_errors"] = errors
            summary["tool_calls"] = [
                e.get("tool") for e in results["events"] if e["event"] == "tool_call"
            ]
        for key in (
            "chat_reply_ms",
            "indexing_cpu_pct",
            "indexing_cpu_total_pct",
            "reopen_layer_embeds",
            "reopen_file_embeds",
            "reopen_unchanged_layers",
            "rss_opened_mb",
            "rss_indexed_mb",
            "rss_idle_mb",
            "map_during_ms",
            "map_idle_ms",
            "stop_ms",
            "stopped_tool_succeeded",
            "first_prompt_mode",
            "layers_before",
            "layers_after_buffer",
            "layers_after_undo",
        ):
            if key in results:
                summary[key] = results[key]
        times = {
            e["event"]: e["at_ms"]
            for e in results["events"]
            if e["event"] in ("indexing_task_started", "indicator_shown")
        }
        if len(times) == 2:
            summary["indicator_delay_ms"] = (
                times["indicator_shown"] - times["indexing_task_started"]
            )
        if "quit_requested_wall" in results:
            summary["exit_ms"] = round(
                (exit_wall - results["quit_requested_wall"]) * 1000
            )
    if not args.keep_profiles:
        shutil.rmtree(profile_dir, ignore_errors=True)
    return summary


def check(summary, thresholds):
    problems = []
    if summary.get("timed_out"):
        problems.append("timed out")
    if summary.get("returncode") not in (0, None):
        problems.append(f"exit code {summary['returncode']}")
    if "error" in summary:
        problems.append(summary["error"])
    if summary.get("quit_reason") not in (None, "done", "quit_during_indexing"):
        problems.append(f"scenario stopped: {summary['quit_reason']}")
    if summary.get("max_ai_stall_ms", 0) > thresholds["max_ai_stall_ms"]:
        problems.append(
            f"AI stall {summary['max_ai_stall_ms']} ms > {thresholds['max_ai_stall_ms']} ms"
        )
    if summary.get("exit_ms", 0) > thresholds["max_exit_ms"]:
        problems.append(
            f"exit took {summary['exit_ms']} ms > {thresholds['max_exit_ms']} ms"
        )
    if "reopen_layer_embeds" in summary and "max_reopen_embeds" in thresholds:
        embeds = summary["reopen_layer_embeds"] + summary.get("reopen_file_embeds", 0)
        if embeds > thresholds["max_reopen_embeds"]:
            problems.append(f"reopening embedded {embeds} times")
    if "rss_idle_mb" in summary and "max_idle_rss_growth_mb" in thresholds:
        growth = summary["rss_idle_mb"] - summary["rss_opened_mb"]
        if growth > thresholds["max_idle_rss_growth_mb"]:
            problems.append(
                f"idle memory {growth} MB above the opened project > {thresholds['max_idle_rss_growth_mb']} MB"
            )
    if "map_during_ms" in summary and "max_map_slowdown" in thresholds:
        slowdown = summary["map_during_ms"] / max(1, summary["map_idle_ms"])
        if slowdown > thresholds["max_map_slowdown"]:
            problems.append(
                f"map drawn {slowdown:.2f}x slower during indexing > {thresholds['max_map_slowdown']}x"
            )
    if "indicator_delay_ms" in summary and "max_indicator_delay_ms" in thresholds:
        if summary["indicator_delay_ms"] > thresholds["max_indicator_delay_ms"]:
            problems.append(
                f"indicator shown {summary['indicator_delay_ms']} ms after indexing started"
            )
    if (
        "indexing_cpu_total_pct" in summary
        and "max_indexing_cpu_total_pct" in thresholds
        and summary["indexing_cpu_total_pct"] > thresholds["max_indexing_cpu_total_pct"]
    ):
        problems.append(
            f"indexing used {summary['indexing_cpu_total_pct']}% of the computer > {thresholds['max_indexing_cpu_total_pct']}%"
        )
    if "stop_ms" in summary and "max_stop_ms" in thresholds:
        if summary["stop_ms"] > thresholds["max_stop_ms"]:
            problems.append(
                f"Stop took {summary['stop_ms']} ms > {thresholds['max_stop_ms']} ms"
            )
        if summary.get("stopped_tool_succeeded") is not False:
            problems.append("Stop did not stop the running tool")
    if "first_prompt_mode" in thresholds and "first_prompt_mode" in summary:
        if summary["first_prompt_mode"] != thresholds["first_prompt_mode"]:
            problems.append(
                f"a new profile starts in {summary['first_prompt_mode']} mode"
            )
        if summary.get("layers_after_buffer") != summary.get("layers_before", -1) + 1:
            problems.append("the buffer did not add its layer")
        if summary.get("layers_after_undo") != summary.get("layers_before"):
            problems.append("Undo this turn did not remove the buffer")
    if "tool_errors" in summary and "max_tool_errors" in thresholds:
        if len(summary["tool_errors"]) > thresholds["max_tool_errors"]:
            problems.append(
                f"{len(summary['tool_errors'])} tools failed: {summary['tool_errors']}"
            )
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--model-dir", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--scenarios", nargs="+", default=SCENARIOS, choices=SCENARIOS)
    parser.add_argument("--stall-ms", type=int, default=100)
    parser.add_argument("--max-s", type=int, default=240)
    parser.add_argument("--idle-s", type=int, default=10)
    parser.add_argument("--check", choices=sorted(CHECKS))
    parser.add_argument(
        "--visible", action="store_true", help="show the window instead of offscreen"
    )
    parser.add_argument("--keep-profiles", action="store_true")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    summaries, failed = [], False
    for scenario in args.scenarios:
        summary = run_scenario(args, scenario)
        if args.check:
            summary["problems"] = check(summary, CHECKS[args.check])
            failed = failed or bool(summary["problems"])
        summaries.append(summary)
        print(json.dumps({k: v for k, v in summary.items() if k != "worst_ai_stalls"}))
        for stall in summary.get("worst_ai_stalls", [])[:3]:
            print(f"    stall {stall['ms']} ms during {stall['during']}")

    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(summaries, f, indent=1)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
