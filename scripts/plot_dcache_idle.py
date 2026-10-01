#!/usr/bin/env python3
"""
The four reader ops at REST -- no renames -- scaling with the reader count.

Every other reader-scaling panel runs against 8 writers renaming (paced to
100k renames/s, or flat out).  These are the same four panels with those
writers idle: the same 8 writer threads own the same namespace on the same
cores, paced to 0.001 renames/s, so their only renames -- one each -- fall in
the warm-up and the timed window is rename-free.  Point for point, each panel
is the at-rest baseline of its loaded counterpart:

  idle_scale    probing lookups (random dir, mostly negative hits)  ~ split_scale
  idle_hit      positive hits on the writers' objects               ~ hit_scale
  idle_dpath    reverse walk from a pinned handle                   ~ dpath_scale
  idle_readdir  directory listing, 32 children per directory        ~ readdir_scale

What they show is what each engine's READER costs with nothing to defend
against: the per-hop work a rename load hides -- per-node generation sampling,
the bucket lock's pointer decoding, the seqlock's d_seq and rename_lock reads,
a txn reverse-walk hop's d_top load.

Data: scripts/dcache_sweep.csv (panels idle_scale, idle_hit, idle_dpath,
idle_readdir).  2x96 EPYC, one hw thread per core.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               NullFormatter)
import dcache_plotlib as dp

_num = FuncFormatter(lambda v, pos: f"{v:g}")

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_sweep.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_idle.png"))

COLOR = {"seqlock": "#D55E00", "seqlock-snapshot": "#E69F00",
         "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "seqlock-snapshot": "P", "txn-global": "o",
          "txn-pernode": "^", "txn-mark": "D", "bucketlock": "X"}
LABEL = {
    "seqlock": "seqlock — kernel RCU walk\n(hand-over-hand d_seq)",
    "seqlock-snapshot": "seqlock-snapshot — whole-walk\nrename_lock bracket",
    "txn-global": "urcu-txn — GLOBAL rename_gen",
    "txn-pernode": "urcu-txn — PER-NODE host gen",
    "txn-mark": "urcu-txn — deletion MARK",
    "bucketlock": "bucket lock + SW txn (mark)",
}
ENGINES = tuple(os.environ.get(
    "ENGINES",
    "seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock").split())

PANELS = [
    ("idle_scale", "probing lookups (random directory: mostly negative hits)",
     "lookups / s   (Mlookups/s)"),
    ("idle_hit", "positive hits on the writers' (idle) objects",
     "lookups / s   (Mlookups/s)"),
    ("idle_dpath", "reverse walk from a pinned leaf handle (dentry_path_raw)",
     "reverse walks / s   (Mdpaths/s)"),
    ("idle_readdir", "directory listing (32 children per directory)",
     "listings / s   (Mreaddir/s)"),
]

rows = dp.load(CSV)
fig, axes = plt.subplots(2, 2, figsize=(14, 11.5))
ticks = [0, 32, 64, 96, 128, 160, 184]
for ax, (panel, title, ylabel) in zip(axes.flat, PANELS):
    for e in ENGINES:
        # mlookups_s is the readdir CALL rate in readdir mode
        dp.plot_series(ax, dp.series(rows, "readers", "mlookups_s",
                                     panel=panel, engine=e),
                       COLOR[e], MARKER[e], LABEL[e])
    if ax.has_data():
        ax.set_xlim(0, max(ticks) * 1.02)
        ax.xaxis.set_major_locator(FixedLocator(ticks))
        ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.set_ylim(bottom=0)
        ax.yaxis.set_major_formatter(_num)
    ax.grid(alpha=0.3, ls=":")
    ax.set_title(title, fontsize=10)
    ax.set_ylabel(ylabel + ", higher is better", fontsize=9)
    ax.set_xlabel("dedicated reader threads (8 idle writer threads beside them)")
    dp.legend(ax, fontsize=7.2, loc="upper left")
    dp.ratio_strip(ax, rows, "readers", "mlookups_s", ENGINES, COLOR, MARKER,
                   panel=panel)
fig.suptitle("Userspace dcache — the four reader ops AT REST (no renames in the "
             "timed window)   ·   2×96-core EPYC, one hw thread per core\n"
             "the at-rest baseline of the rename-load panels: same namespace, "
             "same 8 writer threads, idle   ·   strip: every engine ÷ seqlock "
             "(dashed = parity)", fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.95])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
