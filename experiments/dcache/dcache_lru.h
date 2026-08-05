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
 *   int lru_alive_validate(struct urcu_txn *, struct dentry *);  (MCAS arm)
 *       -- 0 if @d is still hashed, -ENOENT if it has been unhashed.  This is
 *          mainline retain_dentry's FIRST test (d_unhashed), which this port
 *          omitted; see lru_push_prepare().  An engine whose liveness bit lives
 *          in a slot the MCAS engine exclusively owns should RECORD A VALIDATE
 *          so the answer joins the commit's conflict set; one whose bit is
 *          written by a plain store under some other lock MUST NOT, and says so.
 *   #define DC_LRU_ALIVE_TRANSACTED 0|1                          (MCAS arm)
 *       -- WHICH of those two the engine just did, as a value this header can
 *          branch on.  It is not documentation: an engine that answers 0 gets
 *          lru_retain's re-arm COMPILED OUT, because a guard that only narrows
 *          cannot make that push safe.  Measured, not asserted -- with the
 *          window widened (-DDC_LRU_PUSH_DELAY) the 0-engine fires 10/10 and
 *          the 1-engine 0/10.  See lru_retain().
 *   int lru_alive_hint(struct dentry *);   (LOCK arm)
 *       -- the same question with no transaction to put it in.
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
/*
 * ⭐ ISOLATION AND OWNERSHIP ARE THE SAME ACT -- the shrink-list handoff.
 *
 *   DC_LRU_OFF        ownerless: no shard names it, nobody owes it a free.
 *   DC_LRU_DEAD       TERMINAL: unhashed and queued for reclaim -- never link it
 *   DC_LRU_ON(i)      LINKED into shard i's list  -- mainline DCACHE_LRU_LIST
 *   + DC_LRU_SHRINK_BIT   ... off the list, but shard i's SHRINKER holds it
 *                                                  -- mainline DCACHE_SHRINK_LIST
 *   + DC_LRU_KILL_BIT     ... and a killer handed it the free (`can_free`)
 *
 * ⭐⭐ THE WORD IS THE EXCLUSION, and it has to be, because THE TWO SIDES OF
 * THIS RACE DO NOT SHARE A SHARD LOCK.  lru_add enqueues on the CALLER'S shard
 * while a killer locks the shard the dentry is CURRENTLY on -- different locks
 * the moment a dentry migrates, and NO lock at all when the killer finds the
 * word already OFF.  So no amount of shard-lock discipline orders an unhash
 * against a concurrent enqueue; the "still hashed" test in lru_add is a plain
 * read of a slot written under a bucket lock the enqueue does not take, and it
 * NARROWS the race without closing it.  (Measured: with only the shrink-list
 * handoff below, --evict bursty went 5/5 -> 0/5 and --evict continuous stayed
 * 5/5, the witness being a dentry freed while LINKED, state ON(14).)
 *
 * The fix is that every transition out of OFF is an RMW on this one word:
 *
 *   lru_add        cmpxchg OFF -> ON(j), TAKEN UNDER SHARD j's LOCK so the
 *                  claim can never be observed before the links exist -- the
 *                  "claimed but not linked" state is the old defect this port
 *                  already paid for once.
 *   a killer       cmpxchg OFF -> DEAD, which SEALS the dentry: every later
 *                  claim fails, so no enqueue can follow the free.  Exactly one
 *                  of the two CASes wins; if the adder wins, the killer then
 *                  reads ON(j), blocks on shard j's lock until the link is
 *                  complete, and splices it out.
 *
 * ⚠ A KILLER MUST NEVER LET THE WORD PASS THROUGH OFF on its way to DEAD.  The
 * transient would be visible to an adder holding a DIFFERENT shard's lock,
 * which would then claim and link a dentry already queued for reclaim -- the
 * very hole being closed.  That is why lru_unlink_locked() takes its successor
 * state as an argument instead of hardcoding OFF.
 *
 * Correctness therefore no longer rests on the liveness read at all; that read
 * survives only as an optimisation (do not bother claiming a dentry already
 * known dead).  Mainline gets the same atomicity from d_lock, which serialises
 * retain_dentry's d_lru_add against __dentry_kill's unhash + d_lru_del; this
 * port has no per-dentry lock, and the state word is what stands in for one.
 *
 * The shrink bit is what makes the isolate safe, and it is the one thing this
 * port had left out.  Mainline's dentry_lru_isolate does NOT make its victim
 * ownerless: d_lru_shrink_move() takes it off the shared list onto a PRIVATE
 * one with DCACHE_LRU_LIST still set and DCACHE_SHRINK_LIST added, and
 * __dentry_kill honours that by skipping d_lru_del and leaving can_free =
 * false, handing the free to shrink_dentry_list.  So a victim under eviction
 * is never unowned, and a killer that meets one does not free it -- it
 * DELEGATES.
 *
 * This port isolated by UNLINKING, which disowned the victim for the whole
 * length of the eviction.  A concurrent dc_unlink could then unhash it, find
 * nothing on the LRU to remove, call_rcu it -- and the shrinker's put-back
 * would link a dentry already queued for reclaim.  -DDC_LRU_FREE_ASSERT caught
 * that 5/5 on BOTH cadences at 48 writers.
 *
 * ⭐ ONE WORD, and every transition to it is written UNDER THE SHARD LOCK, so
 * membership, holder and debt stay coherent with the links.  That is the same
 * rule the MCAS arm gets from the deque's `owner` living inside the commit: a
 * separately-maintained second membership record is exactly the defect both
 * arms exist to avoid.
 */
#define DC_LRU_OFF		0u		/* not on any shard */
#ifndef DC_LRU_NO_DEAD_SEAL
#define DC_LRU_DEAD		1u		/* terminal: sealed by a killer */
#else
/*
 * MUTATION TEST (-DDC_LRU_NO_DEAD_SEAL): collapse DEAD onto OFF, which retires
 * the seal without touching any other line -- `st == DC_LRU_DEAD` becomes the
 * old early return on OFF, and every "seal it" store becomes the old release.
 * The shrink-list handoff below is left intact, so this isolates exactly what
 * the seal contributes.  If it changes nothing, the seal is not the fix and the
 * comment above is a story.
 */
#define DC_LRU_DEAD		DC_LRU_OFF
#endif
#define DC_LRU_ON(i)		((i) + 2u)	/* linked into shard i */
#define DC_LRU_SHRINK_BIT	0x80000000u	/* shard i's shrinker holds it */
#define DC_LRU_KILL_BIT		0x40000000u	/* ... and owes it a free */
#define DC_LRU_SHRINK(i)	(DC_LRU_ON(i) | DC_LRU_SHRINK_BIT)
#define DC_LRU_STATE_BITS	(DC_LRU_SHRINK_BIT | DC_LRU_KILL_BIT)
#define DC_LRU_SHARD_OF(st)	((((st) & ~DC_LRU_STATE_BITS)) - DC_LRU_ON(0))
/* OWNED: some shard is responsible for it -- linked, or held by its shrinker.
 * This is the DCACHE_LRU_LIST question, and so the one lru_retain must ask. */
#define DC_LRU_IS_OWNED(st)	(((st) & ~DC_LRU_STATE_BITS) >= DC_LRU_ON(0))
#define DC_LRU_IS_SHRINK(st)	(((st) & DC_LRU_SHRINK_BIT) != 0u)
/* LINKED: actually spliced into the shard's list, so lru_unlink_locked applies.
 * ⚠ NOT the same question as OWNED -- a shrink-held victim answers yes to one
 * and no to the other, and conflating them is what re-disowns the victim. */
#define DC_LRU_IS_LINKED(st)	(DC_LRU_IS_OWNED(st) && !DC_LRU_IS_SHRINK(st))
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
#ifdef DC_LRU_MCAS
/* The shrinker's re-add targets a SPECIFIC shard; only that arm has one. */
static void lru_add_at(struct dcache *dc, struct dentry *d, unsigned int idx);
#endif
static void lru_del(struct dcache *dc, struct dentry *d);
static int lru_del_can_free(struct dcache *dc, struct dentry *d, int freeing);
static inline void lru_retain(struct dcache *dc, struct dentry *d);
static void lru_assert_not_queued(struct dentry *d);

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
#ifdef DC_LRU_MCAS
static void lru_add_at(struct dcache *dc, struct dentry *d, unsigned int idx);
#endif

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
	/* owner(), so this answers "queued OR SEALED" -- both of which mean
	 * "do not re-arm", which is exactly what lru_retain wants.  Anything
	 * asking the MEMBERSHIP question must use urcu_txn_deque_queued(). */
	on = urcu_txn_deque_owner(&d->d_lru.dnode) != NULL;
	rcu_read_unlock();
	return on;
}
#else
/*
 * ⭐ OWNED, not LINKED.  A victim the shrinker is holding is off the shard's
 * list but still owned by it, and mainline answers this question with
 * DCACHE_LRU_LIST -- which d_lru_shrink_move deliberately LEAVES SET.  So
 * retain_dentry does not re-add a dentry under eviction, which is one half of
 * why mainline's shrinker can be interrupted safely at all.
 */
static inline int lru_listed(struct dentry *d)
{
	return DC_LRU_IS_OWNED(uatomic_load(&d->d_lru.shard, CMM_RELAXED));
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
	 *
	 * ⚠ TWO SEPARATE PROBES, because there are TWO re-adds and one macro used
	 * to gate both -- which made every result from it uninterpretable:
	 *
	 *   -DDC_LRU_NO_RETAIN_READD   kills THIS one (a third party re-arming a
	 *                              dentry the shrinker is mid-eviction on);
	 *   -DDC_LRU_NO_SHRINK_READD   kills the shrinker's own put-back, in
	 *                              dcache_lru_shrink.h;
	 *   -DDC_LRU_NO_READD          kills BOTH (the historical spelling; keep
	 *                              it only for reproducing old results).
	 */
#if defined(DC_LRU_MCAS) && !DC_LRU_ALIVE_TRANSACTED && \
	defined(DC_LRU_NO_DEQUE_SEAL) && !defined(DC_LRU_MCAS_RETAIN_READD)
	/*
	 * ⛔ NO RE-ARM WHEN THE ENGINE CANNOT TRANSACT THE LIVENESS GUARD
	 * *AND* THE DEQUE SEAL IS DISABLED (-DDC_LRU_NO_DEQUE_SEAL).
	 *
	 * ⭐ THE SEAL RETIRED THIS.  urcu_txn_deque_remove_seal_prepare() poisons
	 * `owner` in the same commit that unlinks, so a push after the kill is
	 * refused by the engine rather than by a guard that has to be read
	 * early.  With it, the re-arm is safe on BOTH engines and is kept -- the
	 * branch below survives only as the A/B arm that shows what the seal is
	 * worth.  See lru_del_can_free().
	 *
	 * This was the last free-while-queued pusher on the MCAS arm.  The
	 * guard in lru_push_prepare() has to be part of the SAME COMMIT as the
	 * edges, and DC_LRU_ALIVE_TRANSACTED says whether the engine can do
	 * that.  On bucketlock it cannot: bl_hlist_del_locked writes
	 * d_hash.next with a plain store, so an MCAS proxy there would be
	 * clobbered and the settle would resurrect a deleted node.  The guard
	 * therefore only NARROWS, and a dentry unhashed between the read and
	 * the commit still gets pushed onto a deque that then names freed
	 * storage -- the witness said exactly that: owner == next == prev, the
	 * victim was the SOLE element of its shard, freshly pushed.
	 *
	 * ⭐⭐ PROVEN BY A TARGETED REPRO, because natural timings could not
	 * gate it: the defect is ~2 in 64 runs, and a 40-trial mutation test
	 * came back 0/40 on BOTH arms, proving nothing.  With
	 * -DDC_LRU_PUSH_DELAY widening the window, at 48w/48r --evict
	 * continuous, 10 runs:
	 *
	 *	bucketlock, re-arm restored (plain guard)     10/10 FIRES
	 *	bucketlock, re-arm removed  (this)             0/10
	 *	txn,        re-arm restored (transacted guard)  0/10
	 *
	 * That last row is why this is conditional rather than blanket: the
	 * transacted guard genuinely closes it, so the txn engine keeps its
	 * re-arm and its LRU accuracy.  It is also a direct demonstration of
	 * the engine contract at the top of this file -- the two spellings of
	 * lru_alive_validate are not stylistic.
	 *
	 * The cost here is near zero, and structurally so rather than by luck:
	 * NOTHING TAKES A LIVE DENTRY OFF THE DEQUE on this arm.  The shrinker
	 * ROTATES in-use entries instead of removing them
	 * (dc_lru_inuse_is_removed == 0) and lru_del runs only from dc_unlink,
	 * on a dentry that is dying anyway.  So the only node this path could
	 * legitimately re-arm is one that never got on (lru_add answered
	 * -ENOMEM), and the price is that it stays un-evictable until it is
	 * unlinked.  A stale hint lands here too and push_tail answers -EEXIST.
	 *
	 * ⚠ The LOCK arm MUST keep re-arming: its shrinker REMOVES in-use
	 * entries the way mainline does, so an entry off the list really is
	 * waiting for its next touch.  That arm is safe because the state word
	 * carries a terminal DC_LRU_DEAD a killer CASes in -- an exclusion the
	 * deque's `owner` slot has no equivalent of, since NULL there means
	 * "free to push" and nothing can poison it in the same commit that
	 * removes.  Sealing the node would need a remove-to-POISON variant in
	 * <urcu/rcu-txn-deque.h>: the principled fix, a liburcu change, and
	 * what would let bucketlock keep its re-arm too.
	 *
	 * -DDC_LRU_MCAS_RETAIN_READD restores it (the mutation arm).
	 */
	(void) dc;
#elif !defined(DC_LRU_NO_READD) && !defined(DC_LRU_NO_RETAIN_READD)
	lru_add(dc, d);
#else
	(void) dc;
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
 * The three mutators, each with its own hand-opened bracket rather than the
 * header's convenience form.  TWO reasons, and the second is the load-bearing
 * one now:
 *
 *   under -DDC_TXN_STATS the transaction HANDLE is ours, so its in_fallback /
 *   retry state is observable -- the state the escalation question needs;
 *
 *   the push must COMPOSE a liveness guard with the deque's own prepare, and a
 *   convenience bracket cannot express "these two records commit together".
 *   See lru_push_prepare().
 *
 * ⚠ The bracket must mirror the header's TERMINAL BAIL exactly.  An escalated
 * handle KEEPS its lane across end() so that a re-attempt does not go to the
 * back of the FIFO, so a path that does NOT re-attempt must urcu_txn_abandon()
 * first or the domain's lane is held forever and every other writer parks
 * behind it.  push_tail answers -EEXIST on every duplicate, which is a hot path
 * here, so this is not a corner case (b69b4a53).
 */
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

/*
 * PUSH, COMPOSED WITH A LIVENESS GUARD -- retain_dentry's FIRST test, which this
 * port omitted.
 *
 * Mainline's retain_dentry (fs/dcache.c) opens with
 *
 *	// Unreachable? Nobody would be able to look it up, no point retaining
 *	if (unlikely(d_unhashed(dentry)))
 *		return false;
 *
 * and only then considers the LRU.  We went straight to the enqueue, so a
 * dentry that had just been unhashed and handed to call_rcu could be pushed
 * back on -- and then freed while a deque still named it.  Note this is NOT the
 * refcount's job upstream: d_unhashed is tested before any count is consulted.
 *
 * ⚠ A BARE CHECK IS ONLY A NARROWER RACE.  The unhash can land between the test
 * and the commit.  Mainline closes that with d_lock; here the guard has to be
 * part of the SAME COMMIT as the edges, which is what lru_alive_validate() is
 * for -- an engine that can transact its liveness slot records a conflict-set
 * entry, so a concurrent unhash aborts this push instead of racing it.
 *
 * Returns -ENOENT when @d is already unhashed, which the caller treats as "do
 * not re-arm", not as an error.
 */
static int lru_push_prepare(struct urcu_txn *txn, struct urcu_txn_deque *q,
			    struct dentry *d)
{
	int ret = lru_alive_validate(txn, d);

	if (ret)
		return ret;			/* -ENOENT: unhashed, drop it */
#ifdef DC_LRU_PUSH_DELAY
	{
		/*
		 * TARGETED REPRO (-DDC_LRU_PUSH_DELAY), not a control.
		 *
		 * WIDEN the exact window this guard fails to close on the
		 * bucketlock engine: between the liveness read above and the
		 * commit that installs the edges, a peer can unhash @d and hand
		 * it to call_rcu, and the push then queues a dentry that is
		 * about to be freed.  In the wild that window is a few hundred
		 * cycles and the defect surfaced 2 times in 64 runs -- too rare
		 * for a 40-trial mutation test to gate, which is exactly why the
		 * mutation arm came back 0/40 and proved nothing.
		 *
		 * ⚠ IT IS A REPRODUCER, SO IT ONLY EVER ARGUES ONE WAY.  Firing
		 * with it on says the race is real and says which build closes
		 * it; NOT firing with it on says nothing about the shipped
		 * timings.  Never quote a rate measured under it as the rate.
		 */
		volatile unsigned int spin;

		for (spin = 0; spin < 2000; spin++)
			caa_cpu_relax();
	}
#endif
	return urcu_txn_deque_push_tail_prepare(txn, q, &d->d_lru.dnode);
}

static int lru_dq_push(struct dcache *dc, struct urcu_txn_deque *q,
		       struct dentry *d)
{
	LRU_DQ_BRACKET(DC_TS_LRU_ADD, dc,
		lru_push_prepare(&txn, q, d))
}

static int lru_dq_remove(struct dcache *dc, struct urcu_txn_deque *q,
			 struct urcu_txn_deque_node *n)
{
	LRU_DQ_BRACKET(DC_TS_LRU_DEL, dc,
		urcu_txn_deque_remove_prepare(&txn, q, n))
}

#ifndef DC_LRU_NO_DEQUE_SEAL
/*
 * The two halves of a KILL: take @n off (if it is on) and make it unpushable,
 * in ONE commit either way.  See lru_del_can_free().
 */
static int lru_dq_remove_seal(struct dcache *dc, struct urcu_txn_deque *q,
			      struct urcu_txn_deque_node *n)
{
	LRU_DQ_BRACKET(DC_TS_LRU_DEL, dc,
		urcu_txn_deque_remove_seal_prepare(&txn, q, n))
}

static int lru_dq_seal(struct dcache *dc, struct urcu_txn_deque_node *n)
{
	LRU_DQ_BRACKET(DC_TS_LRU_DEL, dc,
		urcu_txn_deque_seal_prepare(&txn, n))
}
#endif	/* !DC_LRU_NO_DEQUE_SEAL */

static int lru_dq_rotate(struct dcache *dc, struct urcu_txn_deque *q)
{
	LRU_DQ_BRACKET(DC_TS_LRU_ROT, dc,
		urcu_txn_deque_rotate_head_prepare(&txn, q))
}

/*
 * Enqueue at the TAIL of shard @idx.
 *
 * No claim: push_tail's own &n->owner : NULL -> q record is the guard, so a
 * dentry that is already queued -- anywhere, including on another shard --
 * answers -EEXIST and nothing is written.  That is the whole reason lru_retain
 * may branch on a stale hint.
 *
 * -ENOENT means @d was already unhashed, so it must NOT be listed: enqueueing a
 * dentry that is on its way to call_rcu is what let one be freed while a deque
 * still named it.  Silently correct, like -EEXIST, and not an error.
 */
static void lru_add_at(struct dcache *dc, struct dentry *d, unsigned int idx)
{
	struct urcu_txn_deque *q = &dc->lru[idx].deque;
	int r;

	rcu_read_lock();
	r = lru_dq_push(dc, q, d);
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
	/* -EEXIST: already queued, nothing to do.  -ENOENT: unhashed, must not
	 * be listed.  -ENOMEM: simply not listed; lru_retain re-arms it on the
	 * next touch. */
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
	/* queued(), NOT owner(): a SEALED node has a non-NULL owner and is on
	 * no deque, so owner() would hand lru_dq_remove the poison value. */
	q = urcu_txn_deque_queued(&d->d_lru.dnode);
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
	/* ⚠ NOT lru_listed(): that answers "queued OR sealed", and a sealed
	 * node never leaves, so this loop would never terminate. */
	while (urcu_txn_deque_queued(&d->d_lru.dnode))
		(void) lru_try_del(dc, d);
}

/*
 * The MCAS arm never hands a free OFF -- it always answers "yes, free it" --
 * but when the caller really is freeing, it must also make the node
 * UNPUSHABLE, and that is what @freeing selects.
 *
 * ⭐ THE SEAL IS WHY THIS ARM CAN KEEP lru_retain's RE-ARM.  A plain remove
 * leaves `owner` NULL, which is exactly what a push wants, so between the
 * remove and the call_rcu a concurrent push may legitimately queue storage that
 * is already condemned.  No check outside the commit closes that; it narrows
 * it, and this port shipped the narrowed version and measured it firing.
 * urcu_txn_deque_remove_seal_prepare() writes `owner : q -> POISON` in the SAME
 * three-slot commit as the unlink, so there is no instant at which a push can
 * win -- the deque's own analogue of the LOCK arm's terminal DC_LRU_DEAD.
 *
 * Two entry points because the node may or may not currently be queued, and
 * the loop is bounded: every outcome that retries requires a peer to have won
 * a CAS on the one slot both sides contend.
 *
 * ⛔ EVICT-FIRST STILL DOES NOT PORT TO THE LOCK ARM -- tried, measured,
 * reverted (no improvement, 5/5 SEGV inside urcu_txn_install_mw_depth on a
 * dangling slot).  There, isolation WAS the ownership, so evicting first
 * removes it with nothing to replace it and many sweepers evict one victim at
 * once.  That arm uses the shrink state instead; see the other half of this
 * file.
 */
static int lru_del_can_free(struct dcache *dc, struct dentry *d, int freeing)
{
	if (!freeing) {
		lru_del(dc, d);
		return 1;
	}
#ifdef DC_LRU_NO_DEQUE_SEAL
	/* A/B ARM: the pre-seal behaviour -- remove, then free, with the window
	 * between them open.  Pair with -DDC_LRU_MCAS_RETAIN_READD to reproduce
	 * the defect the seal closes. */
	lru_del(dc, d);
	return 1;
#else
	for (;;) {
		struct urcu_txn_deque *q;
		int r;

		rcu_read_lock();
		q = urcu_txn_deque_queued(&d->d_lru.dnode);
		r = q ? lru_dq_remove_seal(dc, q, &d->d_lru.dnode)
		      : lru_dq_seal(dc, &d->d_lru.dnode);
		rcu_read_unlock();
		if (r == 0) {
			if (q)
				uatomic_dec(&q->count);
			return 1;
		}
		if (r == -ESTALE)		/* a peer sealed it already */
			return 1;
		/* -ENOENT: removed under us.  -EEXIST: pushed under us.
		 * -ENOMEM: transient.  Re-derive and take the other branch. */
	}
#endif
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

/*
 * Add at the TAIL (newest).  Called with no lock held.
 *
 * The liveness test is re-done UNDER THE SHARD LOCK, not just at entry, and it
 * is still only a narrowing: the shard lock does not exclude the unhash (that
 * is the bucket lock / the index transaction), so a dentry can be unhashed
 * between the test and the link.  Mainline gets atomicity here from d_lock,
 * which serialises retain_dentry's d_lru_add against __dentry_kill's unhash +
 * d_lru_del; this arm has no per-dentry lock and so cannot close it this way.
 * Recorded rather than papered over -- -DDC_LRU_FREE_ASSERT measures what is
 * left.
 */
static void lru_add(struct dcache *dc, struct dentry *d)
{
	unsigned int idx = lru_shard_index(dc);
	struct dc_lru_shard *sh = &dc->lru[idx];

	/*
	 * The liveness test is an OPTIMISATION, not the guard.  It reads a slot
	 * written under a bucket lock this path does not take, so it can be
	 * stale in the one direction that matters; what actually makes the
	 * enqueue safe is the claim below.  Skipping the work for a dentry
	 * already known dead is still worth a load.
	 */
	if (!lru_alive_hint(d))
		return;			/* unhashed: never re-arm a dying dentry */
	lru_lock(sh);
	/*
	 * CLAIM THE WORD, UNDER THIS SHARD'S LOCK.  Both halves are load-bearing:
	 *
	 *   the cmpxchg is what a killer's OFF -> DEAD seal races against, so
	 *   exactly one of us wins and an enqueue can never follow a free;
	 *
	 *   holding the lock ACROSS the claim and the splice is what stops the
	 *   killer from acting on a claimed-but-not-yet-linked node -- it reads
	 *   ON(idx), comes to this same lock, and by the time it gets in the
	 *   links exist.  (Claiming outside the lock is the "claimed but not
	 *   linked" defect the MCAS arm's history records, and it corrupted the
	 *   list by splicing a node whose prev/next were still NULL.)
	 *
	 * A failed claim means DEAD, or already ON/SHRINK somewhere: in every
	 * case there is nothing to do, which is also what makes lru_listed()
	 * safe to consult as a mere hint.
	 */
	if (uatomic_cmpxchg(&d->d_lru.shard, DC_LRU_OFF, DC_LRU_ON(idx)) !=
	    DC_LRU_OFF) {
		lru_unlock(sh);
		return;
	}
	d->d_lru.prev = sh->tail;
	d->d_lru.next = NULL;
	if (sh->tail)
		sh->tail->d_lru.next = d;
	else
		sh->head = d;
	sh->tail = d;
	sh->count++;
	lru_unlock(sh);
}

/*
 * Splice out, wherever it sits, and leave the word at @newst.  Caller holds @sh.
 *
 * ⚠ @newst is a PARAMETER rather than a hardcoded DC_LRU_OFF because a killer
 * must not let the word pass through OFF on its way to DEAD: an adder holding
 * some OTHER shard's lock would see the transient, win the claim, and link a
 * dentry already queued for reclaim.  DC_LRU_OFF is right only when @d is
 * genuinely alive and re-armable (the shrinker's LRU_REMOVED case).
 */
static void lru_unlink_locked_to(struct dc_lru_shard *sh, struct dentry *d,
				 unsigned int newst)
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
	uatomic_store(&d->d_lru.shard, newst, CMM_RELAXED);
	sh->count--;
}

/* @d stays alive and re-armable: the shrinker's LRU_REMOVED, and lru_rotate. */
static void lru_unlink_locked(struct dc_lru_shard *sh, struct dentry *d)
{
	lru_unlink_locked_to(sh, d, DC_LRU_OFF);
}

/*
 * Take @d off the LRU, and answer whether the caller may still free it.
 *
 * @freeing says whether the caller was ABOUT TO call_rcu @d.  It has to be told,
 * because the answer is a TRANSFER of that obligation, not advice: when the
 * shrinker holds @d, this marks the debt and returns 0, and the shrinker frees
 * it instead.  That is mainline __dentry_kill's
 *
 *	if (dentry->d_flags & DCACHE_SHRINK_LIST) { ... can_free = false; }
 *
 * with shrink_dentry_list finishing the kill.  A caller that was not going to
 * free @d passes 0 and always gets 1 back -- there is nothing to hand over, and
 * the shrinker simply keeps its victim.
 *
 * ⛔ THE TEMPTING WRONG ANSWER is to unlink it here anyway.  A shrink-held
 * victim is not ON the list, so there is nothing to splice; forcing it OFF only
 * clears the ownership that stops the shrinker's put-back from resurrecting a
 * dentry already queued for reclaim.  Disowning IS the bug.
 */
static int lru_del_can_free(struct dcache *dc, struct dentry *d, int freeing)
{
	for (;;) {
		unsigned int st = uatomic_load(&d->d_lru.shard, CMM_RELAXED);
		struct dc_lru_shard *sh;

		if (st == DC_LRU_DEAD)
			return 1;		/* already sealed by a peer */
		if (st == DC_LRU_OFF) {
			/*
			 * ⭐ SEAL IT.  Returning here -- which is what this did
			 * before -- leaves nothing to stop a concurrent
			 * lru_retain from claiming @d and enqueueing it on ITS
			 * OWN shard AFTER we call_rcu.  There is no shard lock
			 * to take on this branch (no shard owns @d), so the
			 * word is the only place the exclusion can live.
			 * Losing this cmpxchg means an adder claimed first --
			 * re-read and take it off through the shard it named.
			 */
			if (uatomic_cmpxchg(&d->d_lru.shard, DC_LRU_OFF,
					    DC_LRU_DEAD) == DC_LRU_OFF)
				return 1;
			continue;
		}
		sh = &dc->lru[DC_LRU_SHARD_OF(st)];
		lru_lock(sh);
		/*
		 * ⚠ RE-DERIVE, do not merely re-test.  lru_retain enqueues on the
		 * CALLER'S shard, so a dentry that left this shard can come back
		 * on a DIFFERENT one while the lock was down.  The old
		 * `>= DC_LRU_ON(0)` re-check passed in that case and spliced the
		 * dentry out of the wrong shard's list.  Compare the WHOLE word,
		 * which also catches a LINKED -> SHRINK transition on the same
		 * shard -- exactly the race this function exists to resolve.
		 */
		if (uatomic_load(&d->d_lru.shard, CMM_RELAXED) != st) {
			lru_unlock(sh);
			continue;
		}
		if (DC_LRU_IS_SHRINK(st)) {
			if (!freeing) {
				lru_unlock(sh);
				return 1;	/* nothing to hand over */
			}
			uatomic_store(&d->d_lru.shard, st | DC_LRU_KILL_BIT,
				      CMM_RELAXED);
			lru_unlock(sh);
			return 0;		/* HANDED OFF: do not free */
		}
		/* ON(i) -> DEAD in one store, never through OFF: see
		 * lru_unlink_locked_to(). */
		lru_unlink_locked_to(sh, d, freeing ? DC_LRU_DEAD : DC_LRU_OFF);
		lru_unlock(sh);
		return 1;
	}
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
 *
 * For a caller that is about to free @d, use lru_del_can_free() instead -- this
 * spelling cannot express the handoff and would silently drop it.
 */
static void lru_del(struct dcache *dc, struct dentry *d)
{
	(void) lru_del_can_free(dc, d, 0);
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

/*
 * PROBE (-DDC_LRU_FREE_ASSERT): is @d still owned by the LRU at the instant its
 * storage is released?
 *
 * If it is, free() hands the storage back and the next dentry to land on it is
 * memset to zero, while the neighbours that pointed at the old node STILL NAME
 * IT.  On the MCAS arm that surfaces downstream as "a live ring reaches a node
 * with owner == NULL"; on the lock arm it corrupts the shard's head/tail chain
 * the same way.  The free callback is the only place that can decide it --
 * everywhere else the answer is a race.
 *
 * ⚠⚠ IT LIVES HERE, ONCE, BECAUSE THE COPY IN ONE ENGINE WAS A VACUOUS ZERO.
 * This started life inline in dcache_bucketlock.c's dentry_free_cb and was
 * never added to dcache_txn.c's, whose callback is a bare free().  Two rows of
 * a handoff table -- "txn MCAS deque 0/5", "txn LOCK 0/5, 0 hits" -- were
 * therefore reporting that a probe which CANNOT FIRE did not fire.  Both
 * engines now call this one definition, so the divergence is not expressible.
 * (The same fn==NULL / dead-probe trap has now cost this project four wrong
 * negatives; the rule is: verify a probe is live before trusting its zero.)
 *
 * ⚠ BOTH ARMS, deliberately.  Asking it only of the MCAS arm invites the
 * conclusion that the mechanism is at fault, when the question is whether the
 * PORT lets a dying dentry be re-added -- a property of retain_dentry's policy
 * and of the shrinker's isolate, not of the structure underneath.
 */
static void lru_assert_not_queued(struct dentry *d)
{
#ifdef DC_LRU_FREE_ASSERT
	int reachable = -1;
#ifdef DC_LRU_MCAS
	/*
	 * ⚠ queued(), NOT owner().  A SEALED node has a non-NULL owner and is on
	 * no deque -- which is the state a correctly-killed dentry is SUPPOSED
	 * to be freed in, so asking owner() here makes this probe fire on every
	 * single free.  It did: 10/10 immediately after the seal landed, which
	 * looked exactly like the seal having failed rather than the probe
	 * having asked the wrong question.
	 */
	struct urcu_txn_deque *q = urcu_txn_deque_queued(&d->d_lru.dnode);
	void *a = uatomic_load((void **) &d->d_lru.dnode.next, CMM_RELAXED);
	void *b = uatomic_load((void **) &d->d_lru.dnode.prev, CMM_RELAXED);
	int queued = q != NULL;
#else
	unsigned int st = uatomic_load(&d->d_lru.shard, CMM_RELAXED);
	void *q = (void *) (uintptr_t) st;
	void *a = uatomic_load(&d->d_lru.next, CMM_RELAXED);
	void *b = uatomic_load(&d->d_lru.prev, CMM_RELAXED);
	int queued = DC_LRU_IS_OWNED(st);
#endif

	if (caa_likely(!queued))
		return;
#ifndef DC_LRU_MCAS
	/*
	 * SECOND, INDEPENDENT WITNESS on the lock arm: the state word says
	 * "owned", but the word is exactly the thing that could be stale.  Walk
	 * the shard it names, under that shard's lock, and report whether the
	 * chain really reaches @d.  One probe is not a finding, and this claim
	 * -- that the LOCK arm, the honest A/B control, frees owned dentries --
	 * is too consequential to rest on a word.
	 *
	 * A SHRINK-held victim is legitimately unreachable (it is off the list
	 * by design), so chain-reachable=0 with the shrink bit set means the
	 * handoff was missed, not that the list is corrupt.  The state word is
	 * printed raw so the two cases are distinguishable.
	 */
	if (dc_lru_validate_dc) {
		struct dc_lru_shard *sh =
			&dc_lru_validate_dc->lru[DC_LRU_SHARD_OF(st)];
		struct dentry *w;
		unsigned long hop = 0;

		lru_lock(sh);
		reachable = 0;
		for (w = sh->head; w && hop++ < 100000; w = w->d_lru.next) {
			if (w == d) { reachable = 1; break; }
		}
		lru_unlock(sh);
	}
#endif
	fprintf(stderr,
		"FREE-WHILE-QUEUED d=%p owner=%p next=%p prev=%p "
		"chain-reachable=%d\n",
		(void *) d, (void *) q, a, b, reachable);
	abort();
#else
	(void) d;
#endif	/* DC_LRU_FREE_ASSERT */
}

#else	/* DC_NO_LRU: the A/B control -- no LRU field, no rseq, no shrinker */
static inline void lru_retain(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline void lru_add(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline void lru_del(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline int lru_del_can_free(struct dcache *dc, struct dentry *d, int f)
{ (void) dc; (void) d; (void) f; return 1; }
static inline void lru_assert_not_queued(struct dentry *d) { (void) d; }
unsigned long dc_lru_count(struct dcache *dc) { (void) dc; return 0; }
long dc_shrink(struct dcache *dc, long nr) { (void) dc; (void) nr; return 0; }
const int dc_lru_inuse_is_removed = 1;
#endif	/* DC_NO_LRU */
#endif	/* DCACHE_LRU_TYPES */
