#!/usr/bin/env python3
"""Plot scripts/dcache_churn_scaling.csv -> figures/dcache_churn_scaling.png.

Insert/remove WRITER scaling to 192, with the two bottlenecks the fixed-ndirs
glibc run in dcache_churn.png hides both removed: default jemalloc for the
allocator, and ndirs scaled WITH the writer count for the shared child-hlist
heads.  Linear axes -- the message is the scaling divergence, which linear shows
starkly (contended and seqlock lines flat near the axis, the decontended txn
line climbing).

Left panel: one engine (txn-mark) across three ndirs -- how much decontention
buys.  Right panel: the widest ndirs (16*writers), four engines -- who scales.
Measured on the bump-free engine (unlink owes no walk-causality bump), so churn
generates NO gen traffic and the three txn arms converge; the crossover the old
figure showed was the unlink bump, now removed (see dcache_optype.png for the
crossover that remains, on directory operations).

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


def linx(ax):
    ax.set_xlim(0, 196)
    ax.set_ylim(bottom=0)
    ticks = [1, 16, 32, 64, 96, 128, 160, 192]
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6.2))

for dm in ("writers/16", "writers", "16*writers"):
    ys = [d[(dm, w)]["txn-mark"] for w in Ws]
    ax1.plot(Ws, ys, color=DCOL[dm], marker=DMARK[dm], lw=2.2, ms=6.5,
             label=DLAB[dm])
linx(ax1)
ax1.set_title("Directory decontention unlocks the write path (txn-mark)\n"
              "insert+remove Mops/s vs writers, at three directory counts\n"
              "each writer toggles 32 slots; shared child-hlist HEADS are the\n"
              "contention removed as ndirs grows past the writer count",
              fontsize=9.5)
ax1.set_xlabel("writer threads")
ax1.set_ylabel("insert+remove Mops/s   (higher is better)")
ax1.grid(alpha=0.3, ls=":")
ax1.legend(fontsize=9, loc="upper left")

for e in ("txn-mark", "txn-pernode", "txn-global", "seqlock"):
    ys = [d[("16*writers", w)][e] for w in Ws]
    ax2.plot(Ws, ys, color=COLOR[e], marker=MARK[e], lw=2.2, ms=6.5,
             label=ELAB[e])
linx(ax2)
ax2.set_title("Decontended (ndirs = 16×writers, jemalloc) — who scales\n"
              "insert+remove Mops/s vs writers.  Churn is BUMP-FREE (add never\n"
              "bumped; unlink no longer does), so all three txn arms CONVERGE\n"
              "and scale; only seqlock (mutator-lock serialized) stays flat",
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
