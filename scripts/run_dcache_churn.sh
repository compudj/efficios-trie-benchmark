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
# Output: scripts/dcache_churn.csv  (plot with scripts/plot_dcache_churn.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
CSV=$REPO/scripts/dcache_churn.csv

SLOTS=32
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
DUR=${DUR:-1000}
RUNS=${RUNS:-5}

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
ENGINES="seqlock txn-global txn-pernode txn-mark bucketlock"

for e in $ENGINES; do
  test -x "$BIN/${BINOF[$e]}" || {
    echo "MISSING $BIN/${BINOF[$e]} -- run 'make -C experiments/dcache churn'" >&2
    exit 1; }
done

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }

# PANELS (env): space-separated subset to (re)run.  Empty => all, fresh CSV.
# Non-empty => keep the CSV and re-run ONLY those panels, dropping their old
# rows first, so one unstable panel can be resswept (optionally with a larger
# RUNS) without disturbing the others.
PANELS="${PANELS:-}"
want() { [[ -z "$PANELS" || " $PANELS " == *" $1 "* ]]; }
HDR="panel,engine,readers,writers,mchurn_s,mlookups_s,conserved"
if [[ -z "$PANELS" ]]; then
  echo "$HDR" > "$CSV"
else
  [[ -f "$CSV" ]] || echo "$HDR" > "$CSV"
  for p in $PANELS; do grep -v "^$p," "$CSV" > "$CSV.tmp" && mv "$CSV.tmp" "$CSV"; done
fi

# run <panel> <engine> <readers> <writers> -> best-of-RUNS, appends a CSV row
run() {
  local panel=$1 eng=$2 rd=$3 w=$4
  local bin=$BIN/${BINOF[$eng]} r out cons=OK
  local best_ch=0 best_lk=0
  for r in $(seq 1 $RUNS); do
    local nd=$(( 16 * (w < 1 ? 1 : w) ))
    out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./"$(basename "$bin")" \
          --readers "$rd" --writers "$w" --ndirs "$nd" $COMMON 2>/dev/null)
    if ! grep -q "conservation: OK" <<< "$out"; then
      cons=FAIL
      echo "!! $panel/$eng rd=$rd w=$w CHURN INVARIANT FAILED" >&2
      continue
    fi
    local ch lk
    ch=$(field "$out" "Mchurn/s:"); lk=$(field "$out" "Mlookups/s:")
    awk -v v="${ch:-0}" -v b="$best_ch" 'BEGIN{exit !(v>b)}' && best_ch=$ch
    awk -v v="${lk:-0}" -v b="$best_lk" 'BEGIN{exit !(v>b)}' && best_lk=$lk
  done
  echo "$panel,$eng,$rd,$w,${best_ch:-0},${best_lk:-0},$cons" >> "$CSV"
  printf "  %-12s %-11s rd=%-4s w=%-3s  churn=%8s Mops/s  rd=%8s Mlk/s  %s\n" \
    "$panel" "$eng" "$rd" "$w" "$best_ch" "$best_lk" "$cons" >&2
}

WPTS="1 2 4 8 16 32 48"

if want churn_w; then
echo ">> churn_w panel: writers only, raw insert/remove scaling" >&2
for w in $WPTS; do
  for e in $ENGINES; do run churn_w "$e" 0 "$w"; done
done
fi

if want churn_rd; then
echo ">> churn_rd panel: 32 readers + W churn writers, reader Mlookups/s vs W" >&2
for w in $WPTS; do
  for e in $ENGINES; do run churn_rd "$e" 32 "$w"; done
done
fi

WFIX=8
RMAX=$((NCORE - WFIX))
RDPTS=$(for rd in 2 4 8 16 32 48 64 96 128 160 $RMAX; do
          (( rd >= 1 && rd <= RMAX )) && echo "$rd"; done | sort -n -u)
if want churn_scale; then
echo ">> churn_scale panel: $WFIX churn writers, sweep readers to $RMAX" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run churn_scale "$e" "$rd" "$WFIX"; done
done
fi

echo ">> DONE: $(( $(wc -l < "$CSV") - 1 )) rows -> $CSV" >&2
