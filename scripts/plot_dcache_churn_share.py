#!/usr/bin/env python3
"""Plot scripts/dcache_churn.csv -> figures/dcache_churn_share.png.

Create/delete throughput against WHO SHARES A DIRECTORY, writers only, flat out.
Each writer toggles 32 slots spread over 32 directories, one slot per
directory; the three columns differ only in who else uses those directories:

  PRIVATE     nobody (bench --share 1) -- churn_w, as in dcache_churn.png
  SAME NODE   one other writer, on the same NUMA node (--share 2, stride 1)
  CROSS NODE  one other writer, on another node (--share 2, stride = cores per
              node), so every child-list and directory-lock line the pair
              shares crosses between CCXs

The axis exists because it used to be invisible: until 2026-10-02 the churn
sweeps ran --ndirs 16 x writers, which pairs writer i with writer i+W/2 -- on
the same node up to 8 writers, on another node from 16 on -- and that alone
moved the allocating-path ratios by ~6%.  Every run's geometry is checked
against the bench's own classification of its directories (`share:` line).

Rows: the two toggle modes, as in dcache_churn.png.  Strips: every engine ÷
seqlock at the same writer count.

Env: ENGINES="..." to plot a subset; OUT= to redirect.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               NullFormatter)
import dcache_plotlib as dp

_num = FuncFormatter(lambda v, _: ("%g" % v))


def plain_y(ax):
    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(_num)


def plain_thread_x(ax, ticks):
    ax.set_xlim(0, max(ticks) * 1.02)
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
    ax.xaxis.set_minor_formatter(NullFormatter())


HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.environ.get("CSV", os.path.join(HERE, "dcache_churn.csv"))
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures",
                                  "dcache_churn_share.png"))

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D",
          "bucketlock": "X"}
LABEL = {
    "seqlock": "seqlock — hlist_bl + d_seq\n(faithful kernel baseline)",
    "txn-global": "urcu-txn — GLOBAL rename_gen",
    "txn-pernode": "urcu-txn — PER-NODE host gen",
    "txn-mark": "urcu-txn — deletion MARK as gen",
    "bucketlock": "bucket lock + SW txn\n(bit-lock add/unlink)",
}
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock txn-global txn-pernode txn-mark bucketlock").split())

# (panel, dirs) per column; private comes from the writers-only panel
COLUMNS = [
    ("churn_w", "private",
     "PRIVATE directories — no other writer uses them"),
    ("churn_share", "same-node",
     "SHARED by a pair of writers on the SAME node"),
    ("churn_share", "cross-node",
     "SHARED by a pair of writers on DIFFERENT nodes"),
]

rows = dp.load(CSV)
MODES = [m for m in ("inplace", "alloc")
         if any(r.get("mode") == m and r.get("panel") == "churn_share"
                for r in rows)]
if not MODES:
    raise SystemExit("no churn_share rows in %s: run "
                     "PANELS=churn_share scripts/run_dcache_churn.sh" % CSV)
MODE_TITLE = {
    "inplace": "IN PLACE — d_delete to a negative, then d_instantiate "
               "(no allocation, no LRU traffic)",
    "alloc": "ALLOCATING — dc_unlink + dc_add per toggle (allocation, LRU "
             "enqueue/dequeue, a free on every pair)",
}

fig, axes = plt.subplots(len(MODES), len(COLUMNS),
                         figsize=(19, 7.4 * len(MODES)), squeeze=False)
for row, mode in enumerate(MODES):
    for col, (panel, dirs, title) in enumerate(COLUMNS):
        ax = axes[row][col]
        for e in ENGINES:
            dp.plot_series(ax, dp.series(rows, "writers", "mchurn_s",
                                         panel=panel, dirs=dirs, engine=e,
                                         mode=mode),
                           COLOR[e], MARKER[e], LABEL[e], lw=2.1)
        if ax.has_data():
            plain_thread_x(ax, [0, 8, 16, 24, 32, 40, 48])
            plain_y(ax)
        ax.set_title(title + "\ninsert+remove Mops/s vs writer count, "
                     "writers flat out", fontsize=9.5)
        ax.set_xlabel("churn writer threads")
        ax.set_ylabel("insert+remove Mops/s   (higher is better)")
        ax.grid(alpha=0.3, ls=":")
        dp.legend(ax, fontsize=8, loc="best")
        dp.ratio_strip(ax, rows, "writers", "mchurn_s", ENGINES, COLOR, MARKER,
                       panel=panel, dirs=dirs, mode=mode)
    axes[row][1].annotate(MODE_TITLE[mode], xy=(0.5, 1.0),
                          xycoords="axes fraction", xytext=(0, 40),
                          textcoords="offset points", ha="center", fontsize=11,
                          fontweight="bold").set_in_layout(False)

fig.suptitle("Userspace dcache — create/delete vs directory sharing   ·   "
             "2×96-core EPYC, 8-core NUMA nodes (one per CCX)\nstrips: every "
             "engine ÷ seqlock at the same x (dashed = parity)", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.95], h_pad=5.0)
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
