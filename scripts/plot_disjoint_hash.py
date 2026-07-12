#!/usr/bin/env python3
"""
The DISJOINT-write-set hint on the urcu-txn 3-hash.

A mutator whose write set touches distinct slots -- a hash add/remove over
distinct buckets -- can take the age-0 blind append (skip the O(nr) reconcile
find on every store) WITHOUT the read-your-own-writes Bloom, via
urcu_txn_declare_disjoint().  It then maintains no Bloom filter at all: no
per-store hash-and-set, no per-load test, nothing to zero per attempt.

Two curves, both from ONE binary (spinlatch + AGE0 flat install + k=3/1024-bit
filter compiled in), differing ONLY by the runtime flag, so there is no
code-layout confound:
  disj : --disjoint 1   -- blind append, NO Bloom (urcu_txn_declare_disjoint).
  ryw1 : --disjoint 0   -- the engine's default read-your-own-writes: Bloom +
                           blind append.  The filter is pure cost here (the
                           disjoint write set has zero true WAW), taxing low
                           contention and, at high core counts, adding
                           counterproductive false-positive escalations.
The right panel is disj / ryw1: what declaring the write set disjoint buys over
the default.

(The invisible-writes "find" mode -- ryw off, a reconcile find on every store --
is retired; read-your-own-writes is now unconditional, so it is no longer a curve
here.  Older CSVs still carrying a "find" column plot fine -- it is just ignored.)
"""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.environ.get("CSV", os.path.join(HERE, "disjoint_hash.csv"))
OUT = os.environ.get("OUT", os.path.join(HERE, "..", "figures", "disjoint_hash.png"))

BASE = "ryw1"			# the default read-your-own-writes, the ratio baseline
COLOR = {"disj": "#009E73", "ryw1": "#0072B2"}
MARK = {"disj": "^", "ryw1": "o"}
LABEL = {
    "disj": "declare_disjoint: blind-append, NO Bloom  (the fast knob)",
    "ryw1": "default ryw (Bloom + blind-append)  — the baseline",
}
rows = list(csv.DictReader(open(CSV)))


def series(v):
    acc = {int(r["cores"]): float(r["median_mmoves"]) for r in rows if r["variant"] == v}
    xs = sorted(acc)
    return xs, [acc[x] for x in xs]


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
base = dict(zip(*series(BASE)))
for v in ("disj", "ryw1"):
    xs, ys = series(v)
    if not xs:
        continue
    ax1.plot(xs, ys, color=COLOR[v], marker=MARK[v], lw=2, ms=6, label=LABEL[v], alpha=0.92)
    if v != BASE:
        ax2.plot(xs, [ys[i] / base[xs[i]] for i in range(len(xs)) if xs[i] in base],
                 color=COLOR[v], marker=MARK[v], lw=2, ms=6, label=LABEL[v], alpha=0.92)

ax1.set_ylabel("Mmoves/s   (higher is better)")
ax1.set_title("throughput", fontsize=10)
ax2.axhline(1.0, color=COLOR[BASE], lw=1.0, ls="--", alpha=0.7)
ax2.set_ylabel("ratio to default ryw   (>1 = disjoint faster)")
ax2.set_title("÷ default ryw", fontsize=10)
for ax in (ax1, ax2):
    ax.set_xscale("log", base=2)
    xt = sorted({int(r["cores"]) for r in rows})
    ax.set_xticks(xt)
    ax.set_xticklabels(xt)
    ax.set_xlabel("updater cores")
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8, loc="best")

# Headline: the disj/ryw1 ratio range, computed from the plotted data.
dx, dy = series("disj")
ratios = [dy[i] / base[dx[i]] for i in range(len(dx)) if dx[i] in base]
head = ("declaring the write set disjoint drops the Bloom entirely: "
        "%+.0f%% to %+.0f%% vs the default across %d–%d cores"
        % (100 * (min(ratios) - 1), 100 * (max(ratios) - 1), min(dx), max(dx))
        if ratios else "")
fig.suptitle("Disjoint-write-set hint — urcu-txn 3-hash (2×96-core EPYC, spinlatch, jemalloc)\n"
             + head, fontsize=10)
fig.tight_layout(rect=[0, 0, 1, 0.90])
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
