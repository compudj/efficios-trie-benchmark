#!/bin/sh
# run_txn_vs_existence_skiplist.sh — urcu-txn (rcu-mcas) vs McKenney "existence"
# on the ORDERED axis: three shared skiplists, atomic cross-skiplist key move.
# The ordered dual of run_txn_vs_existence_scale.sh (3-hash).
#
# ⚠ THIS SUPERSEDES THE FIRST VERSION OF THIS SWEEP, WHOSE DATA WAS INVALID.
# Three defects, all fixed in the perfbook commits this script depends on:
#
#   1. random_level() drew from an UNSEEDED __thread Park-Miller state.  Zero
#      is a fixed point of Schrage's step, so random() returned 2^31-1 forever
#      and every tower came out full height: existence's "skiplist" was
#      SL_MAX_LEVELS parallel sorted lists with O(n) search.  Verified in the
#      running code: 960/960 nodes at level 7.  The old figure's "existence is
#      flat, txn crosses over at 128 cores" was entirely this artifact.
#   2. skiplist_rotate() moved only 3*ceil(nobjects/3) of its 3*nobjects
#      resident keys -- 6 of 15 -- while bench_txn_3skiplist moved all 15.
#      Per-key-move normalization then handed existence a 2.5x smaller
#      mutation footprint.
#   3. The engines committed at DIFFERENT atomic widths, existence's tied to
#      nobjects.  --groupobjs decouples it so it can be matched to --movesper.
#
# And one confound that was not a bug but wrecked the interpretation: each
# updater owns b keys per skiplist, so keys/skiplist = b * cores.  The old
# x-axis grew the STRUCTURE 192-fold while it grew the parallelism.  Every
# "scaling" curve there was really a curve of a growing problem.
#
# So this sweep measures four things instead of one:
#
#   grow   keys/skiplist = 5*cores.  What the bench naturally does, and what
#          the old figure showed.  Kept, and now LABELLED as a growing problem
#          rather than presented as scaling.
#   fixed  keys/skiplist held at KEYS_FIXED for every core count (b =
#          KEYS_FIXED/cores).  The honest scaling curve: perfect scaling is
#          FLAT ns/key-move; any rise is writer interference and nothing else.
#   size   cores pinned at 192, sweep b.  The headline correction -- the
#          existence/txn gap is a strong function of structure size (3.98x at
#          960 keys/skiplist, 2.02x at 7680).  With 192 writers on 960 keys a
#          level-7 predecessor slot is written by ~26 distinct cores; the old
#          single point sat at the worst case for an optimistic engine.
#   read   N readers + 1 updater.  existence taxes every lookup with an
#          eh_egi check; txn's plain RCU readers do not.
#
# COMMIT WIDTH is matched at 3 key-moves -- one turn of the rotation's 3-cycle,
# existence's NARROWEST atomic unit.  txn-mp1 (one key-move per commit) has no
# existence peer and is reported as the narrowest txn unit.  Note that neither
# engine's commit is a transaction in the strict sense: txn makes its write set
# visible in one step and validates only the slots it stores or explicitly
# folds in, and existence flips a group word; both are atomic multi-slot
# updates, not serializable transactions.
#
# Both binaries come from `make`, never from a bare path: the existence side
# MUST carry -DSL_XORSHIFT_LEVEL so both skiplists draw tower heights from one
# generator, and perfbook's own Makefile does not pass it.  The repo Makefile
# target forces the rebuild and verifies the flag took.
#
# ALLOCATOR: jemalloc for both engines, with glibc kept as a control on the
# `fixed` panel only.  The allocator is not what separates these engines and
# running every panel twice doubles a sweep that already takes ~15 minutes.
#
# Neither engine's reader gives a multi-structure snapshot -- both scan
# sl[0..2] sequentially -- so both report hit% < 100.  The read-side difference
# is throughput, not the miss rate.
#
# Writes scripts/txn_vs_existence_skiplist.csv:
#   panel,alloc,engine,run,cores,nreaders,b,keys_per_sl,width,mmoves_s,ns_per_keymove,mqueries_s,hitpct
#
# Env: DUR(ms) RUNS KEYS_FIXED READ_B JE
set -u

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TXN="$REPO/bench_txn_3skiplist"
EX="$REPO/perfbook/datastruct/existence/existence_3skiplist_uperf"
CSV="$REPO/scripts/txn_vs_existence_skiplist.csv"

DUR=${DUR:-1000}
RUNS=${RUNS:-3}
KEYS_FIXED=${KEYS_FIXED:-3840}	# divisible by every core count below
READ_B=${READ_B:-3840}		# 1 updater => keys/skiplist = READ_B
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}

CORES="1 2 4 8 16 32 64 96 128 192"
SIZES="5 10 20 40 80"		# b = keys per updater per skiplist
READERS="1 2 4 8 16 32 64 96 191"

echo ">> building both engines (make enforces -DSL_XORSHIFT_LEVEL on existence)" >&2
make -C "$REPO" bench_txn_3skiplist existence_3skiplist_uperf >/dev/null || exit 1
for b in "$TXN" "$EX"; do
	[ -x "$b" ] || { echo "ERROR: missing $b"; exit 1; }
done
nm "$EX" | grep -q sl_rng_state || {
	echo "ERROR: $EX lacks -DSL_XORSHIFT_LEVEL"; exit 1; }

field() { awk -v L="$2" '{for (i=1;i<=NF;i++) if ($i==L) {print $(i+1); exit}}' <<EOF
$1
EOF
}

spacing() { echo $((16 + 3 * $1)); }	# nobjects = (spacing-16)/3, in both benches

# run_one panel alloc engine cores b nreaders width binary extra...
run_one() {
	panel=$1; alloc=$2; engine=$3; cores=$4; b=$5; nrd=$6; width=$7; bin=$8
	shift 8
	pre=""; [ "$alloc" = jemalloc ] && pre="$JE"
	kps=$((b * cores))
	r=1
	while [ "$r" -le "$RUNS" ]; do
		out=$(LD_PRELOAD="$pre" timeout 600 "$bin" --nupdaters "$cores" \
			--nreaders "$nrd" --cpustride 1 \
			--updatespacing "$(spacing "$b")" --duration "$DUR" "$@" 2>/dev/null)
		if [ $? -ne 0 ]; then
			echo "!! $panel/$engine cores=$cores b=$b run=$r FAILED" >&2
			echo "$panel,$alloc,$engine,$r,$cores,$nrd,$b,$kps,$width,0,0,0,0" >> "$CSV"
			r=$((r + 1)); continue
		fi
		case "$out" in
		*"CONSERVATION FAILED"*)
			echo "!! $panel/$engine cores=$cores b=$b run=$r CONSERVATION FAILED" >&2 ;;
		esac
		mm=$(field "$out" "Mmoves/s:");    nk=$(field "$out" "ns/key-move:")
		mq=$(field "$out" "Mqueries/s:");  hp=$(field "$out" "hit%:")
		echo "$panel,$alloc,$engine,$r,$cores,$nrd,$b,$kps,$width,${mm:-0},${nk:-0},${mq:-0},${hp:-0}" >> "$CSV"
		r=$((r + 1))
	done
}

# existence + txn at matched width 3, plus txn-mp1 (narrowest txn unit, no peer)
trio() {
	panel=$1; alloc=$2; cores=$3; b=$4; nrd=$5
	run_one "$panel" "$alloc" existence "$cores" "$b" "$nrd" 3 "$EX"  --groupobjs 3
	run_one "$panel" "$alloc" txn-mp3   "$cores" "$b" "$nrd" 3 "$TXN" --movesper 3
	run_one "$panel" "$alloc" txn-mp1   "$cores" "$b" "$nrd" 1 "$TXN" --movesper 1
}

echo "panel,alloc,engine,run,cores,nreaders,b,keys_per_sl,width,mmoves_s,ns_per_keymove,mqueries_s,hitpct" > "$CSV"

echo ">> panel 'grow': keys/skiplist = 5*cores (the historical config)" >&2
for c in $CORES; do
	echo "   cores=$c keys/sl=$((5 * c))" >&2
	trio grow jemalloc "$c" 5 0
done

echo ">> panel 'fixed': keys/skiplist = $KEYS_FIXED at every core count" >&2
for c in $CORES; do
	b=$((KEYS_FIXED / c))
	[ $((b * c)) -eq "$KEYS_FIXED" ] || { echo "   !! $c does not divide $KEYS_FIXED" >&2; continue; }
	echo "   cores=$c b=$b keys/sl=$KEYS_FIXED" >&2
	trio fixed jemalloc "$c" "$b" 0
	trio fixed glibc    "$c" "$b" 0
done

echo ">> panel 'size': 192 cores, sweep keys/skiplist" >&2
for b in $SIZES; do
	echo "   b=$b keys/sl=$((b * 192))" >&2
	trio size jemalloc 192 "$b" 0
done

echo ">> panel 'read': 1 updater + N readers, keys/skiplist = $READ_B" >&2
for n in $READERS; do
	echo "   readers=$n" >&2
	run_one read jemalloc existence 1 "$READ_B" "$n" 3 "$EX"  --groupobjs 3
	run_one read jemalloc txn-mp3   1 "$READ_B" "$n" 3 "$TXN" --movesper 3
done

echo ">> wrote $CSV ($(($(wc -l < "$CSV") - 1)) rows)" >&2
