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
lifts it ~6x at the top).  Right panel: the widest ndirs (16*writers), four
engines -- who scales.  Churn is BUMP-FREE (add never bumped; unlink no longer
does), so the three txn arms are indistinguishable.  The result inverts the old
figure: the faithful bit-lock baseline is the FASTEST here -- ~1.4-2x ahead of
the transactional engine across the range, because a bit-lock + hlist splice is
lighter than txn's per-op MCAS (two commits + a descriptor per churn op); the
txn arms only draw level at the full 192-writer machine.  The txn engine earns
its keep on the READ path and on renames (see dcache_churn.png / dcache_s3.png),
not on pure insert/remove.

Env: ENGINES / OUT overrides as usual.
"""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
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
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7"}
MARK = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D"}
ELAB = {"seqlock": "seqlock (kernel baseline)",
        "txn-global": "txn — GLOBAL rename_gen",
        "txn-pernode": "txn — PER-NODE host gen",
        "txn-mark": "txn — deletion MARK"}
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


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6.2))

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

for e in ("seqlock", "txn-mark", "txn-pernode", "txn-global"):
    ys = [d[("16*writers", w)][e] for w in Ws]
    ax2.plot(Ws, ys, color=COLOR[e], marker=MARK[e], lw=2.2, ms=6.5,
             label=ELAB[e])
# annotate the seqlock lead over the (indistinguishable) txn arms
for w in Ws:
    s = d[("16*writers", w)].get("seqlock")
    t = d[("16*writers", w)].get("txn-mark")
    if s and t and w in (1, 8, 32, 96, 160):
        ax2.annotate(f"{s / t:.1f}×", (w, s), textcoords="offset points",
                     xytext=(0, 8), ha="center", fontsize=8,
                     color=COLOR["seqlock"], fontweight="bold")
linx(ax2)
ax2.set_title("Decontended (ndirs = 16×writers, jemalloc) — who scales\n"
              "insert+remove Mops/s vs writers.  Churn is BUMP-FREE, so the\n"
              "three txn arms coincide; the faithful bit-lock BASELINE LEADS\n"
              "(× = seqlock ÷ txn) — MCAS is per-op overhead on pure churn",
              fontsize=9.5)
ax2.set_xlabel("writer threads")
ax2.set_ylabel("insert+remove Mops/s   (higher is better)")
ax2.grid(alpha=0.3, ls=":")
ax2.legend(fontsize=9, loc="upper left")

fig.suptitle("Userspace dcache — INSERT/REMOVE writer scaling to 192 "
             "(default jemalloc, linear axes)   ·   2×96-core EPYC", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.95])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
