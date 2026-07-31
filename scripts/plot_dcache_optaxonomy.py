#!/usr/bin/env python3
"""Plot scripts/dcache_optaxonomy.csv -> figures/dcache_optaxonomy.png.

The four mutating operations, per engine.  The taxonomy is a 2x2 of
{file|directory} x {same-dir|cross-dir}, and the two axes ARE the two code
branches: same-vs-cross gates the O(depth) ancestry cycle check, the d_moving
lock and the reparent store, while file-vs-dir sets how many reader walks the
relocation invalidates.  Two of the four cells had no perf bench at all until
this sweep -- `rename` (same-dir file, the most common op a filesystem issues)
and `directory move`.

LEFT column, leaf ops (bench_dcache, file leaves): rename vs file move vs
exchange.  The rename-to-move gap on the WRITER row is the price of the
cross_parent branch with no subtree attached -- the cleanest reading of that
branch anywhere in the experiment.

RIGHT column, directory ops (bench_dcache_height at one height): the two ONE-WAY
arms move a spare subtree that no reader path spells, so they price the op
itself; the two EXCHANGE arms keep the moved subtree on reader paths, so they
also carry the B^H walk invalidation.  Reading a one-way bar against its
exchange counterpart separates the two costs.

TOP row is the writer (Mrenames/s), BOTTOM row the reader (Mlookups/s) measured
concurrently.  Linear axes.
"""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_optaxonomy.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures",
                                  "dcache_optaxonomy.png"))

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
ELAB = {"seqlock": "seqlock (kernel baseline)",
        "txn-global": "txn — GLOBAL rename_gen (a seqcount)",
        "txn-pernode": "txn — PER-NODE host gen",
        "txn-mark": "txn — deletion MARK",
        "bucketlock": "bucket lock + SW txn"}
ORDER = ("seqlock", "txn-global", "txn-pernode", "txn-mark", "bucketlock")

# (panel, op) -> x label.  Only the taxonomy's FILE row is plotted on the leaf
# side; the directory-leaf rows stay in the CSV (they are empty-directory ops,
# which dcache_optype.png already charts as the causality-bump A/B).
LEAF_OPS = [("rename", "rename\n(same-dir file)"),
            ("move", "file move\n(cross-dir file)"),
            ("exchange", "exchange\n(two shells, 1 commit)")]
DIR_OPS = [("rename", "directory rename\n(one-way)"),
           ("move", "directory move\n(one-way)"),
           ("exchange", "directory rename\n(exchange)"),
           ("exchange-cross", "directory move\n(exchange)")]

rows = [r for r in csv.DictReader(open(CSV)) if r["conserved"] == "OK"]
val = collections.defaultdict(dict)          # (panel, op) -> engine -> (lk, rn)
for r in rows:
    if r["panel"] == "leaf" and r["leaftype"] != "file":
        continue
    val[(r["panel"], r["op"])][r["engine"]] = (float(r["mlookups_s"]),
                                               float(r["mrenames_s"]))
engines = [e for e in ORDER if any(e in v for v in val.values())]

fig, axes = plt.subplots(2, 2, figsize=(15.5, 9.0))
for col, (panel, ops, title) in enumerate((
        ("leaf", LEAF_OPS, "Leaf operations — bench_dcache (file leaves)"),
        ("dir", DIR_OPS, "Directory operations — bench_dcache_height"))):
    present = [(op, lab) for op, lab in ops if (panel, op) in val]
    x = np.arange(len(present))
    w = 0.8 / max(len(engines), 1)
    for row, (metric, ylab) in enumerate(((1, "writer  Mrenames/s"),
                                          (0, "reader  Mlookups/s"))):
        ax = axes[row][col]
        for i, e in enumerate(engines):
            ys = [val[(panel, op)].get(e, (0.0, 0.0))[metric] for op, _ in present]
            ax.bar(x + i * w - 0.4 + w / 2, ys, w, color=COLOR[e],
                   edgecolor="white", linewidth=0.6,
                   label=ELAB[e] if (row == 0 and col == 0) else None)
        ax.set_xticks(x)
        ax.set_xticklabels([lab for _, lab in present], fontsize=9)
        ax.set_ylabel(ylab)
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", alpha=0.25)
        ax.set_axisbelow(True)
        if row == 0:
            ax.set_title(title, fontsize=11)

fig.suptitle("dcache op taxonomy: all four mutating operations, per engine",
             fontsize=13)
fig.legend(loc="lower center", ncol=len(engines), frameon=False, fontsize=9)
fig.tight_layout(rect=(0, 0.05, 1, 0.96))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
