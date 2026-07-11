#!/usr/bin/env python3
"""
The DISJOINT-write-set hint on the urcu-txn 3-hash.

A mutator whose write set touches distinct slots -- a hash add/remove over
distinct buckets -- can take the age-0 blind append (skip the O(nr) reconcile
find on every store) WITHOUT enabling read-your-own-writes, via
urcu_txn_declare_disjoint().  It then maintains no Bloom filter at all: no
per-store hash-and-set, no per-load test, nothing to zero per attempt.

Three curves, all from ONE esc-smart binary (spinlatch + AGE0 flat install +
k=3/1024-bit filter compiled in), differing ONLY by runtime flag, so there is
no code-layout confound:
  find : --ryw 0             -- blind append OFF, so every store runs the
                               reconcile find; this is today's ryw-off default.
  disj : --disjoint 1 --ryw 0 -- blind append, NO Bloom (the new hint).
  ryw1 : --ryw 1             -- Bloom + blind append; the filter is pure cost
                               here (the disjoint write set has zero true WAW),
                               taxing low contention and, at 128-192 cores,
                               adding counterproductive false-positive escalations.

The disjoint hint beats the reconcile-find path at EVERY core count (+3.6% at
4 cores to +20.6% at 128) and beats ryw-on everywhere except a 2.5% dip at 64
cores -- the residual microarchitectural incidental that the interleaved,
data-dependent Bloom RMW happens to buy at that one contention resonance, and
which a plain spin or the find cannot reproduce.
"""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.environ.get("CSV", os.path.join(HERE, "disjoint_hash.csv"))
OUT = os.environ.get("OUT", os.path.join(HERE, "..", "figures", "disjoint_hash.png"))

COLOR = {"find": "#D55E00", "disj": "#009E73", "ryw1": "#0072B2"}
MARK = {"find": "v", "disj": "^", "ryw1": "o"}
LABEL = {
    "find": "ryw off (reconcile find) — today's disjoint default",
    "disj": "declare_disjoint: blind-append, NO Bloom  (new)",
    "ryw1": "ryw on (Bloom + blind-append) — low-contention tax",
}
rows = list(csv.DictReader(open(CSV)))


def series(v):
    acc = {int(r["cores"]): float(r["median_mmoves"]) for r in rows if r["variant"] == v}
    xs = sorted(acc)
    return xs, [acc[x] for x in xs]


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
base = dict(zip(*series("find")))
for v in ("find", "disj", "ryw1"):
    xs, ys = series(v)
    ax1.plot(xs, ys, color=COLOR[v], marker=MARK[v], lw=2, ms=6, label=LABEL[v], alpha=0.92)
    if v != "find":
        ax2.plot(xs, [ys[i] / base[xs[i]] for i in range(len(xs))],
                 color=COLOR[v], marker=MARK[v], lw=2, ms=6, label=LABEL[v], alpha=0.92)

ax1.set_ylabel("Mmoves/s   (higher is better)")
ax1.set_title("throughput", fontsize=10)
ax2.axhline(1.0, color="#D55E00", lw=1.0, ls="--", alpha=0.7)
ax2.set_ylabel("ratio to ryw-off find   (>1 = faster)")
ax2.set_title("÷ ryw-off find", fontsize=10)
for ax in (ax1, ax2):
    ax.set_xscale("log", base=2)
    xt = sorted({int(r["cores"]) for r in rows})
    ax.set_xticks(xt)
    ax.set_xticklabels(xt)
    ax.set_xlabel("updater cores")
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8, loc="best")

fig.suptitle("Disjoint-write-set hint — urcu-txn 3-hash (2×96-core EPYC, spinlatch, jemalloc)\n"
             "a hash API over distinct buckets blind-appends at age 0 without the RYW Bloom: "
             "beats ryw-off find everywhere, beats ryw-on except the 64c band",
             fontsize=10)
fig.tight_layout(rect=[0, 0, 1, 0.90])
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
