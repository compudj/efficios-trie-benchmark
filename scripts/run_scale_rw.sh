#!/bin/sh
# run_scale_rw.sh — run each per-engine read/write scaling benchmark in its own
# process and assemble the combined table.
#
# Each engine (FT, Judy, qp-trie, ART, BIND9-QP) is a separate executable that
# builds only its own structure, so the RSS it reports is that structure's
# footprint in isolation (one process = one trie).  This script runs them all
# and merges their output into one throughput table plus a per-engine RSS row.
#
# Usage:
#   scripts/run_scale_rw.sh [max_threads]      # max_threads default 384
#
# Environment:
#   BENCH_DIR      directory holding the bench_scale_* executables
#                  (default: <repo>/bind9-src/build/tests/bench)
#   URCU_LIBDIR    liburcu .libs dir for LD_LIBRARY_PATH
#                  (default: <repo>/urcu-build/src/.libs)
#   ENGINES        space-separated subset to run
#                  (default: "ft judy qp art b9qp")
#   BENCH_NO_PRIME if set, disables the (default-on) cache-priming pass;
#                  passed through to each engine.
#   BENCH_NUMA_INTERLEAVE if set, each engine interleaves its keys + structure
#                  across all NUMA nodes (numa_set_interleave_mask); passed
#                  through to each engine.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BENCH_DIR=${BENCH_DIR:-$REPO/bind9-src/build/tests/bench}
URCU_LIBDIR=${URCU_LIBDIR:-$REPO/urcu-build/src/.libs}
ENGINES=${ENGINES:-ft judy qp art b9qp}
MAXT=${1:-384}

export LD_LIBRARY_PATH="$URCU_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -n "${BENCH_NO_PRIME:-}" ] && export BENCH_NO_PRIME
[ -n "${BENCH_NUMA_INTERLEAVE:-}" ] && export BENCH_NUMA_INTERLEAVE

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for e in $ENGINES; do
	exe="$BENCH_DIR/bench_scale_$e"
	# hotrowex (and any other non-bind9 MT engine) is built standalone into
	# the repo root by the top-level Makefile; fall back to it there.
	[ -x "$exe" ] || exe="$REPO/bench_scale_$e"
	if [ ! -x "$exe" ]; then
		echo "WARNING: $exe not found — skipping '$e'" >&2
		: > "$tmp/$e.out"
		continue
	fi
	echo ">> running $e (max_threads=$MAXT) ..." >&2
	if ! "$exe" "$MAXT" > "$tmp/$e.out" 2> "$tmp/$e.err"; then
		echo "WARNING: bench_scale_$e exited nonzero:" >&2
		sed 's/^/    /' "$tmp/$e.err" >&2 || true
	fi
done

awk -v engines="$ENGINES" -v tmpdir="$tmp" -v dur="3" '
BEGIN {
	n = split(engines, eng, " ")
	for (i = 1; i <= n; i++) {
		e = eng[i]; rss[e] = ""
		f = tmpdir "/" e ".out"
		while ((getline line < f) > 0) {
			ncol = split(line, a, " ")
			# format: "engine <name> rss_kb <value>"
			if (a[1] == "engine") { rss[e] = a[4]; continue }
			if (substr(a[1], 1, 1) == "#") continue
			if (ncol == 3 && a[1] ~ /^[0-9]+$/) {
				nt = a[1] + 0
				rd[e SUBSEP nt] = a[2]; wr[e SUBSEP nt] = a[3]
				seen[nt] = 1
			}
		}
		close(f)
	}

	printf "\nRead/Write scalability (1M DNS keys, 1 writer + N readers, %ss/point)\n\n", dur

	printf "RSS (MB): "
	for (i = 1; i <= n; i++) {
		e = eng[i]
		if (rss[e] == "") printf " %s=-", e
		else printf " %s=%.0f", e, rss[e] / 1024.0
	}
	printf "\n\n"

	printf "%-8s", "Readers"
	for (i = 1; i <= n; i++) printf " %9s %8s", eng[i] "_rd", eng[i] "_wr"
	printf "\n%-8s", ""
	for (i = 1; i <= n; i++) printf " %9s %8s", "(Mops/s)", "(Kops/s)"
	printf "\n"

	cnt = 0
	for (nt in seen) order[cnt++] = nt + 0
	for (x = 0; x < cnt; x++)
		for (y = x + 1; y < cnt; y++)
			if (order[y] < order[x]) { t = order[x]; order[x] = order[y]; order[y] = t }

	for (j = 0; j < cnt; j++) {
		nt = order[j]
		printf "%-8d", nt
		for (i = 1; i <= n; i++) {
			e = eng[i]
			r = ((e SUBSEP nt) in rd) ? rd[e SUBSEP nt] : "-"
			w = ((e SUBSEP nt) in wr) ? wr[e SUBSEP nt] : "-"
			printf " %9s %8s", r, w
		}
		printf "\n"
	}
}'
