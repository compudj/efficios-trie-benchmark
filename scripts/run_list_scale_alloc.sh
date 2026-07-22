#!/bin/bash
# run_list_scale_alloc.sh -- regenerate figures/list_scale_alloc.png: txn_list
# writer scaling under three allocators, showing the per-CPU descriptor slab
# (<urcu/rcu-txn-slab.h>) closes the allocator gap with no external allocator.
#
# Three allocators, each writer-scaled 1..192 on two workloads:
#   glibc         -- glibc malloc, slab OFF (URCU_TXN_NO_CACHE=1): the mmap_lock
#                    baseline.
#   jemalloc      -- jemalloc percpu_arena:percpu, slab OFF: external allocator
#                    hides the cross-thread free.
#   glibc + slab  -- glibc malloc, slab ON (the shipping default): the slab hides
#                    it with no external allocator.
# Workloads: plain churn (LIST_SIZE=4096 CHURN=3072, 2-3-edge MCAS) and a
# composable transacted 100k-slot index (BENCH_RANDOM_POS, CHURN=NINDEX=100000 --
# index-slot update folded into the list MCAS, one heavier commit).
#
# The slab is a runtime toggle (URCU_TXN_NO_CACHE), so only two builds are needed:
# glibc (default) and jemalloc (JEMALLOC=1).  Reconstructed generator for what was
# an ad-hoc run (README "MCAS descriptor slab" tables).  Writes
# scripts/list_scale_alloc.csv:  panel,variant,run,writers,write_mops,viol
set -u
cd /mnt/data/efficios/git/efficios-trie-benchmark
export DURATION_SEC=${DURATION_SEC:-3}
RUNS=${RUNS:-2}
OUT=scripts/list_scale_alloc.csv
TXN_TREE=${TXN_TREE:-/mnt/data/efficios/git/userspace-rcu-txn}

echo ">> building glibc + jemalloc variants (forced clean) ..." >&2
rm -f bench_list_scale src/bench_list_scale.o
make bench_list_scale URCU_TXN_BUILD="$TXN_TREE" >/dev/null 2>&1 && cp bench_list_scale /tmp/ls_glibc \
  || { echo "glibc BUILD FAILED" >&2; exit 1; }
rm -f bench_list_scale src/bench_list_scale.o
make bench_list_scale JEMALLOC=1 URCU_TXN_BUILD="$TXN_TREE" >/dev/null 2>&1 && cp bench_list_scale /tmp/ls_jem \
  || { echo "jemalloc BUILD FAILED" >&2; exit 1; }
ldd /tmp/ls_jem | grep -qi jemalloc || { echo "jemalloc NOT linked" >&2; exit 1; }

echo "panel,variant,run,writers,write_mops,viol" > "$OUT"

# ws <panel> <variant> <binary> <extra-env...>  (writescale, 0 readers)
ws() {
  local panel=$1 variant=$2 bin=$3; shift 3
  for r in $(seq 1 $RUNS); do
    echo ">> $panel $variant run=$r" >&2
    env BENCH_WRITESCALE=1 BENCH_READERS=0 "$@" "$bin" txn_list 192 2>/dev/null \
      | awk -v P="$panel" -v V="$variant" -v R="$r" '/^[0-9]/{print P","V","R","$1","$3","$4}' >> "$OUT"
  done
}

# ---- panel: plain churn ----
export LIST_SIZE=4096 CHURN=3072
ws churn glibc      /tmp/ls_glibc URCU_TXN_NO_CACHE=1
ws churn jemalloc   /tmp/ls_jem   URCU_TXN_NO_CACHE=1 MALLOC_CONF=percpu_arena:percpu
ws churn glibc+slab /tmp/ls_glibc
unset LIST_SIZE CHURN

# ---- panel: composable transacted 100k-slot index ----
export LIST_SIZE=100000 CHURN=100000 BENCH_RANDOM_POS=1
ws composable glibc      /tmp/ls_glibc URCU_TXN_NO_CACHE=1
ws composable jemalloc   /tmp/ls_jem   URCU_TXN_NO_CACHE=1 MALLOC_CONF=percpu_arena:percpu
ws composable glibc+slab /tmp/ls_glibc
unset LIST_SIZE CHURN BENCH_RANDOM_POS
echo "# ALL DONE -> $OUT" >&2
