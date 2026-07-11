#!/usr/bin/env python3
"""
Age-0/age-1 optimistic RYW escalation vs the baseline RYW path, on the urcu-txn
3-skiplist, all curves on the current spinlatch engine (NO_HELP + NO_STEAL).  The
age-0 fast path skips the write-set find and the record sort and, on any Bloom
coincidence, ESCALATES (aborts, re-runs at age 1 with full Bloom+find+sort).

The determinant is write-set DISJOINTNESS, and the two panels bracket it:
  - BATCHED (movesper 3): adjacent keys share a predecessor, so every commit has
    genuine read-your-own-writes -> age 0 pre-aborts in the descent on ~100% of
    commits, regardless of dataset size or filter quality.  It LOSES 16-37%.
  - SPARSE (movesper 1, RYW forced on): the write-set is disjoint, genuine RYW is
    ~0, so with a good filter age 0 almost never escalates -- it reaches install
    and skips the sort + blocking install.  It WINS 1-6% with the k=3 filter; the
    k=1 filter false-positives on ~80% of commits and loses 30-40%.

So the scheme is not "always loses on the skiplist": on a DISJOINT skiplist it
wins, exactly as it does on the disjoint 3-hash (see age_escalate_hash).  It
loses only where adjacency FORCES genuine RYW.  The filter must be good either
way -- a false positive escalates just like a real coincidence.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.environ.get("CSV", os.path.join(HERE, "age_escalate_sweep.csv"))
OUT = os.environ.get("OUT", os.path.join(HERE, "..", "figures",
                                          "age_escalate_skiplist.png"))

COLOR = {"baseline": "#0072B2", "esc-dumb": "#D55E00", "esc-smart": "#009E73"}
MARKER = {"baseline": "o", "esc-dumb": "v", "esc-smart": "^"}
LABEL = {
    "baseline":  "baseline RYW (Bloom+find every load)",
    "esc-dumb":  "age-0 escalate, 64-bit k=1 filter",
    "esc-smart": "age-0 escalate, 1024-bit k=3 filter",
}
rows = list(csv.DictReader(open(CSV)))


def series(panel, eng):
    acc = {}
    for r in rows:
        if r["panel"] == panel and r["engine"] == eng and float(r["mmoves_s"]) > 0:
            acc[int(r["cores"])] = float(r["mmoves_s"])
    xs = sorted(acc)
    return xs, [acc[x] for x in xs]


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

panels = [
    (ax1, "batched",
     "BATCHED  (movesper 3)  —  adjacency-forced ~100% genuine RYW\n"
     "age 0 pre-aborts in the descent; loses regardless of filter",
     [1, 4, 16, 64, 128, 192]),
    (ax2, "sparse",
     "SPARSE  (movesper 1, disjoint)  —  ~0% genuine RYW\n"
     "age 0 reaches install; k=3 filter WINS, k=1 false-positives and loses",
     [1, 4, 16, 64, 128, 192]),
]

for ax, panel, title, xticks in panels:
    base = dict(zip(*series(panel, "baseline")))
    for eng in ("baseline", "esc-dumb", "esc-smart"):
        xs, ys = series(panel, eng)
        ax.plot(xs, ys, color=COLOR[eng], marker=MARKER[eng], lw=1.9, ms=6,
                label=LABEL[eng], alpha=0.92)
        if eng != "baseline":
            # annotate the ratio to baseline at the last point
            x, y = xs[-1], ys[-1]
            ax.annotate(f"{y/base[x]:.2f}×", (x, y), textcoords="offset points",
                        xytext=(4, -2 if eng == "esc-dumb" else 8),
                        fontsize=8.5, color=COLOR[eng], fontweight="bold")
    ax.set_xscale("log", base=2)
    ax.set_xticks(xticks)
    ax.set_xticklabels(xticks)
    ax.set_title(title, fontsize=10)
    ax.set_xlabel("updater cores")
    ax.set_ylabel("Mmoves/s   (higher is better)")
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8, loc="upper left")

# escalation-rate callouts (measured single-thread, no contention)
ax2.annotate("escalation rate (nu=1):\n"
             "64-bit k=1:  80.7%\n"
             "1024-bit k=3:  0.10%",
             xy=(0.97, 0.03), xycoords="axes fraction", ha="right", va="bottom",
             fontsize=7.5, color="#444444",
             bbox=dict(boxstyle="round", fc="#f4f4f4", ec="#cccccc"))
ax1.annotate("escalation rate (nu=1):\n"
             "≈100% (raw 9.2 + waw 5.2\nrecorded slots per commit)",
             xy=(0.97, 0.03), xycoords="axes fraction", ha="right", va="bottom",
             fontsize=7.5, color="#444444",
             bbox=dict(boxstyle="round", fc="#f4f4f4", ec="#cccccc"))

fig.suptitle("Age-0/age-1 optimistic RYW escalation vs baseline — urcu-txn 3-skiplist "
             "(2×96-core EPYC, spinlatch engine, jemalloc)\n"
             "write-set disjointness decides it: age 0 wins on the disjoint (sparse) "
             "skiplist and loses where adjacency forces genuine RYW (batched)",
             fontsize=11)
fig.tight_layout(rect=[0, 0, 1, 0.93])
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
