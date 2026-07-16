#!/usr/bin/env python3
"""
Directory listing (readdir) under a concurrent rename load -- the S3 companion.

Headline two-way (the lookup figure's companion): the faithful kernel baseline
vs the winning design, readers ENUMERATE a directory (dc_readdir) rather than
doing a full-path leaf lookup:

  seqlock      per-DIRECTORY rwsem readdir (the honest kernel-inode-rwsem
               analogue: readers of a dir share a read-lock, a rename write-locks
               only its affected parents -- NOT one global lock)
  txn-pernode  lock-free RCU walk of the MCAS child hlist, per-node host gen

Note: readdir's READER path reads NO generation counter at all (a concurrently
renamed child may or may not appear -- POSIX-soft, but it never tears), so
txn-pernode and txn-global run IDENTICAL readdir reader code.  They still differ
by ~15-25% at scale -- a second-order WRITER-side effect (the global rename_gen
cacheline the concurrent writers contend), documented in §7.2 -- but txn-global
is dropped from this plot to keep the headline a clean baseline-vs-winner; it
stays in the CSV.  The contrast that matters here is the lock-free RCU walk vs the
per-directory lock.

Data: scripts/dcache_sweep.csv (panels readdir_scale, readdir_w).  2x96 EPYC.
"""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               LogLocator, NullFormatter)

_num = FuncFormatter(lambda v, pos: f"{v:g}")


def plain_y(ax):
    ax.set_yscale("log")
    ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1, 2, 5)))
    ax.yaxis.set_major_formatter(_num)
    ax.yaxis.set_minor_formatter(NullFormatter())


def plain_thread_x(ax, ticks):
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
    ax.xaxis.set_minor_formatter(NullFormatter())


HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_sweep.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_readdir.png"))

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2", "txn-pernode": "#009E73"}
MARKER = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^"}
LABEL = {
    "seqlock": "seqlock — per-directory rwsem\n(kernel inode-rwsem analogue)",
    "txn-global": "urcu-txn — GLOBAL rename_gen\n(lock-free RCU child-walk)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(lock-free RCU child-walk)",
}
ENGINES = ("seqlock", "txn-pernode")
rows = list(csv.DictReader(open(CSV)))


def series(panel, eng, xcol):
    acc = {}
    for r in rows:
        if r["panel"] == panel and r["engine"] == eng and r["conserved"] == "OK":
            y = float(r["mlookups_s"])          # readdir CALL rate in readdir mode
            if y > 0:
                acc[float(r[xcol])] = y
    xs = sorted(acc)
    return xs, [acc[x] for x in xs]


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.5, 6))

# ---- Panel 1: reader scaling at fixed writer load ---------------------------
base = {}
for e in ENGINES:
    xs, ys = series("readdir_scale", e, "readers")
    base[e] = dict(zip(xs, ys))
    if not xs:
        continue
    ax1.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.0, ms=6.5,
             label=LABEL[e], alpha=0.94)
# per-node lead over the seqlock rwsem baseline where both exist
for rd in sorted(base.get("txn-pernode", {})):
    if rd in base.get("seqlock", {}) and base["seqlock"][rd] > 0:
        r = base["txn-pernode"][rd] / base["seqlock"][rd]
        if r >= 1.5:
            ax1.annotate(f"{r:.1f}×", (rd, base["txn-pernode"][rd]),
                         textcoords="offset points", xytext=(0, 8),
                         ha="center", fontsize=8, color=COLOR["txn-pernode"],
                         fontweight="bold")
if ax1.has_data():
    plain_thread_x(ax1, [2, 8, 16, 32, 64, 96, 128, 184])
    plain_y(ax1)
ax1.set_title("readdir reader scaling — 8 writers fixed, sweep readers to 184\n"
              "listings/s vs reader count, fixed dir size (~32 children), one hw\n"
              "thread/core (the rwsem read-side is a shared cacheline → saturates;\n"
              "the RCU walk has no shared read-side state → scales)", fontsize=9.5)
ax1.set_xlabel("dedicated reader threads (each listing a random dir)")
ax1.set_ylabel("directory listings / s   (Mreaddir/s, higher is better)")

# ---- Panel 2: reader throughput vs writer (rename) load ---------------------
for e in ENGINES:
    xs, ys = series("readdir_w", e, "writers")
    if not xs:
        continue
    ax2.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.0, ms=6.5,
             label=LABEL[e], alpha=0.94)
if ax2.has_data():
    plain_thread_x(ax2, [1, 2, 4, 8, 16, 24, 32, 48])
    plain_y(ax2)
ax2.set_title("readdir vs writer load — 32 readers fixed, sweep writers\n"
              "listings/s vs concurrent renamers, namespace held constant\n"
              "(each rename write-locks its dirs → the seqlock readdir is excluded;\n"
              "the RCU walk never blocks on a writer)", fontsize=9.5)
ax2.set_xlabel("concurrent writer threads (renaming)")
ax2.set_ylabel("directory listings / s   (Mreaddir/s, higher is better)")

for ax in (ax1, ax2):
    ax.grid(alpha=0.3, ls=":")
    if ax.get_legend_handles_labels()[0]:
        ax.legend(fontsize=7.5, loc="best")

fig.suptitle("Userspace dcache — directory listing (readdir) under concurrent "
             "renames   ·   lock-free RCU child-walk vs per-directory rwsem   ·   "
             "2×96-core EPYC", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
