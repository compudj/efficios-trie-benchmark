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

CC       ?= gcc
CFLAGS   ?= -O2 -g -pthread -Wall -Wextra -march=native
CPPFLAGS := -I$(URCU_TXN_INC) -I.
LDFLAGS  := -L$(URCU_TXN_LIB) -Wl,-rpath,$(URCU_TXN_LIB)
LDLIBS   := -lurcu-qsbr -lurcu-common -lpthread

# The default 1-CL split layout returns the host ADDRESS as identity.  The
# correctness harnesses assert logical ids (id==gid torn-read checks), so they
# build -DDC_SPLIT_KEEPID: same 1-CL layout, but dc_lookup reads the cold d_id and
# returns it.  The bench (census-gated) uses the address default.
KEEPID   := -DDC_SPLIT_KEEPID
HCPP     := $(KEEPID) $(CPPFLAGS)

ENGINE   ?= seqlock
HDRS     := dcache.h seqcount.h

.PHONY: all check check-asan repro repro-asan repro-fold	stress stress-asan stress-tsan stress-dirs stress-dirs-asan \
	stress-dirs-tsan xchg xchg-asan xchg-tsan bench bench-check bench-asan \
	check-pernode check-mark repro-samedir churn check-churn clean check-lib

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
URCU_TSAN_LIB   := $(URCU_TSAN_BUILD)/src/.libs
stress-tsan: stress_dcache.c dcache_txn.c $(HDRS)
	@test -f "$(URCU_TSAN_LIB)/liburcu-qsbr.so" || { \
	  echo "ERROR: TSAN liburcu not found at $(URCU_TSAN_LIB) -- see the recipe"; \
	  echo "       in the stress-tsan comment in this Makefile."; exit 1; }
	$(CC) $(TSANFLAGS) $(KEEPID) -I$(URCU_TSAN_INC) -I. -o stress_dcache_tsan \
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
	$(CC) $(TSANFLAGS) $(KEEPID) -I$(URCU_TSAN_INC) -I. -o stress_dcache_dirs_tsan \
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
	$(CC) $(TSANFLAGS) $(KEEPID) -I$(URCU_TSAN_INC) -I. -o stress_dcache_xchg_tsan \
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
       bench_dcache_txn_mark churn

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
CHURN_ENGINES := seqlock txn txn_pernode txn_mark
churn: $(addprefix bench_dcache_churn_,$(CHURN_ENGINES))

bench_dcache_churn_seqlock: bench_dcache_churn.c dcache_seqlock.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -o $@ bench_dcache_churn.c dcache_seqlock.c $(LDFLAGS) $(LDLIBS)
bench_dcache_churn_txn: bench_dcache_churn.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) $(HCPP) -o $@ bench_dcache_churn.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
bench_dcache_churn_txn_pernode: bench_dcache_churn.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_PER_NODE_GEN $(HCPP) -o $@ bench_dcache_churn.c dcache_txn.c $(LDFLAGS) $(LDLIBS)
bench_dcache_churn_txn_mark: bench_dcache_churn.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(HCPP) -o $@ bench_dcache_churn.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

# Quick correctness gate for the churn invariant (state + census + ids).
check-churn: churn
	./bench_dcache_churn_txn_mark --writers 6 --readers 6 --duration 1500
	./bench_dcache_churn_txn_pernode --writers 6 --readers 6 --duration 1500
	./bench_dcache_churn_seqlock --writers 6 --readers 6 --duration 1500

# Localized walk causality with NO counter: the hlist deletion mark is the
# version, so d_seq is gone and DC_NAME_MAX rises 32 -> 40.  Layout is otherwise
# identical (d_hash still at @56, sizeof(dentry) still 160), and dc_qstr_eq
# short-circuits on hash/len, so the wider name array is a capability win rather
# than a confound for this benchmark's short names.
bench_dcache_txn_mark: bench_dcache.c dcache_txn.c $(HDRS) | check-lib
	$(CC) $(CFLAGS) -DDC_MARK_GEN $(CPPFLAGS) -o $@ \
	  bench_dcache.c dcache_txn.c $(LDFLAGS) $(LDLIBS)

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
	bench_dcache_height_txn_pernode bench_dcache_height_txn_mark

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
	  stress_dcache_xchg_tsan
