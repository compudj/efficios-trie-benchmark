// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_negdir.c -- deterministic exercise of the NEGATIVE-DIRECTORY guard pair
 * on the lock-free (all-MW) engine.
 *
 * rmdir-to-negative needs one invariant: a negative directory must not GAIN a
 * child.  On the lock-bearing engines that is a lock both sides already take.
 * On this one it is a pair of transactional guards:
 *
 *   dc_add    WRITES parent->d_child_head  and GUARDS parent->d_iparent
 *   d_delete  GUARDS host->d_child_head    and WRITES host->d_iparent
 *
 * Each side's write set hits the other's read set, so the two cannot both
 * commit -- in EITHER order.  BOTH guards are needed, and one guard is NOT
 * enough, which is exactly what a single-threaded test cannot show: there each
 * side's own up-front check already answers, so removing either guard still
 * passes.  (Verified: both mutants pass test_dcache.)  The guards only ever
 * fire against a concurrent peer, so they need this.
 *
 * Two interleavings, one per direction, forced through rendezvous hooks fired
 * between each side's decision and its commit:
 *
 *   A. ADD-then-DELETE-wins.  dc_add reads the parent POSITIVE and parks.
 *      d_delete runs to completion, making the directory negative.  dc_add
 *      resumes and commits.
 *      CORRECT: the add ABORTS on its parent guard, retries, sees negative,
 *      returns -ENOENT.  BROKEN: a child lands under a negative directory.
 *
 *   B. DELETE-then-ADD-wins.  d_delete finds the child list EMPTY and parks.
 *      dc_add runs to completion, adding a child.  d_delete resumes and commits.
 *      CORRECT: the delete ABORTS on its child-head guard, retries, sees the
 *      child, returns -ENOTEMPTY.  BROKEN: the directory goes negative with a
 *      child under it.
 *
 * Build/run: make repro-negdir.  Exit 0 = both directions held.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <urcu-qsbr.h>

#include "dcache.h"

extern void (*dc_test_add_hook)(void);
extern void (*dc_test_del_hook)(void);

static struct dcache *g_dc;
static sem_t g_parked;		/* parked side -> peer: decision made */
static sem_t g_peer_done;	/* peer -> parked side: go commit */
static int   g_fired;		/* one-shot: only the first pass parks */
static int   g_peer_ret;	/* what the peer returned */

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

static void park_once(void)
{
	if (!g_fired) {
		g_fired = 1;
		sem_post(&g_parked);
		sem_wait_quiescent(&g_peer_done);
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

/* ---- direction A: dc_add parks, d_delete wins -------------------------- */

static void *a_deleter(void *arg)
{
	struct dc_path p;

	(void) arg;
	dc_register_thread();
	sem_wait_quiescent(&g_parked);		/* the add has read the parent */
	g_peer_ret = dc_delete(g_dc, path_of(&p, "/r/dir"));
	sem_post(&g_peer_done);			/* let the add commit */
	dc_unregister_thread();
	return NULL;
}

static int direction_a(void)
{
	struct dc_path p;
	pthread_t th;
	uint64_t id = ~0ULL;
	int r, fail = 0;

	g_fired = 0;
	dc_test_add_hook = park_once;
	pthread_create(&th, NULL, a_deleter, NULL);

	r = dc_add_file(g_dc, path_of(&p, "/r/dir/kid"), 91);

	pthread_join(th, NULL);
	dc_test_add_hook = NULL;

	if (!g_fired) {
		fprintf(stderr, "A VACUOUS: the add hook never fired\n");
		return 2;
	}
	if (g_peer_ret != 0) {
		fprintf(stderr, "A: the concurrent d_delete failed (%d)\n",
			g_peer_ret);
		return 2;
	}
	if (r != -ENOENT) {
		fprintf(stderr,
			"A FAIL: dc_add returned %d, expected -ENOENT -- its "
			"guard on the parent's state did not fire, so a child "
			"landed under a NEGATIVE directory\n", r);
		fail = 1;
	}
	if (dc_lookup(g_dc, path_of(&p, "/r/dir/kid"), &id) != DC_ABSENT) {
		fprintf(stderr, "A FAIL: /r/dir/kid exists under a negative dir\n");
		fail = 1;
	}
	printf("  A add-parks/delete-wins : %s\n", fail ? "FAIL" : "ok");
	return fail;
}

/* ---- direction B: d_delete parks, dc_add wins --------------------------- */

static void *b_adder(void *arg)
{
	struct dc_path p;

	(void) arg;
	dc_register_thread();
	sem_wait_quiescent(&g_parked);		/* the delete saw it empty */
	g_peer_ret = dc_add_file(g_dc, path_of(&p, "/r/dir2/kid"), 92);
	sem_post(&g_peer_done);			/* let the delete commit */
	dc_unregister_thread();
	return NULL;
}

static int direction_b(void)
{
	struct dc_path p;
	pthread_t th;
	uint64_t id = ~0ULL;
	int r, fail = 0;

	g_fired = 0;
	dc_test_del_hook = park_once;
	pthread_create(&th, NULL, b_adder, NULL);

	r = dc_delete(g_dc, path_of(&p, "/r/dir2"));

	pthread_join(th, NULL);
	dc_test_del_hook = NULL;

	if (!g_fired) {
		fprintf(stderr, "B VACUOUS: the delete hook never fired\n");
		return 2;
	}
	if (g_peer_ret != 0) {
		fprintf(stderr, "B: the concurrent dc_add failed (%d)\n",
			g_peer_ret);
		return 2;
	}
	if (r != -ENOTEMPTY) {
		fprintf(stderr,
			"B FAIL: dc_delete returned %d, expected -ENOTEMPTY -- "
			"its guard on the child list did not fire, so the "
			"directory went NEGATIVE with a child under it\n", r);
		fail = 1;
	}
	if (dc_lookup(g_dc, path_of(&p, "/r/dir2"), &id) != DC_POSITIVE) {
		fprintf(stderr, "B FAIL: /r/dir2 is not positive\n");
		fail = 1;
	}
	printf("  B delete-parks/add-wins : %s\n", fail ? "FAIL" : "ok");
	return fail;
}

int main(void)
{
	struct dc_path p;
	int fail = 0;

	rcu_register_thread();
	g_dc = dc_create(1024);
	sem_init(&g_parked, 0, 0);
	sem_init(&g_peer_done, 0, 0);

	printf("== repro_negdir (engine: %s) ==\n", dc_engine_name());

	if (!dc_delete_dir_supported) {
		printf("  SKIP: this engine declines rmdir-to-negative\n");
		printf("RESULT: SKIP\n");
		return 0;
	}

	if (dc_add(g_dc, path_of(&p, "/r"), 1) != 0 ||
	    dc_add(g_dc, path_of(&p, "/r/dir"), 2) != 0 ||
	    dc_add(g_dc, path_of(&p, "/r/dir2"), 3) != 0) {
		fprintf(stderr, "seed failed\n");
		return 2;
	}

	fail |= direction_a();
	fail |= direction_b();

	dc_destroy(g_dc);
	rcu_unregister_thread();
	printf("RESULT: %s\n", fail ? "FAIL" : "PASS (both guards fire)");
	return fail != 0;
}
