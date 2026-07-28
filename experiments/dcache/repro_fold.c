// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_fold.c -- deterministic reproduction of the concurrent-re-rename
 * fold-overlap bug in the SYNCHRONOUS fold (CONCURRENCY-TODO #1).
 *
 * shell_move stacks a shell (demoting the content host out of every bucket)
 * then immediately folds it back, ASSUMING its shell is still the named top.
 * If another rename of the same entry interposes between the stack and the fold
 * (stacking a newer shell on top), that assumption breaks: the fold should
 * instead splice out as a middle relay.  The synchronous fold does not, so the
 * entry corrupts.
 *
 * Interleaving, forced through the shell_move rendezvous hook:
 *
 *   A: rename /d0/n -> /d1/n
 *        stack shell_A in d1, demote n          <-- PAUSE here (fold hook)
 *                                 B: rename /d1/n -> /d2/n  (reaches n via shell_A)
 *        fold  ...                              <-- resumes on a superseded shell
 *
 * Correct final state (quiescent): the leaf (id 7) is reachable at exactly one
 * path, /d2/n.  A fold-overlap bug leaves it at two paths, a broken chain, or a
 * crash -- caught by a conservation walk.
 *
 * Build/run: make repro-fold.  Exit 0 = conserved (engine fixed); 1 = corrupted.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <urcu-qsbr.h>

#include "dcache.h"

extern void (*dc_test_fold_hook)(void);

#define LEAF_ID   7u

static struct dcache *g_dc;
static sem_t g_a_stacked;	/* A -> B: shell stacked, n demoted */
static sem_t g_b_done;		/* B -> A: re-rename applied, fold may proceed */
static int   g_fold_fired;	/* one-shot: only writer A pauses */

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

/* Runs on writer A's thread, inside shell_move, after the stack. */
static void fold_hook(void)
{
	if (!g_fold_fired) {
		g_fold_fired = 1;
		sem_post(&g_a_stacked);
		sem_wait_quiescent(&g_b_done);
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

static void *writer_a(void *arg)
{
	struct dc_path from, to;

	(void) arg;
	dc_register_thread();
	/* Pauses inside shell_move (fold_hook) after stacking. */
	if (dc_rename(g_dc, path_of(&from, "/d0/n"), path_of(&to, "/d1/n")) != 0)
		fprintf(stderr, "A: rename /d0/n -> /d1/n failed\n");
	dc_unregister_thread();
	return NULL;
}

static void *writer_b(void *arg)
{
	struct dc_path from, to;
	int ret;

	(void) arg;
	dc_register_thread();
	sem_wait_quiescent(&g_a_stacked);	/* wait until A stacked + demoted n */
	ret = dc_rename(g_dc, path_of(&from, "/d1/n"), path_of(&to, "/d2/n"));
	fprintf(stderr, "B: rename /d1/n -> /d2/n = %d\n", ret);
	sem_post(&g_b_done);			/* let A's fold proceed */
	dc_unregister_thread();
	return NULL;
}

/* ---- conservation walk ---- */

struct census {
	int leaf_count;			/* nodes with id == LEAF_ID */
	char paths[8][128];
	int npaths;
};

static void path_to_str(const struct dc_path *p, char *out, size_t outsz)
{
	size_t off = 0;
	uint32_t i;

	if (p->ndepth == 0) {
		snprintf(out, outsz, "/");
		return;
	}
	for (i = 0; i < p->ndepth; i++)
		off += snprintf(out + off, outsz - off, "/%s", p->comp[i].name);
}

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	if (id == LEAF_ID) {
		c->leaf_count++;
		if (c->npaths < 8)
			path_to_str(p, c->paths[c->npaths++], sizeof(c->paths[0]));
	}
}

int main(void)
{
	pthread_t a, b;
	struct dc_path d0, d1, d2, n;
	struct census c;
	int anomaly, i;

	rcu_register_thread();
	g_dc = dc_create(1024);
	printf("== repro_fold (engine: %s) ==\n", dc_engine_name());

	if (dc_add(g_dc, path_of(&d0, "/d0"), 100) ||
	    dc_add(g_dc, path_of(&d1, "/d1"), 101) ||
	    dc_add(g_dc, path_of(&d2, "/d2"), 102) ||
	    dc_add(g_dc, path_of(&n, "/d0/n"), LEAF_ID)) {
		fprintf(stderr, "setup failed\n");
		return 2;
	}

	sem_init(&g_a_stacked, 0, 0);
	sem_init(&g_b_done, 0, 0);
	dc_test_fold_hook = fold_hook;

	pthread_create(&a, NULL, writer_a, NULL);
	pthread_create(&b, NULL, writer_b, NULL);
	/*
	 * Main registered with RCU but reports no quiescent state while parked in
	 * pthread_join, so leaving it online stalls EVERY grace period process-
	 * wide -- and any writer that waits on one then deadlocks against us.
	 * Same guard stress_dcache.c and bench_dcache.c already carry.
	 */
	rcu_thread_offline();
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	rcu_thread_online();
	dc_test_fold_hook = NULL;

	memset(&c, 0, sizeof(c));
	dc_walk(g_dc, census_cb, &c);

	printf("child-hlist: leaf (id %u) at %d path(s):", LEAF_ID, c.leaf_count);
	for (i = 0; i < c.npaths; i++)
		printf(" %s", c.paths[i]);
	printf("\n");

	/*
	 * Cross-check the NAME-HASH (lookup) against the child-hlist (walk).  The
	 * MCAS child-hlist keeps its own membership consistent, so a fold overlap
	 * that corrupts only the shell chain / name-bucket is invisible to
	 * dc_walk -- catch it by lookup.  The two indexes must agree: the leaf is
	 * reachable by lookup at exactly one path, and it is the same one.
	 */
	{
		static const char *const cand[] = { "/d0/n", "/d1/n", "/d2/n" };
		int lookup_hits = 0, k;
		struct dc_path p;

		printf("name-hash: leaf reachable at:");
		for (k = 0; k < 3; k++) {
			uint64_t id = 0;

			if (dc_lookup(g_dc, path_of(&p, cand[k]), &id) == DC_POSITIVE
			    && id == LEAF_ID) {
				lookup_hits++;
				printf(" %s", cand[k]);
			}
		}
		printf("\n");

		anomaly = (c.leaf_count != 1) || (lookup_hits != 1);
		if (anomaly) {
			printf("ANOMALY: child-hlist=%d name-hash=%d reachable "
			       "path(s), expected exactly 1 each and agreeing "
			       "(fold overlap corrupted an index)\n",
			       c.leaf_count, lookup_hits);
			printf("RESULT: FAIL (fold-overlap bug reproduced)\n");
		} else {
			printf("RESULT: PASS (entry conserved, indexes agree)\n");
		}
	}

	sem_destroy(&g_a_stacked);
	sem_destroy(&g_b_done);
	dc_destroy(g_dc);
	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
