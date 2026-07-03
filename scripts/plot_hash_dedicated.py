#!/usr/bin/env python3
"""
Dedicated reader/writer hash figure (same 1000 x 100 hash): write scaling, read
scaling, and the 50/50 balanced split (write half + read half).  Reads
scripts/hash_dedicated.csv (best-of-2), writes figures/hash_dedicated_rw.png.
Run: python3 scripts/plot_hash_dedicated.py
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullLocator

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "hash_dedicated.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "hash_dedicated_rw.png")

ENGINES = [
    ("txn_hlist", "txn_hlist", "#0072B2", "o"),
    ("rcu_hlist", "rcu_hlist", "#D55E00", "s"),
    ("lfht",      "lfht",      "#009E73", "^"),
    ("RLU-defer", "rlu_defer", "#CC79A7", "D"),
    ("RLU-sync",  "rlu_sync",  "#E69F00", "v"),
]
rows = list(csv.DictReader(open(CSV)))

def series(mode, ekey, ycol):
    acc = defaultdict(list)
    for r in rows:
        if r["mode"] == mode and r["engine"] == ekey:
            acc[int(r["x"])].append(float(r[ycol]))
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs], [min(acc[x]) for x in xs], [max(acc[x]) for x in xs]

# (subplot title, mode, y-column, y-axis label)
PANELS = [
    ("Write scaling (writers only)",        "write", "b", "write throughput (Mops/s)"),
    ("Read scaling (readers + 1 writer)",   "read",  "a", "read throughput (Mvisits/s)"),
    ("50/50 balanced — write half",         "bal",   "b", "write throughput (Mops/s)"),
    ("50/50 balanced — read half",          "bal",   "a", "read throughput (Mvisits/s)"),
]
XLAB = {"write": "writers", "read": "readers", "bal": "total threads (½ read, ½ write)"}

plt.rcParams.update({"font.size": 10, "axes.edgecolor": "#888888",
                     "axes.linewidth": 0.8, "figure.facecolor": "white"})
fig, axes = plt.subplots(2, 2, figsize=(11, 7.2), dpi=150)
xticks = [1, 8, 32, 64, 128, 192]

for ax, (title, mode, ycol, ylab) in zip(axes.flat, PANELS):
    ymax = 0
    for label, ekey, color, marker in ENGINES:
        xs, best, lo, hi = series(mode, ekey, ycol)
        if not xs:
            continue
        ymax = max(ymax, max(hi))
        ax.fill_between(xs, lo, hi, color=color, alpha=0.15, lw=0, zorder=2)
        ax.plot(xs, best, color=color, lw=1.8, marker=marker, ms=5.5,
                markeredgecolor="white", markeredgewidth=0.9, zorder=4, label=label)
    ax.set_title(title, fontsize=11, fontweight="bold")
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_locator(FixedLocator(xticks))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.set_xticklabels([str(t) for t in xticks])
    ax.set_ylim(0, ymax * 1.08 if ymax else 1)
    ax.set_xlabel(XLAB[mode], fontsize=10)
    ax.set_ylabel(ylab, fontsize=10)
    ax.grid(True, color="#ececec", lw=0.7, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

handles, labels = axes[0, 0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=5, fontsize=10.5,
           frameon=False, bbox_to_anchor=(0.5, 1.005))
fig.suptitle("Dedicated reader/writer hash-of-lists (1000 × 100), five engines — 2× EPYC 9654",
             fontsize=12.5, fontweight="bold", y=1.05)
fig.tight_layout(rect=(0, 0, 1, 0.97))
fig.savefig(OUT, bbox_inches="tight")
print("wrote", os.path.normpath(OUT))
