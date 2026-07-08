#!/bin/sh
# run_txn_vs_existence_scale.sh — scaling sweep for the urcu-txn vs. McKenney
# "existence" 3-hash atomic-move comparison (design/txn-vs-existence-3hash.md).
#
# Two modes, both engines (txn = whole-rotation MCAS; existence = existence_flip):
#   update : pure updaters (nreaders 0), x = nupdaters      -> aggregate Mmoves/s
#   read   : 1 background updater + N readers, x = nreaders -> aggregate Mqueries/s
#
# Runs under one or more allocators (glibc, jemalloc via LD_PRELOAD): the update
# side is allocator-bound (existence allocates a group + nodes per rotation), the
# read side is not (readers do not allocate).  Worker i pins to CPU i (distinct
# physical cores 0..191 on this 2x96-core EPYC).  Read mode pins the lone updater
# to core 0 and readers to cores 1..N, so x <= 191.
#
# Best-of-N (max) tames the run-to-run variance from the per-updater RT call_rcu
# workers oversubscribing the cores at high thread counts.
#
# Writes (or appends) scripts/txn_vs_existence_scale.csv:
#   alloc,mode,engine,run,x,mmoves_s,mqueries_s
#
# Env: DUR(ms) RUNS NBUCKETS UPD_THREADS RD_THREADS
#      ALLOCS="glibc jemalloc"  MODES="update read"  APPEND=1  JE=<libjemalloc.so>
set -u

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TXN="$REPO/bench_txn_3hash"
EX="$REPO/perfbook/datastruct/existence/existence_3hash_uperf"
CSV="$REPO/scripts/txn_vs_existence_scale.csv"

DUR=${DUR:-1000}
RUNS=${RUNS:-5}
NBUCKETS=${NBUCKETS:-4096}
UPD_THREADS=${UPD_THREADS:-"1 2 4 8 16 32 64 96 128 192"}
RD_THREADS=${RD_THREADS:-"1 2 4 8 16 32 64 96 128 191"}
ALLOCS=${ALLOCS:-"glibc jemalloc"}
MODES=${MODES:-"update read"}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
APPEND=${APPEND:-}

for b in "$TXN" "$EX"; do
	[ -x "$b" ] || { echo "ERROR: missing $b (make bench_txn_3hash / make -C perfbook/datastruct/existence)"; exit 1; }
done

field() { awk -v L="$2" '{for (i=1;i<=NF;i++) if ($i==L) {print $(i+1); exit}}' <<EOF
$1
EOF
}

[ -n "$APPEND" ] || echo "alloc,mode,engine,run,x,mmoves_s,mqueries_s" > "$CSV"

run_one() {  # alloc engine_label bin mode x extra_args...
	alloc=$1; elabel=$2; bin=$3; mode=$4; x=$5; shift 5
	pre=""; [ "$alloc" = "jemalloc" ] && pre="$JE"
	r=1
	while [ "$r" -le "$RUNS" ]; do
		out=$(LD_PRELOAD="$pre" "$bin" "$@" --nbuckets "$NBUCKETS" --duration "$DUR" 2>/dev/null)
		mm=$(field "$out" "Mmoves/s:"); mq=$(field "$out" "Mqueries/s:")
		echo "$alloc,$mode,$elabel,$r,$x,${mm:-0},${mq:-0}" >> "$CSV"
		r=$((r + 1))
	done
}

for alloc in $ALLOCS; do
	for mode in $MODES; do
		if [ "$mode" = update ]; then TH="$UPD_THREADS"; else TH="$RD_THREADS"; fi
		for x in $TH; do
			echo ">> $alloc $mode x=$x" >&2
			if [ "$mode" = update ]; then
				run_one "$alloc" txn       "$TXN" update "$x" --nupdaters "$x" --nreaders 0 --movesper 0 --cpustride 1
				run_one "$alloc" existence "$EX"  update "$x" --nupdaters "$x" --nreaders 0 --cpustride 1
			else
				run_one "$alloc" txn       "$TXN" read "$x" --nupdaters 1 --nreaders "$x" --movesper 0 --cpustride 1
				run_one "$alloc" existence "$EX"  read "$x" --nupdaters 1 --nreaders "$x" --cpustride 1
			fi
		done
	done
done

echo ">> wrote $CSV" >&2
