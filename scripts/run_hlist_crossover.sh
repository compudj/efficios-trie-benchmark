#!/bin/bash
# run_hlist_crossover.sh -- regenerate scripts/hlist_crossover.csv, the data of
# figures/hlist_crossover.png and of the README "Dataset size" tables.
#
# Throughput: the hash-of-lists engines at a fixed 64 threads and 10 % updates,
# load factor held at ~1 (HL_INIT = HL_BUCKETS, keys drawn from 2 x HL_INIT so
# occupancy stays steady under toggling) while the bucket count -- the dataset --
# grows from 64K to 8M.  REPS repeats of the whole sweep, interleaved by engine,
# so a slow drift of the machine lands on every engine alike.
#
# Footprint: table RSS at 1 000 000 buckets / 10 000 keys, one thread, read from
# the "engine ... rss_kb" line the bench prints once the table is built.
#
# Reconstructed generator for what was an ad-hoc run.  Writes
#   scripts/hlist_crossover.csv      rep,buckets,engine,mops
#   scripts/hlist_crossover_rss.csv  engine,buckets,keys,rss_kb
#
# Env: DURATION_SEC REPS THREADS UPDATE_PCT TXN_TREE
set -u
cd "$(dirname "$0")/.." || exit 1
export DURATION_SEC=${DURATION_SEC:-3}
REPS=${REPS:-5}
THREADS=${THREADS:-64}
UPDATE_PCT=${UPDATE_PCT:-10}
OUT=scripts/hlist_crossover.csv
RSS=scripts/hlist_crossover_rss.csv
# See the note in run_list_scale_alloc.sh: this repo's own engine build.
TXN_TREE=${TXN_TREE:-$PWD/urcu-txn-build}

echo ">> rebuilding bench_list_scale against $TXN_TREE (forced clean) ..." >&2
rm -f bench_list_scale src/bench_list_scale.o
make bench_list_scale URCU_TXN_BUILD="$TXN_TREE" >/dev/null 2>&1 \
  || { echo "BUILD FAILED" >&2; exit 1; }

ENGINES="txn_hlist rlu_hlist rcu_hlist lfht"

echo "rep,buckets,engine,mops" > "$OUT"
for rep in $(seq 1 "$REPS"); do
  for b in 65536 524288 1048576 2097152 4194304 8388608; do
    for e in $ENGINES; do
      echo ">> rep=$rep buckets=$b $e" >&2
      m=$(env HL_BUCKETS=$b HL_INIT=$b HL_RANGE=$((2 * b)) BENCH_UPDATE_PCT=$UPDATE_PCT \
            BENCH_FIXED_THREADS=$THREADS ./bench_list_scale "$e" 192 2>/dev/null \
          | awk '/^[0-9]/{print $2}')
      echo "$rep,$b,$e,${m:-NA}" >> "$OUT"
    done
  done
  echo "# repeat $rep complete" >> "$OUT"
done
echo "# ALL DONE" >> "$OUT"

echo "engine,buckets,keys,rss_kb" > "$RSS"
for e in $ENGINES; do
  echo ">> rss $e" >&2
  k=$(env HL_BUCKETS=1000000 HL_INIT=10000 HL_RANGE=20000 BENCH_UPDATE_PCT=$UPDATE_PCT \
        BENCH_FIXED_THREADS=1 DURATION_SEC=1 ./bench_list_scale "$e" 1 2>/dev/null \
      | awk '$1=="engine"{print $4}')
  echo "$e,1000000,10000,${k:-NA}" >> "$RSS"
done
echo "wrote $OUT $RSS" >&2
