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

/*
 * COMMIT LOG.  Record what each SUCCESSFUL commit derived, so the corruption
 * can be replayed against the operations that built it rather than guessed at.
 * Ring buffer, one atomic index; dumped filtered to the three addresses the
 * discriminator names.
 */
#define HIST_N	(1u << 21)
struct hev { unsigned long seq; int op; int st; void *n, *a, *b; };
static struct hev g_hist[HIST_N];
static unsigned long g_hseq;

/*
 * Log EVERY outcome, not just OK.  The previous version recorded successes
 * only, which left two explanations standing for a node that is queued with no
 * push behind it: a commit that reported OK without applying all its slots, or
 * simply a missing log entry.  Recording the bails and the failed commits
 * separates them -- in particular a push answering -EEXIST after the node's
 * last remove proves owner was still set, i.e. that remove's commit reported OK
 * without applying &n->owner.
 */
static void hist_log(int op, int st, void *n, void *a, void *b)
{
	unsigned long i = uatomic_add_return(&g_hseq, 1) - 1;
	struct hev *e = &g_hist[i & (HIST_N - 1)];

	e->op = op; e->st = st; e->n = n; e->a = a; e->b = b;
	cmm_smp_wmb();
	e->seq = i;
}

static const char *stn(int v)
{
	switch (v) {
	case 0:		return "OK";
	case -EEXIST:	return "EEXIST";
	case -ENOENT:	return "ENOENT";
	case -EAGAIN:	return "EAGAIN";
	case 1:		return "ABORT";
	default:	return "ERR";
	}
}

static void hist_dump(void *x, void *y, void *z)
{
	unsigned long now = uatomic_load(&g_hseq, CMM_RELAXED), i;
	unsigned long lo = now > HIST_N ? now - HIST_N : 0;
	static const char *opn[] = { "?", "PUSH", "REMOVE" };

	fprintf(stderr, "-- commit log touching %p / %p / %p --\n", x, y, z);
	for (i = lo; i < now; i++) {
		struct hev *e = &g_hist[i & (HIST_N - 1)];

		if (e->seq != i)
			continue;
		if (e->n != x && e->n != y && e->n != z &&
		    e->a != x && e->a != y && e->a != z &&
		    e->b != x && e->b != y && e->b != z)
			continue;
		fprintf(stderr, "  #%lu %-6s %-8s n=%p prev=%p next=%p\n",
			e->seq, opn[e->op], stn(e->st), e->n, e->a, e->b);
	}
}

/*
 * PUSH with the same hand-opened bracket, so its derived edges are logged too.
 */
static int push_dbg(struct urcu_txn_deque *d, struct urcu_txn_deque_node *n)
{
	struct urcu_txn txn;

	urcu_txn_init(&txn, &g_domain);
	for (;;) {
		enum urcu_txn_status st;
		int prep;
		void *ot;

		urcu_txn_begin(&txn);
		prep = urcu_txn_deque_push_tail_prepare(&txn, d, n);
		if (prep) {
			hist_log(1, prep, n, NULL, NULL);
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			return prep;
		}
		ot = (void *) urcu_txn_deque_resolve(uatomic_load(
				(void **) &d->sentinel.prev, CMM_RELAXED));
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		hist_log(1, st == URCU_TXN_STATUS_OK ? 0 :
			 (st == URCU_TXN_STATUS_ABORT ? 1 : -5), n, ot,
			 &d->sentinel);
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		return st == URCU_TXN_STATUS_OK ? 0 : -ENOMEM;
	}
}

/*
 * THE DISCRIMINATOR.  At the livelock one thread retries forever inside the
 * escalation lane while its only competitor is parked -- so nothing can be
 * racing it, and a CAS that still will not land has an expected-old that
 * memory does not hold.  Two possibilities, and they point at opposite code:
 *
 *   the structure is CORRECT here (owner == d, prev->next == n,
 *   next->prev == n)  =>  every expected-old the prepare records IS what
 *   memory holds, and the commit is failing anyway   =>  ENGINE
 *
 *   the structure is CORRUPT  =>  the deque's own prepares built it wrong
 *   =>  DEQUE
 *
 * So open the bracket by hand, count the retries, and dump the three edges the
 * prepare derives against what memory actually holds.
 */
static int remove_dbg(struct urcu_txn_deque *d, struct urcu_txn_deque_node *n)
{
	struct urcu_txn txn;
	unsigned long tries = 0;

	urcu_txn_init(&txn, &g_domain);
	for (;;) {
		enum urcu_txn_status st;
		int prep;

		if (++tries == 100000) {
			struct urcu_txn_deque_node *pv, *nx, *w;
			unsigned long hop = 0;
			int reach = 0;

			pv = urcu_txn_deque_resolve(
				uatomic_load((void **) &n->prev, CMM_RELAXED));
			nx = urcu_txn_deque_resolve(
				uatomic_load((void **) &n->next, CMM_RELAXED));
			w = urcu_txn_deque_resolve(uatomic_load(
				(void **) &d->sentinel.next, CMM_RELAXED));
			while (w && w != &d->sentinel && hop++ < 4096) {
				if (w == n) { reach = 1; break; }
				w = urcu_txn_deque_resolve(uatomic_load(
					(void **) &w->next, CMM_RELAXED));
			}
			fprintf(stderr,
				"LIVELOCK n=%p owner=%p(want %p) reachable=%d\n"
				"   n->prev=%p  pv->next=%p  names_n=%d\n"
				"   n->next=%p  nx->prev=%p  names_n=%d\n"
				"   => %s\n",
				(void *) n, (void *) urcu_txn_deque_owner(n),
				(void *) d, reach,
				(void *) pv,
				pv ? (void *) uatomic_load((void **) &pv->next,
							   CMM_RELAXED) : NULL,
				pv && urcu_txn_deque_resolve(uatomic_load(
					(void **) &pv->next, CMM_RELAXED)) == n,
				(void *) nx,
				nx ? (void *) uatomic_load((void **) &nx->prev,
							   CMM_RELAXED) : NULL,
				nx && urcu_txn_deque_resolve(uatomic_load(
					(void **) &nx->prev, CMM_RELAXED)) == n,
				(pv && nx &&
				 urcu_txn_deque_resolve(uatomic_load(
					(void **) &pv->next, CMM_RELAXED)) == n &&
				 urcu_txn_deque_resolve(uatomic_load(
					(void **) &nx->prev, CMM_RELAXED)) == n &&
				 urcu_txn_deque_owner(n) == d)
				? "STRUCTURE IS CORRECT -> the ENGINE is failing"
				: "STRUCTURE IS CORRUPT -> the DEQUE is failing");
			hist_dump(n, pv, nx);
			abort();
		}

		urcu_txn_begin(&txn);
		prep = urcu_txn_deque_remove_prepare(&txn, d, n);
		if (prep) {
			hist_log(2, prep, n, NULL, NULL);
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			return prep;
		}
		{
			void *pv = (void *) urcu_txn_deque_resolve(
				uatomic_load((void **) &n->prev, CMM_RELAXED));
			void *nx = (void *) urcu_txn_deque_resolve(
				uatomic_load((void **) &n->next, CMM_RELAXED));

			st = urcu_txn_commit(&txn);
			urcu_txn_end(&txn);
			hist_log(2, st == URCU_TXN_STATUS_OK ? 0 :
				 (st == URCU_TXN_STATUS_ABORT ? 1 : -5),
				 n, pv, nx);
		}
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		return st == URCU_TXN_STATUS_OK ? 0 : -ENOMEM;
	}
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
			ret = push_dbg(&g_dq, &it->dn);
			if (ret == 0)
				w->pushed++;
			else if (ret == -EEXIST)
				w->exists++;
		} else if (op < 6) {
			ret = remove_dbg(&g_dq, &it->dn);
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

	/* 1b. seq must stay EVEN: bit 0 is the engine's descriptor proxy tag on
	 * every transacted slot, so an odd value means either a leaked proxy or
	 * a bump by 1 somewhere. */
	for (i = 0; i < NNODES; i++) {
		if (g_items[i].dn.seq & 1UL) {
			fprintf(stderr,
				"FAIL: node %lu seq=%lu is ODD -- bit 0 is the "
				"proxy tag and must stay clear\n",
				i, g_items[i].dn.seq);
			fail = 1;
		}
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
	{
		unsigned long tot = 0;

		for (i = 0; i < NNODES; i++)
			tot += g_items[i].dn.seq;
		printf("  ring: %lu nodes, closed, both edges agree, "
		       "owner<=>reachable; seq total %lu (all even)\n",
		       walked, tot);
	}
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
