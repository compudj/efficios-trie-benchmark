// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_walk_holdoff.c -- deterministic reproduction of the unbounded lookup,
 * and of the walk hold-off that bounds it (bucket lock engine,
 * -DDC_WALK_HOLDOFF=N; see walk_hold_raise() in dcache_bucketlock.c).
 *
 * dc_lookup re-walks whenever a top on its path is marked.  Two directories
 * that both hold the rest of the path, exchanged over and over, restart it for
 * as long as the exchanges keep coming: /A/B/K and /A/C/K both exist, a walker
 * resolves /A/B/K, and a writer exchanges /A/B with /A/C during every pass.
 *
 * Built WITHOUT the flag (the control) the walker is forced through one pass
 * per exchange, CONTROL_SWAPS + 1 in all: the writer decides how long the
 * lookup runs.
 *
 * Built WITH it, the same writer is stopped after N passes, and one writer
 * caught between its check and its commit shows what the count does not do:
 *
 *   passes 1..N    W1 exchanges during each pass                 -> re-walk
 *     (during pass N, W2 also enters an exchange: it reads the count as zero
 *      and is parked there, past its check, by a test hook)
 *   pass N+1       the walker raises the count on entry; W2, already past its
 *                  check, lands its exchange all the same        -> re-walk
 *   pass N+2       W1 tries again, finds the count raised, sleeps; a shrinker
 *                  sweeps the LRU and evicts nothing             -> stands
 *
 * So the walker takes exactly N + 2 passes: N to reach the threshold, one for
 * the commit that was in flight when the count rose (the mark re-check, not
 * the count, is what catches it), and the one that stands.  W1's held-off
 * exchange completes once the walker has dropped the count, and the same
 * shrinker sweep then evicts: the two K leaves were candidates all along.
 *
 * Every wait is a rendezvous on a hook, none is a sleep.  Built by
 * `make check-walk-holdoff`.  Exit 0 = the pass count and the answer are the
 * expected ones for this build; 1 = they are not.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <urcu-qsbr.h>
#include <urcu/uatomic.h>

#include "dcache.h"

/* Installed into the engine's dc_lookup and writers (-DDC_TEST_HOOKS). */
extern void (*dc_test_walk_hook)(int depth);
#ifdef DC_WALK_HOLDOFF
extern void (*dc_test_held_hook)(void);
extern void (*dc_test_inflight_hook)(void);
#endif

#define ID_UNDER_B	10u	/* /A/B/K as seeded */
#define ID_UNDER_C	20u	/* /A/C/K as seeded */
#define CONTROL_SWAPS	64	/* control build: exchanges the writer offers */
#define SHRINK_NR	64	/* more than the tree holds */
#ifndef WAIT_SEC
#define WAIT_SEC	20	/* a rendezvous that takes this long has failed */
#endif

static struct dcache *g_dc;
static sem_t g_go, g_done;		/* walker <-> W1: one exchange */
static sem_t g_go2, g_inflight, g_raised, g_done2;	/* walker <-> W2 */
static sem_t g_held;			/* W1 -> walker: found the count raised */
static sem_t g_go_shrink, g_shrink_done;	/* walker <-> shrinker */
#ifdef DC_WALK_HOLDOFF
static long g_freed_held;		/* evicted while the count was raised */
#endif
static int g_stop;
static unsigned int g_passes;		/* walker thread only */
static unsigned long g_swaps;		/* exchanges that returned */
static unsigned long g_held_count;

static enum dc_result g_walker_res;
static uint64_t g_walker_id;

#ifdef DC_WALK_HOLDOFF
static __thread int t_is_w2;
#endif

static struct dc_path *path_of(struct dc_path *p, const char *s)
{
	if (dc_path_parse(p, s) != 0) {
		fprintf(stderr, "bad path %s\n", s);
		exit(2);
	}
	return p;
}

/* Park without stalling grace periods; see repro_dcache.c.  Only for a thread
 * that holds no RCU-protected reference across the park. */
static void sem_wait_quiescent(sem_t *s)
{
	unsigned long was_online = rcu_read_ongoing();

	if (was_online)
		rcu_thread_offline();
	while (sem_wait(s) != 0 && errno == EINTR)
		;
	if (was_online)
		rcu_thread_online();
}

/* The WALKER's waits: it is mid-descent, so it stays online, and a peer that
 * never shows up is a failed run rather than a hang. */
static void walker_wait(sem_t *s, const char *what)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += WAIT_SEC;
	while (sem_timedwait(s, &ts) != 0) {
		if (errno == EINTR)
			continue;
		printf("RESULT: FAIL (pass %u: %s never happened)\n",
		       g_passes, what);
		exit(1);
	}
}

/* Runs on the WALKER thread, inside its RCU read-side section. */
static void walk_hook(int depth)
{
	if (depth == 0) {
		g_passes++;
		return;
	}
	if (depth != 1)
		return;
	/* B is latched: an exchange landing from here on fails this pass. */
#ifdef DC_WALK_HOLDOFF
	if (g_passes < DC_WALK_HOLDOFF) {
		sem_post(&g_go);
		walker_wait(&g_done, "W1's exchange");
	} else if (g_passes == DC_WALK_HOLDOFF) {
		sem_post(&g_go2);		/* W2 gets past its check first */
		walker_wait(&g_inflight, "W2 passing its check");
		sem_post(&g_go);
		walker_wait(&g_done, "W1's exchange");
	} else if (g_passes == DC_WALK_HOLDOFF + 1) {
		sem_post(&g_raised);		/* raised on entry to this pass */
		walker_wait(&g_done2, "W2's in-flight exchange");
	} else if (g_passes == DC_WALK_HOLDOFF + 2) {
		sem_post(&g_go);
		walker_wait(&g_held, "W1 being held off");
		sem_post(&g_go_shrink);
		walker_wait(&g_shrink_done, "the shrinker's sweep");
	}
#else
	if (g_passes <= CONTROL_SWAPS) {
		sem_post(&g_go);
		walker_wait(&g_done, "the writer's exchange");
	}
#endif
}

static void exchange_b_c(void)
{
	struct dc_path b, c;
	int ret = dc_rename_exchange(g_dc, path_of(&b, "/A/B"),
				     path_of(&c, "/A/C"));

	if (ret != 0) {
		fprintf(stderr, "exchange /A/B <-> /A/C failed: %d\n", ret);
		exit(2);
	}
	uatomic_inc(&g_swaps);
}

static void *walker_fn(void *arg)
{
	dc_register_thread();
	g_walker_res = dc_lookup(g_dc, (const struct dc_path *) arg, &g_walker_id);
	dc_unregister_thread();
	return NULL;
}

/* W1: one exchange per request, until told to stop. */
static void *w1_fn(void *arg)
{
	(void) arg;
	dc_register_thread();
	for (;;) {
		sem_wait_quiescent(&g_go);
		if (g_stop)
			break;
		exchange_b_c();
		sem_post(&g_done);
	}
	dc_unregister_thread();
	return NULL;
}

#ifdef DC_WALK_HOLDOFF
/* Fired on a writer that found the count raised, just before it sleeps. */
static void held_hook(void)
{
	uatomic_inc(&g_held_count);
	sem_post(&g_held);
}

/* Fired on a writer once it is past its check.  W2 stays here, holding
 * nothing, until the walker has raised the count. */
static void inflight_hook(void)
{
	if (!t_is_w2)
		return;
	sem_post(&g_inflight);
	sem_wait_quiescent(&g_raised);
}

/* Two sweeps: the first may only clear second-chance bits. */
static long shrink_twice(void)
{
	long freed = dc_shrink(g_dc, SHRINK_NR);

	return freed + dc_shrink(g_dc, SHRINK_NR);
}

/* The shrinker, run while the walker holds the count raised. */
static void *shrinker_fn(void *arg)
{
	(void) arg;
	dc_register_thread();
	sem_wait_quiescent(&g_go_shrink);
	g_freed_held = shrink_twice();
	sem_post(&g_shrink_done);
	dc_unregister_thread();
	return NULL;
}

/* W2: the one exchange that is in flight when the count rises. */
static void *w2_fn(void *arg)
{
	(void) arg;
	t_is_w2 = 1;
	dc_register_thread();
	sem_wait_quiescent(&g_go2);
	exchange_b_c();
	sem_post(&g_done2);
	dc_unregister_thread();
	return NULL;
}
#endif

int main(void)
{
	pthread_t walker, w1;
#ifdef DC_WALK_HOLDOFF
	pthread_t w2, shrinker;
	long freed_after;
#endif
	struct dc_path p, abk;
	unsigned int want_passes;
	unsigned long want_swaps, swaps_at_answer;
	uint64_t want_id, final_id = 0;
	enum dc_result final_res;
	int ok;

	rcu_register_thread();
	g_dc = dc_create(1024);
	printf("== repro_walk_holdoff (engine: %s) ==\n", dc_engine_name());

	if (dc_add(g_dc, path_of(&p, "/A"), 1) ||
	    dc_add(g_dc, path_of(&p, "/A/B"), 2) ||
	    dc_add(g_dc, path_of(&p, "/A/C"), 3) ||
	    dc_add(g_dc, path_of(&p, "/A/B/K"), ID_UNDER_B) ||
	    dc_add(g_dc, path_of(&p, "/A/C/K"), ID_UNDER_C)) {
		fprintf(stderr, "setup failed\n");
		return 2;
	}

	sem_init(&g_go, 0, 0);
	sem_init(&g_done, 0, 0);
	sem_init(&g_go2, 0, 0);
	sem_init(&g_inflight, 0, 0);
	sem_init(&g_raised, 0, 0);
	sem_init(&g_done2, 0, 0);
	sem_init(&g_held, 0, 0);
	sem_init(&g_go_shrink, 0, 0);
	sem_init(&g_shrink_done, 0, 0);
	dc_test_walk_hook = walk_hook;
#ifdef DC_WALK_HOLDOFF
	dc_test_held_hook = held_hook;
	dc_test_inflight_hook = inflight_hook;
	/* N by W1, one in flight by W2; W1's last one waits for the walker. */
	swaps_at_answer = DC_WALK_HOLDOFF + 1;
	want_passes = DC_WALK_HOLDOFF + 2;
	want_swaps = DC_WALK_HOLDOFF + 2;
	printf("hold-off after %d passes\n", DC_WALK_HOLDOFF);
#else
	swaps_at_answer = CONTROL_SWAPS;
	want_passes = CONTROL_SWAPS + 1;
	want_swaps = CONTROL_SWAPS;
	printf("control: no hold-off, the writer offers %d exchanges\n",
	       CONTROL_SWAPS);
#endif
	/* Each exchange flips which entry /A/B names. */
	want_id = (swaps_at_answer & 1) ? ID_UNDER_C : ID_UNDER_B;

	pthread_create(&w1, NULL, w1_fn, NULL);
#ifdef DC_WALK_HOLDOFF
	pthread_create(&w2, NULL, w2_fn, NULL);
	pthread_create(&shrinker, NULL, shrinker_fn, NULL);
#endif
	pthread_create(&walker, NULL, walker_fn, path_of(&abk, "/A/B/K"));

	rcu_thread_offline();			/* see repro_dcache.c */
	pthread_join(walker, NULL);
#ifdef DC_WALK_HOLDOFF
	pthread_join(w2, NULL);
	pthread_join(shrinker, NULL);
	/* W1's held-off exchange runs once the walker has dropped the count. */
	while (sem_wait(&g_done) != 0 && errno == EINTR)
		;
#endif
	g_stop = 1;
	sem_post(&g_go);
	pthread_join(w1, NULL);
	rcu_thread_online();

	dc_test_walk_hook = NULL;
#ifdef DC_WALK_HOLDOFF
	dc_test_held_hook = NULL;
	dc_test_inflight_hook = NULL;
#endif
	final_res = dc_lookup(g_dc, &abk, &final_id);

	printf("walker resolved /A/B/K -> %s",
	       g_walker_res == DC_ABSENT ? "ABSENT" :
	       g_walker_res == DC_POSITIVE ? "POSITIVE" : "NEGATIVE");
	if (g_walker_res == DC_POSITIVE)
		printf(" id=%llu", (unsigned long long) g_walker_id);
	printf(" in %u passes (expected id=%llu in %u passes)\n", g_passes,
	       (unsigned long long) want_id, want_passes);
	printf("exchanges: %lu (expected %lu); writers held off: %lu\n",
	       g_swaps, want_swaps, g_held_count);

	ok = g_walker_res == DC_POSITIVE && g_walker_id == want_id &&
	     g_passes == want_passes && g_swaps == want_swaps &&
	     final_res == DC_POSITIVE &&
	     final_id == ((want_swaps & 1) ? ID_UNDER_C : ID_UNDER_B);
#ifdef DC_WALK_HOLDOFF
	ok = ok && g_held_count == 1;
	/* The control for the sweep above: with the count dropped, it evicts. */
	freed_after = shrink_twice();
	printf("shrinker: evicted %ld with the count raised (expected 0), "
	       "%ld after (expected > 0)\n", g_freed_held, freed_after);
	ok = ok && g_freed_held == 0 && freed_after > 0;
#ifdef DC_STRESS_DEBUG
	{
		extern unsigned long dc_dbg_hold_evict_skips;

		printf("eviction attempts skipped by the hold-off: %lu "
		       "(expected > 0)\n", dc_dbg_hold_evict_skips);
		ok = ok && dc_dbg_hold_evict_skips > 0;
	}
#endif
	printf("RESULT: %s\n", ok ? "PASS (bounded: N passes, one for the "
	       "in-flight commit, one that stands)" : "FAIL");
#else
	printf("RESULT: %s\n", ok ? "PASS (control: one pass per exchange, "
	       "the writer sets the count)" : "FAIL");
#endif

	dc_destroy(g_dc);
	rcu_unregister_thread();
	return ok ? 0 : 1;
}
