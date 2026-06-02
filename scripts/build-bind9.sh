#!/bin/sh
# build-bind9.sh — set up and build the BIND9-integrated MT benchmarks.
#
# Clones a clean upstream bind9 at a pinned commit (offline, --local) into
# $BIND9_SRC, overlays our tests/bench files (rendering meson.build from
# meson.build.in with our liburcu paths), then configures + builds only the
# benchmark executables.  Never modifies $BIND9_UPSTREAM or the userspace-rcu
# source tree.
#
# All inputs come from the environment (set by the Makefile from config.mk):
#   REPO            this repository's root
#   BIND9_UPSTREAM  bind9 git repo to clone from (its HEAD = the pinned commit)
#   BIND9_COMMIT    commit to check out
#   BIND9_SRC       where to create our clone + build
#   URCU_BUILD      our in-tree liburcu clone build (made by `make urcu`)
set -eu

: "${REPO:?set REPO}"
: "${BIND9_UPSTREAM:?set BIND9_UPSTREAM}"
: "${BIND9_COMMIT:?set BIND9_COMMIT}"
: "${BIND9_SRC:?set BIND9_SRC}"
: "${URCU_BUILD:?set URCU_BUILD}"

URCU_INC="$URCU_BUILD/include"
URCU_LIBDIR="$URCU_BUILD/src/.libs"
BUILD="$BIND9_SRC/build"
DEST="$BIND9_SRC/tests/bench"
OV="$REPO/bind9-overlay/tests/bench"

test -f "$URCU_LIBDIR/liburcu-cds.so" || {
	echo "ERROR: $URCU_LIBDIR/liburcu-cds.so not found — run 'make urcu' first." >&2
	exit 1
}

# 1. Clean clone at the pinned commit (offline).
if [ ! -d "$BIND9_SRC/.git" ]; then
	echo ">> cloning bind9 (offline, --local) into $BIND9_SRC"
	git clone --local --no-hardlinks "$BIND9_UPSTREAM" "$BIND9_SRC"
fi
echo ">> checking out $BIND9_COMMIT"
git -C "$BIND9_SRC" checkout -q "$BIND9_COMMIT"
if [ -f "$BIND9_SRC/.gitmodules" ]; then
	git -C "$BIND9_SRC" submodule update --init --recursive
fi

# 2. Overlay our tests/bench files + the vendored competitor sources.
echo ">> applying tests/bench overlay"
cp "$OV/load-names.c" "$OV/qpmulti_ft.c" "$DEST/"
# Per-engine read/write scaling benchmark: shared driver + one file per engine.
cp "$OV/bench_scale_common.h" "$OV/bench_scale_common.c" \
   "$OV/bench_scale_ft.c"   "$OV/bench_scale_judy.c" "$OV/bench_scale_qp.c" \
   "$OV/bench_scale_art.c"  "$OV/bench_scale_b9qp.c"  "$DEST/"
cp "$REPO/third_party/qp-trie/Tbl.c" "$REPO/third_party/qp-trie/Tbl.h" \
   "$REPO/third_party/qp-trie/qp.c"  "$REPO/third_party/qp-trie/qp.h"  \
   "$REPO/third_party/libart/art.c"  "$REPO/third_party/libart/art.h"  "$DEST/"
sed -e "s#@URCU_INC@#${URCU_INC}#g" \
    -e "s#@URCU_LIBDIR@#${URCU_LIBDIR}#g" \
    "$OV/meson.build.in" > "$DEST/meson.build"

# 3. Configure (release, matching the reference build) + build the benches.
if [ ! -d "$BUILD" ]; then
	echo ">> meson setup ($BUILD)"
	CFLAGS="-march=native -mpopcnt -msse4.2 ${CFLAGS:-}" \
		meson setup "$BUILD" "$BIND9_SRC" --buildtype=release
else
	echo ">> reconfiguring existing build dir"
	meson setup --reconfigure "$BUILD" "$BIND9_SRC" >/dev/null
fi

# load-names is the primary, up-to-date MT scaling test — it must build.
echo ">> building load-names (primary MT scaling test)"
ninja -C "$BUILD" tests/bench/load-names

# Per-engine read/write scaling benchmarks (one structure per process for
# isolated RSS).  These track the current fractal-trie-dev API.
echo ">> building per-engine read/write scaling benchmarks"
for b in bench_scale_ft bench_scale_ft_qsbr bench_scale_judy bench_scale_qp \
	 bench_scale_art bench_scale_b9qp; do
	ninja -C "$BUILD" "tests/bench/$b"
done

# qpmulti_ft still tracks an older FT API and may not build against the current
# fractal-trie-dev; attempt it but don't fail the target.
echo ">> attempting qpmulti_ft"
if ninja -C "$BUILD" tests/bench/qpmulti_ft >/dev/null 2>&1; then
	echo "  built:   qpmulti_ft"
else
	echo "  SKIPPED: qpmulti_ft (build failed — likely stale FT API; see README)"
fi

echo ""
echo "Built binaries under $BUILD/tests/bench/ :"
for b in load-names bench_scale_ft bench_scale_ft_qsbr bench_scale_judy \
	 bench_scale_qp bench_scale_art bench_scale_b9qp qpmulti_ft; do
	[ -x "$BUILD/tests/bench/$b" ] && echo "  $b"
done
echo ""
echo "Run with our liburcu on the library path, e.g.:"
echo "  LD_LIBRARY_PATH=$URCU_LIBDIR BENCH_THREADS='1 2 4 8' FT_PRIME=1 \\"
echo "    $BUILD/tests/bench/load-names $REPO/datasets/names-1M-shuf.csv"
echo ""
echo "Read/write scaling sweep (per-engine processes, combined table):"
echo "  sh $REPO/scripts/run_scale_rw.sh 16"
