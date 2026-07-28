#!/usr/bin/env python3
"""Aggregate scripts/p2_engine_remeasure.csv into the four numbers P2 quotes.

Reports BOTH best-of-N and median per point.  The 2026-07-10 capture reported
best-of-3 only; carrying the median beside it is what shows whether a margin is
a real separation or two noisy distributions overlapping.  A margin that holds
on the median is the one worth printing in a paper.
"""
import csv, statistics, sys, collections

PATH = sys.argv[1] if len(sys.argv) > 1 else "scripts/p2_engine_remeasure.csv"

pts = collections.defaultdict(list)     # (panel, arm, cores, b) -> [mmoves]
bad = 0
with open(PATH) as fh:
    for r in csv.DictReader(fh):
        if r["status"] != "ok" or not r["mmoves_s"]:
            bad += 1
            continue
        pts[(r["panel"], r["arm"], int(r["cores"]), int(r["b"]))].append(
            float(r["mmoves_s"]))

def agg(panel, arm, cores, b):
    v = pts.get((panel, arm, cores, b))
    if not v:
        return None
    return {"best": max(v), "med": statistics.median(v), "min": min(v),
            "max": max(v), "n": len(v),
            "spread": (max(v) - min(v)) / statistics.median(v) * 100}

def pct(a, b):
    return (a / b - 1.0) * 100.0

if bad:
    print(f"!! {bad} failed/blank rows excluded\n")

# ---------------------------------------------------------------- headline A/B
print("=" * 78)
print("FIXED panel -- sole-driver vs helping, both at engine 97443472")
print("  (this is the +21.7% claim in P2 sec:varhelping)")
print("=" * 78)
print(f"{'cores':>5} | {'sole best':>10} {'help best':>10} {'delta%':>8} "
      f"| {'sole med':>9} {'help med':>9} {'delta%':>8} | {'spread%':>7}")
print("-" * 78)
fixed_best = fixed_med = None
for c in (1, 2, 4, 8, 16, 32, 64, 96, 128, 192):
    b = 3840 // c
    s, h = agg("fixed", "old-sole", c, b), agg("fixed", "old-helping", c, b)
    if not s or not h:
        continue
    db, dm = pct(s["best"], h["best"]), pct(s["med"], h["med"])
    print(f"{c:>5} | {s['best']:>10.3f} {h['best']:>10.3f} {db:>+7.1f}% "
          f"| {s['med']:>9.3f} {h['med']:>9.3f} {dm:>+7.1f}% "
          f"| {max(s['spread'], h['spread']):>6.1f}%")
    if c == 192:
        fixed_best, fixed_med = db, dm

# ------------------------------------------------------------------ drift check
print()
print("=" * 78)
print("DRIFT -- sole-driver at the PIN (b3e23f9f) vs at 97443472")
print("  (does the A/B's winner still measure the same on the shipped engine?)")
print("=" * 78)
print(f"{'cores':>5} | {'pin best':>10} {'old best':>10} {'delta%':>8} "
      f"| {'pin med':>9} {'old med':>9} {'delta%':>8}")
print("-" * 78)
for c in (1, 2, 4, 8, 16, 32, 64, 96, 128, 192):
    b = 3840 // c
    p, o = agg("fixed", "pin-sole", c, b), agg("fixed", "old-sole", c, b)
    if not p or not o:
        continue
    print(f"{c:>5} | {p['best']:>10.3f} {o['best']:>10.3f} "
          f"{pct(p['best'], o['best']):>+7.1f}% | {p['med']:>9.3f} "
          f"{o['med']:>9.3f} {pct(p['med'], o['med']):>+7.1f}%")

# -------------------------------------------------------------------- size panel
print()
print("=" * 78)
print("SIZE panel -- 192 cores, shrinking structures")
print("  (this is the '+25.8% under contention' claim)")
print("=" * 78)
print(f"{'keys/sl':>7} | {'sole best':>10} {'help best':>10} {'delta%':>8} "
      f"| {'sole med':>9} {'help med':>9} {'delta%':>8}")
print("-" * 78)
size_best = size_med = None
for b in (5, 10, 20, 40, 80):
    s, h = agg("size", "old-sole", 192, b), agg("size", "old-helping", 192, b)
    if not s or not h:
        continue
    db, dm = pct(s["best"], h["best"]), pct(s["med"], h["med"])
    print(f"{b * 192:>7} | {s['best']:>10.3f} {h['best']:>10.3f} {db:>+7.1f}% "
          f"| {s['med']:>9.3f} {h['med']:>9.3f} {dm:>+7.1f}%")
    if size_best is None or db > size_best:
        size_best, size_med = db, dm

# -------------------------------------------------------------------- lane panel
print()
print("=" * 78)
print("LANE panel -- optimistic vs forced serialized lane, at the PIN")
print("  (this is the 'roughly 2.6' factor in P2 sec:escalation)")
print("=" * 78)
print(f"{'cores':>5} | {'optimistic':>11} {'serialized':>11} {'factor':>8} "
      f"| {'opt med':>9} {'ser med':>9} {'factor':>8}")
print("-" * 78)
lane_best = lane_med = None
for c in (64, 96, 128, 192):
    b = 3840 // c
    o, s = agg("lane", "pin-sole", c, b), agg("lane", "pin-serial", c, b)
    if not o or not s:
        continue
    fb, fm = o["best"] / s["best"], o["med"] / s["med"]
    print(f"{c:>5} | {o['best']:>11.3f} {s['best']:>11.3f} {fb:>7.2f}x "
          f"| {o['med']:>9.3f} {s['med']:>9.3f} {fm:>7.2f}x")
    if c == 192:
        lane_best, lane_med = fb, fm

# ------------------------------------------------------------------- what to say
print()
print("=" * 78)
print("REPLACEMENTS FOR THE STALE NUMBERS IN P2")
print("=" * 78)
def show(label, old, best, med, unit):
    if best is None:
        print(f"  {label:<34} NO DATA")
        return
    print(f"  {label:<34} was {old:<10} now {best:+.1f}{unit} best / "
          f"{med:+.1f}{unit} median" if unit == "%" else
          f"  {label:<34} was {old:<10} now {best:.2f}x best / {med:.2f}x median")
show("sec:varhelping @192c", "+21.7%", fixed_best, fixed_med, "%")
show("sec:varhelping under contention", "+25.8%", size_best, size_med, "%")
show("sec:escalation lane factor", "~2.6x", lane_best, lane_med, "x")
print()
print("  Engine for the A/B rows: 97443472 (the last commit carrying BOTH")
print("  install disciplines).  P2 must say so -- helping cannot be built at")
print("  the pin.  The DRIFT table is what ties it to b3e23f9f.")
