/*
 * Adaptive Radix Tree (libart) engine for the read/write scaling benchmark.
 * Lock-based (rwlock from common); no RCU, no bind9.
 *
 * ART keys are NUL-terminated, so we pass str_lens[i] + 1 as the key length.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "bench_scale_common.h"

#include "art.h"
#include <stdint.h>

static art_tree g_art;

static void art_build(void)
{
	art_tree_init(&g_art);
	for (unsigned int i = 0; i < N_KEYS; i++)
		art_insert(&g_art, (unsigned char *)str_keys[i],
			str_lens[i] + 1, (void *)(uintptr_t)((i + 1) << 1));
}

static void art_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	pthread_rwlock_rdlock(&g_rwlock);
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		volatile void *sink;
		sink = art_search(&g_art, (unsigned char *)str_keys[idx],
			str_lens[idx] + 1);
		(void)sink;
	}
	pthread_rwlock_unlock(&g_rwlock);
}

static void art_writer_step(void *ctx, uint64_t *seed, unsigned long writes)
{
	(void)ctx;
	(void)writes;
	unsigned int cidx = xorshift64(seed) % CHURN_KEYS;

	pthread_rwlock_wrlock(&g_rwlock);
	{
		void *v = art_search(&g_art, (unsigned char *)churn_keys[cidx],
			churn_lens[cidx] + 1);
		if (v) {
			art_delete(&g_art, (unsigned char *)churn_keys[cidx],
				churn_lens[cidx] + 1);
		} else {
			art_insert(&g_art, (unsigned char *)churn_keys[cidx],
				churn_lens[cidx] + 1,
				(void *)(uintptr_t)((cidx + 1) << 1));
		}
	}
	pthread_rwlock_unlock(&g_rwlock);
}

/* Mutator-benchmark op: art_insert overwrites the value in place (native
 * REPLACE, returns the old value); INSERT/REMOVE by NUL-terminated key. */
static void art_writer_op(void *ctx, int op, unsigned int idx)
{
	(void)ctx;
	pthread_rwlock_wrlock(&g_rwlock);
	switch (op) {
	case BENCH_OP_INSERT:
		art_insert(&g_art, (unsigned char *)churn_keys[idx],
			churn_lens[idx] + 1, (void *)(uintptr_t)((idx + 1) << 1));
		break;
	case BENCH_OP_REPLACE:
		art_insert(&g_art, (unsigned char *)churn_keys[idx],
			churn_lens[idx] + 1,
			(void *)(uintptr_t)(((idx + 1) << 1) + 0x100));
		break;
	case BENCH_OP_REMOVE:
		art_delete(&g_art, (unsigned char *)churn_keys[idx],
			churn_lens[idx] + 1);
		break;
	}
	pthread_rwlock_unlock(&g_rwlock);
}

static const struct bench_engine art_engine = {
	.name		= "art",
	.label		= "ART+rwl",
	.build		= art_build,
	.reader_batch	= art_reader_batch,
	.writer_step	= art_writer_step,
	.writer_op	= art_writer_op,
};

int main(int argc, char **argv)
{
	bench_init_rwlock();
	return bench_scale_main(argc, argv, &art_engine);
}
