#!/usr/bin/env python3
"""One-page summary -> figures/dcache_bucketlock_summary.png.

The bucket lock + SW txn engine against the kernel-faithful seqlock baseline,
over every dcache benchmark: one row per benchmark, its ratio (bucket lock ÷
seqlock) at every point of that benchmark's sweep, on a log2 axis so 2x and
0.5x sit the same distance from parity.  A bar spans the row's min-max, a dot
marks each measured point, and the label names the detailed figure.

Rows are grouped by what they say -- on par (median within 5%), bucket lock
faster, bucket lock slower -- and the flat-out writer rows sit apart under
WRITER CAPACITY: they measure how fast each engine can rename, not what
renames cost its readers (every reader row compares the engines at the same
offered, paced rename load, and keeps only points where both engines' writers
sustained it).

Left out on purpose: the homogeneous-mix panel of dcache_s3.png (a mixed
lookup/rename throughput, writer-bound, and carrying the queued shard lock's
convoy caveat) and the name-width control (an engine-vs-itself A/B).

Every number is read from the sweep CSVs at plot time; the provenance id they
carry is printed in the figure.  A row whose median contradicts its group is
reported on stderr rather than silently drawn.

Env: OUT= to redirect.
"""
import csv, os, statistics, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FixedFormatter, NullLocator

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get("OUT", os.path.join(HERE, os.pardir, "figures",
                                         "dcache_bucketlock_summary.png"))
BL, SEQ = "bucketlock", "seqlock"
srcs = set()


def load(name):
    rows = [r for r in csv.DictReader(open(os.path.join(HERE, name + ".csv")))
            if r.get("conserved") == "OK"]
    srcs.update(r["src"] for r in rows if r.get("src"))
    return rows


def ratios(rows, key, xcol, ycol, match, base=SEQ, need_paced=True, xs=None):
    """bucket lock ÷ @base at each x of the rows equal to @match."""
    pts = defaultdict(dict)
    for r in rows:
        if any(r.get(k) != v for k, v in match.items()):
            continue
        pts[r[xcol]][r[key]] = r
    out = []
    for x, by in pts.items():
        if BL not in by or base not in by:
            continue
        if xs is not None and float(x) not in xs:
            continue
        b, s = by[BL], by[base]
        if need_paced and "SHORT" in (b.get("paced"), s.get("paced")):
            continue
        sv, bv = float(s[ycol]), float(b[ycol])
        if sv > 0 and bv > 0:
            out.append(bv / sv)
    return out


sweep = load("dcache_sweep")
churn = load("dcache_churn")
scal = load("dcache_churn_scaling")
height = load("dcache_height")
otax = load("dcache_optaxonomy")
otype = load("dcache_optype")
rdch = load("dcache_readdir_churn")

E = "engine"
# (group, label, figure, ratios)
ROWS = [
    ("par", "Lookups, 10k-30k renames/s (184 readers)", "s3",
     ratios(sweep, E, "rate_target", "mlookups_s", {"panel": "rate"},
            xs={10000.0, 30000.0})),
    ("par", "Lookups at rest, 2-184 readers", "idle",
     ratios(sweep, E, "readers", "mlookups_s", {"panel": "idle_scale"})),
    ("par", "Lookups during directory exchanges, every height (100k/s)",
     "height",
     ratios(height, E, "move_height", "mlookups_s", {"rate_target": "100000"})),
    ("par", "Lookups while writers create/delete (paced churn)", "churn",
     ratios(churn, E, "writers", "mlookups_s", {"panel": "churn_rd"}) +
     ratios(churn, E, "readers", "mlookups_s", {"panel": "churn_scale"})),
    ("par", "Create/delete, allocating, private dirs, 1-192 writers",
     "churn, churn_scaling",
     ratios(churn, E, "writers", "mchurn_s",
            {"panel": "churn_w", "mode": "alloc", "dirs": "private"}) +
     ratios(scal, E, "writers", "mchurn_s", {"share": "private"})),

    ("faster", "Lookups under heavy renames, 100k-300k renames/s", "s3",
     ratios(sweep, E, "rate_target", "mlookups_s", {"panel": "rate"},
            xs={100000.0, 300000.0})),
    ("faster", "Lookup scaling at 100k renames/s, 2-184 readers", "s3",
     ratios(sweep, E, "readers", "mlookups_s", {"panel": "split_scale"})),
    ("faster", "Lookups by file / directory rename type (100k/s)", "optype",
     ratios(otype, E, "readers", "mlookups_s", {"leaftype": "file"}) +
     ratios(otype, E, "readers", "mlookups_s", {"leaftype": "dir"})),
    ("faster", "Reverse walk (d_path) under renames", "dpath",
     ratios(sweep, E, "readers", "mlookups_s", {"panel": "dpath_scale"})),
    ("faster", "Reverse walk at rest", "idle",
     ratios(sweep, E, "readers", "mlookups_s", {"panel": "idle_dpath"})),
    ("faster", "readdir under renames", "readdir",
     ratios(sweep, E, "readers", "mlookups_s", {"panel": "readdir_scale"})),
    ("faster", "readdir at rest", "idle",
     ratios(sweep, E, "readers", "mlookups_s", {"panel": "idle_readdir"})),
    ("faster", "readdir against paced create/delete (listing rate)",
     "readdir_churn",
     ratios(rdch, E, "writers", "mreaddir_s", {"panel": "list_vs_churn"},
            base="seqlock-krwsem")),
    ("faster", "Create/delete against 2-184 listers (churn rate)",
     "readdir_churn",
     ratios(rdch, E, "readers", "mchurn_s", {"panel": "churn_vs_list"},
            base="seqlock-krwsem")),
    ("faster", "Create/delete in place, private dirs, 1-48 writers", "churn",
     ratios(churn, E, "writers", "mchurn_s",
            {"panel": "churn_w", "mode": "inplace", "dirs": "private"})),
    ("faster", "Create/delete in place, dirs shared across nodes",
     "churn_share",
     ratios(churn, E, "writers", "mchurn_s",
            {"panel": "churn_share", "mode": "inplace",
             "dirs": "cross-node"})),
    ("faster", "Create/delete, all writers share one dir set, 16-192 writers",
     "churn_scaling",
     ratios(scal, E, "writers", "mchurn_s", {"share": "all"},
            xs={16.0, 32.0, 48.0, 64.0, 96.0, 128.0, 160.0, 192.0})),

    ("slower", "Positive hits on objects being renamed (100k/s)", "hit",
     ratios(sweep, E, "readers", "mlookups_s", {"panel": "hit_scale"})),
    ("slower", "Positive hits at rest", "idle",
     ratios(sweep, E, "readers", "mlookups_s", {"panel": "idle_hit"})),

    ("capacity", "Leaf renames and exchanges (8 writers + 184 readers)",
     "optaxonomy",
     ratios(otax, E, "leaftype", "mrenames_s",
            {"panel": "leaf", "op": "rename", "rate_target": "0"},
            need_paced=False) +
     ratios(otax, E, "leaftype", "mrenames_s",
            {"panel": "leaf", "op": "exchange", "rate_target": "0"},
            need_paced=False)),
    ("capacity", "Leaf moves across directories", "optaxonomy",
     ratios(otax, E, "leaftype", "mrenames_s",
            {"panel": "leaf", "op": "move", "rate_target": "0"},
            need_paced=False)),
    ("capacity", "Directory rename / move / exchange", "optaxonomy",
     [v for op in ("rename", "move", "exchange", "exchange-cross")
      for v in ratios(otax, E, "leaftype", "mrenames_s",
                      {"panel": "dir", "op": op, "rate_target": "0"},
                      need_paced=False)]),
    ("capacity", "Directory exchanges at every height", "height",
     ratios(height, E, "move_height", "mexch_s", {"rate_target": "0"},
            need_paced=False)),
    ("capacity", "Renames with 2-184 concurrent readers (8 writers)", "sat",
     ratios(sweep, E, "readers", "mrenames_s", {"panel": "sat_scale"},
            need_paced=False)),
]

GROUPS = [
    ("par", "ON PAR — within 5% (median)", "#7F7F7F"),
    ("faster", "BUCKET LOCK FASTER", "#009E73"),
    ("slower", "BUCKET LOCK SLOWER", "#D55E00"),
    ("capacity", "WRITER CAPACITY — flat out\n(rename rate, not reader cost)",
     "#0072B2"),
]


def consistent(group, med):
    return {"par": 0.95 <= med <= 1.05, "faster": med > 1.05,
            "slower": med < 0.95, "capacity": True}[group]


bad = 0
for g, lab, fig_, vals in ROWS:
    if not vals:
        print(f"!! no data: {lab}", file=sys.stderr)
        bad += 1
    elif not consistent(g, statistics.median(vals)):
        print(f"!! {lab}: median {statistics.median(vals):.2f} contradicts "
              f"group '{g}'", file=sys.stderr)
        bad += 1

# layout: groups top to bottom, a gap row before each group header
layout = []				# (kind, payload)
for key, title, color in GROUPS:
    layout.append(("header", (title, color)))
    for row in ROWS:
        if row[0] == key and row[3]:
            layout.append(("row", (row, color)))
    layout.append(("gap", None))
layout.pop()

n = len(layout)
fig, ax = plt.subplots(figsize=(15, 0.36 * n + 2.2))
ax.axvspan(0.95, 1.05, color="#BBBBBB", alpha=0.35, lw=0, zorder=0)
ax.axvline(1.0, color="#444444", lw=1.0, ls="--", zorder=1)
XMIN, XMAX = 0.5, 64.0
for i, (kind, payload) in enumerate(layout):
    y = n - 1 - i
    if kind == "header":
        title, color = payload
        ax.text(-0.01, y, title, transform=ax.get_yaxis_transform(),
                color=color, fontsize=10.5, fontweight="bold", va="center",
                ha="right")
    elif kind == "row":
        (g, lab, figname, vals), color = payload
        lo, hi = min(vals), max(vals)
        ax.plot([lo, hi], [y, y], color=color, lw=7, alpha=0.35,
                solid_capstyle="round", zorder=2)
        ax.scatter(vals, [y] * len(vals), s=16, color=color, zorder=3,
                   edgecolors="white", linewidths=0.4)
        rng = f"{lo:.2f}×" if abs(hi - lo) < 0.005 else \
              f"{lo:.2f}–{hi:.2f}×"
        ax.text(hi * 1.06, y, rng, fontsize=9, va="center", ha="left",
                color="#222222")
        ax.text(-0.01, y, lab, transform=ax.get_yaxis_transform(),
                fontsize=9.2, va="center", ha="right")
        ax.annotate(f"dcache_{figname.split(',')[0].strip()}.png",
                    xy=(1.0, y), xycoords=("axes fraction", "data"),
                    xytext=(6, 0), textcoords="offset points", fontsize=7.5,
                    va="center", ha="left", color="#666666")

ax.set_xscale("log", base=2)
ax.set_xlim(XMIN, XMAX)
ticks = [0.5, 0.75, 1, 1.5, 2, 4, 8, 16, 32]
ax.xaxis.set_major_locator(FixedLocator(ticks))
ax.xaxis.set_major_formatter(FixedFormatter([f"{t:g}×" for t in ticks]))
ax.xaxis.set_minor_locator(NullLocator())
ax.set_ylim(-0.8, n - 0.2)
ax.set_yticks([])
ax.grid(axis="x", alpha=0.3, ls=":")
ax.set_xlabel("bucket lock + SW txn ÷ seqlock baseline   (log scale; grey "
              "band = within 5%; dots = every point of the benchmark's sweep)")
for side in ("top", "right", "left"):
    ax.spines[side].set_visible(False)
src = ", ".join(sorted(srcs))
fig.suptitle("Userspace dentry cache — bucket lock + SW txn vs the "
             "kernel-faithful seqlock baseline\nreaders compared at the same "
             "offered (paced) rename load; 2×96-core EPYC 9654, 24 NUMA nodes"
             f"   ·   sweep {src}", fontsize=12)
fig.subplots_adjust(left=0.30, right=0.87, top=0.93, bottom=0.07)
fig.savefig(OUT, dpi=140)
print("wrote", os.path.abspath(OUT))
sys.exit(1 if bad else 0)
