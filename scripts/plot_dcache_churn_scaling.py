#!/usr/bin/env python3
"""Plot scripts/dcache_churn_scaling.csv -> figures/dcache_churn_scaling.png.

Insert/remove WRITER scaling to 192.  The seqlock baseline's write path is now
kernel-faithful and FINE-GRAINED -- a per-bucket hlist_bl bit lock (bit 0 of the
bucket head word) plus the per-directory rwsem, exactly the kernel's add/unlink
locking, NOT one global mutator lock.  So add/unlink in different dirs and buckets
proceed in parallel, and this figure measures the write path, not a serialization
artifact.  (Renames still take rename_lock + a cross-dir s_vfs_rename_mutex, as
the kernel does -- but churn is add/unlink, which take neither.)

Two stacked bottlenecks the naive fixed-ndirs glibc run hides are removed:
default jemalloc for the allocator, and ndirs scaled WITH the writer count for
the shared child-hlist HEADS.  Linear axes.

Left panel: one engine (the seqlock baseline) across three ndirs -- how much
decontention buys (matched ndirs=writers is child-hlist-head bound; 16*writers
lifts it ~6x at the top).  Right panel: the widest ndirs (16*writers), every engine -- who scales, on the
ALLOCATING path (dc_unlink + dc_add per toggle).  Churn is BUMP-FREE (add never
bumped; unlink no longer does), so the three txn arms are indistinguishable.
The seqlock baseline leads here.

Read that as a statement about the allocating path, not about create/delete in
general.  Profiled 2026-09-29, this path spends 39-57% of its cycles on the
per-NUMA-node LRU shard lock (one list per node, as the kernel's list_lru; 24
nodes on this machine), which every engine takes on each add and unlink.  The
MW txn engines additionally pay a descriptor per commit: batch retirement
(URCU_TXN_SLAB_BATCH, figures/dcache_slabroute.png) recovers part of that at
1-4 writers and nothing from 8 up.  On the path a kernel takes when a name is
removed and created again -- d_delete to a negative, then d_instantiate, no
allocation and no LRU operation -- the order reverses: every txn arm and the
bucket lock beat seqlock, by up to 2.4x (dcache_churn.png, top row).

Env: ENGINES / OUT overrides as usual.
"""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import dcache_plotlib as dp
from matplotlib.ticker import FixedLocator, FixedFormatter

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_churn_scaling.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures",
                                  "dcache_churn_scaling.png"))

rows = [r for r in csv.DictReader(open(CSV)) if r["conserved"] == "OK"]
d = collections.defaultdict(dict)
for r in rows:
    d[(r["dirmul"], int(r["writers"]))][r["engine"]] = float(r["mchurn_s"])
Ws = sorted({int(r["writers"]) for r in rows})

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARK = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D",
        "bucketlock": "X"}
ELAB = {"seqlock": "seqlock (kernel baseline)",
        "txn-global": "txn — GLOBAL rename_gen",
        "txn-pernode": "txn — PER-NODE host gen",
        "txn-mark": "txn — deletion MARK",
        "bucketlock": "bucket lock + SW txn"}
DCOL = {"writers/16": "#CC79A7", "writers": "#E69F00", "16*writers": "#009E73"}
DMARK = {"writers/16": "v", "writers": "o", "16*writers": "D"}
DLAB = {"writers/16": "ndirs = writers ÷ 16", "writers": "ndirs = writers",
        "16*writers": "ndirs = 16×writers"}
DECON_ENGINE = "seqlock"


def linx(ax):
    ax.set_xlim(0, 196)
    ax.set_ylim(bottom=0)
    ticks = [1, 16, 32, 64, 96, 128, 160, 192]
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 7.6))

for dm in ("writers/16", "writers", "16*writers"):
    ys = [d[(dm, w)][DECON_ENGINE] for w in Ws]
    ax1.plot(Ws, ys, color=DCOL[dm], marker=DMARK[dm], lw=2.2, ms=6.5,
             label=DLAB[dm])
linx(ax1)
ax1.set_title("Directory decontention unlocks the write path (seqlock)\n"
              "insert+remove Mops/s vs writers, at three directory counts\n"
              "each writer toggles 32 slots; the shared child-hlist HEADS are\n"
              "the contention removed as ndirs grows past the writer count",
              fontsize=9.5)
ax1.set_xlabel("writer threads")
ax1.set_ylabel("insert+remove Mops/s   (higher is better)")
ax1.grid(alpha=0.3, ls=":")
ax1.legend(fontsize=9, loc="upper left")

for e in ("seqlock", "txn-mark", "bucketlock", "txn-pernode", "txn-global"):
    ys = [d[("16*writers", w)][e] for w in Ws]
    ax2.plot(Ws, ys, color=COLOR[e], marker=MARK[e], lw=2.2, ms=6.5,
             label=ELAB[e])
linx(ax2)
ax2.set_title("Decontended (ndirs = 16×writers, jemalloc) — who scales\n"
              "insert+remove Mops/s vs writers.  Churn is BUMP-FREE, so the\n"
              "three txn arms coincide; the bit-lock baseline leads on this\n"
              "ALLOCATING path, LRU-lock bound -- in place it reverses (strip: every engine\n"
              "÷ seqlock; dashed = parity)", fontsize=9.5)
ax2.set_xlabel("writer threads")
ax2.set_ylabel("insert+remove Mops/s   (higher is better)")
ax2.grid(alpha=0.3, ls=":")
ax2.legend(fontsize=9, loc="upper left")
dp.ratio_strip(ax2, rows, "writers", "mchurn_s",
               ("seqlock", "txn-mark", "bucketlock", "txn-pernode", "txn-global"),
               COLOR, MARK, dirmul="16*writers")

fig.suptitle("Userspace dcache — INSERT/REMOVE writer scaling to 192 "
             "(default jemalloc, linear axes)   ·   2×96-core EPYC", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.95])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
