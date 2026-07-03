#!/usr/bin/env python3
"""
Write-contention hash figure: (left) total throughput vs bucket count at 192
threads, 40 % updates, ~100 nodes/bucket; (right) thread scaling at a heavily
contended 16 buckets.  Reads scripts/hash_contention.csv (best-of-2), writes
figures/hash_contention.png.  Run: python3 scripts/plot_hash_contention.py
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullLocator

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "hash_contention.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "hash_contention.png")

ENGINES = [
    ("txn_hlist", "txn_hlist", "#0072B2", "o"),
    ("rcu_hlist", "rcu_hlist", "#D55E00", "s"),
    ("lfht",      "lfht",      "#009E73", "^"),
    ("RLU-defer", "rlu_defer", "#CC79A7", "D"),
    ("RLU-sync",  "rlu_sync",  "#E69F00", "v"),
]
rows = list(csv.DictReader(open(CSV)))

def series(mode, ekey):
    acc = defaultdict(list)
    for r in rows:
        if r["mode"] == mode and r["engine"] == ekey:
            acc[int(r["x"])].append(float(r["total_mops"]))
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs], [min(acc[x]) for x in xs], [max(acc[x]) for x in xs]

plt.rcParams.update({"font.size": 10, "axes.edgecolor": "#888888",
                     "axes.linewidth": 0.8, "figure.facecolor": "white"})
fig, (axL, axR) = plt.subplots(1, 2, figsize=(11.5, 4.8), dpi=150)

# Left: throughput vs #buckets @192 threads
for label, ekey, color, marker in ENGINES:
    xs, best, lo, hi = series("buckets", ekey)
    if not xs:
        continue
    axL.fill_between(xs, lo, hi, color=color, alpha=0.15, lw=0, zorder=2)
    axL.plot(xs, best, color=color, lw=1.9, marker=marker, ms=6.5,
             markeredgecolor="white", markeredgewidth=1.0, zorder=4, label=label)
bticks = [1, 4, 16, 64, 256, 1024]
axL.set_xscale("log", base=2); axL.xaxis.set_major_locator(FixedLocator(bticks))
axL.xaxis.set_minor_locator(NullLocator()); axL.set_xticklabels([str(b) for b in bticks])
axL.set_xlabel("buckets = independent write lanes (~100 nodes each)", fontsize=10)
axL.set_ylabel("total throughput (Mops/s)", fontsize=10)
axL.set_title("Write contention: throughput vs #buckets\n(192 threads, 40 % updates)",
              fontsize=11, fontweight="bold")

# Right: thread scaling at 16 buckets
for label, ekey, color, marker in ENGINES:
    xs, best, lo, hi = series("threads16", ekey)
    if not xs:
        continue
    axR.fill_between(xs, lo, hi, color=color, alpha=0.15, lw=0, zorder=2)
    axR.plot(xs, best, color=color, lw=1.9, marker=marker, ms=6.5,
             markeredgecolor="white", markeredgewidth=1.0, zorder=4, label=label)
tticks = [1, 8, 32, 64, 128, 192]
axR.set_xscale("log", base=2); axR.xaxis.set_major_locator(FixedLocator(tticks))
axR.xaxis.set_minor_locator(NullLocator()); axR.set_xticklabels([str(t) for t in tticks])
axR.set_xlabel("threads", fontsize=10)
axR.set_ylabel("total throughput (Mops/s)", fontsize=10)
axR.set_title("Heavily contended: thread scaling\n(16 buckets, 40 % updates)",
              fontsize=11, fontweight="bold")

for ax in (axL, axR):
    ax.set_ylim(bottom=0)
    ax.grid(True, color="#ececec", lw=0.7, zorder=0); ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

handles, labels = axL.get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=5, fontsize=10.5,
           frameon=False, bbox_to_anchor=(0.5, 1.02))
fig.tight_layout(rect=(0, 0, 1, 0.92))
fig.savefig(OUT, bbox_inches="tight")
print("wrote", os.path.normpath(OUT))
