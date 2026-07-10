#!/bin/sh
# run_txn_vs_existence_scale.sh — urcu-txn (rcu-mcas) vs McKenney "existence" on
# the UNORDERED axis: three chained hash tables, atomic cross-table key move.
# The hash dual of run_txn_vs_existence_skiplist.sh.
#
# ⚠ THIS SUPERSEDES THE FIRST VERSION OF THIS SWEEP, WHOSE DATA WAS UNFAIR.
#
# existence_3hash_uperf's hash_rotate() allocated hei[]/heo[] with 3*nobjects
# entries but looped `i < nobjects` with a stride of 3, so it moved 6 of its 15
# resident keys per rotation while bench_txn_3hash rotated all 15.  Normalizing
# per key-move then amortized existence's one existence_group alloc + flip +
# call_rcu over 6 moves against txn's 15, and left the engines at different
# atomic widths (the sweep runs txn --movesper 0, the whole 15-key rotation).
# Fixing it makes existence 1.24-1.37x faster and INVERTS the old headline:
#
#   cores          1      8     32     96    192
#   published   0.74x  1.10x  1.36x  1.36x  1.06x   txn/existence
#   corrected   0.60x  0.86x  0.99x  1.06x  0.78x
#
# "urcu-txn wins on the hash from 2 cores up" was existence doing 2.5x less
# mutation work.  Same defect as skiplist_rotate(); see that sweep's header.
#
# And the confound both sweeps shared: each updater owns 3*nobjects keys, so
# keys/table = nobjects * cores.  The old x-axis grew the STRUCTURE 192-fold
# while it grew the parallelism.  (Second wrinkle: bucket = key % nbuckets with
# firstkey = id*updatespacing, so at 4096 buckets updaters 128..191 alias onto
# buckets already owned by updaters 0..64.)
#
# So this sweep measures four things instead of two:
#
#   grow    keys/table = 5*cores, nbuckets 4096.  The published configuration,
#           now width-matched by construction (existence's whole-rotation flip
#           IS 15 key-moves after the fix, as is txn --movesper 0).  Kept, and
#           labelled as a growing problem rather than presented as scaling.
#   fixed   keys/table held at KEYS_FIXED for every core count, so the load
#           factor is constant too, and the commit width matched at 3 key-moves
#           (--groupobjs 3 against --movesper 3).  Perfect scaling is then FLAT
#           ns/key-move.  glibc drawn as an allocator control.
#   size    192 cores, sweep keys/table with nbuckets scaled to hold the load
#           factor at 1/LOAD_DIV -- so this isolates structure SIZE from chain
#           length, and incidentally removes the bucket aliasing above.
#   read    N readers + 1 updater.
#
# RESULT.  At equal structure and equal width the two engines are within ~20% of
# each other at every core count, with no trend, and urcu-txn's 1->192 contention
# penalty is slightly SMALLER than existence's.  Contrast the ORDERED structure,
# where the same control puts txn a flat 2.2-2.3x behind from 1 core to 192.
# That is the edges-per-mutation result: an hlist key-move transacts 3-5 pointers
# and the count does not grow with n; a skiplist key-move transacts 8.70, growing
# as O(log n).  MCAS coordinates per transacted edge, at four atomic RMWs each;
# existence coordinates per node and writes its pointers with plain stores
# outside the atomic step, because it has an invisibility state to build into.
#
# Neither engine's commit is a transaction in the strict sense: txn makes its
# write set visible in one step and validates only the slots it stores or
# explicitly folds into the read set; existence flips a group word.  Both are
# atomic multi-slot updates, not serializable transactions.
#
# Subsumes the standalone run_txn_vs_existence_3hash_control.sh: its `fixed` and
# `grow` panels are two of the four here.
#
# Writes scripts/txn_vs_existence_scale.csv:
#   panel,alloc,engine,run,cores,nreaders,nobjects,keys_per_table,nbuckets,width,mmoves_s,ns_per_keymove,mqueries_s
#
# Env: DUR(ms) RUNS KEYS_FIXED NBUCKETS LOAD_DIV JE
set -u

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TXN="$REPO/bench_txn_3hash"
EX="$REPO/perfbook/datastruct/existence/existence_3hash_uperf"
CSV="$REPO/scripts/txn_vs_existence_scale.csv"

DUR=${DUR:-1000}
RUNS=${RUNS:-3}
KEYS_FIXED=${KEYS_FIXED:-960}	# divisible by every core count below
NBUCKETS=${NBUCKETS:-4096}	# grow/fixed/read: the published bucket count
LOAD_DIV=${LOAD_DIV:-4}		# size panel: nbuckets = LOAD_DIV * keys/table
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}

CORES="1 2 4 8 16 32 64 96 192"
SIZES="5 10 20 40 80"		# nobjects at 192 cores => keys/table = 192*nobjects
READERS="1 2 4 8 16 32 64 96 191"

echo ">> building both engines" >&2
make -C "$REPO" bench_txn_3hash >/dev/null || exit 1
make -C "$REPO/perfbook/datastruct/existence" existence_3hash_uperf >/dev/null || exit 1
for b in "$TXN" "$EX"; do
	[ -x "$b" ] || { echo "ERROR: missing $b"; exit 1; }
done

field() { awk -v L="$2" '{for (i=1;i<=NF;i++) if ($i==L) {print $(i+1); exit}}' <<EOF
$1
EOF
}

# run_one panel alloc engine cores nreaders nobjects nbuckets width binary extra...
run_one() {
	panel=$1; alloc=$2; engine=$3; cores=$4; nrd=$5; nobj=$6; nb=$7; width=$8; bin=$9
	shift 9
	pre=""; [ "$alloc" = jemalloc ] && pre="$JE"
	kpt=$((nobj * cores))
	r=1
	while [ "$r" -le "$RUNS" ]; do
		out=$(LD_PRELOAD="$pre" timeout 600 "$bin" --nupdaters "$cores" \
			--nreaders "$nrd" --cpustride 1 --nbuckets "$nb" \
			--updatespacing $((16 + 3 * nobj)) --duration "$DUR" "$@" 2>/dev/null)
		if [ $? -ne 0 ]; then
			echo "!! $panel/$engine cores=$cores nobj=$nobj FAILED" >&2
			echo "$panel,$alloc,$engine,$r,$cores,$nrd,$nobj,$kpt,$nb,$width,0,0,0" >> "$CSV"
			r=$((r + 1)); continue
		fi
		case "$out" in
		*"CONSERVATION FAILED"*)
			echo "!! $panel/$engine cores=$cores nobj=$nobj CONSERVATION FAILED" >&2 ;;
		esac
		mm=$(field "$out" "Mmoves/s:");   nk=$(field "$out" "ns/key-move:")
		mq=$(field "$out" "Mqueries/s:")
		echo "$panel,$alloc,$engine,$r,$cores,$nrd,$nobj,$kpt,$nb,$width,${mm:-0},${nk:-0},${mq:-0}" >> "$CSV"
		r=$((r + 1))
	done
}

echo "panel,alloc,engine,run,cores,nreaders,nobjects,keys_per_table,nbuckets,width,mmoves_s,ns_per_keymove,mqueries_s" > "$CSV"

echo ">> panel 'grow': keys/table = 5*cores, nbuckets $NBUCKETS (the published config)" >&2
for c in $CORES; do
	echo "   cores=$c keys/table=$((5 * c))" >&2
	run_one grow jemalloc existence "$c" 0 5 "$NBUCKETS" 15 "$EX"
	run_one grow jemalloc txn       "$c" 0 5 "$NBUCKETS" 15 "$TXN" --movesper 0
done

echo ">> panel 'fixed': keys/table = $KEYS_FIXED everywhere, width matched at 3" >&2
for c in $CORES; do
	nobj=$((KEYS_FIXED / c))
	[ $((nobj * c)) -eq "$KEYS_FIXED" ] || { echo "   !! $c does not divide $KEYS_FIXED" >&2; continue; }
	echo "   cores=$c nobjects=$nobj" >&2
	for a in jemalloc glibc; do
		run_one fixed "$a" existence "$c" 0 "$nobj" "$NBUCKETS" 3 "$EX"  --groupobjs 3
		run_one fixed "$a" txn       "$c" 0 "$nobj" "$NBUCKETS" 3 "$TXN" --movesper 3
	done
done

echo ">> panel 'size': 192 cores, nbuckets scaled to hold load factor at 1/$LOAD_DIV" >&2
for nobj in $SIZES; do
	kpt=$((nobj * 192)); nb=$((LOAD_DIV * kpt))
	echo "   nobjects=$nobj keys/table=$kpt nbuckets=$nb" >&2
	run_one size jemalloc existence 192 0 "$nobj" "$nb" 3 "$EX"  --groupobjs 3
	run_one size jemalloc txn       192 0 "$nobj" "$nb" 3 "$TXN" --movesper 3
done

echo ">> panel 'read': 1 updater + N readers, keys/table = $KEYS_FIXED" >&2
for n in $READERS; do
	echo "   readers=$n" >&2
	run_one read jemalloc existence 1 "$n" "$KEYS_FIXED" "$NBUCKETS" 3 "$EX"  --groupobjs 3
	run_one read jemalloc txn       1 "$n" "$KEYS_FIXED" "$NBUCKETS" 3 "$TXN" --movesper 3
done

echo ">> wrote $CSV ($(($(wc -l < "$CSV") - 1)) rows)" >&2
