#!/usr/bin/env python3
"""Plot scripts/dcache_optype.csv -> figures/dcache_optype.png.

File-operation vs directory-operation reader scaling, same benchmark, only the
leaf TYPE differs (which flips the writer's walk-causality bump).  Two panels so
the GLOBAL arm's crossover is the visible story:

  left  (file ops, bump SKIPPED): global brackets on a stable whole-tree counter
        and does no per-hop second pass, so it is competitive -- even edging the
        localized arms, whose reader always pays the up-pass.
  right (directory ops, bump):    every bump is tree-wide, every reader
        re-walks, global collapses; per-node/mark localize and scale.

seqlock (kernel-faithful) bumps regardless of type, so its two panels match --
the kernel makes no file/dir distinction.  Linear axes.
"""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FixedFormatter

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_optype.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_optype.png"))

rows = [r for r in csv.DictReader(open(CSV)) if r["conserved"] == "OK"]
d = collections.defaultdict(dict)
for r in rows:
    d[(r["leaftype"], int(r["readers"]))][r["engine"]] = float(r["mlookups_s"])
RDs = sorted({int(r["readers"]) for r in rows})

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARK = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D",
        "bucketlock": "X"}
ELAB = {"seqlock": "seqlock (kernel baseline)",
        "txn-global": "txn — GLOBAL rename_gen",
        "txn-pernode": "txn — PER-NODE host gen",
        "txn-mark": "txn — deletion MARK",
        "bucketlock": "bucket lock + SW txn"}
ORDER = ("bucketlock", "txn-mark", "txn-pernode", "txn-global", "seqlock")


def linx(ax):
    ax.set_xlim(0, max(RDs) + 6)
    ax.set_ylim(bottom=0)
    ticks = [t for t in (2, 32, 64, 96, 128, 160, max(RDs)) if t <= max(RDs)]
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6.2), sharey=True)

for lt, ax, title in (
    ("file", ax1,
     "FILE rename/move — the bump is SKIPPED\nreader Mlookups/s vs readers "
     "(8 writers, rename-frac 1).  Global\nbrackets on a stable counter and does "
     "NO second pass, so it\nis competitive -- even edging the localized arms"),
    ("dir", ax2,
     "DIRECTORY rename/move — the bump fires\nsame axes.  Every bump is "
     "whole-tree, every reader re-walks:\nglobal COLLAPSES; per-node/mark localize "
     "the retry and\nscale.  seqlock bumps regardless of type (kernel-faithful)")):
    for e in ORDER:
        ys = [d[(lt, rd)].get(e, 0) for rd in RDs]
        ax.plot(RDs, ys, color=COLOR[e], marker=MARK[e], lw=2.2, ms=6,
                label=ELAB[e])
    linx(ax)
    ax.set_title(title, fontsize=9.5)
    ax.set_xlabel("dedicated reader threads")
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8.5, loc="upper left")
ax1.set_ylabel("reader lookup Mops/s   (higher is better)")

fig.suptitle("Userspace dcache — file vs directory operations, same benchmark: "
             "who does the walk-causality second pass matter for\n"
             "(8 writers, jemalloc, ndirs = 16×writers, linear axes)   ·   "
             "2×96-core EPYC", fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.93])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
