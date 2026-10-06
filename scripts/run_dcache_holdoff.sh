#!/bin/bash
# run_dcache_holdoff.sh -- A/B of the bucket lock engine's walk hold-off
# (-DDC_WALK_HOLDOFF=N, experiments/dcache/dcache_bucketlock.c) against the
# default build, on run_dcache.sh's panels, geometry, pinning, allocator and
# warm-up, so the default arm can be checked against dcache_sweep.csv.
#
# Arms, one binary each (built here, -B, in one go):
#   bucketlock   the default build
#   hold1        -DDC_WALK_HOLDOFF=1: a lookup raises the count after ONE
#                failed pass -- the most a lookup can ask of the writers
#   hold3        -DDC_WALK_HOLDOFF=3
#
# What the panels are for:
#   idle_scale, idle_hit    no rename in the timed window: what the pass
#                           counter costs a lookup that never re-walks
#   split_scale, hit_scale  8 writers paced at 100k renames/s
#   rate, hit_rate          184 readers, the offered rename rate swept
#   sat_scale, sat_hit      8 writers flat out: the most marks a lookup faces
#   frac                    every thread mixes lookups and renames
# The *_hit panels look up the objects being renamed (bench --hit-current);
# the others probe (about 97% negative hits).  hit_rate and sat_hit are not in
# run_dcache.sh.
#
# Unlike run_dcache.sh this records EVERY run, not the best of RUNS: the
# question is whether two arms differ by more than a point's own spread.  The
# arms are interleaved inside each repetition, so drift lands on all of them.
#
# After the timed runs each hold-off arm is run once more per point from its
# counting twin, for how often the count was raised (dcache_holdoff_counts.csv).
# Those counts cover the whole process (warm-up and teardown included) and
# come from a build whose debug counters are shared atomics: indicative.
#
# Output: scripts/dcache_holdoff.csv, scripts/dcache_holdoff_counts.csv
# Summarize with scripts/dcache_holdoff_summary.py.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || exit 2
SRC_ID=$("$REPO/scripts/dcache_src_id.sh" "${URCU_TXN_BUILD:-$REPO/urcu-txn-build}")
BIN=$REPO/experiments/dcache
CSV=${CSV:-$REPO/scripts/dcache_holdoff.csv}
CNT=${CNT:-$REPO/scripts/dcache_holdoff_counts.csv}

JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
DEPTH=4
LEAVES=32
DUR=${DUR:-1000}
RUNS=${RUNS:-5}
PER_W=${PER_W:-12500}
HOLDS=${HOLDS:-"1 3"}

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
[[ -n "$CPULIST" ]] || { echo "hwloc-calc unavailable" >&2; exit 1; }
NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .)
PIN="--cpulist $CPULIST"
COMMON="--depth $DEPTH --leaves $LEAVES --nbuckets 1048576 --duration $DUR $PIN"
[[ -f "$JE" ]] || { echo "jemalloc not at $JE" >&2; exit 1; }

declare -A BINOF=( [bucketlock]=bench_dcache_bucketlock )
ARMS="bucketlock"
TARGETS="bench_dcache_bucketlock"
for n in $HOLDS; do
  BINOF[hold$n]=bench_dcache_bucketlock_hold$n
  ARMS+=" hold$n"
  TARGETS+=" bench_dcache_bucketlock_hold$n bench_dcache_bucketlock_hold${n}_count"
done

echo ">> load before: $(cut -d' ' -f1-3 /proc/loadavg)  src $SRC_ID" >&2
echo ">> building: $TARGETS" >&2
# shellcheck disable=SC2086
make -B -C "$BIN" -j8 $TARGETS > /dev/null || { echo "build failed" >&2; exit 1; }
for a in $ARMS; do
  if nm -D --undefined-only "$BIN/${BINOF[$a]}" | grep -q __assert_fail; then
    echo "!! ${BINOF[$a]} was built with assertions" >&2; exit 1
  fi
done

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }

echo "panel,arm,threads,writers,readers,rename_frac,rate_target,run,mlookups_s,mrenames_s,conserved,src" > "$CSV"
echo "panel,arm,threads,writers,readers,rename_frac,rate_target,raises,writer_sleeps,refolds,evict_skips,mlookups_s,mrenames_s,src" > "$CNT"

# point <panel> <threads> <writers(-1=homog)> <rename_frac> [rate|idle]
RUN_EXTRA=""
point() {
  local panel=$1 threads=$2 writers=$3 frac=$4 rate=${5:-0}
  local split="" readers=$threads pace="" r a out cons lk rn
  if [[ "$writers" -ge 0 ]]; then split="--writers $writers"; readers=$((threads-writers)); fi
  if [[ "$rate" == idle ]]; then pace="--rename-rate 0.001"; rate=0
  elif [[ "$rate" != 0 ]]; then pace="--rename-rate $rate"; fi
  local nw=$(( writers >= 0 ? writers : threads ))
  local nd=$(( 16 * (nw < 1 ? 1 : nw) ))
  local args="--nthreads $threads $split --ndirs $nd --rename-frac $frac $pace $COMMON $RUN_EXTRA"
  for r in $(seq 1 "$RUNS"); do
    for a in $ARMS; do
      # shellcheck disable=SC2086
      out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./"${BINOF[$a]}" $args 2>/dev/null)
      cons=OK; grep -q "conservation: OK" <<< "$out" || cons=FAIL
      lk=$(field "$out" "Mlookups/s:"); rn=$(field "$out" "Mrenames/s:")
      echo "$panel,$a,$threads,$writers,$readers,$frac,$rate,$r,${lk:-0},${rn:-0},$cons,$SRC_ID" >> "$CSV"
      [[ $cons == OK ]] || echo "!! $panel/$a thr=$threads w=$writers rate=$rate run=$r CONSERVATION FAILED" >&2
    done
  done
  for n in $HOLDS; do
    # shellcheck disable=SC2086
    out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./bench_dcache_bucketlock_hold${n}_count $args 2>&1)
    local line; line=$(grep -m1 '^walk hold-off' <<< "$out")
    echo "$panel,hold$n,$threads,$writers,$readers,$frac,$rate,$(sed -E 's/.*raises=([0-9]+) writer-sleeps=([0-9]+) refolds=([0-9]+) evict-skips=([0-9]+).*/\1,\2,\3,\4/' <<< "$line"),$(field "$out" "Mlookups/s:"),$(field "$out" "Mrenames/s:"),$SRC_ID" >> "$CNT"
  done
  printf "  %-11s thr=%-4s w=%-3s frac=%-5s rate=%-8s done  %s\n" \
    "$panel" "$threads" "$writers" "$frac" "$rate" "$(date +%T)" >&2
}

WFIX=8
RMAX=$((NCORE - WFIX))
RATE_FIX=$((WFIX * PER_W))
RDPTS=${RDPTS:-"2 8 32 96 $RMAX"}
SATPTS=${SATPTS:-"2 4 8 16 32 96 $RMAX"}
PANELS="${PANELS:-}"
want() { [[ -z "$PANELS" || " $PANELS " == *" $1 "* ]]; }

if want idle_scale; then
  for rd in $RDPTS; do point idle_scale $((rd+WFIX)) $WFIX 1.0 idle; done
fi
if want idle_hit; then RUN_EXTRA="--hit-current"
  for rd in $RDPTS; do point idle_hit $((rd+WFIX)) $WFIX 1.0 idle; done
RUN_EXTRA=""; fi
if want split_scale; then
  for rd in $RDPTS; do point split_scale $((rd+WFIX)) $WFIX 1.0 $RATE_FIX; done
fi
if want hit_scale; then RUN_EXTRA="--hit-current"
  for rd in $RDPTS; do point hit_scale $((rd+WFIX)) $WFIX 1.0 $RATE_FIX; done
RUN_EXTRA=""; fi
if want rate; then
  for rt in 100000 1000000 10000000; do point rate $((RMAX+WFIX)) $WFIX 1.0 $rt; done
fi
if want hit_rate; then RUN_EXTRA="--hit-current"
  for rt in 1000000 10000000; do point hit_rate $((RMAX+WFIX)) $WFIX 1.0 $rt; done
RUN_EXTRA=""; fi
if want sat_scale; then
  for rd in $SATPTS; do point sat_scale $((rd+WFIX)) $WFIX 1.0; done
fi
if want sat_hit; then RUN_EXTRA="--hit-current"
  for rd in $RDPTS; do point sat_hit $((rd+WFIX)) $WFIX 1.0; done
RUN_EXTRA=""; fi
if want frac; then
  for f in 0 0.01 0.1 0.5; do point frac 48 -1 $f; done
fi

echo ">> load after: $(cut -d' ' -f1-3 /proc/loadavg)" >&2
echo ">> DONE: $(($(wc -l < "$CSV") - 1)) runs -> $CSV, $(($(wc -l < "$CNT") - 1)) count rows -> $CNT" >&2
