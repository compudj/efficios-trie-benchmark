#!/usr/bin/env python3
"""Plot scripts/dcache_churn_scaling.csv -> figures/dcache_churn_scaling.png.

Insert/remove WRITER scaling to 192, on the ALLOCATING path (dc_unlink + dc_add
per toggle).  The seqlock baseline's write path is kernel-faithful and
FINE-GRAINED -- a per-bucket hlist_bl bit lock (bit 0 of the bucket head word)
plus the per-directory rwsem, exactly the kernel's add/unlink locking, NOT one
global mutator lock -- so this figure measures the write path, not a
serialization artifact.  Every engine's LRU shard lock is the same queued
spinlock (dcache_qspinlock.h), as the kernel's list_lru_one.lock is.

The axis is WHO SHARES A DIRECTORY (column `share`, bench --share).  Each writer
toggles 32 slots spread over 32 directories, one slot each:
  private     nobody else uses them
  same-node   one other writer, on the same NUMA node
  cross-node  one other writer, on another node (8-core nodes, one per CCX)
  all         every writer uses the same 32
Until 2026-10-02 this axis was ndirs = writers/16, writers, 16*writers through
the bench's modulo mapping, which made the sharers -- and whether they were on
one node or several -- a side effect of the writer count.

Left panel: one engine (the seqlock baseline) across the four arms -- what
sharing costs.  Right panel: private directories, every engine -- who scales.
Default jemalloc, linear axes.

Env: ENGINES / OUT overrides as usual.
"""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import dcache_plotlib as dp
from matplotlib.ticker import FixedLocator, FixedFormatter

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.environ.get("CSV", os.path.join(HERE, "dcache_churn_scaling.csv"))
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures",
                                  "dcache_churn_scaling.png"))

rows = [r for r in csv.DictReader(open(CSV)) if r["conserved"] == "OK"]
d = collections.defaultdict(dict)
for r in rows:
    d[(r["share"], int(r["writers"]))][r["engine"]] = float(r["mchurn_s"])

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARK = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D",
        "bucketlock": "X"}
ELAB = {"seqlock": "seqlock (kernel baseline)",
        "txn-global": "txn — GLOBAL rename_gen",
        "txn-pernode": "txn — PER-NODE host gen",
        "txn-mark": "txn — deletion MARK",
        "bucketlock": "bucket lock + SW txn"}
SHARES = ("private", "same-node", "cross-node", "all")
DCOL = {"private": "#009E73", "same-node": "#56B4E9", "cross-node": "#E69F00",
        "all": "#CC79A7"}
DMARK = {"private": "D", "same-node": "o", "cross-node": "^", "all": "v"}
DLAB = {"private": "private — each writer's own 32 directories",
        "same-node": "shared by a pair on ONE node",
        "cross-node": "shared by a pair on TWO nodes",
        "all": "all writers share the same 32"}
DECON_ENGINE = "seqlock"


def pts(share, eng):
    ws = sorted(w for (sh, w), v in d.items() if sh == share and eng in v)
    return ws, [d[(share, w)][eng] for w in ws]


def linx(ax):
    ax.set_xlim(0, 196)
    ax.set_ylim(bottom=0)
    ticks = [1, 16, 32, 64, 96, 128, 160, 192]
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 7.6))

for sh in SHARES:
    ws, ys = pts(sh, DECON_ENGINE)
    if ws:
        ax1.plot(ws, ys, color=DCOL[sh], marker=DMARK[sh], lw=2.2, ms=6.5,
                 label=DLAB[sh])
linx(ax1)
ax1.set_title("What sharing a directory costs (seqlock)\n"
              "insert+remove Mops/s vs writers, by who shares each writer's\n"
              "32 directories (child-list heads, directory locks)",
              fontsize=9.5)
ax1.set_xlabel("writer threads")
ax1.set_ylabel("insert+remove Mops/s   (higher is better)")
ax1.grid(alpha=0.3, ls=":")
ax1.legend(fontsize=9, loc="upper left")

for e in ("seqlock", "txn-mark", "bucketlock", "txn-pernode", "txn-global"):
    ws, ys = pts("private", e)
    if ws:
        ax2.plot(ws, ys, color=COLOR[e], marker=MARK[e], lw=2.2, ms=6.5,
                 label=ELAB[e])
linx(ax2)
ax2.set_title("Private directories (jemalloc) — who scales\n"
              "insert+remove Mops/s vs writers, ALLOCATING path.  Churn is\n"
              "BUMP-FREE, so the three txn arms coincide (strip: every engine\n"
              "÷ seqlock; dashed = parity)", fontsize=9.5)
ax2.set_xlabel("writer threads")
ax2.set_ylabel("insert+remove Mops/s   (higher is better)")
ax2.grid(alpha=0.3, ls=":")
ax2.legend(fontsize=9, loc="upper left")
dp.ratio_strip(ax2, rows, "writers", "mchurn_s",
               ("seqlock", "txn-mark", "bucketlock", "txn-pernode", "txn-global"),
               COLOR, MARK, share="private")

fig.suptitle("Userspace dcache — INSERT/REMOVE writer scaling to 192 "
             "(default jemalloc, linear axes)   ·   2×96-core EPYC", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.95])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
