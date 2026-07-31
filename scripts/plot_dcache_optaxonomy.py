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
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000",
         # the bucketlock engine's three chain strategies: greys off the shipped
         # arm's black, so a reader groups them by eye as one engine.
         "bucketlock-chainlock": "#7F7F7F", "bucketlock-swmw": "#4D4D4D",
         "bucketlock-swmw-pad": "#B0B0B0"}
ELAB = {"seqlock": "seqlock (kernel baseline)",
        "txn-global": "txn — GLOBAL rename_gen (a seqcount)",
        "txn-pernode": "txn — PER-NODE host gen",
        "txn-mark": "txn — deletion MARK",
        "bucketlock": "bucket lock — FOLD LOCK chain (shipped, 176 B)",
        "bucketlock-chainlock": "bucket lock — CHAIN LOCK (legacy, 176 B)",
        "bucketlock-swmw": "bucket lock — all-MW chain (168 B)",
        "bucketlock-swmw-pad": "bucket lock — all-MW chain, padded (176 B ctl)"}
ORDER = ("seqlock", "txn-global", "txn-pernode", "txn-mark", "bucketlock",
         "bucketlock-chainlock", "bucketlock-swmw", "bucketlock-swmw-pad")

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
# sharey="row": both panels of a row carry ONE scale, so bar height means the
# same number left and right.  Without it each subplot auto-scales and a taller
# directory-panel bar can stand for a smaller throughput than a shorter leaf-panel
# one -- the columns are different harnesses, but the units are identical and the
# reader is invited to compare them.  Costs the directory reader panel some
# vertical resolution (its ceiling is ~half the leaf panel's); that is the honest
# trade.
fig, axes = plt.subplots(2, 2, figsize=(15.5, 9.0), sharey="row")
handles = {}                                 # label -> bar handle, for one legend
for col, (panel, ops, title) in enumerate((
        ("leaf", LEAF_OPS, "Leaf operations — bench_dcache (file leaves)"),
        ("dir", DIR_OPS, "Directory operations — bench_dcache_height"))):
    present = [(op, lab) for op, lab in ops if (panel, op) in val]
    # Per-PANEL engine list: a panel re-run with a wider engine set than the
    # other must not paint the absent engines as zero-height bars, which reads
    # as "measured zero" rather than "not measured".
    engines = [e for e in ORDER
               if any(e in val[(panel, op)] for op, _ in present)]
    x = np.arange(len(present))
    w = 0.8 / max(len(engines), 1)
    for row, (metric, ylab) in enumerate(((1, "writer  Mrenames/s"),
                                          (0, "reader  Mlookups/s"))):
        ax = axes[row][col]
        for i, e in enumerate(engines):
            # nan (not 0) for an op an engine has no OK row for -- a dropped
            # run must leave a gap, never a bar that reads as a measurement.
            ys = [val[(panel, op)].get(e, (np.nan, np.nan))[metric]
                  for op, _ in present]
            b = ax.bar(x + i * w - 0.4 + w / 2, ys, w, color=COLOR[e],
                       edgecolor="white", linewidth=0.6)
            handles.setdefault(ELAB[e], b)
        ax.set_xticks(x)
        ax.set_xticklabels([lab for _, lab in present], fontsize=9)
        if col == 0:                 # shared row scale -> one label, on the left
            ax.set_ylabel(ylab)
        ax.set_ylim(bottom=0)
        ax.grid(axis="y", alpha=0.25)
        ax.set_axisbelow(True)
        if row == 0:
            ax.set_title(title, fontsize=11)

fig.suptitle("dcache op taxonomy: all four mutating operations, per engine",
             fontsize=13)
fig.legend([handles[k] for k in handles], list(handles),
           loc="lower center", ncol=min(len(handles), 4), frameon=False,
           fontsize=9)
fig.tight_layout(rect=(0, 0.05, 1, 0.96))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
