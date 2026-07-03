#!/usr/bin/env python3
"""
Crossover chart: hash-of-lists throughput vs dataset size, four engines.

Reads scripts/hlist_crossover.csv (rep,buckets,engine,mops -- 5 repeats of the
BENCH_UPDATE_PCT=10, BENCH_FIXED_THREADS=64 dataset sweep, load ~1) and renders
figures/hlist_crossover.png: mean line per engine with a min-max band, log-x in
buckets (head-array bytes annotated), and L2/L3 reference markers.

The story: txn_hlist's 8-byte, sentinel-free head touches the fewest cache lines
per lookup, so it crosses from 2nd (cache-resident) to 1st once the working set
spills past L3 (~1M buckets on this 2x EPYC 9654: L2 1 MB/core, L3 32 MB/CCD).

Colors: Okabe-Ito (CVD-safe); the two engines that cross (txn_hlist, rcu_hlist)
get the canonical blue/vermilion pair; marker shapes double-encode identity for
grayscale/print. Run: python3 scripts/plot_hlist_crossover.py
"""
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, NullLocator

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "hlist_crossover.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "hlist_crossover.png")

# ── aggregate: mean / min / max per (engine, buckets) over the repeats ────────
vals = defaultdict(list)
with open(CSV) as f:
    for row in csv.DictReader(f):
        if not row["rep"].isdigit() or row["mops"] == "NA":
            continue
        vals[(row["engine"], int(row["buckets"]))].append(float(row["mops"]))

# Engines in draw order: protagonists first so their labels sit on top.
# (label, csv-name, Okabe-Ito color, marker)
ENGINES = [
    ("txn_hlist", "txn_hlist", "#0072B2", "o"),   # blue      -- the subject
    ("rcu_hlist", "rcu_hlist", "#D55E00", "s"),   # vermilion -- the engine it crosses
    ("lfht",      "lfht",      "#009E73", "^"),   # green
    ("rlu_hlist", "rlu_hlist", "#CC79A7", "D"),   # reddish purple
]
SIZES = sorted({b for (_, b) in vals})

def label_size(n):
    return f"{n // 1024}K" if n < 1_048_576 else f"{n // 1_048_576}M"

# ── figure ───────────────────────────────────────────────────────────────────
plt.rcParams.update({
    "font.size": 11, "axes.edgecolor": "#888888", "axes.linewidth": 0.8,
    "figure.facecolor": "white", "axes.facecolor": "white",
})
fig, ax = plt.subplots(figsize=(9.2, 5.6), dpi=150)

# Cache reference lines: head-array (8 B/bucket) == L2 (1 MB) / L3 (32 MB).
for buckets, txt in [(131072, "L2  1 MB/core"), (4194304, "L3  32 MB/CCD")]:
    ax.axvline(buckets, color="#b8b8b8", ls=(0, (4, 3)), lw=1.0, zorder=1)
    ax.text(buckets, 5, "  " + txt, rotation=90, va="bottom", ha="left",
            fontsize=8.5, color="#7a7a7a")

for label, name, color, marker in ENGINES:
    xs = [b for b in SIZES if (name, b) in vals]
    mean = [sum(vals[(name, b)]) / len(vals[(name, b)]) for b in xs]
    lo = [min(vals[(name, b)]) for b in xs]
    hi = [max(vals[(name, b)]) for b in xs]
    ax.fill_between(xs, lo, hi, color=color, alpha=0.15, linewidth=0, zorder=2)
    ax.plot(xs, mean, color=color, lw=2.0, marker=marker, ms=7,
            markeredgecolor="white", markeredgewidth=1.2, zorder=4, label=label)

# Crossover callout at ~1M buckets (txn overtakes rcu).
ax.annotate("crossover ≈ 1M buckets\n(working set exceeds L3)",
            xy=(1_048_576, 405), xytext=(1_250_000, 120),
            fontsize=9.5, color="#333333", ha="left",
            arrowprops=dict(arrowstyle="-|>", color="#555555", lw=1.2,
                            connectionstyle="arc3,rad=0.2"))

ax.set_xscale("log", base=2)
ax.xaxis.set_major_locator(FixedLocator(SIZES))
ax.xaxis.set_minor_locator(NullLocator())
ax.set_xticklabels([f"{label_size(b)}\n{b * 8 // 1024 // 1024 if b*8>=1048576 else b*8//1024}"
                    f"{' MB' if b*8>=1048576 else ' KB'}" for b in SIZES], fontsize=9.5)
ax.set_xlim(SIZES[0] * 0.8, SIZES[-1] * 1.15)
ax.set_ylim(0, 600)
ax.set_xlabel("buckets  (head-array size below;  keys ≈ buckets, chains ≈ 1)", fontsize=10.5)
ax.set_ylabel("throughput  (Mops/s, higher is better)", fontsize=10.5)
ax.set_title("Hash-of-lists throughput vs dataset size — the 8-byte head wins past cache",
             fontsize=13, fontweight="bold", pad=30)
ax.text(0.0, 1.045,
        "64 threads, 10% updates, mean of 5 runs (band = min–max) · 2× AMD EPYC 9654",
        transform=ax.transAxes, fontsize=9.5, color="#666666")

ax.grid(axis="y", color="#e6e6e6", lw=0.8, zorder=0)
ax.set_axisbelow(True)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
leg = ax.legend(loc="upper right", frameon=True, fontsize=10.5, handlelength=1.8,
                borderpad=0.7, labelspacing=0.5)
leg.get_frame().set_edgecolor("#dddddd")
leg.get_frame().set_facecolor("white")

fig.tight_layout()
fig.savefig(OUT, bbox_inches="tight")
print("wrote", os.path.normpath(OUT))
