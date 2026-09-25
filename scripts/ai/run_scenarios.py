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
]
HERE = os.path.dirname(os.path.abspath(__file__))
STALL_RE = re.compile(r"^gui_stall ms=(\d+) during=(.*)$")

# Pass/fail thresholds per roadmap phase (see roadmap_ottimizzazione/00_ROADMAP.md).
CHECKS = {
    "phase1": {"max_ai_stall_ms": 200, "max_exit_ms": 5000},
    "phase2": {"max_ai_stall_ms": 200, "max_exit_ms": 5000, "max_avg_cpu_pct": 400},
}


class LoopbackChatHandler(BaseHTTPRequestHandler):
    """OpenAI-compatible chat completions: a search_workspace call first, then a text reply."""

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(length) or b"{}")
        self.server.requests += 1
        if any(message.get("role") == "tool" for message in body.get("messages", [])):
            message = {
                "role": "assistant",
                "content": "SCENARIO-REPLY: the parcels layers are indexed.",
            }
            finish = "stop"
        else:
            call = {
                "id": "call_scenario_search",
                "type": "function",
                "function": {
                    "name": "search_workspace",
                    "arguments": json.dumps({"query": "land use parcels"}),
                },
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


def start_loopback_chat():
    server = ThreadingHTTPServer(("127.0.0.1", 0), LoopbackChatHandler)
    server.requests = 0
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
    chat_server = start_loopback_chat()
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
            # Tools are off for new profiles until fix_primo_prompt_agisce (roadmap phase 4).
            "[strata]\nagent\\allow_custom_actions=true\n"
        )

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

    summary = {
        "scenario": scenario,
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
        if "chat_reply_ms" in results:
            summary["chat_reply_ms"] = results["chat_reply_ms"]
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
    if (
        "max_avg_cpu_pct" in thresholds
        and summary.get("cpu_avg_pct", 0) > thresholds["max_avg_cpu_pct"]
    ):
        problems.append(
            f"average CPU {summary['cpu_avg_pct']}% > {thresholds['max_avg_cpu_pct']}%"
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
