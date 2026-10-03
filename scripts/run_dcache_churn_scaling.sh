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
# default jemalloc).  Child-hlist heads: WHO SHARES A DIRECTORY is the axis
# (column `share`), stated through bench --share and checked against the
# bench's own classification of every directory (its `share:` line):
#   private     each writer's 32 directories are its own        (--share 1)
#   same-node   shared by a pair of writers on one NUMA node    (--share 2)
#   cross-node  shared by a pair on two nodes        (--share 2, stride = CPN)
#   all         one set of 32 shared by EVERY writer            (--share W)
# Until 2026-10-02 the arms were ndirs = writers/16, writers and 16*writers
# through the bench's modulo mapping, which made the sharers -- and whether they
# sat on one node or several -- a side effect of W: the "decontended" 16*writers
# arm paired writer i with i+W/2, on another node from 16 writers on.
#
# Two questions, two figure panels (scripts/plot_dcache_churn_scaling.py):
#   - what does sharing cost (one engine, the four arms)?
#   - private, which engine scales (every engine)?
#
# Output: scripts/dcache_churn_scaling.csv
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
# Provenance stamped on every row (scripts/dcache_src_id.sh).
SRC_ID=$("$REPO/scripts/dcache_src_id.sh" "${URCU_TXN_BUILD:-$REPO/urcu-txn-build}")
BIN=$REPO/experiments/dcache
CSV=${CSV:-$REPO/scripts/dcache_churn_scaling.csv}

SLOTS=32
DUR=${DUR:-1000}
RUNS=${RUNS:-5}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
[[ -n "$CPULIST" ]] && PIN="--cpulist $CPULIST" || PIN=""
# cores per NUMA node in that order: the stride that splits a pair across nodes
CPN=$(hwloc-calc --number-of core node:0 2>/dev/null)
[[ "$CPN" =~ ^[0-9]+$ && "$CPN" -gt 0 ]] || CPN=8
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

echo "engine,share,writers,ndirs,mchurn_s,conserved,src" > "$CSV"

# share <arm> <writers> -> the bench arguments for that arm
share_args() {
  case "$1" in
    private)    echo "--share 1" ;;
    same-node)  echo "--share 2 --share-stride 1" ;;
    cross-node) echo "--share 2 --share-stride $CPN" ;;
    all)        echo "--share $2" ;;
  esac
}
# Did the bench get that arm?  Read its `share:` line: every directory in the
# arm's class, or -- for `all` -- every writer on every directory.
share_ok() {
  awk -v arm="$1" -v w="$2" '/^share:/ { for (i = 2; i <= NF; i++) {
        split($i, kv, "="); v[kv[1]] = kv[2] }
      if (arm == "all") ok = (v["writers/dir"] == w)
      else ok = (v["dirs"] > 0 && v[arm] == v["dirs"]) }
    END { exit !ok }' <<< "$3"
}

run() {
  local eng=$1 arm=$2 w=$3 r out best=0 cons=OK nd=0
  for r in $(seq 1 $RUNS); do
    out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./"${BINOF[$eng]}" --readers 0 \
          --writers "$w" $(share_args "$arm" "$w") --slots $SLOTS \
          --nbuckets 1048576 --duration $DUR $PIN 2>/dev/null)
    share_ok "$arm" "$w" "$out" || { cons=SHARE
      echo "!! $eng w=$w: not $arm: $(grep '^share:' <<< "$out")" >&2; continue; }
    grep -q "conservation: OK" <<< "$out" || { cons=FAIL; continue; }
    nd=$(awk '/^share:/ { for (i = 2; i <= NF; i++) if ($i ~ /^dirs=/) {
           sub(/^dirs=/, "", $i); print $i } }' <<< "$out")
    local c; c=$(awk '/Mchurn\/s:/{print $2}' <<< "$out")
    awk -v v="${c:-0}" -v b="$best" 'BEGIN{exit !(v>b)}' && best=$c
  done
  echo "$eng,$arm,$w,$nd,${best:-0},$cons,$SRC_ID" >> "$CSV"
  printf "  %-11s %-10s w=%-4s nd=%-6s %8s Mchurn/s  %s\n" \
    "$eng" "$arm" "$w" "$nd" "$best" "$cons" >&2
}

for w in 1 2 4 8 16 32 48 64 96 128 160 192; do
  for e in $ENGINES; do
    run "$e" private "$w"
    (( w % 2 == 0 )) && run "$e" same-node "$w"
    (( w % (2 * CPN) == 0 )) && run "$e" cross-node "$w"
    (( w > 1 )) && run "$e" all "$w"
  done
done
echo ">> DONE: $(( $(wc -l < "$CSV") - 1 )) rows -> $CSV" >&2
