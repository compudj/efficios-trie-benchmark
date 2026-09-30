#!/bin/bash
# Adversarial MOVE-HEIGHT sweep for the userspace dentry-cache per-node arm.
#
# The S3 role-split sweep (run_dcache.sh) moves only LEAVES -- fan-in 1, the
# per-node counter's best case.  This sweep holds the reader workload fixed
# (uniform full-depth leaf walks over a balanced B-ary forest) and climbs the
# HEIGHT at which the writers move nodes: a move at height H swaps two sibling
# subtrees of B^H leaves, so it invalidates a fraction ~B^(H-D) of reader walks.
# The per-node reader's lead over the seqlock baseline should therefore erode from
# its leaf-case peak toward parity as H -> D-1 (moves near the band root touch
# almost every walk, like rename_lock does).  That erosion bounds the S3 headline.
#
# Fixed: 8 writers + 32 readers (mirrors run_dcache.sh split_w), balanced binary
# bands (branch 2), tree-depth 8 (256 leaves/band, 2048 total), one HW thread/core.
# Three arms: seqlock / txn-global / txn-pernode.  Best-of-RUNS, conservation-gated.
#
# PACING.  The writers are held to PER_W exchanges/s each (bench --rename-rate;
# 8 writers: 100k/s), so every engine's readers face one offered load: flat out,
# the engines' exchange rates differ severalfold and the one with the slower
# writers gets its readers measured on a quieter machine (see run_dcache.sh).
# Each height is ALSO run flat out as mode height_sat -- where each engine's
# writers top out under reader load, not a reader comparison.  Columns:
# rate_target (0 = flat out), paced (OK/SHORT/-, as run_dcache.sh), absent (the
# chosen run's walks that went ABSENT: the digit namespace is complete at every
# instant, so any nonzero count is a false negative).
#
# Output: scripts/dcache_height.csv  (plot with scripts/plot_dcache_height.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
# Provenance stamped on every row (scripts/dcache_src_id.sh).
SRC_ID=$("$REPO/scripts/dcache_src_id.sh" "${URCU_TXN_BUILD:-$REPO/urcu-txn-build}")
BIN=$REPO/experiments/dcache
CSV=${CSV:-$REPO/scripts/dcache_height.csv}

WRITERS=8
READERS=32
BRANCH=2
DEPTH=8			# leaves/band = BRANCH^DEPTH = 256; heights 0..DEPTH-1
DUR=${DUR:-1000}
RUNS=${RUNS:-5}
PER_W=${PER_W:-12500}		# paced exchanges/s per writer

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
if [[ -n "$CPULIST" ]]; then
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .)
  PIN="--cpulist $CPULIST"
  echo ">> hwloc: one hw thread per core, $NCORE cores" >&2
else
  PIN="--cpustride 1"
  echo ">> hwloc-calc unavailable; --cpustride 1" >&2
fi

declare -A BINOF=( [txn-mark]=bench_dcache_height_txn_mark \
                   [seqlock]=bench_dcache_height_seqlock \
                   [seqlock-snapshot]=bench_dcache_height_seqlock_snapshot \
                   [txn-global]=bench_dcache_height_txn \
                   [txn-pernode]=bench_dcache_height_txn_pernode \
                   [bucketlock]=bench_dcache_height_bucketlock )
ENGINES=${ENGINES:-"seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock"}
for e in $ENGINES; do
  test -x "$BIN/${BINOF[$e]}" || { echo "MISSING $BIN/${BINOF[$e]} -- run 'make -C experiments/dcache height'" >&2; exit 1; }
done
field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }
. "$REPO/scripts/dcache_pick_run.sh"

echo "height,engine,move_height,fanin,readers,writers,rate_target,mlookups_s,mexch_s,absent,paced,conserved,src" > "$CSV"

NTHREADS=$((READERS + WRITERS))
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
[[ -f "$JE" ]] || { echo "jemalloc not at $JE"; exit 1; }
COMMON="--writers $WRITERS --nthreads $NTHREADS --branch $BRANCH --tree-depth $DEPTH --duration $DUR $PIN"

RATE=$((WRITERS * PER_W))
for mode in height height_sat; do
  rate=$RATE; pace="--rename-rate $RATE"
  [[ $mode == height_sat ]] && { rate=0; pace=""; }
  for h in $(seq 0 $((DEPTH - 1))); do
    fanin=$((BRANCH ** h))
    for e in $ENGINES; do
      cons=OK; runs=""
      for r in $(seq 1 $RUNS); do
        out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./"${BINOF[$e]}" --move-height "$h" $pace $COMMON 2>/dev/null)
        if ! grep -q "conservation: OK" <<< "$out"; then
          cons=FAIL; echo "!! $mode $e H=$h CONSERVATION FAILED" >&2; continue
        fi
        runs+="$(field "$out" "Mlookups/s:") $(field "$out" "Mrenames/s:") $(field "$out" "absent:")"$'\n'
      done
      read -r best_lk best_ex absent paced < <(pick_run "$rate" <<< "$runs")
      [[ "$paced" == "" ]] && { paced=$absent; absent=0; }	# no conserved run
      echo "$mode,$e,$h,$fanin,$READERS,$WRITERS,$rate,$best_lk,$best_ex,$absent,$paced,$cons,$SRC_ID" >> "$CSV"
      printf "  %-10s H=%-2s fanin=%-4s %-16s rd=%9s Mlk/s  wr=%8s Mexch/s  absent=%-6s %-5s %s\n" \
        "$mode" "$h" "$fanin" "$e" "$best_lk" "$best_ex" "$absent" "$paced" "$cons" >&2
    done
  done
done
echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
