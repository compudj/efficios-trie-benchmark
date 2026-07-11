#!/usr/bin/env python3
"""
Positive half of the age-0/age-1 optimistic RYW escalation study, on the DISJOINT
urcu-txn 3-hash workload (the opposite of the adjacency-forced 3-skiplist).

All curves run on the current engine -- the spinlatch install (NO_HELP + NO_STEAL,
decide/settle CAS -> release store).  The write-set is disjoint (8 consecutive
keys hit 8 distinct buckets), so genuine read-your-own-writes is ~0 and every
escalation is a Bloom false positive:
  - esc-dumb  (64-bit k=1) trips on ~all commits -- contiguous calloc'd bucket-head
    addresses collide in a single-bit filter -- so it escalates everything and loses.
  - esc-smart (1024-bit k=3) drives false positives toward zero, so age 0 almost
    always reaches install and skips the O(nr) record sort, the O(nr) reconcile
    find (the write-set scan on every store), and pays a FLAT install.

The reconcile-find skip is the age-0 blind append now folded into AGE_ESCALATE:
at age 0 esc_pending already discards any same-slot coincidence before install,
so the find is redundant and the store just appends.  That scan was the dominant
age-0 self-time and is contention-independent, so it lifts the WHOLE curve.

The install itself is now FLAT under the spinlatch + AGE0_TRYLATCH: one try-CAS
per record, OR the outcomes, bail at the first conflict, decide arithmetically --
no install latch, no self-settle, no pre-CAS slot load (the latch machinery is
dead when no helper/thief can touch a record).  The point is not "no branches"
but no MISPREDICTED branches: the latched install's per-record is-proxy/slot-
still-old tests are data-dependent on a freshly loaded slot value, so they
mispredict exactly when a slot is contended; the flat install OR-accumulates the
CAS outcome and its only branch is a well-predicted after-CAS bail.  Adds +2-10%
over the latched age-0 install across the whole range.

Left: absolute throughput.  Right: ratio to baseline.  Age 0 now wins across the
entire range -- +5 to +15% -- rather than fading to parity at high core counts as
it did before the blind append: it holds +6-7% at 64-192 where the sort/find skip
still pays even as install collisions reintroduce some escalation.
"""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.environ.get("CSV", os.path.join(HERE, "age_escalate_hash.csv"))
OUT = os.environ.get("OUT", os.path.join(HERE, "..", "figures",
                                         "age_escalate_hash.png"))

COLOR = {"baseline": "#0072B2", "esc-dumb": "#D55E00", "esc-smart": "#009E73"}
MARKER = {"baseline": "o", "esc-dumb": "v", "esc-smart": "^"}
LABEL = {
    "baseline":  "baseline RYW (Bloom+find every load)",
    "esc-dumb":  "age-0 escalate, 64-bit k=1 filter",
    "esc-smart": "age-0 escalate, 1024-bit k=3 filter",
}
rows = list(csv.DictReader(open(CSV)))


def series(eng):
    acc = {int(r["cores"]): float(r["mmoves_s"]) for r in rows
           if r["engine"] == eng and float(r["mmoves_s"]) > 0}
    xs = sorted(acc)
    return xs, [acc[x] for x in xs]


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
base = dict(zip(*series("baseline")))

for eng in ("baseline", "esc-dumb", "esc-smart"):
    xs, ys = series(eng)
    ax1.plot(xs, ys, color=COLOR[eng], marker=MARKER[eng], lw=1.9, ms=6,
             label=LABEL[eng], alpha=0.92)
    if eng != "baseline":
        ax2.plot(xs, [ys[i] / base[xs[i]] for i in range(len(xs))],
                 color=COLOR[eng], marker=MARKER[eng], lw=1.9, ms=6,
                 label=LABEL[eng], alpha=0.92)

ax1.set_ylabel("Mmoves/s   (higher is better)")
ax1.set_title("throughput", fontsize=10)
ax2.axhline(1.0, color="#0072B2", lw=1.0, ls="--", alpha=0.7)
ax2.set_ylabel("ratio to baseline   (>1 = age-0 wins)")
ax2.set_title("age-0 escalate ÷ baseline", fontsize=10)
for ax in (ax1, ax2):
    ax.set_xscale("log", base=2)
    xt = sorted({int(r["cores"]) for r in rows})
    ax.set_xticks(xt)
    ax.set_xticklabels(xt)
    ax.set_xlabel("updater cores")
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8, loc="best")

fig.suptitle("Age-0 optimistic RYW escalation — disjoint urcu-txn 3-hash "
             "(2×96-core EPYC, spinlatch, jemalloc)\n"
             "age 0 skips the reconcile find + sort and installs flat: "
             "+13–22% over baseline with a k=3 filter",
             fontsize=10)
fig.tight_layout(rect=[0, 0, 1, 0.90])
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
