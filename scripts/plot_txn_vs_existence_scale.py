#!/usr/bin/env python3
"""
urcu-txn vs. McKenney "existence" — 3-hash atomic-move scaling to 192 cores.
Reads scripts/txn_vs_existence_scale.csv (best-of-N), writes
figures/txn_vs_existence_scale.png.  Three single-axis panels:
  - update scaling under glibc malloc      (aggregate key-moves/s vs updater cores)
  - update scaling under jemalloc          (same, fairer allocator)
  - read scaling                           (aggregate queries/s vs reader cores;
                                            readers do not allocate -> allocator-neutral)
Run: python3 scripts/plot_txn_vs_existence_scale.py
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "txn_vs_existence_scale.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "txn_vs_existence_scale.png")

# Okabe-Ito, matching the tree's other figures: txn = blue, existence = orange.
ENGINES = [
    ("urcu-txn (MCAS)",   "txn",       "#0072B2", "o"),
    ("existence (flip)",  "existence", "#D55E00", "s"),
]
rows = list(csv.DictReader(open(CSV)))

def series(alloc, mode, ekey, ycol):
    acc = defaultdict(list)
    for r in rows:
        if r["alloc"] == alloc and r["mode"] == mode and r["engine"] == ekey:
            v = float(r[ycol])
            if v > 0:
                acc[int(r["x"])].append(v)
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs], [min(acc[x]) for x in xs], [max(acc[x]) for x in xs]

# (title, alloc, mode, y-column, y-label, x-label)
PANELS = [
    ("Update scaling — glibc malloc",   "glibc",    "update", "mmoves_s",
     "aggregate key-moves/s (millions)", "updater cores"),
    ("Update scaling — jemalloc",       "jemalloc", "update", "mmoves_s",
     "aggregate key-moves/s (millions)", "updater cores"),
    ("Read scaling  (N readers + 1 updater)", "glibc", "read", "mqueries_s",
     "aggregate queries/s (millions)",   "reader cores"),
]

plt.rcParams.update({"font.size": 10, "axes.edgecolor": "#888888",
                     "axes.linewidth": 0.8, "figure.facecolor": "white"})
fig, axes = plt.subplots(1, 3, figsize=(15, 4.4), dpi=150)
xticks = [0, 32, 64, 96, 128, 160, 192]

# Share the y-scale across the two update panels so the allocator effect is
# read directly off the height difference.
umax = 0
for alloc in ("glibc", "jemalloc"):
    for _, ekey, _, _ in ENGINES:
        _, best, _, _ = series(alloc, "update", ekey, "mmoves_s")
        if best:
            umax = max(umax, max(best))

for ax, (title, alloc, mode, ycol, ylab, xlab) in zip(axes.flat, PANELS):
    for label, ekey, color, marker in ENGINES:
        xs, best, lo, hi = series(alloc, mode, ekey, ycol)
        if not xs:
            continue
        ax.fill_between(xs, lo, hi, color=color, alpha=0.15, lw=0, zorder=2)
        ax.plot(xs, best, color=color, lw=1.8, marker=marker, ms=5.5,
                markeredgecolor="white", markeredgewidth=0.9, zorder=4, label=label)
    ax.set_title(title, fontsize=11, fontweight="bold")
    ax.set_xlabel(xlab)
    ax.set_ylabel(ylab)
    ax.set_xlim(0, 196)
    ax.set_xticks(xticks)
    if mode == "update":
        ax.set_ylim(0, umax * 1.06)
    else:
        ax.set_ylim(bottom=0)
    ax.grid(True, color="#DDDDDD", lw=0.6, zorder=0)
    ax.legend(frameon=False, fontsize=9, loc="upper left")

fig.suptitle("urcu-txn vs. existence — 3-hash atomic move, 2×96-core EPYC "
             "(4096 buckets, best-of-5, 1 s/point)",
             fontsize=12, fontweight="bold")
fig.tight_layout(rect=(0, 0, 1, 0.95))
fig.savefig(OUT)
print("wrote", OUT)
