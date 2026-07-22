#!/usr/bin/env python3
"""
Adversarial move-HEIGHT sweep -- how far does the localized reader's lead survive?

The S3 sweep moves leaves (fan-in 1): the localized reader's best case.  Here the
reader workload is fixed (uniform full-depth walks over a balanced binary forest)
and the writers move nodes at a swept HEIGHT H; a move at height H swaps two
sibling subtrees of 2^H leaves, invalidating a fraction ~2^(H-D) of reader walks.
The localized reader's lead over the seqlock baseline erodes from its leaf-case
peak as H climbs (a near-root move touches almost every walk), but it does NOT
reach parity: at H=7 the deletion-MARK reader and the bucket lock are still ~10x
and ~8x the seqlock baseline, because seqlock retries the WHOLE walk on ANY rename
regardless of height, while the localized reader only retries walks that actually
pass through the moved subtree.  So this bounds the S3 headline: the localized
lead shrinks ~40% for the rare near-root move but never collapses to the baseline.

Plotted: seqlock vs the localized deletion-MARK reader (txn-mark) and the bucket
lock + SW (bucketlock, same mark reader, fold-lock writer).  The txn-global and
txn-pernode arms stay in the CSV but are dropped from the plot (global is
uniformly floored -- one counter slot serializes all writers; per-node coincides
with the mark reader, differing only in which CL0 word the stamp reads).

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

COLOR = {"seqlock": "#D55E00", "txn-pernode": "#009E73", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARKER = {"seqlock": "s", "txn-pernode": "^", "txn-mark": "D", "bucketlock": "X"}
LABEL = {
    "seqlock": "seqlock — rename_lock + d_seq\n(faithful kernel baseline)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(localized: moved subtree only)",
    "txn-mark": "urcu-txn — deletion MARK as gen\n(localized, no counter)",
    "bucketlock": "bucket lock + SW txn\n(fold-lock writer)",
}
# The height panel is where the localized arms are WEAKEST (their lead
# erodes toward the root), so it is the panel where they could plausibly diverge
# rather than coincide -- worth plotting the mark arm here explicitly.
ENGINES = tuple(os.environ.get(
    "ENGINES", "seqlock txn-mark bucketlock").split())
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
# the ANNOT (mark) arm's lead over the seqlock baseline at each height -- the erosion.
# Label above the point where it leads, below where it dips under
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
             "× = mark ÷ seqlock\nboth localized arms (deletion MARK reader, bucket "
             "lock) erode ~40% as moves\nclimb toward the band root, but stay ~8–10× "
             "the seqlock baseline (jemalloc)",
             fontsize=9)
ax.grid(alpha=0.3, ls=":")
ax.legend(fontsize=8, loc="best")
fig.suptitle("Userspace dcache — how high can a DIRECTORY rename climb before the\n"
             "localized reader stops helping?  It erodes toward the root but stays ~8–10× "
             "the\nseqlock baseline (which retries the whole walk on any rename) — never parity "
             "(jemalloc)",
             fontsize=10.5)
fig.tight_layout(rect=[0, 0, 1, 0.93])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
