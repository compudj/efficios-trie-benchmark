#!/bin/bash
# Sweep D: separate the two things that depress read throughput as writers scale.
#
# THE PROBLEM.  Sweep C (run_p2_writer_scaling.sh) measures reader throughput
# against writer count and CANNOT ATTRIBUTE what it finds, because two mechanisms
# move together:
#
#   proxies    a reader lands on a TAGGED slot and pays an indirection plus a
#              status load.  This is a cost of the FACILITY.
#   coherency  writers dirty the cache lines readers are walking.  EVERY
#              concurrent-writer design pays this; those lines would be
#              invalidated under a plain lock too.
#
# The two give OPPOSITE verdicts on sec:tombstoneplacement's claim that the read
# side is "byte-for-byte and cycle-for-cycle what it would be with no facility
# present".  If the degradation is coherency, the claim survives -- the facility
# added nothing.  If it is proxies, the claim is damaged.  So an unattributed
# curve cannot defend that sentence, and reading one as though it could is how
# the 3-skiplist knee got misread as a socket wall.
#
# THE SEPARATION.  BENCH_WRITE_RATE=N paces each writer to N toggles/s in
# absolute time.  Hold the AGGREGATE rate constant while varying the writer
# count -- rate = TOTAL / writers -- and then across the sweep:
#
#   total stores/s               CONSTANT  => coherency traffic ~constant
#   concurrent in-flight txns    ~ writers => proxy encounters RISE
#
# so a reader-throughput decline across this sweep is attributable to PROXIES.
#
# THE CONTROL IS THE CONSTANT RATE, NOT ANOTHER ENGINE.  The cleanest comparison
# is txn_list AGAINST ITSELF across writer counts: same engine, same traversal,
# same reader code, mutation rate pinned -- so the only variable left is how many
# transactions are in flight at once.  A decline there is the proxy effect.
#
# rculist is a SECONDARY check and is read for SHAPE ONLY.  It has RCU readers
# and NO proxies (plain pointer publish), so if its reader curve is flat where
# txn_list's declines, the decline is proxies; if BOTH decline, the constant-rate
# premise is incomplete -- N writers touching distinct lines means more SHARERS
# and more invalidation broadcast than one writer storing N times as often, which
# is a coherency effect the pacer does not hold constant.  Either outcome is
# worth knowing before a reader number reaches the paper.
#
# Its ABSOLUTE level is NOT comparable: rculist walks forward twice where the
# transacted lists walk forward-then-backward, which the harness notes is "a
# systematic edge to whichever engine reverses" with nodes across 24 NUMA nodes.
# Shape is comparable because each arm is its own baseline; levels are not.
#
# MUTEX WAS DROPPED, having failed as a control in sweep C: its READERS take the
# same lock its writers do (mtx_read calls pthread_mutex_lock), so 32 readers
# starve the writers -- its measured write rate collapsed to 0.00-0.05 Mops/s,
# far below any TOTAL worth pacing at, and its readers serialize too.  It is a
# different concurrency model on the read side, not a no-proxy version of this
# one.
#
# TOTAL must be sustainable by ONE writer, since W=1 asks a single writer to
# carry all of it.  A run whose achieved write_mops falls short of TOTAL at low W
# has silently stopped being a constant-rate sweep, so the achieved rate is
# recorded per point and checked.
#
# THE PACER HAS ITS OWN CEILING, MEASURED 2026-07-28: ~110k toggles/s PER WRITER.
# It paces with clock_nanosleep(TIMER_ABSTIME) once per toggle, and the wakeup
# latency floor is ~9us, so a single writer cannot exceed roughly 110k/s whatever
# the target says.  This is the PACER, not the engine: unpaced, one writer with 32
# readers does ~490k/s.
#
# So TOTAL must be under ~100k for W=1 to participate.  The 2026-07-28 run used
# TOTAL=200000 and W=1 came out at 0.11 Mops/s against a 0.20 target while every
# W>=2 point hit 0.20 exactly -- so that run is VALID FROM W=2 ONWARD and W=1 is
# excluded, normalising to W=2 instead.  W=2..160 is an 80x span in writer count
# at pinned mutation, which is the experiment; nothing was lost.  Either keep
# TOTAL<=100000 and use W=1, or keep 200000 and normalise to W=2 -- but do not
# quote a W=1 point from a 200000 run.
set -u

REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/bench_list_scale
CSV=${CSV:-$REPO/scripts/p2_readcost_isolate.csv}
DUR=${DUR:-2}
RUNS=${RUNS:-3}
ARMS=${ARMS:-"txn_list txn_sw_list rculist"}
WRITERS=${WRITERS:-"1 2 4 8 16 32 64 96 128 160"}
READERS=${READERS:-32}
TOTAL=${TOTAL:-0}		# aggregate toggles/s across ALL writers -- MUST be set
MAXTH=192

if [ "$TOTAL" -le 0 ]; then
	echo "ERROR: set TOTAL (aggregate toggles/s) from sweep A's W=1 point." >&2
	echo "       e.g. TOTAL=320000 $0" >&2
	exit 2
fi

# ALLOCATOR.  jemalloc with percpu_arena:phycpu, paired deliberately with the
# harness's per-CPU call_rcu reclaim workers (on by default).  The harness says
# why: per-CPU reclaim "pays off most when the allocator is ALSO per-CPU ... so a
# node freed on CPU X returns to the same pool the writer on CPU X allocates
# from".  Supplying only half the pairing -- per-CPU reclaim on glibc malloc --
# leaves descriptor alloc/free crossing CPUs through glibc arenas at 191 threads,
# which is a plausible chunk of the sublinearity and is NOT a property of the
# facility under test.  A first attempt at this sweep ran that way; its partial
# data is kept as p2_writer_scaling_glibc_partial.csv rather than deleted,
# because the allocator sensitivity is itself worth knowing.
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
MALLOC_CONF_VAL=${MALLOC_CONF_VAL:-percpu_arena:phycpu}
[ -r "$JE" ] || { echo "ERROR: jemalloc not readable at $JE" >&2; exit 2; }
LD_PRELOAD=$JE MALLOC_CONF=$MALLOC_CONF_VAL /bin/true 2>&1 | grep -q "Invalid conf" \
	&& { echo "ERROR: jemalloc rejected MALLOC_CONF=$MALLOC_CONF_VAL" >&2; exit 2; }

fail=0
echo "sweep,arm,writers,readers,rate_per_writer,total_target,list_size,churn,run,write_mops,read_mvisits,violations,status" > "$CSV"

echo ">> D: constant aggregate write rate ($TOTAL toggles/s), writers vary" >&2
for w in $WRITERS; do
	rate=$((TOTAL / w))
	[ "$rate" -lt 1 ] && rate=1
	ch=$((64 * w)); ls=$((2 * ch))
	for arm in $ARMS; do
		for r in $(seq 1 "$RUNS"); do
			out=$(env LD_PRELOAD="$JE" MALLOC_CONF="$MALLOC_CONF_VAL" LIST_SIZE="$ls" CHURN="$ch" DURATION_SEC="$DUR" \
				BENCH_WRITESCALE=1 BENCH_FIXED_WRITERS="$w" \
				BENCH_READERS="$READERS" BENCH_WRITE_RATE="$rate" \
				timeout 900 "$BIN" "$arm" "$MAXTH" 2>/dev/null)
			line=$(grep -v '^#' <<< "$out" | awk 'NF>=4' | tail -1)
			if [ -z "$line" ]; then
				echo "!! D/$arm w=$w run=$r NO OUTPUT" >&2; fail=1
				echo "D,$arm,$w,$READERS,$rate,$TOTAL,$ls,$ch,$r,,,,NOOUT" >> "$CSV"
				continue
			fi
			mvis=$(awk '{print $2}' <<< "$line")
			mops=$(awk '{print $3}' <<< "$line")
			viol=$(awk '{print $4}' <<< "$line")
			if [ "${viol:-1}" != "0" ]; then
				echo "!! D/$arm w=$w run=$r VIOLATIONS=$viol" >&2; fail=1
				echo "D,$arm,$w,$READERS,$rate,$TOTAL,$ls,$ch,$r,$mops,$mvis,$viol,VIOLATION" >> "$CSV"
				continue
			fi
			echo "D,$arm,$w,$READERS,$rate,$TOTAL,$ls,$ch,$r,$mops,$mvis,$viol,ok" >> "$CSV"
			printf "  D  %-12s w=%-4s rate/w=%-8s run=%s  wr=%-10s rd=%s\n" \
				"$arm" "$w" "$rate" "$r" "$mops" "$mvis" >&2
		done
	done
done

echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
echo ">> CHECK BEFORE USING: achieved write_mops must be ~flat across writer" >&2
echo "   counts and close to TOTAL/1e6 = $(awk -v t="$TOTAL" 'BEGIN{printf "%.4f", t/1e6}'). Where it is not, the" >&2
echo "   point was rate-limited by the engine rather than by the pacer and the" >&2
echo "   constant-traffic premise does not hold there." >&2
[ "$fail" -eq 0 ] || echo ">> WARNING: failures/violations present" >&2
exit "$fail"
