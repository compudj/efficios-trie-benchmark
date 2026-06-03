/*
 * qp-trie engine (Tony Finch's qp-trie via the Tbl dispatch + qp backend) for
 * the read/write scaling benchmark.  Lock-based (rwlock from common); no RCU,
 * no bind9.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "bench_scale_common.h"

#include <stdbool.h>	/* Tbl.h uses bool without including it */
#include <stdint.h>
#include "Tbl.h"

static Tbl *g_qp;

/*
 * qp-trie values MUST be 4-byte aligned (low 2 bits clear): the Trie union
 * overlays leaf.val with the branch's `flags:2` field, and isbranch() == (flags
 * != 0).  A value with bit 0 or 1 set makes that leaf read as a branch and
 * corrupts the trie (silently breaking lookups and iteration).  So shift by 2,
 * not 1.  (The read sweep never validated lookup results, so a 2-aligned value
 * here went unnoticed until ordered iteration exposed it.)
 */
static void qp_build(void)
{
	for (unsigned int i = 0; i < N_KEYS; i++)
		g_qp = Tsetl(g_qp, str_keys[i], str_lens[i],
			(void *)(uintptr_t)((i + 1) << 2));
}

static void qp_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	pthread_rwlock_rdlock(&g_rwlock);
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		volatile void *sink;
		sink = Tgetl(g_qp, str_keys[idx], str_lens[idx]);
		(void)sink;
	}
	pthread_rwlock_unlock(&g_rwlock);
}

static void qp_writer_step(void *ctx, uint64_t *seed, unsigned long writes)
{
	(void)ctx;
	(void)writes;
	unsigned int cidx = xorshift64(seed) % CHURN_KEYS;

	pthread_rwlock_wrlock(&g_rwlock);
	{
		void *v = Tgetl(g_qp, churn_keys[cidx], churn_lens[cidx]);
		if (v) {
			g_qp = Tdell(g_qp, churn_keys[cidx], churn_lens[cidx]);
		} else {
			g_qp = Tsetl(g_qp, churn_keys[cidx], churn_lens[cidx],
				(void *)(uintptr_t)((cidx + 1) << 2));
		}
	}
	pthread_rwlock_unlock(&g_rwlock);
}

/* Mutator-benchmark op: Tsetl overwrites the value in place (native REPLACE);
 * INSERT/REMOVE by key.  Values stay even (low bit reserved by Tbl). */
static void qp_writer_op(void *ctx, int op, unsigned int idx)
{
	(void)ctx;
	pthread_rwlock_wrlock(&g_rwlock);
	switch (op) {
	case BENCH_OP_INSERT:
		g_qp = Tsetl(g_qp, churn_keys[idx], churn_lens[idx],
			(void *)(uintptr_t)((idx + 1) << 2));
		break;
	case BENCH_OP_REPLACE:
		g_qp = Tsetl(g_qp, churn_keys[idx], churn_lens[idx],
			(void *)(uintptr_t)(((idx + 1) << 2) + 0x100));
		break;
	case BENCH_OP_REMOVE:
		g_qp = Tdell(g_qp, churn_keys[idx], churn_lens[idx]);
		break;
	}
	pthread_rwlock_unlock(&g_rwlock);
}

/* Ordered-iteration op: Tnextl walks all keys in sorted order (NULL = start). */
static unsigned long qp_iterate(void *ctx)
{
	const char *key = NULL;
	size_t klen = 0;
	void *val;
	unsigned long n = 0;

	(void)ctx;
	pthread_rwlock_rdlock(&g_rwlock);
	while (Tnextl(g_qp, &key, &klen, &val))
		n++;
	pthread_rwlock_unlock(&g_rwlock);
	return n;
}

static const struct bench_engine qp_engine = {
	.name		= "qp",
	.label		= "QP+rwl",
	.build		= qp_build,
	.reader_batch	= qp_reader_batch,
	.writer_step	= qp_writer_step,
	.writer_op	= qp_writer_op,
	.iterate	= qp_iterate,
};

int main(int argc, char **argv)
{
	bench_init_rwlock();
	return bench_scale_main(argc, argv, &qp_engine);
}
