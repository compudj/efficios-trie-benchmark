#!/bin/bash
# run_rlu_vs_txn_sweep.sh -- regenerate figures/rlu_vs_txn.png (all 3 panels) with
# the current tree, on RLU's allocator methodology: a JEMALLOC=1 build (bakes
# malloc_conf = percpu_arena:percpu) for both schemes, best-of-2, DURATION_SEC=3.
#
# Panels (each a write-scaling curve, write Mops/s vs writers):
#   churn  -- disjoint strided churn : txn_list  vs rlu_list  (LIST_SIZE=4096 CHURN=3072)
#   random -- multi-slot random      : txn_list  vs rlu_list  (BENCH_RANDOM_POS LIST_SIZE=1000 CHURN=64)
#   hash   -- hash-of-lists          : txn_hlist vs rlu_hlist (1000 x 100, one shared domain)
# Plus a hash read-scaling pass (readers + 1 writer) for the @191 read number.
# RLU in both modes: defer = BENCH_RLU_WS=100, sync = BENCH_RLU_WS=1.
#
# Only the *hash* panel changed structurally this session (bidir bucket ->
# singly-linked hlist); the two list panels use txn_list (unchanged) and are
# re-run here only to rebuild the figure from one consistent dataset.  Build
# self-check: disjoint-churn txn_list should hit ~185 Mops/s @192 -- if it lands
# ~128, the originals used JEMALLOC=1 PCPU=1 (rerun with PCPU=1).
#
# Output: scripts/rlu_vs_txn.csv  (panel,mode,engine,run,x,a,b,viol).
set -u
cd /mnt/data/efficios/git/efficios-trie-benchmark
export DURATION_SEC=3
RUNS=2
OUT=scripts/rlu_vs_txn.csv
TXN_TREE=/mnt/data/efficios/git/userspace-rcu-txn

echo ">> building JEMALLOC=1 against the local txn tree (forced clean) ..." >&2
rm -f bench_list_scale src/bench_list_scale.o
make bench_list_scale JEMALLOC=1 URCU_TXN_BUILD="$TXN_TREE" >/dev/null 2>&1 \
  || { echo "BUILD FAILED" >&2; exit 1; }
ldd bench_list_scale 2>/dev/null | grep -qi jemalloc \
  || { echo "jemalloc NOT linked -- aborting" >&2; exit 1; }

echo "panel,mode,engine,run,x,a,b,viol" > "$OUT"

# writescale <panel> <engine> <label> <extra-env...>  (writers 1..192, 0 readers)
writescale() {
  local panel=$1 eng=$2 lbl=$3; shift 3
  for r in $(seq 1 $RUNS); do
    echo ">> $panel write $lbl run=$r" >&2
    env BENCH_WRITESCALE=1 BENCH_READERS=0 "$@" ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v P="$panel" -v L="$lbl" -v R="$r" '/^[0-9]/{print P",write,"L","R","$1","$2","$3","$4}' >> "$OUT"
  done
}

# ---- panel 1: disjoint strided churn (txn_list vs rlu_list) ----
export LIST_SIZE=4096 CHURN=3072
writescale churn txn_list txn_list
writescale churn rlu_list rlu_defer BENCH_RLU_WS=100
writescale churn rlu_list rlu_sync  BENCH_RLU_WS=1
unset LIST_SIZE CHURN

# ---- panel 2: multi-slot random on a hot 64-slot index ----
export LIST_SIZE=1000 CHURN=64 BENCH_RANDOM_POS=1
writescale random txn_list txn_list
writescale random rlu_list rlu_defer BENCH_RLU_WS=100
writescale random rlu_list rlu_sync  BENCH_RLU_WS=1
unset LIST_SIZE CHURN BENCH_RANDOM_POS

# ---- panel 3: hash-of-lists (txn_hlist vs rlu_hlist), one shared domain ----
export HL_BUCKETS=1000 HL_INIT=100000 HL_RANGE=200000
writescale hash txn_hlist txn_hlist
writescale hash rlu_hlist rlu_defer BENCH_RLU_WS=100
writescale hash rlu_hlist rlu_sync  BENCH_RLU_WS=1
# hash read-scaling (readers + 1 writer) for the @191 read number
for c in "txn_hlist|txn_hlist|" "rlu_defer|rlu_hlist|BENCH_RLU_WS=100" "rlu_sync|rlu_hlist|BENCH_RLU_WS=1"; do
  IFS='|' read -r lbl eng extra <<<"$c"
  for r in $(seq 1 $RUNS); do
    echo ">> hash read $lbl run=$r" >&2
    env $extra ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "hash,read,"L","R","$1","$2","$3","$4}' >> "$OUT"
  done
done

echo "# ALL DONE -> $OUT" >&2
