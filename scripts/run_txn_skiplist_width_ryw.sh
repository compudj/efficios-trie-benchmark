#!/bin/sh
# run_txn_skiplist_width_ryw.sh — commit-width sweep on bench_txn_3skiplist.
#
#   width : commit-width curve.  x = --movesper (1,2,3,4,6,8,15).  Read-your-own-
#           writes is now the engine's unconditional default, so width is the only
#           variable with no flag to force.  15 == the whole rotation
#           (K = 3*nobjects = 15 at updatespacing 32), passed as --movesper 0.
#           Existence's atomic unit is 6 -> that column is the width-matched one.
#
# The pure updaters run with nreaders 0; worker i pins to CPU i.
#
# HISTORICAL: this script also carried a "ryw" sweep -- the fast-path A/B of
# read-your-own-writes ON vs OFF (--movesper 1, --ryw 0 vs 1; measured 10.0% at 1
# updater, ~0% at >= 32).  The invisible-writes (ryw-off) mode is retired, so that
# A/B can no longer be produced and the sweep is gone.  The CSV keeps its `ryw`
# column (now always 1, the default) for schema stability.
#
# NOTE: the width curve must be run against an engine carrying urcu-txn-dev
# c7d86222.  Before it, a 15-move commit's record count (mean 72, max 133)
# occasionally crossed URCU_TXN_BIG=128, and because every lane holder re-asserted
# domain->active a single such commit captured the whole domain in the serialized
# fallback lane -- the mp15 column then measured that funnel, not commit width.
#
# Writes scripts/txn_skiplist_width_ryw.csv:
#   sweep,variant,alloc,run,updaters,movesper,ryw,mmoves_s,ns_per_keymove,commits,aborts
#
# Env: DUR(ms) RUNS ALLOCS WIDTH_UPDATERS RYW_UPDATERS WIDTHS SWEEPS APPEND JE
#      TXN=<binary> VARIANT=<label>   (to A/B a rebuilt bench, e.g. URCU_TXN_BIG)
set -u

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TXN=${TXN:-"$REPO/bench_txn_3skiplist"}
CSV="$REPO/scripts/txn_skiplist_width_ryw.csv"

# Label for the binary under test, so two builds can share one CSV.  Escalation
# thresholds (URCU_TXN_BIG / URCU_TXN_FALLBACK) live ONLY inside static-inline
# functions in <urcu/rcu-txn.h>, so rebuilding the BENCH alone -- no liburcu
# rebuild -- genuinely changes them.
VARIANT=${VARIANT:-big128}

DUR=${DUR:-1000}
RUNS=${RUNS:-3}
ALLOCS=${ALLOCS:-"jemalloc"}
WIDTHS=${WIDTHS:-"1 2 3 4 6 8 15"}
WIDTH_UPDATERS=${WIDTH_UPDATERS:-"1 4 16 64"}
SWEEPS=${SWEEPS:-"width"}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
APPEND=${APPEND:-}

[ -x "$TXN" ] || { echo "ERROR: missing $TXN"; exit 1; }

field() { awk -v L="$2" '{for (i=1;i<=NF;i++) if ($i==L) {print $(i+1); exit}}' <<EOF
$1
EOF
}

[ -n "$APPEND" ] || echo "sweep,variant,alloc,run,updaters,movesper,ryw,mmoves_s,ns_per_keymove,commits,aborts" > "$CSV"

# run_one sweep alloc updaters movesper ryw
run_one() {
	sw=$1; alloc=$2; u=$3; mp=$4; ryw=$5
	pre=""; [ "$alloc" = "jemalloc" ] && pre="$JE"
	# --movesper 0 means "whole rotation"; label it by its true width, 15.
	arg_mp=$mp; [ "$mp" = 15 ] && arg_mp=0
	r=1
	while [ "$r" -le "$RUNS" ]; do
		out=$(LD_PRELOAD="$pre" timeout 120 "$TXN" --nupdaters "$u" --nreaders 0 \
			--cpustride 1 --movesper "$arg_mp" --duration "$DUR" 2>/dev/null)
		rc=$?
		if [ "$rc" -ne 0 ]; then
			echo "!! $sw u=$u mp=$mp ryw=$ryw run=$r FAILED rc=$rc" >&2
			echo "$sw,$VARIANT,$alloc,$r,$u,$mp,$ryw,0,0,0,0" >> "$CSV"
			r=$((r + 1)); continue
		fi
		case "$out" in
		*"CONSERVATION FAILED"*) echo "!! $sw u=$u mp=$mp CONSERVATION FAILED" >&2 ;;
		esac
		mm=$(field "$out" "Mmoves/s:")
		nk=$(field "$out" "ns/key-move:")
		# "(commits: N  aborts: M)" -- strip the parenthesis off the last field.
		co=$(field "$out" "(commits:" | tr -cd '0-9')
		ab=$(field "$out" "aborts:" | tr -cd '0-9')
		echo "$sw,$VARIANT,$alloc,$r,$u,$mp,$ryw,${mm:-0},${nk:-0},${co:-0},${ab:-0}" >> "$CSV"
		r=$((r + 1))
	done
}

for alloc in $ALLOCS; do
	for sw in $SWEEPS; do
		case "$sw" in
		width)
			for u in $WIDTH_UPDATERS; do
				for mp in $WIDTHS; do
					echo ">> $alloc width u=$u movesper=$mp" >&2
					run_one width "$alloc" "$u" "$mp" 1
				done
			done ;;
		esac
	done
done

echo ">> wrote $CSV" >&2
