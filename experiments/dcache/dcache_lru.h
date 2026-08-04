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
 *   struct dentry { ... struct { struct urcu_txn_deque_node dnode;  (MCAS arm)
 *                                struct dentry *prev, *next;        (lock arm)
 *                                unsigned int shard;                (lock arm)
 *                                unsigned char referenced; } d_lru; }
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
 *
 * THE MCAS ARM RUNS ON A DEQUE, NOT A LIST, and the difference is a contract:
 * <urcu/rcu-txn-deque.h> offers RELOCATION and forbids read-side TRAVERSAL,
 * because those two cannot coexist -- a traverser standing on a moved node
 * follows its NEW next and silently skips or repeats a span, and no grace period
 * repairs that (grace periods govern reclamation, not logical position).  This
 * port has no LRU traversal at all: the sweeper re-reads the head every
 * iteration and dc_lookup touches the LRU zero times.  So the LRU is exactly the
 * caller that wants the relocating half of that trade.
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
 * State word encoding -- THE LOCK ARM ONLY.
 *
 * There used to be a third state, DC_LRU_BUSY, and a claim protocol around it,
 * because with no lock the add and del paths had to exclude each other somehow.
 * The MCAS arm no longer has any of it: the deque node's `owner` pointer is
 * written by the same commit that moves the edges, so the commit IS the
 * exclusion and membership cannot desynchronise from the links.  These are
 * deliberately NOT defined on that arm, so any surviving use fails to compile
 * rather than reintroducing a second membership record.
 */
#ifndef DC_LRU_MCAS
#define DC_LRU_OFF	0u		/* not on any shard */
#define DC_LRU_ON(i)	((i) + 2u)	/* on shard i */
#endif

/*
 * LTTng scaffolding (-DDC_ENABLE_TRACING).  Only the COMMIT event survives the
 * move to the deque: dc:claim and dc:wedge instrumented the claim protocol and
 * the stale-prev live-lock, neither of which is expressible any more.  Inert --
 * not even a branch -- in every other build.
 */
#ifdef DC_ENABLE_TRACING
#include "dcache_tp.h"
#define DC_TP_COMMIT(n, a, b, op, st) \
	lttng_ust_tracepoint(dc, commit, (n), (a), (b), (op), (st))
#else
#define DC_TP_COMMIT(n, a, b, op, st)			do { } while (0)
#endif

struct dc_lru_shard {
#ifdef DC_LRU_MCAS
	/*
	 * THE MCAS ARM.  No lock: the shard is a <urcu/rcu-txn-deque.h> deque,
	 * whose every structural change flips ALL its edges in one MCAS commit,
	 * so an arbitrary mid-deque splice is atomic and lock-free.  That is the
	 * property the whole arm exists for -- it is the one thing neither the
	 * two-lock FIFO nor a lazy/tombstone scheme can do
	 * (design/dcache-lru-txn.md sections 6-7), and unlink NEEDS it.
	 *
	 * What it buys: the shrinker never shares a lock with dentry ops, so
	 * producer-vs-consumer contention disappears rather than being bounded.
	 * What it costs: a descriptor and a multi-CAS on EVERY enqueue and EVERY
	 * unlink -- a constant tax on the hot churn path to decouple a consumer
	 * that may be idle.  Section 7 says which wins is decided by RECLAIM
	 * CADENCE, not by readers: bursty reclaim favours the lock, continuous
	 * eviction favours this.  That is a measurement, hence both arms.
	 *
	 * The deque owns its own approximate `count`, so there is no separate
	 * shard counter to drift against it.
	 */
	struct urcu_txn_deque deque;
	char pad[64 - sizeof(struct urcu_txn_deque) % 64];
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

/*
 * Is @d on a shard?  A HINT on both arms, and deliberately only a hint.
 *
 * The lock arm reads the shard word without the lock; the MCAS arm reads the
 * deque node's `owner`, which its own header documents as a hint for exactly
 * the same reason.  Either way this is test-and-then-act: by the time the
 * caller acts a peer may have added or removed @d.  That is SAFE because the
 * only action taken on a false negative is an enqueue, and the enqueue
 * re-derives membership inside its own commit and answers -EEXIST -- so a
 * stale hint costs a wasted attempt, never a wrong edge.
 *
 * DO NOT cache this in a second word.  A separately-maintained membership
 * record next to a transacted one is precisely the defect the deque exists to
 * remove.
 */
#ifdef DC_LRU_MCAS
static inline int lru_listed(struct dentry *d)
{
	int on;

	/* owner may hold a parked proxy, and resolving one dereferences the
	 * writer's descriptor -- so this read needs RCU even though it reads no
	 * neighbour.  Nested inside a caller's section under QSBR: a counter. */
	rcu_read_lock();
	on = urcu_txn_deque_owner(&d->d_lru.dnode) != NULL;
	rcu_read_unlock();
	return on;
}
#else
static inline int lru_listed(struct dentry *d)
{
	return uatomic_load(&d->d_lru.shard, CMM_RELAXED) >= DC_LRU_ON(0);
}
#endif

static inline void lru_retain(struct dcache *dc, struct dentry *d)
{
	if (caa_likely(lru_listed(d))) {
		if (!uatomic_load(&d->d_lru.referenced, CMM_RELAXED))
			uatomic_store(&d->d_lru.referenced, 1, CMM_RELAXED);
		return;
	}
	/*
	 * Re-arm.  On the LOCK arm this is retain_dentry's second half and the
	 * normal way an LRU_REMOVED entry comes back.  On the MCAS arm the
	 * shrinker ROTATES in-use entries rather than removing them, so nothing
	 * routinely lands here -- it is reachable only after an allocation
	 * failure left the node off the deque, or when the hint was stale, and
	 * in the latter case push_tail answers -EEXIST and nothing happens.
	 */
#ifndef DC_LRU_NO_READD
	lru_add(dc, d);		/* see the probe in dcache_lru_shrink.h */
#endif
}


/*
 * Allocate the shard array.  Cacheline-aligned because sharding is pointless if
 * two shards' head/tail/count words share a line.  Under -DDC_LRU_MCAS each
 * shard's deque is CIRCULAR, so its sentinel must point at itself -- a zeroed
 * shard is not an empty deque, it is a NULL-edged one that faults on the first
 * push.  Returns 0, or -ENOMEM.
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
		urcu_txn_deque_init(&dc->lru[i].deque);
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
/*
 * The shard is a <urcu/rcu-txn-deque.h> deque.  Three things that used to live
 * here are GONE, and it is worth naming them because each was a bug source:
 *
 *   THE CLAIM PROTOCOL (DC_LRU_OFF/BUSY/ON, lru_claim, lru_unclaim,
 *   lru_del_claimed, lru_unlink_claimed).  Membership is now the deque node's
 *   `owner` pointer, written by the SAME COMMIT that moves the edges, so the
 *   commit is the exclusion.  Two concurrent enqueues of one dentry both CAS
 *   &n->owner : NULL -> D; the loser aborts, retries, sees non-NULL and gets
 *   -EEXIST.  The old shard word had to carry membership, identity and
 *   exclusion at once and was maintained OUTSIDE the commit, so it could
 *   disagree with the links -- the "claimed but not linked" state that made a
 *   del derive a stale `prev` and lose its CAS forever.
 *
 *   THE DELETION MARK, and with it the raw-versus-resolved rule on link slots.
 *   `owner` is the membership witness, so nothing marks `next`.
 *
 *   lru_move_tail AND ITS RATIONALE.  A general move was justified as
 *   protecting "a lockless traverser standing on the node".  There is no such
 *   traverser -- dc_lookup touches the LRU zero times and the sweeper re-reads
 *   the head every iteration -- so the rationale was vacuous.  The sweeper's
 *   second chance is urcu_txn_deque_rotate_head(), a NARROWER primitive whose
 *   predecessor is always the sentinel, which is what makes its six slots
 *   provably distinct.
 *
 * RCU is still required, and protects something other than readers: the
 * mutators read a node's neighbours and then CAS into them, so a neighbour
 * freed in between is a use-after-free.  Every entry point below therefore
 * brackets its commit in rcu_read_lock().  Under QSBR that is a nesting
 * counter, so nesting inside a caller's section costs nothing.
 */

static inline struct dentry *lru_dentry(struct urcu_txn_deque_node *n)
{
	return caa_container_of(n, struct dentry, d_lru.dnode);
}

/*
 * The three mutators.  Under -DDC_TXN_STATS the convenience brackets in the
 * header are re-implemented here so that the transaction HANDLE is ours and its
 * in_fallback / retry state is observable -- that is the state the escalation
 * question needs.  The uninstrumented build calls the header's own, unchanged.
 *
 * ⚠ The instrumented bracket must mirror the header's TERMINAL BAIL exactly.
 * An escalated handle KEEPS its lane across end() so that a re-attempt does not
 * go to the back of the FIFO, so a path that does NOT re-attempt must
 * urcu_txn_abandon() first or the domain's lane is held forever and every other
 * writer parks behind it.  push_tail answers -EEXIST on every duplicate, which
 * is a hot path here, so this is not a corner case (b69b4a53).
 */
#ifdef DC_TXN_STATS
#define LRU_DQ_BRACKET(site, dcp, call)					\
	struct urcu_txn txn;						\
	int prep;							\
	enum urcu_txn_status st;					\
									\
	urcu_txn_init(&txn, &(dcp)->lru_domain);			\
	for (;;) {							\
		urcu_txn_begin(&txn);					\
		DC_TS_BEGIN((site), &txn);				\
		prep = (call);						\
		if (prep && prep != -EAGAIN) {				\
			/* terminal: NOT a conflict, and must abandon */	\
			urcu_txn_abandon(&txn);				\
			urcu_txn_end(&txn);				\
			return prep;					\
		}							\
		if (prep == -EAGAIN) {					\
			urcu_txn_conflict(&txn);			\
			urcu_txn_end(&txn);				\
			DC_TS_EAGAIN(site);				\
			continue;					\
		}							\
		st = urcu_txn_commit(&txn);				\
		DC_TS_COMMIT((site), &txn, st);				\
		urcu_txn_end(&txn);					\
		if (st == URCU_TXN_STATUS_ABORT)			\
			continue;					\
		return st == URCU_TXN_STATUS_OK ? 0 : -ENOMEM;		\
	}

static int lru_dq_push(struct dcache *dc, struct urcu_txn_deque *q,
		       struct urcu_txn_deque_node *n)
{
	LRU_DQ_BRACKET(DC_TS_LRU_ADD, dc,
		urcu_txn_deque_push_tail_prepare(&txn, q, n))
}

static int lru_dq_remove(struct dcache *dc, struct urcu_txn_deque *q,
			 struct urcu_txn_deque_node *n)
{
	LRU_DQ_BRACKET(DC_TS_LRU_DEL, dc,
		urcu_txn_deque_remove_prepare(&txn, q, n))
}

static int lru_dq_rotate(struct dcache *dc, struct urcu_txn_deque *q)
{
	LRU_DQ_BRACKET(DC_TS_LRU_ROT, dc,
		urcu_txn_deque_rotate_head_prepare(&txn, q))
}
#else
static inline int lru_dq_push(struct dcache *dc, struct urcu_txn_deque *q,
			      struct urcu_txn_deque_node *n)
{
	return urcu_txn_deque_push_tail(q, n, &dc->lru_domain);
}

static inline int lru_dq_remove(struct dcache *dc, struct urcu_txn_deque *q,
				struct urcu_txn_deque_node *n)
{
	return urcu_txn_deque_remove(q, n, &dc->lru_domain);
}

static inline int lru_dq_rotate(struct dcache *dc, struct urcu_txn_deque *q)
{
	return urcu_txn_deque_rotate_head(q, &dc->lru_domain);
}
#endif

/*
 * Enqueue at the TAIL of shard @idx.
 *
 * No claim: push_tail's own &n->owner : NULL -> q record is the guard, so a
 * dentry that is already queued -- anywhere, including on another shard --
 * answers -EEXIST and nothing is written.  That is the whole reason lru_retain
 * may branch on a stale hint.
 */
static void lru_add_at(struct dcache *dc, struct dentry *d, unsigned int idx)
{
	struct urcu_txn_deque *q = &dc->lru[idx].deque;
	int r;

	rcu_read_lock();
	r = lru_dq_push(dc, q, &d->d_lru.dnode);
	/* The trace reads the node's own links, so it belongs INSIDE the read
	 * section: once out of it a peer's unlink can retire @d. */
	if (r == 0)
		DC_TP_COMMIT(&d->d_lru.dnode,
			     uatomic_load(&d->d_lru.dnode.prev, CMM_RELAXED),
			     uatomic_load(&d->d_lru.dnode.next, CMM_RELAXED),
			     1, 0);
	rcu_read_unlock();
	if (r == 0)
		uatomic_inc(&q->count);
	/* -EEXIST: already queued, nothing to do.  -ENOMEM: simply not listed;
	 * lru_retain re-arms it on the next touch. */
}

/*
 * Enqueue on the CALLER'S shard -- the producer path (dc_add, retain_dentry).
 *
 * The shrinker must NOT use this, and getting that wrong is what made
 * --evict bursty collapse.  Re-adding through the caller's shard MIGRATES the
 * node onto the SWEEPER'S shard: sweep after sweep the whole cache drains into
 * one shard while the producers keep enqueueing on their own, so a single
 * sentinel ends up carrying every producer add and every unlink.  That is a
 * permanent hot slot.  Use lru_add_at(.., i).
 */
static void lru_add(struct dcache *dc, struct dentry *d)
{
	lru_add_at(dc, d, lru_shard_index(dc));
}

/*
 * ONE attempt at physical removal from wherever @d sits.  Returns 1 if THIS
 * call removed it, 0 if it was already off or a peer won.
 *
 * The owner read is a hint, but it does not need to be more than one: remove
 * re-derives membership inside its own commit and answers -ENOENT both for "not
 * queued" and for "queued somewhere else", so a stale hint costs an attempt.
 * Note the count is decremented from the deque we actually removed from, not
 * from the caller's shard -- the two differ whenever a dentry migrated.
 */
static int lru_try_del(struct dcache *dc, struct dentry *d)
{
	struct urcu_txn_deque *q;
	int r;

	rcu_read_lock();
	q = urcu_txn_deque_owner(&d->d_lru.dnode);
	r = q ? lru_dq_remove(dc, q, &d->d_lru.dnode) : -ENOENT;
	/* Inside the read section: see lru_add_at(). */
	if (r == 0)
		DC_TP_COMMIT(&d->d_lru.dnode,
			     uatomic_load(&d->d_lru.dnode.prev, CMM_RELAXED),
			     uatomic_load(&d->d_lru.dnode.next, CMM_RELAXED),
			     2, 0);
	rcu_read_unlock();
	if (r == 0) {
		uatomic_dec(&q->count);
		return 1;
	}
	return 0;
}

/*
 * Remove @d from whatever shard it is on, and do not return until it is off.
 *
 * dc_unlink calls this immediately before call_rcu'ing the dentry free, so
 * returning early would leave a deque pointing into storage about to be
 * recycled.  The loop is bounded in practice by the fact that nothing re-adds a
 * dentry that is being unlinked; it retries on -ENOMEM too, because a
 * descriptor shortage is transient and the alternative is a use-after-free.
 *
 * There is no BUSY state to spin on any more: a peer's commit either has
 * already taken the node off (owner reads NULL and we return) or has not
 * (our own commit takes it off).
 */
static void lru_del(struct dcache *dc, struct dentry *d)
{
	while (lru_listed(d))
		(void) lru_try_del(dc, d);
}

/*
 * The MCAS arm ROTATES an in-use entry rather than removing it; see
 * dcache_lru_shrink.h for why the two arms diverge here.  Consumed by
 * test_dcache.c, which must not expect a populated directory to leave the LRU.
 */
const int dc_lru_inuse_is_removed = 0;

/*
 * ⚠ A BOUNDED, YIELDING shrinker-side delete was TRIED AND MEASURED WORSE.
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
 * ⛔⭐ AND ONE HALF OF THAT PROBLEM IS NOT IN THIS FILE.  Chasing the collapse to
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
 *
 * The OTHER half was a retry wedge, and the deque removes it by construction:
 * it had a plain, un-CAS'd prepare-time store to the inserted node's own links
 * and a separately-maintained membership word, and neither exists here.
 */

/*
 * STRUCTURAL DUMP of every shard, called from the stats signal handler -- i.e.
 * on a process that may be WEDGED, not on a quiescent one.  It is therefore
 * bounded everywhere and reports rather than asserts.
 *
 * It reads the raw link fields instead of an accessor ON PURPOSE: the deque
 * exports no traversal, and it must not grow one, because a traversal accessor
 * silently breaks every rotate for every caller.  A bounded diagnostic walk in
 * the engine that owns the nodes is a different thing from an exported
 * contract, and this is the only place that does it.
 *
 * What it checks is the half of the deque test's biconditional that is decidable
 * from the dcache alone: every node reachable from a shard's sentinel must name
 * THAT shard as its owner.  The other half (owner set => reachable) needs the
 * node array, which only test_deque.c has.
 *
 * ⚠ READ `disowned` AS AN AMPLIFIED COUNT, NOT AS A NUMBER OF DEFECTS.  A
 * removed node keeps stale next/prev by design, so the moment this walk steps
 * onto ONE departed node it leaves the live ring and follows that node's stale
 * links for as long as they happen to chain -- possibly all the way back to the
 * sentinel.  One bad edge can therefore report a hundred disowned nodes.  What
 * localises the defect is `first-bad`, the hop at which the walk left the ring:
 * first-bad 0 means the SENTINEL itself names a departed node.
 */
void dc_lru_validate(void *stream)
{
	FILE *f = stream;
	struct dcache *dc = dc_lru_validate_dc;
	unsigned int i;

	if (!dc)
		return;
	for (i = 0; i < dc->nlru; i++) {
		struct urcu_txn_deque *q = &dc->lru[i].deque;
		struct urcu_txn_deque_node *n;
		unsigned long pos = 0, disowned = 0, inflight = 0;
		unsigned long cnt = uatomic_load(&q->count, CMM_RELAXED);
		long first_bad = -1;
		int closed = 0;

		if (!cnt)
			continue;
		n = urcu_txn_deque_resolve(
			uatomic_load((void **) &q->sentinel.next, CMM_RELAXED));
		while (n && pos < 10000) {
			void *raw;

			if (n == &q->sentinel) {
				closed = 1;
				break;
			}
			/*
			 * A node's owner is a transacted slot, so it can hold a
			 * parked descriptor.  Resolving it here would dereference
			 * a writer's descriptor from a signal handler on a wedged
			 * process, so count those apart instead of following them.
			 */
			raw = uatomic_load((void **) &n->owner, CMM_RELAXED);
			if ((uintptr_t) raw & URCU_TXN_TAG)
				inflight++;
			else if ((struct urcu_txn_deque *) raw != q) {
				if (first_bad < 0)
					first_bad = (long) pos;
				disowned++;
				if (disowned <= 4)
					fprintf(f, "LRUCHK shard %u pos %lu: "
						"LINKED-but-owner=%p (want %p) "
						"node=%p\n", i, pos, raw,
						(void *) q, (void *) n);
			}
			raw = uatomic_load((void **) &n->next, CMM_RELAXED);
			n = urcu_txn_deque_resolve(raw);
			pos++;
		}
		fprintf(f, "LRUCHK shard %u: count=%lu walked=%lu disowned=%lu "
			"first-bad=%ld owner-in-flight=%lu closed=%d%s\n",
			i, cnt, pos, disowned, first_bad, inflight, closed,
			pos >= 10000 ? "  (WALK CAPPED -- cycle?)" : "");
	}
}

unsigned long dc_lru_count(struct dcache *dc)
{
	unsigned long n = 0;
	unsigned int i;

	for (i = 0; i < dc->nlru; i++)
		n += uatomic_load(&dc->lru[i].deque.count, CMM_RELAXED);
	return n;
}

#else	/* ================= THE LOCK ARM (per-shard spinlock) ================ */

const int dc_lru_inuse_is_removed = 1;	/* LOCK arm REMOVES, as the kernel does */

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
const int dc_lru_inuse_is_removed = 1;
#endif	/* DC_NO_LRU */
#endif	/* DCACHE_LRU_TYPES */
