#!/usr/bin/env python3
"""
Directory LISTING (readdir) vs CREATE/DELETE (add/unlink) on a hot directory set.
Both ops contend on the same child list; the figure asks the two questions the
kernel's per-directory lock forces a trade-off between:

  left  (list_vs_churn):  32 readdir readers fixed, sweep churn writers.
        Does directory LISTING survive concurrent create/delete?  (y = Mdirents/s)
  right (churn_vs_list):  8 churn writers fixed, sweep readdir readers.
        Does CREATE/DELETE survive concurrent listing?            (y = Mchurn/s)

The seqlock baseline guards the child list with a per-directory pthread_rwlock,
whose BIAS decides the winner.  The glibc default is reader-preferring (listing
wins, churn starves); PREFER_WRITER_NONRECURSIVE flips it (churn wins, listing
starves).  The kernel uses a FAIR rw_semaphore (inode->i_rwsem), so its result
lies IN BETWEEN -- shown as the shaded band between the two seqlock biases.  The
txn / bucket-lock engines take no per-dir rwlock (lock-free RCU readdir + a
bit-lock add/unlink splice), so they escape the trade-off: high on BOTH axes.

Data: scripts/dcache_readdir_churn.csv (best-of-5, conservation-gated).  Linear
axes.  2x96-core EPYC.
"""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FixedFormatter, FuncFormatter

_num = FuncFormatter(lambda v, _: f"{v:g}")

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_readdir_churn.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures",
                                  "dcache_readdir_churn.png"))

rows = [r for r in csv.DictReader(open(CSV)) if r["conserved"] == "OK"]

COLOR = {"seqlock-rp": "#D55E00", "seqlock-wp": "#E69F00",
         "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARK = {"seqlock-rp": "s", "seqlock-wp": "v", "txn-mark": "D", "bucketlock": "X"}
LABEL = {
    "seqlock-rp": "seqlock — per-dir rwlock\nreader-pref (glibc default)",
    "seqlock-wp": "seqlock — per-dir rwlock\nwriter-pref (PREFER_WRITER)",
    "txn-mark": "urcu-txn — deletion MARK\n(lock-free RCU readdir)",
    "bucketlock": "bucket lock + SW txn\n(lock-free readdir, bit-lock churn)",
}
ORDER = ("bucketlock", "txn-mark", "seqlock-wp", "seqlock-rp")


def series(panel, eng, xcol, ycol):
    acc = {}
    for r in rows:
        if r["panel"] == panel and r["engine"] == eng:
            acc[int(r[xcol])] = float(r[ycol])
    xs = sorted(acc)
    return xs, [acc[x] for x in xs]


def linx(ax, xmax, ticks):
    ax.set_xlim(0, xmax)
    ax.set_ylim(bottom=0)
    ticks = [t for t in ticks if t <= xmax]
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
    ax.yaxis.set_major_formatter(_num)


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6.3))

# ---- Panel 1: readdir throughput vs churn writers (32 readdir readers) --------
for e in ORDER:
    xs, ys = series("list_vs_churn", e, "writers", "mreaddir_s")
    if xs:
        ax1.plot(xs, ys, color=COLOR[e], marker=MARK[e], lw=2.2, ms=6.5,
                 label=LABEL[e])
# fair-rwsem bracket: shade between the two seqlock biases
rp = dict(zip(*series("list_vs_churn", "seqlock-rp", "writers", "mreaddir_s")))
wp = dict(zip(*series("list_vs_churn", "seqlock-wp", "writers", "mreaddir_s")))
common = sorted(set(rp) & set(wp))
if common:
    ax1.fill_between(common, [wp[x] for x in common], [rp[x] for x in common],
                     color="#D55E00", alpha=0.10, zorder=0,
                     label="kernel fair-rwsem lies in this band")
linx(ax1, 48, [1, 8, 16, 24, 32, 40, 48])
ax1.set_title("Does directory LISTING survive concurrent create/delete?\n"
              "32 readdir readers fixed, sweep churn writers (16 hot dirs)\n"
              "reader-pref seqlock keeps listing FAST but (right panel) starves\n"
              "churn; the lock-free arms list fast with no bias to pay",
              fontsize=9.5)
ax1.set_xlabel("concurrent create/delete (churn) writer threads")
ax1.set_ylabel("directory entries listed / s   (Mdirents/s, higher is better)")

# ---- Panel 2: churn throughput vs readdir readers (8 churn writers) -----------
for e in ORDER:
    xs, ys = series("churn_vs_list", e, "readers", "mchurn_s")
    if xs:
        ax2.plot(xs, ys, color=COLOR[e], marker=MARK[e], lw=2.2, ms=6.5,
                 label=LABEL[e])
rp = dict(zip(*series("churn_vs_list", "seqlock-rp", "readers", "mchurn_s")))
wp = dict(zip(*series("churn_vs_list", "seqlock-wp", "readers", "mchurn_s")))
common = sorted(set(rp) & set(wp))
if common:
    ax2.fill_between(common, [rp[x] for x in common], [wp[x] for x in common],
                     color="#D55E00", alpha=0.10, zorder=0,
                     label="kernel fair-rwsem lies in this band")
xmax = max((int(r["readers"]) for r in rows if r["panel"] == "churn_vs_list"),
           default=184)
linx(ax2, xmax + 4, [2, 32, 64, 96, 128, 160, xmax])
ax2.set_title("Does CREATE/DELETE survive concurrent listing?\n"
              "8 churn writers fixed, sweep readdir readers (16 hot dirs)\n"
              "reader-pref seqlock COLLAPSES (listing readers starve the writers);\n"
              "bucket-lock's bit-lock add/unlink never blocks on a reader",
              fontsize=9.5)
ax2.set_xlabel("concurrent readdir (directory-listing) reader threads")
ax2.set_ylabel("create+delete / s   (Mchurn/s, higher is better)")

for ax in (ax1, ax2):
    ax.grid(alpha=0.3, ls=":")
    ax.legend(fontsize=8, loc="best")

fig.suptitle("Userspace dcache — directory listing vs create/delete on a HOT dir: "
             "the seqlock rwlock forces a reader/writer bias trade-off the "
             "lock-free engines escape   ·   2×96-core EPYC", fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
