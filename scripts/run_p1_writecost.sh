#!/bin/bash
# run_p1_writecost.sh -- re-take P1's fig:writecost on the guard-free harness,
# in both node layouts, and its no-reader ratio on the pinned engine.
#
# fig:writecost is the single writer's update rate with 1-191 concurrent
# readers, rculist against txn_sw_list, each under the (uncontended) bench
# writer mutex.  The published figure is the writetax panel of
# scripts/p1_readers.csv (e0bf1b1): readers with the runaway step guard, nodes in
# walk order.  The text's no-reader ratio (2.84) came from the older
# scripts/p1_sw_list.csv, on engine 5fc3cadf rather than P1's pin.
#
# This batch: the 06f998a binary (guard-free readers, BENCH_SHUFFLE), engine
# b3e23f9f, packed nodes; rculist and txn_sw_list with their writer mutex, each
# reader walking as in the published figure (rculist forward twice,
# txn_sw_list forward then backward).  Two layouts -- walk order, and stable
# nodes at random positions (BENCH_SHUFFLE=1) -- and two modes:
#
#   mixed    one writer + x readers, x = 1..191 (MAXT=192)
#   noread   one writer, no reader (BENCH_WRITESCALE=1 BENCH_READERS=0)
#
# 5 runs, arm order rotated per run.
#
# Writes scripts/p1_writecost.csv  layout,mode,engine,run,x,read_mvisits,write_mops,viol
#        scripts/p1_writecost.log  provenance + per-invocation machine state
set -u
cd /home/efficios/git/efficios-trie-benchmark
BIN=${BIN:-./arms-p1/bench_list_scale.pin-packed-readclass}
ENG_TREE=urcu-txn-build-b3e23f9f
export LIST_SIZE=${LIST_SIZE:-10000} CHURN=${CHURN:-200} DURATION_SEC=${DURATION_SEC:-3}
RUNS=${RUNS:-5}
MAXT=${MAXT:-192}
OUT=scripts/p1_writecost.csv
LOG=scripts/p1_writecost.log

[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
ldd "$BIN" | grep -q "$ENG_TREE/src/.libs/liburcu-qsbr" \
  || { echo "ERROR: $BIN does not load liburcu from $ENG_TREE" >&2; exit 1; }
ldd "$BIN" | grep -qi jemalloc \
  || { echo "ERROR: $BIN not linked against jemalloc" >&2; exit 1; }

{
  echo "# run_p1_writecost.sh  $(date -Is)"
  echo "# bench HEAD $(git rev-parse --short=8 HEAD); bench_list_scale.c sha256 $(sha256sum src/bench_list_scale.c | cut -c1-16)"
  echo "# binary $BIN sha256 $(sha256sum "$BIN" | cut -c1-16); engine $(git -C "$ENG_TREE" rev-parse --short=8 HEAD)"
  echo "# LIST_SIZE=$LIST_SIZE CHURN=$CHURN DURATION_SEC=$DURATION_SEC RUNS=$RUNS MAXT=$MAXT"
  echo "# start: $(cat /proc/loadavg)"
} > "$LOG"
echo "layout,mode,engine,run,x,read_mvisits,write_mops,viol" > "$OUT"

LAYOUTS=("walk|X=0" "scattered|BENCH_SHUFFLE=1")
ARMS=("rculist_mutex|rculist" "txn_mutex|txn_sw_list")
N=${#ARMS[@]}

for r in $(seq 1 "$RUNS"); do
  for lay in "${LAYOUTS[@]}"; do
    IFS='|' read -r lname lenv <<<"$lay"
    for i in $(seq 0 $((N - 1))); do
      IFS='|' read -r lbl eng <<<"${ARMS[$(( (i + r - 1) % N ))]}"
      echo ">> $lname $lbl run=$r" >&2
      {
        echo "## $lname $lbl run=$r $(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"
        top -bn1 -o %CPU | sed -n '8,10p'
      } >> "$LOG"
      env $lenv "$BIN" "$eng" "$MAXT" 2>/dev/null \
        | awk -v L="$lname" -v E="$lbl" -v R="$r" \
            '/^[0-9]/{print L",mixed,"E","R","$1","$2","$3","$4}' >> "$OUT"
      env $lenv BENCH_WRITESCALE=1 BENCH_READERS=0 "$BIN" "$eng" 1 2>/dev/null \
        | awk -v L="$lname" -v E="$lbl" -v R="$r" \
            '/^[0-9]/{print L",noread,"E","R",0,"$2","$3","$4}' >> "$OUT"
    done
  done
done

echo "# end: $(date -Is) $(cat /proc/loadavg)" >> "$LOG"
if awk -F, 'NR>1 && $8+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: nonzero coherence violations in $OUT" >&2
  exit 1
fi
echo "# ALL DONE, zero coherence violations -> $OUT" >&2
