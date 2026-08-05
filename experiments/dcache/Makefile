# experiments/dcache/Makefile -- build the userspace dentry-cache experiment.
#
# Links the repo's in-tree urcu-txn engine build (../../urcu-txn-build), same as
# the top-level bench_txn_* targets; run `make urcu-txn` at the repo root first
# if urcu-txn-build/ is empty.  The seqlock baseline uses only the QSBR flavor +
# call_rcu; the txn engine (S2) will add -I for <urcu/rcu-txn.h>.
#
# Usage:
#   make            # build test_dcache against the seqlock engine
#   make check      # build + run the correctness harness
#   make ENGINE=txn # (S2) build against dcache_txn.o instead

URCU_TXN_BUILD ?= $(abspath ../../urcu-txn-build)
URCU_TXN_INC   := $(URCU_TXN_BUILD)/include
URCU_TXN_LIB   := $(URCU_TXN_BUILD)/src/.libs

# The descriptor slab's rseq arm.  URCU_SLAB_RSEQ is HEADER-inline
# (rcu-txn-slab.h is static inline) and that header requires the macro to be
# IDENTICAL across every TU of a process: a TU built without it takes the
# arena's pop lock while one built with it pops the same freelist locklessly,
# which lfstack's synchronization matrix forbids.  So selecting the rseq arm
# with URCU_TXN_BUILD alone -- linking the rseq library but compiling these TUs
# without the macro -- silently produces exactly that broken build.
#
# Derive it from the liburcu build being linked instead of offering a second
# knob, so the two cannot disagree.  It is read from that build's CPPFLAGS, not
# its config.h: configure appends -DURCU_SLAB_RSEQ rather than AC_DEFINE-ing it.
comma := ,
URCU_SLAB_RSEQ := $(shell grep -sqE '^CPPFLAGS = .*-DURCU_SLAB_RSEQ' \
		     $(URCU_TXN_BUILD)/src/Makefile && echo yes)
# URCU_TXN_SLAB_BATCH routes the engines' descriptor retirement through the
# slab's batch machinery (urcu_slab_free_pending at commit) instead of one
# call_rcu per descriptor.  urcu_txn_retire() is a static inline compiled into
# THESE TUs, so the flag has to reach them too -- derived from the liburcu build
# for the same can-never-disagree reason as the rseq flag above.
URCU_TXN_SLAB_BATCH := $(shell grep -sqE '^CPPFLAGS = .*-DURCU_TXN_SLAB_BATCH' \
			  $(URCU_TXN_BUILD)/src/Makefile && echo yes)
ifeq ($(URCU_TXN_SLAB_BATCH),yes)
SLAB_CPP += -DURCU_TXN_SLAB_BATCH
endif

ifeq ($(URCU_SLAB_RSEQ),yes)
RSEQ_INC := $(shell grep -shoE '\-I[^ ]*librseq[^ ]*' $(URCU_TXN_BUILD)/src/Makefile | head -1)
RSEQ_L   := $(shell grep -shoE '\-L[^ ]*librseq[^ ]*' $(URCU_TXN_BUILD)/src/Makefile | head -1)
SLAB_CPP += -DURCU_SLAB_RSEQ $(RSEQ_INC)
SLAB_LD  := $(RSEQ_L) $(patsubst -L%,-Wl$(comma)-rpath$(comma)%,$(RSEQ_L))
SLAB_LIB := -lrseq
endif

CC       ?= gcc
CFLAGS   ?= -O2 -g -pthread -Wall -Wextra -march=native
CPPFLAGS := -I$(URCU_TXN_INC) -I. $(SLAB_CPP)
LDFLAGS  := -L$(URCU_TXN_LIB) -Wl,-rpath,$(URCU_TXN_LIB) $(SLAB_LD)
# Phase 3's LRU shards by NUMA node, read from the rseq ABI page
# (rseq_current_node_id) -- a plain load, no syscall and no libnuma.  Linked
# unconditionally because every engine now carries the LRU or its stub; build
# with -DDC_NO_LRU to drop both the field and this dependency.
LDLIBS   := -lurcu-qsbr -lurcu-common $(SLAB_LIB) -lrseq -lpthread

# Rebuild when the LIBRARY changes, not only when our sources do.  Without this
# every binary here silently survives a liburcu rebuild: the prerequisites are
# .c/.h files, so make sees nothing to do, and the stale binary either segfaults
# on an ABI change ("urcu_txn_slab has different size in shared object") or,
# worse, runs and reports numbers for the previous library.  Both have happened.
URCU_DEP := $(URCU_TXN_LIB)/liburcu-qsbr.so

# The default 1-CL split layout returns the host ADDRESS as identity.  The
# correctness harnesses assert logical ids (id==gid torn-read checks), so they
# build -DDC_SPLIT_KEEPID: same 1-CL layout, but dc_lookup reads the cold d_id and
# returns it.  The bench (census-gated) uses the address default.
KEEPID   := -DDC_SPLIT_KEEPID
HCPP     := $(KEEPID) $(CPPFLAGS)

ENGINE   ?= seqlock
# $(URCU_DEP) rides in HDRS because nearly every target here already lists HDRS,
# so one line makes a liburcu rebuild invalidate every binary that links it.
HDRS     := dcache.h seqcount.h dcache_lru.h dcache_lru_shrink.h $(URCU_DEP)

.PHONY: all check check-asan repro repro-asan repro-fold	stress stress-asan stress-tsan stress-dirs stress-dirs-asan \
	stress-dirs-tsan xchg xchg-asan xchg-tsan bench bench-check bench-asan \
	check-pernode check-mark check-bucketlock check-bucketlock-tsan check-bucketlock-swmw check-bucketlock-swmw-tsan \
	check-bucketlock-chainlock check-bucketlock-chainlock-tsan \
	check-filetype repro-samedir churn check-churn check-deque check-deque-tsan \
	clean check-lib

# ASan flags for the from-source concurrency builds.
ASANFLAGS := -O1 -g -pthread -fsanitize=address -fno-omit-frame-pointer
TSANFLAGS := -O1 -g -pthread -fsanitize=thread -fno-omit-frame-pointer

all: test_dcache

check-lib:
	@test -f "$(URCU_TXN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: urcu-txn engine not built at $(URCU_TXN_LIB)"; \
	  echo "       Run 'make urcu-txn' at the repo root first."; exit 1; }

test_dcache: test_dcache.o dcache_$(ENGINE).o | check-lib
	$(CC) $(CFLAGS) -o $@ test_dcache.o dcache_$(ENGINE).o $(LDFLAGS) $(LDLIBS)

%.o: %.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# test_dcache links this object; build it -DDC_SPLIT_KEEPID so the suite's
# logical-id assertions hold on the 1-CL split layout.  Only test_dcache uses the
# .o -- the bench builds each engine from source (address default).
dcache_txn.o: dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -c $< -o $@

dcache_seqlock.o: dcache_seqlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -c $< -o $@

# bucket lock+SW engine (dcache_bucketlock.c): per-bucket bit-lock + single-writer SW commit +
# per-host transition-chain lock.  MARK-ONLY (a bare build defaults to the mark
# arm; a counter arm #errors).  Explicit -DDC_MARK_GEN for clarity.  Use with
# `make test_dcache ENGINE=bucketlock`.
dcache_bucketlock.o: dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -c $< -o $@

check: test_dcache
	./test_dcache

# Single-thread harness under ASan (from source, so -fsanitize can't collide with
# the plain object files).  Use ENGINE=txn / seqlock.
check-asan: test_dcache.c dcache_$(ENGINE).c $(HDRS) | check-lib
	$(CC) $(ASANFLAGS) $(HCPP) -o test_dcache_asan \
	  test_dcache.c dcache_$(ENGINE).c $(LDFLAGS) $(LDLIBS)
	./test_dcache_asan

# Concurrency repro of the walk-causality anomaly (txn engine only; needs the
# DC_TEST_HOOKS rendezvous hook).  Built straight from source so the -D can't
# collide with the plain object files.
repro: repro_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -DDC_TEST_HOOKS -o repro_dcache \
	  repro_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_dcache

repro-fold: repro_fold.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -DDC_TEST_HOOKS -o repro_fold \
	  repro_fold.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_fold

# The fold-TRANSFER vs d_delete lost-update race (phase 2).  Runs on BOTH txn
# engines: the window is the fold's identity handover, which both implement.
repro-delete-fold: repro_delete_fold.c dcache_txn.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -DDC_TEST_HOOKS -o repro_delete_fold_mark \
	  repro_delete_fold.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_delete_fold_mark
	$(CC) $(CFLAGS) $(HCPP) -DDC_TEST_HOOKS -o repro_delete_fold_bl \
	  repro_delete_fold.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_delete_fold_bl

# The negative-directory guard pair (all-MW engine).  Both directions, since a
# single guard is one-directional and single-threaded tests miss both.
repro-negdir: repro_negdir.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -DDC_TEST_HOOKS -o repro_negdir_mark \
	  repro_negdir.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_negdir_mark

repro-asan: repro_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(ASANFLAGS) \
	  $(HCPP) -DDC_TEST_HOOKS -o repro_dcache_asan \
	  repro_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_dcache_asan

# Concurrent stress + conservation harness -- the primary validator for the
# async fold (the deterministic repro is latent once the fold is deferred).
# Built from source, txn engine only.
stress: stress_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -o stress_dcache \
	  stress_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache

stress-asan: stress_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(ASANFLAGS) $(HCPP) -o stress_dcache_asan \
	  stress_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_asan

# TSAN needs liburcu ITSELF rebuilt with compiler atomic builtins AND
# -fsanitize=thread (else the uatomic ops are inline asm TSAN can't see and the
# RCU grace-period edge is invisible -> a pile of phantom descriptor-recycle
# races while the real ordering bug goes unreported).  Build that library once:
#   cd <repo root> && rm -rf urcu-txn-tsan-build && mkdir urcu-txn-tsan-build && \
#   tar -c --exclude=.git -C urcu-txn-build . | tar -x -C urcu-txn-tsan-build && \
#   cd urcu-txn-tsan-build && make distclean; ./configure \
#     --enable-compiler-atomic-builtins \
#     CFLAGS="-fsanitize=thread -O1 -g -fno-omit-frame-pointer" \
#     LDFLAGS="-fsanitize=thread" && make -C src
URCU_TSAN_BUILD ?= $(abspath ../../urcu-txn-tsan-build)
URCU_TSAN_INC   := $(URCU_TSAN_BUILD)/include
# Slab-route flags for the TSAN builds, derived from the TSAN liburcu -- NOT
# from $(SLAB_CPP), which is derived from URCU_TXN_BUILD and would describe a
# different library.  These recipes do not use $(CPPFLAGS), so without this a
# TSAN run compiles the DEFAULT route against a batch-built liburcu: a mixed
# configuration that tests neither route and is exactly the state
# rcu-txn-slab.h forbids.  A gate that silently tests the wrong thing is worse
# than no gate.
TSAN_SLAB_CPP := $(shell grep -sqE '^CPPFLAGS = .*-DURCU_TXN_SLAB_BATCH' \
		    $(URCU_TSAN_BUILD)/src/Makefile && echo -DURCU_TXN_SLAB_BATCH)
TSAN_SLAB_CPP += $(shell grep -sqE '^CPPFLAGS = .*-DURCU_SLAB_RSEQ' \
		    $(URCU_TSAN_BUILD)/src/Makefile && echo -DURCU_SLAB_RSEQ)
URCU_TSAN_LIB   := $(URCU_TSAN_BUILD)/src/.libs
stress-tsan: stress_dcache.c dcache_txn.c $(HDRS)
	@test -f "$(URCU_TSAN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: TSAN liburcu not found at $(URCU_TSAN_LIB) -- see the recipe"; \
	  echo "       in the stress-tsan comment in this Makefile."; exit 1; }
	$(CC) $(TSANFLAGS) $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. -o stress_dcache_tsan \
	  stress_dcache.c dcache_txn.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_tsan 4 2 8 8 15000

# Concurrent DIRECTORY-move stress: transacted d_parent + the atomic cross-dir
# loop check (subtree relocation conservation + the mutual-cycle-abort race).
# Defaults stay below the liveness cliff (see the file header); txn engine only.
stress-dirs: stress_dcache_dirs.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -o stress_dcache_dirs \
	  stress_dcache_dirs.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_dirs

stress-dirs-asan: stress_dcache_dirs.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(ASANFLAGS) $(HCPP) -o stress_dcache_dirs_asan \
	  stress_dcache_dirs.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_dirs_asan 4 8 4 2 15000

stress-dirs-tsan: stress_dcache_dirs.c dcache_txn.c $(HDRS)
	@test -f "$(URCU_TSAN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: TSAN liburcu not found at $(URCU_TSAN_LIB) -- see stress-tsan"; \
	  exit 1; }
	$(CC) $(TSANFLAGS) $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. -o stress_dcache_dirs_tsan \
	  stress_dcache_dirs.c dcache_txn.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	# TSAN's ~10x slowdown pushes the heavier dir-move config past the GP-bound
	# fold-drain liveness cliff (see the file header); keep this run light.
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_dirs_tsan 2 4 2 1 4000

# Concurrent ATOMIC-EXCHANGE stress: two shell stacks in one commit, permutation
# conservation + the "a slot is never empty" atomicity assertion; txn engine only.
xchg: stress_dcache_xchg.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -o stress_dcache_xchg \
	  stress_dcache_xchg.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_xchg

xchg-asan: stress_dcache_xchg.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(ASANFLAGS) $(HCPP) -o stress_dcache_xchg_asan \
	  stress_dcache_xchg.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_xchg_asan

xchg-tsan: stress_dcache_xchg.c dcache_txn.c $(HDRS)
	@test -f "$(URCU_TSAN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: TSAN liburcu not found at $(URCU_TSAN_LIB) -- see stress-tsan"; \
	  exit 1; }
	$(CC) $(TSANFLAGS) $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. -o stress_dcache_xchg_tsan \
	  stress_dcache_xchg.c dcache_txn.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_xchg_tsan 4 4 6 8 15000

# S3 throughput sweep: the engine-agnostic bench, one binary PER arm (built
# straight from source so they never share a stale .o).  Three arms: the seqlock
# baseline, the txn engine on its GLOBAL rename_gen, and the txn engine on the
# PER-NODE host generation (-DDC_PER_NODE_GEN -- a rename only invalidates walks
# that pass through the moved entry).  scripts/run_dcache.sh drives all three;
# plot_dcache.py charts them.
bench: bench_dcache_seqlock bench_dcache_txn bench_dcache_txn_pernode \
       bench_dcache_txn_mark bench_dcache_bucketlock churn

# The seqlock baseline builds the DEFAULT layout too: the fair 1-CL split hot
# line (d_parent tags + d_seq on CL0, d_id cold) returning the dentry ADDRESS as
# identity.  A/B partner: _nosplit = the legacy 3-CL struct.  Equal layout quality
# is what makes the txn-vs-seqlock footprint A/B mechanism-vs-mechanism.
bench_dcache_seqlock: bench_dcache.c dcache_seqlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ bench_dcache.c dcache_seqlock.c \
	  $(LDFLAGS) $(LDLIBS)

bench_dcache_seqlock_nosplit: bench_dcache.c dcache_seqlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_NO_HOT1CL_SPLIT $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_seqlock.c $(LDFLAGS) $(LDLIBS)

# The txn arms build the DEFAULT layout: the 1-CL split (straddle + d_iparent
# tags + d_seq on CL0) returning the host ADDRESS as identity (census-gated
# conservation).  A/B partners: _nosplit = legacy 3-CL baseline; _pernode_keepid
# = the same 1-CL layout but reading the cold d_id (logical id, so the reader
# touches 2 CLs -- the cost of keeping d_id hot).
bench_dcache_txn: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ bench_dcache.c dcache_txn.c \
	  $(LDFLAGS) $(LDLIBS)

bench_dcache_txn_nosplit: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_NO_HOT1CL_SPLIT $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

bench_dcache_txn_pernode: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

# Insert/remove throughput (bench_dcache_churn).  Built -DDC_SPLIT_KEEPID: a
# re-added dentry is a NEW allocation, so address identity is not stable across
# churn and the harness's id checks need logical ids.  The reader path is
# therefore not directly comparable with the address-default bench_dcache runs.
CHURN_ENGINES := seqlock txn txn_pernode txn_mark bucketlock
churn: $(addprefix bench_dcache_churn_,$(CHURN_ENGINES))

bench_dcache_churn_seqlock: bench_dcache_churn.c dcache_seqlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -o $@ bench_dcache_churn.c dcache_seqlock.c $(LDFLAGS) $(LDLIBS)
bench_dcache_churn_txn: bench_dcache_churn.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -o $@ bench_dcache_churn.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
bench_dcache_churn_txn_pernode: bench_dcache_churn.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN $(HCPP) -o $@ bench_dcache_churn.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
bench_dcache_churn_txn_mark: bench_dcache_churn.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -o $@ bench_dcache_churn.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
# bucket lock+SW churn arm: the add/unlink write path this engine targets (design §0.1).
# A/B partner to bench_dcache_churn_txn_mark (same reader + mark; MCAS vs bit-lock
# writer) and bench_dcache_churn_seqlock (the kernel-faithful write-path baseline).
bench_dcache_churn_bucketlock: bench_dcache_churn.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -o $@ bench_dcache_churn.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
# Mixed SW/MW churn arm: add/unlink are UNCHANGED from the chain-lock build (plain
# locked stores), so this should track bench_dcache_churn_bucketlock -- a null-result A/B
# that confirms the mixed engine costs the shell-FREE path nothing.
bench_dcache_churn_bucketlock_swmw: bench_dcache_churn.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(HCPP) -o $@ bench_dcache_churn.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

# ---- fold TRANSFER name-guard gate (REVIEW.md standing hazard) ------------
#
# The fold's TRANSFER copies 40/48 bytes of d_iname into the successor with a
# plain store, safe only because no reader reads a non-top node's name.  That
# was a comment; -DDC_DEBUG_NAME_GUARD makes it a check (see dcache.h).
#
# The gate is TWO runs per engine, because a detector that never fires proves
# nothing: the correct build must stay silent, and the MUTATED build -- which
# matches against the HOST's name, exactly the mistake the invariant forbids --
# must ABORT.  A mutated build that survives means the guard is vacuous and the
# silent run above was worthless.
NAMEGUARD_RUN := --nthreads 8 --writers 3 --leaves 16 --ndirs 8 --duration 2000
NAMEGUARD_BINS := bench_dcache_nameguard_bucketlock \
		  bench_dcache_nameguard_bucketlock_mutate \
		  bench_dcache_nameguard_txn bench_dcache_nameguard_txn_mutate

bench_dcache_nameguard_bucketlock: bench_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_DEBUG_NAME_GUARD $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
bench_dcache_nameguard_bucketlock_mutate: bench_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_DEBUG_NAME_GUARD \
	  -DDC_DEBUG_NAME_GUARD_MUTATE $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
bench_dcache_nameguard_txn: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_DEBUG_NAME_GUARD $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
bench_dcache_nameguard_txn_mutate: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_DEBUG_NAME_GUARD \
	  -DDC_DEBUG_NAME_GUARD_MUTATE $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

check-nameguard: $(NAMEGUARD_BINS)
	@for e in bucketlock txn; do \
	  ./bench_dcache_nameguard_$$e $(NAMEGUARD_RUN) > /dev/null 2>&1 || \
	    { echo "FAIL: $$e fired the name guard on CORRECT code"; exit 1; }; \
	  echo "  ok: $$e clean under the guard"; \
	  if ./bench_dcache_nameguard_$${e}_mutate $(NAMEGUARD_RUN) > /dev/null 2>&1; then \
	    echo "FAIL: mutated $$e SURVIVED -- the name guard is VACUOUS"; exit 1; \
	  fi; \
	  echo "  ok: mutated $$e aborted (guard is live)"; \
	done

# Quick correctness gate for the churn invariant (state + census + ids).
check-churn: churn
	./bench_dcache_churn_txn_mark --writers 6 --readers 6 --duration 1500
	./bench_dcache_churn_txn_pernode --writers 6 --readers 6 --duration 1500
	./bench_dcache_churn_seqlock --writers 6 --readers 6 --duration 1500

# Localized walk causality with NO counter: the hlist deletion mark is the
# version, so d_seq is gone and DC_NAME_MAX rises 32 -> 40.  The DENTRY is
# otherwise identical (d_hash still at @56, sizeof(dentry) still 168), and
# dc_qstr_eq short-circuits on hash/len, so the wider name array costs the MATCH
# nothing -- but see the w32 control below for what it does cost.
bench_dcache_txn_mark: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

# ---- matched-name-width controls (REVIEW.md open item) --------------------
#
# DC_NAME_MAX is not only a dentry knob: struct qstr is shared with struct
# dc_path, so the mark arm's 40 also makes the HARNESS path object 20% bigger
# (sizeof(dc_path) 964 -> 1156), the per-component path copy 48 B instead of
# 40 B, and the precomputed leaf-qstr table 20% bigger.  Those sit on the
# reader's hot path and are NOT the mechanism under test, so a mark-vs-{global,
# per-node,seqlock} reader table mixes the two.  See dcache.h's DC_NAME_PAD.
#
#   _w32        NAME_MAX=32 + 8 B pad: dentry BYTE-IDENTICAL to the shipped mark
#               arm (sizeof 168/176, d_hash @56), harness path matched to the
#               baselines.  Isolates the harness co-footprint -- the arm to
#               publish alongside mark.
#   _w32_shrink NAME_MAX=32, no pad: dentry shrinks 8 B and d_hash moves @56 ->
#               @48.  Prices what the 8 bytes the mark mechanism freed buy.
#
# A _w32 that lands on the mark curve RETIRES the caveat with evidence; a gap
# says the published mark reader numbers were paying a harness tax.
namectl: bench_dcache_txn_mark_w32 bench_dcache_txn_mark_w32_shrink \
	 bench_dcache_bucketlock_w32 bench_dcache_bucketlock_w32_shrink \
	 bench_dcache_height_txn_mark_w32 bench_dcache_height_bucketlock_w32 \
	 bench_dcache_churn_txn_mark_w32 bench_dcache_churn_bucketlock_w32

NAME_W32       := -DDC_NAME_MAX=32 -DDC_NAME_PAD=8
NAME_W32_SHRINK := -DDC_NAME_MAX=32

bench_dcache_txn_mark_w32: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(NAME_W32) $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

bench_dcache_txn_mark_w32_shrink: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(NAME_W32_SHRINK) $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

bench_dcache_bucketlock_w32: bench_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(NAME_W32) $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

bench_dcache_bucketlock_w32_shrink: bench_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(NAME_W32_SHRINK) $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

bench_dcache_height_txn_mark_w32: bench_dcache_height.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(NAME_W32) $(CPPFLAGS) -o $@ \
	  bench_dcache_height.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

bench_dcache_height_bucketlock_w32: bench_dcache_height.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(NAME_W32) $(CPPFLAGS) -o $@ \
	  bench_dcache_height.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

# readdir is the most exposed panel: dc_readdir COPIES each child's d_iname into
# the caller's dc_path, so the qstr width is paid per dirent, not per lookup.
bench_dcache_churn_txn_mark_w32: bench_dcache_churn.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(NAME_W32) $(HCPP) -o $@ \
	  bench_dcache_churn.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

bench_dcache_churn_bucketlock_w32: bench_dcache_churn.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(NAME_W32) $(HCPP) -o $@ \
	  bench_dcache_churn.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

# bucket lock+SW reader bench (address default): the reader path is IDENTICAL to the
# txn-mark arm (same resolve + mark), so this A/B isolates the writer-engine swap
# on the read-under-churn panels -- reads should stay on the txn-mark curve.  The
# DEFAULT chain strategy is now the per-host FOLD LOCK (SW enqueue + fold-lock
# dequeue); the legacy chain lock is bench_dcache_bucketlock_chainlock (-DDC_CHAIN_LOCK).
bench_dcache_bucketlock: bench_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

# Legacy per-host CHAIN LOCK reference (-DDC_CHAIN_LOCK): the demote AND the folds
# take the lock.  The A/B baseline the fold-lock default (bench_dcache_bucketlock) and the
# MW dequeue (bench_dcache_bucketlock_swmw) are measured against.
bench_dcache_bucketlock_chainlock: bench_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

# bucket lock MIXED SW/MW reader/rename bench (-DDC_CHAIN_SWMW): identical reader; the
# transition chain rides the mixed commit instead of the per-host chain lock
# (sizeof(dentry) 176 -> 168).  A/B partner to bench_dcache_bucketlock (same reader+mark;
# chain lock vs lock-free MW chain).
bench_dcache_bucketlock_swmw: bench_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

# Same-SIZE (176 B) A/B control: the mixed engine with the retired chain lock's 8
# bytes restored as dead padding, so bench_dcache_bucketlock_swmw_pad vs bench_dcache_bucketlock
# isolates the lock-free-chain MECHANISM and _swmw vs _swmw_pad isolates the -8B.
bench_dcache_bucketlock_swmw_pad: bench_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW -DDC_SWMW_PAD $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

# Per-node A/B partners: legacy 3-CL baseline, and the split layout that keeps
# the cold d_id read (logical id, 2-CL) to isolate the d_id-elision gain.
bench_dcache_txn_pernode_nosplit: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN -DDC_NO_HOT1CL_SPLIT $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

bench_dcache_txn_pernode_keepid: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN $(KEEPID) $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

# Fair-weather 1-CL pack (global gen, d_hash cold): A/B vs bench_dcache_txn.
bench_dcache_txn_hot1cl: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_HOT1CL $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

# Adversarial MOVE-HEIGHT sweep: writers exchange sibling subtrees at height H in
# a balanced B-ary band; readers walk full leaf paths.  Bounds the per-node arm
# (leaf case H=0 is its best case; near-root moves degenerate to the global
# counter).  Same three arms as `bench`; scripts/run_dcache_height.sh drives them.
height: bench_dcache_height_seqlock bench_dcache_height_txn \
	bench_dcache_height_txn_pernode bench_dcache_height_txn_mark \
	bench_dcache_height_bucketlock

bench_dcache_height_seqlock: bench_dcache_height.c dcache_seqlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ bench_dcache_height.c dcache_seqlock.c \
	  $(LDFLAGS) $(LDLIBS)

bench_dcache_height_txn: bench_dcache_height.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ bench_dcache_height.c dcache_txn.c \
	  $(LDFLAGS) $(LDLIBS)

bench_dcache_height_txn_pernode: bench_dcache_height.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN $(CPPFLAGS) -o $@ \
	  bench_dcache_height.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

bench_dcache_height_txn_mark: bench_dcache_height.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(CPPFLAGS) -o $@ bench_dcache_height.c \
	  dcache_txn.c $(LDFLAGS) $(LDLIBS)

bench_dcache_height_bucketlock: bench_dcache_height.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(CPPFLAGS) -o $@ bench_dcache_height.c \
	  dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

bench_dcache_height_bucketlock_swmw: bench_dcache_height.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(CPPFLAGS) -o $@ bench_dcache_height.c \
	  dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)

# ASan smoke of the height harness (small tree, short) on both engines.
height-asan: bench_dcache_height.c dcache_seqlock.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(ASANFLAGS) $(CPPFLAGS) -o bench_dcache_height_seqlock_asan \
	  bench_dcache_height.c dcache_seqlock.c $(LDFLAGS) $(LDLIBS)
	./bench_dcache_height_seqlock_asan --writers 4 --nthreads 12 --move-height 2 \
	  --branch 2 --tree-depth 6 --duration 300
	$(CC) $(ASANFLAGS) -DDC_PER_NODE_GEN $(CPPFLAGS) -o bench_dcache_height_txn_asan \
	  bench_dcache_height.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./bench_dcache_height_txn_asan --writers 4 --nthreads 12 --move-height 2 \
	  --branch 2 --tree-depth 6 --duration 300

# Quick smoke: a short contended run on all three arms; nonzero exit on any
# conservation failure (each binary returns 1 on CONSERVATION FAILED).
bench-check: bench
	./bench_dcache_seqlock --nthreads 8 --rename-frac 0.2 --ndirs 8 --depth 3 \
	  --leaves 16 --duration 400
	./bench_dcache_txn --nthreads 8 --rename-frac 0.2 --ndirs 8 --depth 3 \
	  --leaves 16 --duration 400
	./bench_dcache_txn_pernode --nthreads 8 --rename-frac 0.2 --ndirs 8 \
	  --depth 3 --leaves 16 --duration 400

# The per-node reader (host seqcount + sample/confirm-top/up-pass validation)
# must clear the SAME correctness bar as the global-gen reader: the full suite,
# the deterministic walk-causality repro (no misdirection), and concurrent
# conservation under ASan.  Built from source with -DDC_PER_NODE_GEN.
# Same-directory twin of `repro`: catches a walk-causality regression that every
# other harness structurally cannot see (they only ever rename cross-parent, or
# only move leaves).  See the file header.
repro-samedir: repro_dcache_samedir.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_TEST_HOOKS $(HCPP) -o repro_dcache_samedir \
	  repro_dcache_samedir.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_dcache_samedir

# DC_MARK_GEN arm: walk causality on the d_hash.next deletion mark, so d_iparent
# stays write-once and d_seq is gone (DC_NAME_MAX 40).
# File/directory type: ENOTDIR enforcement + file-rename bump-skip soundness.
# Runs on all three txn arms and the seqlock baseline.
check-filetype: test_file_type.c dcache_txn.c dcache_seqlock.c $(HDRS) | check-lib
	@for a in "" "-DDC_PER_NODE_GEN" "-DDC_MARK_GEN"; do \
	  $(CC) $(CFLAGS) $$a $(HCPP) -o test_file_type_txn test_file_type.c \
	    dcache_txn.c $(LDFLAGS) $(LDLIBS) && ./test_file_type_txn; done
	$(CC) $(CFLAGS) $(HCPP) -o test_file_type_seqlock test_file_type.c \
	  dcache_seqlock.c $(LDFLAGS) $(LDLIBS)
	./test_file_type_seqlock

check-mark: test_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -o test_dcache_mark \
	  test_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_mark
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_dcache_mark repro_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_dcache_mark
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_samedir_mark repro_dcache_samedir.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_samedir_mark
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN $(HCPP) -o stress_dcache_mark_asan \
	  stress_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_mark_asan 4 4 8 16 15000

# bucket lock+SW engine validation gate (mark arm) -- the DEFAULT chain strategy, now the
# per-host FOLD LOCK (SW enqueue + fold-lock dequeue).  The full single-thread
# suite, both walk-causality repros + the fold repro, then the concurrent
# conservation harnesses under ASan -- general, dir-move cycle, atomic exchange, and
# the mixed rename/exchange mutual-cycle harness (mixed_cycle.c).  The legacy chain
# lock is check-bucketlock-chainlock; the MW dequeue is check-bucketlock-swmw.
check-bucketlock: test_dcache.c repro_dcache.c repro_dcache_samedir.c repro_fold.c \
	   stress_dcache.c stress_dcache_dirs.c stress_dcache_xchg.c mixed_cycle.c \
	   dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -o test_dcache_bucketlock \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_bucketlock
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_dcache_bucketlock repro_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_dcache_bucketlock
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_samedir_bucketlock repro_dcache_samedir.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_samedir_bucketlock
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_fold_bucketlock repro_fold.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_fold_bucketlock
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN $(HCPP) -o stress_dcache_bucketlock_asan \
	  stress_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_bucketlock_asan 4 4 8 16 15000
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN $(HCPP) -o stress_dcache_dirs_bucketlock_asan \
	  stress_dcache_dirs.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_dirs_bucketlock_asan 4 8 4 2 15000
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN $(HCPP) -o stress_dcache_xchg_bucketlock_asan \
	  stress_dcache_xchg.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_xchg_bucketlock_asan
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN $(HCPP) -o mixed_cycle_bucketlock_asan \
	  mixed_cycle.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./mixed_cycle_bucketlock_asan

# bucket lock+SW under TSAN (needs the compiler-builtins liburcu; see stress-tsan): the
# per-host chain lock + demote + fold + the two-node exchange replace must be
# race-free.  General, exchange, and dir-move, at the light configs TSAN's ~10x
# slowdown keeps below the fold-drain liveness cliff.
check-bucketlock-tsan: stress_dcache.c stress_dcache_xchg.c stress_dcache_dirs.c dcache_bucketlock.c
	@test -f "$(URCU_TSAN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: TSAN liburcu not found at $(URCU_TSAN_LIB) -- see stress-tsan"; \
	  exit 1; }
	$(CC) $(TSANFLAGS) -DDC_MARK_GEN $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. \
	  -o stress_dcache_bucketlock_tsan stress_dcache.c dcache_bucketlock.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_bucketlock_tsan 4 2 8 8 15000
	$(CC) $(TSANFLAGS) -DDC_MARK_GEN $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. \
	  -o stress_dcache_xchg_bucketlock_tsan stress_dcache_xchg.c dcache_bucketlock.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_xchg_bucketlock_tsan 4 4 6 8 15000
	$(CC) $(TSANFLAGS) -DDC_MARK_GEN $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. \
	  -o stress_dcache_dirs_bucketlock_tsan stress_dcache_dirs.c dcache_bucketlock.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_dirs_bucketlock_tsan 2 4 2 1 4000

# LEGACY per-host CHAIN LOCK gate (-DDC_CHAIN_LOCK): the original strategy (demote +
# folds both take the lock), kept as the A/B reference and to confirm it still builds
# under the DC_CHAIN_MIXED refactor.  Same surface as check-bucketlock.
check-bucketlock-chainlock: test_dcache.c repro_dcache.c repro_dcache_samedir.c repro_fold.c \
	   stress_dcache.c stress_dcache_dirs.c stress_dcache_xchg.c mixed_cycle.c \
	   dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK $(HCPP) -o test_dcache_bucketlock_chainlock \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_bucketlock_chainlock
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_dcache_bucketlock_chainlock repro_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_dcache_bucketlock_chainlock
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_samedir_bucketlock_chainlock repro_dcache_samedir.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_samedir_bucketlock_chainlock
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_fold_bucketlock_chainlock repro_fold.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_fold_bucketlock_chainlock
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK $(HCPP) -o stress_dcache_bucketlock_chainlock_asan \
	  stress_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_bucketlock_chainlock_asan 4 4 8 16 15000
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK $(HCPP) -o stress_dcache_dirs_bucketlock_chainlock_asan \
	  stress_dcache_dirs.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_dirs_bucketlock_chainlock_asan 4 8 4 2 15000
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK $(HCPP) -o stress_dcache_xchg_bucketlock_chainlock_asan \
	  stress_dcache_xchg.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_xchg_bucketlock_chainlock_asan
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK $(HCPP) -o mixed_cycle_bucketlock_chainlock_asan \
	  mixed_cycle.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./mixed_cycle_bucketlock_chainlock_asan

check-bucketlock-chainlock-tsan: stress_dcache.c stress_dcache_xchg.c stress_dcache_dirs.c dcache_bucketlock.c
	@test -f "$(URCU_TSAN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: TSAN liburcu not found at $(URCU_TSAN_LIB) -- see stress-tsan"; \
	  exit 1; }
	$(CC) $(TSANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. \
	  -o stress_dcache_bucketlock_chainlock_tsan stress_dcache.c dcache_bucketlock.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_bucketlock_chainlock_tsan 4 2 8 8 15000
	$(CC) $(TSANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_LOCK $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. \
	  -o stress_dcache_xchg_bucketlock_chainlock_tsan stress_dcache_xchg.c dcache_bucketlock.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_xchg_bucketlock_chainlock_tsan 4 4 6 8 15000

# bucket lock MIXED SW/MW engine validation gate (-DDC_CHAIN_SWMW): identical to check-bucketlock
# but the transition chain (d_fwd/d_back) rides the mixed SW/MW commit instead of a
# per-host chain lock (index stays store_sw + bucket-locked; demote/promote/splice
# become store_mw records; the chain lock + d_chain_lock are retired).  The shell
# ops re-inherit the fair-mutex escalation discipline, so mixed_cycle.c (the
# rename/exchange mutual-cycle harness) is the key added stressor here.
check-bucketlock-swmw: test_dcache.c repro_dcache.c repro_dcache_samedir.c repro_fold.c \
	   stress_dcache.c stress_dcache_dirs.c stress_dcache_xchg.c mixed_cycle.c \
	   dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(HCPP) -o test_dcache_bucketlock_swmw \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_bucketlock_swmw
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_dcache_bucketlock_swmw repro_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_dcache_bucketlock_swmw
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_samedir_bucketlock_swmw repro_dcache_samedir.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_samedir_bucketlock_swmw
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_fold_bucketlock_swmw repro_fold.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./repro_fold_bucketlock_swmw
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(HCPP) -o stress_dcache_bucketlock_swmw_asan \
	  stress_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_bucketlock_swmw_asan 4 4 8 16 15000
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(HCPP) -o stress_dcache_dirs_bucketlock_swmw_asan \
	  stress_dcache_dirs.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_dirs_bucketlock_swmw_asan 4 8 4 2 15000
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(HCPP) -o stress_dcache_xchg_bucketlock_swmw_asan \
	  stress_dcache_xchg.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_xchg_bucketlock_swmw_asan
	$(CC) $(ASANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(HCPP) -o mixed_cycle_bucketlock_swmw_asan \
	  mixed_cycle.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./mixed_cycle_bucketlock_swmw_asan

# bucket lock MIXED SW/MW under TSAN (needs the compiler-builtins liburcu; see stress-tsan):
# the store_mw demote/promote/splice, the CAS-old adjacent-fold serialization, and
# the fair-mutex lane must all be race-free without the chain lock.
check-bucketlock-swmw-tsan: stress_dcache.c stress_dcache_xchg.c stress_dcache_dirs.c dcache_bucketlock.c
	@test -f "$(URCU_TSAN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: TSAN liburcu not found at $(URCU_TSAN_LIB) -- see stress-tsan"; \
	  exit 1; }
	$(CC) $(TSANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. \
	  -o stress_dcache_bucketlock_swmw_tsan stress_dcache.c dcache_bucketlock.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_bucketlock_swmw_tsan 4 2 8 8 15000
	$(CC) $(TSANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. \
	  -o stress_dcache_xchg_bucketlock_swmw_tsan stress_dcache_xchg.c dcache_bucketlock.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_xchg_bucketlock_swmw_tsan 4 4 6 8 15000
	$(CC) $(TSANFLAGS) -DDC_MARK_GEN -DDC_CHAIN_SWMW $(KEEPID) $(TSAN_SLAB_CPP) -I$(URCU_TSAN_INC) -I. \
	  -o stress_dcache_dirs_bucketlock_swmw_tsan stress_dcache_dirs.c dcache_bucketlock.c \
	  -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS)
	TSAN_OPTIONS="halt_on_error=0 history_size=4" ./stress_dcache_dirs_bucketlock_swmw_tsan 2 4 2 1 4000

check-pernode: test_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN $(HCPP) -o test_dcache_pernode \
	  test_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_pernode
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_dcache_pernode repro_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./repro_dcache_pernode
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN -DDC_TEST_HOOKS $(HCPP) \
	  -o repro_samedir_pernode repro_dcache_samedir.c dcache_txn.c \
	  $(LDFLAGS) $(LDLIBS)
	./repro_samedir_pernode
	$(CC) $(ASANFLAGS) -DDC_PER_NODE_GEN $(HCPP) -o stress_dcache_pernode_asan \
	  stress_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./stress_dcache_pernode_asan 4 4 8 16 15000

# Transaction instrumentation smoke: the -DDC_TXN_STATS build must still be
# correct, not just compile -- it re-implements the LRU list mutators with the
# _prepare form so the handle is ours to inspect.
check-txnstats: test_dcache.c dcache_bucketlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_TXN_STATS $(HCPP) -o test_dcache_txnstats \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_txnstats
	$(CC) $(CFLAGS) -DDC_LRU_MCAS -DDC_TXN_STATS $(HCPP) -o test_dcache_txnstats_mcas \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_txnstats_mcas

# PHASE 3 LRU SHARD-AXIS arms.  Per-node is the kernel's own granularity and the
# default; per-CPU and mm_cid are the userspace refinements this compares against.
# Correctness must hold on all three -- a shard axis is not allowed to change what
# the cache CONTAINS, only how the eviction order is approximated.
check-lru-arms: test_dcache.c dcache_bucketlock.c dcache_txn.c $(HDRS) | check-lib
	@echo "--- txn engine (all-MW): lock arm, MCAS arm, no-LRU control ---"
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -o test_dcache_txn_lru \
	  test_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_txn_lru
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_LRU_MCAS $(HCPP) -o test_dcache_txn_lru_mcas \
	  test_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_txn_lru_mcas
	$(CC) $(CFLAGS) -DDC_MARK_GEN -DDC_NO_LRU $(HCPP) -o test_dcache_txn_no_lru \
	  test_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_txn_no_lru
	@echo "--- bucketlock engine ---"
	$(CC) $(CFLAGS) $(HCPP) -o test_dcache_lru_pernode \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_lru_pernode
	$(CC) $(CFLAGS) -DDC_LRU_PERCPU $(HCPP) -o test_dcache_lru_percpu \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_lru_percpu
	$(CC) $(CFLAGS) -DDC_LRU_MM_CID $(HCPP) -o test_dcache_lru_mmcid \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_lru_mmcid
	$(CC) $(CFLAGS) -DDC_LRU_MCAS $(HCPP) -o test_dcache_lru_mcas \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_lru_mcas
	$(CC) $(CFLAGS) -DDC_LRU_MCAS -DDC_LRU_PERCPU $(HCPP) -o test_dcache_lru_mcas_percpu \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_lru_mcas_percpu
	$(CC) $(CFLAGS) -DDC_NO_LRU $(HCPP) -o test_dcache_no_lru \
	  test_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_no_lru
	$(CC) $(ASANFLAGS) -DDC_LRU_PERCPU $(HCPP) -o stress_lru_percpu_asan \
	  stress_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_lru_percpu_asan 4 4 8 16 15000
	$(CC) $(ASANFLAGS) -DDC_LRU_MCAS $(HCPP) -o stress_lru_mcas_asan \
	  stress_dcache.c dcache_bucketlock.c $(LDFLAGS) $(LDLIBS)
	./stress_lru_mcas_asan 4 4 8 16 15000

# ---- the deque itself, below the dcache -----------------------------------
#
# test_deque checks the biconditional urcu-txn-deque exists to hold -- owner ==
# d <=> reachable in d's ring -- over the whole node array, after hammering
# push / remove / rotate / retire-and-reuse concurrently.
#
# The arms are the AXES, not repetitions: NDEQUES=1 is the single-ring shape the
# first version of this test had (kept as the regression), 4 and 16 add
# MIGRATION between rings, and the reuse op adds a node whose storage is
# recycled -- which resets `seq` and so is the one event that breaks the ABA
# guard's monotonicity premise.
#
# NO_SEQ_GUARD is a MUTATION TEST, and it is here to keep an uncomfortable fact
# measurable rather than forgotten: the guard has never been shown to be
# load-bearing.  If this arm ever starts failing, the guard has earned its
# place; while it passes, the honest statement is that it is unproven.
#
# ⚠ THE TSAN ARM IS check-deque-tsan, AND IT NEEDS A CURRENT TSAN LIBURCU.
# An earlier revision of this comment said a TSAN gate here would be pure noise
# -- "TSAN cannot model a QSBR grace period" -- on the strength of 20-38 reports
# that all looked like descriptor recycle.  That was wrong twice over: the TSAN
# liburcu was FOUR DAYS STALE (a Jul 31 copy against an Aug 3 engine), and every
# surviving report was a real defect in the harness's own diagnostic reads.
# Rebuilt and fixed, the arm is 0 warnings.  See the recipe above stress-tsan.
check-deque: test_deque.c $(URCU_DEP) | check-lib
	@for w in 2 8 32; do \
	  echo "--- test_deque: $$w writers, 4 deques ---"; \
	  $(CC) $(CFLAGS) -DNWRITERS=$$w $(CPPFLAGS) -o test_deque \
	    test_deque.c $(LDFLAGS) $(LDLIBS) || exit 1; \
	  ./test_deque || exit 1; \
	done
	@echo "--- test_deque: 1 deque (single-ring regression) ---"
	$(CC) $(CFLAGS) -DNWRITERS=8 -DNDEQUES=1 $(CPPFLAGS) -o test_deque_1dq \
	  test_deque.c $(LDFLAGS) $(LDLIBS)
	./test_deque_1dq
	@echo "--- test_deque: 16 deques (many shards, heavy migration) ---"
	$(CC) $(CFLAGS) -DNWRITERS=8 -DNDEQUES=16 $(CPPFLAGS) -o test_deque_16dq \
	  test_deque.c $(LDFLAGS) $(LDLIBS)
	./test_deque_16dq
	@echo "--- test_deque: MUTATION -- seq ABA guard compiled out ---"
	$(CC) $(CFLAGS) -DNWRITERS=32 -DURCU_TXN_DEQUE_NO_SEQ_GUARD $(CPPFLAGS) \
	  -o test_deque_noseq test_deque.c $(LDFLAGS) $(LDLIBS)
	@./test_deque_noseq && \
	  echo "  NOTE: passes without the seq guard -- the guard remains UNPROVEN" \
	  || { echo "  the seq guard is LOAD-BEARING -- record this"; exit 1; }
	@echo "--- test_deque: SEAL retire (owner poison replaces GP #1) ---"
	$(CC) $(CFLAGS) -DNWRITERS=32 -DDEQUE_SEAL_RETIRE $(CPPFLAGS) \
	  -o test_deque_seal test_deque.c $(LDFLAGS) $(LDLIBS)
	./test_deque_seal
	@echo "--- test_deque: CONTROL -- GP #1 dropped WITHOUT the seal (must FAIL) ---"
	$(CC) $(CFLAGS) -DNWRITERS=32 -DDEQUE_NO_GP1 $(CPPFLAGS) \
	  -o test_deque_nogp1 test_deque.c $(LDFLAGS) $(LDLIBS)
	@timeout 90 ./test_deque_nogp1 >/dev/null 2>&1 && { \
	  echo "  CONTROL PASSED -- GP #1 is NOT load-bearing, so the seal arm"; \
	  echo "  above proves NOTHING.  Do not claim the seal replaced it."; \
	  exit 1; } || \
	  echo "  control does not complete, as required: the seal arm is meaningful"
	@echo "--- test_deque: ASan ---"
	$(CC) $(ASANFLAGS) -DNWRITERS=8 $(CPPFLAGS) -o test_deque_asan \
	  test_deque.c $(LDFLAGS) $(LDLIBS)
	./test_deque_asan
	@echo "--- test_deque: harness self-check (-DRETIRE_AUDIT) ---"
	$(CC) $(CFLAGS) -DNWRITERS=8 -DRETIRE_AUDIT $(CPPFLAGS) -o test_deque_audit \
	  test_deque.c $(LDFLAGS) $(LDLIBS)
	./test_deque_audit

# TSAN over the deque.  Needs the TSAN liburcu -- built with
# --enable-compiler-atomic-builtins, else uatomic is inline asm TSAN cannot see
# -- AND KEPT IN SYNC WITH $(URCU_TXN_BUILD): a stale TSAN tree tests a
# different engine and its reports cannot be reasoned about.  The recipe is in
# the comment above stress-tsan; check the sync with
#   md5sum $(URCU_TSAN_INC)/urcu/rcu-txn-mcas.h $(URCU_TXN_INC)/urcu/rcu-txn-mcas.h
#
# TSAN_SLAB_CPP, not SLAB_CPP: the latter describes $(URCU_TXN_BUILD)'s slab
# route, and compiling one route against a library built for the other tests
# neither.
check-deque-tsan: test_deque.c
	@test -f "$(URCU_TSAN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: TSAN liburcu not found at $(URCU_TSAN_LIB) -- see the recipe"; \
	  echo "       in the stress-tsan comment in this Makefile."; exit 1; }
	@cmp -s $(URCU_TSAN_INC)/urcu/rcu-txn-mcas.h $(URCU_TXN_INC)/urcu/rcu-txn-mcas.h || { \
	  echo "ERROR: the TSAN liburcu is out of sync with $(URCU_TXN_BUILD)."; \
	  echo "       Rebuild it (recipe above stress-tsan) -- a stale TSAN tree"; \
	  echo "       reports races in an engine you are not running."; exit 1; }
	@for a in "-DNWRITERS=4" "-DNWRITERS=8" "-DNWRITERS=4 -DNDEQUES=1" \
		  "-DNWRITERS=4 -DNDEQUES=16"; do \
	  echo "--- test_deque TSAN: $$a ---"; \
	  $(CC) $(TSANFLAGS) $(TSAN_SLAB_CPP) $$a -DDURATION_MS=400 \
	    -I$(URCU_TSAN_INC) -I. -o test_deque_tsan test_deque.c \
	    -L$(URCU_TSAN_LIB) -Wl,-rpath,$(URCU_TSAN_LIB) $(LDLIBS) || exit 1; \
	  TSAN_OPTIONS="halt_on_error=0 history_size=4" ./test_deque_tsan || exit 1; \
	done

# LAYOUT arms.  The gates above sweep the causality arms and the chain
# strategies, but they all build the DEFAULT 1-CL split layout -- while the
# benchmark matrix also builds _nosplit (legacy 3-CL: d_seq kept, pos/neg in its
# own d_inode word, no d_iparent tags) and _hot1cl (the fair-weather pack).
# Those are the arms a tag-encoded feature silently breaks, and phase 2 DID
# break both txn _nosplit arms: the gate matrix did not cover the build matrix,
# so the breakage only surfaced when a sweep tried to build.  This gate makes
# the two matrices meet.  It runs the tests, not just the compiler: pos/neg
# lives in a different WORD in this layout, so a build-only check would miss a
# wrong one being published.
check-layouts: test_dcache.c dcache_txn.c dcache_seqlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_NO_HOT1CL_SPLIT $(CPPFLAGS) -o test_dcache_txn_nosplit \
	  test_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_txn_nosplit
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN -DDC_NO_HOT1CL_SPLIT $(CPPFLAGS) \
	  -o test_dcache_pernode_nosplit test_dcache.c dcache_txn.c \
	  $(LDFLAGS) $(LDLIBS)
	./test_dcache_pernode_nosplit
	$(CC) $(CFLAGS) -DDC_HOT1CL $(CPPFLAGS) -o test_dcache_txn_hot1cl \
	  test_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./test_dcache_txn_hot1cl
	$(CC) $(CFLAGS) -DDC_NO_HOT1CL_SPLIT $(CPPFLAGS) \
	  -o test_dcache_seqlock_nosplit test_dcache.c dcache_seqlock.c \
	  $(LDFLAGS) $(LDLIBS)
	./test_dcache_seqlock_nosplit
	$(CC) $(ASANFLAGS) -DDC_NO_HOT1CL_SPLIT $(CPPFLAGS) \
	  -o stress_dcache_nosplit_asan stress_dcache.c dcache_txn.c \
	  $(LDFLAGS) $(LDLIBS)
	./stress_dcache_nosplit_asan 4 4 8 16 15000

# Sanitizer smoke of the harness itself on both engines (small, short).
bench-asan: bench_dcache.c dcache_seqlock.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(ASANFLAGS) $(CPPFLAGS) -o bench_dcache_seqlock_asan \
	  bench_dcache.c dcache_seqlock.c $(LDFLAGS) $(LDLIBS)
	./bench_dcache_seqlock_asan --nthreads 4 --rename-frac 0.3 --ndirs 6 \
	  --depth 3 --leaves 8 --duration 300
	$(CC) $(ASANFLAGS) $(CPPFLAGS) -o bench_dcache_txn_asan \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
	./bench_dcache_txn_asan --nthreads 4 --rename-frac 0.3 --ndirs 6 \
	  --depth 3 --leaves 8 --duration 300

clean:
	rm -f *.o test_dcache test_dcache_asan repro_dcache repro_dcache_asan \
	  bench_dcache_seqlock bench_dcache_seqlock_nosplit \
	  bench_dcache_txn bench_dcache_txn_nosplit \
	  bench_dcache_txn_pernode bench_dcache_txn_pernode_nosplit \
	  bench_dcache_txn_pernode_keepid bench_dcache_txn_hot1cl \
	  test_dcache_hot1cl test_dcache_hot1cl_asan \
	  bench_dcache_seqlock_asan bench_dcache_txn_asan \
	  bench_dcache_height_seqlock bench_dcache_height_txn \
	  bench_dcache_height_txn_pernode \
	  bench_dcache_height_seqlock_asan bench_dcache_height_txn_asan \
	  test_dcache_pernode repro_dcache_pernode stress_dcache_pernode_asan \
	  repro_fold repro_fold_asan \
	  stress_dcache stress_dcache_asan \
	  stress_dcache_tsan stress_dcache_dirs stress_dcache_dirs_asan \
	  stress_dcache_dirs_tsan stress_dcache_xchg stress_dcache_xchg_asan \
	  stress_dcache_xchg_tsan \
	  bench_dcache_churn_bucketlock bench_dcache_bucketlock bench_dcache_height_bucketlock \
	  bench_dcache_bucketlock_swmw bench_dcache_bucketlock_swmw_pad bench_dcache_churn_bucketlock_swmw \
	  bench_dcache_height_bucketlock_swmw bench_dcache_bucketlock_chainlock \
	  test_dcache_bucketlock_chainlock repro_dcache_bucketlock_chainlock repro_samedir_bucketlock_chainlock \
	  repro_fold_bucketlock_chainlock stress_dcache_bucketlock_chainlock_asan stress_dcache_dirs_bucketlock_chainlock_asan \
	  stress_dcache_xchg_bucketlock_chainlock_asan mixed_cycle_bucketlock_chainlock_asan \
	  stress_dcache_bucketlock_chainlock_tsan stress_dcache_xchg_bucketlock_chainlock_tsan \
	  test_dcache_bucketlock repro_dcache_bucketlock repro_samedir_bucketlock repro_fold_bucketlock \
	  stress_dcache_bucketlock_asan stress_dcache_dirs_bucketlock_asan stress_dcache_xchg_bucketlock_asan \
	  mixed_cycle_bucketlock_asan stress_dcache_bucketlock_tsan stress_dcache_xchg_bucketlock_tsan \
	  stress_dcache_dirs_bucketlock_tsan \
	  test_dcache_bucketlock_swmw repro_dcache_bucketlock_swmw repro_samedir_bucketlock_swmw repro_fold_bucketlock_swmw \
	  stress_dcache_bucketlock_swmw_asan stress_dcache_dirs_bucketlock_swmw_asan \
	  stress_dcache_xchg_bucketlock_swmw_asan mixed_cycle_bucketlock_swmw_asan \
	  stress_dcache_bucketlock_swmw_tsan stress_dcache_xchg_bucketlock_swmw_tsan stress_dcache_dirs_bucketlock_swmw_tsan \
	  bench_dcache_churn_seqlock bench_dcache_churn_txn bench_dcache_churn_txn_pernode \
	  bench_dcache_churn_txn_mark
