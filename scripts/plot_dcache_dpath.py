#!/usr/bin/env python3
"""
Reverse walk (dentry_path_raw) under a concurrent rename load.

Readers ask "where is this object NOW?" of a leaf handle pinned at seed time
(dc_dentry_path), while 8 writers rename.  This is the one reader the kernel
itself serves with a whole-path SNAPSHOT -- __dentry_path brackets its climb on
rename_lock (read_seqbegin_or_lock) -- so every engine here gives the same
guarantee, each with its own mechanism:

  seqlock      the kernel's __dentry_path, step for step (both seqlock arms run
               identical reverse-walk code; only their dc_lookup differs)
  txn-global   climb + rename_gen bracket
  txn-pernode  climb + per-host generation double collect (+ leaf mark re-test)
  txn-mark     climb + deletion-mark double collect
  bucketlock   climb + deletion-mark double collect

On the txn engines each hop reads the name off the host's d_top -- the shell
that names it, or the host itself -- in two loads.  Until 2026-10-01 the
shells formed a chain and each hop climbed it, O(renames of that object not
yet folded): 2-3 shells at this load, which is why seqlock led at 2-8 readers
then (0.63-0.75x).  The readers own no leaves: every handle is one of the
writers' 256 leaves at every reader count (before 2026-09-30 each reader also
owned 32 leaves nothing moves, so only 8/(readers+8) of the walks targeted a
moving object).

The writers are PACED to 100k renames/s (bench --rename-rate), the same
offered load for every engine; the right panel shows what each engine's
writers actually achieved against that target.  Hollow markers: they could not
sustain it -- read_seqbegin_or_lock's locked pass takes rename_lock, so a
reverse-walk-heavy seqlock load starves its renamers -- and that reader number
was measured under a lighter load.

Data: scripts/dcache_sweep.csv (panel dpath_scale).  2x96 EPYC.
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
                     os.path.join(HERE, os.pardir, "figures", "dcache_dpath.png"))

COLOR = {"seqlock": "#D55E00", "seqlock-snapshot": "#E69F00",
         "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "seqlock-snapshot": "P", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D",
          "bucketlock": "X"}
LABEL = {
    "seqlock-snapshot": "seqlock-snapshot — same __dentry_path\n(reverse walk identical to seqlock)",
    "seqlock": "seqlock — kernel __dentry_path\n(rename_lock, bounded retry)",
    "txn-global": "urcu-txn — GLOBAL rename_gen\n(bracketed climb)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(double-collect climb)",
    "txn-mark": "urcu-txn — deletion MARK\n(double-collect climb)",
    "bucketlock": "bucket lock + SW txn\n(deletion-mark climb)",
}
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock").split())
rows = dp.load(CSV)
target = next((float(r["rate_target"]) / 1e6 for r in rows
               if r["panel"] == "dpath_scale" and float(r["rate_target"]) > 0),
              None)

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.5, 7.4))
for e in ENGINES:
    dp.plot_series(ax1, dp.series(rows, "readers", "mlookups_s",
                                  panel="dpath_scale", engine=e),
                   COLOR[e], MARKER[e], LABEL[e])
    dp.plot_series(ax2, dp.series(rows, "readers", "mrenames_s",
                                  panel="dpath_scale", engine=e),
                   COLOR[e], MARKER[e], LABEL[e])
if target:
    ax2.axhline(target, color="0.4", ls="--", lw=1.0,
                label=f"offered: {target:g} M renames/s")
for ax in (ax1, ax2):
    if ax.has_data():
        ticks = [0, 32, 64, 96, 128, 160, 184]
        ax.set_xlim(0, max(ticks) * 1.02)
        ax.xaxis.set_major_locator(FixedLocator(ticks))
        ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.set_ylim(bottom=0)
        ax.yaxis.set_major_formatter(_num)
    ax.grid(alpha=0.3, ls=":")
    dp.legend(ax, fontsize=7.5, loc="best")
    ax.set_xlabel("dedicated reader threads (each reporting a random leaf's path)")
ax1.set_title("reverse-walk reader scaling — 8 writers paced to 100k renames/s,\n"
              "sweep readers; one hw thread per core.  256 leaves share the offered\n"
              "rate; a txn hop reads its name off the host's d_top (two loads)",
              fontsize=9.5)
ax1.set_ylabel("reverse walks / s   (Mdpaths/s, higher is better)")
dp.ratio_strip(ax1, rows, "readers", "mlookups_s", ENGINES, COLOR, MARKER,
               panel="dpath_scale")
ax2.set_title("the rename load each engine's writers actually carried\n"
              "(8 writers, achieved renames/s vs reader count; dashed = offered)",
              fontsize=9.5)
ax2.set_ylabel("renames / s   (Mrenames/s, higher is better)")
fig.suptitle("Userspace dcache — reverse walk (dentry_path_raw) under concurrent "
             "renames   ·   every engine gives the kernel's snapshot guarantee   ·   "
             "2×96-core EPYC\nstrip: every engine ÷ seqlock at the same reader "
             "count (paced points only; dashed = parity)", fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.93])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
