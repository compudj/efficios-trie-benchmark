#!/usr/bin/env python3
"""
Saturation: the role-split reader sweep with the writers FLAT OUT.

This is NOT a reader comparison.  Each engine's readers face whatever rename
rate that engine's own writers reach, and those rates differ by up to two
orders of magnitude (every seqlock rename serializes on rename_lock), so the
engine with the slower writers has its readers measured on a quieter machine.
The matched-load reader comparison is dcache_s3.png.  What this figure does
show is where each engine's WRITERS top out under a growing reader load (right),
and what the readers got alongside (left) -- read the two panels together.

Data: scripts/dcache_sweep.csv (panel sat_scale).  2x96 EPYC.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               NullFormatter)
import dcache_plotlib as dp

_num = FuncFormatter(lambda v, pos: f"{v:g}")
HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_sweep.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_sat.png"))

COLOR = {"seqlock": "#D55E00", "seqlock-snapshot": "#E69F00",
         "txn-global": "#0072B2", "txn-pernode": "#009E73",
         "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "seqlock-snapshot": "P", "txn-global": "o",
          "txn-pernode": "^", "txn-mark": "D", "bucketlock": "X"}
LABEL = {
    "seqlock": "seqlock — kernel RCU-walk",
    "seqlock-snapshot": "seqlock — whole-walk rename_lock",
    "txn-global": "urcu-txn — GLOBAL rename_gen",
    "txn-pernode": "urcu-txn — PER-NODE host gen",
    "txn-mark": "urcu-txn — deletion MARK",
    "bucketlock": "bucket lock + SW txn",
}
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock").split())
rows = dp.load(CSV)

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.5, 6))
for e in ENGINES:
    dp.plot_series(ax1, dp.series(rows, "readers", "mlookups_s",
                                  panel="sat_scale", engine=e),
                   COLOR[e], MARKER[e], LABEL[e])
    dp.plot_series(ax2, dp.series(rows, "readers", "mrenames_s",
                                  panel="sat_scale", engine=e),
                   COLOR[e], MARKER[e], LABEL[e])
for ax in (ax1, ax2):
    if ax.has_data():
        ticks = [0, 32, 64, 96, 128, 160, 184]
        ax.set_xlim(0, max(ticks) * 1.02)
        ax.xaxis.set_major_locator(FixedLocator(ticks))
        ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.set_ylim(bottom=0)
        ax.yaxis.set_major_formatter(_num)
    ax.grid(alpha=0.3, ls=":")
    dp.legend(ax, fontsize=7.5, loc="best")
    ax.set_xlabel("dedicated reader threads")
ax1.set_title("readers alongside 8 FLAT-OUT writers — NOT a reader comparison:\n"
              "each engine's readers face its own writers' rate (right panel);\n"
              "the matched-load comparison is dcache_s3.png", fontsize=9.5)
ax1.set_ylabel("reader lookup Mops/s")
ax2.set_title("where the writers top out under reader load\n"
              "(8 writers flat out, renames/s vs reader count)", fontsize=9.5)
ax2.set_ylabel("renames / s   (Mrenames/s, higher is better)")
fig.suptitle("Userspace dcache — saturation: writers flat out   ·   "
             "2×96-core EPYC", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
