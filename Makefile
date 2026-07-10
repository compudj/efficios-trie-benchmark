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

.PHONY: all clean clean-urcu urcu check-urcu bind9 clean-bind9 \
        urcu-txn check-urcu-txn check-urcu-txn-lib clean-urcu-txn \
        existence_3skiplist_uperf
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

# ---------------------------------------------------------------------------
# Bidirectional-list scaling benchmark (bench_list_scale).
#
# Compares the NEW userspace-rcu bidirectional RCU lists (single-updater +
# concurrent, from the urcu-txn-dev branch) against lock/seqlock lists at
# scale.  This is the shared urcu-txn ENGINE build (rcu-mcas / rcu-txn, the
# per-record proxy-tag API): bench_list_scale and any other txn-engine consumer
# link it.  It is a reproducible in-tree clone+build, urcu-txn-build/, DERIVED
# from the local reference tree URCU_TXN_UPSTREAM @ urcu-txn-dev (analogous to
# urcu-build/ for the FT).  Run `make urcu-txn` first, then the consumer target
# (e.g. `make bench_list_scale`).  `make urcu-txn` re-fetches + ff-merges to
# keep the clone current with the reference tree.
#
# Engines: txn_sw_list txn_list rculist mutex fairmutex rwlock_r rwlock_w iscrw
# seqlock.  The iscrw engine links bind9's real isc_rwlock (C-RW-WP) from
# bind9-src/lib/isc/rwlock.c, compiled standalone with a no-op probes shim
# (src/iscrw-shim/) + an isolation wrapper (src/bench_iscrw.c) so the isc/
# macros never reach the main TU and no libisc constructor runs.  `make bind9`
# is what populates bind9-src/ (only the isc headers + rwlock.c are needed).
# ---------------------------------------------------------------------------
# Reference tree we derive the engine build from (offline --local clone).  This
# is the canonical txn engine tree (urcu-txn-dev); override to a URL/other path
# if needed.  Kept in sync via `make urcu-txn`.
URCU_TXN_UPSTREAM ?= /home/efficios/git/userspace-rcu-txn
URCU_TXN_BRANCH   ?= urcu-txn-dev
URCU_TXN_BUILD    ?= $(CURDIR)/urcu-txn-build
URCU_TXN_INC      := $(URCU_TXN_BUILD)/include
URCU_TXN_LIB      := $(URCU_TXN_BUILD)/src/.libs

ISC_INC  := bind9-src/lib/isc/include
ISC_SHIM := src/iscrw-shim
TOPO_DIR := bind9-overlay/tests/bench

LIST_CFLAGS := $(OPTFLAGS) -pthread -Wall
HWLOC_CFLAGS := $(shell pkg-config --cflags hwloc)
HWLOC_LIBS   := $(shell pkg-config --libs hwloc)

# Reference Read-Log-Update (third_party/rlu), an added bench_list_scale engine.
# RLU_DEFS MUST be identical for rlu.o and bench_list_scale.o: both see
# sizeof(rlu_thread_data_t), so a mismatch is an ABI break.  The sweep reaches
# 192 threads (>128) and our commits touch 2-3 tiny nodes (default 20 MB/thread
# of write-set buffers is wasteful) -- see third_party/rlu/PROVENANCE.txt.
RLU_DIR  := third_party/rlu
RLU_DEFS := -DRLU_MAX_THREADS=256 -DRLU_MAX_WRITE_SET_BUFFER_SIZE=8192

# ── Pooled-build knobs (reproduce the write-optimised profiling config) ───────
# The default build is plain glibc malloc + non-inline rcu_head -- the FAIR READ
# build (smaller nodes read ~1.6x faster).  These opt in to the write side of
# the RLU comparison's "pooled" config, which is NOT the default because each
# has a read-side or dependency cost:
#   make bench_list_scale JEMALLOC=1          # link jemalloc, bake percpu_arena
#   make bench_list_scale PCPU=1              # per-CPU node slab + inline rcu_head
#   make bench_list_scale JEMALLOC=1 PCPU=1   # full pooled build
# JEMALLOC moves the per-op MCAS descriptor off glibc's arena mprotect/mmap_lock
# (~2x writer throughput at scale; needs libjemalloc-dev).  PCPU recycles
# txn_list churn nodes per-CPU but forces inline rcu_head (=> slower reads), and
# is ON by default in that binary (BENCH_NO_PCPU_ALLOC opts back to glibc for an
# in-binary A/B).  Both apply ONLY to bench_list_scale.o -- neither touches the
# rlu.o ABI (that is fixed by RLU_DEFS alone).  See the RLU-engine commit.
LIST_POOL_CFLAGS :=
LIST_POOL_LIBS   :=
ifeq ($(JEMALLOC),1)
  LIST_POOL_CFLAGS += -DBENCH_JEMALLOC
  LIST_POOL_LIBS   += $(shell pkg-config --libs jemalloc 2>/dev/null || echo -ljemalloc)
  LIST_JE_CHECK    := check-jemalloc
endif
ifeq ($(PCPU),1)
  LIST_POOL_CFLAGS += -DLIST_RCU_INLINE_RCU_HEAD -DBENCH_PCPU_ALLOC_DEFAULT
endif
# Optimistic-retry budget before a starved txn escalates into the serialized
# fair lane (urcu/rcu-txn.h URCU_TXN_FALLBACK, default 64).  Higher = stay on the
# parallel MCAS/priority path longer before serializing.  For collapse studies.
ifdef TXN_FALLBACK
  LIST_POOL_CFLAGS += -DURCU_TXN_FALLBACK=$(TXN_FALLBACK)
endif

urcu-txn:
	@if [ ! -d "$(URCU_TXN_BUILD)/.git" ]; then \
	  echo ">> cloning $(URCU_TXN_UPSTREAM) ($(URCU_TXN_BRANCH)) -> $(URCU_TXN_BUILD)"; \
	  git clone --no-hardlinks --branch "$(URCU_TXN_BRANCH)" --single-branch "$(URCU_TXN_UPSTREAM)" "$(URCU_TXN_BUILD)"; \
	else \
	  echo ">> fetching $(URCU_TXN_BRANCH)"; \
	  git -C "$(URCU_TXN_BUILD)" fetch origin "$(URCU_TXN_BRANCH)" && \
	  git -C "$(URCU_TXN_BUILD)" checkout "$(URCU_TXN_BRANCH)" && \
	  git -C "$(URCU_TXN_BUILD)" merge --ff-only "origin/$(URCU_TXN_BRANCH)"; \
	fi
	@test -x "$(URCU_TXN_BUILD)/configure" || ( cd "$(URCU_TXN_BUILD)" && ./bootstrap )
	@test -f "$(URCU_TXN_BUILD)/config.status" || \
	  ( cd "$(URCU_TXN_BUILD)" && CFLAGS="$(URCU_CFLAGS)" ./configure )
	$(MAKE) -C "$(URCU_TXN_BUILD)/src"

check-urcu-txn:
	@test -f "$(URCU_TXN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: liburcu (urcu-txn engine) not found under $(URCU_TXN_LIB)"; \
	  echo "       Run 'make urcu-txn' to clone + build it."; exit 1; }
	@test -f "$(ISC_INC)/isc/rwlock.h" || { \
	  echo "ERROR: bind9 isc headers not found under $(ISC_INC)"; \
	  echo "       Run 'make bind9' to populate bind9-src/ (for the iscrw engine)."; exit 1; }

check-jemalloc:
	@pkg-config --exists jemalloc 2>/dev/null || ldconfig -p 2>/dev/null | grep -qi 'libjemalloc' || { \
	  echo "ERROR: JEMALLOC=1 but libjemalloc was not found."; \
	  echo "       Install it (e.g. 'apt install libjemalloc-dev') or drop JEMALLOC=1."; exit 1; }

bench_list_scale: src/bench_list_scale.c src/bench_iscrw.c $(RLU_DIR)/rlu.c \
		bind9-src/lib/isc/rwlock.c $(TOPO_DIR)/bench_topology.c | check-urcu-txn $(LIST_JE_CHECK)
	$(CC) $(LIST_CFLAGS) $(RLU_DEFS) $(LIST_POOL_CFLAGS) -I$(URCU_TXN_INC) -I$(RLU_DIR) -I$(TOPO_DIR) \
	  -c src/bench_list_scale.c -o src/bench_list_scale.o
	$(CC) $(LIST_CFLAGS) $(RLU_DEFS) -I$(RLU_DIR) \
	  -c $(RLU_DIR)/rlu.c -o src/rlu.o
	$(CC) $(LIST_CFLAGS) -I$(ISC_SHIM) -I$(ISC_INC) \
	  -c src/bench_iscrw.c -o src/bench_iscrw.o
	$(CC) $(LIST_CFLAGS) -I$(ISC_SHIM) -I$(ISC_INC) \
	  -c bind9-src/lib/isc/rwlock.c -o src/iscrw.o
	$(CC) $(LIST_CFLAGS) $(HWLOC_CFLAGS) -I$(TOPO_DIR) \
	  -c $(TOPO_DIR)/bench_topology.c -o src/bench_topology_list.o
	$(CC) -O2 -pthread -o $@ src/bench_list_scale.o src/rlu.o src/bench_iscrw.o \
	  src/iscrw.o src/bench_topology_list.o $(LIST_POOL_LIBS) \
	  -L$(URCU_TXN_LIB) -Wl,-rpath,$(URCU_TXN_LIB) \
	  -lurcu-qsbr -lurcu-cds -lurcu-common $(HWLOC_LIBS) -lnuma -lpthread

# ---------------------------------------------------------------------------
# bench_txn_3hash: urcu-txn (rcu-mcas) analogue of perfbook's existence 3-hash
# atomic-move microbench (perfbook/datastruct/existence/existence_3hash_uperf.c),
# for the urcu-txn-vs-existence comparison.  Links the urcu-txn engine build
# (run `make urcu-txn` first), like bench_list_scale, but needs no bind9/RLU/
# topology deps -- hence check-urcu-txn-lib (lib only) rather than check-urcu-txn.
# See design/txn-vs-existence-3hash.md.  The bench defines _GNU_SOURCE/_LGPL_SOURCE
# and the QSBR flavor itself.
# ---------------------------------------------------------------------------
TXN3_CFLAGS := -O2 -pthread -Wall

check-urcu-txn-lib:
	@test -f "$(URCU_TXN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: liburcu (urcu-txn engine) not found under $(URCU_TXN_LIB)"; \
	  echo "       Run 'make urcu-txn' to clone + build it."; exit 1; }

bench_txn_3hash: src/bench_txn_3hash.c | check-urcu-txn-lib
	$(CC) $(TXN3_CFLAGS) -I$(URCU_TXN_INC) -c src/bench_txn_3hash.c \
	  -o src/bench_txn_3hash.o
	$(CC) -O2 -pthread -o $@ src/bench_txn_3hash.o \
	  -L$(URCU_TXN_LIB) -Wl,-rpath,$(URCU_TXN_LIB) \
	  -lurcu-qsbr -lurcu-cds -lurcu-common -lpthread

# ---------------------------------------------------------------------------
# bench_txn_3skiplist: the ORDERED-map dual of bench_txn_3hash -- three
# urcu_txn_skiplists instead of three hlist tables, matching perfbook's
# existence_3skiplist_uperf knobs and its ns/key-move metric.  A key move is one
# composed skiplist_del_prepare(src) + skiplist_insert_prepare(dst) committed as
# a single txn.  --movesper composes N moves per commit; N != 1 requires the
# engine's read-your-own-writes + chained same-slot stores (urcu_txn_enable_ryw,
# on by default there), because two batched ordered edits collide on one
# pred->next[L] slot -- see the bench's header comment for the mechanism and
# --ryw for the A/B.  movesper 1 is the like-for-like unit against existence's
# per-object flip.  Needs an urcu-txn-build synced past the RYW commits
# (rcu-txn.h urcu_txn_enable_ryw); run `make urcu-txn`.
#
# Header-only skiplist over rcu-txn: no cds_* symbols, hence no -lurcu-cds.
# See design/rcu-txn-skiplist.md and design/txn-vs-existence-3hash.md.
# ---------------------------------------------------------------------------
bench_txn_3skiplist: src/bench_txn_3skiplist.c | check-urcu-txn-lib
	$(CC) $(TXN3_CFLAGS) -I$(URCU_TXN_INC) -c src/bench_txn_3skiplist.c \
	  -o src/bench_txn_3skiplist.o
	$(CC) -O2 -pthread -o $@ src/bench_txn_3skiplist.o \
	  -L$(URCU_TXN_LIB) -Wl,-rpath,$(URCU_TXN_LIB) \
	  -lurcu-qsbr -lurcu-common -lpthread

# ---------------------------------------------------------------------------
# existence_3skiplist_uperf: the perfbook side of the ordered comparison.
#
# Built HERE rather than by perfbook's own Makefile because the comparison needs
# -DSL_XORSHIFT_LEVEL: both skiplists must draw tower heights from the same
# generator, or the figure is partly a plot of Park-Miller versus xorshift.  That
# flag is not in perfbook/datastruct/existence/Makefile, and a plain `make` there
# silently produces a binary the sweep must not use -- so the sweep script depends
# on this target instead of on the binary's existence.
#
# Requires the seeding fix (perfbook: seed the per-thread PRNG ...): unseeded,
# random_level() returns SL_MAX_LEVELS-1 forever and the "skiplist" is a sorted
# linked list.  See scripts/run_txn_vs_existence_skiplist.sh.
# ---------------------------------------------------------------------------
EXISTENCE_DIR := perfbook/datastruct/existence
existence_3skiplist_uperf: $(EXISTENCE_DIR)/existence_3skiplist_uperf.c \
                           perfbook/datastruct/skiplist/skiplist.h
	@# GCC_ARGS is invisible to make's timestamp check: a binary left behind by a
	@# plain `make` in $(EXISTENCE_DIR) looks up to date but lacks the flag.  Force.
	rm -f $(EXISTENCE_DIR)/existence_3skiplist_uperf
	$(MAKE) -C $(EXISTENCE_DIR) existence_3skiplist_uperf \
	  GCC_ARGS="-g -O3 -Wall -DSL_XORSHIFT_LEVEL"
	@# Verify the flag took.  Probe the __thread state skiplist.h defines under it;
	@# do NOT probe for 0x9e3779b97f4a7c15 -- existence_3skiplist_uperf.c seeds its
	@# reader threads with the same golden-ratio constant, so that always matches.
	@nm $(EXISTENCE_DIR)/existence_3skiplist_uperf | grep -q sl_rng_state \
	  || { echo "ERROR: -DSL_XORSHIFT_LEVEL did not take"; exit 1; }

clean-urcu-txn:
	rm -rf "$(URCU_TXN_BUILD)"

clean:
	rm -f $(BENCHES) $(QP_OBJS) $(ART_OBJS) bench_list_scale \
	  bench_txn_3hash src/bench_txn_3hash.o \
	  bench_txn_3skiplist src/bench_txn_3skiplist.o \
	  src/bench_list_scale.o src/rlu.o src/bench_iscrw.o src/iscrw.o src/bench_topology_list.o
