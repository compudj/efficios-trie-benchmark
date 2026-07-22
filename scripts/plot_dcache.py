#!/usr/bin/env python3
"""
S3 -- userspace dentry-cache: can urcu-txn dissolve rename_lock + d_seq?

Three arms:
  seqlock      faithful kernel-style GLOBAL rename_lock + per-dentry d_seq
  txn-global   urcu-txn port, GLOBAL rename_gen walk bracket (deletes d_seq,
               but the reader still brackets one whole-tree counter)
  txn-pernode  urcu-txn port, PER-NODE host generation -- a rename bumps only
               the moved entry's own host counter, so a walk down a disjoint
               path re-reads a disjoint set of counters (no shared cacheline)

Data: scripts/dcache_sweep.csv (best-of-N, conservation-gated).  2x96 EPYC.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               LogLocator, NullFormatter)

# plain decimal label: 0.2, 0.5, 1, 10, 100, 200 -- never 10^n, never "0" for 0.5
_num = FuncFormatter(lambda v, pos: f"{v:g}")


def plain_y(ax):
    """log-SPACED y (the data spans decades) but PLAIN-NUMBER labels: 1, 10, 100
    (with 2/5 subdivisions), never 10^n."""
    ax.set_yscale("log")
    ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1, 2, 5)))
    ax.yaxis.set_major_formatter(_num)
    ax.yaxis.set_minor_formatter(NullFormatter())


def plain_thread_x(ax, ticks):
    """log2-spaced thread-count x with plain integer labels at the sampled points."""
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
    ax.xaxis.set_minor_formatter(NullFormatter())


def percent_x(ax, fracs, labels):
    """log-spaced rename-fraction x, labelled as percentages (0, 1%, 5%, ...)."""
    ax.set_xscale("log")
    ax.xaxis.set_major_locator(FixedLocator(fracs))
    ax.xaxis.set_major_formatter(FixedFormatter(labels))
    ax.xaxis.set_minor_formatter(NullFormatter())

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_sweep.csv")
OUT = os.environ.get("OUT", os.path.join(HERE, os.pardir, "figures", "dcache_s3.png"))

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D",
          "bucketlock": "X"}
LABEL = {
    "seqlock": "seqlock — rename_lock + d_seq\n(faithful kernel baseline)",
    "txn-global": "urcu-txn — GLOBAL rename_gen\n(d_seq deleted, one global bracket)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(localized: moved entry only)",
    "txn-mark": "urcu-txn — deletion MARK as gen\n(localized, no counter; name 40)",
    "bucketlock": "bucket lock + SW txn\n(same mark reader; fold-lock writer)",
}
# Headline comparison: the faithful kernel baseline vs the localized designs.
# The txn-global arm (an intermediate A/B showing a naive port that keeps one
# global counter does NOT scale) stays in the CSV + the §7 prose, but is dropped
# from the plotted lines to keep the figure readable.
#
# txn-mark plots ON TOP of txn-pernode and is expected to sit on it: both run the
# same localized reader and differ only in which CL0 word the stamp reads.  The
# figure's job here is to show they coincide -- the mark arm's gain is the
# retired counter and the 8 name bytes, not throughput.  Override with
# ENGINES=... to plot a different subset.
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock txn-mark bucketlock").split())
rows = list(csv.DictReader(open(CSV)))


def series(panel, eng, xcol, ycol):
    acc = {}
    for r in rows:
        if r["panel"] == panel and r["engine"] == eng and r["conserved"] == "OK":
            y = float(r[ycol])
            if y > 0:
                acc[float(r[xcol])] = y
    xs = sorted(acc)
    return xs, [acc[x] for x in xs]


fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(19, 6))

# ---- Panel 1: HOMOGENEOUS mix -- lookup throughput vs rename fraction --------
for e in ENGINES:
    xs, ys = series("frac", e, "rename_frac", "mlookups_s")
    if not xs:
        continue
    xs = [max(x, 0.003) for x in xs]           # 0 -> just inside the log axis
    ax1.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=1.9, ms=6,
             label=LABEL[e], alpha=0.92)
if ax1.has_data():
    percent_x(ax1, [0.003, 0.01, 0.05, 0.1, 0.2, 0.5],
              ["0", "1%", "5%", "10%", "20%", "50%"])
    plain_y(ax1)
ax1.set_title("Homogeneous mix (48 threads) — lookup Mops/s vs rename fraction\n"
              "collapse is WRITER-bound (a rename ≈ 50× a lookup, so a small\n"
              "fraction eats a large time-share) — masks the reader-gen gap",
              fontsize=9.5)
ax1.set_xlabel("rename fraction   (leftmost = 0)")
ax1.set_ylabel("lookup Mops/s   (higher is better)")

# ---- Panel 2: ROLE-SPLIT -- reader throughput vs writer load (THE headline) --
base = {}
for e in ENGINES:
    xs, ys = series("split_w", e, "writers", "mlookups_s")
    base[e] = dict(zip(xs, ys))
    if not xs:
        continue
    ax2.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=6.5,
             label=LABEL[e], alpha=0.94)
# Annotate the lead of the SURVIVING design over the seqlock baseline.  ANNOT
# tracks whichever arm is the one being argued for -- txn-mark since the
# per-node counter arm was superseded -- so the ratios label the line a reader
# is meant to take away.
ANNOT = os.environ.get("ANNOT", "txn-mark")
for w in sorted(base.get(ANNOT, {})):
    if w in base["seqlock"] and base["seqlock"][w] > 0:
        r = base[ANNOT][w] / base["seqlock"][w]
        if r >= 1.15:
            ax2.annotate(f"{r:.1f}×", (w, base[ANNOT][w]),
                         textcoords="offset points", xytext=(0, 8),
                         ha="center", fontsize=8, color=COLOR[ANNOT],
                         fontweight="bold")
if ax2.has_data():
    plain_thread_x(ax2, [1, 2, 4, 8, 16, 24, 32, 48])
    plain_y(ax2)
    # headroom so the leftmost ratio label is not clipped by the axes top
    lo, hi = ax2.get_ylim()
    ax2.set_ylim(lo, hi * 1.35)
ax2.set_title("Role-split: 32 dedicated readers + W writers — reader Mops/s vs W\n"
              "ISOLATES the reader path: seqlock retries the whole walk on any\n"
              "rename; the per-node counter is touched only by walks through the\n"
              "moved entry (× = mark ÷ seqlock)",
              fontsize=9.5)
ax2.set_xlabel("concurrent writer threads (renaming)")
ax2.set_ylabel("reader lookup Mops/s   (higher is better)")

# ---- Panel 3: ROLE-SPLIT reader scaling at fixed writer load ----------------
for e in ENGINES:
    xs, ys = series("split_scale", e, "readers", "mlookups_s")
    if not xs:
        continue
    ax3.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=1.9, ms=6,
             label=LABEL[e], alpha=0.92)
if ax3.has_data():
    plain_thread_x(ax3, [2, 8, 16, 32, 64, 96, 128, 184])
    plain_y(ax3)
ax3.set_title("Role-split reader scaling — 8 writers fixed, sweep readers to 184\n"
              "reader Mops/s vs reader count under a constant rename load, one hw\n"
              "thread per core (per-node keeps scaling; seqlock never scales)",
              fontsize=9.5)
ax3.set_xlabel("dedicated reader threads")
ax3.set_ylabel("reader lookup Mops/s   (higher is better)")

for ax in (ax1, ax2, ax3):
    ax.grid(alpha=0.3, ls=":")
    if ax.get_legend_handles_labels()[0]:
        ax.legend(fontsize=7.5, loc="best")

fig.suptitle("Userspace dcache — does urcu-txn dissolve rename_lock + d_seq?   "
             "seqlock (kernel baseline) vs two urcu-txn engines: all-MW deletion-mark "
             "and the bucket-lock + SW txn (fold-lock writer)   ·   2×96-core EPYC",
             fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
