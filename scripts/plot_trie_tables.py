#!/usr/bin/env python3
"""
Render the trie-comparison tables of README.md as figures (no benchmark rerun:
the data below is transcribed verbatim from the README tables, which are the
published medians/best-of-N).  Writes one PNG per table (or per table pair that
tells one story) into figures/.  Run: python3 scripts/plot_trie_tables.py

Style follows the existing repo figures: Okabe-Ito CVD-safe categorical colors,
white surface, marker shapes double-encoding series identity for print/CVD.
Engine colors are entity-consistent across every figure (FT is always blue,
HOTRowex vermilion, ART-OLC green, ART-ROWEX reddish purple, Masstree orange,
BIND9-QP black); the rwlock trio (judy/qp/art) renders as a muted dashed-gray
class since its role is a baseline that collapses.
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FixedLocator

HERE = os.path.dirname(os.path.abspath(__file__))
FIGDIR = os.path.join(HERE, os.pardir, "figures")

# ── shared style ──────────────────────────────────────────────────────────────
plt.rcParams.update({
    "font.size": 10.5, "axes.edgecolor": "#888888", "axes.linewidth": 0.8,
    "figure.facecolor": "white", "axes.facecolor": "white",
})

# (color, marker) per engine — identical in every figure it appears in.
ENG = {
    "ft":       ("#0072B2", "o"),   # blue      -- the subject (ft / ft_spec / ft_spec_il)
    "ft2":      ("#56B4E9", "v"),   # sky blue  -- second FT variant (ft_qsbr / ft_eager)
    "hotrowex": ("#D55E00", "s"),   # vermilion
    "artolc":   ("#009E73", "^"),   # green
    "artrowex": ("#CC79A7", "D"),   # reddish purple
    "masstree": ("#E69F00", "P"),   # orange
    "b9qp":     ("#000000", "X"),   # black     -- BIND9 dns_qpmulti
    "judy":     ("#8c8c8c", "<"),   # ┐ rwlock trio: muted grays, dashed
    "qp":       ("#5a5a5a", ">"),   # │
    "art":      ("#b3b3b3", "d"),   # ┘
}
RWLOCK = {"judy", "qp", "art"}
GRAY_BAR = "#c2c2c2"


def style_ax(ax, ygrid=True, xgrid=False):
    if ygrid:
        ax.grid(axis="y", color="#e6e6e6", lw=0.8, zorder=0)
    if xgrid:
        ax.grid(axis="x", color="#e6e6e6", lw=0.8, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)


def save(fig, name, rect=None):
    out = os.path.join(FIGDIR, name)
    fig.tight_layout(rect=rect) if rect else fig.tight_layout()
    fig.savefig(out, bbox_inches="tight")
    plt.close(fig)
    print("wrote", os.path.normpath(out))


def lookup_bar_panels(name, title, subtitle, panels, rows, figsize):
    """Horizontal-bar small multiples for the single-threaded ns/op tables.
    rows = [(label, accent_color_or_None, [v per panel])] in README table order
    (fastest-first by the lead dataset); same engine on the same row in every
    panel so cross-panel comparison is by eye."""
    n = len(rows)
    fig, axes = plt.subplots(1, len(panels), figsize=figsize, dpi=150, sharey=True)
    ypos = [n - 1 - i for i in range(n)]
    for j, (ax, pname) in enumerate(zip(axes, panels)):
        vals = [r[2][j] for r in rows]
        colors = [r[1] or GRAY_BAR for r in rows]
        ax.barh(ypos, vals, height=0.72, color=colors, zorder=3)
        vmax = max(vals)
        for y, v in zip(ypos, vals):
            ax.text(v + vmax * 0.02, y, str(v), va="center", fontsize=8.5,
                    color="#333333", zorder=4)
        ax.set_xlim(0, vmax * 1.18)
        ax.set_title(f"`{pname}`".replace("`", ""), fontsize=11, fontweight="bold")
        ax.set_xlabel("lookup ns/op (lower is better)", fontsize=9)
        style_ax(ax, ygrid=False, xgrid=True)
        ax.tick_params(axis="y", length=0)
    axes[0].set_yticks(ypos, [r[0] for r in rows], fontsize=9.5)
    fig.suptitle(title, fontsize=13, fontweight="bold", x=0.5, y=1.06)
    fig.text(0.5, 1.0, subtitle, ha="center", fontsize=9.5, color="#666666")
    save(fig, name)


# ══ 1. bench_one_st — string keys (ns/op, 1M keys, table order = by dns) ══════
lookup_bar_panels(
    "st_lookup_strings.png",
    "Single-threaded lookup latency — string keys (1M keys)",
    "best of RUNS timed passes · 2× EPYC 9654 · FT in blue · † separate GPL binary · ‡ hash-based, not order-preserving",
    ["dns", "dict", "paths"],
    [
        ("hot",        None,          [99, 106, 77]),
        ("wormhole †", None,          [113, 101, 114]),
        ("qp",         None,          [118, 127, 177]),
        ("ft_spec",    ENG["ft"][0],  [119, 112, 143]),
        ("judyhs ‡",   None,          [145, 121, 149]),
        ("art",        None,          [183, 167, 223]),
        ("ft_eager",   ENG["ft2"][0], [185, 158, 202]),
        ("judysl",     None,          [202, 250, 263]),
        ("artolc",     None,          [218, 212, 238]),
        ("masstree",   None,          [231, 193, 207]),
        ("cuckoo",     None,          [335, 301, 332]),
    ],
    (10.5, 4.6),
)

# ══ 2. bench_one_st — integer keys (ns/op, table order = by u64d) ═════════════
lookup_bar_panels(
    "st_lookup_ints.png",
    "Single-threaded lookup latency — integer keys (1M keys)",
    "u32/u64 × dense sequential / sparse random · FT in blue · † separate GPL binary · ‡ hash-based, not order-preserving",
    ["u32d", "u32s", "u64d", "u64s"],
    [
        ("judyl",      None,          [11, 38, 11, 64]),
        ("art",        None,          [11, 46, 12, 47]),
        ("qp",         None,          [13, 13, 13, 13]),
        ("ft_spec",    ENG["ft"][0],  [14, 33, 15, 33]),
        ("ft_eager",   ENG["ft2"][0], [13, 42, 16, 44]),
        ("hot",        None,          [20, 49, 20, 49]),
        ("artolc",     None,          [22, 96, 24, 98]),
        ("judyhs ‡",   None,          [24, 46, 38, 78]),
        ("wormhole †", None,          [34, 88, 38, 92]),
        ("masstree",   None,          [46, 172, 46, 168]),
        ("cuckoo",     None,          [96, 114, 118, 117]),
    ],
    (11.5, 4.6),
)

# ══ 3. load-names — FT vs BIND9 QP-trie @ 192 cores (median of 5, min–max) ════
fig, ax = plt.subplots(figsize=(8.0, 2.6), dpi=150)
data = [  # (label, median, lo, hi, color)
    ("ft_spec_il", 1246, 1227, 1273, ENG["ft"][0]),
    ("qp_il",       938,  804,  976, ENG["b9qp"][0]),
]
ypos = [1, 0]
for (label, med, lo, hi, color), y in zip(data, ypos):
    ax.barh(y, med, height=0.62, color=color, zorder=3)
    ax.errorbar(med, y, xerr=[[med - lo], [hi - med]], fmt="none",
                ecolor="#555555", elinewidth=1.4, capsize=4, zorder=5)
    ax.text(hi + 25, y, f"{med} Mops/s  ({lo}–{hi})", va="center",
            fontsize=10, color="#333333")
ax.set_yticks(ypos, [d[0] for d in data], fontsize=11)
ax.tick_params(axis="y", length=0)
ax.set_xlim(0, 1600)
ax.set_xlabel("query throughput @ 192 cores (Mops/s, higher is better)", fontsize=10)
ax.text(1246 / 2, 1, "≈ 1.3× BIND9-QP", va="center", ha="center",
        fontsize=10.5, color="white", fontweight="bold", zorder=6)
style_ax(ax, ygrid=False, xgrid=True)
ax.set_title("load-names @ 192 cores — Fractal Trie vs BIND9 dns_qpmulti",
             fontsize=12.5, fontweight="bold", pad=24)
ax.text(0.0, 1.12, "1M DNS names, both NUMA-interleaved + cache-primed · "
        "median of 5 (whisker = min–max) · 2× EPYC 9654",
        transform=ax.transAxes, fontsize=9, color="#666666")
save(fig, "loadnames_ft_vs_qp.png")

# ══ 4. load-names — thread sweep, 5 concurrent engines (median of 4) ══════════
LOADNAMES = [  # (engine key, label, [64, 128, 192])
    ("ft",       "ft_spec_il", [317, 758, 1212]),
    ("hotrowex", "hotrowex",   [355, 719, 1021]),
    ("artolc",   "artolc",     [208, 428, 685]),
    ("artrowex", "artrowex",   [203, 424, 701]),
    ("masstree", "masstree",   [166, 262, 267]),
]
fig, ax = plt.subplots(figsize=(8.6, 5.2), dpi=150)
xs = [64, 128, 192]
for key, label, ys in LOADNAMES:
    color, marker = ENG[key]
    ax.plot(xs, ys, color=color, lw=2.0, marker=marker, ms=7,
            markeredgecolor="white", markeredgewidth=1.1, zorder=4, label=label)
ax.annotate("crossover ≈ 128 threads", xy=(128, 740), xytext=(76, 950),
            fontsize=9.5, color="#333333",
            arrowprops=dict(arrowstyle="-|>", color="#555555", lw=1.1,
                            connectionstyle="arc3,rad=-0.2"))
ax.set_xlim(52, 204)
ax.xaxis.set_major_locator(FixedLocator(xs))
ax.set_ylim(0, 1350)
ax.set_xlabel("threads (one per physical core)", fontsize=10.5)
ax.set_ylabel("query throughput (Mops/s, higher is better)", fontsize=10.5)
style_ax(ax)
leg = ax.legend(loc="upper left", frameon=True, fontsize=10)
leg.get_frame().set_edgecolor("#dddddd")
ax.set_title("load-names lookup scaling — FT vs HOTRowex vs ART-OLC/ROWEX vs Masstree",
             fontsize=12.5, fontweight="bold", pad=26)
ax.text(0.0, 1.03, "1M DNS names, sequential lookups, all engines validating · "
        "median of 4 · Masstree plateaus past 128", transform=ax.transAxes,
        fontsize=9.5, color="#666666")
save(fig, "loadnames_scaling.png")

# ══ 5. bench_scale — read throughput vs readers (+RSS), 6 engines ═════════════
#      two tables → two line panels (1 writer + N readers / readers only) + RSS
SCALE_RW = [  # (key, label, w=[64,96,128,191], ro=[64,96,128,192], rss MB)
    ("ft",       "ft",       [160, 226, 286, 380], [166, 236, 298, 396], 320),
    ("ft2",      "ft_qsbr",  [166, 234, 290, 377], [173, 248, 301, 393], 320),
    ("hotrowex", "hotrowex", [177, 252, 320, 426], [179, 259, 323, 431], 110),
    ("artolc",   "artolc",   [127, 188, 250, 367], [129, 192, 255, 378], 141),
    ("artrowex", "artrowex", [123, 182, 243, 356], [127, 188, 251, 372], 141),
    ("masstree", "masstree", [116, 171, 227, 330], [116, 172, 228, 333], 185),
]
fig, axes = plt.subplots(1, 3, figsize=(12.5, 4.8), dpi=150,
                         gridspec_kw={"width_ratios": [1, 1, 0.62]})
for ax, xs, col, ptitle in [
        (axes[0], [64, 96, 128, 191], 3, "1 writer + N readers (churn)"),
        (axes[1], [64, 96, 128, 192], 4, "readers only (no writer)")]:
    for row in SCALE_RW:
        key, label = row[0], row[1]
        color, marker = ENG[key]
        ax.plot(xs, row[col - 1], color=color, lw=1.9, marker=marker, ms=6,
                markeredgecolor="white", markeredgewidth=1.0, zorder=4, label=label)
    ax.set_xlim(56, 200)
    ax.xaxis.set_major_locator(FixedLocator(xs))
    ax.set_ylim(0, 460)
    ax.set_xlabel("reader threads", fontsize=10)
    ax.set_title(ptitle, fontsize=11, fontweight="bold")
    style_ax(ax)
axes[0].set_ylabel("read throughput (Mops/s)", fontsize=10.5)
ypos = list(range(len(SCALE_RW)))[::-1]
axes[2].barh(ypos, [r[4] for r in SCALE_RW], height=0.7,
             color=[ENG[r[0]][0] for r in SCALE_RW], zorder=3)
for y, r in zip(ypos, SCALE_RW):
    axes[2].text(r[4] + 8, y, f"{r[4]} MB", va="center", fontsize=8.5, color="#333333")
axes[2].set_yticks(ypos, [r[1] for r in SCALE_RW], fontsize=9)
axes[2].tick_params(axis="y", length=0)
axes[2].set_xlim(0, 420)
axes[2].set_xlabel("RSS after build (MB)", fontsize=10)
axes[2].set_title("footprint", fontsize=11, fontweight="bold")
style_ax(axes[2], ygrid=False, xgrid=True)
handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", ncol=6, fontsize=10,
           frameon=False, bbox_to_anchor=(0.5, 1.02))
fig.suptitle("bench_scale read scaling — 1M DNS keys, validated lookups, "
             "one thread per physical core (medians)",
             fontsize=12.5, fontweight="bold", y=1.09)
save(fig, "scale_rw_reads.png")

# ══ 6. mutator insert throughput vs reader count (the rwlock cliff) ═══════════
INSERT = [  # (key, label, [0, 1, 16, 64, 191] readers, kops/s)
    ("masstree", "masstree (optimistic)", [11272, 11123, 10589, 10508, 9494]),
    ("hotrowex", "hotrowex (ROWEX)",      [5680, 5610, 5235, 4713, 4152]),
    ("artolc",   "artolc (OLC)",          [4091, 3272, 2160, 1796, 1457]),
    ("artrowex", "artrowex (ROWEX)",      [3421, 2849, 2087, 1639, 1469]),
    ("ft",       "ft (RCU)",              [1340, 592, 666, 326, 498]),
    ("b9qp",     "b9qp (RCU)",            [441, 419, 288, 268, 191]),
    ("judy",     "judy (rwlock)",         [10628, 2.8, 19, 92, 429]),
    ("qp",       "qp (rwlock)",           [9170, 3.8, 33, 134, 513]),
    ("art",      "art (rwlock)",          [10197, 2.8, 34, 113, 461]),
]
READERS = ["0", "1", "16", "64", "191"]
fig, ax = plt.subplots(figsize=(9.6, 5.8), dpi=150)
xp = range(len(READERS))
for key, label, ys in INSERT:
    color, marker = ENG[key]
    ls = (0, (4, 2.2)) if key in RWLOCK else "-"
    ax.plot(xp, ys, color=color, lw=1.9, ls=ls, marker=marker, ms=6.5,
            markeredgecolor="white", markeredgewidth=1.0, zorder=4, label=label)
ax.annotate("rwlock cliff: ~10M → 3 kops/s the instant\none reader appears "
            "(readers hold the rdlock\nacross 1000-lookup batches)",
            xy=(1, 3.0), xytext=(1.42, 1.7), fontsize=9.5, color="#333333",
            arrowprops=dict(arrowstyle="-|>", color="#555555", lw=1.2,
                            connectionstyle="arc3,rad=-0.25"))
ax.set_yscale("log")
ax.set_ylim(1, 40000)
ax.set_xticks(list(xp), READERS)
ax.set_xlabel("concurrent reader threads (1 mutator)", fontsize=10.5)
ax.set_ylabel("insert throughput (kops/s, log scale)", fontsize=10.5)
ax.grid(axis="y", which="both", color="#ececec", lw=0.7, zorder=0)
ax.set_axisbelow(True)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
fig.legend(*ax.get_legend_handles_labels(), loc="upper center", ncol=5,
           fontsize=9, frameon=False, bbox_to_anchor=(0.5, 0.995))
fig.text(0.5, 1.008, "BENCH_MUTATOR=1, median of 3 · rwlock engines dashed · log y",
         ha="center", fontsize=9.5, color="#666666")
fig.suptitle("Single-mutator insert throughput vs reader concurrency — "
             "RCU and the concurrent tries never collapse",
             fontsize=12, fontweight="bold", y=1.065)
save(fig, "mutator_insert.png", rect=(0, 0, 1, 0.89))

# ══ 7. replace / remove at the endpoints (0 vs 191 readers) — dumbbells ═══════
REPL = [  # (label, replace0, replace191, remove0, remove191)  None = n/a
    ("masstree", 12913, 12183, 12175, 9207),
    ("hotrowex", 6185, 4516, None, None),
    ("artolc",   1243, 1170, 1193, 1057),
    ("artrowex", 790, 702, 1144, 929),
    ("ft",       1057, 375, 2412, 754),
    ("b9qp",     446, 196, 471, 206),
    ("judy",     14923, 669, 9015, 358),
    ("qp",       17265, 1358, 16343, 1050),
    ("art",      15361, 800, 12311, 482),
]
C0, C191 = "#b3b3b3", "#0072B2"
fig, axes = plt.subplots(1, 2, figsize=(11.5, 4.8), dpi=150, sharey=True)
n = len(REPL)
ypos = [n - 1 - i for i in range(n)]
for ax, (i0, i1), ptitle in [(axes[0], (1, 2), "replace"), (axes[1], (3, 4), "remove")]:
    for y, row in zip(ypos, REPL):
        v0, v1 = row[i0], row[i1]
        if v0 is None:
            ax.text(80, y, "n/a — ROWEX has no delete", va="center",
                    fontsize=8.5, color="#888888", style="italic")
            continue
        ax.plot([v0, v1], [y, y], color="#cccccc", lw=2.2, zorder=2)
        ax.plot(v0, y, "o", color=C0, ms=8, markeredgecolor="white",
                markeredgewidth=1.0, zorder=4)
        ax.plot(v1, y, "o", color=C191, ms=8, markeredgecolor="white",
                markeredgewidth=1.0, zorder=5)
    ax.set_xscale("log")
    ax.set_xlim(60, 40000)
    ax.set_title(ptitle, fontsize=11.5, fontweight="bold")
    ax.set_xlabel("throughput (kops/s, log scale)", fontsize=10)
    ax.grid(axis="x", which="major", color="#ececec", lw=0.7, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.tick_params(axis="y", length=0)
axes[0].set_yticks(ypos, [r[0] for r in REPL], fontsize=10)
legend_items = [
    Line2D([], [], color=C0, marker="o", ls="", ms=8, markeredgecolor="white",
           label="0 readers"),
    Line2D([], [], color=C191, marker="o", ls="", ms=8, markeredgecolor="white",
           label="191 readers"),
]
fig.legend(handles=legend_items, loc="upper center", ncol=2, fontsize=10.5,
           frameon=False, bbox_to_anchor=(0.5, 1.02))
fig.suptitle("Replace & remove throughput, 0 → 191 concurrent readers — "
             "the rwlock engines lose 10–25×, RCU/optimistic engines degrade gently",
             fontsize=12, fontweight="bold", y=1.1)
save(fig, "mutator_replace_remove.png")

# ══ 8. ordered iteration throughput vs reader threads ═════════════════════════
ITER = [  # (key, label, [1, 16, 64, 192] next-Mops/s)
    ("ft",       "ft (batched cell gather)", [510, 8168, 32678, 88124]),
    ("hotrowex", "hotrowex",                 [175, 2540, 9759, 27471]),
    ("b9qp",     "b9qp",                     [91, 1467, 5851, 15072]),
    ("art",      "art",                      [22, 339, 1243, 3745]),
    ("masstree", "masstree",                 [19, 261, 1065, 2972]),
    ("artolc",   "artolc",                   [18, 224, 840, 2532]),
    ("artrowex", "artrowex",                 [15, 194, 818, 2227]),
    ("judy",     "judy",                     [5.2, 82, 337, 968]),
    ("qp",       "qp",                       [4.3, 65, 263, 785]),
]
XT = ["1", "16", "64", "192"]
fig, ax = plt.subplots(figsize=(9.6, 5.8), dpi=150)
xp = range(len(XT))
for key, label, ys in ITER:
    color, marker = ENG[key]
    ls = (0, (4, 2.2)) if key in RWLOCK else "-"
    ax.plot(xp, ys, color=color, lw=1.9, ls=ls, marker=marker, ms=6.5,
            markeredgecolor="white", markeredgewidth=1.0, zorder=4, label=label)
ax.text(0.03, 0.96, "ft = batched cell gather (FT_ORD + compaction + FT_BATCH=64):\n"
        "~3.2× hotrowex at 192T",
        transform=ax.transAxes, fontsize=9.5, color="#333333", va="top")
ax.set_yscale("log")
ax.set_ylim(3, 300000)
ax.set_xticks(list(xp), XT)
ax.set_xlabel("reader threads (each loops a full in-order traversal)", fontsize=10.5)
ax.set_ylabel("aggregate next-op throughput (Mops/s, log scale)", fontsize=10.5)
ax.grid(axis="y", which="major", color="#ececec", lw=0.7, zorder=0)
ax.set_axisbelow(True)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
fig.legend(*ax.get_legend_handles_labels(), loc="upper center", ncol=5,
           fontsize=9, frameon=False, bbox_to_anchor=(0.5, 0.995))
fig.text(0.5, 1.008, "BENCH_ITERATE=1, median of 3 · 1M keys · log y · "
         "ST engines (judy/qp/art) dashed gray",
         ha="center", fontsize=9.5, color="#666666")
fig.suptitle("Ordered iteration scaling — every engine near-linear; "
             "contiguity + a tight inner loop win",
             fontsize=12, fontweight="bold", y=1.065)
save(fig, "ordered_iteration.png", rect=(0, 0, 1, 0.89))

# ══ 9. FT ordered-scan configuration steps @ 192T ═════════════════════════════
STEPS = [  # (label, Mops/s) — sequential blues: one hue, light→dark (magnitude)
    ("cds_ft_next descent (pre-cell)", 350, "#b8d4ea"),
    ("+ ordered cell list (FT_ORD)", 3641, "#7fb2d9"),
    ("+ compaction (FT_BENCH_COMPACT)", 20008, "#3d8bc4"),
    ("+ batched gather (FT_BATCH=64)", 88124, "#0072B2"),
]
fig, ax = plt.subplots(figsize=(9.0, 3.0), dpi=150)
ypos = [3, 2, 1, 0]
ax.barh(ypos, [s[1] for s in STEPS], height=0.66,
        color=[s[2] for s in STEPS], zorder=3)
prev = None
for y, (label, v, _) in zip(ypos, STEPS):
    note = f"{v:,}" + (f"   (×{v / prev:.1f})" if prev else "")
    ax.text(v * 1.12, y, note, va="center", fontsize=9.5, color="#333333")
    prev = v
ax.set_xscale("log")
ax.set_xlim(200, 700000)
ax.set_yticks(ypos, [s[0] for s in STEPS], fontsize=10)
ax.tick_params(axis="y", length=0)
ax.set_xlabel("next-op throughput @ 192 threads (Mops/s, log scale)", fontsize=10)
ax.grid(axis="x", which="major", color="#ececec", lw=0.7, zorder=0)
ax.set_axisbelow(True)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
ax.set_title("How FT ordered iteration got 252× faster — each step, same 1M-key set",
             fontsize=12, fontweight="bold", pad=10)
save(fig, "ft_iter_steps.png")

# ══ 10. qpmulti_ft — FT vs dns_qpmulti in bind9's event loop (2 panels) ═══════
QPM = {
    "Read-only": {
        "xticks": ["1", "16", "64", "192"],
        "series": [  # (key, label, values Mops/s)
            ("b9qp", "qp (dns_qpmulti)", [2.3, 42.7, 170, 508]),
            ("ft",   "FT speculative",   [2.3, 40.3, 161, 462]),
            ("ft2",  "FT eager",         [2.2, 39.9, 159, 443]),
        ],
        "note": "miss-heavy (~50%): qp exits early on a miss — FT trails ~9%",
    },
    "Mutate + read": {
        "xticks": ["1", "16", "64", "191"],
        "series": [
            ("b9qp", "qp (dns_qpmulti)", [1.3, 34.2, 133, 450]),
            ("ft",   "FT speculative",   [1.7, 35.8, 148, 450]),
            ("ft2",  "FT eager",         [1.5, 34.6, 140, 430]),
        ],
        "note": "under write contention FT pulls level/ahead (1.0–1.31×)",
    },
}
fig, axes = plt.subplots(1, 2, figsize=(11.0, 4.8), dpi=150, sharey=True)
for ax, (ptitle, p) in zip(axes, QPM.items()):
    xp = range(len(p["xticks"]))
    for key, label, ys in p["series"]:
        color, marker = ENG[key]
        ax.plot(xp, ys, color=color, lw=2.0, marker=marker, ms=7,
                markeredgecolor="white", markeredgewidth=1.1, zorder=4, label=label)
    ax.set_yscale("log")
    ax.set_ylim(1, 900)
    ax.set_xticks(list(xp), p["xticks"])
    ax.set_xlabel("reader loops", fontsize=10)
    ax.set_title(ptitle, fontsize=11.5, fontweight="bold")
    ax.text(0.03, 0.965, p["note"], transform=ax.transAxes, fontsize=9,
            color="#666666", va="top")
    ax.grid(axis="y", which="major", color="#ececec", lw=0.7, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
axes[0].set_ylabel("aggregate read throughput (Mops/s, log scale)", fontsize=10.5)
leg = axes[1].legend(loc="lower right", frameon=True, fontsize=9.5)
leg.get_frame().set_edgecolor("#dddddd")
fig.suptitle("qpmulti_ft — FT vs BIND9 dns_qpmulti inside isc_loopmgr "
             "(~500k entries, ~50% lookup misses, 192 cores)",
             fontsize=12, fontweight="bold", y=1.02)
save(fig, "qpmulti_ft.png")
