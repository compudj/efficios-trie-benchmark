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
# Output: scripts/dcache_height.csv  (plot with scripts/plot_dcache_height.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
CSV=$REPO/scripts/dcache_height.csv

WRITERS=8
READERS=32
BRANCH=2
DEPTH=8			# leaves/band = BRANCH^DEPTH = 256; heights 0..DEPTH-1
DUR=1000
RUNS=5

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
if [[ -n "$CPULIST" ]]; then
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .)
  PIN="--cpulist $CPULIST"
  echo ">> hwloc: one hw thread per core, $NCORE cores" >&2
else
  PIN="--cpustride 1"
  echo ">> hwloc-calc unavailable; --cpustride 1" >&2
fi

declare -A BINOF=( [seqlock]=bench_dcache_height_seqlock \
                   [txn-global]=bench_dcache_height_txn \
                   [txn-pernode]=bench_dcache_height_txn_pernode )
ENGINES="seqlock txn-global txn-pernode"
for e in $ENGINES; do
  test -x "$BIN/${BINOF[$e]}" || { echo "MISSING $BIN/${BINOF[$e]} -- run 'make -C experiments/dcache height'" >&2; exit 1; }
done
field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }

echo "height,engine,move_height,fanin,readers,writers,mlookups_s,mexch_s,conserved" > "$CSV"

NTHREADS=$((READERS + WRITERS))
COMMON="--writers $WRITERS --nthreads $NTHREADS --branch $BRANCH --tree-depth $DEPTH --duration $DUR $PIN"

for h in $(seq 0 $((DEPTH - 1))); do
  fanin=$((BRANCH ** h))
  for e in $ENGINES; do
    best_lk=0; best_ex=0; cons=OK
    for r in $(seq 1 $RUNS); do
      out=$(cd "$BIN" && ./"${BINOF[$e]}" --move-height "$h" $COMMON 2>/dev/null)
      if ! grep -q "conservation: OK" <<< "$out"; then
        cons=FAIL; echo "!! $e H=$h CONSERVATION FAILED" >&2; continue
      fi
      lk=$(field "$out" "Mlookups/s:"); ex=$(field "$out" "Mrenames/s:")
      awk -v v="${lk:-0}" -v b="$best_lk" 'BEGIN{exit !(v>b)}' && best_lk=$lk
      awk -v v="${ex:-0}" -v b="$best_ex" 'BEGIN{exit !(v>b)}' && best_ex=$ex
    done
    echo "height,$e,$h,$fanin,$READERS,$WRITERS,${best_lk:-0},${best_ex:-0},$cons" >> "$CSV"
    printf "  H=%-2s fanin=%-4s %-12s rd=%9s Mlk/s  wr=%8s Mexch/s  %s\n" \
      "$h" "$fanin" "$e" "$best_lk" "$best_ex" "$cons" >&2
  done
done
echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
