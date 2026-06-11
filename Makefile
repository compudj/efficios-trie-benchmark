# efficios-trie-benchmark — benchmarks for the Fractal Trie (FT) vs competing
# trie implementations (qp-trie, libart/ART, Judy) and BIND9's QP-trie.
#
# Layout:
#   src/                single- and multi-threaded benchmark harnesses
#   third_party/qp-trie vendored Tony Finch qp-trie (CC0) — Tbl dispatch + qp backend
#   third_party/libart  vendored libart ART (BSD-2-Clause)
#   urcu-build/         our liburcu clone (fractal-trie-dev), built in-tree
#   config.mk           local paths / feature flags (edit this, not the Makefile)
#
# Quick start:
#   make urcu      # clone (or fetch) our FT checkout and build liburcu in-tree
#   make           # build the single-threaded benchmark
#   ./bench_one_st dns ft
#
# urcu-build/ is a git clone of userspace-rcu on the fractal-trie-dev branch,
# built in-tree.  `make urcu` clones it if absent, otherwise fetches + fast-
# forwards $(URCU_BRANCH) from $(URCU_UPSTREAM) and rebuilds.

include config.mk

# In-tree clone build: headers and libraries both live under $(URCU_BUILD).
URCU_INC := $(URCU_BUILD)/include
URCU_LIB := $(URCU_BUILD)/src/.libs

URCU_CPPFLAGS := -I$(URCU_INC)

# The harnesses define _GNU_SOURCE / _LGPL_SOURCE / the RCU flavor themselves,
# so we only add the FT feature flags and include paths here.
CPPFLAGS_COMMON := $(FT_FEATURES) $(URCU_CPPFLAGS) \
                   -Ithird_party/qp-trie -Ithird_party/libart

CFLAGS := $(OPTFLAGS) -Wall

LDFLAGS := -L$(URCU_LIB) -Wl,-rpath,$(URCU_LIB)
# Judy is a system library (libjudy-dev / Judy-devel).
LDLIBS  := -lurcu-qsbr -lurcu-cds -lurcu -lJudy -lpthread

# ---------------------------------------------------------------------------
# Competitor object files.
#
# qp-trie dispatches through Tbl.o, which must be linked with EXACTLY ONE
# backend object that implements the Tbl API (qp.o, fn.o, fp.o, ...).  They all
# export the same symbols, so the linker silently takes whichever you give it.
# To benchmark the *real* qp-trie we link qp.o.  Linking fn.o instead would
# quietly measure the "fn" naive-bitwise variant under the "qp" label — that is
# the trap the original build_one_st_bench.sh fell into.  Do not add fn.o here.
# ---------------------------------------------------------------------------
QP_OBJS  := third_party/qp-trie/Tbl.o third_party/qp-trie/qp.o
ART_OBJS := third_party/libart/art.o

# libart measurably benefits from -O3 (~-11% on string lookups, ~-18% on dense
# integers — its node-256 scan and path-compression loops unroll/vectorize),
# whereas qp/HOT/ART-OLC/FT all saturate at -O2.  So, like Cuckoo, it gets its
# own opt level instead of the global -O2 OPTFLAGS.  LTO adds nothing (single TU).
ART_CF   := -O3 -DNDEBUG -march=native -mpopcnt -Wall

BENCHES := bench_one_st

# Standalone scale engines (built into the repo root, no bind9 clone needed) and
# the separate GPL wormhole binary.  The bind9-built scale engines (ft, ft_qsbr,
# b9qp, art, judy, qp) require a bind9 checkout and stay under `make bind9`.
SCALE_BENCHES := bench_scale_hotrowex bench_scale_masstree \
                 bench_scale_artolc bench_scale_artrowex

.PHONY: all clean clean-urcu urcu check-urcu bind9 clean-bind9
all: $(BENCHES) $(SCALE_BENCHES) bench_wormhole_gpl

# HOT (Height Optimized Trie, third_party/hot, ISC): header-only C++14, compiled
# via a small extern "C" shim (src/bench_hot.cpp).  Needs AVX2+BMI2 (covered by
# OPTFLAGS' -march=native).  bench_one_st links it + libstdc++.
HOT_INC  := -Ithird_party/hot/single-threaded-include \
            -Ithird_party/hot/commons-include \
            -Ithird_party/hot/content-helpers-include \
            -Ithird_party/hot/utils-include
HOT_OBJS := src/bench_hot.o

src/bench_hot.o: src/bench_hot.cpp
	$(CXX) $(OPTFLAGS) -std=c++14 -w $(HOT_INC) -c -o $@ $<

# Cuckoo Trie (third_party/cuckoo-trie, Unlicense / public domain): a C library
# + a C shim (src/bench_cuckoo.c).  -D_GNU_SOURCE for MADV_HUGEPAGE in util.c's
# hugepage-mmap fallback; -march=native (in OPTFLAGS) covers its Haswell baseline.
CUCKOO_DIR  := third_party/cuckoo-trie
# Build with Cuckoo's own recommended flags (-O3 -flto -fno-strict-aliasing) so
# it is measured at its best — its lookup hot path benefits markedly from LTO.
# -D_GNU_SOURCE for MADV_HUGEPAGE in util.c's hugepage-mmap fallback;
# -march=native (>= its Haswell baseline) enables the bextr builtins it uses.
CUCKOO_CF   := -O3 -DNDEBUG -march=native -fno-strict-aliasing -flto \
               -std=gnu11 -w -D_GNU_SOURCE -I$(CUCKOO_DIR)
CUCKOO_OBJS := $(addprefix $(CUCKOO_DIR)/,main.o util.o verify_trie.o random.o \
               atomics.o mt_debug.o) src/bench_cuckoo.o

$(CUCKOO_DIR)/%.o: $(CUCKOO_DIR)/%.c
	$(CC) $(CUCKOO_CF) -c -o $@ $<
src/bench_cuckoo.o: src/bench_cuckoo.c
	$(CC) $(CUCKOO_CF) -c -o $@ $<

# Single-threaded, single-engine benchmark:
#   ft_* / judy / qp / art / hot / cuckoo / masstree / artolc.
# -flto at link lets the Cuckoo objects (compiled -flto) be optimized together.
# Masstree (MIT) and ART-OLC (Apache-2.0) are linked single-threaded via thin
# shims over their vendored sources (+ oneTBB for ART-OLC's epoch).  The masstree
# core / artolc Tree objects are built by the pattern rules in the bench_scale
# section below (MASSTREE_CXXFLAGS / ARTOLC_CXXFLAGS).
ST_MT_OBJS := third_party/masstree/compiler.o third_party/masstree/str.o \
              third_party/masstree/string.o third_party/masstree/straccum.o \
              third_party/masstree/kvthread.o src/bench_masstree_st.o \
              third_party/artolc/OptimisticLockCoupling/Tree.o src/bench_artolc_st.o

src/bench_masstree_st.o: src/bench_masstree_st.cpp
	$(CXX) $(MASSTREE_CXXFLAGS) -c -o $@ $<
src/bench_artolc_st.o: src/bench_artolc_st.cpp
	$(CXX) $(ARTOLC_CXXFLAGS) -c -o $@ $<

bench_one_st: src/bench_one_st.c $(QP_OBJS) $(ART_OBJS) $(HOT_OBJS) $(CUCKOO_OBJS) $(ST_MT_OBJS) | check-urcu
	$(CC) $(CFLAGS) $(CPPFLAGS_COMMON) -flto -o $@ $^ $(LDFLAGS) $(LDLIBS) -lstdc++ -lm -ltbb

# Vendored competitor sources: compile with the same opt flags, but only their
# own include dir (they are independent of urcu/FT).
third_party/qp-trie/%.o: third_party/qp-trie/%.c
	$(CC) $(CFLAGS) -std=gnu99 -Ithird_party/qp-trie -c -o $@ $<

third_party/libart/%.o: third_party/libart/%.c
	$(CC) $(ART_CF) -Ithird_party/libart -c -o $@ $<

# ---------------------------------------------------------------------------
# Wormhole (GPL-3.0) — SEPARATE, GPL-licensed binary.
#
# Wormhole (third_party/wormhole/) is GPL-3.0, so it is built into its OWN
# executable and is NEVER linked into bench_one_st (which stays permissively
# licensed — that is the whole point of keeping it separate).  The resulting
# bench_wormhole_gpl binary is therefore GPL-3.0.  It is intentionally NOT part
# of `all`; build it explicitly for the Wormhole datapoint:
#     make bench_wormhole_gpl
#     ./bench_wormhole_gpl        # output: <ns/op> <RSS_kB> on the dns key set
# ---------------------------------------------------------------------------
WORMHOLE_SRC := third_party/wormhole/wh.c third_party/wormhole/lib.c \
                third_party/wormhole/kv.c
bench_wormhole_gpl: src/bench_wormhole_gpl.c $(WORMHOLE_SRC)
	$(CC) $(CFLAGS) -w -Ithird_party/wormhole -o $@ $^ -lpthread -lm

# ---------------------------------------------------------------------------
# HOTRowex MT engine — concurrent (ROWEX) HOT for the read/write scaling sweep.
#
# Self-contained: links neither bind9 nor liburcu, only the header-only HOT
# (ISC) + oneTBB (libtbb-dev, for HOT's epoch-based reclamation).  It shares
# the engine-agnostic bench_scale_common.c driver with the bind9-built MT
# engines but is compiled here standalone into the repo root, so the sweep
# script finds it next to the rest.  ROWEX has no delete: its writer churns by
# upsert (see src/bench_scale_hotrowex.cpp).  Needs AVX2+BMI2 (covered by
# OPTFLAGS' -march=native).  Not part of `all` (extra TBB dep); build explicitly:
#     make bench_scale_hotrowex
#     ENGINES="ft hotrowex" scripts/run_scale_rw.sh   # ROWEX-vs-RCU comparison
# ---------------------------------------------------------------------------
SCALE_COMMON_SRC := bind9-overlay/tests/bench/bench_scale_common.c
HOTROWEX_INC := -Ithird_party/hot/rowex-include \
                -Ithird_party/hot/single-threaded-include \
                -Ithird_party/hot/commons-include \
                -Ithird_party/hot/content-helpers-include \
                -Ithird_party/hot/utils-include \
                -Ibind9-overlay/tests/bench

src/bench_scale_hotrowex.o: src/bench_scale_hotrowex.cpp
	$(CXX) $(OPTFLAGS) -std=c++14 -w $(HOTROWEX_INC) -c -o $@ $<
# Compiled as C (gcc), not as C++ (g++ would treat the .c source as C++).
bench_scale_common.o: $(SCALE_COMMON_SRC)
	$(CC) $(CFLAGS) -Ibind9-overlay/tests/bench -c -o $@ $<
bench_scale_hotrowex: src/bench_scale_hotrowex.o bench_scale_common.o
	$(CXX) $(OPTFLAGS) -o $@ $^ -ltbb -lpthread -lnuma

# ---------------------------------------------------------------------------
# Masstree MT engine — concurrent B+tree-of-tries (kohler/masstree-beta, MIT)
# for the read/write scaling sweep.  Self-contained (no bind9/liburcu): compiles
# the vendored Masstree core sources (config.h generated by its ./configure was
# vendored too) + the engine with g++.  Force-includes config.h, as Masstree's
# headers require.  Not part of `all`; build explicitly:
#     make bench_scale_masstree
#     ENGINES="ft masstree hotrowex" scripts/run_scale_rw.sh 192
# ---------------------------------------------------------------------------
MASSTREE_DIR := third_party/masstree
# Masstree gains a marginal ~3-4% on dense-integer lookups at -O3 (the rest is
# flat), so it uses -O3 rather than the global -O2 OPTFLAGS (like libart / Cuckoo).
MASSTREE_CXXFLAGS := -O3 -DNDEBUG -march=native -mpopcnt -std=gnu++14 -w \
                     -I$(MASSTREE_DIR) -include $(MASSTREE_DIR)/config.h
MASSTREE_OBJS := $(addprefix $(MASSTREE_DIR)/, \
                 compiler.o str.o string.o straccum.o kvthread.o)

$(MASSTREE_DIR)/%.o: $(MASSTREE_DIR)/%.cc
	$(CXX) $(MASSTREE_CXXFLAGS) -c -o $@ $<
src/bench_scale_masstree.o: src/bench_scale_masstree.cpp
	$(CXX) $(MASSTREE_CXXFLAGS) -Ibind9-overlay/tests/bench -c -o $@ $<
bench_scale_masstree: src/bench_scale_masstree.o $(MASSTREE_OBJS) bench_scale_common.o
	$(CXX) $(OPTFLAGS) -o $@ $^ -lpthread -lnuma

# ---------------------------------------------------------------------------
# ART-OLC MT engine — concurrent adaptive radix tree, Optimistic Lock Coupling
# (flode/ARTSynchronized, Apache-2.0) for the read/write scaling sweep.  Needs
# oneTBB (its epoch reclamation uses tbb::enumerable_thread_specific).  The
# vendored sources are a unity build: OptimisticLockCoupling/Tree.cpp #includes
# N.cpp (-> N4/N16/N48/N256.cpp) and Epoche.cpp, so only Tree.cpp is compiled.
# Not in `all`; build explicitly:
#     make bench_scale_artolc
#     ENGINES="ft hotrowex masstree artolc" scripts/run_scale_rw.sh 192
# ---------------------------------------------------------------------------
ARTOLC_DIR := third_party/artolc
ARTOLC_CXXFLAGS := $(OPTFLAGS) -std=c++14 -w -I$(ARTOLC_DIR)
ARTOLC_OBJS := $(ARTOLC_DIR)/OptimisticLockCoupling/Tree.o

$(ARTOLC_DIR)/OptimisticLockCoupling/Tree.o: $(ARTOLC_DIR)/OptimisticLockCoupling/Tree.cpp
	$(CXX) $(ARTOLC_CXXFLAGS) -c -o $@ $<
src/bench_scale_artolc.o: src/bench_scale_artolc.cpp
	$(CXX) $(ARTOLC_CXXFLAGS) -Ibind9-overlay/tests/bench -c -o $@ $<
bench_scale_artolc: src/bench_scale_artolc.o $(ARTOLC_OBJS) bench_scale_common.o
	$(CXX) $(OPTFLAGS) -o $@ $^ -ltbb -lpthread -lnuma

# ART-ROWEX: same vendored repo (third_party/artolc), the Read-Optimized Write
# EXclusion variant.  Unity build like OLC: ROWEX/Tree.cpp #includes N.cpp
# (-> N4/N16/N48/N256.cpp) and -- unless ART_ROWEX_SKIP_EPOCHE -- Epoche.cpp.
# Standalone here, so it compiles its own Epoche (no -DART_ROWEX_SKIP_EPOCHE).
#     make bench_scale_artrowex
#     ENGINES="ft artolc artrowex" scripts/run_scale_rw.sh 192
ARTROWEX_OBJS := $(ARTOLC_DIR)/ROWEX/Tree.o

$(ARTOLC_DIR)/ROWEX/Tree.o: $(ARTOLC_DIR)/ROWEX/Tree.cpp
	$(CXX) $(ARTOLC_CXXFLAGS) -c -o $@ $<
src/bench_scale_artrowex.o: src/bench_scale_artrowex.cpp
	$(CXX) $(ARTOLC_CXXFLAGS) -Ibind9-overlay/tests/bench -c -o $@ $<
bench_scale_artrowex: src/bench_scale_artrowex.o $(ARTROWEX_OBJS) bench_scale_common.o
	$(CXX) $(OPTFLAGS) -o $@ $^ -ltbb -lpthread -lnuma

# ---------------------------------------------------------------------------
# Our Fractal Trie checkout: a git clone of $(URCU_UPSTREAM) on $(URCU_BRANCH),
# built in-tree under $(URCU_BUILD).  Clones if absent, otherwise fetches and
# fast-forwards the branch, then (re)bootstraps/configures as needed and builds
# the libraries.  Re-run `make urcu` to pull the latest FT and rebuild.
# ---------------------------------------------------------------------------
urcu:
	@if [ ! -d "$(URCU_BUILD)/.git" ]; then \
	  echo ">> cloning $(URCU_UPSTREAM) -> $(URCU_BUILD)"; \
	  git clone --no-hardlinks "$(URCU_UPSTREAM)" "$(URCU_BUILD)"; \
	else \
	  echo ">> fetching $(URCU_BRANCH) from origin"; \
	  git -C "$(URCU_BUILD)" fetch origin "$(URCU_BRANCH)"; \
	fi
	git -C "$(URCU_BUILD)" checkout "$(URCU_BRANCH)"
	git -C "$(URCU_BUILD)" merge --ff-only "origin/$(URCU_BRANCH)"
	@test -x "$(URCU_BUILD)/configure" || ( cd "$(URCU_BUILD)" && ./bootstrap )
	@test -f "$(URCU_BUILD)/config.status" || \
	  ( cd "$(URCU_BUILD)" && CFLAGS="$(URCU_CFLAGS)" ./configure )
	$(MAKE) -C "$(URCU_BUILD)/src"

clean-urcu:
	rm -rf "$(URCU_BUILD)"

# ---------------------------------------------------------------------------
# Multithreaded bind9 benchmarks: load-names (FT_PRIME priming MT scaling),
# qpmulti_ft, and bench_scale_rw_bind9.  Builds inside a clean bind9 clone with
# our tests/bench overlay; links our own liburcu (run `make urcu` first).
# Binaries land in $(BIND9_SRC)/build/tests/bench/.
# ---------------------------------------------------------------------------
bind9: | check-urcu
	REPO="$(CURDIR)" \
	BIND9_UPSTREAM="$(BIND9_UPSTREAM)" \
	BIND9_COMMIT="$(BIND9_COMMIT)" \
	BIND9_SRC="$(BIND9_SRC)" \
	URCU_BUILD="$(URCU_BUILD)" \
	sh scripts/build-bind9.sh

clean-bind9:
	rm -rf "$(BIND9_SRC)"

check-urcu:
	@test -f "$(URCU_LIB)/liburcu-cds.so" || { \
	  echo "ERROR: liburcu-cds not found under $(URCU_LIB)"; \
	  echo "       Run 'make urcu' to clone + build our liburcu (the Fractal Trie)."; \
	  exit 1; }

clean:
	rm -f $(BENCHES) $(QP_OBJS) $(ART_OBJS)
