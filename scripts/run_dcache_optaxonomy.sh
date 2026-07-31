#!/bin/bash
# Op-taxonomy sweep for the dcache: all four mutating operations, per engine.
#
# The four ops are a 2x2 of {file|directory} x {same-dir|cross-dir}:
#
#                     same directory        cross directory
#   file (leaf)       rename                file move
#   directory         directory rename      directory move
#
# and the two axes ARE the two code branches -- same-vs-cross gates the O(depth)
# ancestry cycle check, the d_moving lock and the reparent store; file-vs-dir
# sets how many reader walks the relocation invalidates.  Until now the perf
# benches covered only `file move` (bench_dcache, which hardcoded cross-dir leaf
# moves) and an exchange approximation of `directory rename`
# (bench_dcache_height).  `rename` -- the most common op a real filesystem
# issues -- and `directory move` had no perf number at all.  This closes both.
#
# Two panels, because the two halves live in different harnesses:
#
#   leaf/   bench_dcache --op-mix: rename (same-dir, flips the token between its
#           two reserved names) vs move (cross-dir) vs exchange, at the S3
#           split-sweep headline point.  Leaf TYPE is a build switch, so each
#           mix runs twice: file leaves (-DDC_BENCH_FILE_LEAVES, whose causality
#           bump the txn engine skips) and directory leaves.
#   dir/    bench_dcache_height --op: one-way `rename`/`move` over the spare
#           subtree (prices the op itself; the subtree is off the reader paths,
#           so no walk goes ABSENT and the reader number is unbiased) plus the
#           reader-visible `exchange`/`exchange-cross` arms (which price the B^H
#           invalidation).  See the harness header for why one-way needs a spare
#           subtree rather than a hole.
#
# Read the two panels together: leaf/ says what the cross_parent branch costs a
# writer, dir/ says what it costs when the moved node also dominates a subtree.
#
# Output: scripts/dcache_optaxonomy.csv (plot with plot_dcache_optaxonomy.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
# Overridable so a stability re-measure can write somewhere else instead of
# overwriting the sweep it is checking (the header write below TRUNCATES).
CSV=${CSV:-$REPO/scripts/dcache_optaxonomy.csv}

WRITERS=${WRITERS:-8}
NDIRS=$((16 * WRITERS))
DUR=${DUR:-1000}
RUNS=${RUNS:-5}
HEIGHT=${HEIGHT:-2}		# move height for the directory panel (needs <= D-2)
BRANCH=${BRANCH:-2}
TREE_DEPTH=${TREE_DEPTH:-8}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
Bd=$REPO/urcu-txn-build
INC="-I$Bd/include -I$BIN"
LIB="-L$Bd/src/.libs -Wl,-rpath,$Bd/src/.libs -lurcu-qsbr -lurcu-common -lpthread"
CC=${CC:-gcc}
CFLAGS="-O2 -g -pthread -march=native"

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
[[ -n "$CPULIST" ]] && PIN="--cpulist $CPULIST" && \
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .) || { PIN=""; NCORE=$(nproc); }
[[ -f "$JE" ]] || { echo "jemalloc not at $JE (set JE=)"; exit 1; }

declare -A EDEF=( [seqlock]="" [txn-global]="" [txn-pernode]="-DDC_PER_NODE_GEN" \
                  [txn-mark]="-DDC_MARK_GEN" [bucketlock]="-DDC_MARK_GEN" \
                  [bucketlock-chainlock]="-DDC_MARK_GEN -DDC_CHAIN_LOCK" \
                  [bucketlock-swmw]="-DDC_MARK_GEN -DDC_CHAIN_SWMW" \
                  [bucketlock-swmw-pad]="-DDC_MARK_GEN -DDC_CHAIN_SWMW -DDC_SWMW_PAD" )
declare -A ESRC=( [seqlock]="dcache_seqlock.c" [txn-global]="dcache_txn.c" \
                  [txn-pernode]="dcache_txn.c" [txn-mark]="dcache_txn.c" \
                  [bucketlock]="dcache_bucketlock.c" \
                  [bucketlock-chainlock]="dcache_bucketlock.c" \
                  [bucketlock-swmw]="dcache_bucketlock.c" \
                  [bucketlock-swmw-pad]="dcache_bucketlock.c" )
# The three chain strategies of the bucketlock engine are carried so the op
# taxonomy can price the published "fold lock vs all-MW chain" gap on the op the
# original measurement never took (same-dir rename) as well as on the cross-dir
# move it did take:
#   bucketlock            per-host FOLD LOCK dequeue (the default/shipped arm), 176 B
#   bucketlock-chainlock  legacy per-host CHAIN LOCK (the A/B baseline), 176 B
#   bucketlock-swmw       lock-free MW chain via the mixed commit, 168 B
#   bucketlock-swmw-pad   the MW chain with the retired 8 B restored as dead
#                         padding -- the SAME-SIZE control, so bucketlock-vs-pad
#                         isolates the chain MECHANISM and pad-vs-swmw the -8 B.
# Reading the published gap against `swmw` alone confounds mechanism with size.
ENGINES=${ENGINES:-"seqlock txn-global txn-pernode txn-mark bucketlock \
                    bucketlock-chainlock bucketlock-swmw bucketlock-swmw-pad"}
# Which panels to run.  Both by default; `PANELS=leaf` re-runs only the leaf
# taxonomy (the panel that prices the cross_parent branch on its own).
PANELS=${PANELS:-"leaf dir"}

# leaf ops: <label>:<--op-mix spec>.  Pure mixes, so each row is ONE op -- a
# blended mix would average two branches into one number and hide the split.
LEAF_OPS="rename:rename=1,move=0,exchange=0 \
          move:rename=0,move=1,exchange=0 \
          exchange:rename=0,move=0,exchange=1"
# directory ops: the four --op arms of bench_dcache_height.
DIR_OPS=${DIR_OPS:-"rename move exchange exchange-cross"}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

build() {   # <engine> <harness> <extra-defs> -> $TMP/<engine>_<tag>
  local e=$1 src=$2 def=$3 tag=$4
  (cd "$BIN" && $CC $CFLAGS ${EDEF[$e]} $def $INC -o "$TMP/${e}_${tag}" \
      "$src" "${ESRC[$e]}" $LIB) || { echo "build $e/$tag failed"; exit 1; }
}

field() { grep -oP "$2 \K[0-9.]+" <<< "$1" | head -1; }

echo "panel,engine,op,leaftype,readers,writers,mlookups_s,mrenames_s,conserved" > "$CSV"

# ---- panel 1: leaf ops (bench_dcache) ------------------------------------
# Role-split at the S3 headline point: writers do nothing but the op under test,
# readers fill the machine, so both columns are clean.
# RD / HTHREADS are overridable so the script can be smoke-run on a busy box
# without claiming the machine; the sweep itself leaves them at the default.
RD=${RD:-$((NCORE - WRITERS))}
HTHREADS=${HTHREADS:-$NCORE}
[[ " $PANELS " == *" leaf "* ]] && for e in $ENGINES; do
  for lt in file dir; do
    def=""; [[ "$lt" == file ]] && def="-DDC_BENCH_FILE_LEAVES"
    build "$e" bench_dcache.c "$def" "leaf_$lt"
    for spec in $LEAF_OPS; do
      op=${spec%%:*}; mix=${spec#*:}
      best_lk=0 best_rn=0 cons=OK
      for r in $(seq 1 $RUNS); do
        out=$(env LD_PRELOAD="$JE" "$TMP/${e}_leaf_$lt" --nthreads $((WRITERS+RD)) \
              --writers $WRITERS --op-mix "$mix" --ndirs $NDIRS --depth 4 \
              --leaves 32 --duration $DUR $PIN 2>/dev/null)
        grep -q "conservation: OK" <<< "$out" || { cons=FAIL; continue; }
        lk=$(field "$out" "Mlookups/s:"); rn=$(field "$out" "Mrenames/s:")
        awk -v v="${lk:-0}" -v b="$best_lk" 'BEGIN{exit !(v>b)}' && best_lk=$lk
        awk -v v="${rn:-0}" -v b="$best_rn" 'BEGIN{exit !(v>b)}' && best_rn=$rn
      done
      echo "leaf,$e,$op,$lt,$RD,$WRITERS,${best_lk:-0},${best_rn:-0},$cons" >> "$CSV"
      printf "  leaf %-11s %-9s %-4s rd=%-4s %8s Mlk/s %8s Mrn/s %s\n" \
        "$e" "$op" "$lt" "$RD" "$best_lk" "$best_rn" "$cons" >&2
    done
  done
done

# ---- panel 2: directory ops (bench_dcache_height) ------------------------
# The moved node dominates B^HEIGHT leaves.  exchange-cross and move need two
# parents at band-depth D-H-1, i.e. HEIGHT <= TREE_DEPTH-2 (the harness rejects
# the rest rather than silently running the same-dir op).
[[ " $PANELS " == *" dir "* ]] && for e in $ENGINES; do
  build "$e" bench_dcache_height.c "" "height"
  for op in $DIR_OPS; do
    best_lk=0 best_rn=0 cons=OK
    for r in $(seq 1 $RUNS); do
      out=$(env LD_PRELOAD="$JE" "$TMP/${e}_height" --writers $WRITERS \
            --nthreads "$HTHREADS" --move-height $HEIGHT --branch $BRANCH \
            --tree-depth $TREE_DEPTH --op "$op" --duration $DUR $PIN 2>/dev/null)
      grep -q "conservation: OK" <<< "$out" || { cons=FAIL; continue; }
      lk=$(field "$out" "Mlookups/s:"); rn=$(field "$out" "Mrenames/s:")
      awk -v v="${lk:-0}" -v b="$best_lk" 'BEGIN{exit !(v>b)}' && best_lk=$lk
      awk -v v="${rn:-0}" -v b="$best_rn" 'BEGIN{exit !(v>b)}' && best_rn=$rn
    done
    echo "dir,$e,$op,directory,$((HTHREADS-WRITERS)),$WRITERS,${best_lk:-0},${best_rn:-0},$cons" >> "$CSV"
    printf "  dir  %-11s %-15s H=%-2s %8s Mlk/s %8s Mrn/s %s\n" \
      "$e" "$op" "$HEIGHT" "$best_lk" "$best_rn" "$cons" >&2
  done
done

echo ">> DONE: $(( $(wc -l < "$CSV") - 1 )) rows -> $CSV" >&2
