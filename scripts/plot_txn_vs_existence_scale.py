#!/usr/bin/env python3
"""
urcu-txn (rcu-mcas) vs. McKenney "existence" — 3-HASH atomic key move,
2x96-core EPYC.  The UNORDERED dual of plot_txn_vs_existence_skiplist.py.

⚠ SUPERSEDES the first version of this figure, whose data was UNFAIR.
existence_3hash_uperf's hash_rotate() moved 6 of its 15 resident keys per
rotation (loop bound `i < nobjects`, stride 3, over 3*nobjects arrays) while
bench_txn_3hash moved all 15 — so per-key-move normalization amortized
existence's group alloc + flip + call_rcu over 6 moves against txn's 15, and
the engines ran at different atomic widths.  Fixing it makes existence
1.24-1.37x faster and inverts the old headline; "urcu-txn wins on the hash
from 2 cores up" was existence doing 2.5x less mutation work.  See
scripts/run_txn_vs_existence_scale.sh.

Reads scripts/txn_vs_existence_scale.csv (best-of-N), writes
figures/txn_vs_existence_scale.png.  Four panels:

  1. GROWING PROBLEM   keys/table = 5*cores, nbuckets 4096.  The published
                       configuration, now width-matched by construction
                       (existence's whole-rotation flip IS 15 key-moves after
                       the fix, as is txn --movesper 0).  NOT a scaling curve:
                       the structure grows 192x along the x-axis.
  2. FIXED PROBLEM     keys/table = 960 at every core count, so the load factor
                       is constant too; width matched at 3 key-moves.  ns per
                       key-move, so PERFECT SCALING IS A FLAT LINE.  This is the
                       honest curve, and it shows a TIE.
  3. SIZE DEPENDENCE   192 cores, sweep keys/table with nbuckets scaled to hold
                       the load factor at 0.25 — isolating structure size from
                       chain length (and removing the bucket aliasing that
                       nbuckets 4096 induces for updaters 128..191).
  4. READ SCALING      N readers + 1 updater.

The result to read off panel 2: at equal structure and equal width the two
engines are within ~20% of each other at every core count, with no trend, and
urcu-txn's 1->192 contention penalty is slightly SMALLER than existence's.

Set that against the ORDERED structure (plot_txn_vs_existence_skiplist.py,
`fixed` panel), where the same control puts txn ~1.4-1.85x behind existence
from 1 core to 192 (with the expect_conflict knob that skips the doomed age-0
attempt on the self-aliasing batched descent; ~2.2-2.3x without it).  That is
the edges-per-mutation result: an hlist key-move transacts 3-5 pointers and the
count does not grow with n; a skiplist key-move transacts 8.70, growing as
O(log n) because delete costs two records per level.
MCAS coordinates per transacted edge, at four atomic RMWs each; existence
coordinates per node and writes its pointers with plain stores outside the
atomic step, because it has an invisibility state to build the destination into.

Neither engine's commit is a transaction in the strict sense: txn makes its
write set visible in one step and validates only the slots it stores or folds
in explicitly; existence flips a group word.  Both are atomic multi-slot
updates, not serializable transactions.

Run: python3 scripts/plot_txn_vs_existence_scale.py
"""
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "txn_vs_existence_scale.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "txn_vs_existence_scale.png")

# Okabe-Ito, matching the tree's other figures: txn = blue, existence = orange.
COLOR = {"existence": "#D55E00", "txn": "#0072B2"}
MARKER = {"existence": "s", "txn": "o"}
LABEL = {"existence": "existence (flip)", "txn": "urcu-txn (MCAS)"}

rows = list(csv.DictReader(open(CSV)))


def pick(panel, engine, xcol, ycol, alloc="jemalloc", lower_is_better=False):
    acc = defaultdict(list)
    for r in rows:
        if r["panel"] != panel or r["engine"] != engine or r["alloc"] != alloc:
            continue
        y = float(r[ycol])
        if y > 0:
            acc[int(r[xcol])].append(y)
    xs = sorted(acc)
    f = min if lower_is_better else max
    return xs, [f(acc[x]) for x in xs]


def style(engine):
    return dict(color=COLOR[engine], marker=MARKER[engine], lw=1.8, ms=5, alpha=0.9)


fig, axes = plt.subplots(2, 2, figsize=(14.5, 9.5))
ax1, ax2, ax3, ax4 = axes.flat

# ---- 1. growing problem -----------------------------------------------------
for e in ("existence", "txn"):
    xs, ys = pick("grow", e, "cores", "mmoves_s")
    ax1.plot(xs, ys, label=LABEL[e], **style(e))
ax1.set_title("1. Growing problem — keys/table = 5×cores, nbuckets 4096\n"
              "(the published configuration; NOT a scaling curve)", fontsize=10)
ax1.set_xlabel("updater cores      (structure grows 192× across this axis)")
ax1.set_ylabel("aggregate key-moves/s (millions)")

# ---- 2. fixed problem -------------------------------------------------------
for e in ("existence", "txn"):
    xs, ys = pick("fixed", e, "cores", "ns_per_keymove", lower_is_better=True)
    ax2.plot(xs, ys, label=LABEL[e], **style(e))
    xs, ys = pick("fixed", e, "cores", "ns_per_keymove", alloc="glibc",
                  lower_is_better=True)
    if xs:
        s = style(e); s.update(lw=0.9, ls=":", ms=3, alpha=0.55)
        ax2.plot(xs, ys, **s)
ax2.set_yscale("log")
ax2.set_title("2. Fixed problem — keys/table = 960 at every core count, width 3\n"
              "perfect scaling is a FLAT line; the two engines TIE\n"
              "(dotted = glibc malloc control)", fontsize=10)
ax2.set_xlabel("updater cores")
ax2.set_ylabel("ns per key-move  (lower is better, log)")

# ---- 3. size dependence -----------------------------------------------------
for e in ("existence", "txn"):
    xs, ys = pick("size", e, "keys_per_table", "mmoves_s")
    ax3.plot(xs, ys, label=LABEL[e], **style(e))
ex = dict(zip(*pick("size", "existence", "keys_per_table", "mmoves_s")))
tx = dict(zip(*pick("size", "txn", "keys_per_table", "mmoves_s")))
for x in sorted(set(ex) & set(tx)):
    ax3.annotate(f"{tx[x]/ex[x]:.2f}×", (x, max(ex[x], tx[x])),
                 textcoords="offset points", xytext=(0, 7), ha="center",
                 fontsize=8, color="#555555")
ax3.set_xscale("log")
ax3.set_title("3. Size dependence — 192 cores, load factor held at 0.25\n"
              "annotations: txn ÷ existence", fontsize=10)
ax3.set_xlabel("keys per table  (log; nbuckets scaled 4×)")
ax3.set_ylabel("aggregate key-moves/s (millions)")

# ---- 4. read scaling --------------------------------------------------------
for e in ("existence", "txn"):
    xs, ys = pick("read", e, "nreaders", "mqueries_s")
    ax4.plot(xs, ys, label=LABEL[e], **style(e))
ax4.set_title("4. Read scaling — N readers + 1 updater, 960 keys/table\n"
              "readers do not allocate, so this panel is allocator-neutral",
              fontsize=10)
ax4.set_xlabel("reader cores")
ax4.set_ylabel("aggregate queries/s (millions)")

for ax in (ax1, ax2, ax3, ax4):
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8, loc="best")
    if ax.get_yscale() == "linear":
        ax.set_ylim(bottom=0)
        ax.ticklabel_format(axis="y", style="plain")

fig.suptitle("urcu-txn (MCAS) vs. existence (flip) — three-hash atomic key move, "
             "2×96-core EPYC   [supersedes the pre-rotate-fix figure]", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.95])
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=130)
print("wrote", OUT)
