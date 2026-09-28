#!/bin/bash
# run_p1_stall.sh -- does a per-hop stall absorb the facility's tag test?
#
# The guard-free read ceiling (run_p1_resolve_control.sh, TAG=_noguard) prices
# the tag test at 11.5-13.5% on a loop that runs at the L1 load-to-use latency
# of its pointer chase: the list is walked in arena order, so the prefetcher
# has every next line ready and each hop costs ~4 cycles.  The hypothesis
# (Mathieu's): a predicted branch nothing waits on costs issue slots, not
# latency, so once each hop stalls for longer the branch executes inside the
# stall and its cost disappears.
#
# BENCH_SHUFFLE places the stable nodes at random arena positions without
# changing list order or keys, so every hop misses L1.  One reader, no writer,
# the same five arms as the _noguard batch, over four layouts:
#
#   seq10k    10k nodes, arena order    (the _noguard batch's layout; L1 hops)
#   shuf10k   10k nodes, shuffled       (240 KB, L2-resident: L2 hops)
#   shuf1m    1M nodes, shuffled        (24 MB: L3 hops, some DRAM)
#   shuf4m    4M nodes, shuffled        (96 MB: DRAM hops)
#
# Prediction: every control's ratio to rculist goes toward 1 from shuf10k on.
#
# The binary is prebuilt (arms-p1/bench_list_scale.pin-packed-shuffle: engine
# b3e23f9f, -DLIST_NODE_ALIGN=0), built as in run_p1_resolve_control.sh from
# the source that adds BENCH_SHUFFLE.  Its reader functions are
# instruction-for-instruction those of the _noguard binary.
#
# Writes scripts/p1_stall.csv  layout,engine,run,list_size,read_mvisits,ns_per_visit,viol
#        scripts/p1_stall.log  provenance + per-invocation machine state
set -u
cd /home/efficios/git/efficios-trie-benchmark
BIN=${BIN:-./arms-p1/bench_list_scale.pin-packed-shuffle}
ENG_TREE=urcu-txn-build-b3e23f9f
RUNS=${RUNS:-5}
OUT=scripts/p1_stall.csv
LOG=scripts/p1_stall.log

[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
ldd "$BIN" | grep -q "$ENG_TREE/src/.libs/liburcu-qsbr" \
  || { echo "ERROR: $BIN does not load liburcu from $ENG_TREE" >&2; exit 1; }
"$BIN" 2>&1 | grep -q rculist_loadbr \
  || { echo "ERROR: $BIN lacks the control arms" >&2; exit 1; }

{
  echo "# run_p1_stall.sh  $(date -Is)"
  echo "# bench HEAD $(git rev-parse --short=8 HEAD); bench_list_scale.c sha256 $(sha256sum src/bench_list_scale.c | cut -c1-16)"
  echo "# binary $BIN sha256 $(sha256sum "$BIN" | cut -c1-16); engine $(git -C "$ENG_TREE" rev-parse --short=8 HEAD)"
  echo "# RUNS=$RUNS, 1 reader, no writer, CHURN=200"
  echo "# start: $(cat /proc/loadavg)"
} > "$LOG"
echo "layout,engine,run,list_size,read_mvisits,ns_per_visit,viol" > "$OUT"

# layout|LIST_SIZE|shuffle-env|DURATION_SEC
LAYOUTS=("seq10k|10000|X=0|3" "shuf10k|10000|BENCH_SHUFFLE=1|3" "shuf1m|1000000|BENCH_SHUFFLE=1|3" "shuf4m|4000000|BENCH_SHUFFLE=1|5")
# label|bench-engine|extra-env
ARMS=("rculist|rculist|X=0" "rculist_load|rculist_load|X=0" "rculist_resolve|rculist_resolve|X=0" "txn_sw_fwd|txn_sw_list|BENCH_SU_FORWARD=1" "rculist_loadbr|rculist_loadbr|X=0")
N=${#ARMS[@]}

for lay in "${LAYOUTS[@]}"; do
  IFS='|' read -r lname lsize lenv ldur <<<"$lay"
  for r in $(seq 1 "$RUNS"); do
    for i in $(seq 0 $((N - 1))); do
      spec=${ARMS[$(( (i + r - 1) % N ))]}
      IFS='|' read -r lbl eng extra <<<"$spec"
      echo ">> $lname $lbl run=$r" >&2
      {
        echo "## $lname $lbl run=$r $(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"
        top -bn1 -o %CPU | sed -n '8,10p'
      } >> "$LOG"
      env LIST_SIZE="$lsize" CHURN=200 DURATION_SEC="$ldur" BENCH_NO_WRITER=1 \
          $lenv $extra "$BIN" "$eng" 1 2>/dev/null \
        | awk -v L="$lname" -v E="$lbl" -v R="$r" -v S="$lsize" \
            '/^[0-9]/{printf "%s,%s,%s,%s,%s,%.3f,%s\n", L, E, R, S, $2, 1000/$2, $4}' >> "$OUT"
    done
  done
done

echo "# end: $(date -Is) $(cat /proc/loadavg)" >> "$LOG"
if awk -F, 'NR>1 && $7+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: nonzero coherence violations in $OUT" >&2
  exit 1
fi
echo "# ALL DONE, zero coherence violations -> $OUT" >&2
