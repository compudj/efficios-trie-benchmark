#!/bin/bash
# run_list_scale_base.sh -- regenerate the bench_list_scale README tables that
# had no generator (they were ad-hoc runs): the read-only ceiling, read scaling
# under one writer, the rcu_head node-layout control, and the "Writer scaling &
# allocation" tables on the tiny default list.
#
# Tables (the `table` column):
#   ro       -- BENCH_NO_WRITER, readers 1..383 (SMT at the top), nine engines.
#   rw1      -- readers 1..191 + one writer, the same nine engines.
#   layout   -- 96 readers, no writer, LIST_SIZE 1000 and 30000, three node
#               layouts: `aligned`, the default since c298aa5 (every node on its
#               own cache line, rcu_head segregated); `packed`, the 24 B nodes
#               of the earlier default (-DLIST_NODE_ALIGN=0); and `packed-inline`,
#               packed with the rcu_head embedded (40 B,
#               -DLIST_RCU_INLINE_RCU_HEAD).  seqlock and txn_sw_list.
#   layout1w -- aligned against packed with ONE writer and 191 readers: the
#               writer dirties the line of the node it links after, and a packed
#               line carries two or three nodes.
#   wchurn   -- txn_list writers 1..192, plain churn, three allocators.
#   wrandom  -- same, BENCH_RANDOM_POS (transacted 200-slot index).
#   index    -- BENCH_RANDOM_POS at 64/128/192 writers as the index grows
#               (CHURN = index slots; LIST_SIZE raised with it past 1000).
#
# Allocators, as in run_list_scale_alloc.sh: glibc (slab off,
# URCU_TXN_NO_CACHE=1), jemalloc percpu_arena:percpu (slab off) and glibc + the
# descriptor slab (the shipping default).  `index` runs the last two.
#
# Writes scripts/list_scale_base.csv:
#   table,variant,engine,run,list_size,churn,x,read_mvisits,write_mops,viol
#
# Env: DURATION_SEC RUNS TXN_TREE OUT TABLES ("ro rw1 layout wchurn wrandom index")
#
# Memory: the writer sweeps outrun reclaim -- a writer shares its hardware thread
# with its call_rcu worker -- so the deferred-free backlog grows for the whole
# point.  The `index` table passed 64 GB at 128 writers; bound the run with a
# memory cgroup sized for that (it ran under MemoryMax=256G), and check the row
# counts afterwards: a killed point leaves a short group, not an error.
set -u
cd "$(dirname "$0")/.." || exit 1
export DURATION_SEC=${DURATION_SEC:-3}
RUNS=${RUNS:-2}
TABLES=${TABLES:-ro rw1 layout wchurn wrandom index}
OUT=${OUT:-scripts/list_scale_base.csv}
# See the note in run_list_scale_alloc.sh: this repo's own engine build.
TXN_TREE=${TXN_TREE:-$PWD/urcu-txn-build}
BIN=$(mktemp -d "${TMPDIR:-/tmp}/ls_base.XXXXXX") || exit 1
trap 'rm -rf "$BIN"' EXIT

build() {	# $1=output name, rest = make arguments
  local name=$1; shift
  rm -f bench_list_scale src/bench_list_scale.o
  make bench_list_scale URCU_TXN_BUILD="$TXN_TREE" "$@" >/dev/null 2>&1 \
    && cp bench_list_scale "$BIN/$name" \
    || { echo "$name BUILD FAILED" >&2; exit 1; }
}
echo ">> building glibc, jemalloc, packed and packed-inline variants (forced clean) ..." >&2
build jem    JEMALLOC=1
ldd "$BIN/jem" | grep -qi jemalloc || { echo "jemalloc NOT linked" >&2; exit 1; }
build packed "LIST_POOL_CFLAGS=-DLIST_NODE_ALIGN=0"
build inline "LIST_POOL_CFLAGS=-DLIST_NODE_ALIGN=0 -DLIST_RCU_INLINE_RCU_HEAD"
build glibc			# last: leaves the default binary in the tree

echo "table,variant,engine,run,list_size,churn,x,read_mvisits,write_mops,viol" > "$OUT"

# $1=table $2=variant $3=engine $4=binary $5=max threads $6=list_size $7=churn,
# rest = environment for the run.
sweep() {
  local table=$1 variant=$2 eng=$3 bin=$4 max=$5 ls=$6 ch=$7 r; shift 7
  for r in $(seq 1 "$RUNS"); do
    echo ">> $table $variant $eng LIST_SIZE=$ls CHURN=$ch run=$r" >&2
    env LIST_SIZE="$ls" CHURN="$ch" "$@" "$BIN/$bin" "$eng" "$max" 2>/dev/null \
      | awk -v P="$table,$variant,$eng,$r,$ls,$ch" '/^[0-9]/{print P","$1","$2","$3","$4}' >> "$OUT"
  done
}

ENGINES="txn_sw_list txn_list rculist seqlock iscrw rwlock_r rwlock_w mutex fairmutex"
JEM="URCU_TXN_NO_CACHE=1 MALLOC_CONF=percpu_arena:percpu"

for t in $TABLES; do
  case $t in
  ro)
    for e in $ENGINES; do sweep ro default "$e" glibc 384 1000 200 BENCH_NO_WRITER=1; done ;;
  rw1)
    for e in $ENGINES; do sweep rw1 default "$e" glibc 192 1000 200; done ;;
  layout)
    for ls in 1000 30000; do
      for e in seqlock txn_sw_list; do
        sweep layout aligned     "$e" glibc  192 "$ls" 200 BENCH_NO_WRITER=1 BENCH_FIXED_READERS=96
        sweep layout packed      "$e" packed 192 "$ls" 200 BENCH_NO_WRITER=1 BENCH_FIXED_READERS=96
      done
      sweep layout packed-inline txn_sw_list inline 192 "$ls" 200 BENCH_NO_WRITER=1 BENCH_FIXED_READERS=96
    done
    for e in txn_sw_list txn_list rculist; do
      sweep layout1w aligned "$e" glibc  192 1000 200 BENCH_FIXED_READERS=191
      sweep layout1w packed  "$e" packed 192 1000 200 BENCH_FIXED_READERS=191
    done ;;
  wchurn|wrandom)
    rnd=; [ "$t" = wrandom ] && rnd=BENCH_RANDOM_POS=1
    sweep "$t" glibc      txn_list glibc 192 1000 200 BENCH_WRITESCALE=1 BENCH_READERS=0 $rnd URCU_TXN_NO_CACHE=1
    sweep "$t" jemalloc   txn_list jem   192 1000 200 BENCH_WRITESCALE=1 BENCH_READERS=0 $rnd $JEM
    sweep "$t" glibc+slab txn_list glibc 192 1000 200 BENCH_WRITESCALE=1 BENCH_READERS=0 $rnd ;;
  index)
    for slots in 200 10000 100000 1000000; do
      ls=$slots; [ "$ls" -lt 1000 ] && ls=1000
      for w in 64 128 192; do
        sweep index jemalloc   txn_list jem   192 "$ls" "$slots" BENCH_WRITESCALE=1 BENCH_READERS=0 \
          BENCH_RANDOM_POS=1 BENCH_FIXED_WRITERS="$w" $JEM
        sweep index glibc+slab txn_list glibc 192 "$ls" "$slots" BENCH_WRITESCALE=1 BENCH_READERS=0 \
          BENCH_RANDOM_POS=1 BENCH_FIXED_WRITERS="$w"
      done
    done ;;
  *) echo "unknown table $t" >&2; exit 1 ;;
  esac
done
echo "# ALL DONE -> $OUT" >&2
