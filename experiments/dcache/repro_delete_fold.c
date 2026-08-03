// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_delete_fold.c -- deterministic reproduction of the LOST STATE CHANGE
 * between the fold's TRANSFER and a concurrent d_delete / d_instantiate.
 *
 * Until phase 2, the fold's TRANSFER was the only writer of a reachable host's
 * d_iparent, so its read-modify-write ("adopt the outgoing top's parent, keep
 * my own shell and pos/neg bits") could be a plain load followed by a plain
 * store.  d0e7955 recorded that, and recorded when it would stop being true:
 *
 *     "benign today only because rename preserves inode-ness, so n.pos/neg ==
 *      m.pos/neg makes the write value-idempotent -- but it is UB and a latent
 *      correctness bug once phase-2 negative dentries land."
 *
 * Phase 2 landed.  d_delete and d_instantiate flip pos/neg on a live, reachable
 * host from another thread, so two read-modify-writes now share one word and
 * one of them can be lost -- silently, with both callers returning success:
 *
 *   fold worker                          caller
 *   -----------                          ------
 *   TRANSFER: read m->d_iparent
 *             (sees POSITIVE)     <-- PAUSE here (transfer hook)
 *                                        dc_delete(name) -> publishes NEGATIVE,
 *                                                           returns 0
 *   TRANSFER: write back
 *             parent | POSITIVE          <-- the delete is GONE
 *
 * A transaction does not fix it and was the first thing tried: store_sw()
 * "parks it with a plain store that never fails" and an SW-only commit never
 * contention-aborts, because SW asserts exclusion across EVERY writer of the
 * slot -- which the fold breaks.
 *
 * THE TWO ENGINES CLOSE IT DIFFERENTLY, and this repro holds both to the same
 * assertion:
 *
 *   dcache_txn (all-MW)  is lock-free by design, so it leaves the fold and the
 *                        state change CONCURRENT and makes each an atomic RMW.
 *                        The deleter runs to completion inside the window.
 *   dcache_bucketlock    EXCLUDES them: the fold already holds the named top's
 *                        bucket head across its handover, so d_delete takes the
 *                        same lock (and re-verifies the top under it, as
 *                        dc_unlink does) and the fold keeps a plain store.  The
 *                        deleter BLOCKS inside the window.
 *
 * The fold lock is deliberately NOT what bucketlock uses: the hierarchy is
 * {fold locks < bucket-head locks}, and the fold takes fold_lock(host) before
 * its buckets, so reaching for it with a bucket in hand would be ABBA.  It is
 * also unnecessary -- the TRANSFER writes the top's immediate SUCCESSOR while
 * d_delete writes the chain TAIL, and those coincide only when the chain is
 * top->host, where both hold the same bucket.
 *
 * Final state after the fold drains: /d/g was deleted and never re-instantiated,
 * so it MUST read NEGATIVE.  Broken engine: POSITIVE, carrying the old id.
 * Mutation-verified both ways -- restoring the plain RMW (txn) or dropping the
 * bucket lock (bucketlock) makes each report the loss.
 *
 * Build/run: make repro-delete-fold.  Exit 0 = state preserved; 1 = lost.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <urcu-qsbr.h>

#include "dcache.h"

extern void (*dc_test_transfer_hook)(void);

#define LEAF_ID 42u

static struct dcache *g_dc;
static sem_t g_in_transfer;	/* fold -> main: read done, write pending */
static sem_t g_deleted;		/* main -> fold: state change published */
static int   g_hook_fired;	/* one-shot */
static int   g_rendezvous;	/* did the delete complete INSIDE the window? */

/* See repro_fold.c: parking while RCU-online stalls every grace period. */
static void sem_wait_quiescent(sem_t *s)
{
	unsigned long was_online = rcu_read_ongoing();

	if (was_online)
		rcu_thread_offline();
	sem_wait(s);
	if (was_online)
		rcu_thread_online();
}

/*
 * TIMED variant, and the timeout is load-bearing rather than defensive.
 *
 * The two correct designs close this window in DIFFERENT ways, and the repro has
 * to accept both.  The all-MW engine leaves the fold and the state change
 * concurrent and makes each an atomic RMW, so the deleter runs to completion
 * inside the window and posts -- the rendezvous completes and we proceed
 * immediately.  The bucket-lock engine EXCLUDES them: the fold holds the named
 * top's bucket head across its handover, so the deleter blocks on that lock and
 * can never post while we are parked here.  Waiting unconditionally would
 * deadlock the second design rather than test it.
 *
 * So: time out, proceed, and let the delete land after the lock is dropped.  The
 * final assertion is the same either way -- the state change must survive.  A
 * BROKEN engine (plain read-modify-write, no lock) is still caught
 * deterministically, because there the deleter completes and posts at once.
 */
static int sem_timedwait_quiescent(sem_t *s, int ms)
{
	unsigned long was_online = rcu_read_ongoing();
	struct timespec ts;
	int r;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long) ms * 1000000L;
	ts.tv_sec += ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;

	if (was_online)
		rcu_thread_offline();
	do {
		r = sem_timedwait(s, &ts);
	} while (r != 0 && errno == EINTR);
	if (was_online)
		rcu_thread_online();
	return r;			/* 0 = peer posted; -1 = timed out */
}

/*
 * Runs on the call_rcu fold worker, INSIDE the TRANSFER, between the read of
 * the host's d_iparent and the write-back.  One-shot: later folds (including
 * the retry a correct cmpxchg performs) must not re-park, or the run deadlocks.
 */
static void transfer_hook(void)
{
	if (!g_hook_fired) {
		g_hook_fired = 1;
		sem_post(&g_in_transfer);
		g_rendezvous = (sem_timedwait_quiescent(&g_deleted, 250) == 0);
	}
}

static struct dc_path *path_of(struct dc_path *p, const char *s)
{
	if (dc_path_parse(p, s) != 0) {
		fprintf(stderr, "bad path %s\n", s);
		exit(2);
	}
	return p;
}

static void *deleter(void *arg)
{
	struct dc_path p;
	int r;

	(void) arg;
	dc_register_thread();

	/* Wait until the fold has READ the host's word but not yet written it. */
	sem_wait(&g_in_transfer);

	r = dc_delete(g_dc, path_of(&p, "/d/g"));
	if (r != 0)
		fprintf(stderr, "delete: dc_delete returned %d\n", r);

	sem_post(&g_deleted);		/* let the TRANSFER write back */
	dc_unregister_thread();
	return NULL;
}

int main(void)
{
	struct dc_path a, b;
	pthread_t th;
	uint64_t id = ~0ULL;
	enum dc_result res;
	int fail = 0;

	rcu_register_thread();
	g_dc = dc_create(1024);
	sem_init(&g_in_transfer, 0, 0);
	sem_init(&g_deleted, 0, 0);

	printf("== repro_delete_fold (engine: %s) ==\n", dc_engine_name());

	if (dc_add(g_dc, path_of(&a, "/d"), 1) != 0 ||
	    dc_add_file(g_dc, path_of(&a, "/d/f"), LEAF_ID) != 0) {
		fprintf(stderr, "seed failed\n");
		return 2;
	}

	/*
	 * Rename to stack a shell, so the named top and the content host are
	 * DIFFERENT nodes and a fold is queued.  Without this the host is the
	 * top, no TRANSFER runs, and there is no window to race.
	 */
	if (dc_rename(g_dc, path_of(&a, "/d/f"), path_of(&b, "/d/g")) != 0) {
		fprintf(stderr, "rename failed\n");
		return 2;
	}

	dc_test_transfer_hook = transfer_hook;
	pthread_create(&th, NULL, deleter, NULL);

	/* Drive the fold: it runs on a call_rcu worker after a grace period. */
	dc_quiescent();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();

	pthread_join(th, NULL);
	dc_test_transfer_hook = NULL;

	/* Let the chain finish collapsing. */
	dc_quiescent();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();

	if (!g_hook_fired) {
		fprintf(stderr,
			"VACUOUS: the TRANSFER hook never fired -- no fold ran, "
			"so the race window was never opened\n");
		return 2;
	}

	printf("  window: the delete %s inside the TRANSFER\n",
	       g_rendezvous ? "COMPLETED (engines that leave the two concurrent)"
			    : "BLOCKED  (engines that exclude them by lock)");

	res = dc_lookup(g_dc, path_of(&a, "/d/g"), &id);
	if (res != DC_NEGATIVE) {
		fprintf(stderr,
			"FAIL: /d/g reads %s (id %lu) -- the d_delete was LOST: "
			"the TRANSFER wrote back the pos/neg bit it read before "
			"the delete published\n",
			res == DC_POSITIVE ? "POSITIVE" : "ABSENT",
			(unsigned long) id);
		fail = 1;
	}

	dc_destroy(g_dc);
	rcu_unregister_thread();
	printf("RESULT: %s\n", fail ? "FAIL (state change lost)"
				    : "PASS (state change preserved)");
	return fail;
}
