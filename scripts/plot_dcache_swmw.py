#!/usr/bin/env python3
"""
DLM+SW dentry cache: chain-serialization strategies for the transition chain.

Four arms (identical reader + mark; only the chain d_fwd/d_back handling differs):
  dlm-chainlock -DDC_CHAIN_LOCK -- legacy per-host chain LOCK: the demote AND every
                fold of a chain take it.  176 B.  The A/B reference baseline.
  dlm-swmw      -DDC_CHAIN_SWMW -- the chain rides the mixed commit as MW records
                (CAS-old serializes adjacent folds); chain lock RETIRED.  168 B.
  dlm-swmw-pad  same mixed engine + 8 B dead padding -> 176 B: the SAME-SIZE
                control.  (swmw-pad vs chainlock) isolates the MECHANISM;
                (swmw vs swmw-pad) isolates the -8B FOOTPRINT.
  dlm-foldlock  the DEFAULT (no chain flag) -- SW enqueue but the fold dequeue takes
                a per-host FOLD lock and rewrites the chain with plain stores (no MW
                descriptor).  The producer never takes that lock.  176 B.

Data: scripts/dcache_swmw.csv (best-of-N, conservation-gated).  2x96 EPYC, one hw
thread per core, jemalloc, dirs decontended to 16/writer.
"""
import csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import (FuncFormatter, FixedLocator, FixedFormatter,
                               LogLocator, NullFormatter)

_num = FuncFormatter(lambda v, pos: f"{v:g}")


def plain_y(ax):
    ax.set_yscale("log")
    ax.yaxis.set_major_locator(LogLocator(base=10, subs=(1, 2, 5)))
    ax.yaxis.set_major_formatter(_num)
    ax.yaxis.set_minor_formatter(NullFormatter())


def plain_thread_x(ax, ticks):
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter([str(t) for t in ticks]))
    ax.xaxis.set_minor_formatter(NullFormatter())


def percent_x(ax, fracs, labels):
    ax.set_xscale("symlog", linthresh=0.008)
    ax.xaxis.set_major_locator(FixedLocator(fracs))
    ax.xaxis.set_major_formatter(FixedFormatter(labels))
    ax.xaxis.set_minor_formatter(NullFormatter())


HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "dcache_swmw.csv")
OUT = os.environ.get("OUT", os.path.join(HERE, os.pardir, "figures", "dcache_swmw.png"))

COLOR = {"dlm-chainlock": "#D55E00", "dlm-swmw": "#0072B2", "dlm-swmw-pad": "#009E73", "dlm-foldlock": "#CC79A7"}
MARKER = {"dlm-chainlock": "s", "dlm-swmw": "o", "dlm-swmw-pad": "^", "dlm-foldlock": "D"}
LABEL = {
    "dlm-chainlock": "chain lock (legacy reference)\ndemote + folds take it — 176 B",
    "dlm-swmw": "mixed SW/MW (chain lock retired)\nchain = MW records, one commit — 168 B",
    "dlm-swmw-pad": "mixed SW/MW + 8 B pad\nsame-size control — 176 B",
    "dlm-foldlock": "fold lock (DEFAULT)\nSW enqueue + fold-lock dequeue — 176 B",
}
ORDER = ["dlm-chainlock", "dlm-swmw", "dlm-foldlock", "dlm-swmw-pad"]


def load():
    rows = defaultdict(lambda: defaultdict(dict))  # panel -> engine -> x -> (lk, rn)
    with open(CSV) as f:
        for r in csv.DictReader(f):
            if r["conserved"] != "OK":
                continue
            panel, eng = r["panel"], r["engine"]
            lk, rn = float(r["mlookups_s"]), float(r["mrenames_s"])
            if panel == "frac":
                x = float(r["rename_frac"])
            elif panel == "rd_w":
                x = int(r["writers"])
            else:
                x = int(r["threads"])
            rows[panel][eng][x] = (lk, rn)
    return rows


def series(rows, panel, eng, idx):
    d = rows[panel].get(eng, {})
    xs = sorted(d)
    return xs, [d[x][idx] for x in xs]


def main():
    rows = load()
    fig, (a0, a1, a2) = plt.subplots(1, 3, figsize=(16.5, 5.2))

    # -- Panel 0: rn_scale -- Mrenames/s vs threads (the headline) -----------
    tks = sorted({x for e in ORDER for x in rows["rn_scale"].get(e, {})})
    for e in ORDER:
        xs, ys = series(rows, "rn_scale", e, 1)
        if xs:
            a0.plot(xs, ys, marker=MARKER[e], color=COLOR[e], lw=2, ms=6,
                    label=LABEL[e])
    if tks:
        plain_thread_x(a0, tks)
    plain_y(a0)
    a0.set_xlabel("threads (homogeneous, each rename-frac 0.5)")
    a0.set_ylabel("Mrenames / s  (best of 5)")
    a0.set_title("Rename throughput vs concurrency\n(dirs decontended 16/thread — isolates the chain)")
    a0.grid(True, which="both", ls=":", alpha=0.4)
    a0.legend(fontsize=8, loc="upper left", framealpha=0.9)

    # -- Panel 1: frac -- Mrenames/s vs rename fraction @ 48 threads ---------
    fr = sorted({x for e in ORDER for x in rows["frac"].get(e, {})})
    flab = {0: "0", 0.01: "1%", 0.05: "5%", 0.1: "10%", 0.2: "20%",
            0.35: "35%", 0.5: "50%"}
    for e in ORDER:
        xs, ys = series(rows, "frac", e, 1)
        xs2 = [x for x in xs if x > 0]           # frac=0 has no renames; drop from log
        ys2 = [rows["frac"][e][x][1] for x in xs2]
        if xs2:
            a1.plot(xs2, ys2, marker=MARKER[e], color=COLOR[e], lw=2, ms=6,
                    label=LABEL[e].split("\n")[0])
    if fr:
        fr2 = [x for x in fr if x > 0]
        percent_x(a1, fr2, [flab.get(x, f"{x:g}") for x in fr2])
    plain_y(a1)
    a1.set_xlabel("rename fraction  (48 threads)")
    a1.set_ylabel("Mrenames / s")
    a1.set_title("Rename throughput vs rename fraction\n(where the chain starts to matter)")
    a1.grid(True, which="both", ls=":", alpha=0.4)
    a1.legend(fontsize=8, loc="upper left", framealpha=0.9)

    # -- Panel 2: rd_w -- reader Mlookups/s vs writers (must be a WASH) ------
    wk = sorted({x for e in ORDER for x in rows["rd_w"].get(e, {})})
    for e in ORDER:
        xs, ys = series(rows, "rd_w", e, 0)
        if xs:
            a2.plot(xs, ys, marker=MARKER[e], color=COLOR[e], lw=2, ms=6,
                    label=LABEL[e].split("\n")[0])
    if wk:
        plain_thread_x(a2, wk)
    plain_y(a2)
    a2.set_xlabel("writers  (32 dedicated readers)")
    a2.set_ylabel("reader Mlookups / s")
    a2.set_title("Reader path vs writer (rename) load\n(identical reader code — expect a wash)")
    a2.grid(True, which="both", ls=":", alpha=0.4)
    a2.legend(fontsize=8, loc="lower left", framealpha=0.9)

    fig.suptitle("DLM+SW dentry cache — transition-chain serialization: chain lock vs "
                 "mixed SW/MW vs the FOLD LOCK default (2×96 EPYC, 1 hw thread/core, jemalloc)",
                 fontsize=12, y=1.02)
    fig.tight_layout()
    fig.savefig(OUT, dpi=130, bbox_inches="tight")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
