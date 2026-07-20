#!/usr/bin/env python3
"""Plot scripts/dcache_churn.csv -> figures/dcache_churn.png.

Insert/remove (create/delete) throughput, which no other dcache figure covers:
the s3 and height figures permute a FIXED namespace (rename, exchange), this
one churns it with dc_add / dc_unlink.

Four arms:
  seqlock      faithful kernel-style rename_lock + per-dentry d_seq
  txn-global   urcu-txn, GLOBAL rename_gen.  Churn is bump-free on the
               corrected engine (unlink owes no bump), so global does NOT
               collapse here -- it tracks the localized arms; its collapse
               is a DIRECTORY-operation effect (see dcache_optype.png)
  txn-pernode  urcu-txn, PER-NODE host generation
  txn-mark     urcu-txn, no counter at all: the hlist deletion mark IS the
               version, so unlink bumps nothing

Panels: raw mutator scaling (writers only), the READ path under churn load, and
reader scaling at fixed churn.

NOTE these binaries are built -DDC_SPLIT_KEEPID (a re-added dentry is a new
allocation, so the harness's identity checks need logical ids).  Reader rates
are therefore comparable BETWEEN ARMS HERE, but not against the address-default
numbers in dcache_s3.png.

Env: ENGINES="..." to plot a subset, ANNOT=<engine> for the ratio labels.
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
    "seqlock": "seqlock — rename_lock + d_seq\n(faithful kernel baseline)",
    "txn-global": "urcu-txn — GLOBAL rename_gen\n(whole-tree bump per dir rename)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(localized bump per dir rename)",
    "txn-mark": "urcu-txn — deletion MARK as gen\n(no counter; the del is the signal)",
}
# The global arm is KEPT here, unlike dcache_s3.png where it is dropped for
# readability: on this workload it is the whole point.  Every unlink bumps one
# whole-tree counter -- but on churn (bump-free) it never moves, so global
# does not collapse here.  Kept plotted for the arm comparison.
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock txn-global txn-pernode txn-mark").split())
ANNOT = os.environ.get("ANNOT", "txn-mark")

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

# ---- panel 1: writers only, raw insert/remove scaling ---------------------
base = {}
for e in ENGINES:
    xs, ys = series("churn_w", e, "writers", "mchurn_s")
    if not xs:
        continue
    base[e] = dict(zip(xs, ys))
    ax1.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=6.5,
             label=LABEL[e], alpha=0.94)
for w in sorted(base.get(ANNOT, {})):
    if w in base.get("seqlock", {}) and base["seqlock"][w] > 0:
        r = base[ANNOT][w] / base["seqlock"][w]
        if r >= 1.5:
            ax1.annotate(f"{r:.1f}×", (w, base[ANNOT][w]),
                         textcoords="offset points", xytext=(0, 9),
                         ha="center", fontsize=8, color=COLOR[ANNOT],
                         fontweight="bold")
if ax1.has_data():
    plain_thread_x(ax1, [1, 2, 4, 8, 16, 32, 48])
    plain_y(ax1)
    lo, hi = ax1.get_ylim(); ax1.set_ylim(lo, hi * 1.35)
ax1.set_title("Writers only — insert/remove Mops/s vs writer count\n"
              "the pure MUTATOR path.  Bump-free churn, decontended ndirs +\n"
              "jemalloc: the three txn arms converge and scale; seqlock is\n"
              "serialized by its mutator lock (× = mark ÷ seqlock)",
              fontsize=9.5)
ax1.set_xlabel("churn writer threads")
ax1.set_ylabel("insert+remove Mops/s   (higher is better)")
ax1.grid(alpha=0.3, ls=":")
ax1.legend(fontsize=8, loc="best")

# ---- panel 2: the read path under churn load ------------------------------
base = {}
for e in ENGINES:
    xs, ys = series("churn_rd", e, "writers", "mlookups_s")
    if not xs:
        continue
    base[e] = dict(zip(xs, ys))
    ax2.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=6.5,
             label=LABEL[e], alpha=0.94)
for w in sorted(base.get(ANNOT, {})):
    if w in base.get("seqlock", {}) and base["seqlock"][w] > 0:
        r = base[ANNOT][w] / base["seqlock"][w]
        if r >= 1.3:
            ax2.annotate(f"{r:.1f}×", (w, base[ANNOT][w]),
                         textcoords="offset points", xytext=(0, 9),
                         ha="center", fontsize=8, color=COLOR[ANNOT],
                         fontweight="bold")
if ax2.has_data():
    plain_thread_x(ax2, [1, 2, 4, 8, 16, 32, 48])
    plain_y(ax2)
    lo, hi = ax2.get_ylim(); ax2.set_ylim(lo, hi * 1.35)
ax2.set_title("32 dedicated readers + W churn writers — reader Mops/s vs W\n"
              "the read path under create/delete load.  Churn is BUMP-FREE\n"
              "(add never bumped; unlink no longer does), so no reader retries\n"
              "on churn: the three txn arms track together, seqlock trails\n"
              "(× = mark ÷ seqlock)", fontsize=9.5)
ax2.set_xlabel("churn writer threads")
ax2.set_ylabel("reader lookup Mops/s   (higher is better)")
ax2.grid(alpha=0.3, ls=":")
ax2.legend(fontsize=8, loc="best")

# ---- panel 3: reader scaling at fixed churn -------------------------------
for e in ENGINES:
    xs, ys = series("churn_scale", e, "readers", "mlookups_s")
    if not xs:
        continue
    ax3.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=6.5,
             label=LABEL[e], alpha=0.94)
if ax3.has_data():
    # thin the ticks: the sampled points crowd at the top end (128/160/184)
    plain_thread_x(ax3, [2, 4, 8, 16, 32, 64, 128, 184])
    plain_y(ax3)
ax3.set_title("Reader scaling under constant churn — 8 writers fixed\n"
              "reader Mops/s vs reader count, one hw thread per core.  Churn\n"
              "bump-free, so all three txn arms scale together (global edges\n"
              "them: no per-hop second pass); only seqlock lags",
              fontsize=9.5)
ax3.set_xlabel("dedicated reader threads")
ax3.set_ylabel("reader lookup Mops/s   (higher is better)")
ax3.grid(alpha=0.3, ls=":")
ax3.legend(fontsize=8, loc="best")

fig.suptitle("Userspace dcache — INSERT/REMOVE (dc_add / dc_unlink) under "
             "concurrent lookups   ·   2×96-core EPYC", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
