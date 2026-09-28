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
set -u
cd /home/efficios/git/efficios-trie-benchmark
RUNS=${RUNS:-2}
OUT=scripts/p1_rlu_cmp.csv
export LIST_SIZE=10000 CHURN=200
BUILDS=("samecmp|./arms-p1/bench_list_scale.pin-packed-readclass" "rawcmp|./arms-p1/bench_list_scale.pin-packed-rawcmp")

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
        env DURATION_SEC="$d" BENCH_NO_WRITER=1 $extra \
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
