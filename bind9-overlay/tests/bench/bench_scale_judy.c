/*
 * JudySL engine for the read/write scaling benchmark.
 * Lock-based (writer-preferring rwlock from the common module); no RCU, no
 * bind9 — this executable links neither liburcu nor libisc.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "bench_scale_common.h"

#include <Judy.h>
#include <stdint.h>

static Pvoid_t g_judy;

static void judy_build(void)
{
	for (unsigned int i = 0; i < N_KEYS; i++) {
		Word_t *pv;
		JSLI(pv, g_judy, (uint8_t *)str_keys[i]);
		*pv = i;
	}
}

static void judy_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	pthread_rwlock_rdlock(&g_rwlock);
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		Word_t *pv;
		JSLG(pv, g_judy, (uint8_t *)str_keys[idx]);
		(void)pv;
	}
	pthread_rwlock_unlock(&g_rwlock);
}

static void judy_writer_step(void *ctx, uint64_t *seed, unsigned long writes)
{
	(void)ctx;
	(void)writes;
	unsigned int cidx = xorshift64(seed) % CHURN_KEYS;

	pthread_rwlock_wrlock(&g_rwlock);
	{
		Word_t *pv;
		int rc;
		JSLG(pv, g_judy, (uint8_t *)churn_keys[cidx]);
		if (pv) {
			JSLD(rc, g_judy, (uint8_t *)churn_keys[cidx]);
			(void)rc;
		} else {
			JSLI(pv, g_judy, (uint8_t *)churn_keys[cidx]);
			*pv = cidx;
		}
	}
	pthread_rwlock_unlock(&g_rwlock);
}

static const struct bench_engine judy_engine = {
	.name		= "judy",
	.label		= "Judy+rwl",
	.build		= judy_build,
	.reader_batch	= judy_reader_batch,
	.writer_step	= judy_writer_step,
};

int main(int argc, char **argv)
{
	bench_init_rwlock();
	return bench_scale_main(argc, argv, &judy_engine);
}
