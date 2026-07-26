#!/usr/bin/env python3
"""ORDO boundary sweep -- python-3 port of mv-rlu tools/ordo/gen_table.py.

Upstream algorithm, unchanged where it matters:
    boundary = max over unordered pairs (a,b) of
                   min( min(samples a->b), min(samples b->a) )
Each direction is one `o/reftable a b ITERS` run; the per-run statistic is the
MINIMUM observed round-trip-derived offset, and the boundary is the MAXIMUM of
those over all pairs.

Deliberate differences from upstream gen_table.py, both documented in the CSV
header so the provenance travels with the number:
  1. Per-pair sample dumps to output/%d-%d.txt are NOT written.  Upstream keeps
     every one of ITERS samples per ordered pair; at the default ITERS and 192
     cores that is ~183 GB of text whose only use is the min this script already
     computes streamingly.
  2. --cores selects the core set.  'all' is upstream behaviour (every primary
     hyperthread).  'nodes' samples N cores per NUMA node, which is the right
     resolution if you believe TSC skew is a property of clock domains rather
     than of individual cores -- much cheaper, but a SAMPLE: it can only
     underestimate the true max, never overestimate it.

REQUIRES ROOT: reftable calls sched_setscheduler(SCHED_FIFO) so the ping-pong
is not preempted.  Without it the measurement is meaningless, so this script
refuses to run unprivileged rather than emit a plausible wrong number.
"""
import argparse
import csv
import itertools
import os
import subprocess
import sys
import time


def primary_cpus():
    """Primary hyperthread of each core, i.e. cpu == min(thread_siblings)."""
    out = []
    base = "/sys/devices/system/cpu"
    for entry in sorted(os.listdir(base)):
        if not entry.startswith("cpu") or not entry[3:].isdigit():
            continue
        cpu = int(entry[3:])
        path = os.path.join(base, entry, "topology/thread_siblings_list")
        try:
            with open(path) as f:
                sibs = f.read().strip()
        except EnvironmentError:
            out.append(cpu)
            continue
        ids = set()
        for part in sibs.split(","):
            if "-" in part:
                lo, hi = part.split("-")
                ids.update(range(int(lo), int(hi) + 1))
            else:
                ids.add(int(part))
        if cpu == min(ids):
            out.append(cpu)
    return out


def numa_of(cpu):
    base = "/sys/devices/system/cpu/cpu%d" % cpu
    for entry in os.listdir(base):
        if entry.startswith("node") and entry[4:].isdigit():
            return int(entry[4:])
    return 0


def pick_cores(mode, per_node):
    cpus = primary_cpus()
    if mode == "all":
        return cpus, {}
    bynode = {}
    for c in cpus:
        bynode.setdefault(numa_of(c), []).append(c)
    picked = []
    for node in sorted(bynode):
        picked.extend(sorted(bynode[node])[:per_node])
    return picked, bynode


def run_pair(reftable, a, b, iters):
    """One direction; returns the min sample, parsed streamingly."""
    p = subprocess.Popen([reftable, str(a), str(b), str(iters)],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    best = None
    for line in p.stdout:
        line = line.strip()
        if not line:
            continue
        try:
            v = int(line)
        except ValueError:
            continue
        if best is None or v < best:
            best = v
    _, err = p.communicate()
    if p.returncode != 0:
        raise RuntimeError("reftable %d %d failed: %s"
                           % (a, b, err.decode("utf8", "replace").strip()))
    if best is None:
        raise RuntimeError("reftable %d %d produced no samples" % (a, b))
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reftable", default="o/reftable")
    ap.add_argument("--iters", type=int, default=1000000,
                    help="samples per direction (upstream: 1000000)")
    ap.add_argument("--cores", choices=("all", "nodes"), default="nodes")
    ap.add_argument("--per-node", type=int, default=1,
                    help="cores sampled per NUMA node when --cores=nodes")
    ap.add_argument("--out", default="ordo_pairs.csv")
    ap.add_argument("--allow-unprivileged", action="store_true",
                    help="DEBUG ONLY -- result is not a valid measurement")
    args = ap.parse_args()

    if os.geteuid() != 0 and not args.allow_unprivileged:
        sys.exit("must run as root (reftable needs SCHED_FIFO); "
                 "rerun under sudo")

    cores, bynode = pick_cores(args.cores, args.per_node)
    pairs = list(itertools.combinations(cores, 2))
    print("cores (%s): %d -> %d pairs, ~%.1f min at %d iters"
          % (args.cores, len(cores), len(pairs),
             len(pairs) * 2 * 1.76 * (args.iters / 1e6) / 60, args.iters),
          file=sys.stderr)

    done = {}
    if os.path.exists(args.out):
        with open(args.out) as f:
            for row in csv.DictReader(l for l in f if not l.startswith("#")):
                done[(int(row["a"]), int(row["b"]))] = int(row["offset"])
        print("resuming: %d pairs already measured" % len(done), file=sys.stderr)

    newfile = not os.path.exists(args.out)
    fh = open(args.out, "a")
    if newfile:
        fh.write("# ORDO boundary sweep, mv-rlu tools/ordo reftable\n")
        fh.write("# iters=%d cores=%s per_node=%d ncores=%d npairs=%d\n"
                 % (args.iters, args.cores, args.per_node, len(cores),
                    len(pairs)))
        fh.write("# offset = min(min(a->b), min(b->a)); boundary = max(offset)\n")
        fh.write("# per-pair sample dumps intentionally not written\n")
        fh.write("a,b,node_a,node_b,offset\n")
        fh.flush()

    best = max(done.values()) if done else 0
    t0 = time.time()
    for i, (a, b) in enumerate(pairs):
        if (a, b) in done:
            continue
        off = min(run_pair(args.reftable, a, b, args.iters),
                  run_pair(args.reftable, b, a, args.iters))
        best = max(best, off)
        fh.write("%d,%d,%d,%d,%d\n" % (a, b, numa_of(a), numa_of(b), off))
        fh.flush()
        el = time.time() - t0
        print("[%d/%d] (%d,%d) offset=%d  max=%d  %.1f min elapsed"
              % (i + 1, len(pairs), a, b, off, best, el / 60), file=sys.stderr)

    fh.close()
    print("\n__ORDO_BOUNDARY = %d" % best)
    print("build with: -DMVRLU_ORDO_TIMESTAMPING -D__ORDO_BOUNDARY=%d" % best)


if __name__ == "__main__":
    main()
