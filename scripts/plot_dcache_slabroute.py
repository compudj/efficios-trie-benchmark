#!/usr/bin/env python3
"""Plot the descriptor-slab ROUTE axis -> figures/dcache_slabroute.png.

Every dcache mutation allocates one transaction descriptor, so past a few
writers this experiment measures liburcu's descriptor slab as much as the
dcache.  This is the figure for that axis: the same nine sweeps were run against
four slab routes, and these are the churn (add/unlink) curves, the workload that
is actually allocation-bound.

  scripts/dcache_churn.csv             default -- one call_rcu per descriptor
  scripts/dcache_churn_rseq.csv        + rseq per-cpu local lists
  scripts/dcache_churn_batch.csv       URCU_TXN_SLAB_BATCH -- retire by the batch
  scripts/dcache_churn_batch_rseq.csv  both

ENCODING.  The routes are a 2x2, so they are drawn as a 2x2 rather than as four
arbitrary colours: HUE carries batching (the axis that matters), LINE STYLE
carries rseq.  That keeps the categorical palette at two hues, which clears the
colourblind separation floor with room to spare -- four hues did not (the
default-vs-rseq pair collapsed to dE 5.4 under tritanopia, below even the
secondary-encoding floor).  seqlock and bucket-lock are context, in recessive
grey: neither touches this slab, so they are the "did the machine move" control.

LEFT panel is throughput.  RIGHT is the speedup of each route over the default,
for the three txn engines, which is where the consistency shows -- a real effect
holds across all three engines and all writer counts, and the control band says
what "no effect" looks like on the same axes.

NOT a dual-axis chart: footprint is a different measure on a different scale and
belongs in its own figure, not on a second y-axis here.
"""
import csv, collections, os, statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get("OUT", os.path.join(HERE, os.pardir, "figures",
                                         "dcache_slabroute.png"))

# hue = batching, linestyle = rseq.  Validated: worst normal-vision dE 20.7,
# worst CVD dE 14.3 (>= 15 / >= 8 required).
ROUTES = [
    ("",            "call_rcu per descriptor",      "#0072B2", "-"),
    ("_rseq",       "+ rseq per-cpu local lists",   "#0072B2", "--"),
    ("_batch",      "batch retirement",             "#D55E00", "-"),
    ("_batch_rseq", "batch + rseq",                 "#D55E00", "--"),
]
CONTEXT = "#AAAAAA"
TXN = ("txn-mark", "txn-pernode", "txn-global")
LEAD = "txn-mark"          # representative; the three track each other closely


def load(suffix):
    """-> {engine: {writers: mchurn_s}} from the writers-only churn panel."""
    p = os.path.join(HERE, f"dcache_churn{suffix}.csv")
    d = collections.defaultdict(dict)
    try:
        rows = list(csv.DictReader(open(p)))
    except FileNotFoundError:
        return d
    for r in rows:
        if r.get("conserved") != "OK" or r.get("panel") != "churn_w":
            continue
        if not r.get("mchurn_s"):
            continue
        d[r["engine"]][int(r["writers"])] = float(r["mchurn_s"])
    return d


data = {suf: load(suf) for suf, _, _, _ in ROUTES}
base = data[""]
if not base:
    raise SystemExit("no scripts/dcache_churn.csv -- run scripts/run_dcache_churn.sh")

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5.6))

# ---- left: throughput ----------------------------------------------------
for eng, lab in (("seqlock", "seqlock (no txn slab)"),
                 ("bucketlock", "bucket lock (no txn slab)")):
    if eng in base:
        xs = sorted(base[eng])
        ax1.plot(xs, [base[eng][x] for x in xs], "-", color=CONTEXT,
                 lw=1.6, marker="o", ms=4, zorder=1)
        ax1.annotate(lab, (xs[-1], base[eng][xs[-1]]), fontsize=8,
                     color="#555555", xytext=(-4, 6),
                     textcoords="offset points", ha="right")

for suf, lab, colr, ls in ROUTES:
    d = data.get(suf, {})
    if LEAD not in d:
        continue
    xs = sorted(d[LEAD])
    ax1.plot(xs, [d[LEAD][x] for x in xs], ls, color=colr, lw=2.0,
             marker="o", ms=5, zorder=3, label=lab)

ax1.set_xlabel("writer threads")
ax1.set_ylabel("churn  Mops/s  (add + unlink)")
ax1.set_title(f"Descriptor-slab route — churn throughput ({LEAD})", fontsize=11)
ax1.set_ylim(bottom=0)
ax1.set_xlim(left=0)
ax1.grid(alpha=0.25)
ax1.set_axisbelow(True)
ax1.legend(fontsize=8.5, frameon=False, loc="upper left")

# ---- right: speedup over the default route -------------------------------
# All three txn engines, so consistency is visible rather than asserted; the
# grey band is the same ratio computed on the engines that CANNOT be affected,
# i.e. the measurement's own noise floor.
ctl = []
for suf, lab, colr, ls in ROUTES[1:]:
    d = data.get(suf, {})
    for eng in ("seqlock", "bucketlock"):
        if eng in d and eng in base:
            ctl += [d[eng][w] / base[eng][w] for w in sorted(d[eng])
                    if base[eng].get(w)]
if ctl:
    lo, hi = min(ctl), max(ctl)
    ax2.axhspan(lo, hi, color=CONTEXT, alpha=0.35, zorder=0)
    ax2.annotate(f"control band ({lo:.2f}–{hi:.2f})\nengines with no txn slab",
                 (0.02, hi), xycoords=("axes fraction", "data"),
                 fontsize=8, color="#555555", va="bottom")

for suf, lab, colr, ls in ROUTES[1:]:
    d = data.get(suf, {})
    if not d:
        continue
    xs = sorted({w for e in TXN if e in d for w in d[e]})
    med = []
    for w in xs:
        v = [d[e][w] / base[e][w] for e in TXN
             if d.get(e, {}).get(w) and base.get(e, {}).get(w)]
        med.append(statistics.median(v) if v else float("nan"))
    ax2.plot(xs, med, ls, color=colr, lw=2.0, marker="o", ms=5,
             zorder=3, label=lab)

ax2.axhline(1.0, color="#333333", lw=1.0, zorder=2)
ax2.set_xlabel("writer threads")
ax2.set_ylabel("speedup over the default route  (x)")
ax2.set_title("Same, as a ratio — median of the three txn engines", fontsize=11)
ax2.set_xlim(left=0)
ax2.grid(alpha=0.25)
ax2.set_axisbelow(True)
ax2.legend(fontsize=8.5, frameon=False, loc="upper left")

fig.suptitle("The allocator underneath: how a committed descriptor is retired",
             fontsize=13)
fig.tight_layout(rect=(0, 0, 1, 0.94))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
