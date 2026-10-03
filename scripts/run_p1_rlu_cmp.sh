#!/bin/bash
# run_p1_rlu_cmp.sh -- what RLU's pointer comparison costs a traversal, on the
# guard-free harness (06f998a).
#
# P1 says RLU's dereference costs the one extra load its cost table states, and
# that the walk's termination test costs more: RLU_IS_SAME_PTRS resolves both
# operands to their originals in an out-of-line rlu_cmp_ptrs() call per
# iteration.  f16cb3f measured that A/B on the guarded harness (5.50 loads,
# 6.01 branches per visit).  The equalization in 06f998a changed RLU's loop, so
# the A/B is re-run: the same source built twice, once as shipped and once with
# -DRLU_RAWCMP, which ends the walk with a plain pointer compare -- sound only
# with no writer, which is the configuration measured.
#
#   arms-p1/bench_list_scale.pin-packed-readclass   RLU_IS_SAME_PTRS
#   arms-p1/bench_list_scale.pin-packed-rawcmp      -DRLU_RAWCMP
#
# Both are engine b3e23f9f, packed nodes, built from 06f998a.  One reader, no
# writer, walk order, rlu_list with BENCH_RLU_WS=100, and rculist in each binary
# as the baseline; counters differenced 8 s - 2 s as in run_deref_cost.sh.
#
# Writes scripts/p1_rlu_cmp.csv  build,engine,run,seconds,mvisits_per_s,instructions,branches,loads
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
# WARM-UP: every point runs BENCH_WARMUP_SEC (default 4) seconds untimed before
# its timed window -- the rule for every paper benchmark (2026-09-29), the same
# 4 s as the writer-scaling sweep.  A process's first point otherwise times its
# own cold start: slab carving, page faults, call_rcu's pipeline filling.
export BENCH_WARMUP_SEC=${BENCH_WARMUP_SEC:-4}
RUNS=${RUNS:-2}
OUT=scripts/p1_rlu_cmp.csv
export LIST_SIZE=10000 CHURN=200
BUILDS=("samecmp|./arms-p1/bench_list_scale.pin-packed-2793224e" "rawcmp|./arms-p1/bench_list_scale.pin-packed-2793224e-rawcmp")

for b in "${BUILDS[@]}"; do
  IFS='|' read -r bname bin <<<"$b"
  [ -x "$bin" ] || { echo "missing $bin" >&2; exit 1; }
done
echo "build,engine,run,seconds,mvisits_per_s,instructions,branches,loads" > "$OUT"
for r in $(seq 1 "$RUNS"); do
  for b in "${BUILDS[@]}"; do
    IFS='|' read -r bname bin <<<"$b"
    for spec in "rculist|X=0" "rlu_list|BENCH_RLU_WS=100"; do
      IFS='|' read -r eng extra <<<"$spec"
      for d in 2 8; do
        echo ">> $bname $eng ${d}s run=$r" >&2
        tmp=$(mktemp)
        # COUNTER PAIRS RUN WITHOUT WARM-UP.  The per-visit counts difference a
        # 2 s and an 8 s run, which cancels the process's start-up cost -- the
        # 2 s run is the warm-up.  A BENCH_WARMUP_SEC phase would add work perf
        # counts but that varies run to run, so the difference would not cancel
        # it (measured: +load read 8.47 instructions/visit instead of 9.01).
        env DURATION_SEC="$d" BENCH_WARMUP_SEC=0 BENCH_NO_WRITER=1 $extra \
          perf stat -x, -e instructions,branches,L1-dcache-loads \
          "$bin" "$eng" 1 >"$tmp" 2>"$tmp.perf"
        rate=$(awk '/^[0-9]/{print $2; exit}' "$tmp")
        ins=$(awk -F, '$3=="instructions"{print $1}' "$tmp.perf")
        brs=$(awk -F, '$3=="branches"{print $1}' "$tmp.perf")
        lds=$(awk -F, '$3=="L1-dcache-loads"{print $1}' "$tmp.perf")
        echo "$bname,$eng,$r,$d,${rate:-0},${ins:-0},${brs:-0},${lds:-0}" >> "$OUT"
        rm -f "$tmp" "$tmp.perf"
      done
    done
  done
done
echo "# done -> $OUT" >&2
