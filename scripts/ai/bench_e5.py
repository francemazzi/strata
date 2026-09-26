#!/usr/bin/env python3
"""Benchmark the local multilingual E5 embedding model the way Strata runs it.

Measures model load time, inference time and memory for a batch configuration,
in a fresh process per configuration so memory numbers are not polluted.

    python3 scripts/ai/bench_e5.py --model-dir ~/.../multilingual-e5-small-int8 \
        --threads 4 2 1 --batch 16 --seq 512 384

Needs the onnxruntime and numpy Python packages. Token ids are random: the cost
of the model does not depend on their values, only on batch size and length.
"""

import argparse
import json
import os
import resource
import subprocess
import sys
import time

MODEL_RELATIVE_PATH = os.path.join("onnx", "model_qint8_avx512_vnni.onnx")


def current_rss_mb(pid):
    out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)]).strip()
    return int(out) / 1024


def peak_rss_mb():
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    # ru_maxrss is in bytes on macOS and in kilobytes on Linux.
    return peak / 1e6 if sys.platform == "darwin" else peak / 1024


def run_one(model_path, threads, batch, seq, arena):
    import numpy as np
    import onnxruntime as ort

    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    options.intra_op_num_threads = threads
    options.enable_cpu_mem_arena = arena

    before = current_rss_mb(os.getpid())
    started = time.perf_counter()
    session = ort.InferenceSession(
        model_path, options, providers=["CPUExecutionProvider"]
    )
    load_ms = (time.perf_counter() - started) * 1000
    loaded = current_rss_mb(os.getpid())

    rng = np.random.default_rng(0)
    feed = {
        "input_ids": rng.integers(5, 250000, size=(batch, seq), dtype=np.int64),
        "attention_mask": np.ones((batch, seq), dtype=np.int64),
        "token_type_ids": np.zeros((batch, seq), dtype=np.int64),
    }
    names = {i.name for i in session.get_inputs()}
    feed = {k: v for k, v in feed.items() if k in names}

    started = time.perf_counter()
    session.run(None, feed)
    run_ms = (time.perf_counter() - started) * 1000
    return {
        "threads": threads,
        "batch": batch,
        "seq": seq,
        "arena": arena,
        "load_ms": round(load_ms),
        "run_ms": round(run_ms),
        "ms_per_chunk": round(run_ms / batch, 1),
        "model_mb": round(loaded - before),
        "peak_mb": round(peak_rss_mb()),
        "held_after_run_mb": round(current_rss_mb(os.getpid())),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--model-dir", required=True)
    parser.add_argument("--threads", type=int, nargs="+", default=[4, 2, 1])
    parser.add_argument("--batch", type=int, nargs="+", default=[16])
    parser.add_argument("--seq", type=int, nargs="+", default=[512, 384])
    parser.add_argument("--no-arena", action="store_true")
    parser.add_argument("--json", action="store_true", help="print JSON lines")
    parser.add_argument("--one", nargs=3, type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()

    model_path = os.path.join(os.path.expanduser(args.model_dir), MODEL_RELATIVE_PATH)
    if not os.path.exists(model_path):
        sys.exit(f"Model not found: {model_path}")

    if args.one:
        print(json.dumps(run_one(model_path, *args.one, not args.no_arena)))
        return

    for threads in args.threads:
        for batch in args.batch:
            for seq in args.seq:
                # One process per configuration: ONNX Runtime keeps its memory arena.
                command = [
                    sys.executable,
                    __file__,
                    "--model-dir",
                    args.model_dir,
                    "--one",
                    str(threads),
                    str(batch),
                    str(seq),
                ]
                if args.no_arena:
                    command.append("--no-arena")
                line = (
                    subprocess.check_output(command, text=True).strip().splitlines()[-1]
                )
                result = json.loads(line)
                if args.json:
                    print(line)
                else:
                    print(
                        f"threads={result['threads']} batch={result['batch']:3d} seq={result['seq']:3d}: "
                        f"{result['run_ms']:6d} ms ({result['ms_per_chunk']} ms/chunk) | "
                        f"load {result['load_ms']} ms, model +{result['model_mb']} MB, "
                        f"peak {result['peak_mb']} MB, held after run {result['held_after_run_mb']} MB"
                    )


if __name__ == "__main__":
    main()
