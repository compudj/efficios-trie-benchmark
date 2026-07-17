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

COLOR = {"seqlock": "#D55E00", "txn-pernode": "#009E73"}
MARKER = {"seqlock": "s", "txn-pernode": "^"}
LABEL = {
    "seqlock": "seqlock — rename_lock + d_seq\n(faithful kernel baseline)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(localized: moved subtree only)",
}
ENGINES = ("seqlock", "txn-pernode")
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
# per-node lead over the seqlock baseline at each height -- the erosion
for h in sorted(base["txn-pernode"]):
    if h in base["seqlock"] and base["seqlock"][h] > 0:
        r = base["txn-pernode"][h] / base["seqlock"][h]
        ax.annotate(f"{r:.2f}×", (h, base["txn-pernode"][h]),
                    textcoords="offset points", xytext=(0, 9),
                    ha="center", fontsize=8.5, color=COLOR["txn-pernode"],
                    fontweight="bold")

heights = sorted({int(r["move_height"]) for r in rows})
ax.set_xticks(heights)
# label each height with its fan-in (2^H leaves the moved node dominates)
ax.xaxis.set_major_formatter(FixedFormatter(
    [f"{h}\n(2^{h}={2**h})" for h in heights]))
ax.set_ylim(bottom=0)
ax.yaxis.set_major_formatter(FuncFormatter(lambda v, p: f"{v:g}"))
ax.set_xlabel("move height H   (fan-in = leaves the moved node dominates)")
ax.set_ylabel("reader Mlookups/s   (higher is better)")
ax.set_title("Adversarial move-height sweep — 32 readers + 8 writers, balanced\n"
             "binary bands (256 leaves each), writers exchange sibling subtrees at\n"
             "height H (× = per-node ÷ seqlock: the localization eroding as moves\n"
             "climb from leaves toward the band root)", fontsize=9.5)
ax.grid(alpha=0.3, ls=":")
ax.legend(fontsize=8, loc="best")
fig.suptitle("Userspace dcache — how high can a rename climb before the per-node "
             "counter stops helping?   ·   2×96-core EPYC", fontsize=11)
fig.tight_layout(rect=[0, 0, 1, 0.95])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
