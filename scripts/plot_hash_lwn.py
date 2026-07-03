#!/usr/bin/env python3
"""
LWN #667720 hash figure: total throughput vs thread count, one panel per update
rate (0/2/20/40 %), five engines.  Reads scripts/hash_lwn.csv (best-of-2), writes
figures/lwn667720_hash.png.  Run: python3 scripts/plot_hash_lwn.py
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullLocator

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "hash_lwn.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "lwn667720_hash.png")

# (legend label, csv engine key, Okabe-Ito colour, marker) -- CVD-safe set.
ENGINES = [
    ("txn_hlist", "txn_hlist", "#0072B2", "o"),
    ("rcu_hlist", "rcu_hlist", "#D55E00", "s"),
    ("lfht",      "lfht",      "#009E73", "^"),
    ("RLU-defer", "rlu_defer", "#CC79A7", "D"),
    ("RLU-sync",  "rlu_sync",  "#E69F00", "v"),
]
PCTS = [("0", "0 % updates (read-only)"), ("2", "2 % updates"),
        ("20", "20 % updates"), ("40", "40 % updates")]

rows = list(csv.DictReader(open(CSV)))

def series(pct, ekey):
    acc = defaultdict(list)
    for r in rows:
        if r["pct"] == pct and r["engine"] == ekey:
            acc[int(r["threads"])].append(float(r["total_mops"]))
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs], [min(acc[x]) for x in xs], [max(acc[x]) for x in xs]

plt.rcParams.update({"font.size": 10, "axes.edgecolor": "#888888",
                     "axes.linewidth": 0.8, "figure.facecolor": "white"})
fig, axes = plt.subplots(2, 2, figsize=(11, 7.2), dpi=150, sharex=True)
xticks = [1, 8, 32, 64, 128, 192]

for ax, (pct, title) in zip(axes.flat, PCTS):
    ymax = 0
    for label, ekey, color, marker in ENGINES:
        xs, best, lo, hi = series(pct, ekey)
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
    ax.set_ylim(0, ymax * 1.08)
    ax.grid(True, color="#ececec", lw=0.7, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

for ax in axes[-1]:
    ax.set_xlabel("threads (each = one op mix)", fontsize=10)
for ax in axes[:, 0]:
    ax.set_ylabel("total throughput (Mops/s)", fontsize=10)

handles, labels = axes[0, 0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=5, fontsize=10.5,
           frameon=False, bbox_to_anchor=(0.5, 1.005))
fig.suptitle("RLU-paper hash (LWN #667720): throughput scaling by update rate — "
             "1000 buckets × 100 nodes, 2× EPYC 9654",
             fontsize=12.5, fontweight="bold", y=1.05)
fig.tight_layout(rect=(0, 0, 1, 0.97))
fig.savefig(OUT, bbox_inches="tight")
print("wrote", os.path.normpath(OUT))
