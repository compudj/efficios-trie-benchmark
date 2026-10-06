#!/usr/bin/env python3
"""Summarize scripts/dcache_holdoff.csv (run_dcache_holdoff.sh).

One line per point: for each arm the median of its runs, with the run-to-run
range, for the readers (Mlookups/s) and the writers (Mrenames/s); for the
hold-off arms the ratio of medians to the default build, marked `=` when the
two arms' ranges overlap (the difference is inside the point's own spread)
and `*` when they do not.  Then how often each hold-off arm raised the count
(dcache_holdoff_counts.csv).
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

here = os.path.dirname(os.path.abspath(__file__))
runs_csv = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "dcache_holdoff.csv")
cnt_csv = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "dcache_holdoff_counts.csv")

BASE = "bucketlock"
pts = defaultdict(lambda: defaultdict(lambda: {"lk": [], "rn": []}))
order, arms, failed = [], [], 0
for r in csv.DictReader(open(runs_csv)):
    key = (r["panel"], int(r["readers"]), int(r["writers"]), r["rename_frac"], r["rate_target"])
    if key not in order:
        order.append(key)
    if r["arm"] not in arms:
        arms.append(r["arm"])
    if r["conserved"] != "OK":
        failed += 1
        continue
    pts[key][r["arm"]]["lk"].append(float(r["mlookups_s"]))
    pts[key][r["arm"]]["rn"].append(float(r["mrenames_s"]))


def stat(v):
    return (statistics.median(v), min(v), max(v)) if v else (0.0, 0.0, 0.0)


def cell(v):
    m, lo, hi = stat(v)
    return f"{m:9.4g} [{lo:.4g}-{hi:.4g}]"


def ratio(v, b):
    m, lo, hi = stat(v)
    bm, blo, bhi = stat(b)
    if bm == 0:
        return "    -   "
    overlap = lo <= bhi and blo <= hi
    return f"{m / bm:6.3f}{'=' if overlap else '*'} "


print(f"runs: {runs_csv}   conservation failures: {failed}")
for metric, name in (("lk", "READERS, Mlookups/s"), ("rn", "WRITERS, Mrenames/s")):
    print(f"\n== {name}: median [min-max] of the runs; hold/default ratio, "
          f"'=' ranges overlap, '*' they do not ==")
    last = None
    for key in order:
        panel, readers, writers, frac, rate = key
        if panel != last:
            print(f"-- {panel}")
            last = panel
        if writers < 0:
            label = f"thr={readers:<4d} frac={frac:<5s}"
        else:
            label = f"rd={readers:<4d} rate={rate:<9s}"
        if metric == "rn" and all(stat(pts[key][a]["rn"])[0] < 1e-4 for a in arms):
            continue  # idle panels: no rename in the window
        line = f"  {label} "
        base = pts[key][BASE][metric]
        for a in arms:
            line += f"| {a} {cell(pts[key][a][metric])} "
            if a != BASE:
                line += ratio(pts[key][a][metric], base)
        print(line)

if os.path.exists(cnt_csv):
    print("\n== how often the count was raised (one counting run per point, whole "
          "process, indicative) ==")
    print(f"  {'panel':11s} {'point':24s} {'arm':6s} {'raises':>10s} {'w-sleeps':>10s} "
          f"{'refolds':>10s} {'raises per M lookups':>22s}")
    for r in csv.DictReader(open(cnt_csv)):
        if not r["raises"].isdigit():
            print(f"  {r['panel']:11s} (no count line: {r['arm']})")
            continue
        writers = int(r["writers"])
        label = (f"thr={r['readers']} frac={r['rename_frac']}" if writers < 0
                 else f"rd={r['readers']} rate={r['rate_target']}")
        lk = float(r["mlookups_s"] or 0)
        per = f"{int(r['raises']) / lk:.3g}" if lk > 0 else "-"
        print(f"  {r['panel']:11s} {label:24s} {r['arm']:6s} {int(r['raises']):10d} "
              f"{int(r['writer_sleeps']):10d} {int(r['refolds']):10d} {per:>22s}")
