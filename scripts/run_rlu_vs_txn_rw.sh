#!/bin/bash
# run_rlu_vs_txn_rw.sh -- regenerate figures/rlu_vs_txn_rw.png: RLU vs txn read /
# write / 50-50 scaling at a representative 10000-node working set with a 2% update
# set (200 churn nodes), large enough that a read traverses a real cache-pressured
# span and each writer touches a spread-out node instead of one L1-resident node.
#
# txn_list runs on its SHIPPING config -- glibc + the descriptor slab, no jemalloc
# (it ties jemalloc's per-CPU arenas; see run_list_scale_alloc.sh) -- and RLU runs
# on glibc, deferred (BENCH_RLU_WS=100) and synchronous (=1).  The engine defaults
# to the shipping config in the headers, so `make bench_list_scale` needs no flags.
#
# Reconstructed generator for what was previously an ad-hoc run (README RLU-vs-txn
# read/write/50-50 tables); writes scripts/rlu_vs_txn_rw.csv:
#   mode,engine,run,x,read_gvisits,write_mops,viol
# where mode in {read,write,5050}, x is readers (read), writers (write), or total
# threads (5050).  read_gvisits is 0 for the write mode, write_mops 0 for read.
set -u
cd /mnt/data/efficios/git/efficios-trie-benchmark
export DURATION_SEC=${DURATION_SEC:-3}
export LIST_SIZE=10000 CHURN=200		# 10k nodes, 2% update set
RUNS=${RUNS:-2}
OUT=scripts/rlu_vs_txn_rw.csv
TXN_TREE=${TXN_TREE:-/mnt/data/efficios/git/userspace-rcu-txn}

echo ">> building glibc + descriptor slab (no jemalloc), forced clean ..." >&2
rm -f bench_list_scale src/bench_list_scale.o
make bench_list_scale URCU_TXN_BUILD="$TXN_TREE" >/dev/null 2>&1 \
  || { echo "BUILD FAILED" >&2; exit 1; }

echo "mode,engine,run,x,read_gvisits,write_mops,viol" > "$OUT"

# each engine: label|bench-engine|extra-env
ENGINES="txn_list|txn_list| rlu_defer|rlu_list|BENCH_RLU_WS=100 rlu_sync|rlu_list|BENCH_RLU_WS=1"

for spec in $ENGINES; do
  IFS='|' read -r lbl eng extra <<<"$spec"
  for r in $(seq 1 $RUNS); do
    # read scaling: readers 1..191 + 1 writer -> col1=readers col2=read_mvisits
    echo ">> read  $lbl run=$r" >&2
    env $extra ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "read,"L","R","$1","$2",0,"$4}' >> "$OUT"
    # write scaling: writers 1..192, 0 readers -> col1=writers col3=write_mops
    echo ">> write $lbl run=$r" >&2
    env BENCH_WRITESCALE=1 BENCH_READERS=0 $extra ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "write,"L","R","$1",0,"$3","$4}' >> "$OUT"
    # 50/50 balanced: T/2 readers + T/2 writers -> col1=total col4=read col5=write
    echo ">> 5050  $lbl run=$r" >&2
    env BENCH_RW_BALANCED=1 $extra ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "5050,"L","R","$1","$4","$5","$6}' >> "$OUT"
  done
done
echo "# ALL DONE -> $OUT" >&2
