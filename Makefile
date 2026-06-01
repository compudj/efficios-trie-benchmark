# efficios-trie-benchmark — benchmarks for the Fractal Trie (FT) vs competing
# trie implementations (qp-trie, libart/ART, Judy) and BIND9's QP-trie.
#
# Layout:
#   src/                single- and multi-threaded benchmark harnesses
#   third_party/qp-trie vendored Tony Finch qp-trie (CC0) — Tbl dispatch + qp backend
#   third_party/libart  vendored libart ART (BSD-2-Clause)
#   urcu-build/         our own out-of-tree liburcu build (the Fractal Trie)
#   config.mk           local paths / feature flags (edit this, not the Makefile)
#
# Quick start:
#   make urcu      # build our own liburcu (Fractal Trie) from $(URCU_SRC)
#   make           # build the benchmarks
#   ./bench_one_st dns ft

include config.mk

URCU_INC_BUILD := $(URCU_BUILD)/include
URCU_INC_SRC   := $(URCU_SRC)/include
URCU_LIB       := $(URCU_BUILD)/src/.libs

# Include both the build-tree include (generated config.h) and the source-tree
# include (public API headers) — same convention as liburcu's own AM_CPPFLAGS.
URCU_CPPFLAGS := -I$(URCU_INC_BUILD) -I$(URCU_INC_SRC)

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

BENCHES := bench_one_st

.PHONY: all clean clean-urcu urcu check-urcu
all: $(BENCHES)

# Single-threaded, single-engine benchmark: FT / ft_skip / ft_cand / judy / qp / art.
bench_one_st: src/bench_one_st.c $(QP_OBJS) $(ART_OBJS) | check-urcu
	$(CC) $(CFLAGS) $(CPPFLAGS_COMMON) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Vendored competitor sources: compile with the same opt flags, but only their
# own include dir (they are independent of urcu/FT).
third_party/qp-trie/%.o: third_party/qp-trie/%.c
	$(CC) $(CFLAGS) -std=gnu99 -Ithird_party/qp-trie -c -o $@ $<

third_party/libart/%.o: third_party/libart/%.c
	$(CC) $(CFLAGS) -Ithird_party/libart -c -o $@ $<

# ---------------------------------------------------------------------------
# Build our own out-of-tree liburcu (the Fractal Trie) from $(URCU_SRC).
# This reads the source tree but writes only into $(URCU_BUILD); it never
# modifies the userspace-rcu source.  Re-run after the FT source changes.
# ---------------------------------------------------------------------------
urcu:
	@test -x "$(URCU_SRC)/configure" || { \
	  echo "ERROR: $(URCU_SRC)/configure not found. Set URCU_SRC in config.mk"; exit 1; }
	mkdir -p "$(URCU_BUILD)"
	cd "$(URCU_BUILD)" && CFLAGS="$(URCU_CFLAGS)" "$(URCU_SRC)/configure"
	$(MAKE) -C "$(URCU_BUILD)/src"

clean-urcu:
	rm -rf "$(URCU_BUILD)"

check-urcu:
	@test -f "$(URCU_LIB)/liburcu-cds.so" || { \
	  echo "ERROR: liburcu-cds not found under $(URCU_LIB)"; \
	  echo "       Run 'make urcu' to build our own liburcu (the Fractal Trie),"; \
	  echo "       or set URCU_SRC / URCU_BUILD in config.mk."; \
	  exit 1; }

clean:
	rm -f $(BENCHES) $(QP_OBJS) $(ART_OBJS)
