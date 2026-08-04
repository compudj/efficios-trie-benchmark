// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * test_deque.c -- concurrent stress + invariant check for urcu-txn-deque.
 *
 * The one invariant the LIST could not hold, and the reason this structure
 * exists, is agreement between membership and the links:
 *
 *   owner == d   <=>   the node is reachable in d's ring
 *   and for every queued node:  prev->next == n  and  next->prev == n
 *
 * Every defect found in the MCAS LRU was a violation of that: a node the
 * forward chain skipped while the backward chain kept it, a shard word saying
 * ON for a node no walk could find, a predecessor naming a departed node.  So
 * the test does not check "it did not crash" -- it checks exactly that
 * biconditional, over the full node array, after hammering the three
 * operations concurrently.
 *
 * Build/run: make check-deque
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <urcu-qsbr.h>
#include <urcu/rcu-txn-deque.h>

#ifndef NNODES
#define NNODES		256
#endif
#ifndef NWRITERS
#define NWRITERS	8
#endif
#ifndef DURATION_MS
#define DURATION_MS	800
#endif

struct item {
	struct urcu_txn_deque_node dn;
	unsigned long		   id;
};

static struct urcu_txn_deque	g_dq;
static struct urcu_txn_domain	g_domain;
static struct item		g_items[NNODES];
static volatile int		g_stop;
static unsigned long		g_ops;

static unsigned long		g_pushed, g_removed, g_rotated, g_exists, g_noent;

struct warg {
	unsigned long idx;
	unsigned long seed;
	unsigned long pushed, removed, rotated, exists, noent;
};

static unsigned long xrand(unsigned long *s)
{
	*s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
	return *s;
}

static void *writer_fn(void *arg)
{
	struct warg *w = arg;
	unsigned long n = 0;

	rcu_register_thread();
	rcu_thread_online();
	while (!g_stop) {
		unsigned long r = xrand(&w->seed);
#ifdef OPS_DISJOINT
		/* Each writer owns a private slice, so the only shared slots
		 * left are the sentinel's.  Distinguishes owner-slot
		 * contention from sentinel contention. */
		struct item *it = &g_items[w->idx * (NNODES / NWRITERS)
					   + (r % (NNODES / NWRITERS))];
#else
		struct item *it = &g_items[r % NNODES];
#endif
		int op = (int) ((r >> 16) % 8), ret;

		/*
		 * Mutators run under the read-side lock: remove() reads a
		 * node's neighbours and then CASes into them, so a neighbour
		 * freed in between would be a use-after-free.  Nothing is freed
		 * in this test, but the discipline is the structure's contract
		 * and the test should exercise it as callers must.
		 */
		uatomic_inc(&g_ops);		/* liveness witness, sampled by gdb */
#ifdef OPS_NO_ROTATE
		if (op >= 6) op = 0;
#endif
#ifdef OPS_ONLY_ROTATE
		op = 6;
#endif
#ifdef OPS_ONLY_PUSH
		op = 0;
#endif
#ifdef OPS_ONLY_REMOVE
		op = 3;
#endif
		rcu_read_lock();
		if (op < 3) {
			ret = urcu_txn_deque_push_tail(&g_dq, &it->dn,
						       &g_domain);
			if (ret == 0)
				w->pushed++;
			else if (ret == -EEXIST)
				w->exists++;
		} else if (op < 6) {
			ret = urcu_txn_deque_remove(&g_dq, &it->dn, &g_domain);
			if (ret == 0)
				w->removed++;
			else if (ret == -ENOENT)
				w->noent++;
		} else {
			if (urcu_txn_deque_rotate_head(&g_dq, &g_domain) == 0)
				w->rotated++;
		}
		rcu_read_unlock();

		if ((++n & 0xff) == 0)
			rcu_quiescent_state();
	}
	rcu_thread_offline();
	rcu_unregister_thread();
	return NULL;
}

/*
 * QUIESCENT verification: every writer has joined, so no commit is in flight
 * and no slot can hold a parked proxy.  Reads are plain.
 */
static int verify(void)
{
	struct urcu_txn_deque_node *w, *prev;
	unsigned long walked = 0, owned = 0, i;
	int fail = 0;
	char *seen;

	seen = calloc(NNODES, 1);
	if (!seen)
		return 2;

	/* 1. walk the ring: closure, and both edges at every step */
	prev = &g_dq.sentinel;
	w = g_dq.sentinel.next;
	while (w != &g_dq.sentinel) {
		struct item *it;

		if (++walked > NNODES + 1) {
			fprintf(stderr, "FAIL: ring does not close (walked %lu)\n",
				walked);
			fail = 1;
			break;
		}
		if (w->prev != prev) {
			fprintf(stderr,
				"FAIL: back edge: node %p prev=%p, expected %p "
				"-- the forward and backward chains disagree\n",
				(void *) w, (void *) w->prev, (void *) prev);
			fail = 1;
		}
		if (w->owner != &g_dq) {
			fprintf(stderr,
				"FAIL: node %p is LINKED but owner=%p -- "
				"membership disagrees with the links\n",
				(void *) w, (void *) w->owner);
			fail = 1;
		}
		it = caa_container_of(w, struct item, dn);
		if (it->id >= NNODES || seen[it->id]) {
			fprintf(stderr, "FAIL: node id %lu bad or duplicated\n",
				it->id);
			fail = 1;
			break;
		}
		seen[it->id] = 1;
		prev = w;
		w = w->next;
	}
	if (!fail && g_dq.sentinel.prev != prev) {
		fprintf(stderr, "FAIL: sentinel.prev=%p, last walked=%p\n",
			(void *) g_dq.sentinel.prev, (void *) prev);
		fail = 1;
	}

	/* 2. THE BICONDITIONAL, over every node -- owner set <=> reachable */
	for (i = 0; i < NNODES; i++) {
		int has_owner = g_items[i].dn.owner != NULL;

		if (has_owner)
			owned++;
		if (has_owner && !seen[i]) {
			fprintf(stderr,
				"FAIL: node %lu claims owner=%p but is NOT "
				"reachable (CLAIMED-BUT-NOT-LINKED)\n",
				i, (void *) g_items[i].dn.owner);
			fail = 1;
		}
		if (!has_owner && seen[i]) {
			fprintf(stderr,
				"FAIL: node %lu is reachable but owner is NULL "
				"(LINKED-BUT-DISOWNED)\n", i);
			fail = 1;
		}
	}
	if (owned != walked && !fail) {
		fprintf(stderr, "FAIL: %lu owned vs %lu walked\n", owned, walked);
		fail = 1;
	}
	printf("  ring: %lu nodes, closed, both edges agree, owner<=>reachable\n",
	       walked);
	free(seen);
	return fail;
}

int main(void)
{
	pthread_t th[NWRITERS];
	struct warg wa[NWRITERS];
	struct timespec ts = { DURATION_MS / 1000,
			       (DURATION_MS % 1000) * 1000000L };
	unsigned long i;
	int fail;

	setvbuf(stdout, NULL, _IONBF, 0);
	rcu_register_thread();
	rcu_thread_offline();		/* main does not participate */

	urcu_txn_domain_init(&g_domain);
	urcu_txn_deque_init(&g_dq);
	for (i = 0; i < NNODES; i++) {
		urcu_txn_deque_node_init(&g_items[i].dn);
		g_items[i].id = i;
	}

	printf("== test_deque: %d writers, %d nodes, %d ms ==\n",
	       NWRITERS, NNODES, DURATION_MS);

	for (i = 0; i < NWRITERS; i++) {
		memset(&wa[i], 0, sizeof(wa[i]));
		wa[i].idx = i;
		wa[i].seed = 0x9e3779b97f4a7c15UL ^ (i + 1) * 0x1234567UL;
		if (pthread_create(&th[i], NULL, writer_fn, &wa[i]))
			return 2;
	}
	nanosleep(&ts, NULL);
	g_stop = 1;
	for (i = 0; i < NWRITERS; i++)
		pthread_join(th[i], NULL);

	for (i = 0; i < NWRITERS; i++) {
		g_pushed += wa[i].pushed;   g_removed += wa[i].removed;
		g_rotated += wa[i].rotated; g_exists  += wa[i].exists;
		g_noent += wa[i].noent;
	}
	printf("  push %lu (EEXIST %lu)  remove %lu (ENOENT %lu)  rotate %lu\n",
	       g_pushed, g_exists, g_removed, g_noent, g_rotated);

	/* A run that never contended proves nothing about the invariant. */
#if defined(OPS_ONLY_PUSH) || defined(OPS_ONLY_REMOVE) || \
    defined(OPS_ONLY_ROTATE) || defined(OPS_NO_ROTATE)
	if (0) {
#else
	if (g_exists == 0 || g_noent == 0 || g_rotated == 0) {
#endif
		fprintf(stderr,
			"VACUOUS: the run did not exercise contention "
			"(EEXIST %lu, ENOENT %lu, rotate %lu)\n",
			g_exists, g_noent, g_rotated);
		return 2;
	}

	fail = verify();
	rcu_unregister_thread();
	printf("RESULT: %s\n", fail ? "FAIL" : "PASS");
	return fail != 0;
}
