// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_dcache.c -- deterministic 1-writer + 1-walker reproduction of the
 * walk-CAUSALITY anomaly the shell mechanism does NOT cover (rename_lock's job,
 * not d_seq's).  See rename-shell-transition.md "Walk causality".
 *
 * The shell move keeps a single component coherent (no d_seq tear), but a
 * multi-component walk is not atomic: a walker can latch an interior directory,
 * have it moved out from under it, then read a child inserted at the directory's
 * NEW location -- returning a node that was never at the walked path at any
 * instant.  On the current (no rename-generation) txn engine this reproduces;
 * once dc_lookup brackets the walk on a rename generation counter it must not.
 *
 * Interleaving, forced on demand through a mid-descent rendezvous hook:
 *
 *   walker: resolve /A/B/K
 *     step "A" -> X_A
 *     step "B" -> X_B          <-- latch interior dir, then BLOCK (hook)
 *                                     writer: rename /A/B -> /Z/B   (X_B relocates)
 *                                     writer: add    /Z/B/K = 99    (planted child)
 *     step "K" under X_B -> finds the planted node -> returns 99 for /A/B/K  (BUG)
 *
 * Correct result: /A/B/K is ABSENT -- it was ABSENT before the writer ran (K did
 * not exist), and after the move /A/B does not resolve at all; the planted node
 * lives only at /Z/B/K.  So POSITIVE id=99 for /A/B/K has no linearization point.
 *
 * Build/run: make repro    (txn engine, -DDC_TEST_HOOKS).
 * Exit 0 = no anomaly (engine has the fix); exit 1 = ANOMALY reproduced.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <urcu-qsbr.h>

#include "dcache.h"

/* Installed into the txn engine's dc_lookup (compiled with DC_TEST_HOOKS). */
extern void (*dc_test_walk_hook)(int depth);

#define BLOCK_DEPTH   1		/* pause after latching /A/B (comp index 1) */
#define INSERTED_ID   99u	/* id the writer plants at /Z/B/K */

static struct dcache *g_dc;
static sem_t g_walker_reached;	/* walker -> writer: X_B latched */
static sem_t g_writer_done;	/* writer -> walker: rename+insert applied */
static int   g_fired;		/* one-shot: only the first descent blocks */

static enum dc_result g_walker_res;
static uint64_t       g_walker_id;

static struct dc_path *path_of(struct dc_path *p, const char *s)
{
	if (dc_path_parse(p, s) != 0) {
		fprintf(stderr, "bad path %s\n", s);
		exit(2);
	}
	return p;
}

/* Runs on the WALKER thread, inside its RCU read-side section. */
static void walk_hook(int depth)
{
	if (depth == BLOCK_DEPTH && !g_fired) {
		g_fired = 1;
		sem_post(&g_walker_reached);
		sem_wait(&g_writer_done);
	}
}

static void *walker_fn(void *arg)
{
	dc_register_thread();
	g_walker_res = dc_lookup(g_dc, (const struct dc_path *) arg, &g_walker_id);
	dc_unregister_thread();
	return NULL;
}

static void *writer_fn(void *arg)
{
	struct dc_path from, to, zbk;

	(void) arg;
	dc_register_thread();
	sem_wait(&g_walker_reached);		/* wait until the walker latched X_B */
	if (dc_rename(g_dc, path_of(&from, "/A/B"), path_of(&to, "/Z/B")) != 0) {
		fprintf(stderr, "rename /A/B -> /Z/B failed\n");
		exit(2);
	}
	if (dc_add(g_dc, path_of(&zbk, "/Z/B/K"), INSERTED_ID) != 0) {
		fprintf(stderr, "add /Z/B/K failed\n");
		exit(2);
	}
	sem_post(&g_writer_done);		/* let the walker resume into "K" */
	dc_unregister_thread();
	return NULL;
}

int main(void)
{
	pthread_t walker, writer;
	struct dc_path a, ab, z, abk;
	int anomaly;

	rcu_register_thread();
	g_dc = dc_create(1024);
	printf("== repro_dcache (engine: %s) ==\n", dc_engine_name());

	if (dc_add(g_dc, path_of(&a, "/A"), 1) ||
	    dc_add(g_dc, path_of(&ab, "/A/B"), 2) ||
	    dc_add(g_dc, path_of(&z, "/Z"), 5)) {
		fprintf(stderr, "setup failed\n");
		return 2;
	}

	sem_init(&g_walker_reached, 0, 0);
	sem_init(&g_writer_done, 0, 0);
	dc_test_walk_hook = walk_hook;

	pthread_create(&walker, NULL, walker_fn, path_of(&abk, "/A/B/K"));
	pthread_create(&writer, NULL, writer_fn, NULL);
	pthread_join(walker, NULL);
	pthread_join(writer, NULL);

	dc_test_walk_hook = NULL;

	printf("walker resolved /A/B/K -> %s",
	       g_walker_res == DC_ABSENT ? "ABSENT" :
	       g_walker_res == DC_POSITIVE ? "POSITIVE" : "NEGATIVE");
	if (g_walker_res == DC_POSITIVE)
		printf(" id=%llu", (unsigned long long) g_walker_id);
	printf("\n");

	anomaly = (g_walker_res == DC_POSITIVE && g_walker_id == INSERTED_ID);
	if (anomaly) {
		printf("ANOMALY: /A/B/K resolved to the node planted at /Z/B/K "
		       "(never at /A/B/K at any instant)\n");
		printf("RESULT: FAIL (walk-causality bug reproduced)\n");
	} else {
		printf("RESULT: PASS (no misdirection)\n");
	}

	sem_destroy(&g_walker_reached);
	sem_destroy(&g_writer_done);
	dc_destroy(g_dc);
	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
