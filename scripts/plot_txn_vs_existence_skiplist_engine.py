#!/usr/bin/env python3
"""
Skiplist: urcu-txn (MCAS) vs perfbook "existence" (flip), CURRENT engine.
Two txn configs: the production default (funnel-fix + bloom + full help/steal)
and the optimized spinlatch (+ NO_HELP + NO_STEAL, blocking/experimental).
Width matched at 3 key-moves; jemalloc; 2x96-core EPYC (384 hwthreads).
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "engine_cmp.csv")
OUT = os.environ.get("OUT", os.path.join(HERE, "txn_vs_existence_skiplist_engine.png"))

COLOR = {"existence": "#D55E00", "txn-default": "#0072B2", "txn-spin": "#009E73"}
MARKER = {"existence": "s", "txn-default": "o", "txn-spin": "^"}
LABEL = {
    "existence": "existence (flip a 3-key group)",
    "txn-default": "urcu-txn — production default\n(funnel-fix + Bloom + full help/steal)",
    "txn-spin": "urcu-txn — spinlatch\n(NO_HELP + NO_STEAL, blocking)",
}
rows = list(csv.DictReader(open(CSV)))

def series(panel, eng, xcol, ycol):
    acc = defaultdict(float)
    for r in rows:
        if r["panel"] == panel and r["engine"] == eng and float(r[ycol]) > 0:
            acc[int(r[xcol])] = float(r[ycol])   # already best-of-N in the CSV
    xs = sorted(acc)
    return xs, [acc[x] for x in xs]

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

# ---- Panel 1: fixed n=3840, ns/key-move vs cores (log) ----------------------
for e in ("existence", "txn-default", "txn-spin"):
    xs, ys = series("fixed", e, "cores", "ns_per_keymove")
    ax1.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=1.9, ms=6,
             label=LABEL[e], alpha=0.92)
# ratio annotations at 192
d = {e: dict(zip(*series("fixed", e, "cores", "ns_per_keymove")))
     for e in ("existence", "txn-default", "txn-spin")}
if 192 in d["existence"]:
    ex = d["existence"][192]
    for e, dy in (("txn-default", 12), ("txn-spin", -16)):
        if 192 in d[e]:
            ax1.annotate(f"{d[e][192]/ex:.2f}× existence", (192, d[e][192]),
                         textcoords="offset points", xytext=(-8, dy), ha="right",
                         fontsize=8.5, color=COLOR[e], fontweight="bold")
ax1.set_xscale("log", base=2); ax1.set_yscale("log")
ax1.set_xticks([1, 2, 4, 8, 16, 32, 64, 128, 192])
ax1.set_xticklabels([1, 2, 4, 8, 16, 32, 64, 128, 192])
ax1.set_title("Scaling — fixed 3840 keys/skiplist, width 3\n"
              "perfect scaling is FLAT; the three curves rise in parallel\n"
              "(same contention slope — the gap is work-per-move, not scaling)",
              fontsize=10)
ax1.set_xlabel("updater cores")
ax1.set_ylabel("ns per key-move   (lower is better, log)")

# ---- Panel 2: size dependence of the gap at 192 cores -----------------------
ex = dict(zip(*series("size", "existence", "keys_per_sl", "mmoves_s")))
for e in ("txn-default", "txn-spin"):
    tx = dict(zip(*series("size", e, "keys_per_sl", "mmoves_s")))
    xs = sorted(set(ex) & set(tx))
    ys = [ex[x] / tx[x] for x in xs]
    ax2.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=1.9, ms=6,
             label=LABEL[e], alpha=0.92)
    for x, y in zip(xs, ys):
        ax2.annotate(f"{y:.2f}×", (x, y), textcoords="offset points",
                     xytext=(0, 7), ha="center", fontsize=8, color=COLOR[e])
ax2.axhline(1.0, color="#888888", lw=1, ls="--", alpha=0.7)
ax2.text(ax2.get_xlim()[1] if False else 15360, 1.02, "parity", fontsize=8,
         color="#888888", ha="right", va="bottom")
ax2.set_xscale("log")
ax2.set_title("Gap vs structure size — 192 cores, width 3\n"
              "existence ÷ txn throughput; the gap shrinks as the skiplist grows\n"
              "(at 960 keys a level-7 predecessor slot is shared by ~26 cores)",
              fontsize=10)
ax2.set_xlabel("keys per skiplist   (log)")
ax2.set_ylabel("existence is this many × faster than txn")
ax2.set_ylim(bottom=1.0)

for ax in (ax1, ax2):
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8, loc="best")

fig.suptitle("Three-skiplist atomic key move — urcu-txn (MCAS) vs existence (flip), "
             "current engine   ·   2×96-core EPYC, jemalloc", fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
