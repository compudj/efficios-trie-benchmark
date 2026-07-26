#!/usr/bin/env python3
"""
MV-RLU vs the single-writer flip-latch on the same coherent bidirectional list.
Reads scripts/mvrlu_vs_txn.csv (best-of-N, min/max band), writes
figures/mvrlu_vs_txn.png.  Run: python3 scripts/plot_mvrlu_vs_txn.py

Both engines give a coherent BIDIRECTIONAL walk, but NOT the same reader
guarantee: MV-RLU readers get a coherent SNAPSHOT, strictly stronger than
txn_sw_list's per-slot-linearizable reads.  MV-RLU runs the gclk clock (no
calibrated per-machine ORDO constant) with its QP thread confined -- see
third_party/mvrlu/PROVENANCE.txt for why both of those matter.

Okabe-Ito colours: txn blue to match the other txn figures, MV-RLU green --
deliberately NOT the purple/amber the RLU figures use, so the two comparisons
stay legible if ever shown together.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "mvrlu_vs_txn.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "mvrlu_vs_txn.png")

rows = list(csv.DictReader(open(CSV)))

# (mode, value column, y-label, x-label, title)
PANELS = [
    ("readceil",   "read_mvisits", "read throughput (Mvisits/s)", "readers",
     "Read ceiling\n(no writer)"),
    ("mixed",      "write_mops",   "write throughput (Mops/s)",   "readers",
     "Writer under readers\n(1 writer + N readers)"),
    ("writescale", "write_mops",   "write throughput (Mops/s)",   "writers",
     "Writer scaling, 0 readers\n(txn_sw serialized: see note)"),
]
STYLE = {"txn_sw_list": ("#0072B2", "o"), "mvrlu_list": ("#009E73", "s")}
LABEL = {"txn_sw_list": "txn_sw_list (flip-latch)", "mvrlu_list": "MV-RLU (gclk)"}


def series(mode, engine, col):
    acc = defaultdict(list)
    for r in rows:
        if r["mode"] == mode and r["engine"] == engine:
            acc[int(r["x"])].append(float(r[col]))
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs], [min(acc[x]) for x in xs], [max(acc[x]) for x in xs]


plt.rcParams.update({"font.size": 10, "axes.edgecolor": "#888888",
                     "axes.linewidth": 0.8, "figure.facecolor": "white"})
fig, axes = plt.subplots(1, 3, figsize=(13.5, 4.6), dpi=150)
xticks = [0, 32, 64, 96, 128, 160, 192]

for ax, (mode, col, ylab, xlab, title) in zip(axes, PANELS):
    ymax = 0
    for eng in ("txn_sw_list", "mvrlu_list"):
        color, marker = STYLE[eng]
        xs, best, lo, hi = series(mode, eng, col)
        if not xs:
            continue
        ymax = max(ymax, max(hi))
        ax.fill_between(xs, lo, hi, color=color, alpha=0.15, lw=0, zorder=2)
        ax.plot(xs, best, color=color, lw=1.9, marker=marker, ms=6,
                markeredgecolor="white", markeredgewidth=1.0, zorder=4,
                label=LABEL[eng])
    ax.set_title(title, fontsize=11, fontweight="bold")
    ax.set_xlim(0, 196)
    ax.xaxis.set_major_locator(FixedLocator(xticks))
    ax.set_xticklabels([str(t) for t in xticks])
    ax.set_ylim(0, ymax * 1.08 if ymax else 1)
    ax.set_xlabel(xlab, fontsize=10)
    ax.set_ylabel(ylab, fontsize=10)
    ax.grid(True, color="#ececec", lw=0.7, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

# The third panel is NOT like-for-like and the figure must say so itself: the
# flip-latch is a SINGLE-WRITER engine, so its >1-writer points are threads
# serialized on the bench mutex, not concurrent writers.  The comparable
# multi-writer arm is txn_list, which is not in this figure.
axes[2].text(0.5, -0.30,
             "txn_sw_list is single-writer: >1 writer = threads serialized on the bench\n"
             "mutex, not concurrent writers.  The like-for-like MW arm is txn_list.\n"
             "MV-RLU's plateau is its single global QP reclaim thread.",
             transform=axes[2].transAxes, ha="center", va="top",
             fontsize=8.2, color="#555555")

handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=2, fontsize=10.5,
           frameon=False, bbox_to_anchor=(0.5, 1.02))
fig.suptitle("MV-RLU (gclk, QP confined) vs single-writer flip-latch — 10k nodes, "
             "2% updates, jemalloc, best-of-2, 2× EPYC 9654",
             fontsize=12.5, fontweight="bold", y=1.10)
fig.tight_layout(rect=(0, 0.06, 1, 0.90))
fig.savefig(OUT, bbox_inches="tight")
print("wrote", os.path.normpath(OUT))
