#!/bin/sh
# Disjoint-write-set hint on the urcu-txn 3-hash: compare, at matched width and
# from ONE binary (only the runtime flag differs), the Bloom-free disjoint
# blind-append against the default read-your-own-writes (Bloom + blind-append).
#
#   disj : --disjoint 1  -- blind append, NO Bloom (urcu_txn_declare_disjoint)
#   ryw1 : --disjoint 0  -- the default read-your-own-writes: Bloom + blind append
#
# HISTORICAL: the committed scripts/disjoint_hash.csv also carries a third "find"
# curve (ryw off, reconcile find).  That invisible-writes mode is retired -- read-
# your-own-writes is now the engine's unconditional default -- so this sweep no
# longer produces it; re-running overwrites the CSV with just the two curves above.
#
# The engine hint is urcu_txn_declare_disjoint() (userspace-rcu-txn commit
# 72ae6027).  Writes scripts/disjoint_hash.csv; plot with plot_disjoint_hash.py.
set -eu
DEV=${DEV:-/home/efficios/git/userspace-rcu-txn}
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/.." && pwd)
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
BEST=${BEST:-5}; DURATION=${DURATION:-800}
CORES=${CORES:-"1 4 16 32 64 128 192"}
BIN=$(mktemp)
OUT=$HERE/disjoint_hash.csv
# The shipping default engine already IS this config (spinlatch install, age-0
# flat install, k=3 / 1024-bit filter), so no engine -D flags are needed; only
# the runtime --disjoint variant below differs across the two curves.
cc -O2 -pthread -I"$DEV/include" -I"$DEV/src" "$BENCH/src/bench_txn_3hash.c" \
	-o "$BIN" -L"$DEV/src/.libs" -Wl,-rpath,"$DEV/src/.libs" \
	-lurcu-qsbr -lurcu-cds -lurcu-common -lpthread
trap 'rm -f "$BIN"' EXIT
echo "variant,cores,median_mmoves,aborts" > "$OUT"
med() { # variant cores flags...
	vals=""; abs=""; i=0
	while [ "$i" -lt "$BEST" ]; do i=$((i+1))
		line=$(LD_PRELOAD="$JE" "$BIN" --nupdaters "$2" --nreaders 0 --cpustride 1 \
			--nbuckets 16384 --updatespacing 512 --movesper 8 $3 \
			--duration "$DURATION" 2>&1 | grep "^UPDATE")
		mm=$(printf '%s' "$line" | sed -n 's/.*Mmoves\/s: \([0-9.]*\).*/\1/p')
		ab=$(printf '%s' "$line" | sed -n 's/.*aborts: \([0-9]*\)).*/\1/p')
		[ -n "$mm" ] || continue
		vals="$vals $mm"; abs="$abs $ab"
	done
	res=$(echo "$vals $abs" | awk -v n="$BEST" '{for(i=1;i<=n;i++)v[i]=$i;
		for(i=1;i<n;i++)for(j=i+1;j<=n;j++)if(v[j]<v[i]){t=v[i];v[i]=v[j];v[j]=t};
		as=0;for(i=n+1;i<=2*n;i++)as+=$i;printf "%.2f,%.0f",v[int((n+1)/2)],as/n}')
	echo "$1,$2,$res" | tee -a "$OUT"
}
for c in $CORES; do
	med disj "$c" "--disjoint 1"
	med ryw1 "$c" "--disjoint 0"
done
echo ">> wrote $OUT" >&2
