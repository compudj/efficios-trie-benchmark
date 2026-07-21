#!/usr/bin/env python3
"""Plot scripts/dcache_churn.csv -> figures/dcache_churn.png.

Insert/remove (create/delete) throughput, which no other dcache figure covers:
the s3 and height figures permute a FIXED namespace (rename, exchange); this one
churns it with dc_add / dc_unlink.

The seqlock baseline's write path is now kernel-faithful and FINE-GRAINED: a
per-bucket hlist_bl bit lock (bit 0 of the bucket head word) + the per-directory
rwsem, exactly the kernel's add/unlink locking -- NOT one global mutator lock.
So its add/unlink scale, and this figure is an honest mechanism comparison, not a
serialization artifact.

Four arms: seqlock (faithful kernel baseline) and the three urcu-txn arms
(GLOBAL rename_gen / PER-NODE host gen / deletion MARK).  Churn is BUMP-FREE
(add never bumped; unlink owes no bump on the corrected engine), so the three
txn arms coincide here.

The three panels tell a SPLIT story -- a genuine tradeoff, not one engine
dominating:
  writers  the pure WRITE path.  The bit-lock BASELINE LEADS ~1.4-2x: a bit-lock
           + hlist splice is lighter than txn's per-op MCAS (two commits + a
           descriptor per churn op).
  churn_rd the READ path under create/delete load.  Here TXN LEADS ~1.3-1.6x:
           its reader does an inline name compare with no sequence counter, while
           the seqlock reader validates d_seq + rename_lock and is disturbed more
           by concurrent churn.
  churn_scale reader scaling at fixed churn -- same read-path story, TXN ahead.
So the txn engine earns its keep on the reader path; the faithful bit-lock wins
the writer path.

NOTE these binaries are built -DDC_SPLIT_KEEPID (a re-added dentry is a new
allocation, so the harness's identity checks need logical ids).  Reader rates are
comparable BETWEEN ARMS HERE, but not against the address-default numbers in
dcache_s3.png.

Env: ENGINES="..." to plot a subset.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               LogLocator, NullFormatter)

_num = FuncFormatter(lambda v, _: ("%g" % v))


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
CSV = os.path.join(HERE, "dcache_churn.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_churn.png"))

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2",
         "txn-pernode": "#009E73", "txn-mark": "#CC79A7"}
MARKER = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^", "txn-mark": "D"}
LABEL = {
    "seqlock": "seqlock — hlist_bl + d_seq\n(faithful kernel baseline)",
    "txn-global": "urcu-txn — GLOBAL rename_gen",
    "txn-pernode": "urcu-txn — PER-NODE host gen",
    "txn-mark": "urcu-txn — deletion MARK as gen",
}
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock txn-global txn-pernode txn-mark").split())
TXN_REF = "txn-mark"        # representative txn arm for ratio labels

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


def annotate_ratio(ax, base, num, den, thresh, color):
    """Label num/den at each x where the ratio clears thresh."""
    for x in sorted(base.get(num, {})):
        if x in base.get(den, {}) and base[den][x] > 0:
            r = base[num][x] / base[den][x]
            if r >= thresh:
                ax.annotate(f"{r:.1f}×", (x, base[num][x]),
                            textcoords="offset points", xytext=(0, 9),
                            ha="center", fontsize=8, color=color,
                            fontweight="bold")


fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(19, 6))

# ---- panel 1: writers only, raw insert/remove scaling ---------------------
# The WRITE path: the bit-lock baseline leads; label seqlock / txn.
base = {}
for e in ENGINES:
    xs, ys = series("churn_w", e, "writers", "mchurn_s")
    if not xs:
        continue
    base[e] = dict(zip(xs, ys))
    ax1.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=6.5,
             label=LABEL[e], alpha=0.94)
annotate_ratio(ax1, base, "seqlock", TXN_REF, 1.3, COLOR["seqlock"])
if ax1.has_data():
    plain_thread_x(ax1, [1, 2, 4, 8, 16, 32, 48])
    plain_y(ax1)
    lo, hi = ax1.get_ylim(); ax1.set_ylim(lo, hi * 1.35)
ax1.set_title("Writers only — insert/remove Mops/s vs writer count\n"
              "the pure WRITE path.  Bump-free churn, decontended ndirs +\n"
              "jemalloc: the faithful bit-lock BASELINE LEADS — a bit-lock +\n"
              "hlist splice beats txn's per-op MCAS (× = seqlock ÷ txn)",
              fontsize=9.5)
ax1.set_xlabel("churn writer threads")
ax1.set_ylabel("insert+remove Mops/s   (higher is better)")
ax1.grid(alpha=0.3, ls=":")
ax1.legend(fontsize=8, loc="best")

# ---- panel 2: the read path under churn load ------------------------------
# The READ path: txn leads; label txn / seqlock.
base = {}
for e in ENGINES:
    xs, ys = series("churn_rd", e, "writers", "mlookups_s")
    if not xs:
        continue
    base[e] = dict(zip(xs, ys))
    ax2.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=6.5,
             label=LABEL[e], alpha=0.94)
annotate_ratio(ax2, base, TXN_REF, "seqlock", 1.3, COLOR[TXN_REF])
if ax2.has_data():
    plain_thread_x(ax2, [1, 2, 4, 8, 16, 32, 48])
    plain_y(ax2)
    lo, hi = ax2.get_ylim(); ax2.set_ylim(lo, hi * 1.35)
ax2.set_title("32 dedicated readers + W churn writers — reader Mops/s vs W\n"
              "the READ path under create/delete load.  Now TXN LEADS: its\n"
              "reader does an inline compare with no sequence counter, while\n"
              "seqlock validates d_seq + rename_lock (× = txn ÷ seqlock)",
              fontsize=9.5)
ax2.set_xlabel("churn writer threads")
ax2.set_ylabel("reader lookup Mops/s   (higher is better)")
ax2.grid(alpha=0.3, ls=":")
ax2.legend(fontsize=8, loc="best")

# ---- panel 3: reader scaling at fixed churn -------------------------------
base = {}
for e in ENGINES:
    xs, ys = series("churn_scale", e, "readers", "mlookups_s")
    if not xs:
        continue
    base[e] = dict(zip(xs, ys))
    ax3.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=6.5,
             label=LABEL[e], alpha=0.94)
if ax3.has_data():
    # thin the ticks: the sampled points crowd at the top end (128/160/184)
    plain_thread_x(ax3, [2, 4, 8, 16, 32, 64, 128, 184])
    plain_y(ax3)
ax3.set_title("Reader scaling under constant churn — 8 writers fixed\n"
              "reader Mops/s vs reader count, one hw thread per core.  Same\n"
              "read-path story: TXN scales ahead (inline compare, no counter);\n"
              "global edges pernode/mark — no per-hop second pass",
              fontsize=9.5)
ax3.set_xlabel("dedicated reader threads")
ax3.set_ylabel("reader lookup Mops/s   (higher is better)")
ax3.grid(alpha=0.3, ls=":")
ax3.legend(fontsize=8, loc="best")

fig.suptitle("Userspace dcache — INSERT/REMOVE (dc_add / dc_unlink) under "
             "concurrent lookups: bit-lock wins writes, txn wins reads   ·   "
             "2×96-core EPYC", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
