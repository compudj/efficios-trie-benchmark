#!/usr/bin/env python3
"""
Adversarial move-HEIGHT sweep -- how far does the per-node localization survive?

The S3 sweep moves leaves (fan-in 1): the per-node counter's best case.  Here the
reader workload is fixed (uniform full-depth walks over a balanced binary forest)
and the writers move nodes at a swept HEIGHT H; a move at height H swaps two
sibling subtrees of 2^H leaves, invalidating a fraction ~2^(H-D) of reader walks.
The per-node reader's lead over the seqlock baseline should erode from its
leaf-case peak toward parity as H climbs, because a near-root move touches almost
every walk -- exactly what rename_lock does.  That erosion is the honest BOUND on
the S3 headline: per-node wins big for the common (low-fan-in) rename, and degrades
gracefully to the baseline for the rare near-root move.

Two-way (seqlock vs per-node), consistent with dcache_s3.png; the txn-global arm
(uniformly floored here -- its one counter slot serializes all 8 writers AND its
deep-walk readers retry on every bump) stays in the CSV.

Data: scripts/dcache_height.csv (best-of-5, conservation-gated).  2x96 EPYC.
"""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, FixedLocator, FixedFormatter

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_height.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures", "dcache_height.png"))

COLOR = {"seqlock": "#D55E00", "txn-pernode": "#009E73", "txn-mark": "#CC79A7"}
MARKER = {"seqlock": "s", "txn-pernode": "^", "txn-mark": "D"}
LABEL = {
    "seqlock": "seqlock — rename_lock + d_seq\n(faithful kernel baseline)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(localized: moved subtree only)",
    "txn-mark": "urcu-txn — deletion MARK as gen\n(localized, no counter)",
}
# The height panel is where the localized arms are WEAKEST (the per-node lead
# erodes toward the root), so it is the panel where they could plausibly diverge
# rather than coincide -- worth plotting the mark arm here explicitly.
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock txn-pernode txn-mark").split())
rows = [r for r in csv.DictReader(open(CSV)) if r["conserved"] == "OK"]


def series(eng):
    pts = sorted((int(r["move_height"]), float(r["mlookups_s"]))
                 for r in rows if r["engine"] == eng)
    return [h for h, _ in pts], [y for _, y in pts]


fig, ax = plt.subplots(figsize=(8.2, 6))
base = {e: dict(zip(*series(e))) for e in ENGINES}
for e in ENGINES:
    xs, ys = series(e)
    if xs:
        ax.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=7,
                label=LABEL[e], alpha=0.94)
# per-node lead over the seqlock baseline at each height -- the erosion.
# Label above the per-node point where it leads, below where it dips under
# seqlock (so the text never lands on the other curve at the crossover).
ANNOT = os.environ.get("ANNOT", "txn-mark")
for h in sorted(base.get(ANNOT, {})):
    if h in base["seqlock"] and base["seqlock"][h] > 0:
        v, sq = base[ANNOT][h], base["seqlock"][h]
        dy = 10 if v >= sq else -18
        ax.annotate(f"{v / sq:.2f}×", (h, v),
                    textcoords="offset points", xytext=(0, dy),
                    ha="center", fontsize=8.5, color=COLOR[ANNOT],
                    fontweight="bold")

heights = sorted({int(r["move_height"]) for r in rows})
ax.set_xticks(heights)
ax.xaxis.set_major_formatter(FixedFormatter([str(h) for h in heights]))
# headroom above the tallest curve so the ratio labels (offset upward) clear the
# top spine; x-margin so the H=0 / H=D-1 labels do not clip the side spines.
ymax = max((float(r["mlookups_s"]) for r in rows), default=1.0)
ax.set_ylim(bottom=0, top=ymax * 1.18)
ax.margins(x=0.06)
ax.yaxis.set_major_formatter(FuncFormatter(lambda v, p: f"{v:g}"))
ax.set_xlabel("move height H above the leaves  "
              "(fan-in = 2^H leaves dominated: 1 at H=0 → 128 at H=7)")
ax.set_ylabel("reader Mlookups/s   (higher is better)")
ax.set_title("Move-height sweep — 32 readers + 8 writers, balanced binary bands "
             "(256 leaves)\nwriters exchange sibling subtrees at height H;  "
             "× = mark ÷ seqlock\nthe COUNTER's localization erodes as moves climb "
             "toward the band root,\ncrossing BELOW the baseline at H=7; the MARK's does not",
             fontsize=9)
ax.grid(alpha=0.3, ls=":")
ax.legend(fontsize=8, loc="best")
fig.suptitle("Userspace dcache — how high can a rename climb before the localized\n"
             "version stops helping?  The per-node COUNTER inverts at H=7; the MARK does not.",
             fontsize=10.5)
fig.tight_layout(rect=[0, 0, 1, 0.93])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
