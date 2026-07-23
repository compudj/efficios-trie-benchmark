#!/bin/bash
# run_p1_sw_list.sh -- P1 (single-writer flip-latch) bidirectional-list numbers.
#
# Two existence-result panels, same-guarantee-class only (no RLU cross-class):
#
#   READ ceiling (BENCH_NO_WRITER): read_mvisits vs reader count, 1..192.
#     txn_sw_list (coherent bidir, forward+reverse walk) vs rculist (plain RCU,
#     two forward passes = same visit count) vs the lock/seqlock readers that
#     tax every traversal.  Shows the txn layer adds nothing to reads.
#
#   WRITE tax (BENCH_WRITESCALE, single writer): write_mops at writers=1.
#     txn_sw_list with the benchmark writer mutex DROPPED (BENCH_SU_NOLOCK) --
#     the raw single-updater cost -- against the plain-RCU single-writer floor
#     rculist with its writer mutex DROPPED (BENCH_RL_NOLOCK).  RCU held constant
#     (both call_rcu-reclaim, both no writer lock); the delta is the txn layer's
#     cost of a reader-coherent reverse walk that rculist cannot provide at all.
#
# Config honours the session requirements: jemalloc per-CPU arenas (JEMALLOC=1),
# per-CPU call_rcu workers (default ON), one hw thread per core (hwloc pins the
# first 192 worker indices one-per-physical-core; every sweep caps at 192).
#
# Writes scripts/p1_sw_list.csv:  mode,engine,run,x,read_mvisits,write_mops,viol
#   mode in {read,write}; x = readers (read) or writers (write);
#   read_mvisits 0 in write rows, write_mops 0 in read rows.
set -u
cd /mnt/data/efficios/git/efficios-trie-benchmark
export DURATION_SEC=${DURATION_SEC:-3}
export LIST_SIZE=${LIST_SIZE:-10000} CHURN=${CHURN:-200}	# 10k nodes, 2% update set
RUNS=${RUNS:-2}
OUT=scripts/p1_sw_list.csv

echo ">> building bench_list_scale JEMALLOC=1 (per-CPU arenas) ..." >&2
make bench_list_scale JEMALLOC=1 >/dev/null 2>&1 \
  || { echo "BUILD FAILED" >&2; exit 1; }
ldd ./bench_list_scale | grep -qi jemalloc \
  || { echo "ERROR: binary not linked against jemalloc" >&2; exit 1; }

echo "mode,engine,run,x,read_mvisits,write_mops,viol" > "$OUT"

# ---- READ ceiling: readers 1..192, no writer (col1=readers col2=read_mvisits col4=viol) ----
READ_ENGINES="txn_sw_list rculist seqlock rwlock_r iscrw"
for eng in $READ_ENGINES; do
  for r in $(seq 1 "$RUNS"); do
    echo ">> read  $eng run=$r" >&2
    BENCH_NO_WRITER=1 ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v E="$eng" -v R="$r" '/^[0-9]/{print "read,"E","R","$1","$2",0,"$4}' >> "$OUT"
  done
done

# ---- WRITE tax: single writer (col1=writers col3=write_mops col4=viol) ----
# Four variants isolate the uncontended-mutex cost from the txn-layer cost, on
# both the plain-RCU list and the SW-txn list.  no-lock = idealized floor;
# mutex = the realistic single-writer-under-a-lock deployment (the SW contract
# assumes the caller holds a lock over the slots anyway).
# spec: label|bench-engine|extra-env   (label distinguishes the CSV rows)
WRITE_ENGINES="rculist_nolock|rculist|BENCH_RL_NOLOCK=1 rculist_mutex|rculist| txn_nolock|txn_sw_list|BENCH_SU_NOLOCK=1 txn_mutex|txn_sw_list|"
for spec in $WRITE_ENGINES; do
  IFS='|' read -r lbl eng extra <<<"$spec"
  for r in $(seq 1 "$RUNS"); do
    echo ">> write $lbl run=$r" >&2
    env BENCH_WRITESCALE=1 BENCH_READERS=0 $extra ./bench_list_scale "$eng" 1 2>/dev/null \
      | awk -v E="$lbl" -v R="$r" '/^[0-9]/{print "write,"E","R","$1",0,"$3","$4}' >> "$OUT"
  done
done

echo ">> done -> $OUT" >&2
# quick coherence gate: any nonzero viol is a hard fail
if awk -F, 'NR>1 && $7+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "WARNING: nonzero monotonicity violations present in $OUT" >&2
fi
