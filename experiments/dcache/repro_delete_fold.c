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
 * slot -- which the fold breaks.  The fix is a cmpxchg on both sides.
 *
 * Final state after the fold drains: /d/f was deleted and never re-instantiated,
 * so it MUST read NEGATIVE.  Broken engine: POSITIVE, carrying the old id.
 *
 * Build/run: make repro-delete-fold.  Exit 0 = state preserved; 1 = lost.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <urcu-qsbr.h>

#include "dcache.h"

extern void (*dc_test_transfer_hook)(void);

#define LEAF_ID 42u

static struct dcache *g_dc;
static sem_t g_in_transfer;	/* fold -> main: read done, write pending */
static sem_t g_deleted;		/* main -> fold: state change published */
static int   g_hook_fired;	/* one-shot */

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
 * Runs on the call_rcu fold worker, INSIDE the TRANSFER, between the read of
 * the host's d_iparent and the write-back.  One-shot: later folds (including
 * the retry a correct cmpxchg performs) must not re-park, or the run deadlocks.
 */
static void transfer_hook(void)
{
	if (!g_hook_fired) {
		g_hook_fired = 1;
		sem_post(&g_in_transfer);
		sem_wait_quiescent(&g_deleted);
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
