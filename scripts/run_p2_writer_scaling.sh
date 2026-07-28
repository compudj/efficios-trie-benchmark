#!/bin/bash
# P2 writer scaling on the bidirectional list: what dropping exclusion buys.
#
# THIS IS THE MEASUREMENT P2'S PREMISE RESTS ON.  The companion paper's
# flip-latch REQUIRES writer exclusion -- it cannot scale writers at all, by
# construction -- and P2 exists to drop that requirement.  The paper currently
# asserts the consequence without measuring it.
#
# The list is P1's worked structure, deliberately: the series keeps the same
# structures across papers so what changes between them is the facility and not
# the example.
#
# ARMS
#   txn_list     P2's multi-writer transacted list.
#   txn_sw_list  P1's single-writer flip-latch list.  The harness serializes its
#                writers through g_su_wlock -- which is not a handicap, it is
#                what exclusion MEANS at N writers.  This is the honest baseline.
#   mutex        plain pthread_mutex bidirectional list.  Included so a flat
#                txn_sw_list curve reads as "serialization is flat", not as
#                "their old facility was slow".
#
# THREE SWEEPS, because one cannot answer these questions at once and conflating
# them is
# exactly the error that made the 3-skiplist curve unusable (a fixed-size
# structure with a growing thread count measures contention collapse and gets
# mistaken for a scaling ceiling).
#
#   A  SCALING, DISJOINT.  CHURN = 64 x writers, so every writer keeps the same
#      elbow room and a rolloff is the FACILITY rather than a shrinking working
#      set.  Writers are disjoint by construction here: writer wid owns churn
#      slots wid, wid+nw, wid+2nw..., each sitting after a unique spread-out
#      anchor, and the harness refuses to run when CHURN < writers.  So NO two
#      writers ever touch the same slot.
#
#      That is the point, not a weakness: the indictment of exclusion is that it
#      serializes writers even when their work is COMPLETELY DISJOINT.  But it
#      also means this sweep never fails a CAS, never aborts, and never enters
#      the escalation lane -- so it says nothing about sec:progress.
#
#   B  CONTENTION.  Fixed LIST_SIZE=2000, CHURN=1000, BENCH_RANDOM_POS -- writers
#      pick random positions, so collisions run at roughly writers/LIST_SIZE
#      (~10% at 192 writers).  This is where aborts and the fair-mutex lane
#      actually appear, and it is the sweep that bears on the bounded-blocking
#      argument.
#
#      CAVEAT THAT MUST REACH THE CAPTION: only txn_list has a write_random
#      hook.  txn_sw_list, mutex and fairmutex fall back to the strided disjoint
#      walk, because random_pos is (g_random_pos && eng->write_random != NULL).
#      Those arms are mutex-bound, so which slot they touch does not change their
#      throughput -- but the MW arm is doing strictly MORE work in this sweep
#      (random selection, real collisions, retries).  B is therefore CONSERVATIVE
#      toward P2, and the caption must say the arms did not run an identical
#      workload rather than implying they did.
#
#   C  READERS UNDER SCALING WRITERS.  Readers fixed at 32, writers 1..160,
#      sized as A.  This is the one read-side question the companion paper
#      CANNOT answer: every reader number in P1 is a ONE-WRITER number, because
#      the flip-latch permits no more (P1 main.tex:1860).  A reader's cost turns
#      on how often it lands on a TAGGED slot, and that rises with writer
#      concurrency -- so P1's reader figure is the best case, not the general
#      one, and P2 promises "the read side unchanged" in a regime P1 never
#      measured.  txn_sw_list is the useful contrast rather than mere baseline:
#      its readers can only ever meet ONE writer's proxies however many writers
#      are queued behind the mutex, which separates "more in-flight
#      transactions" from "more threads".
#
# A and B run with ZERO readers, deliberately: the claim under test there is the
# writer axis, and readers would confound it and eat cores from the writer range
# (the harness caps workers at the physical-core count).  Their captions must say
# so -- an RCU facility measured with no readers is being shown its anti-workload,
# which is fair for THAT claim and for no other.  C is where readers appear.
#
# Every run is recorded, not just the best.  Violations are a correctness gate:
# the ring stays sorted at every instant, so a forward walk must see strictly
# increasing keys -- any coherence defect shows up as a nonzero count and
# invalidates the point.
set -u

REPO=/mnt/data/efficios/git/efficios-trie-benchmark
BIN=$REPO/bench_list_scale
CSV=${CSV:-$REPO/scripts/p2_writer_scaling.csv}
DUR=${DUR:-2}
RUNS=${RUNS:-3}
ARMS=${ARMS:-"txn_list txn_sw_list mutex"}
WRITERS=${WRITERS:-"1 2 4 8 16 32 64 96 128 160 191 192"}
# Sweep C: readers occupy cores too, and the harness caps workers at the
# physical-core count, so the writer range stops where readers+writers = 192.
READERS=${READERS:-32}
WRITERS_C=${WRITERS_C:-"1 2 4 8 16 32 64 96 128 160"}
MAXTH=192

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

# point <sweep> <arm> <writers> <list_size> <churn> <random?> <readers>
point() {
	local sweep=$1 arm=$2 w=$3 ls=$4 ch=$5 rnd=$6 rd=${7:-0} r out line mops mvis viol
	for r in $(seq 1 "$RUNS"); do
		out=$(env LD_PRELOAD="$JE" MALLOC_CONF="$MALLOC_CONF_VAL" LIST_SIZE="$ls" CHURN="$ch" DURATION_SEC="$DUR" \
			BENCH_WRITESCALE=1 BENCH_FIXED_WRITERS="$w" BENCH_READERS="$rd" \
			${rnd:+BENCH_RANDOM_POS=1} \
			timeout 900 "$BIN" "$arm" "$MAXTH" 2>/dev/null)
		# harness prints: writers read_mvisits write_mops violations
		line=$(grep -v '^#' <<< "$out" | awk 'NF>=4' | tail -1)
		if [ -z "$line" ]; then
			echo "!! $sweep/$arm w=$w run=$r NO OUTPUT" >&2
			fail=1
			echo "$sweep,$arm,$w,$rd,$ls,$ch,$r,,,,NOOUT" >> "$CSV"
			continue
		fi
		mvis=$(awk '{print $2}' <<< "$line")
		mops=$(awk '{print $3}' <<< "$line")
		viol=$(awk '{print $4}' <<< "$line")
		if [ "${viol:-1}" != "0" ]; then
			echo "!! $sweep/$arm w=$w run=$r VIOLATIONS=$viol" >&2
			fail=1
			echo "$sweep,$arm,$w,$rd,$ls,$ch,$r,$mops,$mvis,$viol,VIOLATION" >> "$CSV"
			continue
		fi
		echo "$sweep,$arm,$w,$rd,$ls,$ch,$r,$mops,$mvis,$viol,ok" >> "$CSV"
		printf "  %-10s %-12s w=%-4s r=%-3s run=%s  wr=%-10s rd=%s\n" \
			"$sweep" "$arm" "$w" "$rd" "$r" "$mops" "$mvis" >&2
	done
}

echo "sweep,arm,writers,readers,list_size,churn,run,write_mops,read_mvisits,violations,status" > "$CSV"

echo ">> A: scaling, disjoint -- CHURN = 64 x writers (constant room per writer)" >&2
for w in $WRITERS; do
	ch=$((64 * w)); ls=$((2 * ch))
	for a in $ARMS; do point scaling "$a" "$w" "$ls" "$ch" ""; done
done

echo ">> B: contention -- fixed LIST_SIZE=2000 CHURN=1000, random positions" >&2
for w in $WRITERS; do
	for a in $ARMS; do point contention "$a" "$w" 2000 1000 1; done
done

echo ">> C: readers under scaling writers -- readers=$READERS fixed, sizing as A" >&2
for w in $WRITERS_C; do
	ch=$((64 * w)); ls=$((2 * ch))
	for a in $ARMS; do point readers "$a" "$w" "$ls" "$ch" "" "$READERS"; done
done

echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
[ "$fail" -eq 0 ] || echo ">> WARNING: failures/violations present; see status column" >&2
exit "$fail"
