#!/usr/bin/env python3
"""Plot scripts/dcache_churn.csv -> figures/dcache_churn.png.

Create/delete throughput, which no other dcache figure covers: the s3 and height
figures permute a FIXED namespace (rename, exchange); this one churns it.

TWO ROWS, because "create/delete" means two different paths:

  IN PLACE (top, the headline) -- a name removed and created again the way the
    kernel does it: unlink(2) ends in d_delete(), which turns a sole-user dentry
    NEGATIVE in place (fs.dentry-negative = 0), and the next create
    d_instantiate()s that negative.  No allocation, no LRU operation, no
    hash-chain edit (bench --in-place: dc_delete / dc_instantiate).
  ALLOCATING (bottom) -- dc_unlink + dc_add per toggle: allocation, LRU
    enqueue/dequeue and a free on every pair.  The path of ever-NEW names, or of
    a dropping dentry-negative policy.  It spends 39-57% of its cycles on the
    per-node LRU shard lock (perf, 2026-09-29) -- one list per NUMA node, as the
    kernel's list_lru, and this machine has 24 nodes (one per 8-core CCD) -- so
    its writer panel measures that lock as much as any engine.

Measured 2026-09-30 (writers flat out, ÷ seqlock):
  in place    txn-global / txn-pernode 1.19-2.37x, bucketlock 1.20-2.16x,
              txn-mark 0.98-1.78x -- growing with the writer count.  Most of it
              at scale is the per-directory rwsem (the kernel's i_rwsem) the
              seqlock baseline takes and the txn designs do not need (their
              readdir is lock-free, so no writer has a reader to exclude): with
              the rwsem removed seqlock gains 1.72x at 16 writers.  The rest is
              the d_seq write bracket against one cmpxchg, and the walk.
  allocating  seqlock leads (txn 0.60-0.87x, bucketlock 0.74-1.01x): the LRU
              lock above, plus the MW txn engines' per-commit descriptor --
              batch retirement recovers some of that at 1-4 writers only
              (figures/dcache_slabroute.png).
  readers     within +-10% of seqlock in both modes, every arm.

The seqlock baseline's per-directory lock is the vendored Linux kernel
rw_semaphore (dcache_seqlock.c), its hash chains per-bucket bit locks, as the
kernel's.

NOTE these binaries are built -DDC_SPLIT_KEEPID (a re-added dentry is a new
allocation, so the harness's identity checks need logical ids).  Reader rates are
comparable BETWEEN ARMS HERE, but not against the address-default numbers in
dcache_s3.png.

PACING: the writers-only panels run flat out (the writers ARE the
measurement); the reader panels pace the writers to 12.5k ops/s each (bench
--churn-rate), so every engine's readers face one offered churn load.  Hollow
markers: the writers could not sustain it (see dcache_plotlib).

Env: ENGINES="..." to plot a subset.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               LogLocator, NullFormatter)
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
CSV = os.path.join(HERE, "dcache_churn.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_churn.png"))

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

rows = dp.load(CSV)


def series(panel, eng, xcol, ycol, mode):
    return dp.series(rows, xcol, ycol, panel=panel, engine=eng, mode=mode)


MODES = [m for m in ("inplace", "alloc")
         if any(r.get("mode") == m for r in rows)]
MODE_TITLE = {
    "inplace": "IN PLACE — the kernel's same-name churn: d_delete to a negative, "
               "then d_instantiate (no allocation, no LRU traffic)",
    "alloc": "ALLOCATING — dc_unlink + dc_add per toggle: allocation, LRU "
             "enqueue/dequeue and a free on every pair (the ever-new-names path)",
}

fig, axes = plt.subplots(len(MODES), 3, figsize=(19, 7.4 * len(MODES)),
                         squeeze=False)
for row, mode in enumerate(MODES):
    ax1, ax2, ax3 = axes[row]

    # ---- writers only, raw insert/remove scaling -------------------------
    for e in ENGINES:
        dp.plot_series(ax1, series("churn_w", e, "writers", "mchurn_s", mode),
                       COLOR[e], MARKER[e], LABEL[e], lw=2.1)
    if ax1.has_data():
        plain_thread_x(ax1, [0, 8, 16, 24, 32, 40, 48])
        plain_y(ax1)
    ax1.set_title("Writers only — insert/remove Mops/s vs writer count\n"
                  "the pure WRITE path, writers flat out", fontsize=9.5)
    ax1.set_xlabel("churn writer threads")
    ax1.set_ylabel("insert+remove Mops/s   (higher is better)")
    ax1.grid(alpha=0.3, ls=":")
    dp.legend(ax1, fontsize=8, loc="best")
    dp.ratio_strip(ax1, rows, "writers", "mchurn_s", ENGINES, COLOR, MARKER,
                   panel="churn_w", mode=mode)

    # ---- the read path under churn load -----------------------------------
    for e in ENGINES:
        dp.plot_series(ax2, series("churn_rd", e, "writers", "mlookups_s", mode),
                       COLOR[e], MARKER[e], LABEL[e], lw=2.1)
    if ax2.has_data():
        plain_thread_x(ax2, [0, 8, 16, 24, 32, 40, 48])
        plain_y(ax2)
    ax2.set_title("32 readers + W churn writers at 12.5k ops/s each\n"
                  "reader Mops/s — the READ path under create/delete load",
                  fontsize=9.5)
    ax2.set_xlabel("churn writer threads")
    ax2.set_ylabel("reader lookup Mops/s   (higher is better)")
    ax2.grid(alpha=0.3, ls=":")
    dp.legend(ax2, fontsize=8, loc="best")
    dp.ratio_strip(ax2, rows, "writers", "mlookups_s", ENGINES, COLOR, MARKER,
                   panel="churn_rd", mode=mode)

    # ---- reader scaling at fixed churn ------------------------------------
    for e in ENGINES:
        dp.plot_series(ax3, series("churn_scale", e, "readers", "mlookups_s",
                                   mode),
                       COLOR[e], MARKER[e], LABEL[e], lw=2.1)
    if ax3.has_data():
        plain_thread_x(ax3, [0, 32, 64, 96, 128, 160, 184])
        plain_y(ax3)
    ax3.set_title("Reader scaling under constant churn — 8 writers at "
                  "100k ops/s\nreader Mops/s vs reader count, one hw thread "
                  "per core", fontsize=9.5)
    ax3.set_xlabel("dedicated reader threads")
    ax3.set_ylabel("reader lookup Mops/s   (higher is better)")
    ax3.grid(alpha=0.3, ls=":")
    dp.legend(ax3, fontsize=8, loc="best")
    dp.ratio_strip(ax3, rows, "readers", "mlookups_s", ENGINES, COLOR, MARKER,
                   panel="churn_scale", mode=mode)

    # row banner above the three panels
    # (kept out of tight_layout: its width would otherwise be charged to the
    # middle panel and squeeze every column)
    ax2.annotate(MODE_TITLE[mode], xy=(0.5, 1.0), xycoords="axes fraction",
                 xytext=(0, 40), textcoords="offset points", ha="center",
                 fontsize=11, fontweight="bold").set_in_layout(False)

fig.suptitle("Userspace dcache — create/delete under concurrent lookups, no "
             "renames   ·   2×96-core EPYC\nstrips: every engine ÷ seqlock at "
             "the same x (paced points only; dashed = parity)", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.95], h_pad=5.0)
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
