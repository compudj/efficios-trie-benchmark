#!/bin/sh
# Positive half of the age-0/age-1 optimistic RYW escalation study: on the
# DISJOINT 3-hash workload, age-0 (no-sort + try-latch install) can beat the
# baseline RYW path -- the opposite of the adjacency-forced 3-skiplist.  Emits
# scripts/age_escalate_hash.csv (plotted by scripts/plot_age_escalate_hash.py).
#
# All binaries use the current engine -- the spinlatch install (-DURCU_MCAS_NO_HELP
# -DURCU_MCAS_NO_STEAL), decide/settle CAS dropped to a release store -- built
# against the in-tree engine clone (urcu-txn-build, synced by `make urcu-txn`):
#   baseline  = spinlatch + default RYW path (Bloom + find every in-bracket load)
#   esc-dumb  = spinlatch + -DURCU_TXN_AGE_ESCALATE -DURCU_MCAS_AGE0_TRYLATCH (64-bit k=1)
#   esc-smart = the same + -DURCU_TXN_BLOOM_K=3 -DURCU_TXN_BLOOM_WORDS=16 (1024-bit)
#
# AGE_ESCALATE now also appends the age-0 store blind, skipping the reconcile
# find (the O(nr) write-set scan) -- redundant at age 0 because esc_pending
# already discards any same-slot coincidence before install.  So esc-dumb and
# esc-smart get it for free; it lifts the whole positive half +4-11% (largest
# at low contention, where the find scan was the dominant age-0 self-time).
#
# And under the spinlatch + AGE0_TRYLATCH the age-0 INSTALL is now FLAT
# (auto-derived, no extra flag): one try-CAS per record, OR the outcomes, bail at
# the first conflict, decide arithmetically -- no install latch, no self-settle,
# no pre-CAS slot load, and no MISPREDICTED branch (the latched install's per-
# record is-proxy/slot-still-old tests mispredict exactly when a slot is contended;
# the flat install's only branch is a well-predicted after-CAS bail).  esc-dumb and
# esc-smart get this for free too; it adds +2-10% over the latched age-0 install.
#
# Workload: --nbuckets 16384 --updatespacing 512 --movesper 8, RYW FORCED ON
# (--ryw 1).  8 consecutive keys hit 8 distinct buckets (chunk < nbuckets), so the
# write-set is disjoint and genuine RYW is ~0 -- every escalation is a filter false
# positive, which the k=3 filter drives toward zero while the k=1 filter (99% FP on
# contiguous bucket-head addresses) trips on almost everything.  Best-of-$BEST @
# $DURATION ms, jemalloc.  Env overrides: CORES, DURATION, BEST, NBUCKETS, JE, OUT.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
INC="$REPO/urcu-txn-build/include"
LIBDIR="$REPO/urcu-txn-build/src/.libs"
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
OUT=${OUT:-$HERE/age_escalate_hash.csv}
DURATION=${DURATION:-800}
BEST=${BEST:-3}
NBUCKETS=${NBUCKETS:-16384}
SPACING=${SPACING:-512}
MOVESPER=${MOVESPER:-8}
CORES=${CORES:-"1 4 16 32 64 128 192"}

test -f "$LIBDIR/liburcu-qsbr.so" || { echo "engine clone not built -- run 'make urcu-txn'" >&2; exit 1; }
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

SPIN="-DURCU_MCAS_NO_HELP -DURCU_MCAS_NO_STEAL"
build() {	# tag  extra-cflags
	cc -O2 -pthread -Wall $2 -I"$INC" -c "$REPO/src/bench_txn_3hash.c" -o "$BUILD/$1.o"
	cc -O2 -pthread -o "$BUILD/$1" "$BUILD/$1.o" \
		-L"$LIBDIR" -Wl,-rpath,"$LIBDIR" -lurcu-qsbr -lurcu-cds -lurcu-common -lpthread
}
build baseline  "$SPIN"
build esc-dumb  "$SPIN -DURCU_TXN_AGE_ESCALATE -DURCU_MCAS_AGE0_TRYLATCH"
build esc-smart "$SPIN -DURCU_TXN_AGE_ESCALATE -DURCU_MCAS_AGE0_TRYLATCH -DURCU_TXN_BLOOM_K=3 -DURCU_TXN_BLOOM_WORDS=16"

echo "engine,cores,movesper,spacing,nbuckets,mmoves_s,commits,aborts" > "$OUT"

run_point() {	# engine cores
	best=0; bc=0; ba=0
	i=0
	while [ "$i" -lt "$BEST" ]; do
		i=$((i + 1))
		line=$(LD_PRELOAD="$JE" "$BUILD/$1" --nupdaters "$2" --nreaders 0 \
			--cpustride 1 --nbuckets "$NBUCKETS" --updatespacing "$SPACING" \
			--movesper "$MOVESPER" --ryw 1 --duration "$DURATION" 2>&1 \
			| grep "^UPDATE" || true)
		mm=$(printf '%s\n' "$line" | sed -n 's/.*Mmoves\/s: \([0-9.]*\).*/\1/p')
		cm=$(printf '%s\n' "$line" | sed -n 's/.*commits: \([0-9]*\).*/\1/p')
		ab=$(printf '%s\n' "$line" | sed -n 's/.*aborts: \([0-9]*\)).*/\1/p')
		[ -n "$mm" ] || continue
		if awk "BEGIN{exit !($mm > $best)}"; then best=$mm; bc=$cm; ba=$ab; fi
	done
	echo "$1,$2,$MOVESPER,$SPACING,$NBUCKETS,$best,$bc,$ba" | tee -a "$OUT"
}

for c in $CORES; do
	for e in baseline esc-dumb esc-smart; do run_point "$e" "$c"; done
done
echo ">> wrote $OUT" >&2
