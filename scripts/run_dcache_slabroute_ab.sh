#!/bin/bash
# run_dcache_slabroute_ab.sh -- should the descriptor slab's rseq lists and its
# batch retirement be ON by default?  Interleaved A/B of the four slab routes,
# all liburcu 18809ea8:
#
#   base        urcu-txn-build                      atomic lists, call_rcu per descriptor
#   rseq        urcu-txn-build-rseq-18809ea8        rseq lists,   call_rcu per descriptor
#   batch       urcu-txn-build-batch-18809ea8       atomic lists, one call_rcu per batch
#   batch_rseq  urcu-txn-build-batch-rseq-18809ea8  rseq lists,   one call_rcu per batch
#   pf_batch, pf_batch_rseq: the same two batch routes with batches kept PER
#               FLAVOR (urcu-txn-slab-perflavor.patch on 18809ea8)
#
# Batch's structural claim is that the call_rcu worker stops touching the
# slab's lines: per descriptor, the worker's callback dereferences every block
# a grace period after the commit; batched, the freeing thread links the chain
# while it is hot and the worker splices it in O(1).  How much that is worth
# depends on WHERE the worker runs relative to the writer, so churn runs under
# three worker placements (bench_dcache_churn's knobs):
#
#   copin    each writer's RT worker on the writer's own cpu (every figure so
#            far; the per-descriptor route's best case: shared L1/L2)
#   sibling  on the writer's SMT sibling (DC_CRDP_CPU_OFFSET=$SIB)
#   shared   no per-writer worker: liburcu's default one (DC_CRDP=shared), what
#            an embedder gets without asking
#
# Panels (writer paths -- the slab is never on a reader's):
#   churn_alloc  bench_dcache_churn, unlink + re-add (descriptor + dentry free)
#   churn_inpl   bench_dcache_churn --in-place (txn engines still commit
#                descriptors; lock engines commit none: their control)
#   move_w       bench_dcache cross-directory moves, writers only (co-pinned
#                workers: bench_dcache has no placement knob)
#
# seqlock never allocates a descriptor: it is the control every route must
# leave flat.  Each (panel, placement, W, engine) point runs every route back to
# back, RUNS times, rotating the order.  One row per RUN, with peak RSS.
#
# Non-vacuity, checked before any number: each route's build log must carry its
# flags, its binaries must link librseq iff rseq, the rseq runtime must be
# usable, and a counted run must show the batch routes retiring descriptors in
# batches (call_rcu per op 0.6 vs 1.8 on txn churn).
#
# Output: scripts/dcache_slabroute_ab.csv
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
S=$REPO/scripts
CSV=${CSV:-$S/dcache_slabroute_ab.csv}
WORK=${WORK:-/tmp/dcache_slabroute_ab}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
DUR=${DUR:-1000}
RUNS=${RUNS:-7}
ENGINES=${ENGINES:-"seqlock txn-global txn-mark bucketlock"}
PANELS=${PANELS:-"churn_alloc churn_inpl move_w"}
PLACES=${PLACES:-"copin sibling shared"}
W_ALLOC=${W_ALLOC:-"1 4 16 48"}
W_ALLOC_OTHER=${W_ALLOC_OTHER:-"4 48"}	# sibling / shared placements
W_INPL=${W_INPL:-"4 48"}
W_MOVE=${W_MOVE:-"1 8 48"}
ROUTES=${ROUTES:-"base rseq batch batch_rseq"}
SIB=${SIB:-192}

declare -A LIBOF=( [base]=urcu-txn-build [rseq]=urcu-txn-build-rseq-18809ea8 \
	[batch]=urcu-txn-build-batch-18809ea8 [batch_rseq]=urcu-txn-build-batch-rseq-18809ea8 \
	[pf_batch]=urcu-txn-build-pf-batch [pf_batch_rseq]=urcu-txn-build-pf-batch-rseq )
declare -A CHURN=( [seqlock]=bench_dcache_churn_seqlock [txn-global]=bench_dcache_churn_txn \
	[txn-pernode]=bench_dcache_churn_txn_pernode [txn-mark]=bench_dcache_churn_txn_mark \
	[bucketlock]=bench_dcache_churn_bucketlock )
declare -A MOVE=( [seqlock]=bench_dcache_seqlock [txn-global]=bench_dcache_txn \
	[txn-pernode]=bench_dcache_txn_pernode [txn-mark]=bench_dcache_txn_mark \
	[bucketlock]=bench_dcache_bucketlock )
declare -A PLACEENV=( [copin]="" [sibling]="DC_CRDP_CPU_OFFSET=$SIB" [shared]="DC_CRDP=shared" )

CPULIST=$(hwloc-calc --li --po -I PU core:all.pu:0)
PIN="--cpulist $CPULIST"
[[ -f "$JE" ]] || { echo "jemalloc not at $JE"; exit 1; }
die() { echo "FATAL: $*" >&2; exit 1; }

# ---- build: one tree copy per route ----------------------------------------
mkdir -p "$WORK"
RSS=$WORK/probe_maxrss.so
cc -O2 -shared -fPIC -o "$RSS" "$REPO/experiments/callrcu/probe_maxrss.c" || die "rss probe"
cc -O2 -shared -fPIC -o "$WORK/count.so" "$REPO/experiments/callrcu/count_callrcu.c" -ldl ||
	die "count probe"
for rt in $ROUTES; do
	d=$WORK/$rt lib=$REPO/${LIBOF[$rt]}
	if [[ -z "${NOBUILD:-}" ]]; then
		rm -rf "${d:?}" && mkdir -p "$d"
		(cd "$REPO/experiments/dcache" && git ls-files | tar cf - -T -) | tar xf - -C "$d"
		# the working tree's uncommitted harness edits are what we measure
		(cd "$REPO/experiments/dcache" && git diff --name-only . | sed 's|^experiments/dcache/||' |
			xargs -r cp --parents -t "$d")
		echo ">> build $rt" >&2
		make -C "$d" -j32 URCU_TXN_BUILD="$lib" churn bench > "$d/build.log" 2>&1 ||
			die "build $rt, see $d/build.log"
	fi
	# flags: what the Makefile derived must match the route's name
	b=no r=no
	grep -q -- '-DURCU_TXN_SLAB_BATCH' "$d/build.log" && b=yes
	grep -q -- '-DURCU_SLAB_RSEQ' "$d/build.log" && r=yes
	[[ $b == $([[ $rt == *batch* ]] && echo yes || echo no) ]] || die "$rt: batch flag $b"
	[[ $r == $([[ $rt == *rseq ]] && echo yes || echo no) ]] || die "$rt: rseq flag $r"
	if [[ $rt == *rseq ]]; then
		ldd "$d/bench_dcache_churn_txn_mark" | grep -q librseq || die "$rt: no librseq"
	fi
	echo ">> $rt: batch=$b rseq=$r ($(git -C "$lib" log -1 --format=%h))" >&2
done

# rseq runtime: registered + membarrier RSEQ, or the rseq routes are atomic ones
rl=$(grep -ohE '\-L[^ ]*librseq[^ ]*' "$REPO/${LIBOF[rseq]}/src/Makefile" | head -1)
ri=$(grep -ohE '\-I[^ ]*librseq[^ ]*' "$REPO/${LIBOF[rseq]}/src/Makefile" | head -1)
cat > "$WORK/rseqchk.c" <<'CHK'
#define _GNU_SOURCE
#include <stdio.h>
#include <syscall.h>
#include <unistd.h>
#include <rseq/rseq.h>
int main(void) {
	(void) rseq_init();
	if (!rseq_registered()) { printf("not-registered\n"); return 1; }
	if (syscall(__NR_membarrier, 1 << 8 /* REGISTER_PRIVATE_EXPEDITED_RSEQ */, 0, 0)) {
		printf("no-membarrier-rseq\n"); return 1; }
	printf("rseq-active\n"); return 0;
}
CHK
cc -O2 $ri -o "$WORK/rseqchk" "$WORK/rseqchk.c" $rl -Wl,-rpath,${rl#-L} -lrseq || die "rseqchk build"
[[ "$("$WORK/rseqchk")" == rseq-active ]] || die "rseq runtime: $("$WORK/rseqchk")"

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }
# private directories (bench --share 1): --ndirs 16xW, used until 2026-10-02,
# paired writer i with i+W/2 -- on another NUMA node from 16 writers on.
churn_args() { echo "--readers 0 --writers $1 --share 1 --slots 32 --nbuckets 1048576"; }

# batch routes must retire in batches: counted, untimed, 4 writers, txn-mark
for rt in $ROUTES; do
	out=$(cd "$WORK/$rt" && env LD_PRELOAD="$JE $WORK/count.so" ./bench_dcache_churn_txn_mark \
		$(churn_args 4) --duration 500 $PIN 2> "$WORK/count.err")
	n=$(awk '/^callrcu:/{s+=$3} END{print s+0}' "$WORK/count.err")
	per=$(python3 -c "print(f'{$n/($(field "$out" Mchurn/s:)*0.5e6):.2f}')")
	echo ">> $rt: call_rcu per churn op (txn-mark, counted) = $per" >&2
	if [[ $rt == *batch* ]]; then
		python3 -c "import sys; sys.exit(0 if $per < 1.0 else 1)" || die "$rt not batching ($per)"
	else
		python3 -c "import sys; sys.exit(0 if $per > 1.4 else 1)" || die "$rt batching? ($per)"
	fi
done

SRC_ID=$("$S/dcache_src_id.sh" "$REPO/urcu-txn-build")
[[ -f "$CSV" ]] || echo "panel,place,engine,writers,route,run,mops_s,maxrss_kib,conserved,src" > "$CSV"

# one <panel> <place> <engine> <W> <route> <run>
one() {
	local panel=$1 place=$2 e=$3 w=$4 rt=$5 r=$6 d=$WORK/$5 out bin args rate rss ok=OK
	case $panel in
	churn_alloc) bin=${CHURN[$e]}; args=$(churn_args "$w") ;;
	churn_inpl)  bin=${CHURN[$e]}; args="$(churn_args "$w") --in-place" ;;
	move_w)      bin=${MOVE[$e]}
		args="--nthreads $w --writers $w --op-mix rename=0,move=1,exchange=0 --ndirs $((16 * w)) --depth 4 --leaves 32 --nbuckets 1048576" ;;
	esac
	out=$(cd "$d" && env ${PLACEENV[$place]} LD_PRELOAD="$JE $RSS" ./"$bin" $args \
		--duration "$DUR" $PIN 2> "$WORK/err")
	grep -q "conservation: OK" <<< "$out" || ok=FAIL
	case $panel in
	churn_*) rate=$(field "$out" Mchurn/s:) ;;
	move_w)  rate=$(field "$out" Mrenames/s:) ;;
	esac
	rss=$(sed -n 's/^probe_maxrss: //p' "$WORK/err" | tail -1)
	echo "$panel,$place,$e,$w,$rt,$r,$rate,$rss,$ok,$SRC_ID" >> "$CSV"
	printf "  %-11s %-7s %-11s w=%-3s %-10s r=%s %9s Mops/s  %7s MiB  %s\n" \
		"$panel" "$place" "$e" "$w" "$rt" "$r" "$rate" "$(( ${rss:-0} / 1024 ))" "$ok" >&2
}

read -ra RA <<< "$ROUTES"
N=${#RA[@]}
point() {	# point <panel> <place> <engine> <W>
	local r i
	for r in $(seq 1 "$RUNS"); do
		for i in $(seq 0 $((N - 1))); do
			one "$1" "$2" "$3" "$4" "${RA[$(( (i + r) % N ))]}" "$r"
		done
	done
}

for p in $PANELS; do
	places=copin; [[ $p == churn_alloc ]] && places=$PLACES
	for pl in $places; do
		case $p in
		churn_alloc) WS=$W_ALLOC; [[ $pl != copin ]] && WS=$W_ALLOC_OTHER ;;
		churn_inpl)  WS=$W_INPL ;;
		move_w)      WS=$W_MOVE ;;
		esac
		echo ">> $(date +%T) panel $p, workers $pl" >&2
		for w in $WS; do
			for e in $ENGINES; do point "$p" "$pl" "$e" "$w"; done
		done
	done
done
echo ">> $(date +%T) DONE -> $CSV" >&2
