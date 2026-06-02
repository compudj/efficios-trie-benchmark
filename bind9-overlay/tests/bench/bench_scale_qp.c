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

static void qp_build(void)
{
	for (unsigned int i = 0; i < N_KEYS; i++)
		g_qp = Tsetl(g_qp, str_keys[i], str_lens[i],
			(void *)(uintptr_t)((i + 1) << 1));
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
				(void *)(uintptr_t)((cidx + 1) << 1));
		}
	}
	pthread_rwlock_unlock(&g_rwlock);
}

static const struct bench_engine qp_engine = {
	.name		= "qp",
	.label		= "QP+rwl",
	.build		= qp_build,
	.reader_batch	= qp_reader_batch,
	.writer_step	= qp_writer_step,
};

int main(int argc, char **argv)
{
	bench_init_rwlock();
	return bench_scale_main(argc, argv, &qp_engine);
}
