#!/usr/bin/env python3
"""Plot scripts/dcache_namewidth.csv -> figures/dcache_namewidth.png.

The matched-name-width CONTROL for the mark arm.  DC_NAME_MAX is 40 on the mark
arm and 32 everywhere else -- and while the DENTRY is unaffected (sizeof 168,
d_hash @56, both measured), `struct qstr` is shared with `struct dc_path`, so
the width also makes the harness path object 20% bigger (964 -> 1156 B), the
per-component path copy 48 instead of 40 bytes, and the precomputed leaf-qstr
table 20% bigger.  Those are on the reader's hot path and are not the mechanism
under test.

Three arms per engine separate the two changes:
  <e>              shipped, DC_NAME_MAX 40
  <e>-w32          NAME_MAX=32 + 8 B pad -> dentry byte-identical, harness path
                   matched to the baselines.  THE CONTROL.
  <e>-w32-shrink   NAME_MAX=32, no pad -> dentry also 8 B smaller.

Every run is plotted, not just the best: this is a null-result test, and "within
noise" is a claim about the spread.  Lines are the median, bands are min..max.

Read it as: -w32 on top of its shipped arm => the width was never a confound and
the REVIEW caveat retires with evidence.  A visible gap => the published mark
reader numbers were paying a harness tax and the S3/S4 tables need this column.
The readdir panel is the sensitive one -- dc_readdir hands out a qstr per DIRENT
rather than per path component.
"""
import csv, collections, os, statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_namewidth.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures",
                                  "dcache_namewidth.png"))

STYLE = {  # arm -> (color, linestyle, label)
    "txn-pernode":           ("#009E73", "-",  "txn PER-NODE (width 32, the reference)"),
    "txn-mark":              ("#CC79A7", "-",  "txn MARK (shipped, width 40)"),
    "txn-mark-w32":          ("#CC79A7", "--", "txn MARK w32 + pad (control)"),
    "txn-mark-w32-shrink":   ("#CC79A7", ":",  "txn MARK w32, no pad"),
    "bucketlock":            ("#000000", "-",  "bucket lock (shipped, width 40)"),
    "bucketlock-w32":        ("#000000", "--", "bucket lock w32 + pad (control)"),
    "bucketlock-w32-shrink": ("#000000", ":",  "bucket lock w32, no pad"),
}

rows = [r for r in csv.DictReader(open(CSV)) if r["conserved"] == "OK"]
lk = collections.defaultdict(lambda: collections.defaultdict(list))
rdd = collections.defaultdict(list)
for r in rows:
    if r["panel"] == "lookup":
        lk[r["arm"]][int(r["readers"])].append(float(r["mlookups_s"]))
    else:
        rdd[r["arm"]].append(float(r["mrenames_s"]))   # Mdirents/s column
arms = [a for a in STYLE if a in lk or a in rdd]

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6.0),
                               gridspec_kw={"width_ratios": [2, 1]})

for a in arms:
    if a not in lk:
        continue
    color, ls, lab = STYLE[a]
    xs = sorted(lk[a])
    med = [statistics.median(lk[a][x]) for x in xs]
    lo = [min(lk[a][x]) for x in xs]
    hi = [max(lk[a][x]) for x in xs]
    ax1.plot(xs, med, ls, color=color, marker="o", ms=4, label=lab)
    ax1.fill_between(xs, lo, hi, color=color, alpha=0.12, linewidth=0)
ax1.set_xlabel("reader threads")
ax1.set_ylabel("reader  Mlookups/s")
ax1.set_ylim(bottom=0)
ax1.set_xlim(left=0)
ax1.grid(alpha=0.25)
ax1.set_axisbelow(True)
ax1.set_title("Lookup scaling — width is paid per path COMPONENT", fontsize=11)
ax1.legend(fontsize=8, frameon=False)

xs = [a for a in arms if a in rdd]
ax2.bar(range(len(xs)), [statistics.median(rdd[a]) for a in xs],
        yerr=[[statistics.median(rdd[a]) - min(rdd[a]) for a in xs],
              [max(rdd[a]) - statistics.median(rdd[a]) for a in xs]],
        color=[STYLE[a][0] for a in xs], capsize=3, edgecolor="white")
ax2.set_xticks(range(len(xs)))
ax2.set_xticklabels(xs, rotation=30, ha="right", fontsize=8)
ax2.set_ylabel("readdir  Mdirents/s")
ax2.set_ylim(bottom=0)
ax2.grid(axis="y", alpha=0.25)
ax2.set_axisbelow(True)
ax2.set_title("readdir — width is paid per DIRENT", fontsize=11)

fig.suptitle("Matched name-width control: is the mark arm's reader number "
             "the mechanism, or the harness path object?", fontsize=12)
fig.tight_layout(rect=(0, 0, 1, 0.95))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fig.savefig(OUT, dpi=140)
print("wrote", OUT)
