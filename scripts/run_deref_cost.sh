#!/bin/bash
# run_deref_cost.sh -- MEASURE the per-dereference loads and branches that P1's
# cost table states, instead of reading them off the source.
#
# Method.  One reader, no writer, so the traversal loop is essentially the whole
# program.  Hardware counters over the whole process would also count the list
# build and the self-check, which are fixed costs, so each engine is run at TWO
# durations and the counters are DIFFERENCED: (C_long - C_short) divided by
# (visits_long - visits_short) is the marginal cost of a node visit, with every
# fixed cost cancelled.
#
# The table's columns are EXTRA loads and EXTRA branches -- over and above what
# the traversal must do anyway -- so the figure that matters is each engine's
# per-visit count MINUS rculist's.  rculist is plain RCU: it dereferences and
# compares, and tests nothing else.  That subtraction is what makes the result
# comparable to the table rather than to itself.
#
# Loads are counted with L1-dcache-loads (all retired load uops, hits and
# misses alike) -- the table counts memory REFERENCES, not cache misses, which
# is the right notion: the objection to a per-element field is that the reader
# must go and fetch its operand at all.
#
# Writes scripts/deref_cost.csv:
#   engine,run,seconds,mvisits_per_s,instructions,branches,loads
set -u
cd /home/efficios/git/efficios-trie-benchmark
export LIST_SIZE=${LIST_SIZE:-10000} CHURN=${CHURN:-200}
RUNS=${RUNS:-2}
SHORT=${SHORT:-2}
LONG=${LONG:-8}
OUT=scripts/deref_cost.csv

command -v perf >/dev/null || { echo "perf not found" >&2; exit 1; }
echo ">> building JEMALLOC=1 ..." >&2
make bench_list_scale JEMALLOC=1 >/dev/null 2>&1 \
  || { echo "BUILD FAILED" >&2; exit 1; }

echo "engine,run,seconds,mvisits_per_s,instructions,branches,loads" > "$OUT"

# label|bench-engine|extra-env.  rculist is the baseline the table subtracts
# against; txn_sw_list walks forward twice here so the two do identical work
# apart from the resolve.
ENGINES="rculist|rculist|X=0 txn_sw_list|txn_sw_list|BENCH_SU_FORWARD=1 existence_list|existence_list|X=0 rlu_list|rlu_list|BENCH_RLU_WS=100 mvrlu_list|mvrlu_list|X=0"

for r in $(seq 1 "$RUNS"); do
  for spec in $ENGINES; do
    IFS='|' read -r lbl eng extra <<<"$spec"
    for d in "$SHORT" "$LONG"; do
      echo ">> $lbl ${d}s run=$r" >&2
      tmp=$(mktemp)
      env DURATION_SEC="$d" BENCH_NO_WRITER=1 $extra \
        perf stat -x, -e instructions,branches,L1-dcache-loads \
        ./bench_list_scale "$eng" 1 >"$tmp" 2>"$tmp.perf"
      rate=$(awk '/^[0-9]/{print $2; exit}' "$tmp")
      ins=$(awk -F, '$3=="instructions"{print $1}' "$tmp.perf")
      brs=$(awk -F, '$3=="branches"{print $1}' "$tmp.perf")
      lds=$(awk -F, '$3=="L1-dcache-loads"{print $1}' "$tmp.perf")
      echo "$lbl,$r,$d,${rate:-0},${ins:-0},${brs:-0},${lds:-0}" >> "$OUT"
      rm -f "$tmp" "$tmp.perf"
    done
  done
done
echo ">> done -> $OUT" >&2
