/*
 * Fractal Trie (FT) engine for the read/write scaling benchmark.
 *
 * Speculative reference lookup mode (FT's recommended "ft_spec" path):
 * cds_ft_speculative_lookup_key() does a speculative descent and validates the
 * candidate with a library-side memcmp at a fixed key offset
 * (CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE).  This matches the engine the
 * load-names benchmark reports as ft_spec, so the two benchmarks compare the
 * same FT lookup path.
 *
 * Links liburcu only (no bind9), so nothing registers the main thread with
 * RCU on our behalf — main() does it once.  Reader/writer threads are raw
 * pthreads and register themselves.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _LGPL_SOURCE
/*
 * RCU flavor is the ONLY thing -DBENCH_FT_QSBR changes: it picks liburcu-qsbr
 * (the lowest-overhead read side — rcu_read_lock/unlock compile to nothing,
 * readers instead report a quiescent state periodically) over the membarrier
 * liburcu.  Everything else below is identical: the quiescent_state() and
 * thread_online()/thread_offline() calls are real, valid operations under both
 * flavors, so they stay unconditional.  They matter most under QSBR — there an
 * online thread that neither reports a quiescent state nor goes offline stalls
 * every grace period — but are harmless under membarrier (whose grace periods
 * wait on read-side sections, not quiescent states).
 */
#ifdef BENCH_FT_QSBR
#define URCU_API_MAP		/* map generic rcu_* names onto urcu_qsbr_* */
#include <urcu/urcu-qsbr.h>
#else
#define RCU_MEMBARRIER
#include <urcu.h>
#endif
#include <urcu/fractal-trie.h>

#include "bench_scale_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ft_entry {
	struct rcu_head rcu_head;
	struct cds_ft_node ft_node;
	size_t key_len;
	char key[];
};

/*
 * Byte offset from the (struct cds_ft_node *) stored in the trie to the user
 * key bytes, for cds_ft_speculative_lookup_key()'s library-side validation.
 */
#define FT_KEY_OFFSET \
	(offsetof(struct ft_entry, key) - offsetof(struct ft_entry, ft_node))

static struct cds_ft *g_ft;
static pthread_mutex_t g_ft_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Compact arena holding the N_KEYS lookup-set external nodes (the ft_entry
 * that embeds each cds_ft_node + a copy of its key).  The reader's speculative
 * memcmp validates against the key copy here, so packing them contiguously
 * keeps that validating read TLB/cache-friendly (see struct bench_arena).
 * The churn keys are NOT placed here — they are inserted/removed dynamically,
 * so they keep their own malloc'd, RCU-freed ft_entry.
 */
static struct bench_arena ft_str_arena;

/* 16-byte alignment keeps each ft_entry's embedded cds_ft_node aligned. */
#define FT_ENTRY_ALIGN 16

/* Which churn keys are currently inserted, and the live entry for each. */
static struct ft_entry *churn_entries[CHURN_KEYS];

/* RCU callback: free an ft_entry after a grace period. */
static void free_ft_entry_rcu(struct rcu_head *head)
{
	struct ft_entry *e = caa_container_of(head, struct ft_entry, rcu_head);
	free(e);
}

static void ft_build(void)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;

	cds_ft_group_attr_create(&attr);
	cds_ft_group_attr_set_max_key_len(attr, 256);
	cds_ft_group_attr_set_key_len(attr, CDS_FT_LEN_VARIABLE);
	/*
	 * Speculative lookup optimization; the reader uses
	 * cds_ft_speculative_lookup_key (ft_spec), which validates the candidate
	 * with a library-side memcmp at FT_KEY_OFFSET (see ft_reader_batch).
	 */
	cds_ft_group_attr_set_lookup_optimization(attr,
		CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE);
	cds_ft_group_create(attr, &group);
	cds_ft_group_attr_destroy(attr);
	cds_ft_create(group, NULL, &g_ft);

	/* Size the arena exactly: one aligned ft_entry (+ key) per lookup key. */
	size_t arena_bytes = 0;
	for (unsigned int i = 0; i < N_KEYS; i++) {
		size_t slot = sizeof(struct ft_entry) + str_lens[i];
		arena_bytes += (slot + (FT_ENTRY_ALIGN - 1)) & ~(size_t)(FT_ENTRY_ALIGN - 1);
	}
	bench_arena_init(&ft_str_arena, arena_bytes);

	for (unsigned int i = 0; i < N_KEYS; i++) {
		struct cds_ft_node *result;
		struct ft_entry *e = bench_arena_alloc(&ft_str_arena,
			sizeof(*e) + str_lens[i], FT_ENTRY_ALIGN);
		memcpy(e->key, str_keys[i], str_lens[i]);
		e->key_len = str_lens[i];
		cds_ft_insert_unique(g_ft, (const uint8_t *)str_keys[i],
			str_lens[i], &e->ft_node, &result);
		if ((i & 1023) == 0)
			rcu_quiescent_state();
	}
	rcu_quiescent_state();

	/*
	 * Self-check: a present key must resolve via the speculative path, i.e.
	 * FT_KEY_OFFSET must correctly locate the stored key for the library-side
	 * memcmp.  A wrong offset would silently turn every lookup into a miss
	 * (no crash, plausible throughput), so guard it here once.
	 */
	{
		struct cds_ft_node *probe = NULL;
		rcu_read_lock();
		cds_ft_speculative_lookup_key(g_ft, (const uint8_t *)str_keys[0],
			str_lens[0], str_lens[0], FT_KEY_OFFSET, &probe);
		rcu_read_unlock();
		if (probe == NULL) {
			fprintf(stderr, "FT spec self-check FAILED: present key "
				"str_keys[0] not found (bad FT_KEY_OFFSET?)\n");
			abort();
		}
	}

	/*
	 * Drop the main thread offline for the remainder of the sweep.  It does
	 * RCU work only here (build) and in ft_cleanup_churn (which briefly
	 * brings it back online); in between it sleeps through each timed window,
	 * where (under QSBR) an online idle thread would block the writer's grace
	 * periods.
	 */
	rcu_thread_offline();
}

static void *ft_reader_setup(void)
{
	rcu_register_thread();
	return NULL;
}

static void ft_reader_teardown(void *ctx)
{
	(void)ctx;
	rcu_unregister_thread();
}

/* Sink so the optimizer cannot drop the validating memcmp (result unused). */
static volatile unsigned long ft_reader_sink;

static void ft_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	unsigned long acc = 0;
	rcu_read_lock();
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		struct cds_ft_node *found = NULL;
		/*
		 * Speculative descent + library-side memcmp validation (ft_spec).
		 * key_readable_pad = str_lens[idx] matches the prior candidate
		 * call's descent behavior; FT_KEY_OFFSET locates the stored key.
		 */
		cds_ft_speculative_lookup_key(g_ft,
			(const uint8_t *)str_keys[idx],
			str_lens[idx], str_lens[idx], FT_KEY_OFFSET, &found);
		if (found) {
			/*
			 * Force-read the validated leaf's first byte: prevents
			 * DCE of the speculative memcmp and pays the same
			 * post-lookup cache-line touch as load-names' force_read
			 * and HOTRowex's contentEquals.
			 */
			asm volatile("" :: "r"(*(const volatile uint8_t *)found)
				: "memory");
			acc++;
		}
	}
	rcu_read_unlock();
	ft_reader_sink = acc;
	rcu_quiescent_state();
}

static void *ft_writer_setup(void)
{
	struct cds_ft_iter *ft_iter;
	rcu_register_thread();
	cds_ft_iter_create(g_ft, &ft_iter);
	return ft_iter;
}

static void ft_writer_teardown(void *ctx)
{
	cds_ft_iter_destroy((struct cds_ft_iter *)ctx);
	rcu_unregister_thread();
}

static void ft_writer_step(void *ctx, uint64_t *seed, unsigned long writes)
{
	struct cds_ft_iter *ft_iter = ctx;
	unsigned int cidx = xorshift64(seed) % CHURN_KEYS;

	pthread_mutex_lock(&g_ft_mutex);
	if (churn_entries[cidx]) {
		/* Remove the currently-present churn key. */
		struct ft_entry *old = churn_entries[cidx];
		enum cds_ft_status ls, rs;
		cds_ft_iter_invalidate_cache(ft_iter);
		ls = cds_ft_iter_set_key(ft_iter,
			(const uint8_t *)churn_keys[cidx], churn_lens[cidx]);
		if (ls < 0) {
			fprintf(stderr, "iter_set_key error: %d\n", ls);
			abort();
		}
		ls = cds_ft_lookup(g_ft, ft_iter);
		if (ls != CDS_FT_STATUS_OK) {
			fprintf(stderr, "lookup failed: %d cidx=%u key=%.*s\n",
				ls, cidx, (int)churn_lens[cidx], churn_keys[cidx]);
			abort();
		}
		rs = cds_ft_remove(g_ft, ft_iter, &old->ft_node);
		if (rs != CDS_FT_STATUS_OK) {
			fprintf(stderr, "remove failed: %d\n", rs);
			abort();
		}
		churn_entries[cidx] = NULL;
		pthread_mutex_unlock(&g_ft_mutex);
		/* Defer the free until after a grace period. */
		call_rcu(&old->rcu_head, free_ft_entry_rcu);
	} else {
		/* Insert the currently-absent churn key. */
		struct ft_entry *e = calloc(1, sizeof(*e) + churn_lens[cidx]);
		struct cds_ft_node *result;
		enum cds_ft_status status;
		if (!e) {
			fprintf(stderr, "OOM\n");
			abort();
		}
		memcpy(e->key, churn_keys[cidx], churn_lens[cidx]);
		e->key_len = churn_lens[cidx];
		status = cds_ft_insert_unique(g_ft,
			(const uint8_t *)churn_keys[cidx],
			churn_lens[cidx], &e->ft_node, &result);
		if (status == CDS_FT_STATUS_OK) {
			churn_entries[cidx] = e;
		} else if (status == CDS_FT_STATUS_DUPLICATE_FOUND) {
			free(e);
		} else {
			fprintf(stderr, "insert_unique error: %d\n", status);
			abort();
		}
		pthread_mutex_unlock(&g_ft_mutex);
	}

	if (writes % 1000 == 0)
		rcu_quiescent_state();
}

/* Remove the entry currently mapped at churn key @idx (must be present). */
static void ft_churn_remove(struct cds_ft_iter *it, unsigned int idx)
{
	struct ft_entry *old = churn_entries[idx];
	enum cds_ft_status ls, rs;

	cds_ft_iter_invalidate_cache(it);
	ls = cds_ft_iter_set_key(it, (const uint8_t *)churn_keys[idx],
		churn_lens[idx]);
	if (ls < 0) {
		fprintf(stderr, "iter_set_key error: %d\n", ls);
		abort();
	}
	ls = cds_ft_lookup(g_ft, it);
	if (ls != CDS_FT_STATUS_OK) {
		fprintf(stderr, "lookup failed: %d\n", ls);
		abort();
	}
	rs = cds_ft_remove(g_ft, it, &old->ft_node);
	if (rs != CDS_FT_STATUS_OK) {
		fprintf(stderr, "remove failed: %d\n", rs);
		abort();
	}
	churn_entries[idx] = NULL;
	call_rcu(&old->rcu_head, free_ft_entry_rcu);
}

/* Insert a fresh node (the mutator's value) for churn key @idx (must be absent). */
static void ft_churn_insert(unsigned int idx)
{
	struct ft_entry *e = calloc(1, sizeof(*e) + churn_lens[idx]);
	struct cds_ft_node *result;
	enum cds_ft_status status;

	if (!e) {
		fprintf(stderr, "OOM\n");
		abort();
	}
	memcpy(e->key, churn_keys[idx], churn_lens[idx]);
	e->key_len = churn_lens[idx];
	status = cds_ft_insert_unique(g_ft, (const uint8_t *)churn_keys[idx],
		churn_lens[idx], &e->ft_node, &result);
	if (status != CDS_FT_STATUS_OK) {
		fprintf(stderr, "insert_unique error/dup: %d\n", status);
		abort();
	}
	churn_entries[idx] = e;
}

/*
 * Mutator-benchmark op.  FT has no in-place value update, so REPLACE swaps the
 * leaf for a fresh node (remove old + RCU-free, then insert new) — the natural
 * "replace" cost for an RCU trie.  churn_entries[] tracks the live node so the
 * driver's drain can empty the churn set between reader-count points.  Single
 * mutator, so the (uncontended) g_ft_mutex only mirrors the read-sweep writer;
 * FT readers stay lock-free throughout.
 */
static void ft_writer_op(void *ctx, int op, unsigned int idx)
{
	struct cds_ft_iter *it = ctx;

	pthread_mutex_lock(&g_ft_mutex);
	switch (op) {
	case BENCH_OP_INSERT:
		ft_churn_insert(idx);
		break;
	case BENCH_OP_REPLACE:
		if (churn_entries[idx])
			ft_churn_remove(it, idx);
		ft_churn_insert(idx);
		break;
	case BENCH_OP_REMOVE:
		if (churn_entries[idx])
			ft_churn_remove(it, idx);
		break;
	}
	pthread_mutex_unlock(&g_ft_mutex);

	if ((idx & 1023) == 0)
		rcu_quiescent_state();
}

static void ft_run_reset(void)
{
	memset(churn_entries, 0, sizeof(churn_entries));
}

static void ft_cleanup_churn(void)
{
	/*
	 * Runs on the main thread, already RCU-registered by main(); do NOT
	 * register again here.
	 */
	struct cds_ft_iter *cleanup_iter;
	/* Back online: ft_build / the previous cleanup left main offline. */
	rcu_thread_online();
	cds_ft_iter_create(g_ft, &cleanup_iter);
	pthread_mutex_lock(&g_ft_mutex);
	for (int i = 0; i < CHURN_KEYS; i++) {
		if (churn_entries[i]) {
			cds_ft_iter_set_key(cleanup_iter,
				(const uint8_t *)churn_keys[i], churn_lens[i]);
			cds_ft_lookup(g_ft, cleanup_iter);
			cds_ft_remove(g_ft, cleanup_iter,
				&churn_entries[i]->ft_node);
			rcu_quiescent_state();
			free(churn_entries[i]);
			churn_entries[i] = NULL;
		}
	}
	pthread_mutex_unlock(&g_ft_mutex);
	cds_ft_iter_destroy(cleanup_iter);

	/*
	 * Opt-in (FT_BENCH_COMPACT): restore the trie's descent locality after
	 * this run's insert/remove churn.  cds_ft_compact() is a copying GC-style
	 * recompact; it permits concurrent RCU readers but excludes writers, and
	 * here the run's threads have already joined, so the trie is quiescent.
	 * Leaves the next thread-count point measuring a freshly-compacted shape
	 * (closer to how BIND9-QP stays compact via dns_qp_compact during churn).
	 * rcu_barrier() flushes the deferred node frees so the drained ranges are
	 * reclaimed before the next run.  Note: the reported RSS is sampled once
	 * after build (pre-sweep), so this affects throughput/shape, not that RSS.
	 */
	int do_compact = (getenv("FT_BENCH_COMPACT") != NULL);
	if (do_compact)
		cds_ft_compact(g_ft);		/* an online RCU writer op */
	/*
	 * Return main to its sweep-long offline state — and crucially do so
	 * BEFORE the blocking rcu_barrier() below: under QSBR a thread that
	 * blocks while online would stall the very grace period it waits on.
	 */
	rcu_thread_offline();
	if (do_compact)
		rcu_barrier();
}

static const struct bench_engine ft_engine = {
#ifdef BENCH_FT_QSBR
	.name		= "ft_qsbr",
	.label		= "FT spec (QSBR)",
#else
	.name		= "ft",
	.label		= "FT spec",
#endif
	.build		= ft_build,
	.reader_setup	= ft_reader_setup,
	.reader_batch	= ft_reader_batch,
	.reader_teardown = ft_reader_teardown,
	.writer_setup	= ft_writer_setup,
	.writer_step	= ft_writer_step,
	.writer_teardown = ft_writer_teardown,
	.run_reset	= ft_run_reset,
	.cleanup_churn	= ft_cleanup_churn,
	.writer_op	= ft_writer_op,
};

int main(int argc, char **argv)
{
	/*
	 * Register the main thread with RCU exactly once.  This executable does
	 * NOT link libisc, so no ELF constructor does it for us.  (The BIND9-QP
	 * executable must NOT do this — libisc's isc__lib_initialize() already
	 * registers main, and a second registration corrupts the RCU registry.)
	 */
	rcu_register_thread();
	int ret = bench_scale_main(argc, argv, &ft_engine);
	/* ft_build / ft_cleanup_churn leave main offline; balance before unregister. */
	rcu_thread_online();
	rcu_unregister_thread();
	return ret;
}
