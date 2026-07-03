#!/usr/bin/env python3
"""
RLU vs txn (MCAS) write-scaling across three workloads: disjoint strided churn,
multi-slot random on a hot 64-slot index, and hash-of-lists.  Reads
scripts/rlu_vs_txn.csv (best-of-2), writes figures/rlu_vs_txn.png.
Run: python3 scripts/plot_rlu_vs_txn.py

The txn side is txn_list on the two list panels and the current single-pointer
txn_hlist on the hash panel; RLU is shown deferred (BENCH_RLU_WS=100) and
synchronous (=1).  Linear writer axis; Okabe-Ito colours matching the hash
figures (txn blue, RLU-defer purple, RLU-sync amber).
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "rlu_vs_txn.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "rlu_vs_txn.png")

rows = list(csv.DictReader(open(CSV)))

# (panel key, title, txn engine name in this panel)
PANELS = [
    ("churn",  "Disjoint churn\n(strided, no collisions)",        "txn_list"),
    ("random", "Multi-slot random\n(hot 64-slot index)",          "txn_list"),
    ("hash",   "Hash-of-lists\n(1000 × 100, shared domain)",      "txn_hlist"),
]
# (legend label, colour, marker) keyed by engine role
STYLE = {
    "txn":       ("#0072B2", "o"),
    "rlu_defer": ("#CC79A7", "D"),
    "rlu_sync":  ("#E69F00", "v"),
}
LABEL = {"txn": "txn (MCAS)", "rlu_defer": "RLU-defer", "rlu_sync": "RLU-sync"}

def series(panel, engine):
    acc = defaultdict(list)
    for r in rows:
        if r["panel"] == panel and r["mode"] == "write" and r["engine"] == engine:
            acc[int(r["x"])].append(float(r["b"]))          # b = write_mops
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs], [min(acc[x]) for x in xs], [max(acc[x]) for x in xs]

plt.rcParams.update({"font.size": 10, "axes.edgecolor": "#888888",
                     "axes.linewidth": 0.8, "figure.facecolor": "white"})
fig, axes = plt.subplots(1, 3, figsize=(13.5, 4.6), dpi=150)
xticks = [0, 32, 64, 96, 128, 160, 192]

for ax, (panel, title, txn_eng) in zip(axes, PANELS):
    curves = [("txn", txn_eng)] + [("rlu_defer", "rlu_defer"), ("rlu_sync", "rlu_sync")]
    ymax = 0
    for role, eng in curves:
        color, marker = STYLE[role]
        xs, best, lo, hi = series(panel, eng)
        if not xs:
            continue
        ymax = max(ymax, max(hi))
        ax.fill_between(xs, lo, hi, color=color, alpha=0.15, lw=0, zorder=2)
        ax.plot(xs, best, color=color, lw=1.9, marker=marker, ms=6,
                markeredgecolor="white", markeredgewidth=1.0, zorder=4, label=LABEL[role])
    ax.set_title(title, fontsize=11, fontweight="bold")
    ax.set_xlim(0, 196)
    ax.xaxis.set_major_locator(FixedLocator(xticks))
    ax.set_xticklabels([str(t) for t in xticks])
    ax.set_ylim(0, ymax * 1.08 if ymax else 1)
    ax.set_xlabel("writers", fontsize=10)
    ax.grid(True, color="#ececec", lw=0.7, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
axes[0].set_ylabel("write throughput (Mops/s)", fontsize=10)

handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=3, fontsize=10.5,
           frameon=False, bbox_to_anchor=(0.5, 1.02))
fig.suptitle("RLU vs txn (MCAS) write scaling — jemalloc percpu, best-of-2, 2× EPYC 9654",
             fontsize=12.5, fontweight="bold", y=1.10)
fig.tight_layout(rect=(0, 0, 1, 0.90))
fig.savefig(OUT, bbox_inches="tight")
print("wrote", os.path.normpath(OUT))
