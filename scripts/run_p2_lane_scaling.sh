#!/bin/bash
# The optimistic-vs-serialized-lane factor, as a function of WRITER COUNT.
#
# This produces p2_lane_scaling.csv, the curve behind P2 sec:escalation.  It was
# captured ad hoc on 2026-07-28 and is scripted here so the re-run under the
# corrected allocator configuration is reproducible rather than retyped.
#
# WHY IT IS A CURVE AND NOT A FACTOR.  rcu-txn.h claimed for a long time that the
# optimistic path "beats the serialized lane by a factor of 2.6".  It is not a
# property of the workload: the lane is ONE global critical section, so its
# throughput is FLAT in the writer count while the optimistic path scales.  The
# ratio is therefore a function of scale and means nothing without one -- 2.6x is
# about the TWO-writer point, and at 192 it is three orders of magnitude.  Both
# the header and the paper are corrected; this script is the evidence.
#
# THE CONTROL IS THE 1-WRITER POINT.  There the two builds must come out
# IDENTICAL: an uncontended lane costs nothing, so any gap at one writer would
# mean the always-escalate build differs from the optimistic one in some way
# other than the escalation policy, and the whole curve would be suspect.
#
# The serialized arm is PER_COST_NUM=0 with FALLBACK=0, so every transaction
# takes the lane on its first attempt.  That is the LIMIT CASE, not the shipped
# policy, which escalates only a handle that is losing.
set -u

REPO=/mnt/data/efficios/git/efficios-trie-benchmark
ARMS=${ARMS:-$REPO/arms-p2}
JE=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2
# See run_p2_engine_remeasure.sh: jemalloc's DEFAULT arena policy is not the
# configuration this harness wants; the per-CPU descriptor slab and per-CPU
# reclaim both assume a node freed on CPU X returns to CPU X's pool.
export MALLOC_CONF=${MALLOC_CONF:-percpu_arena:phycpu}
[ -r "$JE" ] || { echo "ERROR: jemalloc not readable at $JE" >&2; exit 2; }
LD_PRELOAD=$JE /bin/true 2>&1 | grep -q "Invalid conf" \
	&& { echo "ERROR: jemalloc rejected MALLOC_CONF=$MALLOC_CONF" >&2; exit 2; }

CSV=${CSV:-$REPO/scripts/p2_lane_scaling.csv}
DUR=${DUR:-2000}
RUNS=${RUNS:-3}
CORES=${CORES:-"1 2 4 8 16 32 64 128 192"}

sp() { echo $((16 + 3 * $1)); }
field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if(tolower($i)==tolower(L)){print $(i+1);exit}}' <<< "$1"; }

fail=0
echo "cores,arm,run,mmoves_s" > "$CSV"

for c in $CORES; do
	b=$((3840 / c))
	for a in pin-sole pin-serial; do
		for r in $(seq 1 "$RUNS"); do
			out=$(LD_PRELOAD=$JE timeout 900 "$ARMS/$a" \
				--nupdaters "$c" --nreaders 0 --cpustride 1 \
				--updatespacing "$(sp "$b")" --duration "$DUR" \
				--movesper 3 2>/dev/null); rc=$?
			if [ "$rc" -ne 0 ] || [[ "$out" == *"CONSERVATION FAILED"* ]]; then
				echo "!! $a c=$c run=$r RC=$rc CONSERVATION/EXIT FAILURE" >&2
				fail=1; echo "$c,$a,$r," >> "$CSV"; continue
			fi
			mm=$(field "$out" "Mmoves/s:")
			echo "$c,$a,$r,${mm:-}" >> "$CSV"
			printf "  lane %-11s c=%-4s run=%s  %10s Mmoves/s\n" "$a" "$c" "$r" "$mm" >&2
		done
	done
done

echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
echo ">> CHECK: the 1-writer point must be IDENTICAL across the two arms." >&2
[ "$fail" -eq 0 ] || echo ">> WARNING: failures present" >&2
exit "$fail"
