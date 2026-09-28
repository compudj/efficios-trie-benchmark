#!/bin/bash
# run_p1_writer_profile.sh -- where P1's single writer spends its cycles,
# against plain RCU's, on the guard-free harness.
#
# P1's "Where the writer's cost goes" profiles the widest writer ratio: one
# writer, no reader, no bench writer mutex (BENCH_SU_NOLOCK / BENCH_RL_NOLOCK),
# so the profile holds the list operation and its reclaim and nothing the
# harness adds.  Both lists are profiled with cycles:pp for 5 s, and each
# thread's samples are grouped into categories by
# scripts/p1_writer_profile_categorize.py.  The writer thread sets the update
# rate; the call_rcu worker reclaims on its own core.
#
# Binary arms-p1/bench_list_scale.pin-packed-readclass (06f998a, engine
# b3e23f9f, packed nodes), walk order.  perf.data files go to arms-p1/prof/.
#
# Writes scripts/p1_writer_profile.csv
#   engine,thread,category,pct_of_process,units_per_update  (see the categorizer)
set -u
cd /home/efficios/git/efficios-trie-benchmark
BIN=${BIN:-./arms-p1/bench_list_scale.pin-packed-readclass}
SECS=5
OUT=scripts/p1_writer_profile.csv
mkdir -p arms-p1/prof
echo "engine,thread,category,pct_of_process,units_per_update" > "$OUT"
for spec in "txn_sw_list|BENCH_SU_NOLOCK=1" "rculist|BENCH_RL_NOLOCK=1"; do
  IFS='|' read -r eng extra <<<"$spec"
  data=arms-p1/prof/writer_$eng.data
  rate=$(env LIST_SIZE=10000 CHURN=200 DURATION_SEC=$SECS BENCH_WRITESCALE=1 BENCH_READERS=0 $extra \
         perf record -q -e cycles:pp -o "$data" "$BIN" "$eng" 1 2>/dev/null | awk '/^[0-9]/{print $3}')
  echo "# $eng: $rate Mupdates/s" >&2
  python3 scripts/p1_writer_profile_categorize.py "$eng" "$data" "$rate" "$SECS" >> "$OUT"
done
