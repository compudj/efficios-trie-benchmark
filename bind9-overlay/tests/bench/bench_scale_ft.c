/*
 * Fractal Trie (FT) engine for the read/write scaling benchmark.
 *
 * Candidate lookup mode: the reader gets a candidate node back and validates
 * the key caller-side with memcmp (CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE).
 *
 * Links liburcu only (no bind9), so nothing registers the main thread with
 * RCU on our behalf — main() does it once.  Reader/writer threads are raw
 * pthreads and register themselves.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _LGPL_SOURCE
#define RCU_MEMBARRIER
#include <urcu.h>
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

static struct cds_ft *g_ft;
static pthread_mutex_t g_ft_mutex = PTHREAD_MUTEX_INITIALIZER;

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
	 * Speculative is the default lookup optimization; the reader validates
	 * each candidate caller-side via memcmp (see ft_reader_batch), so the
	 * library needs no key offsets.
	 */
	cds_ft_group_attr_set_lookup_optimization(attr,
		CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE);
	cds_ft_group_create(attr, &group);
	cds_ft_group_attr_destroy(attr);
	cds_ft_create(group, NULL, &g_ft);

	for (unsigned int i = 0; i < N_KEYS; i++) {
		struct cds_ft_node *result;
		struct ft_entry *e = calloc(1, sizeof(*e) + str_lens[i]);
		memcpy(e->key, str_keys[i], str_lens[i]);
		e->key_len = str_lens[i];
		cds_ft_insert_unique(g_ft, (const uint8_t *)str_keys[i],
			str_lens[i], &e->ft_node, &result);
		if ((i & 1023) == 0)
			rcu_quiescent_state();
	}
	rcu_quiescent_state();
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

static void ft_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	rcu_read_lock();
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		struct cds_ft_node *found;
		cds_ft_lookup_candidate_key(g_ft,
			(const uint8_t *)str_keys[idx],
			str_lens[idx], str_lens[idx], &found);
		if (found) {
			struct ft_entry *e = cds_ft_entry(found,
				struct ft_entry, ft_node);
			if (e->key_len != str_lens[idx] ||
			    memcmp(e->key, str_keys[idx], e->key_len) != 0)
				found = NULL;
		}
	}
	rcu_read_unlock();
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
	if (getenv("FT_BENCH_COMPACT") != NULL) {
		cds_ft_compact(g_ft);
		rcu_barrier();
	}
}

static const struct bench_engine ft_engine = {
	.name		= "ft",
	.label		= "FT cand+v",
	.build		= ft_build,
	.reader_setup	= ft_reader_setup,
	.reader_batch	= ft_reader_batch,
	.reader_teardown = ft_reader_teardown,
	.writer_setup	= ft_writer_setup,
	.writer_step	= ft_writer_step,
	.writer_teardown = ft_writer_teardown,
	.run_reset	= ft_run_reset,
	.cleanup_churn	= ft_cleanup_churn,
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
	rcu_unregister_thread();
	return ret;
}
