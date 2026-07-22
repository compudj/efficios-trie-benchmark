#!/usr/bin/env python3
"""Plot scripts/dcache_optype.csv -> figures/dcache_optype.png.

File-operation vs directory-operation reader scaling, same benchmark, only the
leaf TYPE differs -- which flips whether the writer's walk-causality COUNTER is
bumped.  The file/dir distinction is a COUNTER-arm phenomenon:

  left  (file ops): a file is never an interior waypoint, so the counter arms
        (seqlock d_seq, global rename_gen, per-node host gen) SKIP the bump.  The
        GLOBAL arm is a global seqcount -- a seqlock-style bracket over one
        whole-tree counter, no per-hop second pass -- so it is competitive here.
  right (directory ops): the bump fires.  The GLOBAL seqcount's one whole-tree
        bump makes every reader re-walk -> it COLLAPSES; PER-NODE localizes it.

The MARK and bucket-lock arms carry NO counter at all -- the hlist deletion mark
IS the version (the structural edit is the signal) -- so they neither skip nor
fire a bump; they are FLAT across file vs dir, high in both.  seqlock bumps d_seq
regardless of type (kernel-faithful: no file/dir distinction), so its two panels
match.  Linear axes.
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
        "txn-global": "txn — GLOBAL rename_gen (a seqcount)",
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
     "FILE rename/move — a file is never an interior waypoint\nreader Mlk/s vs "
     "readers (8 writers, rename-frac 1).  The counter\narms SKIP the bump; the "
     "GLOBAL seqcount (a seqlock-style\nbracket over one counter) is then "
     "competitive -- even edging the rest"),
    ("dir", ax2,
     "DIRECTORY rename/move — the counter bump FIRES\nsame axes.  The GLOBAL "
     "seqcount's whole-tree bump collapses it;\nPER-NODE localizes.  MARK / bucket "
     "lock carry NO counter (the\ndeletion mark is the version) — flat file-vs-dir, "
     "high in both")):
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
