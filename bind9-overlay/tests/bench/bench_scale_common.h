/*
 * Shared scaffolding for the per-engine read/write scaling benchmarks.
 *
 * Each engine (FT, Judy, qp-trie, ART, BIND9-QP) is built into its OWN
 * executable so that its resident-set size (RSS) can be measured in
 * isolation — one process holds exactly one data structure.  The driver
 * script scripts/run_scale_rw.sh runs all of them and assembles the
 * combined table.  This header is the contract between the common driver
 * (bench_scale_common.c) and each engine's thin main (bench_scale_<eng>.c).
 *
 * Why per-process matters beyond RSS: BIND9's libisc ELF constructor
 * (isc__lib_initialize) calls rcu_register_thread() for the main thread and
 * leaves it registered.  Only the BIND9-QP executable links libisc, so the
 * other four engines have no second registrant and simply register the main
 * thread themselves.  Registering the same thread twice double-adds its one
 * urcu_reader to the RCU registry list and wedges every grace period.
 */
#ifndef BENCH_SCALE_COMMON_H
#define BENCH_SCALE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N_KEYS		1000000
#define DURATION_SEC	3
#define BATCH_SIZE	1000
#define CHURN_KEYS	10000
#define N_DOMAINS	24

/*
 * Lookup key set (all in the structure at build time) and churn key set
 * (toggled in/out by the writer).  Filled by bench_gen_keys(); shared by
 * every engine so the workloads are identical.
 */
extern char  *str_keys[N_KEYS];
extern size_t str_lens[N_KEYS];
extern char  *churn_keys[CHURN_KEYS];
extern size_t churn_lens[CHURN_KEYS];

/* xorshift64 PRNG — the benchmark's only randomness source. */
uint64_t xorshift64(uint64_t *s);

/* Current process resident set size in kB (VmRSS from /proc/self/status). */
long get_rss_kb(void);

/* Populate str_keys/churn_keys with the synthetic DNS-like key sets. */
void bench_gen_keys(void);

/*
 * Writer-preferring rwlock shared by the lock-based engines (Judy/qp/ART).
 * Call bench_init_rwlock() once before use.  Unused by the RCU engines.
 */
extern pthread_rwlock_t g_rwlock;
void bench_init_rwlock(void);

/*
 * Compact bump-allocator arena for an engine's external-node storage.
 *
 * Every engine packs its N_KEYS lookup external nodes (FT: the ft_entry that
 * embeds the cds_ft_node + a copy of the key; HOTRowex: a copy of the key the
 * stored value points at) into one contiguous mmap'd region rather than
 * scattering them across the heap with per-entry malloc.  This matters for a
 * fair lookup comparison: the validating read each lookup does (FT's
 * library-side memcmp at the stored key, HOTRowex's contentEquals) then hits a
 * compact, TLB-friendly region instead of a cold random cache line — and,
 * crucially, neither engine validates against the shared query-key buffer
 * (storing a pointer straight into the input would make validation almost
 * free and unfairly favour that engine).  Pages fault in under the caller's
 * NUMA policy, so BENCH_NUMA_INTERLEAVE interleaves the arena too.
 *
 * Bump-only: no per-slot free (the N_KEYS lookup set is never deleted; the
 * writer's churn keys use their own short-lived allocations).
 */
struct bench_arena {
	char  *base;
	size_t off;
	size_t cap;
};
void  bench_arena_init(struct bench_arena *a, size_t cap_bytes);
void *bench_arena_alloc(struct bench_arena *a, size_t sz, size_t align);

/*
 * Per-engine operations.  Callbacks marked "optional" may be NULL.
 *
 * Threading model (driven by bench_scale_common.c):
 *   - build()          runs once on the main thread before the sweep.
 *   - reader_setup()   runs at the start of each spawned reader thread; its
 *                      return value is passed back as @ctx to reader_batch /
 *                      reader_teardown (e.g. nothing, or an RCU registration).
 *   - reader_batch()   performs BATCH_SIZE random lookups under the engine's
 *                      read-side synchronization.  Used for both the timed
 *                      loop and the (default-on) cache-priming warm-up.
 *   - writer_setup()   runs at the start of the single writer thread; @ctx is
 *                      passed to writer_step / writer_teardown (e.g. an FT
 *                      iterator).
 *   - writer_step()    performs one churn operation; @writes is the count of
 *                      steps done so far (for periodic rcu_quiescent_state()).
 *   - run_reset()      optional; resets per-run churn bookkeeping on the main
 *                      thread before each thread-count point.
 *   - cleanup_churn()  optional; removes leftover churn entries on the main
 *                      thread after each thread-count point.
 */
/*
 * Mutator-benchmark op codes (see writer_op below).  The mutator sweep
 * (BENCH_MUTATOR=1) measures one mutator thread's insert/replace/remove
 * throughput as the reader count scales 0 -> N.
 */
enum bench_op { BENCH_OP_INSERT = 0, BENCH_OP_REPLACE = 1, BENCH_OP_REMOVE = 2 };

struct bench_engine {
	const char *name;	/* short id used on the CLI / in output: "ft" … */
	const char *label;	/* human label for the table column header */

	void  (*build)(void);

	void *(*reader_setup)(void);				/* optional */
	void  (*reader_batch)(void *ctx, uint64_t *seed);
	void  (*reader_teardown)(void *ctx);			/* optional */

	void *(*writer_setup)(void);				/* optional */
	void  (*writer_step)(void *ctx, uint64_t *seed, unsigned long writes);
	void  (*writer_teardown)(void *ctx);			/* optional */

	void  (*run_reset)(void);				/* optional */
	void  (*cleanup_churn)(void);				/* optional */

	/*
	 * Mutator-benchmark hook (optional, enabled by BENCH_MUTATOR=1).  Perform
	 * exactly ONE operation on churn key @idx in [0,CHURN_KEYS):
	 *   BENCH_OP_INSERT  — key currently absent -> present (value A)
	 *   BENCH_OP_REPLACE — key present, value A -> a distinct value B
	 *   BENCH_OP_REMOVE  — key present -> absent
	 * The driver calls it under @ctx (from writer_setup) in timed insert-all /
	 * replace-all / remove-all phases over the whole churn set while N readers
	 * run, so each op's rate is measured in isolation.  An engine with no
	 * concurrent delete (HOTRowex) sets no_remove=1: the remove phase is then
	 * skipped and INSERT/REPLACE act as upserts.
	 */
	void  (*writer_op)(void *ctx, int op, unsigned int idx);
	int   no_remove;
};

/*
 * Generic entry point.  Each engine's main() calls this with its vtable:
 *   int main(int argc, char **argv) { return bench_scale_main(argc, argv, &eng); }
 * Optional arg: argv[1] caps the max thread count (default 384).
 */
int bench_scale_main(int argc, char **argv, const struct bench_engine *eng);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_SCALE_COMMON_H */
