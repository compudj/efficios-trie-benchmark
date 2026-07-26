#!/bin/bash
# run_mvrlu_vs_txn.sh -- MV-RLU vs the single-writer flip-latch on the SAME
# bidirectional-list workload: 10000 nodes, 2% update set (200 churn nodes),
# large enough that a read traverses a real cache-pressured span and a writer
# touches a spread-out node instead of one L1-resident node.
#
# Both engines here give a COHERENT BIDIRECTIONAL walk, which is what makes the
# comparison meaningful -- but NOT the same reader guarantee: an MV-RLU reader
# section observes a coherent SNAPSHOT, strictly stronger than txn_sw_list's
# per-slot-linearizable (non-snapshot) reads.  We measure both as-is rather than
# handicapping either; say which guarantee bought which number when reporting.
#
# CONFIG: jemalloc per-CPU arenas (JEMALLOC=1) for BOTH engines -- this is P1's
# measurement config, and MV-RLU links jemalloc upstream by default anyway
# (Makefile.inc MEMMGR), so neither side is on a foreign allocator.
#
# MV-RLU CAVEATS, both load-bearing -- see third_party/mvrlu/PROVENANCE.txt:
#   * gclk, NOT ordo.  The ordo variant needs __ORDO_BOUNDARY, a per-machine
#     constant that CORRECTNESS depends on and that has never been measured on
#     this box.  gclk needs no constant but reintroduces the central-clock
#     contention ordo exists to remove, so these are MV-RLU's DEPLOYABLE numbers,
#     not the ASPLOS paper's.  Label them as such.
#   * The QP thread is confined (engine default: the NUMA node of CPU 0).
#     Unconfined it roams this 24-node box and costs ~7x with ~7x variance --
#     a handicap no other engine here suffers, since every worker is pinned and
#     liburcu's call_rcu workers are per-CPU.  BENCH_MVRLU_QP_CPUS=free
#     reproduces the unconfined behaviour if you want to see it.
#   * MV-RLU has exactly ONE global QP thread by design (mvrlu.c:18, a single
#     static g_qp_thread), so reclamation does not scale with the machine the way
#     per-CPU call_rcu does.  That is the writescale plateau, and it is a
#     property of the algorithm, not of this harness.
#
# txn_sw_list IS A SINGLE-WRITER ENGINE.  In the writescale panel its >1-writer
# points are threads SERIALIZED ON THE BENCH MUTEX (su_write takes g_su_wlock),
# not concurrent writers -- the flip-latch requires exclusion, so BENCH_SU_NOLOCK
# would be incorrect above one writer and is deliberately NOT set here.  That
# panel therefore compares a lock-serialized SW engine against a concurrent MW
# one; the like-for-like MW arm is txn_list (P2's subject), not this.
#
# Writes scripts/mvrlu_vs_txn.csv:
#   mode,engine,run,x,read_mvisits,write_mops,viol
# where mode in {readceil,mixed,writescale}; x is readers (readceil, mixed) or
# writers (writescale).  write_mops is 0 in readceil rows, read_mvisits 0 in
# writescale rows.  Any nonzero viol is a hard fail -- these engines are
# supposed to keep the list coherent in both directions.
set -u
cd /home/efficios/git/efficios-trie-benchmark
export DURATION_SEC=${DURATION_SEC:-3}
export LIST_SIZE=${LIST_SIZE:-10000} CHURN=${CHURN:-200}
RUNS=${RUNS:-2}
MAXT=${MAXT:-192}
OUT=scripts/mvrlu_vs_txn.csv

echo ">> building bench_list_scale JEMALLOC=1 (per-CPU arenas) ..." >&2
make bench_list_scale JEMALLOC=1 >/dev/null 2>&1 \
  || { echo "BUILD FAILED" >&2; exit 1; }
ldd ./bench_list_scale | grep -qi jemalloc \
  || { echo "ERROR: binary not linked against jemalloc" >&2; exit 1; }

echo "mode,engine,run,x,read_mvisits,write_mops,viol" > "$OUT"

for r in $(seq 1 "$RUNS"); do
  for eng in txn_sw_list mvrlu_list; do
    # read ceiling, no writer: col1=readers col2=read_mvisits col4=viol
    echo ">> readceil   $eng run=$r" >&2
    BENCH_NO_WRITER=1 ./bench_list_scale "$eng" "$MAXT" 2>/dev/null \
      | awk -v E="$eng" -v R="$r" '/^[0-9]/{print "readceil,"E","R","$1","$2",0,"$4}' >> "$OUT"
    # 1 writer + N readers (the read-mostly regime): col2=read col3=write
    echo ">> mixed      $eng run=$r" >&2
    ./bench_list_scale "$eng" "$MAXT" 2>/dev/null \
      | awk -v E="$eng" -v R="$r" '/^[0-9]/{print "mixed,"E","R","$1","$2","$3","$4}' >> "$OUT"
    # writer scaling, 0 readers: col1=writers col3=write_mops
    echo ">> writescale $eng run=$r" >&2
    BENCH_WRITESCALE=1 BENCH_READERS=0 ./bench_list_scale "$eng" "$MAXT" 2>/dev/null \
      | awk -v E="$eng" -v R="$r" '/^[0-9]/{print "writescale,"E","R","$1",0,"$3","$4}' >> "$OUT"
  done
done

echo ">> done -> $OUT" >&2
# coherence gate: any nonzero viol is a hard fail, not a warning
if awk -F, 'NR>1 && $7+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: nonzero coherence violations in $OUT" >&2
  exit 1
fi
echo "# ALL DONE, zero coherence violations -> $OUT" >&2
