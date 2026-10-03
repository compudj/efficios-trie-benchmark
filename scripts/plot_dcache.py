#!/usr/bin/env python3
"""
S3 -- userspace dentry-cache: can urcu-txn dissolve rename_lock + d_seq?

Arms:
  seqlock      the kernel's RCU-walk: hand-over-hand per-dentry d_seq,
               rename_lock sampled per walk, consulted only on a miss
  seqlock-snapshot  same engine, every lookup bracketed on rename_lock with the
               kernel's bounded read_seqbegin_or_lock (the txn arms' guarantee)
  txn-global   urcu-txn port, GLOBAL rename_gen walk bracket (deletes d_seq,
               but the reader still brackets one whole-tree counter)
  txn-pernode  urcu-txn port, PER-NODE host generation -- a rename bumps only
               the moved entry's own host counter, so a walk down a disjoint
               path re-reads a disjoint set of counters (no shared cacheline)

Every reader panel here runs the writers PACED (bench --rename-rate): each
engine's readers face one offered rename load.  Flat out, the engines' rename
rates differ by up to two orders of magnitude and the engine with the slower
writers gets its readers measured on a quieter machine; that comparison lives
in plot_dcache_sat.py, labelled as such.  Hollow markers: the writers could not
sustain the offered rate (see dcache_plotlib).

The homogeneous-mix panel is the one place many renaming threads span NUMA
nodes (48 threads, 6 nodes).  There the queued LRU shard lock -- the kernel's
spinlock_t, dcache_qspinlock.h -- costs the txn and bucket-lock engines'
renames 7-14% at a 10% rename fraction and 25-34% at 50%, against a TTAS lock
(2026-10-02 A/B).  Part of it is a harness convoy: their shell folds run in
call_rcu callbacks on a worker co-pinned with its writer, which can preempt
the writer while it is queued; the kernel's BH-disabled locking rules that
out.  The rest is FIFO handoffs crossing nodes.  Every other rename panel runs
8 writers on one node and is within 5% of TTAS.

Data: scripts/dcache_sweep.csv (best-of-N, one run's numbers per row,
conservation-gated).  2x96 EPYC.
"""
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               LogLocator, NullFormatter)
import dcache_plotlib as dp

# plain decimal label: 0.2, 0.5, 1, 10, 100, 200 -- never 10^n, never "0" for 0.5
_num = FuncFormatter(lambda v, pos: f"{v:g}")


def plain_y(ax):
    """linear y from 0, plain-number labels (never 10^n)."""
    ax.set_ylim(bottom=0)
    ax.yaxis.set_major_formatter(_num)


def plain_thread_x(ax, ticks):
    """linear thread-count x with plain integer labels at the given ticks."""
    ax.set_xlim(0, max(ticks) * 1.02)
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
    ax.xaxis.set_minor_formatter(NullFormatter())


def percent_x(ax, fracs, labels):
    """linear rename-fraction x, labelled as percentages."""
    ax.set_xlim(0, max(fracs) * 1.02)
    ax.xaxis.set_major_locator(FixedLocator(fracs))
    ax.xaxis.set_major_formatter(FixedFormatter(labels))
    ax.xaxis.set_minor_formatter(NullFormatter())

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_sweep.csv")
OUT = os.environ.get("OUT", os.path.join(HERE, os.pardir, "figures", "dcache_s3.png"))

COLOR = {"seqlock": "#D55E00", "seqlock-snapshot": "#E69F00",
         "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "seqlock-snapshot": "P", "txn-global": "o",
          "txn-pernode": "^", "txn-mark": "D", "bucketlock": "X"}
LABEL = {
    "seqlock": "seqlock — kernel RCU-walk\n(hand-over-hand d_seq; faithful baseline)",
    "seqlock-snapshot": "seqlock — whole-walk rename_lock\n(snapshot, as the txn arms give)",
    "txn-global": "urcu-txn — GLOBAL rename_gen\n(d_seq deleted, one global bracket)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(localized: moved entry only)",
    "txn-mark": "urcu-txn — deletion MARK as gen\n(localized, no counter; name 40)",
    "bucketlock": "bucket lock + SW txn\n(same mark reader; fold-lock writer)",
}
# EVERY arm, by default: each reader panel's ratio strip compares every engine
# against the seqlock baseline, and a subset would decide the comparison before
# the reader sees it.  The guarantee-matched pairs to read together: seqlock
# (the kernel's lookup) against the localized designs (txn-pernode, txn-mark,
# bucketlock), which give MORE -- a whole-path snapshot -- and the snapshot
# seqlock arm against txn-global, which give the same guarantee.  txn-pernode
# and txn-mark run the same localized reader and differ only in which CL0 word
# the stamp reads, so they are expected to sit together.  Override with
# ENGINES=... to plot a subset.
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock").split())
rows = dp.load(CSV)
PER_W = 12500                       # run_dcache.sh's paced renames/s per writer


fig, ((axr, ax3), (ax2, ax1)) = plt.subplots(2, 2, figsize=(15, 14))



# ---- Panel A: reader throughput vs OFFERED rename rate (THE headline) --------
rrate = None
for e in ENGINES:
    s = dp.series(rows, "rate_target", "mlookups_s", panel="rate", engine=e)
    dp.plot_series(axr, s, COLOR[e], MARKER[e], LABEL[e])
    rrate = rrate or next((r["readers"] for r in rows if r["panel"] == "rate"),
                          None)
if axr.has_data():
    axr.set_xscale("log")
    ticks = sorted({float(r["rate_target"]) for r in rows if r["panel"] == "rate"})
    axr.xaxis.set_major_locator(FixedLocator(ticks))
    axr.xaxis.set_major_formatter(FixedFormatter([dp.rate_label(t) for t in ticks]))
    axr.xaxis.set_minor_formatter(NullFormatter())
    plain_y(axr)
axr.set_title(f"Reader throughput vs OFFERED rename rate — {rrate} readers + 8 "
              "writers\nevery engine's readers face the same load at each x; "
              "a curve goes\nhollow where its writers cannot sustain the rate",
              fontsize=9.5)
axr.set_xlabel("offered renames / s  (8 paced writers, aggregate)")
axr.set_ylabel("reader lookup Mops/s   (higher is better)")
dp.ratio_strip(axr, rows, "rate_target", "mlookups_s", ENGINES, COLOR, MARKER,
               panel="rate")

# ---- Panel B: reader scaling at a fixed PACED rename rate ---------------------
for e in ENGINES:
    dp.plot_series(ax3, dp.series(rows, "readers", "mlookups_s",
                                  panel="split_scale", engine=e),
                   COLOR[e], MARKER[e], LABEL[e], lw=1.9, ms=6)
if ax3.has_data():
    plain_thread_x(ax3, [0, 32, 64, 96, 128, 160, 184])
    plain_y(ax3)
ax3.set_title(f"Reader scaling at a fixed rename load — 8 writers paced to "
              f"{dp.rate_label(8 * PER_W)} renames/s\nsweep readers to fill the "
              "machine, one hw thread per core", fontsize=9.5)
ax3.set_xlabel("dedicated reader threads")
ax3.set_ylabel("reader lookup Mops/s   (higher is better)")
dp.ratio_strip(ax3, rows, "readers", "mlookups_s", ENGINES, COLOR, MARKER,
               panel="split_scale")

# ---- Panel C: reader path vs writer count, load growing with W ----------------
for e in ENGINES:
    dp.plot_series(ax2, dp.series(rows, "writers", "mlookups_s",
                                  panel="split_w", engine=e),
                   COLOR[e], MARKER[e], LABEL[e], lw=2.1)
if ax2.has_data():
    plain_thread_x(ax2, [0, 8, 16, 24, 32, 40, 48])
    plain_y(ax2)
ax2.set_title(f"32 readers + W writers, each writer paced to {dp.rate_label(PER_W)} "
              "renames/s\n(offered load = W × "
              f"{dp.rate_label(PER_W)}/s, the same for every engine at each W)",
              fontsize=9.5)
ax2.set_xlabel("concurrent writer threads (renaming)")
ax2.set_ylabel("reader lookup Mops/s   (higher is better)")
dp.ratio_strip(ax2, rows, "writers", "mlookups_s", ENGINES, COLOR, MARKER,
               panel="split_w")

# ---- Panel D: HOMOGENEOUS mix -- lookup throughput vs rename fraction --------
for e in ENGINES:
    dp.plot_series(ax1, dp.series(rows, "rename_frac", "mlookups_s",
                                  panel="frac", engine=e),
                   COLOR[e], MARKER[e], LABEL[e], lw=1.9, ms=6)
if ax1.has_data():
    percent_x(ax1, [0, 0.1, 0.2, 0.3, 0.4, 0.5],
              ["0", "10%", "20%", "30%", "40%", "50%"])
    plain_y(ax1)
ax1.set_title("Homogeneous mix (48 threads) — lookup Mops/s vs rename fraction\n"
              "every thread both renames and looks up, unpaced: the curve is\n"
              "WRITER-bound (a rename costs ~50 lookups) -- a mixed-workload\n"
              "throughput, not a reader-path comparison.  ⚠ 6 nodes renaming at\n"
              "once: the queued LRU shard lock (the kernel's) costs txn and\n"
              "bucket-lock renames 7-14% at 10%, 25-34% at 50% vs TTAS, part of\n"
              "it a harness convoy (dcache_qspinlock.h)", fontsize=9.5)
ax1.set_xlabel("rename fraction   (leftmost = 0)")
ax1.set_ylabel("lookup Mops/s   (higher is better)")

for ax in (axr, ax3, ax2, ax1):
    ax.grid(alpha=0.3, ls=":")
    dp.legend(ax, fontsize=7.5, loc="best")

fig.suptitle("Userspace dcache — does urcu-txn dissolve rename_lock + d_seq?   "
             "readers at MATCHED rename loads   ·   2×96-core EPYC\n"
             "strip under each reader panel: every engine ÷ the seqlock "
             "baseline at the same x (paced points only; dashed = parity)",
             fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.955])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
