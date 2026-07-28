#!/bin/bash
# Build the four bench_txn_3skiplist arms the P2 remeasure needs.
# See scripts/run_p2_engine_remeasure.sh for WHY there are four and what the
# three preparation traps are.  Reproducible from a clean tree.
set -eu

REPO=/mnt/data/efficios/git/efficios-trie-benchmark
UPSTREAM=/mnt/data/efficios/git/userspace-rcu-txn
OLD_COMMIT=97443472			# last commit where BOTH install disciplines build
PIN_TREE=$REPO/urcu-txn-build		# already at P2's pin, b3e23f9f
OLD_TREE=$REPO/urcu-txn-build-$OLD_COMMIT
OUT=${OUT:-$REPO/arms-p2}

mkdir -p "$OUT"

# --- the historical engine tree --------------------------------------------
if [ ! -d "$OLD_TREE" ]; then
	echo ">> cloning + building the engine at $OLD_COMMIT"
	git clone -q --no-hardlinks --shared "$UPSTREAM" "$OLD_TREE"
	git -C "$OLD_TREE" checkout -q "$OLD_COMMIT"
	( cd "$OLD_TREE" && ./bootstrap >/dev/null 2>&1 \
	  && CFLAGS="-O2 -g" ./configure >/dev/null 2>&1 )
	make -C "$OLD_TREE/src" -j"$(nproc)" >/dev/null
fi

pin_i=$PIN_TREE/include;  pin_l=$PIN_TREE/src/.libs
old_i=$OLD_TREE/include;  old_l=$OLD_TREE/src/.libs

# At 97443472 the handle is `struct urcu_mcas_txn`; it was renamed to
# `struct urcu_txn` later.  The preprocessor matches WHOLE identifiers, so this
# rewrites the struct tag without touching urcu_txn_begin() and friends -- and
# the bare token `urcu_txn` appears nowhere in the historical headers (checked).
SHIM="-Durcu_txn=urcu_mcas_txn"

# Restore the txn-layer defaults that URCU_MCAS_STOCK would otherwise drag along,
# so the A/B varies the INSTALL DISCIPLINE and nothing else.
STOCK="-DURCU_MCAS_STOCK -DURCU_TXN_AGE_ESCALATE -DURCU_TXN_BLOOM_WORDS=16 -DURCU_TXN_BLOOM_K=3"

# Force escalation into the fair-mutex lane from attempt 0: with PER_COST_NUM=0
# the budget is the flat URCU_TXN_FALLBACK, and the MIN/MAX clamps do not apply.
LANE="-DURCU_TXN_FALLBACK_PER_COST_NUM=0 -DURCU_TXN_FALLBACK=0"

build() {
	local name=$1 inc=$2 lib=$3; shift 3
	gcc -O2 -pthread -Wall "$@" -I"$inc" -c "$REPO/src/bench_txn_3skiplist.c" \
		-o "/tmp/p2arm-$name.o"
	gcc -O2 -pthread -o "$OUT/$name" "/tmp/p2arm-$name.o" \
		-L"$lib" -Wl,-rpath,"$lib" -lurcu-qsbr -lurcu-common -lpthread
	echo "   built $name"
}

build pin-sole    "$pin_i" "$pin_l"
build pin-serial  "$pin_i" "$pin_l" $LANE
build old-sole    "$old_i" "$old_l" -DURCU_TXN_RYW_DEFAULT=1 $SHIM
build old-helping "$old_i" "$old_l" -DURCU_TXN_RYW_DEFAULT=1 $SHIM $STOCK

# --- prove the arms actually differ ----------------------------------------
# Not a size check: count atomic compare-exchange sites in the preprocessed
# translation unit.  Helping adds the per-record install state machine and the
# CAS-based settle that sole-driver collapses into release stores, so the
# helping arm must carry materially MORE of them.  If these are equal, a -D
# silently failed to take and the A/B is measuring nothing.
so=$(gcc -E -DURCU_TXN_RYW_DEFAULT=1 $SHIM -I"$old_i" "$REPO/src/bench_txn_3skiplist.c" \
	2>/dev/null | grep -c uatomic_cmpxchg)
he=$(gcc -E -DURCU_TXN_RYW_DEFAULT=1 $SHIM $STOCK -I"$old_i" "$REPO/src/bench_txn_3skiplist.c" \
	2>/dev/null | grep -c uatomic_cmpxchg)
echo ">> cmpxchg sites: old-sole=$so  old-helping=$he"
[ "$he" -gt "$so" ] || { echo "ERROR: helping arm is not distinct -- a -D did not take"; exit 1; }
echo ">> arms in $OUT"
