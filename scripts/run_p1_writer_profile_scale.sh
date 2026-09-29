#!/bin/bash
# run_p1_writer_profile_scale.sh -- why plain RCU outruns P1 under the same
# striped locks as writers scale.
#
# In the writer-scaling sweep (run_p1_writer_scaling.sh) rculist leads
# txn_sw_list, both under address-keyed stripes (BENCH_NODE_LOCKS=1), by 2.65x
# at 1 writer and by 1.2-1.8x from 16 writers up.  The hypothesis to test: the
# difference is the transaction's allocation and call_rcu overhead.  Per update
# txn_sw_list allocates one descriptor from the engine's per-CPU slab and makes
# 1.5 call_rcu (an insert defers its descriptor, a delete its descriptor and
# the node); rculist makes 0.5 (a delete defers the node).  Each writer PU's
# call_rcu worker is pinned to that PU (bench_list_scale.c, "honest
# accounting"), so every callback, and every worker wake-up, is paid out of
# the writer's time.
#
# Each point runs the sweep's sizing (CHURN = 64 x writers, LIST_SIZE =
# 2 x CHURN, no readers) for 4 s of warm-up and a 6 s timed window, twice:
#   stat    perf stat over the whole process -- cycles, instructions, kernel
#           cycles, context switches, migrations -- per update
#   record  perf record -F 1000 -e cycles:pp over the whole process (launched
#           under perf, events enabled for the window -- see point()), each
#           thread given a role (writer, call_rcu worker, main) and its samples
#           categorized by scripts/p1_writer_profile_scale_categorize.py
# perf measures for 4 s, starting 4.5 s after the harness announces the run:
# inside the timed window, past the warm-up.  Updates in that window are
# the harness's rate x 4 s.  The rate under each pass is logged: sampling
# perturbs it, and the record pass's units are only compared across engines.
#
# RESULT SETS (TAG), all taken at bench 0c72128 with its harness source:
#   (none)        ...-nodelock4, engine b3e23f9f: the baseline profile
#   _rep          the same, repeated after _ftwq as a drift control
#   _ftwq         ...-nodelock4-ftwq: engine b3e23f9f with the Fractal Trie's
#                 include/urcu/static/wfcqueue.h, include/urcu/wait-ladder.h and
#                 include/urcu/rcu-txn-slab.h (userspace-rcu 9484d87e) copied
#                 in, built as urcu-txn-build-b3e23f9f-ftwq (git-ignored)
#   _a69be31e     engine urcu-txn-dev a69be31e: P1 falls to ~10 Mops/s at 192
#                 writers once the slab's 1 GiB budget is spent (fixed on
#                 urcu-txn-dev by bae29fb6, 0719ceb7 and 18809ea8)
#   _a69be31e_sb  the same with -DURCU_TXN_SLAB_BATCH
#
# Writes scripts/p1_writer_profile_scale.csv
#   engine,writers,role,category,pct_of_process,units_per_update
# scripts/p1_writer_profile_scale_stat.csv
#   engine,writers,mops,cycles_per_update,instr_per_update,ipc,kernel_cycle_pct,
#   ctxsw_per_kupdate,migrations_per_kupdate
# and scripts/p1_writer_profile_scale.log (provenance, per-role top symbols).
# perf.data files go to arms-p1/prof/.
set -u
cd /home/efficios/git/efficios-trie-benchmark
BIN=${BIN:-./arms-p1/bench_list_scale.pin-aligned-nodelock4}
TAG=${TAG:-}			# output suffix, one per binary
WRITERS=${WRITERS:-"1 192"}
ENGINES=${ENGINES:-"txn_sw_list rculist"}
WARM=4; DUR=6; LAG=4.5; WIN=4
OUT=scripts/p1_writer_profile_scale$TAG.csv
STAT=scripts/p1_writer_profile_scale${TAG}_stat.csv
LOG=scripts/p1_writer_profile_scale$TAG.log
mkdir -p arms-p1/prof

{
  echo "# run_p1_writer_profile_scale.sh  $(date -Is)"
  echo "# bench HEAD $(git rev-parse --short=8 HEAD); bench_list_scale.c sha256 $(sha256sum src/bench_list_scale.c | cut -c1-16)"
  echo "# binary $BIN sha256 $(sha256sum "$BIN" | cut -c1-16); liburcu from $(ldd "$BIN" | grep -o 'urcu-txn-build[^/]*' | sort -u)"
  echo "# WRITERS=$WRITERS WARM=$WARM DUR=$DUR LAG=$LAG WIN=$WIN; start: $(cat /proc/loadavg)"
} > "$LOG"
echo "engine,writers,role,category,pct_of_process,units_per_update" > "$OUT"
echo "engine,writers,mops,cycles_per_update,instr_per_update,ipc,kernel_cycle_pct,ctxsw_per_kupdate,migrations_per_kupdate" > "$STAT"

# point ENGINE WRITERS PASS -> runs one process under PASS's perf; leaves the
# harness's output in $res and its write rate in $mops.
#
# stat attaches to the running harness.  record cannot: attaching opens a ring
# buffer per thread, and the harness runs ~580 (384 call_rcu workers, one per
# PU, whatever the writer count), past perf's locked-memory budget -- perf
# exits 255 with a truncated file.  So record launches the harness with events
# disabled (-D -1) and enables them for the same window through its control
# fifo; launched, perf inherits into the threads with per-CPU buffers.
point() {
  local eng=$1 w=$2 pass=$3 pid t=0 pre=()
  local ch=$((64 * w)) err=arms-p1/prof/scale${TAG}_${eng}_w${w}_${pass}.stderr
  local ctl=arms-p1/prof/scale.ctl
  res=arms-p1/prof/scale${TAG}_${eng}_w${w}_${pass}.out
  : > "$err"
  if [ "$pass" = record ]; then
    rm -f "$ctl"; mkfifo "$ctl"
    pre=(perf record -D -1 --control="fifo:$ctl" -F 1000 -e cycles:pp
         -o "arms-p1/prof/scale${TAG}_${eng}_w${w}.data" --)
  fi
  "${pre[@]}" setarch "$(uname -m)" -R env LIST_SIZE=$((2 * ch)) CHURN=$ch DURATION_SEC=$DUR \
    BENCH_WARMUP_SEC=$WARM BENCH_WRITESCALE=1 BENCH_FIXED_WRITERS="$w" BENCH_READERS=0 \
    BENCH_NODE_LOCKS=1 "$BIN" "$eng" 192 > "$res" 2> "$err" &
  pid=$!			# stat: setarch and env exec, so this is the harness
  until grep -q 's/point' "$err"; do
    sleep 0.05; t=$((t + 1)); [ $t -gt 600 ] && { echo "!! no start: $eng w=$w" >&2; kill $pid; return 1; }
  done
  sleep $LAG
  if [ "$pass" = stat ]; then
    perf stat -x, -o "arms-p1/prof/scale${TAG}_${eng}_w${w}.stat" \
      -e cycles,instructions,cycles:k,context-switches,cpu-migrations -p $pid -- sleep $WIN
  else
    echo enable > "$ctl"; sleep $WIN; echo disable > "$ctl"
  fi
  wait $pid
  mops=$(grep -hE '^[0-9]+ ' "$res" "$err" | tail -1 | awk '{print $3}')
  echo "## $eng w=$w $pass: $mops Mops/s  $(grep -o 'retries = [0-9]*' "$err")  load $(cut -d' ' -f1 /proc/loadavg)" >> "$LOG"
}

for w in $WRITERS; do
  for eng in $ENGINES; do
    point "$eng" "$w" stat || continue
    f=arms-p1/prof/scale${TAG}_${eng}_w${w}.stat
    awk -F, -v e="$eng" -v w="$w" -v m="$mops" -v s="$WIN" '
      $3=="cycles"{c=$1} $3=="instructions"{i=$1} $3=="cycles:k"{k=$1}
      $3=="context-switches"{x=$1} $3=="cpu-migrations"{g=$1}
      END{u=m*1e6*s; printf "%s,%s,%s,%.0f,%.0f,%.2f,%.1f,%.2f,%.3f\n",
          e,w,m,c/u,i/u,i/c,100*k/c,1000*x/u,1000*g/u}' "$f" >> "$STAT"
    point "$eng" "$w" record || continue
    python3 scripts/p1_writer_profile_scale_categorize.py "$eng" "$w" \
      "arms-p1/prof/scale${TAG}_${eng}_w${w}.data" "$mops" "$WIN" >> "$OUT" 2>> "$LOG"
  done
done
echo "# end: $(date -Is) $(cat /proc/loadavg)" >> "$LOG"
