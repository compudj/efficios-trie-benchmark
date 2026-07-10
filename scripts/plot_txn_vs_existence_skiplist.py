#!/usr/bin/env python3
"""
urcu-txn (rcu-mcas) vs. McKenney "existence" — 3-SKIPLIST atomic key move,
2x96-core EPYC.  The ORDERED dual of plot_txn_vs_existence_scale.py (3-hash).

⚠ SUPERSEDES the first version of this figure, whose data was invalid — see
scripts/run_txn_vs_existence_skiplist.sh for the three defects (unseeded
random_level() making existence's skiplist a sorted linked list; only 6 of 15
keys rotated; mismatched commit widths) and the growing-structure confound.

Reads scripts/txn_vs_existence_skiplist.csv (best-of-N), writes
figures/txn_vs_existence_skiplist.png.  Four panels, because the old single
panel could not distinguish scaling from structure growth:

  1. GROWING PROBLEM   keys/skiplist = 5*cores.  What the bench naturally does.
                       Both engines' aggregate throughput vs cores.  This is the
                       panel the old figure showed, now labelled for what it is.
  2. FIXED PROBLEM     keys/skiplist = 3840 at every core count.  ns per key-move,
                       so PERFECT SCALING IS A FLAT LINE and every rise is writer
                       interference.  glibc drawn dashed as an allocator control.
  3. SIZE DEPENDENCE   192 cores, sweep keys/skiplist.  The headline correction:
                       the gap is a strong function of structure size, because at
                       960 keys a level-7 predecessor slot is shared by ~26 cores.
  4. READ SCALING      N readers + 1 updater on 3840 keys/skiplist.  Both engines
                       scan all three skiplists for one key.  txn reads 2.8-3.2x
                       faster; the MECHANISM IS NOT ESTABLISHED (candidates: the
                       per-hit existence_exists() check, the keyvalue indirection,
                       perfbook's per-hop indirect comparator).  Note existence is
                       built -O3 and txn -O2, an asymmetry that FAVOURS existence,
                       so its deficit is conservative.  (The old read panel ran on
                       a 5-KEY skiplist -- 1 updater at the default b=5 -- where
                       no traversal cost can show up at all.  Its "reads are a
                       wash" conclusion was an artifact of structure size.)

Commit width is matched at 3 key-moves — existence's narrowest atomic unit
(one turn of the rotation's 3-cycle) versus txn --movesper 3.  txn-mp1 is the
narrowest txn unit and has no existence peer, so it is drawn thin/dashed.

Neither engine's commit is a transaction in the strict sense: txn makes its
write set visible in one step and validates only the slots it stores or folds
in explicitly; existence flips a group word.  Both are atomic multi-slot
updates, not serializable transactions.

Run: python3 scripts/plot_txn_vs_existence_skiplist.py
"""
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "txn_vs_existence_skiplist.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "txn_vs_existence_skiplist.png")

# Okabe-Ito, matching the tree's other figures: existence orange, txn blues.
COLOR = {"existence": "#D55E00", "txn-mp3": "#0072B2", "txn-mp1": "#56B4E9"}
MARKER = {"existence": "s", "txn-mp3": "o", "txn-mp1": "^"}
LABEL = {
    "existence": "existence (flip, 3-key group)",
    "txn-mp3": "urcu-txn, 3 moves/commit  (width-matched)",
    "txn-mp1": "urcu-txn, 1 move/commit  (no existence peer)",
}

rows = list(csv.DictReader(open(CSV)))


def best(panel, engine, xcol, ycol, alloc="jemalloc"):
    """best-of-N (max) y per x, dropping failed rows (y == 0)."""
    acc = defaultdict(list)
    for r in rows:
        if r["panel"] != panel or r["engine"] != engine or r["alloc"] != alloc:
            continue
        y = float(r[ycol])
        if y > 0:
            acc[int(r[xcol])].append(y)
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs]


def lowest(panel, engine, xcol, ycol, alloc="jemalloc"):
    """best-of-N (min) for a lower-is-better metric such as ns/key-move."""
    acc = defaultdict(list)
    for r in rows:
        if r["panel"] != panel or r["engine"] != engine or r["alloc"] != alloc:
            continue
        y = float(r[ycol])
        if y > 0:
            acc[int(r[xcol])].append(y)
    xs = sorted(acc)
    return xs, [min(acc[x]) for x in xs]


def style(engine):
    lw = 1.8 if engine != "txn-mp1" else 1.2
    ls = "-" if engine != "txn-mp1" else "--"
    return dict(color=COLOR[engine], marker=MARKER[engine], lw=lw, ls=ls,
                ms=5, alpha=0.9)


fig, axes = plt.subplots(2, 2, figsize=(14.5, 9.5))
ax1, ax2, ax3, ax4 = axes.flat

# ---- 1. growing problem -----------------------------------------------------
for e in ("existence", "txn-mp3", "txn-mp1"):
    xs, ys = best("grow", e, "cores", "mmoves_s")
    if xs:
        ax1.plot(xs, ys, label=LABEL[e], **style(e))
ax1.set_title("1. Growing problem — keys/skiplist = 5×cores\n"
              "(what the benchmark naturally does; NOT a scaling curve)",
              fontsize=10)
ax1.set_xlabel("updater cores      (structure grows 192× across this axis)")
ax1.set_ylabel("aggregate key-moves/s (millions)")

# ---- 2. fixed problem -------------------------------------------------------
for e in ("existence", "txn-mp3", "txn-mp1"):
    xs, ys = lowest("fixed", e, "cores", "ns_per_keymove")
    if xs:
        ax2.plot(xs, ys, label=LABEL[e], **style(e))
    xs, ys = lowest("fixed", e, "cores", "ns_per_keymove", alloc="glibc")
    if xs:
        s = style(e); s.update(lw=0.9, ls=":", ms=3, alpha=0.55)
        ax2.plot(xs, ys, **s)
ax2.set_yscale("log")
ax2.set_title("2. Fixed problem — keys/skiplist = 3840 at every core count\n"
              "perfect scaling is a FLAT line; any rise is writer interference\n"
              "(dotted = glibc malloc control)", fontsize=10)
ax2.set_xlabel("updater cores")
ax2.set_ylabel("ns per key-move  (lower is better, log)")

# ---- 3. size dependence -----------------------------------------------------
for e in ("existence", "txn-mp3", "txn-mp1"):
    xs, ys = best("size", e, "keys_per_sl", "mmoves_s")
    if xs:
        ax3.plot(xs, ys, label=LABEL[e], **style(e))
ex = dict(zip(*best("size", "existence", "keys_per_sl", "mmoves_s")))
tx = dict(zip(*best("size", "txn-mp3", "keys_per_sl", "mmoves_s")))
for x in sorted(set(ex) & set(tx)):
    ax3.annotate(f"{ex[x]/tx[x]:.2f}×", (x, ex[x]), textcoords="offset points",
                 xytext=(0, 7), ha="center", fontsize=8, color="#555555")
ax3.set_xscale("log")
ax3.set_title("3. Size dependence — 192 cores, sweep keys/skiplist\n"
              "annotations: existence ÷ width-matched txn", fontsize=10)
ax3.set_xlabel("keys per skiplist  (log)")
ax3.set_ylabel("aggregate key-moves/s (millions)")

# ---- 4. read scaling --------------------------------------------------------
for e in ("existence", "txn-mp3"):
    xs, ys = best("read", e, "nreaders", "mqueries_s")
    if xs:
        ax4.plot(xs, ys, label=LABEL[e], **style(e))
ax4.set_title("4. Read scaling — N readers + 1 updater, 3840 keys/skiplist\n"
              "both scan all 3 skiplists for one key; mechanism of the gap unattributed\n"
              "(existence is built -O3, txn -O2, so its deficit is conservative)",
              fontsize=9)
ax4.set_xlabel("reader cores")
ax4.set_ylabel("aggregate queries/s (millions)")

for ax in (ax1, ax2, ax3, ax4):
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8, loc="best")
    if ax is not ax2:
        ax.set_ylim(bottom=0)
    ax.ticklabel_format(axis="y", style="plain") if ax.get_yscale() == "linear" else None

fig.suptitle("urcu-txn (MCAS) vs. existence (flip) — three-skiplist atomic key move, "
             "2×96-core EPYC   [supersedes the pre-seeding-fix figure]", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.95])
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=130)
print("wrote", OUT)
