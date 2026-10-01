#!/bin/bash
# S3 sweep for the userspace dentry-cache experiment (experiments/dcache).
#
# Arms, one binary each (built by `make -C experiments/dcache bench`):
#   seqlock       -- the kernel's RCU-walk: hand-over-hand per-dentry d_seq,
#                    rename_lock sampled per walk and consulted only on a miss
#   seqlock-snapshot -- the same engine with every lookup bracketed on
#                    rename_lock (bounded read_seqbegin_or_lock retry): the
#                    whole-path snapshot every txn arm gives, priced with the
#                    kernel's own tool.  Pairs with txn-global.
#   txn-global    -- urcu-txn port, GLOBAL rename_gen walk bracket
#   txn-pernode   -- urcu-txn port, PER-NODE host generation (localized)
#   txn-mark      -- urcu-txn port, localized with NO counter: the hlist deletion
#                    mark is the version, so d_seq is retired and DC_NAME_MAX
#                    rises 32 -> 40 at unchanged sizeof(dentry)
#
# Two headline views + a scaling view, every run gated on namespace conservation
# (a CONSERVATION FAILED run is flagged and its numbers dropped):
#
#   frac         HOMOGENEOUS mix: each of `THREADS` threads does rename-frac of
#                its ops as renames, the rest as full-path lookups.  Sweeps the
#                rename fraction.  Dominated by the WRITER path (a rename is
#                ~50x a lookup), so it shows the mixed-workload throughput but
#                MASKS the reader-side generation difference.
#   split_w      ROLE-SPLIT: `RSPLIT` dedicated readers + W dedicated writers.
#                Reader Mlookups/s vs W isolates the reader path -- where the
#                global bracket contends a whole-tree cacheline and the per-node
#                host counter does not.  THE headline for the per-node arm.
#   split_scale  ROLE-SPLIT reader scaling: W fixed, sweep the reader count.
#   rate         ROLE-SPLIT reader throughput vs the OFFERED rename rate, at a
#                fixed reader count: what a rename costs the readers, per engine,
#                up to the highest rate that engine's writers can sustain.
#   sat_scale    split_scale with the writers FLAT OUT (saturation): each
#                engine's readers face whatever rate its own writers reach.
#
# PACING.  Every reader panel except sat_scale holds the writers to a fixed rate
# (bench --rename-rate): PER_W renames/s per writer, so W writers offer W*PER_W.
# Flat out, the engines' rename rates differ by up to two orders of magnitude
# (every seqlock rename serializes on rename_lock; a seqlock readdir or
# dentry_path starves its renamers), and fewer renames means fewer invalidations
# of the lines readers share: the engine with the SLOWER writers had its readers
# measured on a quieter machine.  Each row records the offered rate
# (rate_target, renames/s; 0 = flat out) and whether the writers sustained it
# (paced: OK >= 95% of target, SHORT below, - unpaced).  A SHORT row is still
# written -- an engine that cannot carry the load is a result -- but the plots
# draw it apart from the paced curve.
#
# Best-of-RUNS picks ONE run and reports its reader AND writer numbers together
# (the best reader run among those that sustained the rate; if none did, the
# run that came closest).  Taking each column's maximum separately paired a
# reader number with a writer number from a different run.
#
# Output: scripts/dcache_sweep.csv  (plot with scripts/plot_dcache.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
# Provenance stamped on every row (scripts/dcache_src_id.sh).
SRC_ID=$("$REPO/scripts/dcache_src_id.sh" "${URCU_TXN_BUILD:-$REPO/urcu-txn-build}")
BIN=$REPO/experiments/dcache
CSV=${CSV:-$REPO/scripts/dcache_sweep.csv}

JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
DEPTH=4
LEAVES=32
DUR=${DUR:-1000}
RUNS=${RUNS:-5}
PER_W=${PER_W:-12500}		# paced renames/s per writer (8 writers: 100k/s)

# Pin ONE hardware thread per physical core: ask hwloc for the first PU of every
# core (core:all.pu:0), OS-indexed.  On this 2x96 EPYC that is cpus 0..191 (the
# SMT siblings 192..383 are left idle), but deriving it from hwloc keeps the
# sweep correct on any PU numbering.  NCORE bounds how many threads we can place
# without doubling two threads onto one core.
CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
if [[ -n "$CPULIST" ]]; then
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .)
  PIN="--cpulist $CPULIST"
  echo ">> hwloc: one hw thread per core, $NCORE cores (${CPULIST:0:24}...)" >&2
else
  NCORE=$(nproc)
  PIN="--cpustride 1"
  echo ">> hwloc-calc unavailable; falling back to --cpustride 1 over $NCORE cpus" >&2
fi

# tree geometry knobs held fixed across the sweep
# Corrected methodology: jemalloc (allocator) + ndirs decontended to
# 16 x writers (child-hlist heads), per-run.  See dcache_optype.png.
COMMON="--depth $DEPTH --leaves $LEAVES --nbuckets 1048576 --duration $DUR $PIN"
[[ -f "$JE" ]] || { echo "jemalloc not at $JE"; exit 1; }

declare -A BINOF=( [seqlock]=bench_dcache_seqlock \
                   [seqlock-snapshot]=bench_dcache_seqlock_snapshot \
                   [txn-global]=bench_dcache_txn \
                   [txn-pernode]=bench_dcache_txn_pernode \
                   [txn-mark]=bench_dcache_txn_mark \
                   [bucketlock]=bench_dcache_bucketlock )
ENGINES=${ENGINES:-"seqlock seqlock-snapshot txn-global txn-pernode txn-mark bucketlock"}

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }

for e in $ENGINES; do
  test -x "$BIN/${BINOF[$e]}" || { echo "MISSING $BIN/${BINOF[$e]} -- run 'make -C experiments/dcache bench'" >&2; exit 1; }
done

# pick_run: best-of-RUNS selection with the pacing verdict.
. "$REPO/scripts/dcache_pick_run.sh"

# run <panel> <engine> <threads> <writers(-1=homog)> <rename_frac> [rate]
# -> best-of-RUNS appends a CSV row; readers = threads-writers in split mode,
# else threads.  [rate] (renames/s, aggregate) paces the writers; omitted or 0
# runs them flat out.  $RUN_EXTRA (global) appends extra flags, e.g. "--readdir
# --leaves 64"; a later --leaves overrides the one baked into $COMMON (argv:
# last wins).  For readdir panels the "Mlookups/s:" field carries the readdir
# CALL rate.
RUN_EXTRA=""
run() {
  local panel=$1 eng=$2 threads=$3 writers=$4 frac=$5 rate=${6:-0}
  local bin=$BIN/${BINOF[$eng]} split="" readers=$threads r pace=""
  local cons=OK out runs="" best_lk best_rn paced
  if [[ "$writers" -ge 0 ]]; then split="--writers $writers"; readers=$((threads-writers)); fi
  [[ "$rate" != 0 ]] && pace="--rename-rate $rate"
  # decontend: writers>=0 -> 16*writers dirs; homogeneous (writers<0) -> 16*threads
  local nw=$(( writers >= 0 ? writers : threads ))
  local nd=$(( 16 * (nw < 1 ? 1 : nw) ))
  for r in $(seq 1 $RUNS); do
    out=$(cd "$BIN" && env LD_PRELOAD="$JE" ./"${BINOF[$eng]}" --nthreads "$threads" \
          $split --ndirs "$nd" --rename-frac "$frac" $pace $COMMON $RUN_EXTRA 2>/dev/null)
    if ! grep -q "conservation: OK" <<< "$out"; then
      cons=FAIL; echo "!! $panel/$eng threads=$threads w=$writers frac=$frac rate=$rate CONSERVATION FAILED" >&2
      continue
    fi
    runs+="$(field "$out" "Mlookups/s:") $(field "$out" "Mrenames/s:")"$'\n'
  done
  read -r best_lk best_rn paced < <(pick_run "$rate" <<< "$runs")
  echo "$panel,$eng,$threads,$writers,$readers,$frac,$rate,$best_lk,$best_rn,$paced,$cons,$SRC_ID" >> "$CSV"
  printf "  %-11s %-16s thr=%-4s w=%-3s frac=%-5s rate=%-8s rd=%8s Mlk/s  wr=%8s Mrn/s  %-5s %s\n" \
    "$panel" "$eng" "$threads" "$writers" "$frac" "$rate" "$best_lk" "$best_rn" "$paced" "$cons" >&2
}

# PANELS (env): space-separated subset of panels to (re)run.  Empty => all, with
# a fresh CSV.  Non-empty => keep the CSV and re-run ONLY those panels, dropping
# their old rows first -- so a single panel can be resswept without disturbing
# the others (e.g. PANELS="readdir_scale readdir_w" after an engine change that
# only affects listing).
PANELS="${PANELS:-}"
want() { [[ -z "$PANELS" || " $PANELS " == *" $1 "* ]]; }
HDR="panel,engine,threads,writers,readers,rename_frac,rate_target,mlookups_s,mrenames_s,paced,conserved,src"
if [[ -z "$PANELS" ]]; then
  echo "$HDR" > "$CSV"
else
  # A CSV with another column layout cannot take new rows: set it aside whole
  # (its rows came from other code anyway) and start fresh.
  if [[ -f "$CSV" && "$(head -1 "$CSV")" != "$HDR" ]]; then
    mv "$CSV" "$CSV.prev"
    echo ">> $CSV had another column layout: moved to $CSV.prev" >&2
  fi
  [[ -f "$CSV" ]] || echo "$HDR" > "$CSV"
  for p in $PANELS; do grep -v "^$p," "$CSV" > "$CSV.tmp" && mv "$CSV.tmp" "$CSV"; done
fi

# Shared reader-scaling sweep points (used by split_scale AND readdir_scale):
# scale readers until readers+writers fill every physical core, one hw thread per
# core.  On the 2x96 EPYC that is 184 readers + 8 writers = 192.
WFIX=8
RMAX=$((NCORE - WFIX))
RATE_FIX=$((WFIX * PER_W))		# the fixed-W panels' offered load
RDPTS=$(for rd in 2 4 8 16 32 48 64 96 128 160 $RMAX; do
          (( rd >= 1 && rd <= RMAX )) && echo "$rd"; done | sort -n -u)

# ---- Panel: HOMOGENEOUS rename-fraction sweep at fixed cores ---------------
if want frac; then
THREADS=48
echo ">> frac panel: homogeneous mix, $THREADS threads, sweep rename fraction" >&2
for f in 0 0.005 0.01 0.02 0.05 0.1 0.2 0.35 0.5; do
  for e in $ENGINES; do run frac "$e" "$THREADS" -1 "$f"; done
done
fi

# ---- Panel: ROLE-SPLIT reader throughput vs OFFERED rename rate ------------
# THE headline: every engine's readers at one offered load per point, so the
# curves differ only by what a rename costs the readers.  An engine's curve ends
# (SHORT) where its writers cannot carry the rate.
if want rate; then
RRATE=${RRATE:-$RMAX}
echo ">> rate panel: $RRATE readers + $WFIX writers, sweep the offered rename rate" >&2
for rt in 10000 30000 100000 300000 1000000 3000000 10000000; do
  for e in $ENGINES; do run rate "$e" $((RRATE+WFIX)) "$WFIX" 1.0 "$rt"; done
done
fi

# ---- Panel: ROLE-SPLIT reader path vs writer load --------------------------
# Each writer offers PER_W renames/s, so the load grows with W as the x axis
# says, and is the same for every engine at each W.
if want split_w; then
RSPLIT=32
echo ">> split_w panel: $RSPLIT dedicated readers + W writers at $PER_W renames/s each" >&2
for w in 1 2 4 8 16 24 32 48; do
  for e in $ENGINES; do run split_w "$e" $((RSPLIT+w)) "$w" 1.0 $((w * PER_W)); done
done
fi

# ---- Panel: ROLE-SPLIT reader scaling at fixed writer load -----------------
if want split_scale; then
echo ">> split_scale panel: $WFIX writers at $RATE_FIX renames/s, sweep readers up to $RMAX (fill $NCORE cores)" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run split_scale "$e" $((rd+WFIX)) "$WFIX" 1.0 "$RATE_FIX"; done
done
fi

# ---- Panel: SATURATION -- split_scale with the writers flat out -------------
# Not a reader comparison (each engine's readers face its own writers' rate);
# it is where each engine's writers top out under reader load.
if want sat_scale; then
echo ">> sat_scale panel: $WFIX writers FLAT OUT, sweep readers up to $RMAX" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run sat_scale "$e" $((rd+WFIX)) "$WFIX" 1.0; done
done
fi

# ---- Panel: READDIR reader scaling (directory listing under rename load) ----
# Readers enumerate a random dir (dc_readdir) instead of a leaf lookup.  Only the
# writers own the namespace, so dir size is fixed as readers scale.  Compares the
# txn lock-free RCU child-walk against the seqlock per-directory rwsem (the honest
# kernel-inode-rwsem analogue, not one global lock).  Both txn arms run identical
# readdir READER code (it reads no generation counter at all); per-node's small
# edge here is a second-order writer-side effect (the global rename_gen cacheline
# the concurrent writers contend), not the reader path.
if want readdir_scale; then
RDLEAVES=64                         # 64 leaves/writer * 8 writers / 16 dirs = 32 kids/dir
RUN_EXTRA="--readdir --leaves $RDLEAVES"
echo ">> readdir_scale panel: $WFIX writers, --readdir, sweep readers up to $RMAX" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run readdir_scale "$e" $((rd+WFIX)) "$WFIX" 1.0 "$RATE_FIX"; done
done
RUN_EXTRA=""
fi

# ---- Panel: REVERSE-WALK reader scaling (dentry_path_raw under rename load) --
# Readers report a random leaf's CURRENT path from a handle pinned at seed time
# (dc_dentry_path) instead of looking a path up.  This is the one reader the
# kernel itself serves with a whole-path snapshot (__dentry_path brackets on
# rename_lock), so it is the like-for-like comparison for engines that give
# every walk a snapshot: the seqlock arms both run the kernel's reverse walk.
# The readers own no leaves (bench writers_own): every handle is one of the
# writers' WFIX*LEAVES leaves, whatever the reader count.
if want dpath_scale; then
RUN_EXTRA="--dpath"
echo ">> dpath_scale panel: $WFIX writers, --dpath, sweep readers up to $RMAX" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run dpath_scale "$e" $((rd+WFIX)) "$WFIX" 1.0 "$RATE_FIX"; done
done
RUN_EXTRA=""
fi

# ---- Panel: POSITIVE-hit reader scaling (lookups of objects being moved) ----
# Readers look up each leaf's CURRENT path, published by its owner (bench
# --hit-current), so nearly every lookup is a positive hit on an object the
# writers are renaming right now: the dense reader/rename interaction the
# probing panels (split_scale: ~97% negative hits) barely touch.  As in
# dpath_scale the readers own no leaves, so that holds at every reader count.
if want hit_scale; then
RUN_EXTRA="--hit-current"
echo ">> hit_scale panel: $WFIX writers, --hit-current, sweep readers up to $RMAX" >&2
for rd in $RDPTS; do
  for e in $ENGINES; do run hit_scale "$e" $((rd+WFIX)) "$WFIX" 1.0 "$RATE_FIX"; done
done
RUN_EXTRA=""
fi

# ---- Panel: READDIR reader throughput vs writer (rename) load ---------------
# Fixed reader pool, sweep writers.  Namespace is held constant (RDTOTAL leaves,
# leaves=RDTOTAL/W), so dir size stays fixed while the offered rename rate
# (PER_W per writer) -- and thus the per-dir wrlock exclusion the seqlock
# readdir suffers -- rises with W.  Isolates
# the writer-exclusion axis (the txn RCU walk never blocks on a writer).
if want readdir_w; then
RRD=32; RDTOTAL=1024
echo ">> readdir_w panel: $RRD readers, --readdir, sweep writers (namespace fixed $RDTOTAL)" >&2
for w in 1 2 4 8 16 24 32 48; do
  RUN_EXTRA="--readdir --leaves $((RDTOTAL / w))"
  for e in $ENGINES; do run readdir_w "$e" $((RRD+w)) "$w" 1.0 $((w * PER_W)); done
done
RUN_EXTRA=""
fi

echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
