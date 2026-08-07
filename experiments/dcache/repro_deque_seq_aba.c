// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * Is the deque's `seq` membership guard LOAD-BEARING?  A targeted attempt.
 *
 * check-deque prints "passes without the seq guard -- the guard remains
 * UNPROVEN", and random stress does not settle it: 12 runs at high ABA pressure
 * (down to 8 nodes / 32 writers / 8 deques) with -DURCU_TXN_DEQUE_NO_SEQ_GUARD
 * all pass.  So this AMPLIFIES THE PRECONDITION instead of hoping for it --
 * the technique that cracked the escalation-lane wedges.
 *
 * THE SCENARIO THE GUARD DOCUMENTS.  remove(n) derives &prev->next from
 * &n->prev.  It cannot validate &prev->next, because that is the slot it writes
 * and a validate+store on one slot is the same-slot merge the list documents as
 * corrupting.  So it validates prev->seq, a separate slot bumped by every
 * membership transition -- "unchanged since I read it" made decidable.
 *
 * FORCING IT.  The derivation happens inside remove_prepare() and the commit is
 * a separate call, so the window is the CALLER'S -- no header patch is needed:
 *
 *	T1: begin(); remove_prepare(d, N);	<- derives prev = P
 *	    ... hand off to T2, wait ...
 *	T2: remove(P); push_tail(P); rotate_head(); rotate_head(); ...
 *	        -- P leaves and returns, so its seq moves by 4, and the
 *	           rotations put it back in front of N so that &N->prev
 *	           reads P again, defeating the load_validate on that slot
 *	T1: commit()
 *
 * WHAT EACH OUTCOME MEANS, decided BEFORE the run:
 *   guard ON,  commit ABORTs           -> the guard sees the membership change
 *   guard OFF, commit succeeds + ring intact -> the guard is REDUNDANT HERE:
 *        &n->prev and &n->next are both load_validated, and in a consistent
 *        deque "&n->prev still reads P" already implies P is a member
 *   guard OFF, commit succeeds + ring BROKEN -> the guard is LOAD-BEARING
 *
 * ⚠ The third outcome is the only one that PROVES it.  The second does not
 * prove the guard useless -- it proves this interleaving does not need it.
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
static unsigned long g_seq_before, g_seq_after;

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

static unsigned long seq_of(struct urcu_txn_deque_node *n)
{
	return (unsigned long) uatomic_load(&n->seq, CMM_RELAXED);
}

/* T2: take P out and put it back, then rotate until it precedes N again. */
static void *peer_fn(void *arg)
{
	struct urcu_txn_deque_node *p = arg;
	int i;

	rcu_register_thread();
	rcu_thread_online();
	wait_phase(1);

	g_seq_before = seq_of(p);
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
	g_seq_after = seq_of(p);

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

		printf("guard:      %s\n",
#ifdef URCU_TXN_DEQUE_NO_SEQ_GUARD
			"OFF (-DURCU_TXN_DEQUE_NO_SEQ_GUARD)"
#else
			"ON"
#endif
			);
		printf("P seq:      %lu -> %lu (%s)\n", g_seq_before,
			g_seq_after,
			g_seq_after != g_seq_before ? "membership changed"
						    : "UNCHANGED -- setup failed");
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
			g_commit_status == URCU_TXN_STATUS_ABORT
				? "guard caught the membership change"
			: broken ? "*** GUARD IS LOAD-BEARING: ring corrupted "
				   "without it ***"
				 : "committed and ring intact -- this "
				   "interleaving does not need the guard");
		rcu_thread_offline();
		rcu_unregister_thread();
		return broken ? 2 : 0;
	}
}
