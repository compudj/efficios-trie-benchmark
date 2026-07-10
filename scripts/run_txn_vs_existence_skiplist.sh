#!/bin/sh
# run_txn_vs_existence_skiplist.sh — scaling sweep for the urcu-txn vs. McKenney
# "existence" 3-SKIPLIST atomic-move comparison (the ORDERED dual of
# run_txn_vs_existence_scale.sh / design/txn-vs-existence-3hash.md).
#
# Two modes:
#   update : pure updaters (nreaders 0), x = nupdaters      -> aggregate Mmoves/s
#   read   : 1 background updater + N readers, x = nreaders -> aggregate Mqueries/s
#
# COMMIT WIDTH.  existence_3skiplist_uperf's skiplist_rotate() issues ONE
# existence_flip() per rotation covering 3*ceil(nobjects/3) key-moves -- with the
# default updatespacing 32 (nobjects 5) that is exactly 6 moves per atomic unit.
# (Upstream's loop bound is `i < nobjects` over arrays of 3*nobjects, so only 6 of
# the thread's 15 resident keys rotate; the other 9 are static residents.  Both
# engines therefore hold 15 keys/updater.)  So existence's atomic unit is a
# 6-move group, NOT a per-object flip.  We report three txn widths:
#   txn-mp1  --movesper 1  (narrowest unit; RYW off -- the fast path)
#   txn-mp6  --movesper 6  (WIDTH-MATCHED to existence's flip; RYW on)
#   txn-mp0  --movesper 0  (whole 15-key rotation in one commit; RYW on)
# RYW is forced explicitly so a width change never silently also flips the mode.
#
# Both engines' readers scan sl[0],sl[1],sl[2] sequentially, so NEITHER gives a
# multi-structure snapshot: both report hit% < 100.  The read-side difference is
# throughput (existence pays existence_exists() per hit), not the miss rate.
#
# Runs under one or more allocators (glibc, jemalloc via LD_PRELOAD).  Worker i
# pins to CPU i (distinct physical cores 0..191 on this 2x96-core EPYC).  Read
# mode pins the lone updater to core 0 and readers to cores 1..N, so x <= 191.
#
# Writes (or appends) scripts/txn_vs_existence_skiplist.csv:
#   alloc,mode,engine,run,x,mmoves_s,ns_per_keymove,mqueries_s,hitpct
#
# Env: DUR(ms) RUNS UPD_THREADS RD_THREADS ALLOCS MODES APPEND JE
set -u

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TXN="$REPO/bench_txn_3skiplist"
EX="$REPO/perfbook/datastruct/existence/existence_3skiplist_uperf"
CSV="$REPO/scripts/txn_vs_existence_skiplist.csv"

DUR=${DUR:-1000}
RUNS=${RUNS:-3}
UPD_THREADS=${UPD_THREADS:-"1 2 4 8 16 32 64 96 128 192"}
RD_THREADS=${RD_THREADS:-"1 2 4 8 16 32 64 96 128 191"}
ALLOCS=${ALLOCS:-"glibc jemalloc"}
MODES=${MODES:-"update read"}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
APPEND=${APPEND:-}

for b in "$TXN" "$EX"; do
	[ -x "$b" ] || { echo "ERROR: missing $b"; exit 1; }
done

field() { awk -v L="$2" '{for (i=1;i<=NF;i++) if ($i==L) {print $(i+1); exit}}' <<EOF
$1
EOF
}

[ -n "$APPEND" ] || echo "alloc,mode,engine,run,x,mmoves_s,ns_per_keymove,mqueries_s,hitpct" > "$CSV"

run_one() {  # alloc engine_label bin mode x extra_args...
	alloc=$1; elabel=$2; bin=$3; mode=$4; x=$5; shift 5
	pre=""; [ "$alloc" = "jemalloc" ] && pre="$JE"
	r=1
	while [ "$r" -le "$RUNS" ]; do
		out=$(LD_PRELOAD="$pre" timeout 120 "$bin" "$@" --duration "$DUR" 2>/dev/null)
		rc=$?
		if [ "$rc" -ne 0 ]; then
			echo "!! $elabel $mode x=$x run=$r FAILED rc=$rc" >&2
			echo "$alloc,$mode,$elabel,$r,$x,0,0,0,0" >> "$CSV"
			r=$((r + 1)); continue
		fi
		case "$out" in
		*"CONSERVATION FAILED"*)
			echo "!! $elabel $mode x=$x run=$r CONSERVATION FAILED" >&2 ;;
		esac
		mm=$(field "$out" "Mmoves/s:")
		nk=$(field "$out" "ns/key-move:")
		mq=$(field "$out" "Mqueries/s:")
		hp=$(field "$out" "hit%:")
		echo "$alloc,$mode,$elabel,$r,$x,${mm:-0},${nk:-0},${mq:-0},${hp:-0}" >> "$CSV"
		r=$((r + 1))
	done
}

for alloc in $ALLOCS; do
	for mode in $MODES; do
		if [ "$mode" = update ]; then TH="$UPD_THREADS"; else TH="$RD_THREADS"; fi
		for x in $TH; do
			echo ">> $alloc $mode x=$x" >&2
			if [ "$mode" = update ]; then
				U="$x"; R=0; XU="--nupdaters $x --nreaders 0"
			else
				U=1; R="$x"; XU="--nupdaters 1 --nreaders $x"
			fi
			run_one "$alloc" existence "$EX"  "$mode" "$x" $XU --cpustride 1
			run_one "$alloc" txn-mp1   "$TXN" "$mode" "$x" $XU --cpustride 1 --movesper 1 --ryw 0
			run_one "$alloc" txn-mp6   "$TXN" "$mode" "$x" $XU --cpustride 1 --movesper 6 --ryw 1
			run_one "$alloc" txn-mp0   "$TXN" "$mode" "$x" $XU --cpustride 1 --movesper 0 --ryw 1
		done
	done
done

echo ">> wrote $CSV" >&2
