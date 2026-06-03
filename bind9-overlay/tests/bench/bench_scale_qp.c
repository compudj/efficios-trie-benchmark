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

/* Mutator-benchmark op: Tsetl overwrites the value in place (native REPLACE);
 * INSERT/REMOVE by key.  Values stay even (low bit reserved by Tbl). */
static void qp_writer_op(void *ctx, int op, unsigned int idx)
{
	(void)ctx;
	pthread_rwlock_wrlock(&g_rwlock);
	switch (op) {
	case BENCH_OP_INSERT:
		g_qp = Tsetl(g_qp, churn_keys[idx], churn_lens[idx],
			(void *)(uintptr_t)((idx + 1) << 1));
		break;
	case BENCH_OP_REPLACE:
		g_qp = Tsetl(g_qp, churn_keys[idx], churn_lens[idx],
			(void *)(uintptr_t)(((idx + 1) << 1) + 0x100));
		break;
	case BENCH_OP_REMOVE:
		g_qp = Tdell(g_qp, churn_keys[idx], churn_lens[idx]);
		break;
	}
	pthread_rwlock_unlock(&g_rwlock);
}

static const struct bench_engine qp_engine = {
	.name		= "qp",
	.label		= "QP+rwl",
	.build		= qp_build,
	.reader_batch	= qp_reader_batch,
	.writer_step	= qp_writer_step,
	.writer_op	= qp_writer_op,
};

int main(int argc, char **argv)
{
	bench_init_rwlock();
	return bench_scale_main(argc, argv, &qp_engine);
}
