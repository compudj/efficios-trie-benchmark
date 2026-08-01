#!/bin/bash
# Directory LISTING (readdir) vs create/delete (add/unlink) on a HOT directory
# set -- the two ops contend on the SAME child list, so this isolates how each
# engine serializes that list.  readdir takes the list SHARED, add/unlink take it
# EXCLUSIVE.
#
# The seqlock baseline guards the child list with a per-directory pthread_rwlock,
# whose BIAS decides the outcome -- and the glibc DEFAULT is reader-preferring,
# which is NOT the kernel.  A directory op in the kernel takes inode->i_rwsem, a
# FAIR FIFO rw_semaphore (a queued writer blocks later readers, so writers do not
# starve).  We cannot embed the truly-fair ISC phase-fair rwlock without inflating
# the dentry (sizeof isc_rwlock_t = 200 B vs pthread's 56 B, which would confound
# the footprint), so we BRACKET the fair result with the two footprint-neutral
# glibc biases -- the kernel's fair-rwsem number lies between them:
#   seqlock-rp   glibc default  PREFER_READER              (listing wins)
#   seqlock-wp   -DDC_DIR_LOCK_WRITER_PREF                 (churn wins)
# The txn / bucket-lock engines take NO per-dir rwlock at all: readdir is a
# lock-free RCU child-walk, add/unlink a bit-lock splice, so they escape the bias
# trade-off entirely.
#
# ndirs is SMALL (hot dirs) on purpose, so readers and writers collide on the same
# child lists.  Two panels / two questions (plot_dcache_readdir_churn.py):
#   list_vs_churn   32 readdir readers fixed, sweep churn writers -> does directory
#                   LISTING survive concurrent create/delete?   (plot Mreaddir/s)
#   churn_vs_list   8 churn writers fixed, sweep readdir readers -> does CREATE/
#                   DELETE survive concurrent listing?           (plot Mchurn/s)
#
# Output: scripts/dcache_readdir_churn.csv
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
CSV=${CSV:-$REPO/scripts/dcache_readdir_churn.csv}

NDIRS=${NDIRS:-16}		# hot: readers & writers collide on these dirs
SLOTS=32
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
CFLAGS="-O2 -g -pthread -march=native -DDC_SPLIT_KEEPID"

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
if [[ -n "$CPULIST" ]]; then
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .); PIN="--cpulist $CPULIST"
  echo ">> hwloc: one hw thread per core, $NCORE cores" >&2
else PIN=""; NCORE=$(nproc); echo ">> hwloc-calc unavailable; unpinned" >&2; fi
[[ -f "$JE" ]] || { echo "jemalloc not at $JE (set JE=)"; exit 1; }

# engine ; extra defines ; source ; extra link objects
declare -A EDEF=( [seqlock-rp]="" [seqlock-wp]="-DDC_DIR_LOCK_WRITER_PREF" \
                  [seqlock-krwsem]="-DDC_DIR_LOCK_KRWSEM" \
                  [txn-mark]="-DDC_MARK_GEN" [bucketlock]="-DDC_MARK_GEN" )
declare -A ESRC=( [seqlock-rp]="dcache_seqlock.c" [seqlock-wp]="dcache_seqlock.c" \
                  [seqlock-krwsem]="dcache_seqlock.c" \
                  [txn-mark]="dcache_txn.c" [bucketlock]="dcache_bucketlock.c" )
# seqlock-krwsem links the vendored Linux kernel rw_semaphore (GPL-2.0).
declare -A EXTRA=( [seqlock-krwsem]="krwsem/libkrwsem.a" )
ENGINES="seqlock-rp seqlock-wp seqlock-krwsem txn-mark bucketlock"

make -C "$BIN/krwsem" libkrwsem.a >/dev/null 2>&1 || { echo "krwsem build failed"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for e in $ENGINES; do
  (cd "$BIN" && $CC $CFLAGS ${EDEF[$e]} $INC -o "$TMP/$e" \
      bench_dcache_churn.c "${ESRC[$e]}" ${EXTRA[$e]:-} $LIB) || { echo "build $e failed"; exit 1; }
done

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }

# run <panel> <engine> <readers> <writers> -> best-of-RUNS, appends a CSV row
run() {
  local panel=$1 e=$2 rd=$3 w=$4 r out cons=OK best_ch=0 best_dr=0
  for r in $(seq 1 $RUNS); do
    out=$(env LD_PRELOAD="$JE" "$TMP/$e" --readers "$rd" --writers "$w" \
          --ndirs "$NDIRS" --slots $SLOTS --nbuckets 1048576 --readdir \
          --duration $DUR $PIN 2>/dev/null)
    grep -q "conservation: OK" <<< "$out" || { cons=FAIL; continue; }
    local ch dr; ch=$(field "$out" "Mchurn/s:"); dr=$(field "$out" "Mdirents/s:")
    awk -v v="${ch:-0}" -v b="$best_ch" 'BEGIN{exit !(v>b)}' && best_ch=$ch
    awk -v v="${dr:-0}" -v b="$best_dr" 'BEGIN{exit !(v>b)}' && best_dr=$dr
  done
  echo "$panel,$e,$rd,$w,${best_ch:-0},${best_dr:-0},$cons" >> "$CSV"
  printf "  %-14s %-11s rd=%-4s w=%-3s  churn=%8s Mops/s  readdir=%9s Mdirents/s  %s\n" \
    "$panel" "$e" "$rd" "$w" "$best_ch" "$best_dr" "$cons" >&2
}

echo "panel,engine,readers,writers,mchurn_s,mreaddir_s,conserved" > "$CSV"

echo ">> list_vs_churn: 32 readdir readers, sweep churn writers" >&2
for w in 1 2 4 8 16 32 48; do
  for e in $ENGINES; do run list_vs_churn "$e" 32 "$w"; done
done

WFIX=8
RMAX=$((NCORE - WFIX))
RDPTS=$(for rd in 2 4 8 16 32 64 96 128 160 $RMAX; do
          (( rd >= 1 && rd <= RMAX )) && echo "$rd"; done | sort -n -u)
echo ">> churn_vs_list: $WFIX churn writers, sweep readdir readers to $RMAX" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run churn_vs_list "$e" "$rd" "$WFIX"; done
done

echo ">> DONE: $(( $(wc -l < "$CSV") - 1 )) rows -> $CSV" >&2
