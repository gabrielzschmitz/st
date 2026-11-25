#!/usr/bin/env python3
"""Benchmark st terminal output, redraws, and frame submission timing."""

import argparse
import base64
import json
import os
import platform
import resource
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

TRACE_MAGIC = 0x53544254
TRACE_HEADER = struct.Struct("<4I")
TRACE_RECORDS = {
    1: struct.Struct("<5Q"),
    2: struct.Struct("<6Q"),
    3: struct.Struct("<7Q"),
}


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


def capture_window(capture, title):
    command = [capture, "--title", title, "--stats"]
    completed = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10
    )
    if completed.returncode:
        raise RuntimeError(
            f"capture failed: {completed.stderr.decode(errors='replace').strip()}"
        )
    return json.loads(completed.stdout)


def test_pattern(rows):
    colors = (31, 32, 33, 34, 35, 36, 37, 91, 92, 93)
    lines = []
    for row in range(rows):
        color = colors[row % len(colors)]
        lines.append(f"\x1b[{color}m{'#' * 24}\x1b[0m")
    return ("\r\n".join(lines)).encode()


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
        for _ in range(args.iterations):
            write_all(1, line)
            byte_count += len(line)

    elapsed = time.perf_counter() - started
    if args.visual:
        write_all(1, b"\x1b[2J\x1b[H" + test_pattern(rows))
    time.sleep(args.settle_ms / 1000)
    result = {"seconds": elapsed, "bytes": byte_count}
    if args.capture:
        time.sleep(0.05)
        result["capture"] = capture_window(args.capture, args.title)
    Path(args.result).write_text(json.dumps(result), encoding="utf-8")
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


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(len(ordered) * fraction + 0.999) - 1))
    return ordered[index]


def parse_trace(path):
    data = Path(path).read_bytes()
    if len(data) < TRACE_HEADER.size:
        raise RuntimeError(f"trace file is truncated: {path}")
    magic, version, record_size, _ = TRACE_HEADER.unpack_from(data)
    record = TRACE_RECORDS.get(version)
    if magic != TRACE_MAGIC or record is None:
        raise RuntimeError(f"unsupported trace format: {path}")
    if record_size != record.size or (len(data) - TRACE_HEADER.size) % record_size:
        raise RuntimeError(f"invalid trace record size: {path}")

    frames = []
    offset = TRACE_HEADER.size
    while offset < len(data):
        fields = record.unpack_from(data, offset)
        offset += record_size
        frame, trigger_ns, draw_ns, flush_ns = fields[:4]
        sync_ns, cells = (fields[4], fields[5]) if version >= 2 else (0, fields[4])
        atlas_evictions = fields[6] if version >= 3 else 0
        if not draw_ns or flush_ns < draw_ns:
            continue
        present_ns = sync_ns if sync_ns >= flush_ns else None
        frames.append({
            "frame": frame,
            "wait_ms": (draw_ns - trigger_ns) / 1e6 if trigger_ns else None,
            "draw_ms": (flush_ns - draw_ns) / 1e6,
            "submit_ms": (flush_ns - trigger_ns) / 1e6 if trigger_ns else None,
            "present_ms": (present_ns - trigger_ns) / 1e6
            if present_ns and trigger_ns else None,
            "cells": cells,
            "atlas_evictions": atlas_evictions,
        })
    if not frames:
        raise RuntimeError(f"trace contains no completed frames: {path}")

    draws = [frame["draw_ms"] for frame in frames]
    submits = [frame["submit_ms"] for frame in frames if frame["submit_ms"] is not None]
    presents = [frame["present_ms"] for frame in frames if frame["present_ms"] is not None]
    return {
        "frames": len(frames),
        "cells": sum(frame["cells"] for frame in frames),
        "atlas_evictions": sum(frame["atlas_evictions"] for frame in frames),
        "draw_ms_p50": percentile(draws, 0.50),
        "draw_ms_p95": percentile(draws, 0.95),
        "draw_ms_max": max(draws),
        "submit_ms_p50": percentile(submits, 0.50),
        "submit_ms_p95": percentile(submits, 0.95),
        "submit_ms_max": max(submits) if submits else None,
        "present_ms_p50": percentile(presents, 0.50),
        "present_ms_p95": percentile(presents, 0.95),
        "present_ms_max": max(presents) if presents else None,
    }


def run_trial(st, display, workload, args, directory, trial):
    result_path = Path(directory) / f"{workload}-{trial}.json"
    trace_path = Path(directory) / f"{workload}-{trial}.trace"
    title = f"st-benchmark-{workload}-{trial}"
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
        "--title", title,
        "--result", str(result_path),
    ]
    if args.capture:
        worker_args.extend(["--capture", args.capture])
    if args.visual:
        worker_args.append("--visual")

    env = os.environ.copy()
    env["DISPLAY"] = display
    if not args.no_trace:
        env["ST_BENCH_TRACE"] = str(trace_path)

    usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.perf_counter()
    process = subprocess.Popen(
        [st, "-T", title, "-g", f"{args.cols}x{args.rows}", "-e", *worker_args],
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
    usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if process.returncode:
        raise RuntimeError(
            f"st exited {process.returncode}: {stderr.decode(errors='replace').strip()}"
        )
    if not result_path.exists():
        raise RuntimeError("Workload did not write results before st exited")

    measured = json.loads(result_path.read_text(encoding="utf-8"))
    if "capture" in measured:
        capture = measured["capture"]
        measured["render_valid"] = (
            capture["distinct"] >= 8 and capture["dominant_ratio"] < 0.95
        )
    measured.update({
        "workload": workload,
        "trial": trial,
        "wall_seconds": wall_seconds,
        "cpu_seconds": (
            (usage_after.ru_utime - usage_before.ru_utime)
            + (usage_after.ru_stime - usage_before.ru_stime)
        ),
    })
    measured["mib_per_second"] = measured["bytes"] / measured["seconds"] / (1024 * 1024)
    measured["cpu_percent"] = (
        100.0 * measured["cpu_seconds"] / wall_seconds if wall_seconds else None
    )
    if not args.no_trace:
        if not trace_path.exists():
            raise RuntimeError(f"st did not write trace file: {trace_path}")
        measured["frame_trace"] = parse_trace(trace_path)
    return measured


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--st", default="st", help="st executable (default: st in PATH)")
    parser.add_argument("--xvfb", action="store_true", help="start isolated Xvfb display")
    workloads = ["stream", "redraw", "scroll", "kitty"]
    parser.add_argument("--workloads", nargs="+", choices=workloads,
                        default=["stream", "redraw", "scroll"])
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--repeat-check", type=int, metavar="N",
                        help="run each workload N times and check spread")
    parser.add_argument("--tolerance", type=float, default=10.0, metavar="PERCENT",
                        help="maximum relative spread for --repeat-check "
                             "(default: 10)")
    parser.add_argument("--mib", type=int, default=16, help="stream workload size per trial")
    parser.add_argument("--iterations", type=int, default=250, help="redraw/scroll cycles")
    parser.add_argument("--cols", type=int, default=120)
    parser.add_argument("--rows", type=int, default=40)
    parser.add_argument("--settle-ms", type=int, default=250)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--capture", metavar="XCAPTURE",
                        help="capture window with xcapture helper for validation")
    parser.add_argument("--visual", action="store_true",
                        help="draw a colored test pattern before capture")
    parser.add_argument("--no-trace", action="store_true", help="disable frame tracing")
    parser.add_argument("--output", default="benchmark-results.json")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--workload", choices=workloads)
    parser.add_argument("--title")
    parser.add_argument("--result")
    args = parser.parse_args()
    if args.worker:
        return args
    if args.repeat_check is not None:
        if args.repeat_check < 2:
            parser.error("repeat-check must run at least 2 trials")
        args.repetitions = args.repeat_check
    if min(args.repetitions, args.mib, args.iterations, args.cols, args.rows) < 1:
        parser.error("repetitions, mib, iterations, cols, and rows must be positive")
    if args.settle_ms < 0 or args.timeout < 1:
        parser.error("settle-ms must be non-negative and timeout must be positive")
    if args.tolerance <= 0:
        parser.error("tolerance must be positive")
    if args.capture:
        args.capture = shutil.which(args.capture) or args.capture
        if not Path(args.capture).is_file():
            parser.error(f"capture helper not found: {args.capture}")
        args.visual = True
    return args


def summarize(results, workloads):
    medians = {}
    for workload in workloads:
        group = [row for row in results if row["workload"] == workload]
        traced = [row for row in group if "frame_trace" in row]
        entry = {
            "generation_mib_per_second": statistics.median(
                row["mib_per_second"] for row in group
            ),
            "terminal_lifetime_seconds": statistics.median(
                row["wall_seconds"] for row in group
            ),
            "cpu_percent": statistics.median(
                row["cpu_percent"] for row in group if row["cpu_percent"] is not None
            ),
        }
        if traced:
            for key in ("draw_ms_p50", "draw_ms_p95", "draw_ms_max",
                        "submit_ms_p50", "submit_ms_p95", "submit_ms_max",
                        "present_ms_p50", "present_ms_p95", "present_ms_max"):
                values = [
                    row["frame_trace"][key] for row in traced
                    if row["frame_trace"][key] is not None
                ]
                entry[key] = statistics.median(values) if values else None
            entry["frames"] = statistics.median(
                row["frame_trace"]["frames"] for row in traced
            )
        validated = [row for row in group if "render_valid" in row]
        if validated:
            entry["render_valid"] = all(row["render_valid"] for row in validated)
        medians[workload] = entry
    return medians


def relative_spread(values):
    values = [value for value in values if value is not None]
    if len(values) < 2:
        return None
    middle = statistics.median(values)
    if middle == 0:
        return None
    return (max(values) - min(values)) / middle


def repeatability_report(results, workloads, tolerance):
    limit = tolerance / 100.0
    report = {}
    failures = []
    for workload in workloads:
        group = [row for row in results if row["workload"] == workload]
        throughput = relative_spread(row["mib_per_second"] for row in group)
        draw_p95 = relative_spread(
            row["frame_trace"]["draw_ms_p95"] for row in group
            if "frame_trace" in row
        )
        exceeded = [
            name for name, spread in (
                ("generation_mib_per_second", throughput),
                ("draw_ms_p95", draw_p95),
            )
            if spread is not None and spread > limit
        ]
        report[workload] = {
            "trials": len(group),
            "generation_spread": throughput,
            "draw_ms_p95_spread": draw_p95,
            "within_tolerance": not exceeded,
        }
        if exceeded:
            failures.append(f"{workload}: {', '.join(exceeded)}")
    return report, failures


def print_repeatability(report, tolerance):
    for workload, entry in report.items():
        parts = []
        if entry["generation_spread"] is not None:
            parts.append(
                f"generation {entry['generation_spread'] * 100:.1f}% spread"
            )
        if entry["draw_ms_p95_spread"] is not None:
            parts.append(
                f"draw p95 {entry['draw_ms_p95_spread'] * 100:.1f}% spread"
            )
        status = "ok" if entry["within_tolerance"] else "EXCEEDS"
        detail = ", ".join(parts) if parts else "not enough samples"
        print(
            f"repeat check {workload}: {detail}, "
            f"tolerance {tolerance:g}%, {status}"
        )


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
                    trace = result.get("frame_trace")
                    frame_text = (
                        f", draw p95 {trace['draw_ms_p95']:.2f} ms, "
                        f"submit p95 {trace['submit_ms_p95']:.2f} ms"
                        if trace else ""
                    )
                    render_text = (
                        f", render {'valid' if result['render_valid'] else 'INVALID'}"
                        if "render_valid" in result else ""
                    )
                    print(
                        f"{workload} {trial}/{args.repetitions}: "
                        f"{result['mib_per_second']:.2f} MiB/s generation, "
                        f"{result['cpu_percent']:.1f}% CPU"
                        f"{frame_text}{render_text}, "
                        f"{result['wall_seconds']:.3f}s lifetime"
                    )
                    if result.get("render_valid") is False:
                        raise RuntimeError(
                            f"{workload} trial {trial} failed render validation"
                        )
            summary = {
                "system": platform.platform(),
                "machine": platform.machine(),
                "display": display,
                "st": st,
                "tracing": not args.no_trace,
                "capture": args.capture,
                "configuration": {
                    "cols": args.cols, "rows": args.rows, "mib": args.mib,
                    "iterations": args.iterations, "settle_ms": args.settle_ms,
                    "repetitions": args.repetitions,
                    "repeat_check": args.repeat_check,
                    "tolerance_percent": args.tolerance,
                },
                "results": results,
                "medians": summarize(results, args.workloads),
            }
            failures = []
            if args.repeat_check:
                report, failures = repeatability_report(
                    results, args.workloads, args.tolerance
                )
                summary["repeat_check"] = {
                    "tolerance_percent": args.tolerance,
                    "workloads": report,
                }
            Path(args.output).write_text(json.dumps(summary, indent=2) + "\n",
                                         encoding="utf-8")
            print(f"Results written to {args.output}")
            if args.repeat_check:
                print_repeatability(report, args.tolerance)
                if failures:
                    raise RuntimeError(
                        "repeat check exceeded tolerance for " + "; ".join(failures)
                    )
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
