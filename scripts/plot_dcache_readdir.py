#!/usr/bin/env python3
"""
Directory listing (readdir) under a concurrent rename load -- the S3 companion.

Readers ENUMERATE a directory (dc_readdir) rather than doing a full-path leaf
lookup:

  seqlock (both arms)  per-DIRECTORY rwsem readdir (the honest kernel-inode-
               rwsem analogue: readers of a dir share a read-lock, a rename
               write-locks only its affected parents -- NOT one global lock)
  txn arms, bucketlock  lock-free RCU walk of the child hlist

Note: readdir's READER path reads NO generation counter at all (a concurrently
renamed child may or may not appear -- POSIX-soft, but it never tears), so the
four txn arms run IDENTICAL readdir reader code, and the two seqlock arms share
theirs too; any spread inside either group is a writer-side effect.  Every arm
is plotted, each with its ratio to seqlock in the strip under its panel.

The writers are PACED (bench --rename-rate): 100k renames/s in the reader-
scaling panel, 12.5k/s per writer in the writer panel, the same offered load
for every engine at each x.  Hollow markers: the writers could not sustain it
-- the seqlock readdir's per-directory rwsem hands its renamers too little
time -- so that reader number was measured under a lighter load.

Data: scripts/dcache_sweep.csv (panels readdir_scale, readdir_w).  2x96 EPYC.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               LogLocator, NullFormatter)
import dcache_plotlib as dp

_num = FuncFormatter(lambda v, pos: f"{v:g}")


def plain_y(ax):
    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(_num)


def plain_thread_x(ax, ticks):
    ax.set_xlim(0, max(ticks) * 1.02)
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
    ax.xaxis.set_minor_formatter(NullFormatter())


HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_sweep.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_readdir.png"))

COLOR = {"seqlock": "#D55E00", "seqlock-snapshot": "#E69F00",
         "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "seqlock-snapshot": "P", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D",
          "bucketlock": "X"}
LABEL = {
    "seqlock-snapshot": "seqlock-snapshot — same per-dir rwsem\n(readdir code identical to seqlock)",
    "seqlock": "seqlock — per-directory rwsem\n(kernel inode-rwsem analogue)",
    "txn-global": "urcu-txn — GLOBAL rename_gen\n(lock-free RCU child-walk)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(lock-free RCU child-walk)",
    "txn-mark": "urcu-txn — deletion MARK as gen\n(lock-free RCU child-walk)",
    "bucketlock": "bucket lock + SW txn\n(lock-free RCU child-walk)",
}
# Every arm: the txn arms share their readdir reader code and are expected to
# coincide; plotting them all shows whether they do.
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock").split())
rows = dp.load(CSV)


def series(panel, eng, xcol):
    # mlookups_s is the readdir CALL rate in readdir mode
    return dp.series(rows, xcol, "mlookups_s", panel=panel, engine=eng)


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.5, 7.4))

# ---- Panel 1: reader scaling at fixed writer load ---------------------------
for e in ENGINES:
    dp.plot_series(ax1, series("readdir_scale", e, "readers"), COLOR[e],
                   MARKER[e], LABEL[e])
if ax1.has_data():
    plain_thread_x(ax1, [0, 32, 64, 96, 128, 160, 184])
    plain_y(ax1)
ax1.set_title("readdir reader scaling — 8 writers paced to 100k renames/s,\n"
              "sweep readers to 184; fixed dir size (~32 children), one hw\n"
              "thread/core (every rwsem reader writes the lock's shared line;\n"
              "the RCU walk writes nothing shared)", fontsize=9.5)
ax1.set_xlabel("dedicated reader threads (each listing a random dir)")
ax1.set_ylabel("directory listings / s   (Mreaddir/s, higher is better)")
dp.ratio_strip(ax1, rows, "readers", "mlookups_s", ENGINES, COLOR, MARKER,
               panel="readdir_scale")

# ---- Panel 2: reader throughput vs writer (rename) load ---------------------
for e in ENGINES:
    dp.plot_series(ax2, series("readdir_w", e, "writers"), COLOR[e], MARKER[e],
                   LABEL[e])
if ax2.has_data():
    plain_thread_x(ax2, [0, 8, 16, 24, 32, 40, 48])
    plain_y(ax2)
ax2.set_title("readdir vs writer load — 32 readers, W writers at 12.5k\n"
              "renames/s each; namespace held constant\n"
              "(each rename write-locks its dirs → the seqlock readdir is excluded;\n"
              "the RCU walk never blocks on a writer)", fontsize=9.5)
ax2.set_xlabel("concurrent writer threads (renaming)")
ax2.set_ylabel("directory listings / s   (Mreaddir/s, higher is better)")
dp.ratio_strip(ax2, rows, "writers", "mlookups_s", ENGINES, COLOR, MARKER,
               panel="readdir_w")

for ax in (ax1, ax2):
    ax.grid(alpha=0.3, ls=":")
    dp.legend(ax, fontsize=7.5, loc="best")

fig.suptitle("Userspace dcache — directory listing (readdir) under concurrent "
             "renames   ·   lock-free RCU child-walk vs per-directory rwsem   ·   "
             "2×96-core EPYC\nstrips: every engine ÷ seqlock at the same x "
             "(paced points only; dashed = parity)", fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.93])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
