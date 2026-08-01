#!/bin/bash
# File-operation vs directory-operation reader-scaling sweep for the dcache.
#
# Same benchmark (bench_dcache), same structure and reader path -- only the LEAF
# TYPE differs, which flips the writer's walk-causality bump:
#   dir  (default)              leaves are directories -> a leaf rename/move is a
#                               DIRECTORY operation, which bumps.
#   file (-DDC_BENCH_FILE_LEAVES) leaves are files -> a FILE operation, whose bump
#                               the txn engine SKIPS (a file is never an interior
#                               waypoint; see rename-shell-transition.md).
#
# The point is the crossover.  The GLOBAL arm brackets the whole walk on one
# whole-tree counter and does NO per-hop second pass, so its reader is the
# lightest -- WHEN the counter rarely moves.  On file operations (bumps skipped)
# that holds and global is competitive, even edging the localized arms.  On
# directory operations every bump is tree-wide, every reader re-walks, and global
# collapses while per-node/mark (localized second-pass, retry only on-path) scale.
# The seqlock (kernel-faithful) arm bumps regardless of type, so its two columns
# match -- the kernel makes no file/dir distinction (verified v7.0-rc3).
#
# Fixed at the split_scale headline point: 8 writers, rename-frac 1.0, sweep
# readers to fill the machine; jemalloc; ndirs = 16*writers (decontended).
#
# Output: scripts/dcache_optype.csv  (plot with scripts/plot_dcache_optype.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
CSV=${CSV:-$REPO/scripts/dcache_optype.csv}

WRITERS=8
NDIRS=$((16 * WRITERS))
DUR=${DUR:-1000}
RUNS=${RUNS:-5}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
# liburcu build to link against; URCU_BUILD=$REPO/urcu-txn-build-rseq selects the
# descriptor slab's rseq arm.  Keep the COMMIT fixed across arms: the slab's
# freelist changed wfstack -> lfstack and retirement became batched in 0d83f466,
# so arms from different commits are not an A/B of the rseq flag.
Bd=${URCU_BUILD:-$REPO/urcu-txn-build}
# URCU_SLAB_RSEQ is header-inline and rcu-txn-slab.h requires it IDENTICAL in
# every TU of the process, so it must be repeated on these compiles.  Derive it
# from the build being linked (from its CPPFLAGS -- configure appends the flag
# rather than AC_DEFINE-ing it, so config.h never mentions it) so the two cannot
# disagree; a mismatch is silent undefined behaviour.
SLABDEF=""; SLABINC=""; SLABLIB=""; SLABMODE="lfstack (atomic paths)"
if grep -qE '^CPPFLAGS = .*-DURCU_SLAB_RSEQ' "$Bd/src/Makefile" 2>/dev/null; then
  SLABDEF="-DURCU_SLAB_RSEQ"; SLABMODE="rseq per-cpu local lists"
  SLABINC=$(grep -ohE '\-I[^ ]*librseq[^ ]*' "$Bd/src/Makefile" 2>/dev/null | head -1)
  rl=$(grep -ohE '\-L[^ ]*librseq[^ ]*' "$Bd/src/Makefile" 2>/dev/null | head -1)
  [[ -n "$rl" ]] && SLABLIB="$rl -Wl,-rpath,${rl#-L} -lrseq"
fi
INC="-I$Bd/include -I$BIN $SLABDEF $SLABINC"
LIB="-L$Bd/src/.libs -Wl,-rpath,$Bd/src/.libs -lurcu-qsbr -lurcu-common -lpthread $SLABLIB"
printf '>> liburcu %s (%s)  slab: %s\n' \
  "$(git -C "$Bd" log -1 --format=%h 2>/dev/null || echo unknown)" "$Bd" "$SLABMODE" >&2
CC=${CC:-gcc}
CFLAGS="-O2 -g -pthread -march=native"

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
[[ -n "$CPULIST" ]] && PIN="--cpulist $CPULIST" && \
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | wc -l) || { PIN=""; NCORE=$(nproc); }
[[ -f "$JE" ]] || { echo "jemalloc not at $JE (set JE=)"; exit 1; }

# engine define ; source
declare -A EDEF=( [seqlock]="" [txn-global]="" [txn-pernode]="-DDC_PER_NODE_GEN" \
                  [txn-mark]="-DDC_MARK_GEN" [bucketlock]="-DDC_MARK_GEN" )
declare -A ESRC=( [seqlock]="dcache_seqlock.c" [txn-global]="dcache_txn.c" \
                  [txn-pernode]="dcache_txn.c" [txn-mark]="dcache_txn.c" \
                  [bucketlock]="dcache_bucketlock.c" )
ENGINES="seqlock txn-global txn-pernode txn-mark bucketlock"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
build() {   # <engine> <leaftype>
  local e=$1 lt=$2 def=""
  [[ "$lt" == file ]] && def="-DDC_BENCH_FILE_LEAVES"
  (cd "$BIN" && $CC $CFLAGS ${EDEF[$e]} $def $INC -o "$TMP/${e}_${lt}" \
      bench_dcache.c "${ESRC[$e]}" $LIB) || { echo "build $e/$lt failed"; exit 1; }
}

RMAX=$((NCORE - WRITERS))
RDPTS=$(for rd in 2 4 8 16 32 48 64 96 128 160 $RMAX; do
          (( rd >= 1 && rd <= RMAX )) && echo "$rd"; done | sort -n -u)

echo "leaftype,engine,readers,writers,mlookups_s,conserved" > "$CSV"
for e in $ENGINES; do
  for lt in file dir; do
    build "$e" "$lt"
    for rd in $RDPTS; do
      best=0 cons=OK
      for r in $(seq 1 $RUNS); do
        out=$(env LD_PRELOAD="$JE" "$TMP/${e}_${lt}" --nthreads $((WRITERS+rd)) \
              --writers $WRITERS --rename-frac 1.0 --ndirs $NDIRS --depth 4 \
              --leaves 32 --duration $DUR $PIN 2>/dev/null)
        grep -q "conservation: OK" <<< "$out" || { cons=FAIL; continue; }
        local_lk=$(grep -oP 'Mlookups/s: \K[0-9.]+' <<< "$out")
        awk -v v="${local_lk:-0}" -v b="$best" 'BEGIN{exit !(v>b)}' && best=$local_lk
      done
      echo "$lt,$e,$rd,$WRITERS,${best:-0},$cons" >> "$CSV"
      printf "  %-4s %-11s rd=%-4s %8s Mlk/s  %s\n" "$lt" "$e" "$rd" "$best" "$cons" >&2
    done
  done
done
echo ">> DONE: $(( $(wc -l < "$CSV") - 1 )) rows -> $CSV" >&2
