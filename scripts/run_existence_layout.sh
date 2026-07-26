#!/bin/bash
# run_existence_layout.sh -- what PER-ELEMENT LAYOUT costs the reader, measured
# on one algorithm.
#
# Three builds of existence_list that differ ONLY in where the per-element state
# sits.  Same mechanism throughout -- same tagged-pointer test, same
# single-store publish, same acquire/release pairing -- so the spread between
# them is layout and nothing else:
#
#   perfbook  existence_head embedded as perfbook ships it.  136 B element
#             spanning 3 cachelines; existence_head at offset 16 pushes `key`
#             out to offset 128, so the reader touches TWO lines per node
#             (list+eh_egi in line 0, key in line 2).
#   packed    same 136 B struct, hot fields reordered so list, key and eh_egi
#             share line 0.  ONE line per node.  Identical size, identical
#             semantics -- only the order differs.
#   split     hot/cold split: only existence's tagged word rides in the
#             traversed element (32 B, one cacheline, every byte read); the
#             104 B of writer-only state -- eh_list, eh_rh, the three
#             callbacks, eh_gone, eh_lock -- is segregated.  This is the
#             canonical arm: we compare the existence CONCEPT, not a struct.
#
# txn_sw_list is measured from the same binary as the reference floor: 24 B
# element, no per-element state at all.
#
# Read ceiling only, NO WRITER -- layout is a read-side property and a writer
# would add reclamation noise that has nothing to do with it.
#
# Writes scripts/existence_layout.csv:
#   layout,engine,run,readers,read_mvisits,viol
set -u
cd /home/efficios/git/efficios-trie-benchmark
export DURATION_SEC=${DURATION_SEC:-3}
export LIST_SIZE=${LIST_SIZE:-10000} CHURN=${CHURN:-200}
RUNS=${RUNS:-2}
MAXT=${MAXT:-192}
OUT=scripts/existence_layout.csv

echo "layout,engine,run,readers,read_mvisits,viol" > "$OUT"

for conf in perfbook packed split; do
  # FORCED CLEAN, and it is mandatory: EXIST_CONF changes only -D flags, so it
  # touches no file, so make finds bench_list_scale newer than every
  # prerequisite and does nothing.  Without this the three "layouts" are three
  # runs of one binary -- which is exactly what an earlier version of this
  # script measured, producing three identical columns.
  echo ">> building EXIST_CONF=$conf (forced clean) ..." >&2
  rm -f bench_list_scale src/bench_existence_list.o
  make bench_list_scale JEMALLOC=1 EXIST_CONF="$conf" >/dev/null 2>&1 \
    || { echo "BUILD FAILED ($conf)" >&2; exit 1; }
  # Sanity: the three layouts must not produce identical columns.  If they do,
  # the rebuild silently did not happen and the run is worthless -- checked
  # after the sweep, below.  (`pahole -C ex_lnode` on the object shows the
  # element size directly, 136 / 136 / 32 B, but needs a -g build.)
  for r in $(seq 1 "$RUNS"); do
    echo ">> $conf existence_list run=$r" >&2
    BENCH_NO_WRITER=1 ./bench_list_scale existence_list "$MAXT" 2>/dev/null \
      | awk -v C="$conf" -v R="$r" '/^[0-9]/{print C",existence_list,"R","$1","$2","$4}' >> "$OUT"
  done
  # Reference floor, from this same binary (unaffected by EXIST_CONF, but
  # recorded per build so any drift between builds is visible rather than
  # hidden).
  for r in $(seq 1 "$RUNS"); do
    echo ">> $conf txn_sw_list run=$r" >&2
    BENCH_NO_WRITER=1 ./bench_list_scale txn_sw_list "$MAXT" 2>/dev/null \
      | awk -v C="$conf" -v R="$r" '/^[0-9]/{print C",txn_sw_list,"R","$1","$2","$4}' >> "$OUT"
  done
done

# Leave the tree on the canonical build.
make bench_list_scale JEMALLOC=1 >/dev/null 2>&1

echo ">> done -> $OUT" >&2
if awk -F, 'NR>1 && $6+0>0{f=1} END{exit f?0:1}' "$OUT"; then
  echo "ERROR: nonzero coherence violations in $OUT" >&2
  exit 1
fi
# Guard against the failure this script was first written with: if EXIST_CONF
# did not actually reach the compiler, all three layouts are one binary and the
# columns come out identical.  They must differ -- perfbook is ~2x below packed.
awk -F, 'NR>1 && $2=="existence_list" && $4==192 {s[$1]=s[$1]" "$5}
	 END {
		n=0; for (k in s) { split(s[k],v," "); m[n++]=(v[1]+v[2])/2 }
		lo=m[0]; hi=m[0]
		for (i=1;i<n;i++) { if (m[i]<lo) lo=m[i]; if (m[i]>hi) hi=m[i] }
		if (n<3 || hi/lo < 1.5) {
			print "ERROR: layouts differ by only " hi/lo "x -- EXIST_CONF" \
			      " almost certainly did not reach the compiler" > "/dev/stderr"
			exit 1
		}
		printf "layout spread at 192 readers: %.2fx\n", hi/lo > "/dev/stderr"
	 }' "$OUT" || exit 1

echo "# ALL DONE, zero coherence violations -> $OUT" >&2
