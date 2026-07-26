/* SPDX-License-Identifier: GPL-2.0
 *
 * existence_list: McKenney's existence structures (perfbook
 * datastruct/existence, GPL-2.0) driving the SAME doubly-linked churn workload
 * as txn_sw_list / rlu_list / mvrlu_list.
 *
 * HOW EXISTENCE EXPRESSES A COHERENT BIDIRECTIONAL SPLICE.  Existence flips
 * MEMBERSHIP, never pointer fields, so at first sight it cannot make
 * `B->next = N` and `D->prev = N` atomic.  It does not have to.  It decouples
 * physical linkage from logical visibility:
 *
 *   insert  link N into BOTH chains while it is still non-existent -- readers
 *           skip it, so every intermediate state is the correct logical list,
 *           no matter how many pointers have been stored -- then ONE release
 *           store to eg_state makes N visible in both directions at once.
 *   delete  flip C non-existent FIRST, then unlink both edges; readers skip C
 *           either way, so again every intermediate state is correct.  C is
 *           freed after a grace period.
 *
 * perfbook's API does this natively through three callbacks, which is why this
 * engine is short: existence_head_init_incoming() calls eh_add() to link the
 * node while invisible, BEFORE the flip; existence_flip() publishes; and for
 * outgoing elements existence_cleanup_gone() calls eh_remove() to unlink AFTER
 * the flip and then call_rcu()s the node to eh_free().  We supply the three
 * callbacks and the group; existence supplies the ordering.
 *
 * So this is existence used in its native idiom, not contorted onto a
 * structure it was not meant for -- the same "link it, then flip it" pattern
 * it uses to move an element between hash tables.
 *
 * READER COST, which is the point of the comparison: existence_exists() is one
 * smp_load_acquire of the per-element eh_egi plus a predicted branch, and in
 * the steady state eh_egi is 0 (existence_flip resets it on incoming elements)
 * so the fast path stops there.  While a flip is in flight the reader takes a
 * second acquire load, of the group's shared state word -- transient, exactly
 * as the pseudo-transaction's proxy resolution is transient.
 */
#define _GNU_SOURCE
#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* QSBR, matching bench_list_scale.c -- the harness links -lurcu-qsbr, and
 * <urcu.h> would silently pull in the memb flavor instead. */
#include <urcu-qsbr.h>
#include <urcu-call-rcu.h>
#include <urcu/compiler.h>
#include <urcu/list.h>
#include <urcu/rculist.h>

/* ---- api.h-equivalent shim -------------------------------------------------
 * perfbook's existence.h/procon.h open with "The following definitions or
 * equivalent need to be supplied" and expect a CodeSamples api.h.  Rather than
 * drag that in (it carries per-arch inline asm and would fight liburcu's
 * namespace), map the handful of primitives they actually use -- enumerated
 * from the two headers -- onto liburcu and GCC atomics.
 */
#define ACCESS_ONCE(x)		CMM_ACCESS_ONCE(x)
#define BUG_ON(c)		assert(!(c))
#define container_of(p, t, m)	caa_container_of(p, t, m)
#ifndef likely
#define likely(x)		caa_likely(x)
#endif
#ifndef unlikely
#define unlikely(x)		caa_unlikely(x)
#endif
#define smp_load_acquire(p)	__atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v)	__atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define smp_mb()		cmm_smp_mb()

#define CACHE_LINE_SIZE		CAA_CACHE_LINE_SIZE

/*
 * A REAL spin lock, 4 bytes, not perfbook's pthread_mutex_t.
 *
 * perfbook/api.h:188 does `typedef pthread_mutex_t spinlock_t`, which is a
 * portability convenience, not a property of existence structures -- the
 * design says "spin lock", and eh_lock is taken only by WRITERS, only while a
 * flip is being assembled.  At 40 bytes a pthread mutex is 30% of
 * existence_head and pushes the reader's hot fields apart, so charging
 * existence for it would measure api.h, not the concept.  4 bytes here.
 *
 * This changes NO existence semantics: same mutual exclusion, same call sites,
 * existence.h untouched.
 */
#ifdef EXL_PTHREAD_LOCK	/* perfbook's api.h choice, for the A/B */
typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(l)	pthread_mutex_init(l, NULL)
#define spin_lock(l)		pthread_mutex_lock(l)
#define spin_unlock(l)		pthread_mutex_unlock(l)
#else
typedef struct { int lock; } spinlock_t;

static inline void spin_lock_init(spinlock_t *l)
{
	__atomic_store_n(&l->lock, 0, __ATOMIC_RELAXED);
}

static inline void spin_lock(spinlock_t *l)
{
	while (__atomic_exchange_n(&l->lock, 1, __ATOMIC_ACQUIRE))
		while (__atomic_load_n(&l->lock, __ATOMIC_RELAXED))
			caa_cpu_relax();
}

static inline void spin_unlock(spinlock_t *l)
{
	__atomic_store_n(&l->lock, 0, __ATOMIC_RELEASE);
}
#endif /* EXL_PTHREAD_LOCK */

#include "procon.h"		/* perfbook/datastruct/existence */
#include "existence.h"

#include "bench_existence_list.h"

/*
 * FIELD ORDER IS A MEASURED VARIABLE, not a style choice.  The read-side cost
 * of existence is not its SIZE but its effective CACHE DENSITY: how much of
 * each line the reader fetches is data it actually reads.
 *
 * Traversal touches exactly three things per node -- list.next (or .prev),
 * eh.eh_egi, and key.  Everything else in existence_head is cold on this path:
 * eh_lock (40 B, a pthread_mutex_t -- perfbook/api.h:188 typedefs spinlock_t
 * that way, so this is not our inflation), eh_rh (16 B), three callback
 * pointers, and eh_list (16 B).  Cold metadata does not merely cost footprint;
 * it SITS IN THE LINES THAT CARRY THE HOT FIELDS and pushes them apart.
 *
 * default layout   list(0-15) eh(16-127) key(128-131)
 *                  -> eh_egi at 16-23 shares line 0 with list, but key lands
 *                     in line 2.  TWO lines per node, ~20 useful bytes.
 * EXL_HOT_PACK     list(0-15) key(16) anchor(20) eh(24-135)
 *                  -> list, key and eh_egi (at 24-31) all in line 0.  ONE line
 *                     per node on the read path; the cold 100+ bytes of
 *                     existence_head follow and are never touched.
 *
 * Same struct, same size, same existence semantics -- only the order differs.
 * The delta between the two isolates line dilution from every other effect,
 * and it is the honest way to report this: existence's INSTRUCTION cost really
 * is the 1 load + 1 branch of the cost table; what the table does not capture
 * is that the field it names sits inside 112 bytes of cold state.  An embedder
 * that segregates that state recovers most of the difference, which is exactly
 * the claim -- the cost is density, not the load.
 */
struct ex_lnode {
#if defined(EXL_SPLIT)
	/*
	 * EXISTENCE AT ITS BEST -- hot/cold split.  We compare CONCEPTS, so
	 * existence must not be charged for perfbook's struct layout.
	 *
	 * pahole says only eh_egi (8 of existence_head's 112 bytes) is read
	 * during traversal.  The other 104 are writer-only: eh_list is live
	 * only while a flip is assembling, eh_rh only during reclamation, the
	 * three callbacks are compile-time constants for any real embedder,
	 * eh_gone/eh_lock are writer state.  Embedding all of that in the
	 * traversed element is what costs the reader, and nothing in the
	 * existence CONCEPT requires it.
	 *
	 * So keep the tagged existence word in the node and segregate the
	 * rest; writers reach the cold state through a side array indexed by
	 * churn slot, so the node needs no back-pointer.
	 *
	 *   list(0-15) key(16) anchor(20) egi(24-31) = 32 B, ONE line,
	 *   two nodes per 64 B line, every byte of it read.
	 *
	 * Against txn_sw_list's 24 B node that is +8 B: precisely the "one
	 * per-element field" the cost table attributes to existence, and
	 * nothing more.  The algorithm is existence's own -- same
	 * tagged-pointer test, same single-store publish, same acquire/release
	 * pairing -- but this is OUR implementation of it, not perfbook's code.
	 */
	struct cds_list_head list;
	int key;
	int anchor;
	uintptr_t egi;		/* tagged &group->state; 0 == permanent */
#elif defined(EXL_HOT_PACK)
	struct cds_list_head list;
	int key;
	int anchor;		/* stable index this churn node follows */
	struct existence_head eh;
#else
	struct cds_list_head list;
	struct existence_head eh;
	int key;
	int anchor;
#endif
}
#ifdef EXL_CL_ALIGN
/*
 * Cache-line align the node.  Without this the hot prefix lands at a different
 * offset in every node -- with a 96 B node the stride mod 64 alternates 0/32,
 * and with the 136 B (pthread-mutex) node it walks 0,8,16,...,56 so the hot
 * prefix eventually SPLITS across two lines.  Aligning pins the hot fields at
 * offset 0 of a line for every node, making it exactly one line per node
 * deterministically, at the cost of padding the node up to a line multiple.
 *
 * This is the layout an implementation that cared about read throughput would
 * choose, so it belongs in the "existence at its best" arm: we are comparing
 * the existence CONCEPT, not prior field-order and alignment choices.
 */
__attribute__((aligned(EXL_ALIGN_BYTES)))
#endif
;

/* The aligned(64) attribute only pays off if the ALLOCATION is aligned too --
 * malloc/calloc guarantee only max_align_t (16 B here), so under EXL_CL_ALIGN
 * we must ask for it explicitly or the nodes land wherever the allocator likes
 * and the whole point is lost. */
static struct ex_lnode *ex_node_alloc(void)
{
	struct ex_lnode *n;

#ifdef EXL_CL_ALIGN
	if (posix_memalign((void **) &n, EXL_ALIGN_BYTES, sizeof(*n)) != 0)
		return NULL;
	memset(n, 0, sizeof(*n));
#else
	n = calloc(1, sizeof(*n));
#endif
	return n;
}

static struct cds_list_head  g_ex_head;		/* sentinel */
static struct ex_lnode     **g_ex_stable;	/* [list_size] permanent nodes */
static struct ex_lnode     **g_ex_churn;	/* [churn] node per slot, or NULL */
static struct exl_ctx        g_ctx;

/* ---- the three existence callbacks ---------------------------------------
 * eh_add runs BEFORE the flip, with the node still non-existent, so it may
 * link both edges non-atomically: readers skip an invisible node, so each
 * intermediate state is the correct logical list.  eh_remove runs AFTER the
 * flip, with the node already invisible, so it may unlink both edges the same
 * way.  eh_free runs after a grace period.
 */
#ifndef EXL_SPLIT
static int ex_add(struct existence_head *ehp)
{
	struct ex_lnode *n = container_of(ehp, struct ex_lnode, eh);
	struct cds_list_head *pos = &g_ex_stable[n->anchor]->list;
	struct cds_list_head *succ = pos->next;

	n->list.next = succ;
	n->list.prev = pos;
	/* Publish the node's own fields before it becomes reachable. */
	cmm_smp_wmb();
	succ->prev = &n->list;
	CMM_STORE_SHARED(pos->next, &n->list);
	return 0;
}

static void ex_remove(struct existence_head *ehp)
{
	struct ex_lnode *n = container_of(ehp, struct ex_lnode, eh);

	CMM_STORE_SHARED(n->list.prev->next, n->list.next);
	n->list.next->prev = n->list.prev;
}

static void ex_free(struct existence_head *ehp)
{
	free(container_of(ehp, struct ex_lnode, eh));
}

/*
 * Group allocation: plain malloc + a call_rcu-deferred free, NOT perfbook's
 * procon per-thread pool.
 *
 * procon publishes its per-thread mpool pointer ASYNCHRONOUSLY (procon_init
 * issues a call_rcu whose callback sets the __thread pointer), and
 * existence_group_rcu_cb -> existence_group__procon_free() then reads that
 * __thread pointer from whichever call_rcu WORKER thread runs the callback --
 * a thread that never ran procon_init.  Wiring that up correctly would mean
 * per-thread init hooks plus a grace period before the first allocation, for
 * an allocator optimisation that is not what this comparison is about.
 *
 * malloc + call_rcu is also the closer analogue of what it is measured
 * against: the pseudo-transaction defers exactly one group/descriptor object
 * per multi-edge commit the same way.  Stated here because it IS a deviation
 * from how perfbook drives existence, and it moves the WRITE side (the read
 * path, which is what this engine exists to measure, is untouched).
 */
static void ex_group_free(struct rcu_head *rhp)
{
	free(caa_container_of(rhp, struct existence_group, eg_rh));
}
#endif /* !EXL_SPLIT */

void exl_build(const struct exl_ctx *ctx)
{
	struct cds_list_head *prev;
	int i;

	g_ctx = *ctx;
	CDS_INIT_LIST_HEAD(&g_ex_head);
	g_ex_stable = calloc((size_t) g_ctx.list_size, sizeof(*g_ex_stable));
	prev = &g_ex_head;
	for (i = 0; i < g_ctx.list_size; i++) {
		struct ex_lnode *n = ex_node_alloc();

		n->key = 2 * i;
		n->anchor = -1;
		/* Permanent: the tagged word is 0, so the existence test takes
		 * the one-load fast path and stops. */
#ifdef EXL_SPLIT
		n->egi = 0;			/* ex_node_alloc() zeroed it */
#else
		existence_head_init_perm(&n->eh, NULL, NULL, NULL);
#endif
		n->list.prev = prev;
		prev->next = &n->list;
		g_ex_stable[i] = n;
		prev = &n->list;
	}
	prev->next = &g_ex_head;
	g_ex_head.prev = prev;
	g_ex_churn = calloc((size_t) g_ctx.churn, sizeof(*g_ex_churn));
}

#ifdef EXL_SPLIT
/*
 * Our implementation of existence's mechanism over the split layout.  Same
 * algorithm as perfbook's, transcribed against a node that carries only the
 * tagged word:
 *
 *   exists(n)  egi==0 -> permanent.  Otherwise (egi & 1) == *(egi & ~1),
 *              i.e. Vjukov's pointer-tagging trick against the group's state
 *              word: incoming is tagged 1 (exists once state flips to 1),
 *              outgoing is tagged 0 (exists until state flips).
 *   flip(g)    ONE release store to g->state publishes every incoming and
 *              retires every outgoing element simultaneously.
 *
 * Single writer, matching the flip-latch's own contract and P1's scope, so the
 * per-element eh_lock that guards concurrent writers in perfbook has no work
 * to do here and is part of the cold state we segregated.
 */
struct exs_group {
	uintptr_t state;			/* 0 -> 1 on flip */
	struct rcu_head rh;
};

/*
 * SINGLE-WRITER CONTRACT, asserted rather than left implicit.  The split layout
 * segregates perfbook's per-element eh_lock, which exists to serialise
 * CONCURRENT writers assembling overlapping flips.  P1's facility is
 * single-writer by contract and this engine is measured in that regime, so the
 * lock has no work to do -- but a future multi-writer sweep would silently
 * corrupt the list instead of failing, so catch it.  Reset per sweep point via
 * exl_point_reset(), since the harness respawns workers at every point.
 */
static pthread_t g_ex_writer;
static int g_ex_writer_set;

static inline int exs_exists(const struct ex_lnode *n)
{
	uintptr_t egi = __atomic_load_n(&n->egi, __ATOMIC_ACQUIRE);
	const uintptr_t *esp;

	if (caa_likely(!egi))
		return 1;
	esp = (const uintptr_t *) (egi & ~(uintptr_t) 0x1);
	return (egi & 0x1) == __atomic_load_n(esp, __ATOMIC_ACQUIRE);
}

static void exs_group_free(struct rcu_head *rhp)
{
	free(caa_container_of(rhp, struct exs_group, rh));
}

/*
 * Segregated rcu_head for node reclamation: the 32 B node deliberately carries
 * none.  This is symmetric with what it is compared against -- bench_list_scale
 * defers txn_sw_list nodes through seg_call_rcu(), a segregated rcu_head, in
 * the default (non-LIST_RCU_INLINE_RCU_HEAD) build.  Neither engine pays 16
 * bytes per element for reclamation metadata the reader never touches.
 */
struct exs_free {
	struct rcu_head rh;
	struct ex_lnode *node;
};

static void exs_node_free(struct rcu_head *rhp)
{
	struct exs_free *f = caa_container_of(rhp, struct exs_free, rh);

	free(f->node);
	free(f);
}

#define EX_EXISTS(n)	exs_exists(n)
#else
#define EX_EXISTS(n)	existence_exists(&(n)->eh)
#endif /* EXL_SPLIT */

/* One traversal step, resolved: skip nodes that do not exist. */
static inline int ex_visit(struct cds_list_head *p, int *prev, int desc,
			   long *viol, unsigned long *vis)
{
	struct ex_lnode *n = container_of(p, struct ex_lnode, list);
	int k;

	if (!EX_EXISTS(n))
		return 0;			/* logically absent */
	k = n->key;
	if (!g_ctx.random_pos) {
		if (desc ? (k >= *prev) : (k <= *prev)) { (*viol)++; return -1; }
		*prev = k;
	}
	(*vis)++;
	return 0;
}

unsigned long exl_read(long *viol)
{
	struct cds_list_head *p;
	unsigned long vis = 0;
	int prev, steps;

	rcu_read_lock();
	prev = INT_MIN; steps = 0;
	for (p = rcu_dereference(g_ex_head.next); p != &g_ex_head;
			p = rcu_dereference(p->next)) {
		if (++steps > g_ctx.step_limit) { (*viol)++; break; }
		if (ex_visit(p, &prev, 0, viol, &vis) < 0)
			break;
	}
	if (g_ctx.forward_only) {
		prev = INT_MIN; steps = 0;
		for (p = rcu_dereference(g_ex_head.next); p != &g_ex_head;
				p = rcu_dereference(p->next)) {
			if (++steps > g_ctx.step_limit) { (*viol)++; break; }
			if (ex_visit(p, &prev, 0, viol, &vis) < 0)
				break;
		}
	} else {
		prev = INT_MAX; steps = 0;
		for (p = rcu_dereference(g_ex_head.prev); p != &g_ex_head;
				p = rcu_dereference(p->prev)) {
			if (++steps > g_ctx.step_limit) { (*viol)++; break; }
			if (ex_visit(p, &prev, 1, viol, &vis) < 0)
				break;
		}
	}
	rcu_read_unlock();
	return vis;
}

#ifdef EXL_SPLIT
/*
 * Same ordering discipline as the perfbook path, expressed directly:
 *   insert  tag the node incoming (invisible), link BOTH edges, then ONE
 *           release store to group->state publishes it in both directions.
 *   delete  tag outgoing, ONE release store retires it, THEN unlink both
 *           edges and reclaim after a grace period.
 * Every intermediate state is the correct logical list because an invisible
 * node is skipped no matter how many pointers have been stored.
 */
void exl_write(int slot)
{
	struct exs_group *g;

	if (caa_unlikely(!g_ex_writer_set)) {
		g_ex_writer = pthread_self();
		g_ex_writer_set = 1;
	} else {
		assert(pthread_equal(g_ex_writer, pthread_self()));
	}
	g = malloc(sizeof(*g));
	if (!g) {
		fprintf(stderr, "existence_list: group alloc failed\n");
		exit(1);
	}
	g->state = 0;

	if (g_ctx.present[slot]) {
		struct ex_lnode *c = g_ex_churn[slot];
		struct cds_list_head *pv = c->list.prev, *nx = c->list.next;

		/* outgoing: tag 0 -> exists while state == 0 */
		__atomic_store_n(&c->egi, (uintptr_t) &g->state, __ATOMIC_RELEASE);
		__atomic_store_n(&g->state, 1, __ATOMIC_RELEASE);   /* retire */
		/* now invisible: unlink both edges, order irrelevant */
		CMM_STORE_SHARED(pv->next, nx);
		nx->prev = pv;
		{
			struct exs_free *f = malloc(sizeof(*f));

			f->node = c;
			call_rcu(&f->rh, exs_node_free);
		}
		g_ex_churn[slot] = NULL;
		g_ctx.present[slot] = 0;
	} else {
		int a = g_ctx.anchor[slot];
		struct cds_list_head *pos = &g_ex_stable[a]->list;
		struct cds_list_head *succ = pos->next;
		struct ex_lnode *n = ex_node_alloc();

		n->key = 2 * a + 1;
		n->anchor = a;
		/* incoming: tag 1 -> exists once state == 1 */
		n->egi = ((uintptr_t) &g->state) | 0x1;
		n->list.next = succ;
		n->list.prev = pos;
		cmm_smp_wmb();
		succ->prev = &n->list;
		CMM_STORE_SHARED(pos->next, &n->list);
		/* both edges linked while invisible; ONE store publishes */
		__atomic_store_n(&g->state, 1, __ATOMIC_RELEASE);
		/* back to the one-load fast path */
		__atomic_store_n(&n->egi, 0, __ATOMIC_RELEASE);
		g_ex_churn[slot] = n;
		g_ctx.present[slot] = 1;
	}
	call_rcu(&g->rh, exs_group_free);
}
#else
void exl_write(int slot)
{
	struct existence_group *egp = malloc(sizeof(*egp));

	if (!egp) {
		fprintf(stderr, "existence_list: group alloc failed\n");
		exit(1);
	}
	existence_group_init(egp);

	if (g_ctx.present[slot]) {
		/* Flip first: the node ceases to exist, and existence_flip
		 * then calls ex_remove() to unlink it and call_rcu()s it to
		 * ex_free().  Readers skip it from the flip onward, so the
		 * two unlink stores need no atomicity. */
		struct ex_lnode *c = g_ex_churn[slot];

		if (existence_head_set_outgoing(&c->eh, egp) != 0)
			abort();		/* single writer: cannot race */
		existence_flip(egp);
		g_ex_churn[slot] = NULL;
		g_ctx.present[slot] = 0;
	} else {
		/* Link first, while non-existent (ex_add, called from
		 * existence_head_init_incoming), then flip to publish both
		 * edges with one release store to eg_state. */
		struct ex_lnode *n = ex_node_alloc();

		n->anchor = g_ctx.anchor[slot];
		n->key = 2 * n->anchor + 1;
		if (existence_head_init_incoming(&n->eh, egp, ex_add,
						 ex_remove, ex_free) != 0)
			abort();
		existence_flip(egp);
		g_ex_churn[slot] = n;
		g_ctx.present[slot] = 1;
	}
	call_rcu(&egp->eg_rh, ex_group_free);
}
#endif /* EXL_SPLIT */

void exl_point_reset(void)
{
#ifdef EXL_SPLIT
	g_ex_writer_set = 0;
#endif
}
