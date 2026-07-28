// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_dcache_samedir.c -- the SAME-DIRECTORY twin of repro_dcache.c.
 *
 * repro_dcache.c moves the interior directory to a different parent
 * (/A/B -> /Z/B).  That made it easy to assume walk causality is a
 * CROSS-PARENT concern and that a same-directory rename, which does not change
 * the tree's shape, needs no version signal at all.  It is not, and it does.
 *
 * The hazard is a stale INTERIOR RESOLUTION, not a parent change: latch /A/B,
 * let it be renamed within its own parent, then read a child inserted under its
 * new name.  The walker returns a node for a path that existed at no instant.
 *
 *   walker: resolve /A/B/K
 *     step "A" -> X_A
 *     step "B" -> X_B          <-- latch interior dir, then BLOCK (hook)
 *                                     writer: rename /A/B -> /A/B2  (SAME parent)
 *                                     writer: add    /A/B2/K = 99   (planted child)
 *     step "K" under X_B -> finds the planted node -> returns 99 for /A/B/K  (BUG)
 *
 * Correct result: ABSENT.  Before the writer ran, K did not exist; afterwards the
 * path is /A/B2/K and /A/B does not resolve.  POSITIVE id=99 for /A/B/K has no
 * linearization point.
 *
 * WHY THIS EXISTS.  No other harness covers it: stress_dcache and
 * stress_dcache_dirs both force the destination directory to differ
 * (`if (nd == pos[i]) nd = (nd + 1) % D;`), so every rename they perform is
 * cross-parent; stress_dcache_xchg does produce same-directory pairs but swaps
 * LEAVES, so no interior node ever moves and its checks are terminal-only.  A
 * change that signalled walk causality only on cross-parent renames therefore
 * passed the entire suite while reintroducing this exact anomaly -- which is how
 * this file came to be written.
 *
 * All three arms (global, DC_PER_NODE_GEN, DC_IPARENT_SKIP) pass; the point is to
 * keep it that way.
 *
 * Build/run: make repro-samedir (txn engine, -DDC_TEST_HOOKS).
 * Exit 0 = no anomaly; exit 1 = ANOMALY reproduced.
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
#define INSERTED_ID   99u	/* id the writer plants at /A/B2/K */

static struct dcache *g_dc;
static sem_t g_walker_reached;	/* walker -> writer: X_B latched */
static sem_t g_writer_done;	/* writer -> walker: rename+insert applied */
static int   g_fired;		/* one-shot: only the first descent blocks */

static enum dc_result g_walker_res;
static uint64_t       g_walker_id;

/*
 * Park on @s WITHOUT stalling grace periods.
 *
 * Under QSBR a registered thread counts as being inside a read-side section
 * until it says otherwise, so a thread that blocks here while online holds off
 * EVERY grace period process-wide.  When the peer this rendezvous waits for is
 * itself waiting on a grace period, that is a three-way deadlock: we wait for
 * the peer, the peer waits for the GP, the GP waits for us.  It presents as an
 * INTERMITTENT hang, because it turns on whether the parked thread registered
 * before the GP sampled the registry -- so it reads like one engine's bug
 * rather than a harness bug affecting every engine.
 *
 * No engine here waits on a grace period on the writer side today, so this is
 * currently a no-op; it is the discipline, not an optimization, and one was
 * prototyped that did (see the git history).  stress_dcache.c and bench_dcache.c
 * already carry the same guard on their main thread across pthread_join.
 *
 * Gated on was_online because these are state TRANSITIONS, not a nesting count:
 * forcing a thread offline that was already offline and then back online would
 * hand the caller a state it did not have.  Same guard liburcu puts on its own
 * offline dance (urcu-qsbr.c, synchronize_rcu).  No barrier needed -- the
 * semaphore pair is the ordering here.
 *
 * ONLY for threads that hold no RCU-protected reference across the park.  A
 * walker parked mid-descent must NOT use this: staying inside its read section
 * is precisely what these repros are testing.
 */
static void sem_wait_quiescent(sem_t *s)
{
	unsigned long was_online = rcu_read_ongoing();

	if (was_online)
		rcu_thread_offline();
	sem_wait(s);
	if (was_online)
		rcu_thread_online();
}

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
	sem_wait_quiescent(&g_walker_reached);	/* wait until the walker latched X_B */
	if (dc_rename(g_dc, path_of(&from, "/A/B"), path_of(&to, "/A/B2")) != 0) {
		fprintf(stderr, "rename /A/B -> /A/B2 (SAME DIR) failed\n");
		exit(2);
	}
	if (dc_add(g_dc, path_of(&zbk, "/A/B2/K"), INSERTED_ID) != 0) {
		fprintf(stderr, "add /A/B2/K failed\n");
		exit(2);
	}
	sem_post(&g_writer_done);		/* let the walker resume into "K" */
	dc_unregister_thread();
	return NULL;
}

int main(void)
{
	pthread_t walker, writer;
	struct dc_path a, ab, abk;
	int anomaly;

	rcu_register_thread();
	g_dc = dc_create(1024);
	printf("== repro_dcache_samedir (engine: %s) ==\n", dc_engine_name());

	if (dc_add(g_dc, path_of(&a, "/A"), 1) ||
	    dc_add(g_dc, path_of(&ab, "/A/B"), 2)) {
		fprintf(stderr, "setup failed\n");
		return 2;
	}

	sem_init(&g_walker_reached, 0, 0);
	sem_init(&g_writer_done, 0, 0);
	dc_test_walk_hook = walk_hook;

	pthread_create(&walker, NULL, walker_fn, path_of(&abk, "/A/B/K"));
	pthread_create(&writer, NULL, writer_fn, NULL);
	/*
	 * Main registered with RCU but reports no quiescent state while parked in
	 * pthread_join, so leaving it online stalls EVERY grace period process-
	 * wide -- and any writer that waits on one then deadlocks against us.
	 * Same guard stress_dcache.c and bench_dcache.c already carry.
	 */
	rcu_thread_offline();
	pthread_join(walker, NULL);
	pthread_join(writer, NULL);
	rcu_thread_online();

	dc_test_walk_hook = NULL;

	printf("walker resolved /A/B/K -> %s",
	       g_walker_res == DC_ABSENT ? "ABSENT" :
	       g_walker_res == DC_POSITIVE ? "POSITIVE" : "NEGATIVE");
	if (g_walker_res == DC_POSITIVE)
		printf(" id=%llu", (unsigned long long) g_walker_id);
	printf("\n");

	anomaly = (g_walker_res == DC_POSITIVE && g_walker_id == INSERTED_ID);
	if (anomaly) {
		printf("ANOMALY: /A/B/K resolved to the node planted at /A/B2/K "
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
