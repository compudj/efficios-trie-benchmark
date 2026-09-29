#!/bin/bash
# run_p1_writer_scaling.sh -- P1's facility in its ordinary deployment: writer
# scaling with fine-grained locks.
#
# P1's exclusion precondition is per slot, not per structure (sec:exclusion):
# writers whose commits touch disjoint slots may run concurrently, each holding
# a lock that covers every slot its commit writes.  The dentry cache (a lock per
# bucket) and the Fractal Trie (a lock per node) deploy it that way.  P2's
# writer-scaling sweep ran P1's list behind ONE global mutex, which is what
# exclusion means only if it is taken per structure; this sweep takes it the
# way embedders do.
#
# BENCH_NODE_LOCKS (bench_list_scale.c) gives the single-writer arms a lock per
# node -- a table of address-keyed stripes, 4x the node count -- and the
# embedders' protocol: plan without locks, lock the planned nodes, validate the
# plan under the locks (no planned node tombstoned, planned links intact),
# retry on failure; a delete tombstones its victim under the lock before
# unlinking it.  Here each writer owns its slots, so a plan never goes stale
# and the harness's retry count must read 0; the arm pays for planning, locking
# and validating all the same.
#
# Two lock layouts (see bench_list_scale.c): BENCH_NODE_LOCKS=1, a table of
# address-keyed stripes; BENCH_NODE_LOCKS=bit, a bit spinlock in each node's
# tail-padding state word.  Either way locks are taken in ascending lock-address
# order.
#
# ARMS (one binary, engine per batch -- see BATCHES -- 64-byte-aligned nodes as in P2's sweep A,
# jemalloc):
#   txn_sw_bitlock    txn_sw_list, BENCH_NODE_LOCKS=bit  P1 in use, per-node lock
#   txn_sw_nodelock   txn_sw_list, BENCH_NODE_LOCKS=1    P1 in use, striped locks
#   rculist_nodelock  rculist, BENCH_NODE_LOCKS=1        plain RCU, striped locks
#   txn_sw_mutex      txn_sw_list, global writer mutex   P2's baseline
#   txn_list          the multi-writer list, no locks    P2's facility
# Plain RCU is compared with striped locks only.  Both deletes plan their
# predecessor by reading prev: txn_sw_list's prev chain is coherent, while
# plain RCU's prev is written with plain stores after the forward link, which a
# stripe only hashes but a lock IN the node would dereference -- unsound on
# weakly-ordered hardware.  Plain RCU with a lock in the node must take the
# predecessor from a forward walk, which txn_sw_list's delete does not pay, so
# that pairing has no like-for-like reading.
#
# BATCHES.  Two earlier batches used 2 s windows with NO warm-up; at high
# writer counts the transacted arms spend most of a fresh process's first ~2 s
# in a slow start (txn_list at 192 writers: 62 Mops/s over 2 s, 339 after 4 s of
# warm-up), so they were discarded.  TAG=_c with ...-nodelock3 and
# BENCH_WARMUP_SEC=4 (default here) was the first steady-state batch: each point
# runs 4 s untimed, then times 2 s.  It rotated a sixth arm, rculist with
# BENCH_NODE_LOCKS=bit and the unsound prev read above; that arm's rows were
# removed from its csv and log.  The harness then refused that arm and had
# su_write_nodelock zero a new element's state word (a slab-recycled element
# would still carry its tombstone; this build allocates with calloc).  TAG=_d
# with ...-nodelock4 re-runs the five arms on that source.  TAG=_e re-takes
# them on urcu-txn-dev 18809ea8, P1's pin since 2026-09-29,
# with ...-18809ea8 built at bench dc08669 against urcu-txn-build-18809ea8;
# _c and _d are on engine b3e23f9f.  TAG=_f (default here) re-takes _e with
# MEMPLACE=socket (below) and is the batch P1's fig:writerscale plots.  TAG=_g
# is a spot check, WRITERS="160 176 184 188 190 191 192", taken with a guest VM
# (compudjdev, 288 vCPUs, unpinned, mostly idle) shut down: _f's 191 -> 192
# jump (8-23% on the finer-locked arms, 192 the only tight point above 96) is the
# same without it, and 176-191 are two-moded.
#
# MEMORY PLACEMENT (MEMPLACE, default "socket").  The harness pins writer i to
# core i, so up to one socket's worth of writers all run on socket 0; by default
# it interleaves memory across every NUMA node, i.e. half on the other socket.
# That made the 16-128-writer points bistable: at 96 writers runs landed either
# at ~145 Mops/s (both lists alike) or at the list's own rate, ~195 for the bit
# lock and ~264 for rculist, at random.  With memory confined to socket 0 every
# run landed at the latter, within ~1%.  The diagnosis (2026-09-29, 96 writers,
# the bit-lock and rculist arms, same sizing and warm-up, alternating order),
# one csv per question, columns cond/arm/rep/Mops/violations[/extra]:
#   p1_writer_scaling_diag_numa.csv    harness interleave on vs off
#                                      (BENCH_NUMA_INTERLEAVE=0), 6 reps each:
#                                      each produced both modes -- not the
#                                      harness policy
#   p1_writer_scaling_diag_mode.csv    base vs MALLOC_CONF=thp:never vs
#                                      URCU_CALL_RCU_QLEN_CAP=0, with /proc/vmstat
#                                      THP/compaction deltas: each produced both
#                                      modes -- not THP, not the call_rcu queue cap
#   p1_writer_scaling_diag_socket.csv  harness default vs numactl --interleave=0-11
#                                      + BENCH_NUMA_INTERLEAVE=0, with the share
#                                      of the process's memory on socket 0 (numastat
#                                      -p, sampled mid-window): ~50% bistable,
#                                      100% tight -- cross-socket memory
# The first two ran 2 s windows, the third 3 s.
# MEMPLACE=socket therefore runs a point that fits on socket 0 under
# `numactl --interleave=<socket 0's nodes>` with BENCH_NUMA_INTERLEAVE=0 (the
# harness would otherwise override the policy), and a point that spans both
# sockets with the default all-node interleave.  MEMPLACE=all restores the
# harness default for every point, as batches _c-_e were taken.
#
# SIZING is P2's sweep A: CHURN = 64 x writers, LIST_SIZE = 2 x CHURN, writer
# wid owns churn slots wid, wid+nw, ... after unique anchors two nodes apart, so
# writers are disjoint by construction and every writer keeps the same room.
# No readers.  2 s per point, RUNS runs, arm order rotated per run, address
# space pinned (setarch -R) as P2's sweeps are.
#
# Writes scripts/p1_writer_scaling$TAG.csv
#   arm,writers,list_size,churn,run,write_mops,violations,retries,loadavg
# and scripts/p1_writer_scaling$TAG.log (provenance, per-invocation machine state,
# the harness's stderr).
set -u
cd /home/efficios/git/efficios-trie-benchmark
BIN=${BIN:-./arms-p1/bench_list_scale.pin-aligned-18809ea8}
TAG=${TAG:-_f}
MEMPLACE=${MEMPLACE:-socket}
export BENCH_WARMUP_SEC=${BENCH_WARMUP_SEC:-4}
ENG_TREE=${ENG_TREE:-urcu-txn-build-18809ea8}
DUR=${DUR:-2}
RUNS=${RUNS:-5}
WRITERS=${WRITERS:-"1 2 4 8 16 32 64 96 128 160 191 192"}
OUT=scripts/p1_writer_scaling$TAG.csv
LOG=scripts/p1_writer_scaling$TAG.log

[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
ldd "$BIN" | grep -q "$ENG_TREE/src/.libs/liburcu-qsbr" \
  || { echo "ERROR: $BIN does not load liburcu from $ENG_TREE" >&2; exit 1; }

{
  echo "# run_p1_writer_scaling.sh  $(date -Is)"
  echo "# bench HEAD $(git rev-parse --short=8 HEAD); bench_list_scale.c sha256 $(sha256sum src/bench_list_scale.c | cut -c1-16)"
  echo "# binary $BIN sha256 $(sha256sum "$BIN" | cut -c1-16); engine $(git -C "$ENG_TREE" rev-parse --short=8 HEAD)"
  echo "# DUR=$DUR RUNS=$RUNS WRITERS=$WRITERS BENCH_WARMUP_SEC=$BENCH_WARMUP_SEC"
  echo "# start: $(cat /proc/loadavg)"
} > "$LOG"
echo "arm,writers,list_size,churn,run,write_mops,violations,retries,loadavg" > "$OUT"

# label|engine|extra-env
# Socket 0: its cores (one PU per core, as the harness pins) and NUMA nodes.
TPC=$(lscpu | awk -F: '/Thread\(s\) per core/{gsub(/ /,"",$2); print $2}')
SOCK0_CORES=$(( $(for c in /sys/devices/system/cpu/cpu[0-9]*; do cat $c/topology/physical_package_id; done | grep -cx 0) / TPC ))
SOCK0_NODES=$(for n in /sys/devices/system/node/node[0-9]*; do
  c=$(cut -d- -f1 $n/cpulist | cut -d, -f1)
  [ "$(cat /sys/devices/system/cpu/cpu$c/topology/physical_package_id)" = 0 ] && basename $n | sed 's/node//'
done | sort -n | paste -sd,)
echo "# MEMPLACE=$MEMPLACE: socket 0 = $SOCK0_CORES cores, NUMA nodes $SOCK0_NODES" >> "$LOG"

ARMS=("txn_sw_bitlock|txn_sw_list|BENCH_NODE_LOCKS=bit" "txn_sw_nodelock|txn_sw_list|BENCH_NODE_LOCKS=1" "rculist_nodelock|rculist|BENCH_NODE_LOCKS=1" "txn_sw_mutex|txn_sw_list|X=0" "txn_list|txn_list|X=0")
N=${#ARMS[@]}

for r in $(seq 1 "$RUNS"); do
  for w in $WRITERS; do
    ch=$((64 * w)); ls=$((2 * ch))
    if [ "$MEMPLACE" = socket ] && [ "$w" -le "$SOCK0_CORES" ]; then
      memplace="numactl --interleave=$SOCK0_NODES"; memenv="BENCH_NUMA_INTERLEAVE=0"; mem=socket0
    else
      memplace=""; memenv=""; mem=all
    fi
    for i in $(seq 0 $((N - 1))); do
      IFS='|' read -r lbl eng extra <<<"${ARMS[$(( (i + r - 1) % N ))]}"
      la=$(cut -d' ' -f1 /proc/loadavg)
      {
        echo "## $lbl w=$w run=$r mem=$mem $(date +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"
        top -bn1 -o %CPU | sed -n '8,10p'
      } >> "$LOG"
      out=$($memplace setarch "$(uname -m)" -R env $memenv LIST_SIZE="$ls" CHURN="$ch" DURATION_SEC="$DUR" \
            BENCH_WRITESCALE=1 BENCH_FIXED_WRITERS="$w" BENCH_READERS=0 $extra \
            timeout 600 "$BIN" "$eng" 192 2>&1)
      echo "$out" | grep -v '^[0-9]' | sed 's/^/   | /' >> "$LOG"
      line=$(echo "$out" | grep -E '^[0-9]+ ' | tail -1)
      ret=$(echo "$out" | grep -o 'retries = [0-9]*' | awk '{print $3}')
      if [ -z "$line" ]; then
        echo "$lbl,$w,$ls,$ch,$r,,,,$la" >> "$OUT"
        echo "!! $lbl w=$w run=$r NO OUTPUT" >&2
        continue
      fi
      set -- $line
      echo "$lbl,$w,$ls,$ch,$r,$3,$4,${ret:-},$la" >> "$OUT"
      echo ">> $lbl w=$w run=$r  $3 Mops/s  viol=$4 retries=${ret:--}" >&2
    done
  done
done

echo "# end: $(date -Is) $(cat /proc/loadavg)" >> "$LOG"
if awk -F, 'NR>1 && ($7+0>0 || $8+0>0){f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: violations or plan retries in $OUT" >&2
  exit 1
fi
echo "# ALL DONE, zero violations and zero plan retries -> $OUT" >&2
