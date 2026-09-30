#!/usr/bin/env python3
"""
Adversarial move-HEIGHT sweep -- how far does the localized reader's lead survive?

The S3 sweep moves leaves (fan-in 1): the localized reader's best case.  Here the
reader workload is fixed (uniform full-depth walks over a balanced binary forest)
and the writers move nodes at a swept HEIGHT H; a move at height H swaps two
sibling subtrees of 2^H leaves, invalidating a fraction ~2^(H-D) of reader walks.

LEFT: the readers with the writers PACED to 100k exchanges/s (bench
--rename-rate), the same offered load for every engine -- the reader
comparison.  RIGHT: the writers FLAT OUT (mode height_sat), where each engine's
exchanges top out; the flat-out READER numbers are in the CSV but are not a
comparison (each engine's readers there face its own writers' rate, which
differ severalfold).  Hollow markers: the writers could not sustain the offered
rate.

Plotted: every arm, each with its ratio to the seqlock baseline in the strip
under its panel (ENGINES=... selects a subset).

Data: scripts/dcache_height.csv (best-of-5, one run's numbers per row,
conservation-gated).  2x96 EPYC.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, FixedFormatter
import dcache_plotlib as dp

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_height.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_height.png"))

COLOR = {"seqlock": "#D55E00", "seqlock-snapshot": "#E69F00",
         "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "seqlock-snapshot": "P", "txn-global": "o",
          "txn-pernode": "^", "txn-mark": "D", "bucketlock": "X"}
LABEL = {
    "seqlock": "seqlock — kernel RCU-walk\n(hand-over-hand d_seq; faithful baseline)",
    "seqlock-snapshot": "seqlock — whole-walk rename_lock\n(snapshot, as the txn arms give)",
    "txn-global": "urcu-txn — GLOBAL rename_gen",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(localized: moved subtree only)",
    "txn-mark": "urcu-txn — deletion MARK as gen\n(localized, no counter)",
    "bucketlock": "bucket lock + SW txn\n(fold-lock writer)",
}
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock").split())
rows = dp.load(CSV)
paced_rate = next((float(r["rate_target"]) for r in rows
                   if r["height"] == "height" and float(r["rate_target"]) > 0),
                  None)

fig, (ax, axs) = plt.subplots(1, 2, figsize=(15, 7.6))
for e in ENGINES:
    dp.plot_series(ax, dp.series(rows, "move_height", "mlookups_s",
                                 height="height", engine=e),
                   COLOR[e], MARKER[e], LABEL[e], lw=2.1, ms=7)
    dp.plot_series(axs, dp.series(rows, "move_height", "mexch_s",
                                  height="height_sat", engine=e),
                   COLOR[e], MARKER[e], LABEL[e], lw=2.1, ms=7)
heights = sorted({int(r["move_height"]) for r in rows})
for a in (ax, axs):
    a.set_xticks(heights)
    a.xaxis.set_major_formatter(FixedFormatter([str(h) for h in heights]))
    a.margins(x=0.06)
    a.set_ylim(bottom=0)
    a.yaxis.set_major_formatter(FuncFormatter(lambda v, p: f"{v:g}"))
    a.set_xlabel("move height H above the leaves  "
                 "(fan-in = 2^H leaves: 1 at H=0 → 128 at H=7)")
    a.grid(alpha=0.3, ls=":")
    dp.legend(a, fontsize=8, loc="best")
rl = dp.rate_label(paced_rate) if paced_rate else "?"
ax.set_ylabel("reader Mlookups/s   (higher is better)")
ax.set_title(f"READERS — 32 readers + 8 writers paced to {rl} exchanges/s\n"
             "writers exchange sibling subtrees at height H, the same offered\n"
             "load for every engine;  strip: every engine ÷ seqlock (dashed = "
             "parity)", fontsize=9.5)
dp.ratio_strip(ax, rows, "move_height", "mlookups_s", ENGINES, COLOR, MARKER,
               height="height")
axs.set_ylabel("exchanges / s   (Mexch/s, higher is better)")
dp.ratio_strip(axs, rows, "move_height", "mexch_s", ENGINES, COLOR, MARKER,
               height="height_sat")
axs.set_title("WRITERS — the same 8 writers FLAT OUT: where each engine's\n"
              "exchanges top out under 32 readers (the flat-out reader numbers\n"
              "are in the CSV, not a comparison)", fontsize=9.5)
fig.suptitle("Userspace dcache — how high can a DIRECTORY rename climb before the "
             "localized reader stops helping?   ·   2×96-core EPYC (jemalloc)",
             fontsize=11)
fig.tight_layout(rect=[0, 0, 1, 0.95])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
