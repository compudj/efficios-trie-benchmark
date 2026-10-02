#!/bin/bash
# run_sweep.sh -- call_rcu() caller cost, wfcq vs rseq local queue, interleaved.
#
# One binary (linked against a liburcu built --enable-call-rcu-rseq), two arms
# chosen at run time: URCU_CALL_RCU_RSEQ=0 (wfcq) and default (rseq).  Plus the
# --op none arm per dirty level: the dirty stores alone, to subtract.  Every
# (P, dirty) point runs its arms back to back, RUNS times, rotating which arm
# goes first, so drift lands on every arm alike.
#
# Output: CSV on stdout-file $CSV:
#   arm,workers,producers,dirty,run,mops_s,ns_op,mean_ns_op,p90_ns_op,local_frac
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-$HERE/bench_call_rcu_cbrseq}
CSV=${CSV:-$HERE/callrcu.csv}
JE=${JE:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
DUR=${DUR:-500}
RUNS=${RUNS:-3}
PTS=${PTS:-"1 8 48 96 192"}
DIRTY=${DIRTY:-"0 2 8"}
WORKERS=${WORKERS:-rt}		# rt | nonrt
CL=$(hwloc-calc --li --po -I PU core:all.pu:0)
NCORE=$(tr ',' '\n' <<< "$CL" | wc -l)

field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if($i==L){print $(i+1);exit}}' <<< "$1"; }
[[ -f "$CSV" ]] || echo "arm,workers,producers,dirty,run,mops_s,ns_op,mean_ns_op,p90_ns_op,local_frac" > "$CSV"
nr=""; [[ "$WORKERS" == nonrt ]] && nr="--nonrt"

one() {	# one <arm> <P> <dirty> <run>
	local arm=$1 p=$2 d=$3 r=$4 out env=() op=call_rcu
	case $arm in
	wfcq) env=(URCU_CALL_RCU_RSEQ=0) ;;
	rseq) ;;
	none) op=none ;;
	esac
	out=$(env "${env[@]}" LD_PRELOAD="$JE" "$BIN" --producers "$p" --cpulist "$CL" \
		--duration "$DUR" --dirty "$d" --op "$op" $nr)
	local lf; lf=$(field "$out" local_frac:)
	if [[ $arm == rseq && $lf != 1.0000 ]]; then
		echo "!! rseq arm P=$p d=$d local_frac=$lf (not all local)" >&2
	fi
	echo "$arm,$WORKERS,$p,$d,$r,$(field "$out" Mops/s:),$(field "$out" ns/op:),$(field "$out" mean_ns/op:),$(field "$out" p90_ns/op:),$lf" >> "$CSV"
	printf "  %-5s %-5s P=%-4s d=%-2s r=%s  %8s Mops/s  %6s ns/op  (mean %6s, p90 %6s)  local %s\n" \
		"$arm" "$WORKERS" "$p" "$d" "$r" "$(field "$out" Mops/s:)" "$(field "$out" ns/op:)" \
		"$(field "$out" mean_ns/op:)" "$(field "$out" p90_ns/op:)" "$lf" >&2
}

for p in $PTS; do
	(( p > NCORE )) && continue
	for d in $DIRTY; do
		arms=(wfcq rseq); (( d > 0 )) && arms+=(none)
		for r in $(seq 1 "$RUNS"); do
			n=${#arms[@]}
			for i in $(seq 0 $((n - 1))); do
				one "${arms[$(( (i + r) % n ))]}" "$p" "$d" "$r"
			done
		done
	done
done
echo ">> DONE -> $CSV" >&2
