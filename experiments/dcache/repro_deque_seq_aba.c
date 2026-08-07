// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * FORCED STRUCTURAL ABA -- the regression that keeps the deque's missing
 * membership sequence honest.
 *
 * The deque used to carry a per-node `seq`, bumped by every membership
 * transition and validated by anyone holding a derivation, as an ABA guard.  It
 * was removed, and this is the test that says the removal is still sound.
 *
 * THE ARGUMENT IT GUARDS.  ABA here is two different things:
 *   identity ABA   -- node freed, address reused as something else.  RCU
 *                     excludes it: mutators run inside a read-side section.
 *   structural ABA -- the SAME node leaves the deque and comes back.
 * Only the second can happen, and it is already decided by the expected-old on
 * every derived write: remove() stores &prev->next : n -> next, so "prev still
 * points at n" is asserted against the CURRENT structure.  If that holds at
 * commit, splicing n out is correct however many times prev left and returned.
 *
 * WHAT THIS FORCES.  remove_prepare() does the derivation and commit() is a
 * separate call, so the window belongs to the caller -- no header hook needed:
 *
 *	T1: begin(); remove_prepare(d, N)	<- derives prev = P
 *	T2: remove(P); push_tail(P); rotate...	<- P leaves and returns, and
 *	                                           the neighbourhood is rotated
 *	                                           back so BOTH of N's edges
 *	                                           read their original values
 *	T1: commit()
 *
 * ⚠ BOTH EDGES, and that is the whole trick.  remove_prepare load_validates
 * &N->prev AND &N->next.  Restoring only the back edge leaves the forward one
 * changed, and THAT validate aborts the commit -- for a reason with nothing to
 * do with ABA.  An earlier version of this repro did exactly that and measured
 * nothing.
 *
 * PASS = the commit SUCCEEDS and the ring is intact: the structural ABA was
 * genuine (P's identity left and returned) and the expected-olds handled it.
 * FAIL = the ring is broken, which would mean the expected-old argument is
 * wrong and the membership sequence has to come back.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <urcu/compiler.h>
#include <urcu-qsbr.h>
#include <urcu-call-rcu.h>
#include <urcu/rcu-txn.h>
#include <urcu/rcu-txn-deque.h>

#define NR_NODES	4

static struct urcu_txn_domain g_dom;
static struct urcu_txn_deque g_d;
static struct urcu_txn_deque_node g_n[NR_NODES];

/* Handshake: T1 parks between prepare and commit; T2 does its work. */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_phase;		/* 0: T1 preparing  1: T2's turn  2: T2 done */

static int g_commit_status;
static int g_peer_ok;

static void wait_phase(int want)
{
	pthread_mutex_lock(&g_mx);
	while (g_phase != want)
		pthread_cond_wait(&g_cv, &g_mx);
	pthread_mutex_unlock(&g_mx);
}

static void set_phase(int p)
{
	pthread_mutex_lock(&g_mx);
	g_phase = p;
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mx);
}

/* T2: take P out and put it back, then rotate until it precedes N again. */
static void *peer_fn(void *arg)
{
	struct urcu_txn_deque_node *p = arg;
	int i;

	rcu_register_thread();
	rcu_thread_online();
	wait_phase(1);

	if (urcu_txn_deque_remove(&g_d, p, &g_dom))
		fprintf(stderr, "peer: remove(P) failed\n");
	if (urcu_txn_deque_push_tail(&g_d, p, &g_dom))
		fprintf(stderr, "peer: push_tail(P) failed\n");
	/*
	 * Rotate until &N->prev reads P again.  Rotation changes no membership
	 * and bumps no seq, so it restores the LINK without restoring the
	 * identity the derivation was taken from -- which is exactly the state
	 * the guard claims to catch and the load_validate on &N->prev cannot.
	 */
	for (i = 0; i < NR_NODES * 4; i++) {
		/*
		 * ⚠ BOTH edges, not just &N->prev.  remove_prepare
		 * load_validates &N->prev AND &N->next, so restoring only the
		 * back edge leaves the forward one changed and THAT validate
		 * aborts the commit -- which is what the first version of this
		 * repro measured: the guard-OFF arm aborted too, for a reason
		 * that had nothing to do with the guard.  Isolating the seq
		 * guard means putting the whole neighbourhood back.
		 */
		if (urcu_txn_deque_resolve(uatomic_load(&g_n[1].prev,
						CMM_RELAXED)) == p &&
		    urcu_txn_deque_resolve(uatomic_load(&g_n[1].next,
						CMM_RELAXED)) == &g_n[2])
			break;
		(void) urcu_txn_deque_rotate_head(&g_d, &g_dom);
	}
	g_peer_ok = 1;

	rcu_thread_offline();
	rcu_unregister_thread();
	set_phase(2);
	return NULL;
}

int main(void)
{
	pthread_t th;
	struct urcu_txn txn;
	int i, ret;

	rcu_register_thread();
	rcu_thread_online();
	urcu_txn_domain_init(&g_dom);
	urcu_txn_deque_init(&g_d);
	for (i = 0; i < NR_NODES; i++) {
		urcu_txn_deque_node_init(&g_n[i]);
		if (urcu_txn_deque_push_tail(&g_d, &g_n[i], &g_dom)) {
			fprintf(stderr, "setup: push %d failed\n", i);
			return 1;
		}
	}
	/* Layout: sentinel, n[0]=P, n[1]=N, n[2], n[3] */
	pthread_create(&th, NULL, peer_fn, &g_n[0]);

	/* T1: derive, hand over, then commit into the changed structure. */
	urcu_txn_init(&txn, &g_dom);
	urcu_txn_begin(&txn);
	ret = urcu_txn_deque_remove_prepare(&txn, &g_d, &g_n[1]);
	if (ret) {
		fprintf(stderr, "remove_prepare(N) = %d\n", ret);
		return 1;
	}
	set_phase(1);
	wait_phase(2);
	g_commit_status = (int) urcu_txn_commit(&txn);
	urcu_txn_end(&txn);
	pthread_join(th, NULL);

	/* Ring integrity: walk forward and check every back edge agrees. */
	{
		struct urcu_txn_deque_node *sent = &g_d.sentinel, *c, *pv;
		int broken = 0, steps = 0;

		pv = sent;
		c = urcu_txn_deque_resolve(uatomic_load(&sent->next,
					CMM_RELAXED));
		while (c != sent && steps++ <= NR_NODES + 2) {
			if (urcu_txn_deque_resolve(uatomic_load(&c->prev,
						CMM_RELAXED)) != pv)
				broken = 1;
			pv = c;
			c = urcu_txn_deque_resolve(uatomic_load(&c->next,
						CMM_RELAXED));
		}
		if (c != sent || steps > NR_NODES + 2)
			broken = 1;

		printf("peer:       %s\n", g_peer_ok
			? "P removed and re-pushed (membership changed)"
			: "PEER FAILED -- setup did not force the ABA");
		printf("N->prev==P: %s   N->next==orig: %s\n",
			urcu_txn_deque_resolve(uatomic_load(&g_n[1].prev,
					CMM_RELAXED)) == &g_n[0] ? "yes" : "NO",
			urcu_txn_deque_resolve(uatomic_load(&g_n[1].next,
					CMM_RELAXED)) == &g_n[2] ? "yes" : "NO");
		printf("commit:     %s\n",
			g_commit_status == URCU_TXN_STATUS_ABORT ? "ABORT"
			: g_commit_status < 0 ? "error" : "OK");
		printf("ring:       %s\n", broken ? "BROKEN" : "intact");
		printf("VERDICT:    %s\n",
			!g_peer_ok ? "*** FAIL: the ABA was not forced ***"
			: broken ? "*** FAIL: ring corrupted -- the expected-old "
				   "argument is wrong, seq must come back ***"
			: g_commit_status == URCU_TXN_STATUS_ABORT
				? "*** FAIL: aborted -- something still versions "
				  "membership ***"
			: "PASS: structural ABA committed, ring intact");
		rcu_thread_offline();
		rcu_unregister_thread();
		return (broken || !g_peer_ok ||
			g_commit_status == URCU_TXN_STATUS_ABORT) ? 2 : 0;
	}
}
