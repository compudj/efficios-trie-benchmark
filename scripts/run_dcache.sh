#!/bin/bash
# S3 sweep for the userspace dentry-cache experiment (experiments/dcache).
#
# Three arms, one binary each (built by `make -C experiments/dcache bench`):
#   seqlock       -- faithful kernel-style rename_lock + per-dentry d_seq
#   txn-global    -- urcu-txn port, GLOBAL rename_gen walk bracket
#   txn-pernode   -- urcu-txn port, PER-NODE host generation (localized)
#
# Two headline views + a scaling view, every run gated on namespace conservation
# (a CONSERVATION FAILED run is flagged and its numbers dropped):
#
#   frac         HOMOGENEOUS mix: each of `THREADS` threads does rename-frac of
#                its ops as renames, the rest as full-path lookups.  Sweeps the
#                rename fraction.  Dominated by the WRITER path (a rename is
#                ~50x a lookup), so it shows the mixed-workload throughput but
#                MASKS the reader-side generation difference.
#   split_w      ROLE-SPLIT: `RSPLIT` dedicated readers + W dedicated writers.
#                Reader Mlookups/s vs W isolates the reader path -- where the
#                global bracket contends a whole-tree cacheline and the per-node
#                host counter does not.  THE headline for the per-node arm.
#   split_scale  ROLE-SPLIT reader scaling: W fixed, sweep the reader count.
#
# Output: scripts/dcache_sweep.csv  (plot with scripts/plot_dcache.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
CSV=$REPO/scripts/dcache_sweep.csv

NDIRS=16
DEPTH=4
LEAVES=32
DUR=1000
RUNS=5

# Pin ONE hardware thread per physical core: ask hwloc for the first PU of every
# core (core:all.pu:0), OS-indexed.  On this 2x96 EPYC that is cpus 0..191 (the
# SMT siblings 192..383 are left idle), but deriving it from hwloc keeps the
# sweep correct on any PU numbering.  NCORE bounds how many threads we can place
# without doubling two threads onto one core.
CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
if [[ -n "$CPULIST" ]]; then
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .)
  PIN="--cpulist $CPULIST"
  echo ">> hwloc: one hw thread per core, $NCORE cores (${CPULIST:0:24}...)" >&2
else
  NCORE=$(nproc)
  PIN="--cpustride 1"
  echo ">> hwloc-calc unavailable; falling back to --cpustride 1 over $NCORE cpus" >&2
fi

# tree geometry knobs held fixed across the sweep
COMMON="--ndirs $NDIRS --depth $DEPTH --leaves $LEAVES --duration $DUR $PIN"

declare -A BINOF=( [seqlock]=bench_dcache_seqlock \
                   [txn-global]=bench_dcache_txn \
                   [txn-pernode]=bench_dcache_txn_pernode )
ENGINES="seqlock txn-global txn-pernode"

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }

for e in $ENGINES; do
  test -x "$BIN/${BINOF[$e]}" || { echo "MISSING $BIN/${BINOF[$e]} -- run 'make -C experiments/dcache bench'" >&2; exit 1; }
done

# run <panel> <engine> <threads> <writers(-1=homog)> <rename_frac> -> best-of-RUNS
# appends a CSV row; readers = threads-writers in split mode, else threads.
# $RUN_EXTRA (global) appends extra flags, e.g. "--readdir --leaves 64"; a later
# --leaves overrides the one baked into $COMMON (argv: last wins).  For readdir
# panels the "Mlookups/s:" field carries the readdir CALL rate.
RUN_EXTRA=""
run() {
  local panel=$1 eng=$2 threads=$3 writers=$4 frac=$5
  local bin=$BIN/${BINOF[$eng]} split="" readers=$threads r
  local best_lk=0 best_rn=0 cons=OK out
  if [[ "$writers" -ge 0 ]]; then split="--writers $writers"; readers=$((threads-writers)); fi
  for r in $(seq 1 $RUNS); do
    out=$(cd "$BIN" && ./"${BINOF[$eng]}" --nthreads "$threads" $split \
          --rename-frac "$frac" $COMMON $RUN_EXTRA 2>/dev/null)
    if ! grep -q "conservation: OK" <<< "$out"; then
      cons=FAIL; echo "!! $panel/$eng threads=$threads w=$writers frac=$frac CONSERVATION FAILED" >&2
      continue
    fi
    local lk rn
    lk=$(field "$out" "Mlookups/s:"); rn=$(field "$out" "Mrenames/s:")
    awk -v v="${lk:-0}" -v b="$best_lk" 'BEGIN{exit !(v>b)}' && best_lk=$lk
    awk -v v="${rn:-0}" -v b="$best_rn" 'BEGIN{exit !(v>b)}' && best_rn=$rn
  done
  echo "$panel,$eng,$threads,$writers,$readers,$frac,${best_lk:-0},${best_rn:-0},$cons" >> "$CSV"
  printf "  %-11s %-11s thr=%-4s w=%-3s frac=%-5s  rd=%8s Mlk/s  wr=%8s Mrn/s  %s\n" \
    "$panel" "$eng" "$threads" "$writers" "$frac" "$best_lk" "$best_rn" "$cons" >&2
}

echo "panel,engine,threads,writers,readers,rename_frac,mlookups_s,mrenames_s,conserved" > "$CSV"

# ---- Panel: HOMOGENEOUS rename-fraction sweep at fixed cores ---------------
THREADS=48
echo ">> frac panel: homogeneous mix, $THREADS threads, sweep rename fraction" >&2
for f in 0 0.005 0.01 0.02 0.05 0.1 0.2 0.35 0.5; do
  for e in $ENGINES; do run frac "$e" "$THREADS" -1 "$f"; done
done

# ---- Panel: ROLE-SPLIT reader path vs writer load (THE headline) -----------
RSPLIT=32
echo ">> split_w panel: $RSPLIT dedicated readers + W writers, reader Mlookups/s vs W" >&2
for w in 1 2 4 8 16 24 32 48; do
  for e in $ENGINES; do run split_w "$e" $((RSPLIT+w)) "$w" 1.0; done
done

# ---- Panel: ROLE-SPLIT reader scaling at fixed writer load -----------------
# Scale readers until readers+writers fill every physical core (RMAX=NCORE-WFIX),
# one hw thread per core.  On the 2x96 EPYC that is 184 readers + 8 writers = 192
# cores.  Cap each candidate point at RMAX and always include RMAX itself.
WFIX=8
RMAX=$((NCORE - WFIX))
RDPTS=$(for rd in 2 4 8 16 32 48 64 96 128 160 $RMAX; do
          (( rd >= 1 && rd <= RMAX )) && echo "$rd"; done | sort -n -u)
echo ">> split_scale panel: $WFIX writers, sweep readers up to $RMAX (fill $NCORE cores)" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run split_scale "$e" $((rd+WFIX)) "$WFIX" 1.0; done
done

# ---- Panel: READDIR reader scaling (directory listing under rename load) ----
# Readers enumerate a random dir (dc_readdir) instead of a leaf lookup.  Only the
# writers own the namespace, so dir size is fixed as readers scale.  Compares the
# txn lock-free RCU child-walk against the seqlock per-directory rwsem (the honest
# kernel-inode-rwsem analogue, not one global lock).  txn-global == txn-pernode
# here by construction: readdir reads no generation counter at all.
RDLEAVES=64                         # 64 leaves/writer * 8 writers / 16 dirs = 32 kids/dir
RUN_EXTRA="--readdir --leaves $RDLEAVES"
echo ">> readdir_scale panel: $WFIX writers, --readdir, sweep readers up to $RMAX" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run readdir_scale "$e" $((rd+WFIX)) "$WFIX" 1.0; done
done
RUN_EXTRA=""

# ---- Panel: READDIR reader throughput vs writer (rename) load ---------------
# Fixed reader pool, sweep writers.  Namespace is held constant (RDTOTAL leaves,
# leaves=RDTOTAL/W), so dir size stays fixed while the rename RATE -- and thus the
# per-dir wrlock exclusion the seqlock readdir suffers -- rises with W.  Isolates
# the writer-exclusion axis (the txn RCU walk never blocks on a writer).
RRD=32; RDTOTAL=1024
echo ">> readdir_w panel: $RRD readers, --readdir, sweep writers (namespace fixed $RDTOTAL)" >&2
for w in 1 2 4 8 16 24 32 48; do
  RUN_EXTRA="--readdir --leaves $((RDTOTAL / w))"
  for e in $ENGINES; do run readdir_w "$e" $((RRD+w)) "$w" 1.0; done
done
RUN_EXTRA=""

echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
