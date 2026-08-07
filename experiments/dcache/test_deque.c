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
 * biconditional, over the full node array, after hammering the operations
 * concurrently.
 *
 * ---- THE TWO AXES ADDED AFTER THE dcache REWIRE ---------------------------
 *
 * The first version hammered ONE deque over a STATIC node array, and the dcache
 * promptly reached a state it could not have produced.  Two things the dcache
 * does and that version did not:
 *
 *   MANY DEQUES.  dc->lru[] is an array of shards and a dentry MIGRATES between
 *   them -- the producer pushes on its own shard, the sweeper re-adds on the
 *   shard being swept.  So `owner` has to distinguish deques, not merely
 *   "queued or not", and a remove has to survive deriving its deque from a
 *   HINT that a migration can invalidate (which is exactly what lru_del does).
 *
 *   REUSE.  A dentry is freed via call_rcu and its storage handed to a new one,
 *   which memsets the node -- so `seq` GOES BACK TO ZERO.  That matters because
 *   seq is the ABA guard and its whole premise is that it never decreases.
 *   Reuse is the one event that breaks the premise, and nothing exercised it.
 *   Modelled here without an allocator: retire a node, wait a grace period,
 *   re-init it in place.  Same memory, same seq reset, no UB in the checker.
 *
 * ⚠ WHAT THIS TEST CANNOT CATCH, stated because a session already mistook one
 * for the other: a caller that pushes a node it no longer owns.  That is legal
 * at this interface -- push cannot know the caller has already handed the
 * storage to call_rcu -- and it is what the dcache's DC_LRU_READD_LEGACY arm
 * does.  Its witness lives in the caller (-DDC_LRU_FREE_ASSERT), not here.
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
#ifndef NDEQUES
#define NDEQUES		4
#endif
#ifndef DURATION_MS
#define DURATION_MS	800
#endif

/*
 * Per-node lifecycle, and it is the test's own discipline rather than the
 * deque's: a node may only be pushed or removed while LIVE.  RETIRING means one
 * writer has claimed it for reuse and is between the unpublish and the re-init,
 * during which nobody else may touch it.
 *
 * That is the caller-side contract the deque assumes and does not enforce --
 * the same one dc_unlink keeps (unhash, so nothing can find it, then reclaim)
 * and the same one the legacy shrinker breaks.
 */
#define ITEM_LIVE	0UL
#define ITEM_RETIRING	1UL

struct item {
	struct urcu_txn_deque_node dn;
	unsigned long		   id;
	unsigned long		   state;	/* ITEM_LIVE / ITEM_RETIRING */
	unsigned long		   recycles;
	unsigned long		   last_dq;	/* diagnostic: last deque pushed to */
};

static struct urcu_txn_deque	g_dq[NDEQUES];
static struct urcu_txn_domain	g_domain;
static struct item		g_items[NNODES];
/*
 * NOT `volatile int`.  volatile orders nothing and is not atomic, so main's
 * store and the writers' loads are a plain data race -- which TSAN reports, and
 * which the C memory model gives no guarantee about however well it happens to
 * work.  uatomic_* is what the rest of this file already uses.
 */
static int			g_stop;
static unsigned long		g_ops;

static unsigned long		g_pushed, g_removed, g_rotated, g_exists, g_noent;
static unsigned long		g_retired, g_migrated, g_hintmiss;

struct warg {
	unsigned long idx;
	unsigned long seed;
	unsigned long pushed, removed, rotated, exists, noent;
	unsigned long retired, migrated, hintmiss;
};

static unsigned long xrand(unsigned long *s)
{
	*s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
	return *s;
}

static unsigned long dq_index(const struct urcu_txn_deque *d)
{
	return (unsigned long) (d - &g_dq[0]);
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
	static const char *opn[] = { "?", "PUSH", "REMOVE", "RETIRE" };

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
 *
 * ⚠ EVERY out-of-transaction load of a transacted slot below is CMM_ACQUIRE, and
 * that is not decoration.  A slot may hold a parked proxy, and resolving one
 * DEREFERENCES the writer's descriptor -- so a relaxed load carries no
 * happens-before to that descriptor's initialisation, and the reader can see a
 * proxy pointer without seeing the fields it points at.  These are diagnostic
 * reads, which is exactly why the mistake was easy to make and easy to miss:
 * they are not part of the algorithm, so they got written casually.  TSAN
 * reported all of them.
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
				(void **) &d->sentinel.prev, CMM_ACQUIRE));
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
				uatomic_load((void **) &n->prev, CMM_ACQUIRE));
			nx = urcu_txn_deque_resolve(
				uatomic_load((void **) &n->next, CMM_ACQUIRE));
			w = urcu_txn_deque_resolve(uatomic_load(
				(void **) &d->sentinel.next, CMM_ACQUIRE));
			while (w && w != &d->sentinel && hop++ < 4096) {
				if (w == n) { reach = 1; break; }
				w = urcu_txn_deque_resolve(uatomic_load(
					(void **) &w->next, CMM_ACQUIRE));
			}
			fprintf(stderr,
				"LIVELOCK n=%p owner=%p(want %p, deque %lu) "
				"reachable=%d\n"
				"   n->prev=%p  pv->next=%p  names_n=%d\n"
				"   n->next=%p  nx->prev=%p  names_n=%d\n"
				"   => %s\n",
				(void *) n, (void *) urcu_txn_deque_owner(n),
				(void *) d, dq_index(d), reach,
				(void *) pv,
				pv ? (void *) uatomic_load((void **) &pv->next,
							   CMM_ACQUIRE) : NULL,
				pv && urcu_txn_deque_resolve(uatomic_load(
					(void **) &pv->next, CMM_ACQUIRE)) == n,
				(void *) nx,
				nx ? (void *) uatomic_load((void **) &nx->prev,
							   CMM_ACQUIRE) : NULL,
				nx && urcu_txn_deque_resolve(uatomic_load(
					(void **) &nx->prev, CMM_ACQUIRE)) == n,
				(pv && nx &&
				 urcu_txn_deque_resolve(uatomic_load(
					(void **) &pv->next, CMM_ACQUIRE)) == n &&
				 urcu_txn_deque_resolve(uatomic_load(
					(void **) &nx->prev, CMM_ACQUIRE)) == n &&
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
				uatomic_load((void **) &n->prev, CMM_ACQUIRE));
			void *nx = (void *) urcu_txn_deque_resolve(
				uatomic_load((void **) &n->next, CMM_ACQUIRE));

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

/*
 * REMOVE VIA THE OWNER HINT -- deliberately the shape dcache_lru.h's lru_del
 * has, because that shape is what a multi-deque caller is forced into: it does
 * not know which deque holds the node, so it reads `owner`, which is a hint,
 * and lets remove answer authoritatively.  A migration between the two makes
 * remove answer -ENOENT ("queued elsewhere"), which the caller must retry
 * rather than treat as "not queued" -- counted as hintmiss so the run can prove
 * it exercised the path.
 */
static int remove_by_hint(struct warg *w, struct item *it)
{
	/* queued(), not owner(): a sealed node is owned by nothing, and asking
	 * the wrong question here would hand remove_dbg() the POISON value. */
	struct urcu_txn_deque *q = urcu_txn_deque_queued(&it->dn);

	if (!q)
		return -ENOENT;
	{
		int ret = remove_dbg(q, &it->dn);

		if (ret == -ENOENT &&
		    urcu_txn_deque_queued(&it->dn) != NULL)
			w->hintmiss++;	/* it migrated under the hint */
		return ret;
	}
}

/*
 * RETIRE AND REUSE -- the axis that models call_rcu handing a node's storage to
 * a new object.
 *
 * TWO GRACE PERIODS by default, and getting this wrong cost a debugging round,
 * so both are
 * spelled out:
 *
 *   unpublish       state LIVE -> RETIRING, so nobody NEW can select the node;
 *   GP #1           drains peers that already read ITEM_LIVE and could still
 *                   push -- without it a push lands after the removal loop and
 *                   the re-init zeroes a QUEUED node's links;
 *   remove          take it off whatever deque it is on;
 *   GP #2           drains peers HOLDING the node as a derived pointer -- a
 *                   sweeper that read it as the head, a mutator that derived it
 *                   as a neighbour.  RCU keeps their pointer's MEMORY alive; it
 *                   says nothing about its CONTENT, so re-initialising here is
 *                   exactly the "free" they are being protected from;
 *   re-init         seq back to zero, as a memset of reused storage does.
 *
 * GP #2 is the call_rcu analogue and it is the one that was missing first time.
 * Without it, rotate_head_prepare reads `h->next` out of a node this function
 * has already zeroed, gets NULL, and dereferences it -- which looked exactly
 * like a deque defect and was not.  The dcache gets this right by construction:
 * it removes from the deque, THEN call_rcu's, so the storage is reused only
 * after a grace period that starts once the node is off.
 */
static void retire_and_reuse(struct warg *w, struct item *it)
{
	if (uatomic_cmpxchg(&it->state, ITEM_LIVE, ITEM_RETIRING) != ITEM_LIVE)
		return;				/* a peer is retiring it */
#if !defined(DEQUE_SEAL_RETIRE) && !defined(DEQUE_NO_GP1)
	/*
	 * GP #1: drain would-be pushers.  Needed ONLY because a plain remove
	 * leaves owner == NULL, which is exactly what a push wants -- so
	 * without a grace period here a thread that read the node before we
	 * started can still push it after our removal loop finishes.
	 *
	 * -DDEQUE_SEAL_RETIRE drops it and seals instead, which is the whole
	 * point of the seal: it makes "no push may ever queue this again" a
	 * postcondition of a commit rather than of a grace period.  That arm is
	 * the load-bearing test of URCU_TXN_DEQUE_POISON -- if it passes, the
	 * seal really did replace this synchronisation; if it corrupts, it did
	 * not, and no amount of reading the header would have told us.
	 */
	synchronize_rcu();
#endif
	for (;;) {
		struct urcu_txn_deque *q;
		int r;

		rcu_read_lock();
		/*
		 * ⚠ queued(), NOT owner().  A sealed node has a non-NULL owner
		 * and belongs to no deque, so the old `while (owner(n))` shape
		 * spins for ever the moment the seal arm is built.
		 */
		q = urcu_txn_deque_queued(&it->dn);
#ifdef DEQUE_SEAL_RETIRE
		r = q ? urcu_txn_deque_remove_seal(q, &it->dn, &g_domain)
		      : urcu_txn_deque_seal(&it->dn, &g_domain);
		rcu_read_unlock();
		/* 0 or -ESTALE: sealed, and nothing can push it again.
		 * -ENOENT (a peer removed it) / -EEXIST (a peer pushed it):
		 * re-read and take the other branch.  Bounded, because each
		 * outcome that retries requires a peer to have won a CAS. */
		if (r == 0 || r == -ESTALE)
			break;
#else
		if (!q) {
			rcu_read_unlock();
			break;
		}
		r = remove_dbg(q, &it->dn);
		(void) r;
		rcu_read_unlock();
#endif
	}
#ifdef DEQUE_SEAL_RETIRE
	if (!urcu_txn_deque_sealed(&it->dn)) {
		fprintf(stderr, "FAIL: node %lu left the retire loop UNSEALED "
			"(owner=%p)\n", it->id,
			(void *) urcu_txn_deque_owner(&it->dn));
		abort();
	}
#endif
	synchronize_rcu();			/* GP #2: drain pointer holders */
	/*
	 * AUDIT THE HARNESS BEFORE BLAMING THE STRUCTURE.  Re-initialising a
	 * node that is still reachable would zero a live ring's links, and the
	 * resulting corruption would be authored here.  So prove it is off every
	 * ring first -- and say so in the abort message, because "the test did
	 * it" and "the deque did it" are opposite conclusions and this project
	 * has already spent a session confusing them once.
	 *
	 * OPT-IN (-DRETIRE_AUDIT), and off by default ON PURPOSE: it is an
	 * O(NNODES) walk per retire, so it widens every window and shrinks the
	 * interleaving space the test exists to explore.  It masked the missing
	 * second grace period completely -- 8/8 PASS with it, 7/8 SEGV without.
	 * So the default arms run WITHOUT it and one gate arm runs WITH it, as a
	 * check on the harness rather than on the deque.
	 */
#ifdef RETIRE_AUDIT
	{
		unsigned long k;

		for (k = 0; k < NDEQUES; k++) {
			struct urcu_txn_deque_node *w2 = urcu_txn_deque_resolve(
				uatomic_load((void **) &g_dq[k].sentinel.next,
					     CMM_ACQUIRE));
			unsigned long hop = 0;

			while (w2 && w2 != &g_dq[k].sentinel && hop++ <= NNODES) {
				if (w2 == &it->dn) {
					fprintf(stderr,
						"HARNESS BUG: about to re-init "
						"node %lu (%p) while it is still "
						"REACHABLE in deque %lu at hop "
						"%lu -- owner=%p\n",
						it->id, (void *) &it->dn, k, hop,
						(void *) urcu_txn_deque_owner(
								&it->dn));
					abort();
				}
				w2 = urcu_txn_deque_resolve(uatomic_load(
					(void **) &w2->next, CMM_ACQUIRE));
			}
		}
	}
#endif
	hist_log(3, 0, &it->dn, NULL, NULL);
	it->recycles++;
	urcu_txn_deque_node_init(&it->dn);	/* THE REUSE: seq back to 0 */
	uatomic_store(&it->state, ITEM_LIVE, CMM_RELEASE);
	w->retired++;
}

static void *writer_fn(void *arg)
{
	struct warg *w = arg;
	unsigned long n = 0;

	rcu_register_thread();
	rcu_thread_online();
	while (!uatomic_load(&g_stop, CMM_ACQUIRE)) {
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
		unsigned long dqi = (r >> 40) % NDEQUES;
		struct urcu_txn_deque *dq = &g_dq[dqi];
		int op = (int) ((r >> 16) % 64), ret;

		uatomic_inc(&g_ops);		/* liveness witness, sampled by gdb */
#ifdef OPS_NO_ROTATE
		if (op >= 48 && op < 60) op = 0;
#endif
#ifdef OPS_ONLY_ROTATE
		op = 48;
#endif
#ifdef OPS_ONLY_PUSH
		op = 0;
#endif
#ifdef OPS_ONLY_REMOVE
		op = 24;
#endif
#ifdef OPS_NO_REUSE
		if (op >= 60) op = 0;
#endif
		/*
		 * RETIRE runs OUTSIDE the read section: it waits for a grace
		 * period, and a QSBR thread that blocks while inside one (or,
		 * worse, while online and parked) holds off every grace period
		 * in the process.
		 */
		if (op >= 60) {
			retire_and_reuse(w, it);
			if ((++n & 0xff) == 0)
				rcu_quiescent_state();
			continue;
		}

		/*
		 * Mutators run under the read-side lock: remove() reads a
		 * node's neighbours and then CASes into them, so a neighbour
		 * reclaimed in between would be a use-after-free.  Nothing is
		 * unmapped in this test -- reuse is modelled in place -- but
		 * the discipline is the structure's contract and the test
		 * should exercise it as callers must.
		 */
		rcu_read_lock();
		if (uatomic_load(&it->state, CMM_ACQUIRE) != ITEM_LIVE) {
			rcu_read_unlock();	/* claimed for reuse */
			continue;
		}
		if (op < 24) {
			ret = push_dbg(dq, &it->dn);
			if (ret == 0) {
				w->pushed++;
				if (uatomic_load(&it->last_dq, CMM_RELAXED) != dqi) {
					w->migrated++;
					uatomic_store(&it->last_dq, dqi,
						      CMM_RELAXED);
				}
			} else if (ret == -EEXIST) {
				w->exists++;
			}
		} else if (op < 48) {
			ret = remove_by_hint(w, it);
			if (ret == 0)
				w->removed++;
			else if (ret == -ENOENT)
				w->noent++;
		} else {
			if (urcu_txn_deque_rotate_head(dq, &g_domain) == 0)
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
 *
 * With NDEQUES > 1 the biconditional gets sharper, not merely wider: `owner`
 * must name the deque the node is actually reachable in, so a node reachable in
 * deque A while owning B is a distinct failure from one reachable nowhere, and
 * the two are reported apart.
 */
static int verify(void)
{
	unsigned long walked = 0, owned = 0, i;
	int fail = 0;
	long *found;			/* deque index a node was walked in, or -1 */

	found = malloc(NNODES * sizeof(*found));
	if (!found)
		return 2;
	for (i = 0; i < NNODES; i++)
		found[i] = -1;

	/* 1. walk every ring: closure, and both edges at every step */
	for (i = 0; i < NDEQUES; i++) {
		struct urcu_txn_deque *d = &g_dq[i];
		struct urcu_txn_deque_node *w, *prev = &d->sentinel;
		unsigned long here = 0;

		w = d->sentinel.next;
		while (w != &d->sentinel) {
			struct item *it;

			if (++here > NNODES + 1) {
				fprintf(stderr, "FAIL: deque %lu ring does not "
					"close (walked %lu)\n", i, here);
				fail = 1;
				break;
			}
			if (w->prev != prev) {
				fprintf(stderr,
					"FAIL: back edge in deque %lu: node %p "
					"prev=%p, expected %p -- the forward and "
					"backward chains disagree\n",
					i, (void *) w, (void *) w->prev,
					(void *) prev);
				fail = 1;
			}
			if (w->owner != d) {
				fprintf(stderr,
					"FAIL: node %p is LINKED in deque %lu but "
					"owner=%p -- membership disagrees with "
					"the links\n", (void *) w, i,
					(void *) w->owner);
				fail = 1;
			}
			it = caa_container_of(w, struct item, dn);
			if (it->id >= NNODES || found[it->id] >= 0) {
				fprintf(stderr,
					"FAIL: node id %lu bad, or reachable in "
					"two deques (%ld and %lu)\n",
					it->id,
					it->id < NNODES ? found[it->id] : -1, i);
				fail = 1;
				break;
			}
			found[it->id] = (long) i;
			prev = w;
			w = w->next;
		}
		if (!fail && d->sentinel.prev != prev) {
			fprintf(stderr,
				"FAIL: deque %lu sentinel.prev=%p, last walked=%p\n",
				i, (void *) d->sentinel.prev, (void *) prev);
			fail = 1;
		}
		walked += here;
	}

	/* 1b. The seq membership counter is GONE -- structural ABA is handled by
	 * each derived write's expected-old, and identity ABA by RCU.  See the
	 * no-membership-sequence note in <urcu/rcu-txn-deque.h>. */
	for (i = 0; i < NNODES; i++) {
		if (uatomic_load(&g_items[i].state, CMM_RELAXED) != ITEM_LIVE) {
			fprintf(stderr,
				"FAIL: node %lu left RETIRING -- a retire did "
				"not complete\n", i);
			fail = 1;
		}
	}

	/* 2. THE BICONDITIONAL, over every node -- owner names the deque the
	 *    node is reachable in, and nothing else. */
	for (i = 0; i < NNODES; i++) {
		struct urcu_txn_deque *o = g_items[i].dn.owner;

		if (o)
			owned++;
		if (o && found[i] < 0) {
			fprintf(stderr,
				"FAIL: node %lu claims owner=%p (deque %lu) but "
				"is NOT reachable (CLAIMED-BUT-NOT-LINKED)\n",
				i, (void *) o, dq_index(o));
			fail = 1;
		}
		if (!o && found[i] >= 0) {
			fprintf(stderr,
				"FAIL: node %lu is reachable in deque %ld but "
				"owner is NULL (LINKED-BUT-DISOWNED)\n",
				i, found[i]);
			fail = 1;
		}
		if (o && found[i] >= 0 && dq_index(o) != (unsigned long) found[i]) {
			fprintf(stderr,
				"FAIL: node %lu owns deque %lu but is reachable "
				"in deque %ld (WRONG-DEQUE)\n",
				i, dq_index(o), found[i]);
			fail = 1;
		}
	}
	if (owned != walked && !fail) {
		fprintf(stderr, "FAIL: %lu owned vs %lu walked\n", owned, walked);
		fail = 1;
	}
	{
		unsigned long rec = 0;

		for (i = 0; i < NNODES; i++)
			rec += g_items[i].recycles;
		printf("  %d rings: %lu nodes, closed, both edges agree, "
		       "owner<=>reachable and names the right deque;\n"
		       "  across %lu recycles\n",
		       NDEQUES, walked, rec);
	}
	free(found);
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
	for (i = 0; i < NDEQUES; i++)
		urcu_txn_deque_init(&g_dq[i]);
	for (i = 0; i < NNODES; i++) {
		urcu_txn_deque_node_init(&g_items[i].dn);
		g_items[i].id = i;
		g_items[i].state = ITEM_LIVE;
		g_items[i].last_dq = (unsigned long) -1;
	}

	printf("== test_deque: %d writers, %d nodes, %d deques, %d ms ==\n",
	       NWRITERS, NNODES, NDEQUES, DURATION_MS);

	for (i = 0; i < NWRITERS; i++) {
		memset(&wa[i], 0, sizeof(wa[i]));
		wa[i].idx = i;
		wa[i].seed = 0x9e3779b97f4a7c15UL ^ (i + 1) * 0x1234567UL;
		if (pthread_create(&th[i], NULL, writer_fn, &wa[i]))
			return 2;
	}
	nanosleep(&ts, NULL);
	uatomic_store(&g_stop, 1, CMM_RELEASE);
	for (i = 0; i < NWRITERS; i++)
		pthread_join(th[i], NULL);

	for (i = 0; i < NWRITERS; i++) {
		g_pushed += wa[i].pushed;   g_removed += wa[i].removed;
		g_rotated += wa[i].rotated; g_exists  += wa[i].exists;
		g_noent += wa[i].noent;     g_retired += wa[i].retired;
		g_migrated += wa[i].migrated; g_hintmiss += wa[i].hintmiss;
	}
	printf("  push %lu (EEXIST %lu)  remove %lu (ENOENT %lu)  rotate %lu\n"
	       "  migrate %lu  retire/reuse %lu  owner-hint miss %lu\n",
	       g_pushed, g_exists, g_removed, g_noent, g_rotated,
	       g_migrated, g_retired, g_hintmiss);

	/*
	 * A run that never contended proves nothing about the invariant, and a
	 * run that never migrated or never recycled proves nothing about the
	 * two axes this file exists to cover.  Fail loudly rather than print
	 * PASS for a test that did not run.
	 *
	 * hintmiss is NOT gated: it needs a migration to land inside another
	 * writer's remove, which is genuinely rare, and demanding it would make
	 * the gate flaky.  It is reported so a zero is visible.
	 */
#if defined(OPS_ONLY_PUSH) || defined(OPS_ONLY_REMOVE) || \
    defined(OPS_ONLY_ROTATE) || defined(OPS_NO_ROTATE) || defined(OPS_DISJOINT)
	if (0) {
#else
	if (g_exists == 0 || g_noent == 0 || g_rotated == 0 ||
	    g_retired == 0 || (NDEQUES > 1 && g_migrated == 0)) {
#endif
		fprintf(stderr,
			"VACUOUS: the run did not exercise contention "
			"(EEXIST %lu, ENOENT %lu, rotate %lu, retire %lu, "
			"migrate %lu)\n",
			g_exists, g_noent, g_rotated, g_retired, g_migrated);
		return 2;
	}

	fail = verify();
	rcu_unregister_thread();
	printf("RESULT: %s\n", fail ? "FAIL" : "PASS");
	return fail != 0;
}
