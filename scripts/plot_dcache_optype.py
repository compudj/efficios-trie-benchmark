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
        bump makes every reader re-walk; PER-NODE localizes it.

The MARK and bucket-lock arms carry NO counter at all -- the hlist deletion mark
IS the version (the structural edit is the signal) -- so they neither skip nor
fire a bump.  seqlock bumps d_seq regardless of type (kernel-faithful: no
file/dir distinction), so its two panels match.  Linear axes.

Measured 2026-10-02 at 100k renames/s (÷ seqlock, 8-184 readers): file ops --
every txn arm 1.04-1.20x, global included; directory ops -- per-node, mark and
bucket lock 1.07-1.20x, global only 1.01-1.13x.  The global arm's file/dir drop
is real but MILD at a matched load: the "collapse" earlier versions of this
figure showed came from flat-out writers renaming several times faster under
the txn arms than under seqlock.

The writers are PACED (bench --rename-rate), the same offered load for every
engine; hollow markers: they could not sustain it (see dcache_plotlib).
"""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FixedFormatter
import dcache_plotlib as dp

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_optype.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_optype.png"))

rows = dp.load(CSV)
RDs = sorted({int(r["readers"]) for r in rows if r["conserved"] == "OK"})
paced_rate = next((float(r["rate_target"]) for r in rows
                   if float(r.get("rate_target") or 0) > 0), None)

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


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 7.6), sharey=True)

for lt, ax, title in (
    ("file", ax1,
     "FILE rename/move — a file is never an interior waypoint\nreader Mlk/s vs "
     "readers.  The counter arms SKIP the bump,\nso the GLOBAL seqcount (one "
     "whole-tree counter) keeps up"),
    ("dir", ax2,
     "DIRECTORY rename/move — the counter bump FIRES\nsame axes.  The GLOBAL "
     "seqcount's whole-tree bump costs it a\nlittle; PER-NODE localizes; MARK / "
     "bucket lock carry no counter")):
    for e in ORDER:
        dp.plot_series(ax, dp.series(rows, "readers", "mlookups_s",
                                     leaftype=lt, engine=e),
                       COLOR[e], MARK[e], ELAB[e], lw=2.2, ms=6, alpha=1.0)
    linx(ax)
    ax.set_title(title, fontsize=9.5)
    ax.set_xlabel("dedicated reader threads")
    ax.grid(alpha=0.3, ls=":")
    dp.legend(ax, fontsize=8.5, loc="upper left")
    dp.ratio_strip(ax, rows, "readers", "mlookups_s", ORDER, COLOR, MARK,
                   leaftype=lt)
ax1.set_ylabel("reader lookup Mops/s   (higher is better)")

fig.suptitle("Userspace dcache — file vs directory operations, same benchmark: "
             "who does the walk-causality second pass matter for\n"
             f"(8 writers PACED to {dp.rate_label(paced_rate or 0)} renames/s, the "
             "same load for every engine; jemalloc, ndirs = 16×writers)   ·   "
             "2×96-core EPYC\nstrips: every engine ÷ seqlock (paced points only; "
             "dashed = parity)", fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.92])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
