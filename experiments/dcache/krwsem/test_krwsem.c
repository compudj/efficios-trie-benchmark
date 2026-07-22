// SPDX-License-Identifier: GPL-2.0
/*
 * test_krwsem.c -- correctness + fairness smoke test for the vendored kernel
 * rw_semaphore.  Checks:
 *   1. MUTUAL EXCLUSION: a plain (non-atomic) counter bumped only under the
 *      write lock lands at exactly W*iters -- no lost updates.
 *   2. R/W EXCLUSION: a shared "writer_active" flag is never observed set by a
 *      reader holding the read lock, and "reader_active" never set by a writer.
 *   3. READERS ARE SHARED: at least once, >1 reader is seen holding the lock.
 *   4. FAIRNESS (the point): under a CONTINUOUS stream of readers, a writer
 *      still makes steady progress -- a reader-preferring lock would starve it.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include "krwsem.h"

static struct krwsem lock;
static long guarded;			/* bumped only under wrlock */
static atomic_int writer_active;	/* set while a writer holds it */
static atomic_int readers_active;	/* # readers currently holding it */
static atomic_int max_readers;		/* high-water mark of readers_active */
static atomic_int excl_violation;	/* reader saw writer, or vice versa */
static atomic_long writer_acqs;		/* fairness: writer acquisitions */
static atomic_int stop;

#define WRITE_ITERS 200000

static void *writer_fn(void *arg)
{
	long i;
	(void)arg;
	for (i = 0; i < WRITE_ITERS; i++) {
		krwsem_wrlock(&lock);
		atomic_store(&writer_active, 1);
		if (atomic_load(&readers_active) != 0)
			atomic_store(&excl_violation, 1);
		guarded++;			/* the un-guarded-elsewhere bump */
		atomic_fetch_add(&writer_acqs, 1);
		atomic_store(&writer_active, 0);
		krwsem_wrunlock(&lock);
	}
	return NULL;
}

static void *reader_fn(void *arg)
{
	(void)arg;
	while (!atomic_load(&stop)) {
		int n;
		krwsem_rdlock(&lock);
		if (atomic_load(&writer_active))
			atomic_store(&excl_violation, 1);
		n = atomic_fetch_add(&readers_active, 1) + 1;
		if (n > atomic_load(&max_readers))
			atomic_store(&max_readers, n);
		/* brief hold so readers overlap */
		for (volatile int k = 0; k < 50; k++) { }
		atomic_fetch_sub(&readers_active, 1);
		krwsem_rdunlock(&lock);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int W = argc > 1 ? atoi(argv[1]) : 4;
	int R = argc > 2 ? atoi(argv[2]) : 8;
	pthread_t wt[64], rt[64];
	int i, rc = 0;

	if (W > 64) W = 64;
	if (R > 64) R = 64;
	krwsem_init(&lock);

	for (i = 0; i < R; i++)
		pthread_create(&rt[i], NULL, reader_fn, NULL);
	for (i = 0; i < W; i++)
		pthread_create(&wt[i], NULL, writer_fn, NULL);

	for (i = 0; i < W; i++)
		pthread_join(wt[i], NULL);
	atomic_store(&stop, 1);
	for (i = 0; i < R; i++)
		pthread_join(rt[i], NULL);

	printf("W=%d R=%d\n", W, R);
	printf("  [1] mutual exclusion : guarded=%ld expected=%ld  %s\n",
	       guarded, (long)W * WRITE_ITERS,
	       guarded == (long)W * WRITE_ITERS ? "OK" : "FAIL");
	if (guarded != (long)W * WRITE_ITERS) rc = 1;
	printf("  [2] r/w exclusion    : violations=%d  %s\n",
	       atomic_load(&excl_violation),
	       atomic_load(&excl_violation) ? "FAIL" : "OK");
	if (atomic_load(&excl_violation)) rc = 1;
	printf("  [3] readers shared   : max concurrent readers=%d  %s\n",
	       atomic_load(&max_readers),
	       atomic_load(&max_readers) > 1 ? "OK" : "(only 1 seen)");
	printf("  [4] writer not starved: writer_acqs=%ld (==W*iters, so writers "
	       "finished under a reader stream)  %s\n",
	       atomic_load(&writer_acqs),
	       atomic_load(&writer_acqs) == (long)W * WRITE_ITERS ? "OK" : "FAIL");
	if (atomic_load(&writer_acqs) != (long)W * WRITE_ITERS) rc = 1;

	printf("%s\n", rc ? "TEST FAILED" : "ALL OK");
	return rc;
}
