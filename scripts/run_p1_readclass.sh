#!/bin/bash
# run_p1_readclass.sh -- re-take P1's fig:readclass on the guard-free harness,
# in two layouts.
#
# fig:readclass places txn_sw_list against the schemes that also deliver a
# coherent bidirectional walk: existence structures, RLU and MV-RLU, all walking
# forward then backward, no writer, 1-192 readers.  The published figure came
# from scripts/p1_readers.csv (e0bf1b1), taken before two harness changes:
#
#   * every reader's runaway step guard is gone where the key-order check makes
#     it redundant (ec54f21 for txn_sw_list/rculist; here for existence, RLU and
#     MV-RLU, whose walks are now specialized at compile time on sortedness, so
#     they no longer test the random-position flag per step either);
#   * BENCH_SHUFFLE (44cd638) can place the nodes at random memory positions.
#     In walk order the prefetcher keeps every next node in L1 and a traversal
#     pays per-step instruction cost; shuffled, each hop misses L1 and the
#     schemes are separated by what the stall does not hide.  Existence, RLU and
#     MV-RLU honour it by linking their per-node allocations in permuted order.
#
# Arms as published: txn_sw_list (forward+backward), existence_list (EXL_SPLIT,
# 32-byte element), rlu_list with BENCH_RLU_WS=100 (rlu_defer), mvrlu_list (gclk).
#
# Phase 1: read ceiling, both layouts, 5 runs, arm order rotated per run.
# Phase 2: per-node instructions/branches/loads by the differenced method of
# run_deref_cost.sh (1 reader, walk order, 2 s vs 8 s), for the same arms as
# that script, so tab:comparison's counted cells can be re-checked.
#
# The binary is prebuilt, as in run_p1_resolve_control.sh:
#   arms-p1/bench_list_scale.pin-packed-readclass (engine b3e23f9f, packed).
#
# Writes scripts/p1_readclass.csv        layout,engine,run,x,read_mvisits,viol
#        scripts/p1_readclass_deref.csv  engine,run,seconds,mvisits_per_s,instructions,branches,loads
#        scripts/p1_readclass.log        provenance + per-invocation machine state
# RE-PINNED 2026-09-28 at urcu-txn-dev 18809ea8, P1's new pin: packed
# binaries arms-p1/bench_list_scale.pin-packed-18809ea8 (and -rawcmp), built at
# bench 974609c against urcu-txn-build-18809ea8 with the flags described here.
# The results taken on b3e23f9f are in this file's and its outputs' git history.
set -u
cd /home/efficios/git/efficios-trie-benchmark
# WARM-UP: every point runs BENCH_WARMUP_SEC (default 4) seconds untimed before
# its timed window -- the rule for every paper benchmark (2026-09-29), the same
# 4 s as the writer-scaling sweep.  A process's first point otherwise times its
# own cold start: slab carving, page faults, call_rcu's pipeline filling.
export BENCH_WARMUP_SEC=${BENCH_WARMUP_SEC:-4}
BIN=${BIN:-./arms-p1/bench_list_scale.pin-packed-18809ea8}
ENG_TREE=${ENG_TREE:-urcu-txn-build-18809ea8}
export LIST_SIZE=${LIST_SIZE:-10000} CHURN=${CHURN:-200} DURATION_SEC=${DURATION_SEC:-3}
RUNS=${RUNS:-5}
MAXT=${MAXT:-192}
DRUNS=${DRUNS:-2}
TAG=${TAG:-}
OUT=scripts/p1_readclass$TAG.csv
DOUT=scripts/p1_readclass_deref$TAG.csv
LOG=scripts/p1_readclass$TAG.log

[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
ldd "$BIN" | grep -q "$ENG_TREE/src/.libs/liburcu-qsbr" \
  || { echo "ERROR: $BIN does not load liburcu from $ENG_TREE" >&2; exit 1; }
ldd "$BIN" | grep -qi jemalloc \
  || { echo "ERROR: $BIN not linked against jemalloc" >&2; exit 1; }

{
  echo "# run_p1_readclass.sh  $(date -Is)"
  echo "# bench HEAD $(git rev-parse --short=8 HEAD); sha256 bench_list_scale.c $(sha256sum src/bench_list_scale.c | cut -c1-16) bench_existence_list.c $(sha256sum src/bench_existence_list.c | cut -c1-16) bench_mvrlu_list.c $(sha256sum src/bench_mvrlu_list.c | cut -c1-16)"
  echo "# binary $BIN sha256 $(sha256sum "$BIN" | cut -c1-16); engine $(git -C "$ENG_TREE" rev-parse --short=8 HEAD)"
  echo "# LIST_SIZE=$LIST_SIZE CHURN=$CHURN DURATION_SEC=$DURATION_SEC RUNS=$RUNS MAXT=$MAXT BENCH_WARMUP_SEC=$BENCH_WARMUP_SEC"
  echo "# start: $(cat /proc/loadavg)"
} > "$LOG"
echo "layout,engine,run,x,read_mvisits,viol" > "$OUT"

LAYOUTS=("walk|X=0" "shuffled|BENCH_SHUFFLE=1")
ARMS=("txn_sw_list|txn_sw_list|X=0" "existence|existence_list|X=0" "rlu|rlu_list|BENCH_RLU_WS=100" "mvrlu|mvrlu_list|X=0")
N=${#ARMS[@]}

for r in $(seq 1 "$RUNS"); do
  for lay in "${LAYOUTS[@]}"; do
    IFS='|' read -r lname lenv <<<"$lay"
    for i in $(seq 0 $((N - 1))); do
      spec=${ARMS[$(( (i + r - 1) % N ))]}
      IFS='|' read -r lbl eng extra <<<"$spec"
      echo ">> $lname $lbl run=$r" >&2
      {
        echo "## $lname $lbl run=$r $(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"
        top -bn1 -o %CPU | sed -n '8,10p'
      } >> "$LOG"
      env BENCH_NO_WRITER=1 $lenv $extra "$BIN" "$eng" "$MAXT" 2>/dev/null \
        | awk -v L="$lname" -v E="$lbl" -v R="$r" \
            '/^[0-9]/{print L","E","R","$1","$2","$4}' >> "$OUT"
    done
  done
done

echo "engine,run,seconds,mvisits_per_s,instructions,branches,loads" > "$DOUT"
DARMS=("rculist|rculist|X=0" "txn_sw_list|txn_sw_list|BENCH_SU_FORWARD=1" "existence|existence_list|X=0" "rlu|rlu_list|BENCH_RLU_WS=100" "mvrlu|mvrlu_list|X=0")
for r in $(seq 1 "$DRUNS"); do
  for spec in "${DARMS[@]}"; do
    IFS='|' read -r lbl eng extra <<<"$spec"
    for d in 2 8; do
      echo ">> deref $lbl ${d}s run=$r" >&2
      tmp=$(mktemp)
      env DURATION_SEC="$d" BENCH_NO_WRITER=1 $extra \
        perf stat -x, -e instructions,branches,L1-dcache-loads \
        "$BIN" "$eng" 1 >"$tmp" 2>"$tmp.perf"
      rate=$(awk '/^[0-9]/{print $2; exit}' "$tmp")
      ins=$(awk -F, '$3=="instructions"{print $1}' "$tmp.perf")
      brs=$(awk -F, '$3=="branches"{print $1}' "$tmp.perf")
      lds=$(awk -F, '$3=="L1-dcache-loads"{print $1}' "$tmp.perf")
      echo "$lbl,$r,$d,${rate:-0},${ins:-0},${brs:-0},${lds:-0}" >> "$DOUT"
      rm -f "$tmp" "$tmp.perf"
    done
  done
done

echo "# end: $(date -Is) $(cat /proc/loadavg)" >> "$LOG"
if awk -F, 'NR>1 && $6+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: nonzero coherence violations in $OUT" >&2
  exit 1
fi
echo "# ALL DONE, zero coherence violations -> $OUT" >&2
