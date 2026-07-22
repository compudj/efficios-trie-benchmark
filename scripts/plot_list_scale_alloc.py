#!/usr/bin/env python3
"""
txn_list writer scaling under three allocators -- glibc (slab off), jemalloc
percpu (slab off), and glibc + the per-CPU descriptor slab (slab on) -- on plain
churn and a composable transacted 100k-slot index.  Reads
scripts/list_scale_alloc.csv (best-of-2), writes figures/list_scale_alloc.png.
Run: python3 scripts/plot_list_scale_alloc.py

Shows the slab matching jemalloc's per-CPU arenas with no external allocator.
Okabe-Ito colours (glibc green, jemalloc amber, slab blue).
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "list_scale_alloc.csv")
OUT = os.path.join(HERE, os.pardir, "figures", "list_scale_alloc.png")

rows = list(csv.DictReader(open(CSV)))

PANELS = [
    ("churn",      "Plain churn\n(2-3-edge MCAS)"),
    ("composable", "Composable 100k-slot index\n(index + list in one MCAS)"),
]
STYLE = {"glibc": ("#009E73", "s"), "jemalloc": ("#E69F00", "v"),
         "glibc+slab": ("#0072B2", "o")}
LABEL = {"glibc": "glibc", "jemalloc": "jemalloc percpu", "glibc+slab": "glibc + descriptor slab"}

def series(panel, variant):
    acc = defaultdict(list)
    for r in rows:
        if r["panel"] == panel and r["variant"] == variant:
            acc[int(r["writers"])].append(float(r["write_mops"]))
    xs = sorted(acc)
    return xs, [max(acc[x]) for x in xs], [min(acc[x]) for x in xs], [max(acc[x]) for x in xs]

plt.rcParams.update({"font.size": 10, "axes.edgecolor": "#888888",
                     "axes.linewidth": 0.8, "figure.facecolor": "white"})
fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.6), dpi=150)
xticks = [0, 32, 64, 96, 128, 160, 192]

for ax, (panel, title) in zip(axes, PANELS):
    ymax = 0
    for variant in ("glibc", "jemalloc", "glibc+slab"):
        color, marker = STYLE[variant]
        xs, best, lo, hi = series(panel, variant)
        if not xs:
            continue
        ymax = max(ymax, max(hi))
        ax.fill_between(xs, lo, hi, color=color, alpha=0.15, lw=0, zorder=2)
        ax.plot(xs, best, color=color, lw=1.9, marker=marker, ms=6,
                markeredgecolor="white", markeredgewidth=1.0, zorder=4, label=LABEL[variant])
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
fig.suptitle("MCAS descriptor slab vs glibc / jemalloc -- txn_list writer scaling, best-of-2, 2× EPYC 9654",
             fontsize=12.5, fontweight="bold", y=1.10)
fig.tight_layout(rect=(0, 0, 1, 0.90))
fig.savefig(OUT, bbox_inches="tight")
print("wrote", os.path.normpath(OUT))
