#!/usr/bin/env python3
"""
Positive-hit lookups of objects being moved, under a concurrent rename load.

Readers look up each leaf's CURRENT path, as published by the leaf's owner
(bench --hit-current), while 8 writers rename those same leaves: nearly every
lookup is a positive hit on an object that is being moved right now.  This is
the dense reader/rename interaction the probing panels (split_scale, whose
terminals are ~97% negative hits on dentries no rename touches) barely reach:
d_seq retries on the seqlock engine, shell resolution on the txn engines.

The namespace is the writers' 256 leaves at every reader count: the readers own
none, so every lookup targets a moving object and adding readers adds no
leaves.  (Before 2026-09-30 each reader also owned 32 leaves nothing moves, so
only 8/(readers+8) of the lookups -- 80% at 2 readers, 4% at 184 -- targeted a
moving object, and the reader axis confounded the two.)

  seqlock           the kernel's RCU walk (hand-over-hand d_seq)
  seqlock-snapshot  the same, bracketed on rename_lock (read_seqbegin_or_lock)
  txn-global        rename_gen bracket
  txn-pernode       per-host generation double collect
  txn-mark          deletion-mark double collect
  bucketlock        deletion-mark double collect, bucket-lock writers

The writers are PACED to 100k renames/s (bench --rename-rate), the same
offered load for every engine; the right panel shows what each engine's
writers achieved against it.  Hollow markers: they could not sustain it, so
that reader number was measured under a lighter load.

Data: scripts/dcache_sweep.csv (panel hit_scale).  2x96 EPYC.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, FixedLocator, FixedFormatter, NullFormatter
import dcache_plotlib as dp

_num = FuncFormatter(lambda v, pos: f"{v:g}")
HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_sweep.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_hit.png"))

COLOR = {"seqlock": "#D55E00", "seqlock-snapshot": "#E69F00",
         "txn-global": "#0072B2", "txn-pernode": "#009E73",
         "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "seqlock-snapshot": "P", "txn-global": "o",
          "txn-pernode": "^", "txn-mark": "D", "bucketlock": "X"}
LABEL = {
    "seqlock": "seqlock — kernel RCU-walk\n(hand-over-hand d_seq)",
    "seqlock-snapshot": "seqlock — whole-walk rename_lock\n(snapshot, as the txn arms give)",
    "txn-global": "urcu-txn — GLOBAL rename_gen",
    "txn-pernode": "urcu-txn — PER-NODE host gen",
    "txn-mark": "urcu-txn — deletion MARK",
    "bucketlock": "bucket lock + SW txn\n(deletion mark)",
}
ENGINES = tuple(os.environ.get(
    "ENGINES",
    "seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock").split())
rows = dp.load(CSV)
target = next((float(r["rate_target"]) / 1e6 for r in rows
               if r["panel"] == "hit_scale" and float(r["rate_target"]) > 0),
              None)

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.5, 7.4))
for e in ENGINES:
    for ax, col in ((ax1, "mlookups_s"), (ax2, "mrenames_s")):
        dp.plot_series(ax, dp.series(rows, "readers", col, panel="hit_scale",
                                     engine=e),
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
    ax.set_xlabel("dedicated reader threads (each looking up a moving leaf)")
ax1.set_title("positive-hit reader scaling — 8 writers paced to 100k renames/s\n"
              "renaming the leaves the readers look up; sweep readers (one hw\n"
              "thread per core)", fontsize=9.5)
ax1.set_ylabel("reader lookup Mops/s   (higher is better)")
dp.ratio_strip(ax1, rows, "readers", "mlookups_s", ENGINES, COLOR, MARKER,
               panel="hit_scale")
ax2.set_title("the rename load each engine's writers actually carried\n"
              "(8 writers, achieved renames/s vs reader count; dashed = offered)",
              fontsize=9.5)
ax2.set_ylabel("renames / s   (Mrenames/s, higher is better)")
fig.suptitle("Userspace dcache — positive-hit lookups of objects being renamed   "
             "·   2×96-core EPYC\nstrip: every engine ÷ seqlock at the same reader "
             "count (paced points only; dashed = parity)", fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.93])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
