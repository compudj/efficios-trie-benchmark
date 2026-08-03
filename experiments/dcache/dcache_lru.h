/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * dcache_lru.h -- PHASE 3: the dentry LRU, shared by the txn engines.
 *
 * Included by dcache_bucketlock.c and dcache_txn.c AFTER each has defined its
 * own struct dentry, struct dcache, children_empty() and lru_evict_settled().
 * One copy rather than one per engine, because the policy here is not
 * engine-specific and a divergence would be silent -- the retain_dentry bug
 * this file's history records had to be fixed in two places at once.
 *
 * dcache_seqlock.c deliberately does NOT include it: the baseline carries its
 * own kernel-shaped `struct list_lru` (per-node shard, batch-isolate shrinker),
 * and sharing an implementation with the engines it is meant to be compared
 * against would let it borrow their refinements.
 *
 * WHAT THE INCLUDING ENGINE MUST PROVIDE:
 *   struct dentry { ... struct { <links>; unsigned int shard;
 *                               unsigned char referenced; } d_lru; }
 *   struct dcache { ... struct dc_lru_shard *lru; unsigned int nlru;
 *                       dc_domain_t lru_domain; }   (domain: MCAS arm only)
 *   int children_empty(struct dentry *);
 *   int lru_evict_settled(struct dcache *, struct dentry *);  -- 0 on success
 *
 * The design (design/dcache-lru-txn.md), in one paragraph: the order is FUZZY on
 * purpose (CLOCK / second chance), a lookup touches the list ZERO times because
 * this port takes no reference on the read side, and the LRU wants no SW
 * transaction because it has no lockless reader to consume a reader-atomic flip.
 * That last point argues against SW, not against MW -- an arbitrary mid-list
 * splice is a genuine multi-writer problem, which is what -DDC_LRU_MCAS is for.
 */


/*
 * Two sections, selected by DCACHE_LRU_TYPES, because the shard type must be
 * declared BEFORE struct dcache (which holds a pointer to an array of them)
 * while the implementation needs struct dcache, children_empty() and
 * lru_evict_settled() -- so the including engine takes it in two bites:
 *
 *	#define DCACHE_LRU_TYPES
 *	#include "dcache_lru.h"
 *	#undef  DCACHE_LRU_TYPES
 *	... struct dcache { ... struct dc_lru_shard *lru; ... } ...
 *	... children_empty(), lru_evict_settled() ...
 *	#include "dcache_lru.h"
 */
#ifdef DCACHE_LRU_TYPES
/*
 * WHICH AXIS TO SHARD ON is a real question with a measurable answer, so it is a
 * build arm rather than a decision baked in:
 *
 *   default          per NUMA NODE   -- what the kernel does (lru->node[nid]),
 *                                       and so the faithful arm.  Coarse: every
 *                                       CPU on a node shares one lock and one
 *                                       pair of head/tail cachelines.
 *   -DDC_LRU_PERCPU  per CPU         -- the obvious userspace refinement.  Kills
 *                                       producer-vs-producer outright, at the
 *                                       price of a shard array sized by the
 *                                       MACHINE and an LRU order that is now
 *                                       fuzzy across CPUs as well as in time.
 *   -DDC_LRU_MM_CID  per mm_cid      -- rseq's dense per-process concurrency id.
 *                                       Same isolation as per-CPU while the
 *                                       shard array is sized by how many threads
 *                                       are ACTUALLY running, not by how many
 *                                       CPUs exist, so the shard heads stay in
 *                                       cache instead of scattering.
 *
 * The comparison is the point.  Finer sharding trivially wins the enqueue
 * microbenchmark; what it costs is eviction QUALITY -- N independent clocks mean
 * the global order is only as good as the shard balance -- and shard-array
 * footprint.  Per-node keeps one clock per node, which is why the kernel can
 * still call the result an LRU.
 */
#if defined(DC_LRU_PERCPU) && defined(DC_LRU_MM_CID)
# error "pick one LRU shard axis"
#endif

/*
 * State word encoding, shared by both mechanisms.  Three states rather than a
 * bare "shard+1" because with no lock the add and del paths must CLAIM the node
 * before touching the list, or two concurrent retains would both enqueue the
 * same dentry and corrupt the edges.
 */
#define DC_LRU_OFF	0u		/* not on any shard */
#define DC_LRU_BUSY	1u		/* a transition is in flight */
#define DC_LRU_ON(i)	((i) + 2u)	/* on shard i */

struct dc_lru_shard {
#ifdef DC_LRU_MCAS
	/*
	 * THE MCAS ARM.  No lock: the list is <urcu/rcu-txn-list.h>, a
	 * bidirectional list whose every structural change flips BOTH edges in
	 * one MCAS commit, so an arbitrary mid-list splice is atomic and
	 * lock-free.  That is the property the whole arm exists for -- it is the
	 * one thing neither the two-lock FIFO nor a lazy/tombstone scheme can do
	 * (design/dcache-lru-txn.md sections 6-7), and unlink NEEDS it.
	 *
	 * What it buys: the shrinker never shares a lock with dentry ops, so
	 * producer-vs-consumer contention disappears rather than being bounded.
	 * What it costs: a descriptor and a multi-CAS on EVERY enqueue and EVERY
	 * unlink -- a constant tax on the hot churn path to decouple a consumer
	 * that may be idle.  Section 7 says which wins is decided by RECLAIM
	 * CADENCE, not by readers: bursty reclaim favours the lock, continuous
	 * eviction favours this.  That is a measurement, hence both arms.
	 */
	struct urcu_txn_list_head list;
	unsigned long count;			/* atomic; no lock to protect it */
	char pad[64 - (sizeof(unsigned long) +
		       sizeof(struct urcu_txn_list_head)) % 64];
#else
	unsigned long lock;		/* test-and-set; see fold_lock */
	struct dentry *head;		/* oldest -- the shrinker's end */
	struct dentry *tail;		/* newest -- the enqueue end */
	unsigned long count;
	/* keep shards off each other's cachelines */
	char pad[64 - (2 * sizeof(unsigned long) + 2 * sizeof(void *)) % 64];
#endif
};


/*
 * Forward declarations, so an engine can call these from code that sits ABOVE
 * the implementation section -- resolve() in particular, which is defined long
 * before it.
 */
struct dcache;
struct dentry;
#ifndef DC_NO_LRU
static int lru_shards_init(struct dcache *dc);
static unsigned int lru_nshards(void);
#endif
static void lru_add(struct dcache *dc, struct dentry *d);
static void lru_add_at(struct dcache *dc, struct dentry *d, unsigned int idx);
static void lru_del(struct dcache *dc, struct dentry *d);
static inline void lru_retain(struct dcache *dc, struct dentry *d);

#else	/* the implementation */

#ifndef DC_NO_LRU
/* ---- PHASE 3: the sharded LRU ------------------------------------------ */
/* ---- shard AXIS: chosen independently of the mechanism below ----------- */
/*
 * Pick a shard: the NUMA NODE, exactly the axis the kernel shards on
 * (`lru->node[nid]`, one list + one lock per node).
 *
 * Read from the RSEQ ABI page -- rseq_current_node_id() is a plain load of a
 * field the kernel maintains in thread-local memory, so it costs a load and no
 * syscall, no vDSO call, and no libnuma.
 *
 * WHICH node, though, is worth stating, because the kernel and this differ in
 * derivation and agree in effect.  The kernel takes the node of the OBJECT'S
 * MEMORY (`list_lru_add_obj` -> `page_to_nid(virt_to_page(item))`) -- the list
 * links live inside the dentry, so it wants the shard whose lock and head are
 * near that memory.  This takes the node of the ENQUEUEING THREAD, which under
 * a first-touch allocator is the node that dentry's memory is on, since the same
 * thread allocated it moments earlier.  Same shard, one load instead of a page
 * lookup.
 *
 * The node is captured ONCE, at enqueue, and stored in d_lru.shard -- a dentry
 * unlinked from a different node must splice out of the list it is actually on,
 * not the caller's.  The kernel gets that property for free by recomputing from
 * the object; we get it by remembering.
 *
 * Without rseq node ids (old kernel, or rseq unavailable) everything lands on
 * shard 0.  That is honest rather than degraded-but-plausible: we genuinely do
 * not know the node, and pretending otherwise -- sharding by CPU, say -- would
 * silently make this arm FINER-grained than the kernel's and flatter it in
 * exactly the comparison it exists to inform.
 */
static inline unsigned int lru_shard_index(const struct dcache *dc)
{
	unsigned int id;

#if defined(DC_LRU_PERCPU)
	int raw = rseq_current_cpu_raw();

	id = raw < 0 ? 0u : (unsigned int) raw;
#elif defined(DC_LRU_MM_CID)
	id = rseq_mm_cid_available() ? rseq_current_mm_cid() : 0u;
#else
	id = rseq_node_id_available() ? rseq_current_node_id() : 0u;
#endif
	return id < dc->nlru ? id : id % dc->nlru;
}

const char *dc_lru_arm(void)
{
#if defined(DC_LRU_PERCPU)
	return "percpu";
#elif defined(DC_LRU_MM_CID)
	return "mm_cid";
#else
	return "pernode";
#endif
}

/* How many shards this arm wants. */
static unsigned int lru_nshards(void)
{
#if defined(DC_LRU_PERCPU)
	int n = rseq_get_max_nr_cpus();

	return n > 0 ? (unsigned int) n : 1u;
#elif defined(DC_LRU_MM_CID)
	int n = rseq_get_max_nr_cpus();		/* mm_cid <= nr_cpus, and dense */

	return n > 0 ? (unsigned int) n : 1u;
#else
	return 64u;				/* >= nr_node_ids anywhere we run */
#endif
}

/*
 * retain_dentry (fs/dcache.c), the kernel's last-dput action:
 *
 *	not on the LRU -> d_lru_add() at the tail
 *	already on it  -> d_flags |= DCACHE_REFERENCED, and do NOT move it
 *
 * Not moving an already-listed dentry is the load-bearing half: recency becomes
 * a per-object bit rather than a shared list-head write, which is the only
 * reason one list per shard survives a busy cache.
 *
 * The re-add half matters just as much, and an earlier cut of this missed it.
 * It is what lets the shrinker answer LRU_REMOVED for an in-use entry the way
 * the kernel does -- an entry taken off the list is not lost, it is waiting for
 * its next touch.  Without a re-arm the only safe answer is to rotate, which
 * keeps un-evictable entries circulating and burning scan budget forever.
 *
 * Called from the WRITER-side resolve only; dc_lookup does not come through
 * there, so the reader pays nothing.  Test-then-set on the bit so a hot
 * directory costs a shared-state load and no invalidation.
 */
static void lru_add(struct dcache *dc, struct dentry *d);
static void lru_add_at(struct dcache *dc, struct dentry *d, unsigned int idx);

static inline void lru_retain(struct dcache *dc, struct dentry *d)
{
	if (caa_likely(uatomic_load(&d->d_lru.shard, CMM_RELAXED)
		       >= DC_LRU_ON(0))) {
		if (!uatomic_load(&d->d_lru.referenced, CMM_RELAXED))
			uatomic_store(&d->d_lru.referenced, 1, CMM_RELAXED);
		return;
	}
	lru_add(dc, d);				/* re-arm after an LRU_REMOVED */
}


/*
 * Allocate the shard array.  Cacheline-aligned because sharding is pointless if
 * two shards' head/tail/count words share a line.  Under -DDC_LRU_MCAS each
 * shard's list is CIRCULAR, so its sentinel must point at itself -- a zeroed
 * shard is not an empty list, it is a NULL-edged one that faults on the first
 * insert.  Returns 0, or -ENOMEM.
 */
struct dcache *dc_lru_validate_dc;

static int lru_shards_init(struct dcache *dc)
{
	unsigned int i;

	dc_lru_validate_dc = dc;
	dc->nlru = lru_nshards();
	if (posix_memalign((void **) &dc->lru, 64,
			   (size_t) dc->nlru * sizeof(*dc->lru)) != 0)
		return -ENOMEM;
	memset(dc->lru, 0, (size_t) dc->nlru * sizeof(*dc->lru));
#ifdef DC_LRU_MCAS
	for (i = 0; i < dc->nlru; i++)
		urcu_txn_list_init(&dc->lru[i].list);
	/* SEPARATE from the index domain: the LRU is not part of the namespace
	 * index, so an escalation raised by a rename must not capture it. */
	urcu_txn_domain_init(&dc->lru_domain);
#else
	(void) i;
#endif
	return 0;
}

#ifdef DC_LRU_MCAS
/* ======================= THE MCAS ARM (lock-free) ======================= */

static inline struct urcu_txn_list_node *urcu_txn_list_node_ptr(void *v)
{
	return (struct urcu_txn_list_node *) ((uintptr_t) v & ~3UL);
}


/*
 * Thin wrappers around the list mutators, so that under -DDC_TXN_STATS the
 * transaction handle is OURS and its in_fallback / retry state is observable.
 * urcu_txn_list_*_rcu() owns its handle internally, which is exactly the state
 * the escalation question needs -- so the instrumented build re-implements the
 * same convenience bracket with the _prepare form.  The uninstrumented build
 * calls the library's own, unchanged.
 */
#ifdef DC_TXN_STATS
static int lru_list_add_tail(struct dcache *dc, struct urcu_txn_list_node *n,
			     struct urcu_txn_list_head *head)
{
	struct urcu_txn txn;

	urcu_txn_init(&txn, &dc->lru_domain);
	for (;;) {
		enum urcu_txn_status st;
		int p;

		void *pre;

		urcu_txn_begin(&txn);
		DC_TS_BEGIN(DC_TS_LRU_ADD, &txn);
		/*
		 * insert_before_prepare does `newp->next = pos` as a PLAIN store
		 * at PREPARE time, before the commit -- so it clears any residual
		 * deletion MARK on the node whether or not the commit then
		 * succeeds.  Snapshot the mark first (RAW: resolve() would strip
		 * exactly the bit under test) so an ABORT can be checked for
		 * having left the node unmarked AND unlinked, which is a state no
		 * later operation can make sense of: a del of it would record
		 * prev->next : elem -> next against a prev that does not name it,
		 * and that CAS can never match.
		 */
		pre = uatomic_load(&n->next, CMM_RELAXED);
		p = urcu_txn_list_insert_before_prepare(&txn, n, &head->node);
		if (p) {
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			if (p == -ENOMEM)
				return -1;
			continue;
		}
		st = urcu_txn_commit(&txn);
		DC_TS_COMMIT(DC_TS_LRU_ADD, &txn, st);
		urcu_txn_end(&txn);
		if (st != URCU_TXN_STATUS_OK) {
			void *now = uatomic_load(&n->next, CMM_RELAXED);

			if (!((uintptr_t) pre & 0x1UL) &&
			    urcu_txn_list_is_marked(pre) &&
			    !((uintptr_t) now & 0x1UL) &&
			    !urcu_txn_list_is_marked(now))
				DC_TS_INSCLEAR(DC_TS_LRU_ADD);
		} else {
			void *pn = urcu_txn_list_resolve(
				uatomic_load(&head->node.prev, CMM_RELAXED));

			if (urcu_txn_list_node_ptr(pn) != n)
				DC_TS_INSEDGE(DC_TS_LRU_ADD);
		}
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		return st < 0 ? -1 : 0;
	}
}

/*
 * POST-COMMIT AUDIT of a del, to catch the TRANSITION rather than the aftermath.
 *
 * A del applies three edges in ONE commit -- mark the victim's next, unlink it
 * forward from its predecessor, unlink it backward from its successor -- so a
 * successful commit must leave all three applied.  A marked-but-still-linked
 * node means the mark landed and the forward unlink did not, which by MCAS
 * atomicity should be impossible; walking the list only ever showed us that
 * state long after the fact.  This checks it at the instant the commit reports
 * OK, while we still know which three slots were supposed to change.
 *
 * Reads are RESOLVED (a slot may legitimately hold a parked descriptor) --
 * comparing raw values here is exactly the mistake that made the first list
 * validator report proxies as marks.  Only VIOLATIONS are logged; a quiet run
 * costs three resolved loads per del.
 */
static void lru_del_audit(struct urcu_txn_list_node *n,
			  struct urcu_txn_list_node *prev,
			  struct urcu_txn_list_node *next)
{
	void *nn_raw = uatomic_load(&n->next, CMM_RELAXED);
	void *pn = urcu_txn_list_resolve(uatomic_load(&prev->next, CMM_RELAXED));
	/*
	 * The MARK must be tested on the RAW value.  urcu_txn_list_resolve()
	 * returns the LOGICAL pointer and strips it, so resolving first reports
	 * every successful del as unmarked -- 33k false positives on the first
	 * run of this audit.  Skip the test entirely if the slot holds a parked
	 * descriptor (bit 0), which is the mirror-image mistake: testing the mark
	 * on an unresolved proxy is what made the list validator report proxies
	 * as marks.  Raw for the mark, resolved for the pointer, and neither for
	 * a value that is mid-commit.
	 */
	int in_flight = ((uintptr_t) nn_raw & 0x1UL) != 0;
	int bad_mark = !in_flight && !urcu_txn_list_is_marked(nn_raw);
	int bad_fwd = (urcu_txn_list_node_ptr(pn) == n);

	if (caa_unlikely(bad_mark || bad_fwd)) {
		DC_TS_DELAUDIT(DC_TS_LRU_DEL, bad_mark, bad_fwd);
		uatomic_store(&dc_ts_last_slot, (void *) &prev->next, CMM_RELAXED);
		uatomic_store(&dc_ts_last_old, (void *) n, CMM_RELAXED);
		uatomic_store(&dc_ts_last_seen, pn, CMM_RELAXED);
	}
	(void) next;
}

static int lru_list_del(struct dcache *dc, struct urcu_txn_list_node *n)
{
	struct urcu_txn txn;

	urcu_txn_init(&txn, &dc->lru_domain);
	for (;;) {
		enum urcu_txn_status st;
		int p;

		urcu_txn_begin(&txn);
		DC_TS_BEGIN(DC_TS_LRU_DEL, &txn);
		/*
		 * RAW read of the node's own next, bypassing the transaction, to
		 * compare against what del_prepare's up-front marked-check sees
		 * through urcu_txn_load().  If the slot is ALREADY marked in
		 * memory and prepare still proceeds (returns 0 rather than
		 * -ENOENT), the transaction is reading a stale value: it will
		 * then record an expected-old that memory no longer holds, and
		 * its CAS must lose forever.  That is the difference between "a
		 * node is marked-but-linked" and "we cannot SEE that it is
		 * marked", and nothing measured so far separates them.
		 */
		{
			void *raw = uatomic_load(&n->next, CMM_RELAXED);
			int marked_now = ((uintptr_t) raw & 0x2UL) != 0;

			p = urcu_txn_list_del_prepare(&txn, n);
			if (marked_now && p == 0)
				DC_TS_STALE(DC_TS_LRU_DEL);
			else if (marked_now)
				DC_TS_MARKEDSEEN(DC_TS_LRU_DEL);
		}
		if (p) {
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			if (p == -ENOENT)
				return 0;	/* a peer removed it */
			if (p == -ENOMEM)
				return -1;
			DC_TS_EAGAIN(DC_TS_LRU_DEL);
			continue;		/* -EAGAIN: successor mid-delete */
		}
		{
			struct urcu_txn_list_node *pv, *nx;

			pv = urcu_txn_list_node_ptr(urcu_txn_list_resolve(
				uatomic_load(&n->prev, CMM_RELAXED)));
			nx = urcu_txn_list_node_ptr(urcu_txn_list_resolve(
				uatomic_load(&n->next, CMM_RELAXED)));
			st = urcu_txn_commit(&txn);
			DC_TS_COMMIT(DC_TS_LRU_DEL, &txn, st);
			urcu_txn_end(&txn);
			if (st == URCU_TXN_STATUS_OK && pv && nx)
				lru_del_audit(n, pv, nx);
		}
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		return st == URCU_TXN_STATUS_OK ? 1 : -1;
	}
}
#else
static inline int lru_list_add_tail(struct dcache *dc,
				    struct urcu_txn_list_node *n,
				    struct urcu_txn_list_head *head)
{
	return urcu_txn_list_add_tail_rcu(n, head, &dc->lru_domain);
}

static inline int lru_list_del(struct dcache *dc, struct urcu_txn_list_node *n)
{
	return urcu_txn_list_del_rcu(n, &dc->lru_domain);
}
#endif

/* dentry <-> list node */
static inline struct dentry *lru_dentry(struct urcu_txn_list_node *n)
{
	return caa_container_of(n, struct dentry, d_lru.link);
}

/*
 * Enqueue at the TAIL.  CLAIM the node first (OFF -> BUSY): with no lock, two
 * concurrent retains would otherwise both enqueue the same dentry and corrupt
 * its edges.  The claim is what the shard lock did for free in the other arm.
 */
static void lru_add_at(struct dcache *dc, struct dentry *d, unsigned int idx)
{
	struct dc_lru_shard *sh = &dc->lru[idx];

	if (uatomic_cmpxchg(&d->d_lru.shard, DC_LRU_OFF, DC_LRU_BUSY)
	    != DC_LRU_OFF)
		return;				/* someone else owns the change */
	if (lru_list_add_tail(dc, &d->d_lru.link, &sh->list) != 0) {
		uatomic_store(&d->d_lru.shard, DC_LRU_OFF, CMM_RELEASE);
		return;				/* -ENOMEM: simply not listed */
	}
	uatomic_inc(&sh->count);
	uatomic_store(&d->d_lru.shard, DC_LRU_ON(idx), CMM_RELEASE);
}

/*
 * Enqueue on the CALLER'S shard -- the producer path (dc_add, retain_dentry).
 *
 * The shrinker must NOT use this, and getting that wrong is what made
 * --evict bursty collapse.  A rotate is del + re-add, and re-adding through the
 * caller's shard MIGRATES the node onto the SWEEPER'S shard: sweep after sweep
 * the whole cache drains into one shard while the producers keep enqueueing on
 * their own, so a single sentinel ends up carrying every rotate, every producer
 * add, and every unlink.  That is a permanent hot slot, which is what actually
 * drove the transaction domain into escalation.  The lock arm never had it --
 * lru_rotate_locked re-links into the shard being swept.  Use lru_add_at(.., i).
 */
static void lru_add(struct dcache *dc, struct dentry *d)
{
	lru_add_at(dc, d, lru_shard_index(dc));
}

/*
 * IMMEDIATE physical removal from anywhere in the list -- the operation that
 * justifies this arm.  del_rcu splices via the node's OWN edges, so unlike the
 * lock arm it does not even need to find the shard head; the shard is consulted
 * only to decrement the counter.
 *
 * Returns 1 if THIS call removed it.  del_rcu reports that directly (0 means a
 * peer won), so the claim below is only needed to keep an ADD from racing in.
 */
static int lru_del_claimed(struct dcache *dc, struct dentry *d)
{
	unsigned int st = uatomic_load(&d->d_lru.shard, CMM_RELAXED);

	if (st < DC_LRU_ON(0))
		return 0;			/* off, or a peer is mid-change */
	if (uatomic_cmpxchg(&d->d_lru.shard, st, DC_LRU_BUSY) != st)
		return 0;
	{
		int r = lru_list_del(dc, &d->d_lru.link);

		DC_TS_DEL_RET(DC_TS_LRU_DEL, r);
		if (r == 1)
			uatomic_dec(&dc->lru[st - DC_LRU_ON(0)].count);
		uatomic_store(&d->d_lru.shard, DC_LRU_OFF, CMM_RELEASE);
		if (r < 0)
			DC_TS_RELINK_BAD(DC_TS_LRU_DEL);
		return 1;
	}
}

static void lru_del(struct dcache *dc, struct dentry *d)
{
	(void) lru_del_claimed(dc, d);
}

/*
 * ⚠ A BOUNDED, YIELDING shrinker-side delete was TRIED HERE AND MEASURED WORSE.
 *
 * The intent was mainline's rule -- dentry_lru_isolate takes spin_trylock(&d_lock)
 * and answers LRU_SKIP rather than block, so reclaim never makes a foreground op
 * wait.  The port was a fresh transaction per attempt (so age never accumulates),
 * a 4-attempt bound, and skip on exhaustion.
 *
 * It made the collapse STRICTLY WORSE: with it, --evict bursty stopped completing
 * even at --evict-batch 1, which had been fine before.  The reason is that
 * escalation is a property of the DOMAIN, not of the transaction -- once any
 * writer escalates on lru_domain, every subsequent urcu_txn_begin() on it enters
 * the fair-mutex lane, fresh handle or not.  So a "cheap retry" is not cheap: each
 * extra begin() is another futex handoff, and four attempts cost four times the
 * lane traffic that one did.
 *
 * A trylock analogue therefore needs support the front-end does not currently
 * offer -- a way to attempt a commit WITHOUT entering the fallback lane, and to
 * fail rather than queue.  Recorded so the next attempt starts from that, not
 * from the retry-count idea again.
 *
 * ⛔⭐ AND THE UNDERLYING PROBLEM IS NOT IN THIS FILE.  Chasing the collapse to
 * the bottom: urcu_txn__enter_fallback() -> cds_fair_mutex_lock() ->
 * cds_fair_mutex_park() blocks on a futex WHILE THE THREAD IS STILL RCU-ONLINE.
 * Under QSBR an online thread that is not running holds off EVERY grace period,
 * so:
 *
 *	escalate -> park (online) -> grace periods stall -> call_rcu never runs
 *	-> descriptors are never reclaimed -> allocation pressure and conflict
 *	rise -> more escalation
 *
 * which is self-reinforcing and ABSORBING: it never recovers.  Observed exactly
 * that -- two threads in cds_fair_mutex_park and the call_rcu worker still
 * inside urcu_qsbr_synchronize_rcu() on samples six seconds apart -- and the
 * onset is stochastic, which is why the same command line sometimes finishes.
 *
 * It is the park-while-online hazard the repro harnesses already guard against
 * with sem_wait_quiescent(), except here the parking is inside liburcu's own
 * escalation lane, where a caller cannot guard it.  The fix belongs there:
 * go offline across the park and online again after, as every other blocking
 * wait in a QSBR program must.  Whether that is safe at the fallback entry point
 * -- the transaction is at begin() and holds no resolved pointers yet, which is
 * the argument that it is -- is a liburcu decision, not a dcache one.
 */





/*
 * Walk every shard and report any node that is MARKED but still LINKED -- the
 * state every measurement so far points at, and which nothing in the engine is
 * supposed to be able to leave behind (a del applies its mark and both unlinks
 * in one commit, or none of them).
 *
 * Reports position, because that is what distinguishes the candidates: a marked
 * SENTINEL is a different bug from a marked interior node, and "the list is a
 * cycle that never reaches the sentinel" is a third.  Bounded so a corrupt list
 * cannot hang the reporter.
 */
void dc_lru_validate(void *stream)
{
	FILE *f = stream;
	struct dcache *dc = dc_lru_validate_dc;
	unsigned int i;

	if (!dc)
		return;
	for (i = 0; i < dc->nlru; i++) {
		struct urcu_txn_list_head *h = &dc->lru[i].list;
		struct urcu_txn_list_node *n;
		unsigned long pos = 0, marked = 0, proxies = 0;
		unsigned long cnt = uatomic_load(&dc->lru[i].count, CMM_RELAXED);

		if (!cnt)
			continue;
		n = (struct urcu_txn_list_node *)
			((uintptr_t) uatomic_load(&h->node.next, CMM_RELAXED)
			 & ~3UL);
		while (n && n != &h->node && pos < 10000) {
			void *raw = uatomic_load(&n->next, CMM_RELAXED);

			/*
			 * BIT 0 is the engine's proxy tag, BIT 1 the deletion
			 * mark.  A slot holding a parked DESCRIPTOR has bit 0
			 * set and arbitrary bits above it, so testing bit 1 on
			 * an unresolved value reports proxies as marks -- which
			 * at a wedge, where a stuck transaction has a planted
			 * prefix, is exactly the common case.  Classify them
			 * apart and resolve before following the link, or the
			 * walk reports corruption that is really work in flight.
			 */
			if ((uintptr_t) raw & 0x1UL) {
				proxies++;
				break;		/* mid-commit: stop, do not follow */
			}
			if ((uintptr_t) raw & 0x2UL) {
				marked++;
				if (marked <= 4)
					fprintf(f, "LRUCHK shard %u pos %lu: "
						"MARKED-but-LINKED node=%p next=%p\n",
						i, pos, (void *) n, raw);
			}
			n = (struct urcu_txn_list_node *)
				((uintptr_t) raw & ~3UL);
			pos++;
		}
		fprintf(f, "LRUCHK shard %u: count=%lu walked=%lu marked=%lu "
			"proxy-stop=%lu%s\n", i, cnt, pos, marked, proxies,
			pos >= 10000 ? "  (WALK CAPPED -- cycle?)" : "");
		{
			void *sr = uatomic_load(&h->node.next, CMM_RELAXED);

			if ((uintptr_t) sr & 0x2UL)
				fprintf(f, "LRUCHK shard %u: THE SENTINEL ITSELF "
					"IS MARKED (next=%p)\n", i, sr);
		}
	}
}

unsigned long dc_lru_count(struct dcache *dc)
{
	unsigned long n = 0;
	unsigned int i;

	for (i = 0; i < dc->nlru; i++)
		n += uatomic_load(&dc->lru[i].count, CMM_RELAXED);
	return n;
}

#else	/* ================= THE LOCK ARM (per-shard spinlock) ================ */

/* MCAS-only diagnostic; the lock arm has no marked-but-linked state. */
void dc_lru_validate(void *stream) { (void) stream; }
/* ---- PHASE 3: the sharded LRU ------------------------------------------- */

static inline void lru_lock(struct dc_lru_shard *sh)
{
	while (uatomic_cmpxchg(&sh->lock, 0UL, 1UL) != 0UL)
		caa_cpu_relax();
	cmm_smp_mb();
}

static inline void lru_unlock(struct dc_lru_shard *sh)
{
	uatomic_store(&sh->lock, 0UL, CMM_RELEASE);
}

/* Add at the TAIL (newest).  Called with no lock held. */
static void lru_add(struct dcache *dc, struct dentry *d)
{
	unsigned int idx = lru_shard_index(dc);
	struct dc_lru_shard *sh = &dc->lru[idx];

	lru_lock(sh);
	d->d_lru.prev = sh->tail;
	d->d_lru.next = NULL;
	if (sh->tail)
		sh->tail->d_lru.next = d;
	else
		sh->head = d;
	sh->tail = d;
	sh->count++;
	d->d_lru.shard = DC_LRU_ON(idx);
	lru_unlock(sh);
}

/* Splice out, wherever it sits.  Caller holds @sh. */
static void lru_unlink_locked(struct dc_lru_shard *sh, struct dentry *d)
{
	if (d->d_lru.prev)
		d->d_lru.prev->d_lru.next = d->d_lru.next;
	else
		sh->head = d->d_lru.next;
	if (d->d_lru.next)
		d->d_lru.next->d_lru.prev = d->d_lru.prev;
	else
		sh->tail = d->d_lru.prev;
	d->d_lru.prev = d->d_lru.next = NULL;
	d->d_lru.shard = DC_LRU_OFF;
	sh->count--;
}

/*
 * IMMEDIATE physical removal, from an arbitrary position.  This is the operation
 * that decides the whole design (design/dcache-lru-txn.md section 6): the
 * tempting alternative -- mark it dead and let the shrinker reap it -- fails
 * under unlink churn with no memory pressure, because the node's call_rcu free
 * cannot fire while the list still points at it.  That turns "freeable after one
 * grace period" into "freeable whenever reclaim wanders by", i.e. unbounded live
 * memory exactly where a dcache is busiest.  A Harris-style logical mark does not
 * rescue it either: Harris needs every traverser to help unlink, and this list
 * has exactly one traverser.
 */
static void lru_del(struct dcache *dc, struct dentry *d)
{
	unsigned int idx;
	struct dc_lru_shard *sh;

	idx = uatomic_load(&d->d_lru.shard, CMM_RELAXED);
	if (idx < DC_LRU_ON(0))
		return;				/* not on any shard */
	sh = &dc->lru[idx - DC_LRU_ON(0)];
	lru_lock(sh);
	if (d->d_lru.shard >= DC_LRU_ON(0))	/* re-check under the lock */
		lru_unlink_locked(sh, d);
	lru_unlock(sh);
}

unsigned long dc_lru_count(struct dcache *dc)
{
	unsigned long n = 0;
	unsigned int i;

	for (i = 0; i < dc->nlru; i++)
		n += uatomic_load(&dc->lru[i].count, CMM_RELAXED);
	return n;
}
#endif	/* DC_LRU_MCAS */


#else	/* DC_NO_LRU: the A/B control -- no LRU field, no rseq, no shrinker */
static inline void lru_retain(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline void lru_add(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline void lru_del(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
unsigned long dc_lru_count(struct dcache *dc) { (void) dc; return 0; }
long dc_shrink(struct dcache *dc, long nr) { (void) dc; (void) nr; return 0; }
#endif	/* DC_NO_LRU */
#endif	/* DCACHE_LRU_TYPES */
