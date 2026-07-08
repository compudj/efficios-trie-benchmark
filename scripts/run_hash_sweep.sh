#!/bin/bash
# run_hash_sweep.sh -- refresh the three README hash sections for all engines.
#
# One bench_list_scale process sweeps all thread points internally, so each
# (engine, config) is a single invocation.  best-of-2 (RUNS=2), DURATION_SEC=3,
# default allocator/reclaim.  Engines: txn_hlist, rcu_hlist, lfht, and rlu_hlist
# in both deferral modes (RLU-defer = BENCH_RLU_WS=100, RLU-sync = =1).
#
# Emits three tidy long-format CSVs in scripts/.  Aggregation (best-of-2) and
# plotting happen in the companion plot_hash_*.py scripts.
set -u
cd /mnt/data/efficios/git/efficios-trie-benchmark
export DURATION_SEC=3
RUNS=2
LWN=scripts/hash_lwn.csv
DED=scripts/hash_dedicated.csv
CON=scripts/hash_contention.csv

echo ">> rebuilding bench_list_scale against the local txn tree ..." >&2
make bench_list_scale URCU_TXN_BUILD=/mnt/data/efficios/git/userspace-rcu-txn >/dev/null 2>&1 \
  || { echo "BUILD FAILED" >&2; exit 1; }

# engine-config table: label | engine | extra-env
CONFIGS=(
  "txn_hlist|txn_hlist|"
  "rcu_hlist|rcu_hlist|"
  "lfht|lfht|"
  "rlu_defer|rlu_hlist|BENCH_RLU_WS=100"
  "rlu_sync|rlu_hlist|BENCH_RLU_WS=1"
)

run() {  # $1=label $2=engine $3=extraenv ; remaining env via caller's export; prints raw stdout
  local lbl=$1 eng=$2 extra=$3
  env $extra ./bench_list_scale "$eng" 192 2>/dev/null
}

echo "pct,engine,run,threads,total_mops,update_mops,read_mops,viol" > "$LWN"
echo "mode,engine,run,x,a,b,viol" > "$DED"
echo "mode,engine,run,x,total_mops,update_mops,read_mops,viol" > "$CON"

########################################################################
# Section 1 -- LWN #667720 mixed % updates, 1000 x 100 hash
########################################################################
export HL_BUCKETS=1000 HL_INIT=100000 HL_RANGE=200000
for pct in 0 2 20 40; do
  export BENCH_UPDATE_PCT=$pct
  for c in "${CONFIGS[@]}"; do IFS='|' read -r lbl eng extra <<<"$c"
    for r in $(seq 1 $RUNS); do
      echo ">> S1 pct=$pct $lbl run=$r" >&2
      run "$lbl" "$eng" "$extra" | awk -v L="$lbl" -v R="$r" -v P="$pct" \
        '/^[0-9]/{print P","L","R","$1","$2","$3","$4","$5}' >> "$LWN"
    done
  done
done
unset BENCH_UPDATE_PCT

########################################################################
# Section 2 -- dedicated reader/writer on the same 1000 x 100 hash
########################################################################
# 2a write scaling: writers 1..N, 0 readers
for c in "${CONFIGS[@]}"; do IFS='|' read -r lbl eng extra <<<"$c"
  for r in $(seq 1 $RUNS); do
    echo ">> S2 write $lbl run=$r" >&2
    env BENCH_WRITESCALE=1 BENCH_READERS=0 $extra ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "write,"L","R","$1","$2","$3","$4}' >> "$DED"
  done
done
# 2b read scaling: readers 1..N + 1 writer  (default dedicated mode)
for c in "${CONFIGS[@]}"; do IFS='|' read -r lbl eng extra <<<"$c"
  for r in $(seq 1 $RUNS); do
    echo ">> S2 read $lbl run=$r" >&2
    env $extra ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "read,"L","R","$1","$2","$3","$4}' >> "$DED"
  done
done
# 2c balanced 50/50: T/2 readers + T/2 writers
for c in "${CONFIGS[@]}"; do IFS='|' read -r lbl eng extra <<<"$c"
  for r in $(seq 1 $RUNS); do
    echo ">> S2 bal $lbl run=$r" >&2
    env BENCH_RW_BALANCED=1 $extra ./bench_list_scale "$eng" 192 2>/dev/null \
      | awk -v L="$lbl" -v R="$r" '/^[0-9]/{print "bal,"L","R","$1","$4","$5","$6}' >> "$DED"
  done
done

########################################################################
# Section 3 -- write contention: shrink bucket count at 40% updates
########################################################################
export BENCH_UPDATE_PCT=40
# 3a throughput vs #buckets at 192 threads, ~100 nodes/bucket.
# Swept out to 8192 so the low-contention plateau is visible: rcu_hlist's
# per-bucket lock recovers to the lock-free ceiling once buckets >> threads.
# Note >~4096 the fixed ~100 nodes/bucket makes the working set outgrow the LLC,
# so all cache-bound engines sag together (footprint-, not contention-bound); the
# plot deliberately stops at 4096 (see plot_hash_contention.py XMAX).
for b in 1 4 16 64 256 1024 4096 8192; do
  export HL_BUCKETS=$b HL_INIT=$((100*b)) HL_RANGE=$((200*b)) BENCH_FIXED_THREADS=192
  for c in "${CONFIGS[@]}"; do IFS='|' read -r lbl eng extra <<<"$c"
    for r in $(seq 1 $RUNS); do
      echo ">> S3 buckets=$b $lbl run=$r" >&2
      run "$lbl" "$eng" "$extra" | awk -v L="$lbl" -v R="$r" -v B="$b" \
        '/^[0-9]/{print "buckets,"L","R","B","$2","$3","$4","$5}' >> "$CON"
    done
  done
done
unset BENCH_FIXED_THREADS
# 3b thread scaling at 16 buckets (heavily contended), 40% updates
export HL_BUCKETS=16 HL_INIT=1600 HL_RANGE=3200
for c in "${CONFIGS[@]}"; do IFS='|' read -r lbl eng extra <<<"$c"
  for r in $(seq 1 $RUNS); do
    echo ">> S3 threads16 $lbl run=$r" >&2
    run "$lbl" "$eng" "$extra" | awk -v L="$lbl" -v R="$r" \
      '/^[0-9]/{print "threads16,"L","R","$1","$2","$3","$4","$5}' >> "$CON"
  done
done

echo "# ALL DONE" >&2
echo "wrote $LWN $DED $CON" >&2
