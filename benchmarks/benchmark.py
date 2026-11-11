#!/usr/bin/env python3
"""Benchmark st's terminal-output and redraw workloads."""

import argparse
import base64
import json
import os
import platform
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path


def write_all(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        view = view[written:]


def payload(cols):
    text = "The quick brown fox jumps over the lazy dog 0123456789"
    return (text * ((cols // len(text)) + 1))[:cols].encode() + b"\r\n"


def png_chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def kitty_png():
    width = height = 64
    row = b"\0" + bytes((40, 160, 220, 255)) * width
    image = b"\x89PNG\r\n\x1a\n"
    image += png_chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0))
    image += png_chunk(b"IDAT", zlib.compress(row * height))
    image += png_chunk(b"IEND", b"")
    return base64.b64encode(image)


def run_worker(args):
    cols, rows = args.cols, args.rows
    line = payload(cols)
    screen = b"\x1b[H" + b"".join(line for _ in range(rows))
    started = time.perf_counter()
    byte_count = 0

    if args.workload == "stream":
        target = args.mib * 1024 * 1024
        chunk = (line * max(1, 65536 // len(line)))[:65536]
        while byte_count < target:
            data = chunk[: min(len(chunk), target - byte_count)]
            write_all(1, data)
            byte_count += len(data)
    elif args.workload == "redraw":
        for _ in range(args.iterations):
            data = b"\x1b[2J" + screen
            write_all(1, data)
            byte_count += len(data)
    elif args.workload == "kitty":
        encoded = kitty_png()
        for image_id in range(1, args.iterations + 1):
            command = (b"\x1b_Ga=T,t=d,f=100,i=" + str(image_id).encode()
                       + b",c=4,r=4,q=2;" + encoded + b"\x1b\\")
            write_all(1, command)
            byte_count += len(command)
    else:
        batch = b"".join(line for _ in range(rows))
        for _ in range(args.iterations):
            write_all(1, batch)
            byte_count += len(batch)

    elapsed = time.perf_counter() - started
    time.sleep(args.settle_ms / 1000)
    Path(args.result).write_text(
        json.dumps({"seconds": elapsed, "bytes": byte_count}), encoding="utf-8"
    )
    return 0


def free_display():
    for number in range(90, 2000):
        if not Path(f"/tmp/.X11-unix/X{number}").exists():
            return number
    raise RuntimeError("No free X display number found")


def start_xvfb():
    xvfb = shutil.which("Xvfb")
    if not xvfb:
        raise RuntimeError("Xvfb not found; install it or use an existing DISPLAY")
    number = free_display()
    display = f":{number}"
    process = subprocess.Popen(
        [xvfb, display, "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    socket_path = Path(f"/tmp/.X11-unix/X{number}")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            error = process.stderr.read().decode(errors="replace")
            raise RuntimeError(f"Xvfb failed to start: {error.strip()}")
        if socket_path.exists():
            return display, process
        time.sleep(0.05)
    process.terminate()
    raise RuntimeError("Timed out waiting for Xvfb")


def run_trial(st, display, workload, args, directory, trial):
    result_path = Path(directory) / f"{workload}-{trial}.json"
    worker_args = [
        sys.executable,
        str(Path(__file__).resolve()),
        "--worker",
        "--workload", workload,
        "--cols", str(args.cols),
        "--rows", str(args.rows),
        "--mib", str(args.mib),
        "--iterations", str(args.iterations),
        "--settle-ms", str(args.settle_ms),
        "--result", str(result_path),
    ]
    env = os.environ.copy()
    env["DISPLAY"] = display
    started = time.perf_counter()
    process = subprocess.Popen(
        [st, "-g", f"{args.cols}x{args.rows}", "-e", *worker_args],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    try:
        _, stderr = process.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise RuntimeError(f"{workload} trial {trial} timed out")
    wall_seconds = time.perf_counter() - started
    if process.returncode:
        raise RuntimeError(
            f"st exited {process.returncode}: {stderr.decode(errors='replace').strip()}"
        )
    if not result_path.exists():
        raise RuntimeError("Workload did not write results before st exited")
    measured = json.loads(result_path.read_text(encoding="utf-8"))
    measured.update({"workload": workload, "trial": trial, "wall_seconds": wall_seconds})
    measured["mib_per_second"] = measured["bytes"] / measured["seconds"] / (1024 * 1024)
    return measured


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--st", default="st", help="st executable (default: st in PATH)")
    parser.add_argument("--xvfb", action="store_true", help="start isolated Xvfb display")
    workloads = ["stream", "redraw", "scroll", "kitty"]
    parser.add_argument("--workloads", nargs="+", choices=workloads,
                        default=["stream", "redraw", "scroll"])
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--mib", type=int, default=16, help="stream workload size per trial")
    parser.add_argument("--iterations", type=int, default=250, help="redraw/scroll cycles")
    parser.add_argument("--cols", type=int, default=120)
    parser.add_argument("--rows", type=int, default=40)
    parser.add_argument("--settle-ms", type=int, default=250)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--output", default="benchmark-results.json")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--workload", choices=workloads)
    parser.add_argument("--result")
    args = parser.parse_args()
    if args.worker:
        return args
    if min(args.repetitions, args.mib, args.iterations, args.cols, args.rows) < 1:
        parser.error("repetitions, mib, iterations, cols, and rows must be positive")
    if args.settle_ms < 0 or args.timeout < 1:
        parser.error("settle-ms must be non-negative and timeout must be positive")
    return args


def main():
    args = parse_args()
    if args.worker:
        return run_worker(args)
    st = shutil.which(args.st)
    if not st:
        raise RuntimeError(f"st executable not found: {args.st}")
    xvfb = None
    display = os.environ.get("DISPLAY")
    if args.xvfb:
        display, xvfb = start_xvfb()
    elif not display:
        raise RuntimeError("DISPLAY is unset; pass --xvfb or set DISPLAY")

    try:
        with tempfile.TemporaryDirectory(prefix="st-benchmark-") as directory:
            results = []
            for workload in args.workloads:
                for trial in range(1, args.repetitions + 1):
                    result = run_trial(st, display, workload, args, directory, trial)
                    results.append(result)
                    print(
                        f"{workload} {trial}/{args.repetitions}: "
                        f"{result['mib_per_second']:.2f} MiB/s generation, "
                        f"{result['wall_seconds']:.3f}s terminal lifetime"
                    )
            summary = {
                "system": platform.platform(),
                "machine": platform.machine(),
                "display": display,
                "st": st,
                "configuration": {
                    "cols": args.cols, "rows": args.rows, "mib": args.mib,
                    "iterations": args.iterations, "settle_ms": args.settle_ms,
                    "repetitions": args.repetitions,
                },
                "results": results,
                "medians": {},
            }
            for workload in args.workloads:
                group = [row for row in results if row["workload"] == workload]
                summary["medians"][workload] = {
                    "generation_mib_per_second": statistics.median(
                        row["mib_per_second"] for row in group
                    ),
                    "terminal_lifetime_seconds": statistics.median(
                        row["wall_seconds"] for row in group
                    ),
                }
            Path(args.output).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
            print(f"Results written to {args.output}")
    finally:
        if xvfb:
            xvfb.terminate()
            xvfb.wait(timeout=5)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"benchmark: {error}", file=sys.stderr)
        raise SystemExit(1)
