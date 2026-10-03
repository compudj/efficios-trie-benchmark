#!/bin/bash
# run_p1_writer_profile.sh -- where P1's single writer spends its cycles,
# against plain RCU's, on the guard-free harness.
#
# P1's "Where the writer's cost goes" profiles one writer with ONE READER
# running, nodes in walk order, no bench writer mutex (BENCH_SU_NOLOCK /
# BENCH_RL_NOLOCK), so the profile holds the list operation and its reclaim and
# nothing the harness adds.  Both lists are profiled with cycles:pp, and each
# thread's samples are grouped into categories by
# scripts/p1_writer_profile_categorize.py.  The writer thread sets the update
# rate; its call_rcu worker reclaims on the writer's own hardware thread.
#
# WHY ONE READER (2026-10-03).  Up to P1's 18809ea8 pin this profiled one writer
# with NO reader, "the widest ratio".  In that configuration rculist is not in
# steady state: its writer defers nodes faster than the worker it shares a
# hardware thread with frees them -- the worker runs half the cpu, never
# sleeps, and the process grows 140-460 MiB/s (27 M updates/s without the
# mutex, 23 with it, 31 with the worker given the sibling thread).  The
# transacted list holds flat there, so the ratio divided by a rate plain RCU
# cannot sustain.  With one reader both lists hold memory flat (rculist 16 M
# updates/s, worker at 44% and sleeping between batches).  Scattered nodes with
# one reader and no mutex put rculist back over the edge (19.6 M updates/s,
# +38 MiB/s), which is why this is walk order.  The script logs each arm's
# resident set at both ends of the window: a profile whose RSS grows is not one
# to cite.
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
# RE-PINNED 2026-10-03 at urcu-txn-dev 2793224e, P1's new pin.  Against 18809ea8
# the engine retires descriptors in batches by default (c21f5a38), inlines the
# record append and no longer rounds reserve() up to 8 records (79ef08e8), reads
# the cpu from the C library's rseq area in the slab (4de8fabf) and issues no
# legacy barrier beside the slab freelist's cmpxchg (2793224e).
# Packed binaries arms-p1/bench_list_scale.pin-packed-2793224e (and -rawcmp;
# -nobatch is the same source with -DURCU_TXN_SLAB_NO_BATCH, the control; -rseq
# is built against urcu-txn-build-2793224e-rseq, configured --enable-slab-rseq),
# built at bench a9c9300 (harness source unchanged since 974609c) with the flags
# described here, against urcu-txn-build-2793224e.  THAT TREE'S LIBRARIES ARE
# -O2 -DNDEBUG (the Makefile's URCU_CFLAGS).  urcu-txn-build-18809ea8 was
# configured bare, so every result taken on 18809ea8 ran liburcu's call_rcu and
# grace-period code at -g -O2 WITH ASSERTIONS; only this translation unit, where
# the engine's headers are inlined, was -O2 -DNDEBUG.  Those results are in this
# file's and its outputs' git history; a first take on c21f5a38 was not kept.
set -u
cd /home/efficios/git/efficios-trie-benchmark
BIN=${BIN:-./arms-p1/bench_list_scale.pin-packed-2793224e}
# WARM-UP (the rule for every paper benchmark, 2026-09-29): each process runs
# BENCH_WARMUP_SEC (default 4) seconds untimed, then a 6 s timed window, and
# perf records only inside it -- launched with events disabled (-D -1) and
# enabled through its control fifo 0.5 s into the window, for 4 s.  Recording
# the whole process, as this script used to, profiles the cold start too.
export BENCH_WARMUP_SEC=${BENCH_WARMUP_SEC:-4}
DUR=6; LAG=0.5; WIN=4
# TAG suffixes the csv and the perf.data files, one per binary: TAG=_nobatch
# with BIN=...-packed-2793224e-nobatch is the per-descriptor call_rcu control.
TAG=${TAG:-}
OUT=scripts/p1_writer_profile$TAG.csv
# The categorizer splits sched_getcpu by call count: 1.5 call_rcu per update
# where each descriptor is deferred on its own, 0.5 (its default) in batches.
case "$BIN" in *-nobatch) export P1_CALL_RCU_PER_UPDATE=${P1_CALL_RCU_PER_UPDATE:-1.5} ;; esac
# ...and gives the slab none of it where the slab takes the cpu from rseq.
case "$BIN" in *-rseq) export P1_SLAB_GETCPU_CALLS=${P1_SLAB_GETCPU_CALLS:-0} ;; esac
mkdir -p arms-p1/prof
echo "engine,thread,category,pct_of_process,units_per_update" > "$OUT"
READERS=${READERS:-1}
# EVENT: cycles:pp (the default, and what P1's shares were taken with up to the
# 18809ea8 pin) is IBS here, which tags an op as it ENTERS the pipeline: a stall
# is charged to the instructions that follow it, a reorder buffer's worth
# downstream, so cost moves between categories from one build to the next.
# EVENT=cycles samples the plain counter at retirement, so a stall lands on the
# instruction that waits (one later, by skid).  P1 QUOTES THAT ONE since the
# 2793224e pin: `EVENT=cycles TAG=_ret` writes scripts/p1_writer_profile_ret.csv,
# in cycles per update (and _ret_nobatch / _ret_rseq for the two other builds).
EVENT=${EVENT:-cycles:pp}
for spec in "txn_sw_list|BENCH_SU_NOLOCK=1" "rculist|BENCH_RL_NOLOCK=1"; do
  IFS='|' read -r eng extra <<<"$spec"
  data=arms-p1/prof/writer${TAG}_$eng.data
  err=arms-p1/prof/writer${TAG}_$eng.stderr
  res=arms-p1/prof/writer${TAG}_$eng.out
  ctl=arms-p1/prof/writer.ctl
  rm -f "$ctl"; mkfifo "$ctl"; : > "$err"
  env LIST_SIZE=10000 CHURN=200 DURATION_SEC=$DUR BENCH_WRITESCALE=1 BENCH_FIXED_WRITERS=1 \
    BENCH_READERS=$READERS $extra \
    perf record -q -D -1 --control="fifo:$ctl" -e "$EVENT" -o "$data" "$BIN" "$eng" 192 \
    > "$res" 2> "$err" &
  pid=$!
  t=0
  until grep -q 's/point' "$err"; do
    sleep 0.05; t=$((t + 1))
    [ $t -gt 600 ] && { echo "!! $eng did not start" >&2; kill $pid; exit 1; }
  done
  sleep "$(awk -v w="$BENCH_WARMUP_SEC" -v l="$LAG" 'BEGIN{print w + l}')"
  hp=$(pgrep -P $pid | head -1)		# the harness, perf's child
  rss0=$(awk '/VmRSS/{print int($2 / 1024)}' /proc/$hp/status)
  echo enable > "$ctl"; sleep $WIN; echo disable > "$ctl"
  rss1=$(awk '/VmRSS/{print int($2 / 1024)}' /proc/$hp/status)
  wait $pid
  rate=$(awk '/^[0-9]/{print $3}' "$res")
  echo "# $eng: $rate Mupdates/s, $READERS reader(s), RSS $rss0 -> $rss1 MiB over the ${WIN} s window" >&2
  python3 scripts/p1_writer_profile_categorize.py "$eng" "$data" "$rate" "$WIN" >> "$OUT"
done
rm -f arms-p1/prof/writer.ctl
