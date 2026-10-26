#!/usr/bin/env python3
"""Statistics of a benchmark's raw samples, for a performance report.

A performance report (docs/perf/TEMPLATE.md) keeps the raw samples of every
stage and takes its numbers from them through this script, never by hand, so
that each number in it can be traced back to what was measured.

The raw format is CSV: a header line `case,metric,unit,value`, then one row
per sample. `case` names what was measured (`map 4K 512`), `metric` what the
number is (`ns/page`, `visits`), `unit` its unit. Lines starting with `#` are
comments, for the machine, the build and the command.

Usage:

    python3 scripts/perf_stats.py FILE.csv
        One table: n, min, median, p90, max, mean and standard deviation of
        every case and metric, in the order they first appear.

    python3 scripts/perf_stats.py --compare BEFORE.csv AFTER.csv
        The gain of AFTER over BEFORE for every case and metric in both: the
        two medians, their ratio (BEFORE / AFTER, so above 1 is better for a
        time or a count), and the change in percent. A ratio inside the two
        runs' spread is marked, since it may be noise.

Both print Markdown tables, ready to paste into the report.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

HEADER = ["case", "metric", "unit", "value"]


def read(path: Path) -> dict[tuple[str, str], tuple[str, list[float]]]:
    """(case, metric) -> (unit, samples), in the order they first appear."""
    rows = [line for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")]
    reader = csv.reader(rows)
    header = next(reader, None)
    if header != HEADER:
        sys.exit(f"{path}: the header must be {','.join(HEADER)}, not {header}")
    out: dict[tuple[str, str], tuple[str, list[float]]] = {}
    for n, row in enumerate(reader, start=2):
        if len(row) != len(HEADER):
            sys.exit(f"{path}:{n}: expected {len(HEADER)} fields, got {len(row)}")
        case, metric, unit, value = row
        key = (case.strip(), metric.strip())
        unit = unit.strip()
        if key in out and out[key][0] != unit:
            sys.exit(f"{path}:{n}: {key} was {out[key][0]}, now {unit}")
        out.setdefault(key, (unit, []))[1].append(float(value))
    return out


def quantile(sorted_samples: list[float], q: float) -> float:
    """Linear interpolation between the closest ranks."""
    if len(sorted_samples) == 1:
        return sorted_samples[0]
    at = q * (len(sorted_samples) - 1)
    low = math.floor(at)
    high = min(low + 1, len(sorted_samples) - 1)
    return sorted_samples[low] + (sorted_samples[high] - sorted_samples[low]) * (at - low)


def stats(samples: list[float]) -> dict[str, float]:
    s = sorted(samples)
    n = len(s)
    mean = sum(s) / n
    var = sum((x - mean) ** 2 for x in s) / (n - 1) if n > 1 else 0.0
    return {"n": n, "min": s[0], "median": quantile(s, 0.5), "p90": quantile(s, 0.9),
            "max": s[-1], "mean": mean, "stddev": math.sqrt(var)}


def fmt(x: float) -> str:
    # Floating-point residue of identical samples (a stddev of 1e-15) is zero.
    if abs(x) < 1e-9:
        return "0"
    if abs(x) >= 1000:
        return f"{x:.0f}"
    if abs(x) >= 10:
        return f"{x:.1f}"
    if abs(x) >= 0.1:
        return f"{x:.2f}"
    return f"{x:.3g}"


def table(path: Path) -> None:
    data = read(path)
    print("| case | metric | unit | n | min | median | p90 | max | mean | stddev |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for (case, metric), (unit, samples) in data.items():
        st = stats(samples)
        print(f"| {case} | {metric} | {unit} | {st['n']} | " + " | ".join(
            fmt(st[k]) for k in ("min", "median", "p90", "max", "mean", "stddev")) + " |")


def compare(before_path: Path, after_path: Path) -> None:
    before, after = read(before_path), read(after_path)
    print("| case | metric | unit | median before | median after | ratio | change |")
    print("|---|---|---|---|---|---|---|")
    for key, (unit, samples) in before.items():
        if key not in after:
            continue
        b, a = stats(samples), stats(after[key][1])
        if a["median"] == 0 or b["median"] == 0:
            ratio, change = "-", "-"
        else:
            ratio = f"{b['median'] / a['median']:.2f}x"
            change = f"{(a['median'] - b['median']) / b['median'] * 100:+.1f}%"
            # Within the spread of either run: the medians may differ by noise.
            spread = max(b["p90"] - b["median"], a["p90"] - a["median"])
            if abs(a["median"] - b["median"]) <= spread:
                ratio += " (within noise)"
        case, metric = key
        print(f"| {case} | {metric} | {unit} | {fmt(b['median'])} | {fmt(a['median'])} | {ratio} | {change} |")
    missing = [k for k in before if k not in after] + [k for k in after if k not in before]
    for case, metric in missing:
        print(f"\nnot in both: {case} / {metric}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compare", action="store_true", help="the gain of the second file over the first")
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    if args.compare:
        if len(args.files) != 2:
            parser.error("--compare takes BEFORE.csv AFTER.csv")
        compare(args.files[0], args.files[1])
    else:
        if len(args.files) != 1:
            parser.error("one file, or --compare BEFORE.csv AFTER.csv")
        table(args.files[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
