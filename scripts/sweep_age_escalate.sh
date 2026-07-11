#!/bin/sh
# Rerun the age-0/age-1 optimistic RYW escalation sweep on bench_txn_3skiplist
# and emit scripts/age_escalate_sweep.csv (plotted by scripts/plot_age_escalate.py).
#
# All binaries use the current engine: the spinlatch install (-DURCU_MCAS_NO_HELP
# -DURCU_MCAS_NO_STEAL), which drops the decide/settle CAS to a release store and
# is ~25% faster than the stock help/steal engine on this workload.  Built against
# the in-tree engine clone (urcu-txn-build, kept in sync by `make urcu-txn`):
#   baseline  = spinlatch + default RYW path (Bloom + find on every in-bracket load)
#   esc-dumb  = spinlatch + -DURCU_TXN_AGE_ESCALATE -DURCU_MCAS_AGE0_TRYLATCH (64-bit k=1)
#   esc-smart = the same + -DURCU_TXN_BLOOM_K=3 -DURCU_TXN_BLOOM_WORDS=16 (1024-bit)
# Two panels: batched (movesper 3, spacing 64) and sparse (movesper 1, spacing
# 4096, RYW forced on).  Best-of-$BEST throughput at $DURATION ms, jemalloc.
#
# Env overrides: CORES, DURATION, BEST, JE, OUT.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
INC="$REPO/urcu-txn-build/include"
LIBDIR="$REPO/urcu-txn-build/src/.libs"
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
OUT=${OUT:-$HERE/age_escalate_sweep.csv}
DURATION=${DURATION:-800}
BEST=${BEST:-2}
CORES=${CORES:-"1 4 16 64 128 192"}

test -f "$LIBDIR/liburcu-qsbr.so" || { echo "engine clone not built -- run 'make urcu-txn'" >&2; exit 1; }
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

build() {	# tag  extra-cflags
	cc -O2 -pthread -Wall $2 -I"$INC" -c "$REPO/src/bench_txn_3skiplist.c" -o "$BUILD/$1.o"
	cc -O2 -pthread -o "$BUILD/$1" "$BUILD/$1.o" \
		-L"$LIBDIR" -Wl,-rpath,"$LIBDIR" -lurcu-qsbr -lurcu-common -lpthread
}
SPIN="-DURCU_MCAS_NO_HELP -DURCU_MCAS_NO_STEAL"
build baseline  "$SPIN"
build esc-dumb  "$SPIN -DURCU_TXN_AGE_ESCALATE -DURCU_MCAS_AGE0_TRYLATCH"
build esc-smart "$SPIN -DURCU_TXN_AGE_ESCALATE -DURCU_MCAS_AGE0_TRYLATCH -DURCU_TXN_BLOOM_K=3 -DURCU_TXN_BLOOM_WORDS=16"

echo "panel,engine,cores,movesper,spacing,mmoves_s,commits,aborts" > "$OUT"

run_point() {	# panel engine movesper spacing cores
	best=0; bc=0; ba=0
	i=0
	while [ "$i" -lt "$BEST" ]; do
		i=$((i + 1))
		line=$(LD_PRELOAD="$JE" "$BUILD/$2" --nupdaters "$5" --nreaders 0 \
			--cpustride 1 --updatespacing "$4" --movesper "$3" --ryw 1 \
			--duration "$DURATION" 2>&1 | grep "^UPDATE" || true)
		mm=$(printf '%s\n' "$line" | sed -n 's/.*Mmoves\/s: \([0-9.]*\).*/\1/p')
		cm=$(printf '%s\n' "$line" | sed -n 's/.*commits: \([0-9]*\).*/\1/p')
		ab=$(printf '%s\n' "$line" | sed -n 's/.*aborts: \([0-9]*\)).*/\1/p')
		[ -n "$mm" ] || continue
		if awk "BEGIN{exit !($mm > $best)}"; then best=$mm; bc=$cm; ba=$ab; fi
	done
	echo "$1,$2,$5,$3,$4,$best,$bc,$ba" | tee -a "$OUT"
}

for c in $CORES; do
	for e in baseline esc-dumb esc-smart; do run_point batched "$e" 3 64 "$c"; done
done
for c in $CORES; do
	for e in baseline esc-dumb esc-smart; do run_point sparse "$e" 1 4096 "$c"; done
done
echo ">> wrote $OUT" >&2
