#!/usr/bin/env python3
"""Compare two binary P6 PPM captures with a per-channel tolerance."""

import argparse
import json
from pathlib import Path


def read_ppm(path):
    with open(path, "rb") as f:
        tokens = []
        while len(tokens) < 4:
            line = f.readline()
            if not line:
                raise ValueError(f"truncated header: {path}")
            if line.startswith(b"#"):
                continue
            tokens.extend(line.split())
        magic, width, height, maxval = tokens[:4]
        if magic != b"P6" or maxval != b"255":
            raise ValueError(f"expected P6 255: {path}")
        data = f.read()
    expected = int(width) * int(height) * 3
    if len(data) != expected:
        raise ValueError(f"bad pixel count: {path}")
    return int(width), int(height), data


def compare(left, right, tolerance):
    if left[:2] != right[:2]:
        raise ValueError(f"dimensions differ: {left[:2]} vs {right[:2]}")
    diffs = [abs(a - b) for a, b in zip(left[2], right[2])]
    if not diffs:
        raise ValueError("empty capture")
    over = sum(1 for diff in diffs if diff > tolerance)
    return {
        "pixels": left[0] * left[1],
        "channels": len(diffs),
        "tolerance": tolerance,
        "max": max(diffs),
        "mean": round(sum(diffs) / len(diffs), 3),
        "over": over,
        "over_fraction": round(over / len(diffs), 6),
    }


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--tolerance", type=int, default=8)
    parser.add_argument("--max-over-fraction", type=float, default=0.0)
    parser.add_argument("--json", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    stats = compare(read_ppm(args.left), read_ppm(args.right), args.tolerance)
    if args.json:
        print(json.dumps(stats, sort_keys=True))
    else:
        print(
            f"pixels={stats['pixels']} channels={stats['channels']} "
            f"max={stats['max']} mean={stats['mean']} "
            f"over{stats['tolerance']}={stats['over']} "
            f"fraction={stats['over_fraction']:.6f}"
        )
    return 1 if stats["over_fraction"] > args.max_over_fraction else 0


if __name__ == "__main__":
    raise SystemExit(main())
