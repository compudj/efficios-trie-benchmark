#!/bin/bash
# Matched-name-width control for the mark arm (REVIEW.md open item).
#
# The mark arm retires d_seq and spends the 8 bytes on the inline name, so
# DC_NAME_MAX is 40 there and 32 everywhere else.  The DENTRY is unaffected --
# sizeof stays 168 (176 for bucketlock) and d_hash stays at @56, measured -- but
# `struct qstr` is shared with `struct dc_path`, so the width ALSO sets:
#
#   sizeof(struct dc_path)   964 -> 1156 B   (per-lookup stack object)
#   per-component path copy   40 ->   48 B   (paid once per path component)
#   precomputed leaf-qstr table        +20%  (reader working set)
#
# all of which sit on the reader's hot path and none of which are the mechanism
# under test.  That is what makes a mark-vs-{global,per-node,seqlock} reader
# table not apples-to-apples -- and note the tax runs AGAINST the mark arm, so
# the published mark reader numbers are if anything conservative.
#
# Three arms per engine, which is what separates the two changes:
#   <e>            the shipped arm, DC_NAME_MAX 40
#   <e>-w32        DC_NAME_MAX=32 + 8 B of dead padding: dentry BYTE-IDENTICAL
#                  to the shipped arm, harness path matched to the baselines.
#                  This is the control -- it moves the harness footprint ONLY.
#   <e>-w32-shrink DC_NAME_MAX=32, no padding: dentry loses 8 B and d_hash moves
#                  @56 -> @48.  Prices what the freed bytes buy.
#
# Verdict rule: if -w32 lands on the shipped arm's curve, the width was never a
# confound and the REVIEW caveat retires WITH EVIDENCE; if it does not, the
# published mark reader numbers were paying a harness tax and the S3/S4 tables
# need the control column beside them.
#
# Output: scripts/dcache_namewidth.csv (plot with plot_dcache_namewidth.py)
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/experiments/dcache
# Overridable: the header write below TRUNCATES, so a partial-panel re-run must
# be able to write elsewhere rather than destroy the panel it is not re-running.
CSV=${CSV:-$REPO/scripts/dcache_namewidth.csv}

WRITERS=${WRITERS:-8}
NDIRS=$((16 * WRITERS))
DUR=${DUR:-1000}
RUNS=${RUNS:-7}		# more than the usual 5: this is a null-result test, and
			# "within noise" is only a claim if the noise is measured
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
Bd=$REPO/urcu-txn-build
INC="-I$Bd/include -I$BIN"
LIB="-L$Bd/src/.libs -Wl,-rpath,$Bd/src/.libs -lurcu-qsbr -lurcu-common -lpthread"
CC=${CC:-gcc}
CFLAGS="-O2 -g -pthread -march=native"

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0 2>/dev/null)
[[ -n "$CPULIST" ]] && PIN="--cpulist $CPULIST" && \
  NCORE=$(tr ',' '\n' <<< "$CPULIST" | grep -c .) || { PIN=""; NCORE=$(nproc); }
[[ -f "$JE" ]] || { echo "jemalloc not at $JE (set JE=)"; exit 1; }

# arm ; engine source ; defines
declare -A ASRC=( [txn-mark]="dcache_txn.c" [txn-mark-w32]="dcache_txn.c" \
                  [txn-mark-w32-shrink]="dcache_txn.c" \
                  [bucketlock]="dcache_bucketlock.c" \
                  [bucketlock-w32]="dcache_bucketlock.c" \
                  [bucketlock-w32-shrink]="dcache_bucketlock.c" \
                  [txn-pernode]="dcache_txn.c" )
declare -A ADEF=( [txn-mark]="-DDC_MARK_GEN" \
                  [txn-mark-w32]="-DDC_MARK_GEN -DDC_NAME_MAX=32 -DDC_NAME_PAD=8" \
                  [txn-mark-w32-shrink]="-DDC_MARK_GEN -DDC_NAME_MAX=32" \
                  [bucketlock]="-DDC_MARK_GEN" \
                  [bucketlock-w32]="-DDC_MARK_GEN -DDC_NAME_MAX=32 -DDC_NAME_PAD=8" \
                  [bucketlock-w32-shrink]="-DDC_MARK_GEN -DDC_NAME_MAX=32" \
                  [txn-pernode]="-DDC_PER_NODE_GEN" )
# txn-pernode is carried as the reference the mark arm is compared AGAINST in
# the published tables: it is already at width 32, so it anchors the scale.
ARMS=${ARMS:-"txn-pernode txn-mark txn-mark-w32 txn-mark-w32-shrink \
              bucketlock bucketlock-w32 bucketlock-w32-shrink"}

# Which panels to run: both by default.  `NWPANELS=readdir` re-runs only the
# per-DIRENT panel (49 runs, not 294) -- what the --readdir-names fix needed.
NWPANELS=${NWPANELS:-"lookup readdir"}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
field() { grep -oP "$2 \K[0-9.]+" <<< "$1" | head -1; }

RMAX=$((NCORE - WRITERS))
RDPTS=${RDPTS:-$(for rd in 8 32 64 128 $RMAX; do
          (( rd >= 1 && rd <= RMAX )) && echo "$rd"; done | sort -n -u)}

echo "arm,panel,readers,writers,run,mlookups_s,mrenames_s,conserved" > "$CSV"
for a in $ARMS; do
  (cd "$BIN" && $CC $CFLAGS ${ADEF[$a]} $INC -o "$TMP/lookup_$a" \
      bench_dcache.c "${ASRC[$a]}" $LIB) || { echo "build $a failed"; exit 1; }
  (cd "$BIN" && $CC $CFLAGS ${ADEF[$a]} $INC -DDC_SPLIT_KEEPID -o "$TMP/readdir_$a" \
      bench_dcache_churn.c "${ASRC[$a]}" $LIB) || { echo "build $a churn failed"; exit 1; }

  [[ " $NWPANELS " == *" lookup "* ]] && for rd in $RDPTS; do
    # Every run is recorded, not just the best: the question is whether two arms
    # differ, and that needs the spread, not a max.
    for r in $(seq 1 $RUNS); do
      out=$(env LD_PRELOAD="$JE" "$TMP/lookup_$a" --nthreads $((WRITERS+rd)) \
            --writers $WRITERS --rename-frac 1.0 --ndirs $NDIRS --depth 4 \
            --leaves 32 --duration $DUR $PIN 2>/dev/null)
      cons=OK; grep -q "conservation: OK" <<< "$out" || cons=FAIL
      echo "$a,lookup,$rd,$WRITERS,$r,$(field "$out" 'Mlookups/s:'),$(field "$out" 'Mrenames/s:'),$cons" >> "$CSV"
    done
    printf "  %-22s lookup  rd=%-4s done\n" "$a" "$rd" >&2
  done

  # readdir is the most exposed panel: dc_readdir hands out one qstr per DIRENT,
  # so the width is paid per child listed rather than per path component.  The
  # churn harness reports the readdir CALL rate as Mlookups/s and the enumerated
  # -children rate as Mdirents/s; the latter is the one the width should move.
  #
  # --readdir-names, NOT --readdir: dc_readdir(fn == NULL) only counts and never
  # materializes a qstr, so plain --readdir cannot see a name width at all and
  # this panel measured nothing until the callback was passed.  The run asserts
  # it got the mode it asked for rather than trusting the flag.
  [[ " $NWPANELS " == *" readdir "* ]] && for r in $(seq 1 $RUNS); do
    out=$(env LD_PRELOAD="$JE" "$TMP/readdir_$a" --writers $WRITERS \
          --readers "$RMAX" --readdir-names --duration $DUR $PIN 2>/dev/null)
    grep -q "READDIR names: 1" <<< "$out" || {
      echo "FATAL: $a readdir ran WITHOUT the name callback -- the panel would"
      echo "       be vacuous (see the --readdir-names comment).  Aborting."; exit 1; }
    cons=OK; grep -q "conservation: OK" <<< "$out" || cons=FAIL
    echo "$a,readdir,$RMAX,$WRITERS,$r,$(field "$out" 'Mlookups/s:'),$(field "$out" 'Mdirents/s:'),$cons" >> "$CSV"
  done
  printf "  %-22s readdir rd=%-4s done\n" "$a" "$RMAX" >&2
done

echo ">> DONE: $(( $(wc -l < "$CSV") - 1 )) rows -> $CSV" >&2
echo ">> compare: txn-mark vs txn-mark-w32 (harness footprint only)," >&2
echo ">>          txn-mark-w32 vs txn-mark-w32-shrink (the freed 8 bytes)" >&2
