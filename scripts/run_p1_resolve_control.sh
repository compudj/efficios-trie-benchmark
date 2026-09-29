#!/bin/bash
# run_p1_resolve_control.sh -- P1 read ceiling with a control arm: rculist plus
# the facility's tag test, and nothing else (Paul McKenney's suggestion).
#
# P1's sec:measured explains txn_sw_list's parity with rculist by saying the
# proxy marker rides in the word the reader loads anyway, so the only residue is
# a predicted branch.  That was inferred from the ABSENCE of a gap.  The control
# arm rculist_resolve (src/bench_list_scale.c, rl_read_resolve) wraps every
# forward dereference of rculist in urcu_txn_sw_resolve() with the list's own
# tag.  rculist never installs a proxy, so the test is never true.  Its hot loop
# is instruction-for-instruction su_read's forward loop (checked with objdump):
# rculist's loop + one `test` + one never-taken `je`, no extra load.
#
# ARMS, ONE BATCH, ONE BINARY:
#   rculist          the published baseline
#   rculist_resolve  rculist + the facility's tag test (one branch)
#   rculist_load     rculist + one load        (guard-free build only)
#   rculist_loadbr   rculist + one load + a branch on it   (guard-free build only)
#   txn_sw_fwd       the facility, forward twice (BENCH_SU_FORWARD=1)
# See the control-arm comment above rl_read_resolve in src/bench_list_scale.c.
#
# BUILDS.  The first batch (TAG empty) used arms-p1/bench_list_scale.pin-packed,
# whose read loops still carried the runaway step guard.  The guard is redundant
# with the key-order check and was the only difference between the July capture
# and that batch (see su_read), so it was removed; TAG=_noguard with
# BIN=arms-p1/bench_list_scale.pin-packed-noguard is that build, and adds the two
# load controls.
#
# LAYOUT.  BENCH_SHUFFLE=<seed> in the environment (44cd638) places the nodes at
# random memory positions, list order unchanged; the log records it.  The
# shuffled batch is TAG=_shuffled with BIN=arms-p1/bench_list_scale.pin-packed-
# readclass (06f998a), whose five reader loops are those of the _noguard binary.
# All three are re-measured here rather than set beside the July curves, because
# the harness changed since that capture (STEP_LIMIT hoisted, f16cb3f).
#
# CONFIGURATION MATCHES THE PUBLISHED fig:readcost, deliberately:
#   * engine b3e23f9f (P1's pin), built in urcu-txn-build-b3e23f9f
#   * PACKED nodes (-DLIST_NODE_ALIGN=0): the published capture predates the
#     cache-line alignment (c298aa5), and the caption says 24-byte elements
#   * jemalloc, LIST_SIZE=10000 CHURN=200 DURATION_SEC=3, no writer, MAXT=192
#   * no setarch -R, as in run_p1_readers.sh
# The binary is prebuilt (arms-p1/bench_list_scale.pin-packed) and NOT rebuilt
# here: `make bench_list_scale` would rebuild with the default 64-byte alignment
# against urcu-txn-build/, silently changing both axes.  Build it with:
#   make -B bench_list_scale JEMALLOC=1 \
#     URCU_TXN_BUILD=$PWD/urcu-txn-build-b3e23f9f \
#     OPTFLAGS="-O2 -DNDEBUG -march=native -mpopcnt -DLIST_NODE_ALIGN=0"
#   mv bench_list_scale arms-p1/bench_list_scale.pin-packed
#
# Arm order ROTATES per run so no arm always runs first or last.  Before each
# invocation the top CPU consumers are logged: the bench is not running at that
# instant, so any foreign load shows up (loadavg cannot show it -- during a sweep
# it is dominated by the sweep's own threads).
#
# Phase 2 counts per-visit instructions/branches/loads for the same three arms,
# by the differenced method of run_deref_cost.sh (1 reader, 2 s vs 8 s).
#
# Writes:
#   scripts/p1_resolve_control$TAG.csv  mode,engine,run,x,read_mvisits,write_mops,viol
#   scripts/p1_resolve_deref$TAG.csv    engine,run,seconds,mvisits_per_s,instructions,branches,loads
#   scripts/p1_resolve_control$TAG.log  provenance + per-invocation machine state
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
TAG=${TAG:-}
ENG_TREE=${ENG_TREE:-urcu-txn-build-18809ea8}
export DURATION_SEC=${DURATION_SEC:-3}
export LIST_SIZE=${LIST_SIZE:-10000} CHURN=${CHURN:-200}
RUNS=${RUNS:-5}
MAXT=${MAXT:-192}
DRUNS=${DRUNS:-2}
OUT=scripts/p1_resolve_control$TAG.csv
DOUT=scripts/p1_resolve_deref$TAG.csv
LOG=scripts/p1_resolve_control$TAG.log

[ -x "$BIN" ] || { echo "missing $BIN (see header to build it)" >&2; exit 1; }
ENG_COMMIT=${ENG_COMMIT:-18809ea8}
[ "$(git -C "$ENG_TREE" rev-parse --short=8 HEAD)" = "$ENG_COMMIT" ] \
  || { echo "ERROR: $ENG_TREE is not at $ENG_COMMIT" >&2; exit 1; }
ldd "$BIN" | grep -q "$ENG_TREE/src/.libs/liburcu-qsbr" \
  || { echo "ERROR: $BIN does not load liburcu from $ENG_TREE" >&2; exit 1; }
ldd "$BIN" | grep -qi jemalloc \
  || { echo "ERROR: $BIN not linked against jemalloc" >&2; exit 1; }
"$BIN" 2>&1 | grep -q rculist_resolve \
  || { echo "ERROR: $BIN has no rculist_resolve engine" >&2; exit 1; }

{
  echo "# run_p1_resolve_control.sh  $(date -Is)"
  echo "# bench HEAD $(git rev-parse --short=8 HEAD); bench_list_scale.c sha256 $(sha256sum src/bench_list_scale.c | cut -c1-16)"
  echo "# binary $BIN"
  echo "# binary sha256 $(sha256sum "$BIN" | cut -c1-16); engine $(git -C "$ENG_TREE" rev-parse --short=8 HEAD)"
  echo "# LIST_SIZE=$LIST_SIZE CHURN=$CHURN DURATION_SEC=$DURATION_SEC RUNS=$RUNS MAXT=$MAXT BENCH_SHUFFLE=${BENCH_SHUFFLE:-unset} BENCH_WARMUP_SEC=$BENCH_WARMUP_SEC"
  echo "# start: $(cat /proc/loadavg)"
} > "$LOG"

echo "mode,engine,run,x,read_mvisits,write_mops,viol" > "$OUT"

# label|bench-engine|extra-env
ARMS=("rculist|rculist|X=0" "rculist_resolve|rculist_resolve|X=0" "txn_sw_fwd|txn_sw_list|BENCH_SU_FORWARD=1")
if "$BIN" 2>&1 | grep -q rculist_loadbr; then
  ARMS+=("rculist_load|rculist_load|X=0" "rculist_loadbr|rculist_loadbr|X=0")
fi
N=${#ARMS[@]}

for r in $(seq 1 "$RUNS"); do
  for i in $(seq 0 $((N - 1))); do
    spec=${ARMS[$(( (i + r - 1) % N ))]}
    IFS='|' read -r lbl eng extra <<<"$spec"
    echo ">> readceil $lbl run=$r" >&2
    {
      echo "## readceil $lbl run=$r $(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"
      top -bn1 -o %CPU | sed -n '8,12p'
    } >> "$LOG"
    env BENCH_NO_WRITER=1 $extra "$BIN" "$eng" "$MAXT" 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "readceil,"L","R","$1","$2",0,"$4}' >> "$OUT"
  done
done

echo "engine,run,seconds,mvisits_per_s,instructions,branches,loads" > "$DOUT"
for r in $(seq 1 "$DRUNS"); do
  for spec in "${ARMS[@]}"; do
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
echo ">> done -> $OUT $DOUT" >&2
if awk -F, 'NR>1 && $7+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: nonzero coherence violations in $OUT" >&2
  exit 1
fi
echo "# ALL DONE, zero coherence violations -> $OUT" >&2
