#!/bin/bash
# Insert/remove (create/delete) sweep for the userspace dentry-cache experiment.
#
# The other sweeps hold the namespace FIXED and permute it (run_dcache.sh:
# rename/exchange/readdir; run_dcache_height.sh: exchange at height).  This one
# CHURNS it: writers toggle their own slots present/absent, so it measures
# dc_add + dc_unlink, which nothing else does.
#
# Four arms, one binary each (built by `make -C experiments/dcache churn`):
#   seqlock       -- faithful kernel-style rename_lock + per-dentry d_seq
#   txn-global    -- urcu-txn port, GLOBAL rename_gen bumped by every unlink
#   txn-pernode   -- urcu-txn port, PER-NODE host generation
#   txn-mark      -- urcu-txn port, no counter: the hlist deletion mark is the
#                    version, so unlink bumps nothing at all
#
# Three panels:
#   churn_w      WRITERS ONLY -- raw insert/remove scaling, Mchurn/s vs W.  No
#                readers, so this is the pure mutator path.
#   churn_rd     32 dedicated readers + W churn writers -- reader Mlookups/s vs
#                W.  Isolates what create/delete load does to the READ path,
#                which is where a shared version counter shows up.
#   churn_scale  8 churn writers fixed, sweep the reader count.
#
# NOTE the binaries are built -DDC_SPLIT_KEEPID (a re-added dentry is a new
# allocation, so the harness's id checks need logical ids).  Reader rates here
# are therefore NOT directly comparable with run_dcache.sh's address-default
# numbers; compare arms within this sweep.
#
# Every run is gated on the churn invariant (state + census + ids agree); a
# failing run is flagged and its numbers dropped.
#
# PACING.  churn_w measures the writers, so they run flat out.  churn_rd and
# churn_scale measure READERS under churn, so their writers are held to PER_W
# adds+unlinks/s each (bench --churn-rate): flat out, each engine's readers
# would face the rate its OWN writers reach, and the engine with the slower
# writers gets its readers measured on a quieter machine (see run_dcache.sh).
# Columns rate_target (0 = flat out) and paced (OK >= 95% of target, SHORT
# below, - unpaced) record it; best-of-RUNS reports ONE run's numbers
# (scripts/dcache_pick_run.sh).
#
# TOGGLE MODES.  Every panel runs twice (column `mode`): `inplace` toggles a
# name the way the kernel does when it is removed and created again (d_delete
# to a negative in place, then d_instantiate: no allocation, no LRU traffic),
# `alloc` unlinks and re-adds it (allocation + LRU enqueue/dequeue + a free on
# every pair -- the ever-new-names path).  Measured 2026-09-29, alloc spends
# 39-57% of its cycles on ONE per-node LRU lock the kernel's same-name churn
# never takes, so `inplace` is the churn figure's headline.
#
# Output: scripts/dcache_churn.csv  (plot with scripts/plot_dcache_churn.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
# Provenance stamped on every row (scripts/dcache_src_id.sh).
SRC_ID=$("$REPO/scripts/dcache_src_id.sh" "${URCU_TXN_BUILD:-$REPO/urcu-txn-build}")
BIN=$REPO/experiments/dcache
CSV=${CSV:-$REPO/scripts/dcache_churn.csv}

SLOTS=32
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
DUR=${DUR:-1000}
RUNS=${RUNS:-5}
PER_W=${PER_W:-12500}		# paced adds+unlinks/s per writer (reader panels)

NCORE=$(nproc)
CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
if [[ -n "$CPULIST" ]]; then
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | wc -l)
  PIN="--cpulist $CPULIST"
  echo ">> hwloc: one hw thread per core, $NCORE cores" >&2
else
  PIN=""
  echo ">> hwloc-calc unavailable; unpinned" >&2
fi

# ndirs is decontended per-run (16 x writers); jemalloc removes the
# allocator ceiling.  This is the corrected methodology (see
# run_dcache_churn_scaling.sh / dcache_optype.png for why).
COMMON="--slots $SLOTS --nbuckets 1048576 --duration $DUR $PIN"
[[ -f "$JE" ]] || { echo "jemalloc not at $JE"; exit 1; }

declare -A BINOF=( [seqlock]=bench_dcache_churn_seqlock \
                   [txn-global]=bench_dcache_churn_txn \
                   [txn-pernode]=bench_dcache_churn_txn_pernode \
                   [txn-mark]=bench_dcache_churn_txn_mark \
                   [bucketlock]=bench_dcache_churn_bucketlock )
ENGINES=${ENGINES:-"seqlock txn-global txn-pernode txn-mark bucketlock"}

for e in $ENGINES; do
  test -x "$BIN/${BINOF[$e]}" || {
    echo "MISSING $BIN/${BINOF[$e]} -- run 'make -C experiments/dcache churn'" >&2
    exit 1; }
done

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }
. "$REPO/scripts/dcache_pick_run.sh"

# PANELS (env): space-separated subset to (re)run.  Empty => all, fresh CSV.
# Non-empty => keep the CSV and re-run ONLY those panels, dropping their old
# rows first, so one unstable panel can be resswept (optionally with a larger
# RUNS) without disturbing the others.
PANELS="${PANELS:-}"
want() { [[ -z "$PANELS" || " $PANELS " == *" $1 "* ]]; }
HDR="panel,mode,engine,readers,writers,rate_target,mchurn_s,mlookups_s,paced,conserved,src"
if [[ -z "$PANELS" ]]; then
  echo "$HDR" > "$CSV"
else
  # A CSV with another column layout cannot take new rows: set it aside whole.
  if [[ -f "$CSV" && "$(head -1 "$CSV")" != "$HDR" ]]; then
    mv "$CSV" "$CSV.prev"
    echo ">> $CSV had another column layout: moved to $CSV.prev" >&2
  fi
  [[ -f "$CSV" ]] || echo "$HDR" > "$CSV"
  for p in $PANELS; do grep -v "^$p," "$CSV" > "$CSV.tmp" && mv "$CSV.tmp" "$CSV"; done
fi

# run <panel> <engine> <readers> <writers> [rate] -> best-of-RUNS, appends a
# CSV row.  [rate] (adds+unlinks/s, aggregate) paces the writers; omitted or 0
# runs them flat out.  With no readers the writers ARE the measurement, so the
# best run is the best writer run.
run() {
  local panel=$1 eng=$2 rd=$3 w=$4 rate=${5:-0}
  local bin=$BIN/${BINOF[$eng]} r out cons=OK runs="" pace="" how=""
  local key best_ch best_lk paced
  [[ "$rate" != 0 ]] && pace="--churn-rate $rate"
  [[ "$MODE" == inplace ]] && how="--in-place"
  for r in $(seq 1 $RUNS); do
    local nd=$(( 16 * (w < 1 ? 1 : w) ))
    out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./"$(basename "$bin")" \
          --readers "$rd" --writers "$w" --ndirs "$nd" $how $pace $COMMON 2>/dev/null)
    if ! grep -q "conservation: OK" <<< "$out"; then
      cons=FAIL
      echo "!! $panel/$MODE/$eng rd=$rd w=$w rate=$rate CHURN INVARIANT FAILED" >&2
      continue
    fi
    local ch lk
    ch=$(field "$out" "Mchurn/s:"); lk=$(field "$out" "Mlookups/s:")
    if (( rd == 0 )); then runs+="$ch $ch $lk"$'\n'; else runs+="$lk $ch $lk"$'\n'; fi
  done
  read -r key best_ch best_lk paced < <(pick_run "$rate" <<< "$runs")
  [[ "$paced" == "" ]] && { paced=$best_lk; best_lk=0; }	# no conserved run
  echo "$panel,$MODE,$eng,$rd,$w,$rate,$best_ch,$best_lk,$paced,$cons,$SRC_ID" >> "$CSV"
  printf "  %-12s %-8s %-11s rd=%-4s w=%-3s rate=%-7s churn=%8s Mops/s  rd=%8s Mlk/s  %-5s %s\n" \
    "$panel" "$MODE" "$eng" "$rd" "$w" "$rate" "$best_ch" "$best_lk" "$paced" "$cons" >&2
}

# MODES (env): which toggle each panel runs.  inplace = the kernel's same-name
# churn (bench --in-place: d_delete to negative + d_instantiate -- no
# allocation, no LRU traffic); alloc = dc_unlink + dc_add per toggle (ever-new
# names, or a dropping dentry-negative policy: allocation + LRU on every pair).
# The slab-route runs set MODES=alloc: in-place toggles commit no descriptor on
# the lock engines, so only the allocating path asks their question.
MODES=${MODES:-"inplace alloc"}
for MODE in $MODES; do
echo ">> toggle mode: $MODE" >&2

WPTS="1 2 4 8 16 32 48"

if want churn_w; then
echo ">> churn_w panel: writers only, raw insert/remove scaling" >&2
for w in $WPTS; do
  for e in $ENGINES; do run churn_w "$e" 0 "$w"; done
done
fi

if want churn_rd; then
echo ">> churn_rd panel: 32 readers + W churn writers at $PER_W ops/s each, reader Mlookups/s vs W" >&2
for w in $WPTS; do
  for e in $ENGINES; do run churn_rd "$e" 32 "$w" $((w * PER_W)); done
done
fi

WFIX=8
RMAX=$((NCORE - WFIX))
RDPTS=$(for rd in 2 4 8 16 32 48 64 96 128 160 $RMAX; do
          (( rd >= 1 && rd <= RMAX )) && echo "$rd"; done | sort -n -u)
if want churn_scale; then
echo ">> churn_scale panel: $WFIX churn writers at $((WFIX * PER_W)) ops/s, sweep readers to $RMAX" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run churn_scale "$e" "$rd" "$WFIX" $((WFIX * PER_W)); done
done
fi
done	# MODE

echo ">> DONE: $(( $(wc -l < "$CSV") - 1 )) rows -> $CSV" >&2
