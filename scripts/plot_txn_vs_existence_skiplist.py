#!/usr/bin/env python3
"""
urcu-txn vs. McKenney "existence" — 3-SKIPLIST atomic-move scaling to 192 cores.
The ORDERED dual of plot_txn_vs_existence_scale.py.

Reads scripts/txn_vs_existence_skiplist.csv (best-of-N), writes
figures/txn_vs_existence_skiplist.png.  Three single-axis panels:
  - update scaling under glibc malloc   (aggregate key-moves/s vs updater cores)
  - update scaling under jemalloc       (same, fairer allocator)
  - read scaling                        (aggregate queries/s vs reader cores)

Commit width: existence flips a 6-key group atomically (see the sweep script),
so txn-mp6 is the WIDTH-MATCHED curve; txn-mp1 is the narrowest txn unit and
txn-mp0 puts the whole 15-key rotation in one commit.

The txn-mp0 column was re-measured after the escalation-funnel fix (urcu-txn-dev
c7d86222): before it, a single size-triggered escalation captured the whole
domain in the serialized fallback lane and mp0 collapsed with core count.

Run: python3 scripts/plot_txn_vs_existence_skiplist.py
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "txn_vs_existence_skiplist.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "txn_vs_existence_skiplist.png")

# Okabe-Ito, matching the tree's other figures: txn = blues, existence = orange.
ENGINES = [
    ("existence (flip, 6-key group)", "existence", "#D55E00", "s"),
    ("urcu-txn, 1 move/commit",       "txn-mp1",   "#0072B2", "o"),
    ("urcu-txn, 6 moves/commit",      "txn-mp6",   "#009E73", "^"),
    ("urcu-txn, 15 moves/commit",     "txn-mp0",   "#CC79A7", "v"),
]
rows = list(csv.DictReader(open(CSV)))


def series(alloc, mode, ekey, ycol):
    acc = defaultdict(list)
    for r in rows:
        if r["alloc"] == alloc and r["mode"] == mode and r["engine"] == ekey:
            v = float(r[ycol])
            if v > 0:
                acc[int(r["x"])].append(v)
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs]


PANELS = [
    ("Update scaling — glibc malloc", "glibc", "update", "mmoves_s",
     "aggregate key-moves/s (millions)", "updater cores"),
    ("Update scaling — jemalloc", "jemalloc", "update", "mmoves_s",
     "aggregate key-moves/s (millions)", "updater cores"),
    ("Read scaling (N readers + 1 updater)", "glibc", "read", "mqueries_s",
     "aggregate queries/s (millions)", "reader cores"),
]

"""Core counts are shown on a LINEAR axis with plain integer labels: the sweep is
about behaviour at high core counts, and a log2 axis compresses exactly the 64-192
range where the curves separate."""
MAJOR_TICKS = (1, 32, 64, 96, 128, 191, 192)

fig, axes = plt.subplots(1, 3, figsize=(16.5, 5.0))
for ax, (title, alloc, mode, ycol, ylab, xlab) in zip(axes, PANELS):
    seen_x = set()
    for label, ekey, color, marker in ENGINES:
        xs, ys = series(alloc, mode, ekey, ycol)
        if not xs:
            continue
        seen_x.update(xs)
        ax.plot(xs, ys, marker=marker, color=color, label=label,
                lw=1.8, ms=5, alpha=0.9)
    xs_all = sorted(seen_x)
    major = [x for x in xs_all if x in MAJOR_TICKS]
    ax.set_xlim(0, max(xs_all) * 1.03)
    ax.set_xticks(major)
    ax.set_xticklabels([str(x) for x in major])
    ax.set_xticks(xs_all, minor=True)     # every measured point, unlabelled
    ax.set_ylim(bottom=0)
    ax.ticklabel_format(axis="y", style="plain")
    ax.set_xlabel(xlab)
    ax.set_ylabel(ylab)
    ax.set_title(title, fontsize=11)
    ax.grid(alpha=0.3, ls=":")
    ax.grid(alpha=0.15, ls=":", which="minor", axis="x")
    ax.legend(fontsize=8, loc="best")

fig.suptitle("urcu-txn (MCAS) vs. existence (flip) — three-skiplist atomic key-move, "
             "2x96-core EPYC", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.95])
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=130)
print("wrote", OUT)
