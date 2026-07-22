#!/bin/bash
# Chain-lock vs mixed-SW/MW A/B sweep for the DLM+SW dentry cache.
#
# Four arms, one binary each (built by `make -C experiments/dcache
# bench_dcache_dlm bench_dcache_dlm_chainlock bench_dcache_dlm_swmw
# bench_dcache_dlm_swmw_pad`):
#   dlm-chainlock -- -DDC_CHAIN_LOCK: the legacy per-host CHAIN LOCK (demote + folds
#                take it).  sizeof(dentry) 176.  The A/B reference baseline.
#   dlm-swmw  -- -DDC_CHAIN_SWMW: the chain (d_fwd/d_back) rides the mixed commit as
#                MW records; chain lock RETIRED (sizeof 168, lock-free folds)
#   dlm-swmw-pad -- -DDC_CHAIN_SWMW -DDC_SWMW_PAD: the mixed engine with 8 bytes of
#                dead padding (sizeof 176), the SAME-SIZE control.  (swmw-pad vs
#                chainlock) isolates the mechanism; (swmw vs swmw-pad) the -8B.
#   dlm-foldlock -- the DEFAULT (bench_dcache_dlm, no chain flag): SW enqueue + a
#                per-host FOLD LOCK dequeue with plain chain stores (the producer
#                does not take the lock).  sizeof 176.
#
# Panels (all conservation-gated; a CONSERVATION FAILED run is dropped):
#   rn_scale  HOMOGENEOUS rename-heavy (frac 0.5), sweep threads 1..NCORE.  THE
#             headline: does retiring the chain lock help the rename path, and does
#             the mixed engine's fair-mutex escalation lane cost it at scale?
#   frac      HOMOGENEOUS mix at fixed 48 threads, sweep the rename fraction: where
#             does the chain start to matter (a pure-lookup frac=0 baseline agrees).
#   rd_w      ROLE-SPLIT: 32 dedicated readers + W writers, reader Mlookups/s vs W.
#             The reader code is IDENTICAL across arms, so this must be a WASH --
#             the writer-engine swap leaves the read path untouched.
#
# Output: scripts/dcache_swmw.csv  (plot with scripts/plot_dcache_swmw.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
CSV=$REPO/scripts/dcache_swmw.csv

JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
DEPTH=4
LEAVES=32
DUR=${DUR:-1000}
RUNS=${RUNS:-5}

# One hardware thread per physical core (core:all.pu:0), OS-indexed -- 0..191 on
# this 2x96 EPYC; SMT siblings 192..383 left idle.  NCORE bounds placement.
CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
if [[ -n "$CPULIST" ]]; then
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .)
  PIN="--cpulist $CPULIST"
  echo ">> hwloc: one hw thread per core, $NCORE cores" >&2
else
  NCORE=$(nproc); PIN="--cpustride 1"
  echo ">> hwloc-calc unavailable; --cpustride 1 over $NCORE cpus" >&2
fi

COMMON="--depth $DEPTH --leaves $LEAVES --nbuckets 1048576 --duration $DUR $PIN"
[[ -f "$JE" ]] || { echo "jemalloc not at $JE"; exit 1; }

declare -A BINOF=( [dlm-chainlock]=bench_dcache_dlm_chainlock \
                   [dlm-swmw]=bench_dcache_dlm_swmw \
                   [dlm-swmw-pad]=bench_dcache_dlm_swmw_pad \
                   [dlm-foldlock]=bench_dcache_dlm )
ENGINES="dlm-chainlock dlm-swmw dlm-swmw-pad dlm-foldlock"
for e in $ENGINES; do
  test -x "$BIN/${BINOF[$e]}" || { echo "MISSING $BIN/${BINOF[$e]} -- run 'make -C experiments/dcache ${BINOF[$e]}'" >&2; exit 1; }
done

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }

# run <panel> <engine> <threads> <writers(-1=homog)> <rename_frac>
run() {
  local panel=$1 eng=$2 threads=$3 writers=$4 frac=$5
  local split="" readers=$threads r best_lk=0 best_rn=0 cons=OK out
  if [[ "$writers" -ge 0 ]]; then split="--writers $writers"; readers=$((threads-writers)); fi
  local nw=$(( writers >= 0 ? writers : threads ))
  local nd=$(( 16 * (nw < 1 ? 1 : nw) ))		# decontend: 16 child-heads/writer
  for r in $(seq 1 $RUNS); do
    out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./"${BINOF[$eng]}" --nthreads "$threads" \
          $split --ndirs "$nd" --rename-frac "$frac" $COMMON 2>/dev/null)
    if ! grep -q "conservation: OK" <<< "$out"; then
      cons=FAIL; echo "!! $panel/$eng thr=$threads w=$writers frac=$frac CONSERVATION FAILED" >&2
      continue
    fi
    local lk rn; lk=$(field "$out" "Mlookups/s:"); rn=$(field "$out" "Mrenames/s:")
    awk -v v="${lk:-0}" -v b="$best_lk" 'BEGIN{exit !(v>b)}' && best_lk=$lk
    awk -v v="${rn:-0}" -v b="$best_rn" 'BEGIN{exit !(v>b)}' && best_rn=$rn
  done
  echo "$panel,$eng,$threads,$writers,$readers,$frac,${best_lk:-0},${best_rn:-0},$cons" >> "$CSV"
  printf "  %-9s %-13s thr=%-4s w=%-3s f=%-5s  rd=%9s Mlk/s  wr=%9s Mrn/s  %s\n" \
    "$panel" "$eng" "$threads" "$writers" "$frac" "$best_lk" "$best_rn" "$cons" >&2
}

echo "panel,engine,threads,writers,readers,rename_frac,mlookups_s,mrenames_s,conserved" > "$CSV"

# ---- Panel rn_scale: rename-heavy homogeneous, sweep threads 1..NCORE --------
echo ">> rn_scale: homogeneous frac=0.5, sweep threads" >&2
for t in 1 2 4 8 16 32 48 64 96 128 $NCORE; do
  (( t <= NCORE )) || continue
  for e in $ENGINES; do run rn_scale "$e" "$t" -1 0.5; done
done

# ---- Panel frac: fixed 48 threads, sweep rename fraction --------------------
echo ">> frac: homogeneous, 48 threads, sweep rename fraction" >&2
for f in 0 0.01 0.05 0.1 0.2 0.35 0.5; do
  for e in $ENGINES; do run frac "$e" 48 -1 "$f"; done
done

# ---- Panel rd_w: reader path (must be a wash) ------------------------------
echo ">> rd_w: 32 readers + W writers, reader Mlookups/s vs W" >&2
RSPLIT=32
for w in 1 2 4 8 16 24 32 48; do
  for e in $ENGINES; do run rd_w "$e" $((RSPLIT+w)) "$w" 1.0; done
done

echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
