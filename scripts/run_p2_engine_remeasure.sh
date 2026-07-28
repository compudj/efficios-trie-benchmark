#!/bin/bash
# P2 remeasure: the sole-driver-vs-helping A/B, and the optimistic-vs-serialized
# lane factor.  Replaces the 2026-07-10 capture behind
# scripts/txn_vs_existence_skiplist_engine.csv, whose numbers P2 still quotes.
#
# WHY FOUR ARMS AND TWO ENGINE TREES
# ----------------------------------
# The helping install was RETIRED from the engine in a07b67ba (2026-07-11 16:01),
# so it cannot be built at P2's pin (b3e23f9f) at all.  The last commit where BOTH
# disciplines build from one tree is 97443472 (2026-07-11 12:27), which made the
# sole-driver configuration the default and kept -DURCU_MCAS_STOCK as the revert
# to helping.  So:
#
#   old-sole     97443472, default            ] the A/B, internally consistent:
#   old-helping  97443472, -DURCU_MCAS_STOCK  ] one tree, one bench, one -D apart
#   pin-sole     b3e23f9f, default            ] the DRIFT CHECK: does the
#                                             ] sole-driver arm still measure the
#                                             ] same on the shipped engine?
#   pin-serial   b3e23f9f, forced lane        ] the optimistic-vs-serialized factor
#
# The drift check is what connects a historical A/B to the engine P2 discloses.
# Without it the A/B describes a tree nobody can build; with it, the claim is
# "helping lost, and the winner is still what ships".
#
# THREE TRAPS FOUND WHILE PREPARING THIS -- do not "simplify" them away
# --------------------------------------------------------------------
# 1. --ryw was RETIRED from the bench (1c1b124) when RYW became unconditional.
#    The old engine_cmp script still passes `--ryw 1`, and the bench's arg parser
#    ends in `else usage()`, which exit(2)s.  Re-running that script today
#    produces zeros for every txn arm, not an error you would notice in a CSV.
#
# 2. At 97443472 URCU_TXN_RYW_DEFAULT is 0 -- RYW was opt-in, and the bench used
#    to call urcu_txn_enable_ryw().  That call is gone.  Building the historical
#    arms without -DURCU_TXN_RYW_DEFAULT=1 therefore runs movesper 3 WITHOUT
#    read-your-own-writes, which the batched ordered moves require: two edits
#    collide on one pred->next[L] slot.  It corrupts rather than errors.
#
# 3. -DURCU_MCAS_STOCK alone flips FIVE things, not one: the install discipline,
#    URCU_TXN_AGE_ESCALATE, Bloom words 16->1, Bloom k 3->1, and AGE0_TRYLATCH.
#    The 2026-07-10 A/B varied ONLY the install discipline (both arms had the
#    wide Bloom).  So the helping arm restores the txn-layer defaults explicitly.
#    Without that, the "sole-driver win" silently absorbs the Bloom widening and
#    the number comes out too big.  (AGE0_TRYLATCH is NOT restored: a static
#    assert forbids it without NO_HELP+NO_STEAL, so it is structurally part of
#    helping's cost, not a free variable.)
#
# Arms are built by scripts/build_p2_remeasure_arms.sh.  Every run is recorded,
# not just the best, so the spread is visible rather than assumed.
set -u

# ADDRESS-SPACE LAYOUT IS PINNED (setarch -R).  ASLR moves the heap base and
# changes cache set/way conflicts, which made the sibling list benchmark cleanly
# BIMODAL (two clusters 19% apart, 20 reps splitting 8/12).  The skiplist panels
# here are not visibly bimodal -- their worst spread is 2.3% -- but the SERIALIZED
# lane arm is: at 64 writers its three runs came out 0.0575 / 0.0921 / 0.1266, a
# 75% spread on the DENOMINATOR of the lane factor, which put that one cell
# anywhere between ~330x and ~730x.  Every other point in that curve sits within
# 0-7%.  Pin the layout so the quiet points stay quiet and the noisy one has a
# chance to settle.

REPO=/mnt/data/efficios/git/efficios-trie-benchmark
ARMS=${ARMS:-$REPO/arms-p2}
JE=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2
# ALLOCATOR CONFIG.  jemalloc alone is not the configuration this harness wants:
# the engine's per-CPU descriptor slab and liburcu's reclaim both assume a node
# freed on CPU X returns to the pool CPU X allocates from, which is what
# percpu_arena:phycpu gives.  The 2026-07-28 capture preloaded jemalloc with its
# DEFAULT arena policy, so the descriptor alloc/free path crossed arenas at 192
# writers.  Both arms did it equally, so the A/B ratio stands -- but the absolute
# Mmoves/s were understated and the two P2 measurements disagreed on setup.
export MALLOC_CONF=${MALLOC_CONF:-percpu_arena:phycpu}
[ -r "$JE" ] || { echo "ERROR: jemalloc not readable at $JE" >&2; exit 2; }
LD_PRELOAD=$JE /bin/true 2>&1 | grep -q "Invalid conf" \
	&& { echo "ERROR: jemalloc rejected MALLOC_CONF=$MALLOC_CONF" >&2; exit 2; }
CSV=${CSV:-$REPO/scripts/p2_engine_remeasure.csv}
DUR=${DUR:-2000}
RUNS=${RUNS:-5}

sp() { echo $((16 + 3 * $1)); }
field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if(tolower($i)==tolower(L)){print $(i+1);exit}}' <<< "$1"; }

fail=0

# run <panel> <arm> <cores> <b>  -- appends one row PER RUN
run() {
	local panel=$1 arm=$2 cores=$3 b=$4 r out mm ns rc
	local kps=$((b * cores))
	for r in $(seq 1 "$RUNS"); do
		out=$(setarch "$(uname -m)" -R env LD_PRELOAD=$JE timeout 900 "$ARMS/$arm" \
			--nupdaters "$cores" --nreaders 0 --cpustride 1 \
			--updatespacing "$(sp "$b")" --duration "$DUR" \
			--movesper 3 2>/dev/null)
		rc=$?
		if [ "$rc" -ne 0 ] || [[ "$out" == *"CONSERVATION FAILED"* ]]; then
			echo "!! $panel/$arm cores=$cores b=$b run=$r RC=$rc CONSERVATION/EXIT FAILURE" >&2
			fail=1
			echo "$panel,$arm,$cores,$b,$kps,$r,,,FAIL" >> "$CSV"
			continue
		fi
		mm=$(field "$out" "Mmoves/s:")
		ns=$(field "$out" "ns/key-move:")
		echo "$panel,$arm,$cores,$b,$kps,$r,${mm:-},${ns:-},ok" >> "$CSV"
		printf "  %-6s %-12s cores=%-4s n=%-7s run=%s  %9s Mmoves/s\n" \
			"$panel" "$arm" "$cores" "$kps" "$r" "$mm" >&2
	done
}

echo "panel,arm,cores,b,keys_per_sl,run,mmoves_s,ns_per_keymove,status" > "$CSV"

# --- conservation gate ------------------------------------------------------
# Every arm must pass a short correctness run BEFORE any timing is trusted.
# This is where trap 2 would surface if the RYW default were wrong.
echo ">> conservation gate (8 updaters, movesper 3, all four arms)" >&2
for a in old-sole old-helping pin-sole pin-serial; do
	out=$(setarch "$(uname -m)" -R env LD_PRELOAD=$JE timeout 300 "$ARMS/$a" --nupdaters 8 --nreaders 0 \
		--cpustride 1 --updatespacing "$(sp 480)" --duration 1000 \
		--movesper 3 2>/dev/null); rc=$?
	if [ "$rc" -ne 0 ] || [[ "$out" == *"CONSERVATION FAILED"* ]]; then
		echo "!! $a FAILED THE CONSERVATION GATE (rc=$rc) -- aborting" >&2; exit 1
	fi
	echo "   $a OK" >&2
done

# --- FIXED panel: the headline A/B + the drift check ------------------------
echo ">> FIXED: 3840 keys/skiplist at every core count, width 3" >&2
for c in 1 2 4 8 16 32 64 96 128 192; do
	b=$((3840 / c))
	for a in old-sole old-helping pin-sole; do run fixed "$a" "$c" "$b"; done
done

# --- SIZE panel: where the margin widens as structures shrink ---------------
echo ">> SIZE: 192 cores, sweep keys/skiplist, width 3" >&2
for b in 5 10 20 40 80; do
	for a in old-sole old-helping; do run size "$a" 192 "$b"; done
done

# --- LANE panel: optimistic vs the serialized fair-mutex lane, at the pin ----
echo ">> LANE: optimistic vs forced-escalation lane, at the pin" >&2
for c in 64 96 128 192; do
	b=$((3840 / c))
	for a in pin-sole pin-serial; do run lane "$a" "$c" "$b"; done
done

echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
[ "$fail" -eq 0 ] || echo ">> WARNING: at least one run FAILED; see status column" >&2
exit "$fail"
