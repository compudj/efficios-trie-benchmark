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
# RE-PINNED 2026-09-28 at urcu-txn-dev 18809ea8, P1's new pin: packed
# binaries arms-p1/bench_list_scale.pin-packed-18809ea8 (and -rawcmp), built at
# bench 974609c against urcu-txn-build-18809ea8 with the flags described here.
# The results taken on b3e23f9f are in this file's and its outputs' git history.
set -u
cd /home/efficios/git/efficios-trie-benchmark
BIN=${BIN:-./arms-p1/bench_list_scale.pin-packed-18809ea8}
# WARM-UP (the rule for every paper benchmark, 2026-09-29): each process runs
# BENCH_WARMUP_SEC (default 4) seconds untimed, then a 6 s timed window, and
# perf records only inside it -- launched with events disabled (-D -1) and
# enabled through its control fifo 0.5 s into the window, for 4 s.  Recording
# the whole process, as this script used to, profiles the cold start too.
export BENCH_WARMUP_SEC=${BENCH_WARMUP_SEC:-4}
DUR=6; LAG=0.5; WIN=4
OUT=scripts/p1_writer_profile.csv
mkdir -p arms-p1/prof
echo "engine,thread,category,pct_of_process,units_per_update" > "$OUT"
for spec in "txn_sw_list|BENCH_SU_NOLOCK=1" "rculist|BENCH_RL_NOLOCK=1"; do
  IFS='|' read -r eng extra <<<"$spec"
  data=arms-p1/prof/writer_$eng.data
  err=arms-p1/prof/writer_$eng.stderr
  res=arms-p1/prof/writer_$eng.out
  ctl=arms-p1/prof/writer.ctl
  rm -f "$ctl"; mkfifo "$ctl"; : > "$err"
  env LIST_SIZE=10000 CHURN=200 DURATION_SEC=$DUR BENCH_WRITESCALE=1 BENCH_READERS=0 $extra \
    perf record -q -D -1 --control="fifo:$ctl" -e cycles:pp -o "$data" "$BIN" "$eng" 1 \
    > "$res" 2> "$err" &
  pid=$!
  t=0
  until grep -q 's/point' "$err"; do
    sleep 0.05; t=$((t + 1))
    [ $t -gt 600 ] && { echo "!! $eng did not start" >&2; kill $pid; exit 1; }
  done
  sleep "$(awk -v w="$BENCH_WARMUP_SEC" -v l="$LAG" 'BEGIN{print w + l}')"
  echo enable > "$ctl"; sleep $WIN; echo disable > "$ctl"
  wait $pid
  rate=$(awk '/^[0-9]/{print $3}' "$res")
  echo "# $eng: $rate Mupdates/s" >&2
  python3 scripts/p1_writer_profile_categorize.py "$eng" "$data" "$rate" "$WIN" >> "$OUT"
done
rm -f arms-p1/prof/writer.ctl
