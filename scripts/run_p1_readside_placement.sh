#!/bin/bash
# run_p1_readside_placement.sh -- does memory placement move P1's read figures?
#
# The writer-scaling sweep found that where memory lives decides its midrange
# (run_p1_writer_scaling.sh, MEMORY PLACEMENT): with writers on socket 0 and
# memory interleaved over both sockets -- the harness default -- runs were
# bistable, and confined to socket 0 they were tight.  P1's read figures are
# taken under the same default.  This checks whether they move when the memory
# is confined to the readers' socket.
#
# Readers 1..96 (MAXT=96: the harness pins reader i to core i, so every reader
# runs on socket 0), no writer, each point after BENCH_WARMUP_SEC=4 untimed:
#   cond=all      harness default, memory interleaved over all NUMA nodes
#   cond=socket0  numactl --interleave=<socket 0's nodes>, BENCH_NUMA_INTERLEAVE=0
# Arms, in both layouts (walk order; BENCH_SHUFFLE=1 scattered):
#   rculist, rculist_resolve, txn_sw_fwd (BENCH_SU_FORWARD=1) -- the
#   run_p1_resolve_control.sh arms
# and, scattered only, the fig:readclass arms: txn_sw_list (forward+backward),
# existence, rlu (BENCH_RLU_WS=100).
# RUNS runs, condition order alternated per run.
#
# RESULT (scripts/p1_readside_placement.csv, 2026-09-29 12:32-13:06, 2 runs):
# no reader figure moved by more than 1.5% (socket0/all), except rlu at 96
# scattered readers, +3%; run-to-run spread 0-4%.  The list is read-only after
# warm-up and fits in the caches, so only writers move lines between sockets.
# P1's setup states this.  That capture ran this loop from a scratch copy that
# wrote no log; the log below is the only addition.
#
# Binary: the packed P1 read binary, as run_p1_resolve_control.sh/readclass.
#
# Writes scripts/p1_readside_placement$TAG.csv  cond,layout,arm,run,x,read_mvisits,viol
#        scripts/p1_readside_placement$TAG.log  provenance + per-invocation machine state
set -u
cd /home/efficios/git/efficios-trie-benchmark
BIN=${BIN:-./arms-p1/bench_list_scale.pin-packed-18809ea8}
ENG_TREE=${ENG_TREE:-urcu-txn-build-18809ea8}
RUNS=${RUNS:-2}
MAXT=${MAXT:-96}
TAG=${TAG:-}
OUT=scripts/p1_readside_placement$TAG.csv
LOG=scripts/p1_readside_placement$TAG.log
unset BENCH_WARMUP_SEC URCU_TXN_SLAB_MAX_MB URCU_CALL_RCU_QLEN_CAP BENCH_SHUFFLE \
      BENCH_NUMA_INTERLEAVE MALLOC_CONF

[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
ldd "$BIN" | grep -q "$ENG_TREE/src/.libs/liburcu-qsbr" \
  || { echo "ERROR: $BIN does not load liburcu from $ENG_TREE" >&2; exit 1; }

# Socket 0's NUMA nodes (this box: 0-11, one per core complex).
SOCK0_NODES=$(for n in /sys/devices/system/node/node[0-9]*; do
  c=$(cut -d- -f1 $n/cpulist | cut -d, -f1)
  [ "$(cat /sys/devices/system/cpu/cpu$c/topology/physical_package_id)" = 0 ] && basename $n | sed 's/node//'
done | sort -n | paste -sd,)

{
  echo "# run_p1_readside_placement.sh  $(date -Is)"
  echo "# bench HEAD $(git rev-parse --short=8 HEAD); bench_list_scale.c sha256 $(sha256sum src/bench_list_scale.c | cut -c1-16)"
  echo "# binary $BIN sha256 $(sha256sum "$BIN" | cut -c1-16); engine $(git -C "$ENG_TREE" rev-parse --short=8 HEAD)"
  echo "# RUNS=$RUNS MAXT=$MAXT socket 0 nodes $SOCK0_NODES"
  echo "# start: $(cat /proc/loadavg)"
} > "$LOG"

echo "cond,layout,arm,run,x,read_mvisits,viol" > "$OUT"
ARMS=("rculist|rculist|X=0|both" "rculist_resolve|rculist_resolve|X=0|both"
      "txn_sw_fwd|txn_sw_list|BENCH_SU_FORWARD=1|both"
      "txn_sw_list|txn_sw_list|X=0|scat" "existence|existence_list|X=0|scat"
      "rlu|rlu_list|BENCH_RLU_WS=100|scat")
for run in $(seq 1 "$RUNS"); do
  conds="all socket0"; [ $((run % 2)) = 0 ] && conds="socket0 all"
  for cond in $conds; do
    if [ $cond = socket0 ]; then pre="numactl --interleave=$SOCK0_NODES"; ni="BENCH_NUMA_INTERLEAVE=0"; else pre=""; ni=""; fi
    for lay in "scat|BENCH_SHUFFLE=1" "walk|X=0"; do
      IFS='|' read -r lname lenv <<<"$lay"
      for spec in "${ARMS[@]}"; do
        IFS='|' read -r lbl eng extra lays <<<"$spec"
        [ "$lays" = scat ] && [ "$lname" = walk ] && continue
        echo "## $cond $lname $lbl run=$run $(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"
        $pre env $ni $lenv $extra LIST_SIZE=10000 CHURN=200 DURATION_SEC=3 BENCH_WARMUP_SEC=4 \
          BENCH_NO_WRITER=1 "$BIN" "$eng" "$MAXT" 2>/dev/null \
          | awk -v C=$cond -v L=$lname -v A=$lbl -v R=$run '/^[0-9]/{print C","L","A","R","$1","$2","$4}' >> "$OUT"
      done
    done
  done
done

echo "# end: $(date -Is) $(cat /proc/loadavg)" >> "$LOG"
if awk -F, 'NR>1 && $7+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: nonzero coherence violations in $OUT" >&2
  exit 1
fi
echo "# ALL DONE, zero coherence violations -> $OUT" >&2
