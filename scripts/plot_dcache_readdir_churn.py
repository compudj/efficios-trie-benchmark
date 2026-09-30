#!/usr/bin/env python3
"""
Directory LISTING (readdir) vs CREATE/DELETE (add/unlink) on a hot directory set.
Both ops contend on the same child list; the figure asks the two questions the
kernel's per-directory lock forces a trade-off between:

  left  (list_vs_churn):  32 readdir readers fixed, sweep churn writers.
        Does directory LISTING survive concurrent create/delete?  (y = Mdirents/s)
  right (churn_vs_list):  8 churn writers fixed, sweep readdir readers.
        Does CREATE/DELETE survive concurrent listing?            (y = Mchurn/s)

The seqlock baseline guards the child list with a per-directory lock whose
BIAS decides the winner.  Three locks are plotted: the glibc pthread_rwlock
reader-preferring (listing wins, churn starves) and writer-preferring (churn
wins, listing starves) -- the shaded band between them -- and the VENDORED Linux
kernel rw_semaphore, the lock the kernel actually uses for inode->i_rwsem and
the seqlock baseline's default everywhere else.

Measured 2026-09-30: the kernel rwsem lands INSIDE the band for churn under
listing, but BELOW BOTH glibc biases for listing under paced churn from 8
writers up (listing readers queue behind the writers; its absolute height also
carries the userspace port's overhead, a naive wait_lock + futex).  The txn /
bucket-lock engines take no per-dir lock at all (lock-free RCU readdir + a
bit-lock add/unlink splice), so they escape the trade-off: listing 3.3-13x the
kernel rwsem, churn at 184 listers 3.22 against its 0.40 Mops/s.

Data: scripts/dcache_readdir_churn.csv (best-of-5, conservation-gated).  Linear
axes.  2x96-core EPYC.
"""
import csv, collections, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FixedFormatter, FuncFormatter
import dcache_plotlib as dp

_num = FuncFormatter(lambda v, _: f"{v:g}")

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_readdir_churn.csv")
OUT = os.environ.get("OUT",
                     os.path.join(HERE, os.pardir, "figures",
                                  "dcache_readdir_churn.png"))

rows = dp.load(CSV)

COLOR = {"seqlock-rp": "#D55E00", "seqlock-wp": "#E69F00",
         "seqlock-krwsem": "#7B3294", "txn-mark": "#CC79A7", "bucketlock": "#000000"}
MARK = {"seqlock-rp": "s", "seqlock-wp": "v", "seqlock-krwsem": "*",
        "txn-mark": "D", "bucketlock": "X"}
LABEL = {
    "seqlock-rp": "seqlock — per-dir rwlock\nreader-pref (glibc default)",
    "seqlock-wp": "seqlock — per-dir rwlock\nwriter-pref (PREFER_WRITER)",
    "seqlock-krwsem": "seqlock — VENDORED Linux\nkernel rw_semaphore (fair)",
    "txn-mark": "urcu-txn — deletion MARK\n(lock-free RCU readdir)",
    "bucketlock": "bucket lock + SW txn\n(lock-free readdir, bit-lock churn)",
}
# krwsem drawn thicker: it is the faithful fair point that should sit INSIDE the
# reader-pref<->writer-pref band.  (Its absolute height also carries the userspace
# port's overhead -- naive wait_lock + futex -- so the same-glibc-lock bracket
# stays the clean quantitative bound; krwsem confirms the fair POLICY is between.)
ORDER = ("bucketlock", "txn-mark", "seqlock-krwsem", "seqlock-wp", "seqlock-rp")


def series(panel, eng, xcol, ycol):
    """Paced (or unpaced) points only; see full_series for the SHORT ones."""
    return full_series(panel, eng, xcol, ycol)[:2]


def full_series(panel, eng, xcol, ycol):
    return dp.series(rows, xcol, ycol, panel=panel, engine=eng)


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
    dp.plot_series(ax1, full_series("list_vs_churn", e, "writers", "mreaddir_s"),
                   COLOR[e], MARK[e], LABEL[e], lw=2.2, alpha=1.0)
# fair-rwsem bracket: shade between the two seqlock biases
rp = dict(zip(*series("list_vs_churn", "seqlock-rp", "writers", "mreaddir_s")))
wp = dict(zip(*series("list_vs_churn", "seqlock-wp", "writers", "mreaddir_s")))
common = sorted(set(rp) & set(wp))
if common:
    ax1.fill_between(common, [wp[x] for x in common], [rp[x] for x in common],
                     color="#D55E00", alpha=0.10, zorder=0,
                     label="between the two glibc rwlock biases")
linx(ax1, 48, [1, 8, 16, 24, 32, 40, 48])
ax1.set_title("Does directory LISTING survive concurrent create/delete?\n"
              "32 readdir readers, W churn writers PACED to 12.5k ops/s each\n"
              "(the same load for every engine; 16 hot dirs)\n"
              "reader-pref glibc keeps listing fast but (right) starves churn;\n"
              "the kernel rwsem lists slowest; the lock-free arms pay no bias",
              fontsize=9.5)
ax1.set_xlabel("concurrent create/delete (churn) writer threads")
ax1.set_ylabel("directory entries listed / s   (Mdirents/s, higher is better)")

# ---- Panel 2: churn throughput vs readdir readers (8 churn writers) -----------
for e in ORDER:
    dp.plot_series(ax2, full_series("churn_vs_list", e, "readers", "mchurn_s"),
                   COLOR[e], MARK[e], LABEL[e], lw=2.2, alpha=1.0)
rp = dict(zip(*series("churn_vs_list", "seqlock-rp", "readers", "mchurn_s")))
wp = dict(zip(*series("churn_vs_list", "seqlock-wp", "readers", "mchurn_s")))
common = sorted(set(rp) & set(wp))
if common:
    ax2.fill_between(common, [rp[x] for x in common], [wp[x] for x in common],
                     color="#D55E00", alpha=0.10, zorder=0,
                     label="between the two glibc rwlock biases")
xmax = max((int(r["readers"]) for r in rows if r["panel"] == "churn_vs_list"),
           default=184)
linx(ax2, xmax + 4, [2, 32, 64, 96, 128, 160, xmax])
ax2.set_title("Does CREATE/DELETE survive concurrent listing?\n"
              "8 churn writers FLAT OUT (the writers are the measurement),\n"
              "sweep readdir readers (16 hot dirs)\n"
              "reader-pref glibc COLLAPSES (listers starve the writers), the\n"
              "kernel rwsem sits between; bit-lock add/unlink never blocks on a reader",
              fontsize=9.5)
ax2.set_xlabel("concurrent readdir (directory-listing) reader threads")
ax2.set_ylabel("create+delete / s   (Mchurn/s, higher is better)")

for ax in (ax1, ax2):
    ax.grid(alpha=0.3, ls=":")
    dp.legend(ax, fontsize=8, loc="best")

fig.suptitle("Userspace dcache — directory listing vs create/delete on a HOT dir: "
             "the seqlock per-dir lock forces a reader/writer bias trade-off the "
             "lock-free engines escape   ·   2×96-core EPYC", fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.94])
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
