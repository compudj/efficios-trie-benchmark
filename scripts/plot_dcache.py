#!/usr/bin/env python3
"""
S3 -- userspace dentry-cache: can urcu-txn dissolve rename_lock + d_seq?

Three arms:
  seqlock      faithful kernel-style GLOBAL rename_lock + per-dentry d_seq
  txn-global   urcu-txn port, GLOBAL rename_gen walk bracket (deletes d_seq,
               but the reader still brackets one whole-tree counter)
  txn-pernode  urcu-txn port, PER-NODE host generation -- a rename bumps only
               the moved entry's own host counter, so a walk down a disjoint
               path re-reads a disjoint set of counters (no shared cacheline)

Data: scripts/dcache_sweep.csv (best-of-N, conservation-gated).  2x96 EPYC.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_sweep.csv")
OUT = os.environ.get("OUT", os.path.join(HERE, os.pardir, "figures", "dcache_s3.png"))

COLOR = {"seqlock": "#D55E00", "txn-global": "#0072B2", "txn-pernode": "#009E73"}
MARKER = {"seqlock": "s", "txn-global": "o", "txn-pernode": "^"}
LABEL = {
    "seqlock": "seqlock — rename_lock + d_seq\n(faithful kernel baseline)",
    "txn-global": "urcu-txn — GLOBAL rename_gen\n(d_seq deleted, one global bracket)",
    "txn-pernode": "urcu-txn — PER-NODE host gen\n(localized: moved entry only)",
}
ENGINES = ("seqlock", "txn-global", "txn-pernode")
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

# ---- Panel 1: HOMOGENEOUS mix -- lookup throughput vs rename fraction --------
for e in ENGINES:
    xs, ys = series("frac", e, "rename_frac", "mlookups_s")
    if not xs:
        continue
    xs = [max(x, 0.003) for x in xs]           # 0 -> just inside the log axis
    ax1.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=1.9, ms=6,
             label=LABEL[e], alpha=0.92)
if ax1.has_data():
    ax1.set_xscale("log"); ax1.set_yscale("log")
ax1.set_title("Homogeneous mix (48 threads) — lookup Mops/s vs rename fraction\n"
              "collapse is WRITER-bound (a rename ≈ 50× a lookup, so a small\n"
              "fraction eats a large time-share) — masks the reader-gen gap",
              fontsize=9.5)
ax1.set_xlabel("rename fraction   (log; leftmost = 0)")
ax1.set_ylabel("lookup Mops/s   (higher better, log)")

# ---- Panel 2: ROLE-SPLIT -- reader throughput vs writer load (THE headline) --
base = {}
for e in ENGINES:
    xs, ys = series("split_w", e, "writers", "mlookups_s")
    base[e] = dict(zip(xs, ys))
    if not xs:
        continue
    ax2.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=2.1, ms=6.5,
             label=LABEL[e], alpha=0.94)
# annotate the per-node lead over global-txn where both exist
for w in sorted(base["txn-pernode"]):
    if w in base["txn-global"] and base["txn-global"][w] > 0:
        r = base["txn-pernode"][w] / base["txn-global"][w]
        if r >= 1.15:
            ax2.annotate(f"{r:.1f}×", (w, base["txn-pernode"][w]),
                         textcoords="offset points", xytext=(0, 8),
                         ha="center", fontsize=8, color=COLOR["txn-pernode"],
                         fontweight="bold")
if ax2.has_data():
    ax2.set_xscale("log", base=2); ax2.set_yscale("log")
ax2.set_title("Role-split: 32 dedicated readers + W writers — reader Mops/s vs W\n"
              "ISOLATES the reader path: global bracket contends one whole-tree\n"
              "cacheline; per-node host counter does not (× = per-node ÷ global)",
              fontsize=9.5)
ax2.set_xlabel("concurrent writer threads (renaming)   (log)")
ax2.set_ylabel("reader lookup Mops/s   (higher better, log)")

# ---- Panel 3: ROLE-SPLIT reader scaling at fixed writer load ----------------
for e in ENGINES:
    xs, ys = series("split_scale", e, "readers", "mlookups_s")
    if not xs:
        continue
    ax3.plot(xs, ys, color=COLOR[e], marker=MARKER[e], lw=1.9, ms=6,
             label=LABEL[e], alpha=0.92)
if ax3.has_data():
    ax3.set_xscale("log", base=2); ax3.set_yscale("log")
ax3.set_title("Role-split reader scaling — 8 writers fixed, sweep readers\n"
              "reader Mops/s vs reader count under a constant rename load\n"
              "(per-node keeps scaling; the global bracket saturates)",
              fontsize=9.5)
ax3.set_xlabel("dedicated reader threads   (log)")
ax3.set_ylabel("reader lookup Mops/s   (higher better, log)")

for ax in (ax1, ax2, ax3):
    ax.grid(alpha=0.3, ls=":")
    if ax.get_legend_handles_labels()[0]:
        ax.legend(fontsize=7.5, loc="best")

fig.suptitle("Userspace dcache — does urcu-txn dissolve rename_lock + d_seq?   "
             "seqlock vs txn(global gen) vs txn(per-node host gen)   ·   2×96-core EPYC",
             fontsize=12)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
