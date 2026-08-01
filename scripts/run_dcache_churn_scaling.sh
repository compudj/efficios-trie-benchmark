#!/bin/bash
# Insert/remove WRITER-SCALING sweep for the userspace dentry-cache experiment.
#
# run_dcache_churn.sh holds ndirs FIXED (16) and uses glibc, which -- as this
# sweep exists to show -- measures two stacked bottlenecks rather than the
# engine: the glibc allocator (every op malloc/free's a dentry) and the shared
# child-hlist HEADS of a small directory set (every op inserts/removes into its
# parent's child list).  Both mask the writer path's real scaling.
#
# This sweep removes both.  Allocator: LD_PRELOAD jemalloc (percpu_arena was
# measured a WASH here -- pinned writers make per-thread arenas CPU-stable -- so
# default jemalloc).  Child-hlist heads: ndirs is scaled WITH the writer count,
# ndirs = MULT * writers, so per-writer directory contention is held constant as
# W grows.  Three MULTs span heavily-shared -> matched -> decontended:
# writers/16, writers, 16*writers (symmetric on a log scale around one dir per
# writer).
#
# Two questions, two figure panels (scripts/plot_dcache_churn_scaling.py):
#   - how much does decontention buy (one engine, three ndirs)?
#   - decontended, which engine scales (three ndirs' widest, four engines)?
#
# Output: scripts/dcache_churn_scaling.csv
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
CSV=${CSV:-$REPO/scripts/dcache_churn_scaling.csv}

SLOTS=32
DUR=${DUR:-1000}
RUNS=${RUNS:-5}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
[[ -n "$CPULIST" ]] && PIN="--cpulist $CPULIST" || PIN=""
[[ -f "$JE" ]] || { echo "jemalloc not found at $JE (set JE=)"; exit 1; }

declare -A BINOF=( [seqlock]=bench_dcache_churn_seqlock \
                   [txn-global]=bench_dcache_churn_txn \
                   [txn-pernode]=bench_dcache_churn_txn_pernode \
                   [txn-mark]=bench_dcache_churn_txn_mark \
                   [bucketlock]=bench_dcache_churn_bucketlock )
ENGINES="seqlock txn-global txn-pernode txn-mark bucketlock"
for e in $ENGINES; do
  test -x "$BIN/${BINOF[$e]}" || {
    echo "MISSING $BIN/${BINOF[$e]} -- run 'make -C experiments/dcache churn'" >&2
    exit 1; }
done

echo "engine,dirmul,writers,ndirs,mchurn_s,conserved" > "$CSV"

run() {
  local eng=$1 dm=$2 nd=$3 w=$4 r out best=0 cons=OK
  for r in $(seq 1 $RUNS); do
    out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./"${BINOF[$eng]}" --readers 0 \
          --writers "$w" --ndirs "$nd" --slots $SLOTS --nbuckets 1048576 \
          --duration $DUR $PIN 2>/dev/null)
    grep -q "conservation: OK" <<< "$out" || { cons=FAIL; continue; }
    local c; c=$(awk '/Mchurn\/s:/{print $2}' <<< "$out")
    awk -v v="${c:-0}" -v b="$best" 'BEGIN{exit !(v>b)}' && best=$c
  done
  echo "$eng,$dm,$w,$nd,${best:-0},$cons" >> "$CSV"
  printf "  %-11s %-10s w=%-4s nd=%-6s %8s Mchurn/s  %s\n" \
    "$eng" "$dm" "$w" "$nd" "$best" "$cons" >&2
}

for w in 1 2 4 8 16 32 48 64 96 128 160 192; do
  d_lo=$(( w/16 < 1 ? 1 : w/16 )); d_hi=$(( 16*w ))
  for e in $ENGINES; do
    run "$e" "writers/16" "$d_lo" "$w"
    run "$e" "writers"    "$w"    "$w"
    run "$e" "16*writers" "$d_hi" "$w"
  done
done
echo ">> DONE: $(( $(wc -l < "$CSV") - 1 )) rows -> $CSV" >&2
