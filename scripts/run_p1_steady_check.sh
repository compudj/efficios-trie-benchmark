#!/bin/bash
# run_p1_steady_check.sh -- is each one-writer point P1 plots in steady state?
#
# A writer's rate only means something if its reclaim keeps up with it.  The
# harness pins a writer's call_rcu worker to the writer's own hardware thread
# (bench_list_scale.c, reclaim_workers_setup), and call_rcu applies no
# backpressure, so a writer that defers faster than its worker frees simply
# runs ahead: the process grows, and the rate it reports is one it cannot
# sustain.  Found 2026-10-03 for rculist with one writer and no reader: the
# worker ran half the cpu, never slept, and the process grew 140-460 MiB/s.
#
# For each point of fig:writecost -- one writer under the bench writer mutex,
# READERS readers, both layouts -- and for rculist and txn_sw_list, this runs
# the point once (BENCH_WARMUP_SEC untimed, then a window of WIN seconds) and
# records, over the window:
#   the update rate the harness reports for the point,
#   the process's resident set at both ends,
#   the worker's share of the window and how often it slept (schedstat and
#     voluntary context switches of the busy thread created first: the
#     worker predates the writer).
# A point is in steady state when its resident set does not grow; a worker that
# runs ~50% and never sleeps is the same finding seen from the scheduler.
#
# Writes scripts/p1_steady_check$TAG.csv
#   layout,engine,readers,write_mops,rss_start_mib,rss_end_mib,worker_run_pct,worker_sleeps
set -u
cd /home/efficios/git/efficios-trie-benchmark
export BENCH_WARMUP_SEC=${BENCH_WARMUP_SEC:-4}
BIN=${BIN:-./arms-p1/bench_list_scale.pin-packed-2793224e}
TAG=${TAG:-}
READERS=${READERS:-"0 1 2 4 8"}
WIN=4
OUT=scripts/p1_steady_check$TAG.csv
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
echo "# run_p1_steady_check.sh $(date -Is); binary $BIN sha256 $(sha256sum "$BIN" | cut -c1-16); load $(cut -d' ' -f1-3 /proc/loadavg)" >&2
echo "layout,engine,readers,write_mops,rss_start_mib,rss_end_mib,worker_run_pct,worker_sleeps" > "$OUT"

snap() {	# PID FILE-PREFIX
  grep -H . /proc/$1/task/*/schedstat 2>/dev/null \
    | sed 's|/proc/[0-9]*/task/||; s|/schedstat:| |' > "$2.ss"
  grep -H '^voluntary_ctxt_switches' /proc/$1/task/*/status 2>/dev/null \
    | sed 's|/proc/[0-9]*/task/||; s|/status:voluntary_ctxt_switches:| |' > "$2.cs"
  awk '/VmRSS/{print int($2 / 1024)}' /proc/$1/status > "$2.rss"
}

for lay in "walk|X=0" "scattered|BENCH_SHUFFLE=1"; do
  IFS='|' read -r lname lenv <<<"$lay"
  for r in $READERS; do
    for eng in rculist txn_sw_list; do
      : > "$tmp/err"
      env LIST_SIZE=10000 CHURN=200 DURATION_SEC=$((WIN + 3)) BENCH_WRITESCALE=1 \
          BENCH_FIXED_WRITERS=1 BENCH_READERS="$r" $lenv \
          "$BIN" "$eng" 192 > "$tmp/out" 2> "$tmp/err" &
      pid=$!; t=0
      until grep -q 's/point' "$tmp/err"; do
        sleep 0.05; t=$((t + 1))
        [ $t -gt 600 ] && { echo "!! $eng did not start" >&2; kill $pid; exit 1; }
      done
      sleep "$(awk -v w="$BENCH_WARMUP_SEC" 'BEGIN{print w + 1}')"
      snap $pid "$tmp/a"; sleep $WIN; snap $pid "$tmp/b"
      wait $pid
      rate=$(awk '/^[0-9]/{print $3}' "$tmp/out")
      python3 - "$tmp" "$WIN" "$lname" "$eng" "$r" "$rate" >> "$OUT" <<'PY'
import sys
tmp, win, lay, eng, r, rate = sys.argv[1], float(sys.argv[2]), *sys.argv[3:7]
def col(f):
    return {l.split()[0]: l.split()[1:] for l in open(f) if len(l.split()) > 1}
a, b = col(tmp + '/a.ss'), col(tmp + '/b.ss')
ca, cb = col(tmp + '/a.cs'), col(tmp + '/b.cs')
busy = sorted(int(t) for t in b if t in a and
              (int(b[t][0]) - int(a[t][0])) / 1e9 > 0.02 * win)
w = str(busy[0]) if busy else None		# created first: the worker
run = 100 * (int(b[w][0]) - int(a[w][0])) / 1e9 / win if w else 0
slp = int(cb[w][0]) - int(ca[w][0]) if w else 0
print(f"{lay},{eng},{r},{rate},{open(tmp + '/a.rss').read().strip()},"
      f"{open(tmp + '/b.rss').read().strip()},{run:.1f},{slp}")
PY
      tail -1 "$OUT" >&2
    done
  done
done
echo "# done -> $OUT" >&2
