#!/bin/sh
# run_txn_vs_existence_3hash_control.sh — the FIXED-SIZE control for the 3-hash
# urcu-txn vs. existence comparison (run_txn_vs_existence_scale.sh), and the
# hash-side counterpart of the skiplist sweep's `fixed` panel.
#
# WHY.  Two independent problems with the published 3-hash sweep.
#
# 1. FAIRNESS (a defect, now fixed).  existence_3hash_uperf's hash_rotate() had
#    the loop bound `i < nobjects` with a stride of 3 over hei[]/heo[] arrays of
#    3*nobjects entries, so it moved 6 of its 15 resident keys per rotation while
#    bench_txn_3hash rotated all 15.  Normalizing per key-move then amortized
#    existence's one existence_group alloc + flip + call_rcu over 6 moves against
#    txn's 15.  Identical defect to skiplist_rotate().  Fixing it makes existence
#    1.24-1.37x faster per key-move and INVERTS the published headline: at
#    nbuckets 4096, jemalloc, best-of-3, txn/existence goes
#
#      cores          1      8     32     96    192
#      published   0.74x  1.10x  1.36x  1.36x  1.06x   (existence moving 6 of 15)
#      corrected   0.60x  0.86x  0.99x  1.06x  0.78x   (existence moving 15 of 15)
#
#    "urcu-txn wins on the hash from 2 cores up" was existence doing 2.5x less
#    mutation work.  The fix also makes the published width (txn --movesper 0 =
#    the whole 15-key rotation) matched by construction, which it was not.
#
# 2. THE X-AXIS GROWS THE STRUCTURE.  Each updater owns 3*nobjects = 15 keys, so
#    keys/table = nobjects * cores: 5 at x=1, 960 at x=192, against a pinned
#    nbuckets.  The published curve is a growing problem, not a scaling curve --
#    the same confound that hid the skiplist's degeneracy for so long.  (There is
#    a second wrinkle: bucket = key % nbuckets with firstkey = id*updatespacing,
#    so at 4096 buckets updaters 128..191 alias onto buckets already owned by
#    updaters 0..64.)
#
# WHAT.  Hold keys/table at KEYS_FIXED for every core count (nobjects =
# KEYS_FIXED/cores), so the load factor is constant too, and match the commit
# width at 3 key-moves (--groupobjs 3 against --movesper 3; --groupobjs is the
# knob added alongside the fairness fix).  Perfect scaling is then FLAT
# ns/key-move, and any rise is writer interference alone.
#
# RESULT (KEYS_FIXED=960, nbuckets 4096, load 0.23, jemalloc, best-of-3):
# existence and urcu-txn are within ~20% of each other at every core count
# (ex/txn 0.97x .. 1.22x, no trend), and txn's 1->192 penalty is 3.0x against
# existence's 3.3x.  Contrast the ORDERED structure, where at fixed size txn is
# a flat 2.2-2.3x behind from 1 core to 192.  That is the edges-per-mutation
# thesis: an hlist key-move transacts 3-5 pointers, a skiplist key-move 8.7 and
# rising as O(log n), and MCAS pays per transacted edge.
#
# Neither engine's commit is a transaction in the strict sense: txn makes its
# write set visible in one step and validates only the slots it stores or
# explicitly folds in; existence flips a group word.  Both are atomic multi-slot
# updates, not serializable transactions.
#
# Writes scripts/txn_vs_existence_3hash_control.csv:
#   panel,engine,run,cores,nobjects,keys_per_table,width,mmoves_s,ns_per_keymove
#
# Env: DUR(ms) RUNS KEYS_FIXED NBUCKETS JE
set -u

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TXN="$REPO/bench_txn_3hash"
EX="$REPO/perfbook/datastruct/existence/existence_3hash_uperf"
CSV="$REPO/scripts/txn_vs_existence_3hash_control.csv"

DUR=${DUR:-1000}
RUNS=${RUNS:-3}
KEYS_FIXED=${KEYS_FIXED:-960}	# divisible by every core count below
NBUCKETS=${NBUCKETS:-4096}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}

CORES="1 2 4 8 16 32 64 96 192"

make -C "$REPO" bench_txn_3hash >/dev/null || exit 1
make -C "$REPO/perfbook/datastruct/existence" existence_3hash_uperf >/dev/null || exit 1
for b in "$TXN" "$EX"; do
	[ -x "$b" ] || { echo "ERROR: missing $b"; exit 1; }
done

field() { awk -v L="$2" '{for (i=1;i<=NF;i++) if ($i==L) {print $(i+1); exit}}' <<EOF
$1
EOF
}

# run_one panel engine cores nobjects width binary extra...
run_one() {
	panel=$1; engine=$2; cores=$3; nobj=$4; width=$5; bin=$6
	shift 6
	r=1
	while [ "$r" -le "$RUNS" ]; do
		out=$(LD_PRELOAD="$JE" timeout 600 "$bin" --nupdaters "$cores" --nreaders 0 \
			--cpustride 1 --nbuckets "$NBUCKETS" \
			--updatespacing $((16 + 3 * nobj)) --duration "$DUR" "$@" 2>/dev/null)
		if [ $? -ne 0 ]; then
			echo "!! $panel/$engine cores=$cores FAILED" >&2
			r=$((r + 1)); continue
		fi
		case "$out" in
		*"CONSERVATION FAILED"*)
			echo "!! $panel/$engine cores=$cores CONSERVATION FAILED" >&2 ;;
		esac
		echo "$panel,$engine,$r,$cores,$nobj,$((nobj * cores)),$width,$(field "$out" "Mmoves/s:"),$(field "$out" "ns/key-move:")" >> "$CSV"
		r=$((r + 1))
	done
}

echo "panel,engine,run,cores,nobjects,keys_per_table,width,mmoves_s,ns_per_keymove" > "$CSV"

echo ">> panel 'fixed': keys/table = $KEYS_FIXED everywhere, width matched at 3" >&2
for c in $CORES; do
	nobj=$((KEYS_FIXED / c))
	[ $((nobj * c)) -eq "$KEYS_FIXED" ] || { echo "   !! $c does not divide $KEYS_FIXED" >&2; continue; }
	echo "   cores=$c nobjects=$nobj" >&2
	run_one fixed existence "$c" "$nobj" 3 "$EX"  --groupobjs 3
	run_one fixed txn-mp3   "$c" "$nobj" 3 "$TXN" --movesper 3
done

echo ">> panel 'grow': the published config (nobjects 5, keys/table = 5*cores)" >&2
for c in $CORES; do
	echo "   cores=$c keys/table=$((5 * c))" >&2
	run_one grow existence "$c" 5 15 "$EX"		# groupobjs 0 => whole rotation
	run_one grow txn-mp0   "$c" 5 15 "$TXN" --movesper 0
done

echo ">> wrote $CSV ($(($(wc -l < "$CSV") - 1)) rows)" >&2
