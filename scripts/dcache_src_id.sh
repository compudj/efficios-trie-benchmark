#!/bin/bash
# dcache_src_id.sh [liburcu-build-dir] -- print the provenance id every dcache
# sweep stamps on each CSV row it writes, and check_dcache_figures.sh compares
# against: <source hash>-<liburcu commit>.
#
#   source hash    sha1 of the dcache engines, their headers, the bench
#                  harnesses and the Makefile -- everything that decides what a
#                  run measures -- first 12 hex digits
#   liburcu commit HEAD of the liburcu build the run links (the default build
#                  unless a route build is given)
#
# Rows stamped with an older id were measured with code that no longer exists.
# Stamping ROWS, not files, is what makes a partial re-run (PANELS=...)
# honest: the panels it did not re-run keep their old id.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || exit 2
BD=${1:-$REPO/urcu-txn-build}
D=$REPO/experiments/dcache

src=$(cd "$D" && ls dcache_*.c dcache*.h seqcount.h bench_dcache*.c Makefile \
	krwsem/*.c krwsem/*.h 2>/dev/null | sort | xargs cat | sha1sum | cut -c1-12)
urcu=$(git -C "$BD" log -1 --format=%h 2>/dev/null || echo unknown)
echo "$src-$urcu"
