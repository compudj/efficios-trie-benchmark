#!/bin/bash
# run_p1_readers.sh -- P1 (single-writer flip-latch): the READER AXIS ONLY.
#
# SCOPE, and it is deliberate.  P1's facility is single-writer by contract: the
# flip-latch requires exclusion over the slots it commits.  So this harness
# sweeps READERS at zero writers and at ONE writer, and has NO writer-scaling
# panel.  Running N writers against txn_sw_list would serialize them on a mutex
# and measure the mutex, not the facility; the concurrent-writer story belongs to
# the MW engine (txn_list) and to P2.  Say that in the paper rather than letting
# the omission read as avoidance.
#
# Restricting to readers is what makes the cross-scheme comparison sound:
#
#   * RLU and MV-RLU give readers a coherent SNAPSHOT; txn_sw_list gives monotone
#     resolution ("never new-then-old"), which is strictly less.  Comparing READ
#     cost across that gap is honest in the direction that matters -- they charge
#     readers more and deliver more, and the gap is what the snapshot costs.
#     Comparing WRITE throughput across it would not be, which is another reason
#     the writer axis is absent.
#
#   * It makes the MV-RLU number nearly independent of its clock variant.  A
#     reader reads the clock ONCE per section (mvrlu.c:1445) and mvrlu_deref only
#     COMPARES against that cached local_clk -- no clock read per dereference.
#     Amortized over a 10000-node traversal, gclk's shared load vs ordo's rdtsc
#     is nothing, and the per-deref comparison is a register op either way.  With
#     no writer there are no commits at all, so p_copy is NULL and the
#     version-chain branch never runs.  This matters because the ordo variant
#     needs a per-machine __ORDO_BOUNDARY that correctness depends on and that
#     has never been measured here (third_party/mvrlu/PROVENANCE.txt).
#
# Config: jemalloc per-CPU arenas, per-CPU call_rcu workers, one hw thread per
# core (hwloc pins worker idx 0-191 one-per-core; sweeps capped at 192).
# MV-RLU's QP thread is confined by the engine default (NUMA node of CPU 0).
#
# Writes scripts/p1_readers.csv:
#   mode,engine,run,x,read_mvisits,write_mops,viol
# modes:
#   readceil  -- no writer.  x = readers.  The read ceiling.
#   mixed     -- ONE writer + x readers.  Reads under concurrent mutation, and
#                each engine's writer rate in that regime.
#   writetax  -- ONE writer + x readers, txn_sw_list vs plain-RCU rculist, each
#                with and without the bench writer mutex.  This REPLACES the
#                BENCH_READERS=0 measurement behind P1's current 2.84x: that
#                figure is a no-reader number, and the regime P1 targets has
#                readers.  Expect the tax to come out LOWER (~2x) here.
set -u
cd /home/efficios/git/efficios-trie-benchmark
export DURATION_SEC=${DURATION_SEC:-3}
export LIST_SIZE=${LIST_SIZE:-10000} CHURN=${CHURN:-200}
RUNS=${RUNS:-2}
MAXT=${MAXT:-192}
OUT=scripts/p1_readers.csv

echo ">> building bench_list_scale JEMALLOC=1 (per-CPU arenas) ..." >&2
make bench_list_scale JEMALLOC=1 >/dev/null 2>&1 \
  || { echo "BUILD FAILED" >&2; exit 1; }
ldd ./bench_list_scale | grep -qi jemalloc \
  || { echo "ERROR: binary not linked against jemalloc" >&2; exit 1; }

echo "mode,engine,run,x,read_mvisits,write_mops,viol" > "$OUT"

# label|bench-engine|extra-env
#
# TRAVERSAL ORDER MUST MATCH THE COMPARISON, and this is not a detail: order is
# worth ~15% on this workload, far more than the per-dereference cost being
# measured.  A backward second pass re-touches the tail first -- the hottest
# lines it just loaded -- while a second forward pass restarts at the head, the
# coldest; with nodes interleaved across 24 NUMA nodes that dominates.  Comparing
# forward+backward against forward+forward measures the access pattern, not the
# engine.  (Measured: rculist gains ~15% just by reversing its second pass.  An
# earlier read-ceiling table showed txn_sw ~8% AHEAD of plain RCU purely from
# this asymmetry; under either symmetric pairing the two are within a few
# percent.)  So we carry TWO txn_sw arms:
#
#   txn_sw_fwd (BENCH_SU_FORWARD=1) -- forward twice, matching rculist exactly.
#       This is the arm to compare against rculist: plain RCU has NO coherent
#       backward walk under mutation, so the capability is not shared and the
#       only honest comparison holds the access pattern fixed and measures the
#       resolve.
#   txn_sw_list (default)           -- forward then backward.
#       This is the arm to compare against RLU, MV-RLU and existence: all of
#       them DO deliver a coherent bidirectional walk, so forward+backward is
#       the shared capability and the natural workload, and every engine here
#       pays the same access pattern.
#
# rculist is the class-matched plain-RCU baseline.  RLU runs in both deferral
# modes, as the other RLU figures do (identical with no writer, a useful check).
ENGINES="txn_sw_list|txn_sw_list|X=0 txn_sw_fwd|txn_sw_list|BENCH_SU_FORWARD=1 rculist|rculist|X=0 rlu_defer|rlu_list|BENCH_RLU_WS=100 rlu_sync|rlu_list|BENCH_RLU_WS=1 mvrlu_list|mvrlu_list|X=0 existence_list|existence_list|X=0"

for r in $(seq 1 "$RUNS"); do
  for spec in $ENGINES; do
    IFS='|' read -r lbl eng extra <<<"$spec"
    echo ">> readceil $lbl run=$r" >&2
    env BENCH_NO_WRITER=1 $extra ./bench_list_scale "$eng" "$MAXT" 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "readceil,"L","R","$1","$2",0,"$4}' >> "$OUT"
    echo ">> mixed    $lbl run=$r" >&2
    env $extra ./bench_list_scale "$eng" "$MAXT" 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "mixed,"L","R","$1","$2","$3","$4}' >> "$OUT"
  done
done

# Writer tax under readers: the four variants P1's tab:writecost contrasts,
# but with a reader present.  no-lock is the idealized floor; mutex is the
# realistic deployment (the SW contract assumes the caller holds a lock anyway).
TAX="rculist_nolock|rculist|BENCH_RL_NOLOCK=1 rculist_mutex|rculist|X=0 txn_nolock|txn_sw_list|BENCH_SU_NOLOCK=1 txn_mutex|txn_sw_list|X=0"
for r in $(seq 1 "$RUNS"); do
  for spec in $TAX; do
    IFS='|' read -r lbl eng extra <<<"$spec"
    echo ">> writetax $lbl run=$r" >&2
    env $extra ./bench_list_scale "$eng" "$MAXT" 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "writetax,"L","R","$1","$2","$3","$4}' >> "$OUT"
  done
done

echo ">> done -> $OUT" >&2
if awk -F, 'NR>1 && $7+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: nonzero coherence violations in $OUT" >&2
  exit 1
fi
echo "# ALL DONE, zero coherence violations -> $OUT" >&2
