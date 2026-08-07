// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_txn.c -- lock-free urcu-txn (rcu-mcas) userspace dentry cache.
 *
 * Implements the design in rename-shell-transition.md: inline names (kernel
 * d_iname locality), NO d_seq, NO rename_lock, address-stable dentries, and
 * renames that move a dentry between buckets via a transient "shell" so a live
 * node never has to del+insert its own hlist link (which would self-conflict).
 *
 * STAGING (see the README): this pass builds the full data-structure mechanism,
 * passes the single-threaded harness, closes the walk-CAUSALITY race (a
 * concurrent walker misdirected by a mid-walk rename) with the global rename
 * generation counter (see dc_lookup / stack_shell and repro_dcache.c), and
 * lands the ASYNC, per-node call_rcu fold: a rename now stacks a shell in ONE
 * MCAS commit (both indexes + gen bump + demote) and defers compression to a
 * fold worker a grace period later, which either transfers the identity one hop
 * down (still top) or splices the node out (demoted to a middle relay by a
 * racing re-rename) -- both over a transacted, doubly linked chain, so
 * concurrent folds stay consistent.  The cross-dir loop check is now lock-free
 * too: d_parent is transacted, and a cross-parent rename folds the whole
 * new_parent->root ancestry walk into its commit's validate set via
 * urcu_txn_load_validate() (reject -EINVAL if the victim appears), so two moves
 * that would jointly form a cycle cannot both commit (see stack_shell).  Unlink
 * works mid-fold too: it removes the current named top from both indexes without
 * demoting it, which the pending fold reads as an unlink and RECLAIMs the whole
 * orphaned chain (see fold()/dc_unlink); and the atomic exchange composes TWO
 * shell stacks -- both index del/insert pairs, both loop checks + reparents -- in
 * ONE commit (dc_rename_exchange).  Chain DEPTH is nobody's fast-path concern:
 * every access resolves the content host in ONE hop through the write-once
 * d_host skip pointer (host_of_rcu), so readers, readdir, walk_rec, the folds and
 * the writers are all O(1) in chain depth and NOTHING traverses a chain.  Depth
 * therefore costs memory, not time, and the async fold -- which call_rcu already
 * batches per grace period -- drains it at the rate renames create it (steady
 * state ~ churn x GP latency).  A synchronous fold-ahead relief valve used to cap
 * depth for the GP-stalled case; it is RETIRED (see the git history): reaching
 * its trigger cost the writer an O(chain) walk INSIDE its read-side section,
 * which stopped it quiescing, which stalled the very grace periods the fold needs
 * -- the valve's own trigger drove the starvation it existed to relieve, a
 * bistable ~60x collapse.  A GP stall now degrades to bounded-rate memory growth,
 * which is the honest consequence: it stops ALL reclaim process-wide, and is a
 * quiescing bug to fix at its source (rcu_thread_offline() while blocking).
 * The reader (incl. its rename-generation bracket), the hash + child-hlist
 * indexes, the shell-vehicle move, and the MCAS stack/fold commits are real,
 * final code.  Validation of the concurrent fold is by stress test under ASan /
 * TSAN, not the deterministic repro (which the async fold makes latent).
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#ifdef DC_TXN_STATS
/* Route liburcu's install-CAS failure hook into the per-site counters.  Must be
 * defined BEFORE <urcu/rcu-txn*.h> is included, per that header's contract. */
struct dc_ts_fwd;
static inline void dc_ts_cas_fail(unsigned int idx, void *slot, void *old,
				  void *seen);
#define URCU_TXN_CAS_FAIL(idx, slot, old, seen) \
	dc_ts_cas_fail((idx), (void *) (slot), (void *) (old), (void *) (seen))
static inline void dc_ts_poison_set(void *slot, void *want, void *got);
#define URCU_TXN_POISON(slot, want, got) \
	dc_ts_poison_set((void *) (slot), (void *) (want), (void *) (got))
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu-qsbr.h>			/* generic rcu_* names => QSBR flavor */
#include <urcu-call-rcu.h>
#ifndef DC_NO_LRU
#include <rseq/rseq.h>			/* phase 3: NUMA node id for LRU sharding */
#ifdef DC_LRU_MCAS
#include <urcu/rcu-txn-deque.h>		/* phase 3: the lock-free LRU arm */
#endif
#endif
#include <urcu/rcu-txn.h>		/* AFTER the RCU flavor */
#include <urcu/rcu-txn-hlist.h>

#include "dcache.h"
#include "dcache_txn_stats.h"

#ifdef DC_TEST_HOOKS
/*
 * Test-only rendezvous hook, fired after each path component is resolved in
 * dc_lookup (0-based depth index of the just-latched node).  A concurrency
 * repro installs it to pause a walker mid-descent -- while it holds the RCU
 * read-side lock and a latched interior dentry -- so a writer can interpose a
 * rename deterministically.  NULL and never compiled into normal builds.
 */
void (*dc_test_walk_hook)(int depth);
/*
 * Test-only rendezvous fired in dc_rename AFTER the shell is stacked (old top
 * demoted, out of every index) and BEFORE its fold is queued.  A repro pauses a
 * writer here to interpose a concurrent re-rename of the same entry and exercise
 * the fold's top-vs-middle-relay branch.  NULL and never compiled into normal
 * builds.
 */
void (*dc_test_fold_hook)(void);
/*
 * Test-only rendezvous fired INSIDE the fold's TRANSFER, between the read of the
 * host's d_iparent and the cmpxchg that writes it back.  That window is the one
 * a concurrent d_delete / d_instantiate can be lost in if the write-back is not
 * atomic, so a repro pauses the fold here and performs the state change from
 * another thread.  NULL and never compiled into normal builds.
 */
void (*dc_test_transfer_hook)(void);
/*
 * Test-only rendezvous for the two halves of the negative-directory guard pair
 * (dc_set_negative_txn).  dc_add fires ADD after reading the parent's state and
 * before its commit; d_delete fires DEL after finding the child list empty and
 * before its commit.  A repro parks in one and runs the other, which is the only
 * way to exercise the guards -- single-threaded they never fire, because each
 * side's own up-front check already answers.
 */
void (*dc_test_add_hook)(void);
void (*dc_test_del_hook)(void);
#define DC_TEST_TRANSFER_HOOK()	do {					\
		if (dc_test_transfer_hook)				\
			dc_test_transfer_hook();			\
	} while (0)
#define DC_TEST_ADD_HOOK()	do {					\
		if (dc_test_add_hook)					\
			dc_test_add_hook();				\
	} while (0)
#define DC_TEST_DEL_HOOK()	do {					\
		if (dc_test_del_hook)					\
			dc_test_del_hook();				\
	} while (0)
#else
#define DC_TEST_TRANSFER_HOOK()	do { } while (0)
#define DC_TEST_ADD_HOOK()	do { } while (0)
#define DC_TEST_DEL_HOOK()	do { } while (0)
#endif

/*
 * The 1-CL split hot-path layout (simplification-s4.md §5) is the DEFAULT: the
 * reader-hot set -- inline identity, the per-node walk gen (d_seq), and the
 * still-indexed mark (d_hash.next) -- all fit in CL0, so BOTH the global and the
 * per-node reader touch one cacheline.  Identity is the host ADDRESS (a real
 * dentry has no logical id); the cold d_id is a benchmark artifact read only by
 * the census and by -DDC_SPLIT_KEEPID (which returns d_id so a harness's
 * id==gid / torn-read checks keep working -- same layout, one extra cold read).
 * Opt out of the layout with -DDC_NO_HOT1CL_SPLIT (legacy 3-CL) or -DDC_HOT1CL
 * (the fair-weather pack that leaves d_hash cold).
 */
/*
 * DC_MARK_GEN (rename-shell-transition.md): carry walk causality on the hlist
 * DELETION MARK -- d_hash.next bit 1, which every operation that changes the
 * named top already sets -- instead of the d_seq counter.  Needs the d_iparent
 * tag encoding, so it implies the SPLIT layout.  Retiring d_seq hands its 8
 * bytes to the name (DC_NAME_MAX 40).
 */
/*
 * ⭐ THE PARENT SEAL IS ON BY DEFAULT.  It closes the stale-d_parent
 * use-after-free -- an eviction freeing a directory that an add is
 * concurrently publishing a child under -- by having the freeing side SEAL the
 * victim's child head in its own commit, so the add's EXISTING write to that
 * slot is the exclusion.  See lru_evict_settled().
 *
 * It is on where DC_TXN_PARENT_GUARD (the older, guard-based pair) is off,
 * because the seal costs nothing measurable and the guard cost 40 points of
 * liveness:
 *
 *   --evict bursty, 48w/48r, 20 trials, runs not finishing in 120s
 *       unprotected 1/20 · GUARD 9/20 · SEAL 0/20
 *   -DDC_TXN_PARENT_DELAY_US=5000 reproducer, 6 trials/arm, equal work
 *       unprotected UAF 6/6 runs (12 occurrences) · SEAL 0/6 (0)
 *
 * -DDC_TXN_NO_PARENT_SEAL is the mutation arm: it must fail the reproducer.
 */
#ifndef DC_TXN_NO_PARENT_SEAL
#define DC_TXN_PARENT_SEAL 1
#endif

/*
 * TXN BRACKET TRACING, scaffolding behind -DDC_ENABLE_TRACING (see dcache_tp.h).
 * DC_TXN_TP(handle, site, act, st) samples the handle's own `in_fallback` --
 * "we currently hold the lane" -- so an `end` that holds it on a path which
 * then RETURNS is visible in the event itself.
 */
#ifdef DC_ENABLE_TRACING
#include "dcache_tp.h"
enum { DC_TA_BEGIN = 0, DC_TA_END, DC_TA_ABANDON, DC_TA_CONFLICT, DC_TA_COMMIT };
#define DC_TXN_TP(h, site, act, st) \
	lttng_ust_tracepoint(dc, txn, (h), (site), (act), (st), (h)->in_fallback)

/*
 * ⭐ THE SITE IS __LINE__, not an enum, and the wrappers are defined BEFORE the
 * macros that redirect to them -- so the bodies below call the real functions
 * and there is no recursion.  That gets exact per-call-site attribution across
 * ~30 bracket sites without touching any of them.
 *
 * begin is traced AFTER it returns: a thread PARKED inside urcu_txn_begin()
 * emits nothing, so its last event stays the previous `end` -- which is exactly
 * the signal wanted (who ended last, and did they still hold the lane).
 */
/*
 * ⭐ @st carries `retrying` for END, and that pair is the whole diagnosis:
 * end() releases the lane only when !retrying, so an END with in_fb=1 AND
 * retrying=1 is a bracket walking away from a held lane.  __LINE__ then names
 * the exact bail that leaked it.
 */
static inline void dc_tp_end(struct urcu_txn *t, int line)
{ DC_TXN_TP(t, line, DC_TA_END, t->retrying); urcu_txn_end(t); }
static inline void dc_tp_abandon(struct urcu_txn *t, int line)
{ DC_TXN_TP(t, line, DC_TA_ABANDON, 0); urcu_txn_abandon(t); }
static inline void dc_tp_conflict(struct urcu_txn *t, int line)
{ DC_TXN_TP(t, line, DC_TA_CONFLICT, 0); urcu_txn_conflict(t); }
static inline void dc_tp_begin(struct urcu_txn *t, int line)
{ urcu_txn_begin(t); DC_TXN_TP(t, line, DC_TA_BEGIN, 0); }
static inline enum urcu_txn_status dc_tp_commit(struct urcu_txn *t, int line)
{
	enum urcu_txn_status s = urcu_txn_commit(t);

	DC_TXN_TP(t, line, DC_TA_COMMIT, (int) s);
	return s;
}
/*
 * ⭐ THE LANE ITSELF, via the weak hook in the (gitignored) urcu build tree's
 * rcu-txn.h under -DURCU_TXN_LANE_TRACE.  Bracket events alone CANNOT answer
 * "does an owner exist": `in_fallback` is set only AFTER the lock is taken, so
 * "no thread ever showed in_fallback=1" is equally consistent with "nobody ever
 * acquired" and with "an owner acquired before the ring window and never let
 * go".  These three events -- attempt / held / release, in the acquire path
 * itself -- name the last successful acquirer and separate the two.
 *
 * Rate is escalation-only, so this cannot flood the ring.
 */
enum { DC_TA_LANE_ATTEMPT = 5, DC_TA_LANE_HELD, DC_TA_LANE_RELEASE };
void urcu_txn__lane_trace(int what, void *txn)
{
	lttng_ust_tracepoint(dc, txn, txn, 0, DC_TA_LANE_ATTEMPT + what, 0,
			     ((struct urcu_txn *) txn)->in_fallback);
}

#define urcu_txn_end(t)		dc_tp_end((t), __LINE__)
#define urcu_txn_abandon(t)	dc_tp_abandon((t), __LINE__)
#define urcu_txn_conflict(t)	dc_tp_conflict((t), __LINE__)
#define urcu_txn_begin(t)	dc_tp_begin((t), __LINE__)
#define urcu_txn_commit(t)	dc_tp_commit((t), __LINE__)
#else
#define DC_TXN_TP(h, site, act, st)			do { } while (0)
#endif

/*
 * ⭐⭐ RELEASE THE ESCALATION LANE ON EVERY PATH THAT DOES NOT RE-ATTEMPT.
 *
 * urcu_txn_end() keeps an ESCALATED handle's lane so a re-attempt need not go
 * to the back of the FIFO -- so a bracket that ends WITHOUT re-attempting owes
 * urcu_txn_abandon() first, or the domain's lane is held for ever and every
 * other writer parks behind it.  fc663f5a fixed that in the liburcu convenience
 * brackets; this engine's own hand-rolled retry loops had the same defect on
 * three paths, and it is what wedges --evict continuous.
 *
 * ⚠ urcu_txn_conflict() IS NOT A SUBSTITUTE: it deliberately KEEPS the turn,
 * which is correct only when the same handle re-attempts.  Two of the three
 * sites called it and then returned.
 *
 * -DDC_NO_LANE_ABANDON is the mutation arm: it must wedge.
 */
#ifdef DC_NO_LANE_ABANDON
#define DC_LANE_GIVE_UP(txn)	do { } while (0)
#else
#define DC_LANE_GIVE_UP(txn)	urcu_txn_abandon(txn)
#endif

/*
 * ⭐⭐ ...AND GIVE BACK A LANE KEPT FOR A RE-ATTEMPT THAT NEVER COMES.
 *
 * The abandon-before-end rule above covers the bails that END the bracket
 * themselves.  It does NOT cover the one that actually wedges --evict
 * continuous, because there the conflict()+end()+continue is CORRECT -- the
 * loop does re-attempt -- and the lane is lost one iteration LATER:
 *
 *	for (;;) {
 *		top = find_top_rcu(...);
 *		if (!top) { ret = -ENOENT; goto out; }	<-- lane still held
 *		...
 *		if (p) { urcu_txn_conflict(&txn);	 <-- keeps the turn
 *			 urcu_txn_end(&txn);		 <-- so end() KEEPS it
 *			 continue; }			 <-- re-decides at the TOP
 *	}
 *
 * The retry loops re-decide at their HEAD, and those head checks exit the
 * operation.  So the iteration after a conflict() may never begin another
 * bracket, and `out:` returns with the fair mutex locked: every later begin()
 * in the domain parks for ever (rcu-txn.h, urcu_txn_end()).  A per-bail fix
 * cannot reach this -- the leaking statement is a `goto out` that knows nothing
 * about transactions -- so the release belongs at the operation's exit.
 *
 * begin() CANNOT re-acquire here: urcu_txn__want_fallback() requires
 * !in_fallback, which is exactly the state being unwound.  So this is a bounded
 * begin/abandon/end that only forfeits the turn, and it is a no-op on the
 * overwhelmingly common path where the handle never escalated.
 *
 * -DDC_NO_LANE_GIVEBACK is the mutation arm: it must wedge.
 */
static inline void dc_lane_giveback(struct urcu_txn *txn)
{
#ifndef DC_NO_LANE_GIVEBACK
	if (!txn->in_fallback)
		return;
	urcu_txn_begin(txn);
	urcu_txn_abandon(txn);		/* forfeit the turn -> end() releases */
	urcu_txn_end(txn);
#else
	(void) txn;
#endif
}

#ifdef DC_MARK_GEN
#define DC_HOT1CL_SPLIT 1
/*
 * Both no-counter arms transact d_iparent: the fold's identity handover is then
 * PUBLISHED by its commit instead of stored in place, which is what removes the
 * d0e7955 hazard for that field.
 */
#define DC_IPARENT_TXN 1
#endif

#if !defined(DC_NO_HOT1CL_SPLIT) && !defined(DC_HOT1CL) && !defined(DC_HOT1CL_SPLIT)
#define DC_HOT1CL_SPLIT 1
#endif

#ifdef DC_HOT1CL_SPLIT
#define DC_HOT1CL 1		/* SPLIT reuses all the HOT1CL tag machinery */
#endif

struct dentry {
	/* reader hot line: inline identity (+ payload under DC_HOT1CL) */
#ifndef DC_HOT1CL
	struct urcu_txn_hlist_node d_hash;	/* transacted; live only while named top */
#endif
	struct dentry *d_iparent;		/* inline identity: parent addr (match);
						 * DC_HOT1CL: low bits carry host/shell +
						 * pos/neg tags (see iparent_of()) */
	struct qstr    d_iname;			/* inline identity: name bytes (match) */
	DC_DENTRY_NAME_GUARD			/* -DDC_DEBUG_NAME_GUARD only */
	DC_DENTRY_NAME_PAD			/* -DDC_NAME_PAD=N: same-size control */
#if defined(DC_HOT1CL_SPLIT)
	/*
	 * SPLIT 1-CL hot line, 64 B all in CL0.  d_hash straddles so next@56 stays
	 * in CL0 (collision walks hot) while pprev@64 spills cold.  The d_id/d_host
	 * union stays COLD: the reader returns the host ADDRESS as identity --
	 * d_id is a benchmark artifact (a real dentry's identity is its address),
	 * read cold only by the census and by -DDC_SPLIT_KEEPID validation builds.
	 *
	 *   default:         d_iparent(8) + d_iname(40) + d_seq(8) + d_hash.next(8)
	 *   DC_MARK_GEN:     d_iparent(8) + d_iname(48) +           d_hash.next(8)
	 *
	 * The per-node walk-causality gen sits ON the hot line (sampled every hop).
	 * In the global build d_seq is unused but kept for a uniform offset.  Under
	 * DC_MARK_GEN there is no per-node gen at all -- causality rides the
	 * d_hash.next deletion mark -- so the 8 bytes go to the name instead
	 * (DC_NAME_MAX 32 -> 40; see dcache.h).
	 */
#ifndef DC_IPARENT_TXN
	void *d_seq;				/* @48: per-node gen, on CL0 */
#endif
	struct urcu_txn_hlist_node d_hash;	/* straddle: next@56 CL0, pprev@64 cold */
#elif defined(DC_HOT1CL)
	/* payload joins the hot line: d_iparent(8)+d_iname(40)+union(8) = 56 B */
	union {
		uint64_t       d_id;
		struct dentry *d_host;
	};
#endif

	/*
	 * Transition chain, doubly linked and TRANSACTED (the splice MCASes both
	 * links atomically so concurrent folds stay consistent).  d_fwd is read by
	 * readers following a chain -- via urcu_txn_read(), since it can briefly
	 * hold a commit descriptor; d_back is read only by fold workers.  Both NULL
	 * in steady state (settled content host = its own top, no chain).
	 */
	struct dentry *d_fwd;			/* down toward content host; NULL at host */
	struct dentry *d_back;			/* up toward named top;     NULL at top  */

	/* writer-side bookkeeping */
	struct dentry *d_parent;		/* logical parent; TRANSACTED (DC_PARENT_TAG) */
	/*
	 * MOVE-IN-PROGRESS flag (cross-dir cycle prevention).  A cross-parent move
	 * sets this on its host before validating the ancestry, and clears it after
	 * the reparent commits (or aborts).  The loop check walks new_parent -> root
	 * with PLAIN loads (not the proxy-installing load_validate that pinned the
	 * shared spine) and aborts (-EAGAIN) if any ancestor carries this flag: a
	 * Dekker set-before-check, so two moves that would jointly form a cycle
	 * cannot both proceed (one sees the other's flag).  Cold word, its own line-
	 * neighbour of d_parent so a walk hop reads both at once; only written when
	 * THIS node is itself the host of a move, so spine nodes keep it 0 forever
	 * (their reads stay S-state -- no cross-CCD ping-pong). */
	unsigned long d_moving;
	struct dcache *d_dc;			/* owner, so a call_rcu fold reaches the domain */

	/*
	 * Child index (readdir fast path + -ENOTEMPTY): a per-directory
	 * rcu-txn-hlist, mutated by MCAS and traversed under RCU.  d_child_head
	 * heads THIS node's children; d_sib links this node into its parent's
	 * child-hlist.  Distinct from d_hash (the name-bucket link).
	 */
	struct urcu_txn_hlist_head d_child_head;
	struct urcu_txn_hlist_node d_sib;

	/*
	 * Identity id (HOST) OR skip pointer to the content host (SHELL), overlaid:
	 * a host reads it as d_id, a shell as d_host.  Which is live is fixed by the
	 * STABLE per-node property d_fwd==NULL (host) vs !=NULL (shell) -- a node is
	 * born a host or a shell and never crosses over, so each node only ever
	 * touches ONE member (no type-punning).  A reader resolves the host in O(1)
	 * with host_of_rcu().  The shell's d_host is WRITE-ONCE (the tail is fold-
	 * invariant), so it's a plain rcu_dereference.  This reuses the old
	 * "cosmetic" shell d_id slot -- shells never needed their own id (readers use
	 * the host's) and hosts never need a self skip pointer -- so the struct does
	 * not grow and the identity slot keeps its original offset.  (Under
	 * DC_HOT1CL the union is hoisted onto the hot line above instead.)
	 */
#if !defined(DC_HOT1CL) || defined(DC_HOT1CL_SPLIT)
	union {
		uint64_t       d_id;	/* host: stable identity (SPLIT: cold) */
		struct dentry *d_host;	/* shell: skip pointer to the tail host */
	};
#endif
	int d_inode;
	/*
	 * File vs directory type -- COLD, writer-only.  Set once at creation on the
	 * content host (immutable; a rename never changes it).  The reader fast path
	 * never touches it.  Two writer uses: dc_add rejects a child under a file
	 * (ENOTDIR), which enforces the invariant a FILE HAS NO CHILDREN; and a
	 * rename skips the walk-causality bump when the moved host is a file (a file
	 * is never an interior waypoint, so it cannot misdirect -- see the "Files are
	 * exempt" note in rename-shell-transition.md).  This makes the dentry cache
	 * TYPE-AWARE, which the kernel is NOT: the kernel's per-dentry d_seq bump is
	 * cheap enough to do unconditionally, so it needs no such distinction.  The
	 * cost buys back only the GLOBAL arm (whole-tree bump); per-node localizes it
	 * and the mark arm's signal is the structural del, so both ignore d_isdir.
	 */
	unsigned char d_isdir;

	/*
	 * Per-entry walk-causality generation (the DC_PER_NODE_GEN reader).  Lives
	 * on the address-stable content host -- durable across renames AND folds
	 * (folds free shells from the TOP down; the tail host never moves, see
	 * fold()) -- and bumped, transacted, in every commit that moves or removes
	 * THIS entry.  A reader samples it per hop on the way down and revalidates
	 * every latched host on the way up, so a rename only invalidates walks that
	 * actually pass through this entry -- the localization the single global
	 * rename_gen cannot give.  Kept EVEN (stepped by 2) so bit 0 (the proxy tag)
	 * stays clear.  Unused (stays 0) in the default global-rename_gen build.
	 *
	 * SPLIT hoists this onto CL0 (above) so the per-node reader stays 1-CL; the
	 * cold copy here is for every other layout.
	 */
#if !defined(DC_HOT1CL_SPLIT)
	void *d_seq;
#endif

#if defined(DC_HOT1CL) && !defined(DC_HOT1CL_SPLIT)
	/* cold under DC_HOT1CL: reader touches d_hash.next only on collision
	 * walks (the global build reads no mark); pprev is writer-only. */
	struct urcu_txn_hlist_node d_hash;
#endif
	struct rcu_head d_rcu;
#ifdef DC_LIFECYCLE_STATE
	/*
	 * Debug-gated lifecycle state (dcache_state.h).  FREE: it lands in the
	 * 24-byte hole between d_rcu@152 and the cacheline-aligned d_lru@192, so
	 * sizeof(struct dentry) stays 256 and the gated build has the SAME layout
	 * as the shipped one -- which is the point, since a debug build that
	 * moved the hot line would stop reproducing the races it is here to
	 * catch.  Cold line; the reader never touches it.
	 */
	unsigned int d_lc;
#endif
#ifndef DC_NO_LRU
	/*
	 * ---- PHASE 3: LRU, ON ITS OWN CACHELINE --------------------------------
	 * Off CL0 because a lookup never touches the LRU, and off every OTHER line
	 * because splicing a node out writes its NEIGHBOURS' links -- dirtying a
	 * line belonging to two arbitrary other dentries on every add/del/rotate.
	 * See dcache_lru.h.
	 */
	struct {
#ifdef DC_LRU_MCAS
		/*
		 * The deque node carries its own membership (`owner`, a pointer
		 * to the shard's deque), so there is NO shard word here: the
		 * word and the links could disagree, the pointer cannot.
		 */
		struct urcu_txn_deque_node dnode;
#else
		struct dentry *prev;
		struct dentry *next;
		unsigned int   shard;
#endif
		unsigned char  referenced;
	} d_lru __attribute__((aligned(64)));
#endif
};

#define sib_dentry(n) caa_container_of((n), struct dentry, d_sib)

#ifdef DC_HOT1CL
/*
 * DC_HOT1CL tag encoding in d_iparent's low bits.  The dentry is 64-byte aligned
 * (posix_memalign, for the 1-CL reader line), so bits 0-5 are free; bit 0 stays
 * reserved for the txn proxy tag, so
 * host/shell and pos/neg ride bits 1 and 2.  Both are stable-or-identity
 * properties (a node never changes kind; pos/neg travels with the identity a fold
 * transfers), so the reader reads them off the already-loaded d_iparent instead
 * of touching d_fwd / d_inode on other cachelines.  Non-HOT1CL builds fall back
 * to the plain field reads.
 */
#define DC_TAG_SHELL	((uintptr_t) 0x2)	/* bit 1: node is a rename shell */
#define DC_TAG_NEG	((uintptr_t) 0x4)	/* bit 2: negative dentry */
#define DC_TAG_MASK	((uintptr_t) 0x7)

/*
 * Both localized arms run the SAME reader shape -- a versioned double-collect
 * that latches a per-entry stamp on the way down and re-reads it on the way up,
 * so a rename only trips walks that actually passed through the moved entry.
 * They differ only in WHICH word is the stamp:
 *
 *   DC_PER_NODE_GEN  host->d_seq, a dedicated counter stepped by 2.
 *   DC_MARK_GEN      the d_hash.next deletion mark, which every operation that
 *                    changes the top already sets.  Free: @56 is on CL0 and the
 *                    bucket walk already loaded it, and the descent's
 *                    top_unhashed_rcu() is already the first collect.
 */
#if defined(DC_PER_NODE_GEN) || defined(DC_MARK_GEN)
#define DC_LOCALIZED_GEN 1
#endif

/*
 * One stamp type for all three localized arms; what it HOLDS differs:
 *   DC_PER_NODE_GEN  the host's generation counter value
 *   DC_MARK_GEN      the latched top itself (re-tested, not re-read)
 */
typedef uintptr_t dc_stamp_t;

#ifdef DC_IPARENT_TXN
/*
 * Engine proxy tag for the TRANSACTED d_iparent slot.  The fold's promote
 * publishes d_iparent inside its commit, so the slot can briefly
 * hold a descriptor and every read must resolve it.  Dentries are 64-byte
 * aligned (posix_memalign), so bit 0 is clear on every live value.
 *
 * Cost of the resolve on the match path: with no txn installed -- the
 * overwhelming common case -- urcu_txn_read() is a tag test and a
 * well-predicted branch on a word already loaded for the compare, the same shape
 * top_unhashed_rcu() already pays on this path.
 */
#define DC_IPARENT_TAG	URCU_TXN_TAG
#endif

static inline uintptr_t iparent_raw(const struct dentry *d)
{
#ifdef DC_IPARENT_TXN
	struct dentry *nc = (struct dentry *) (uintptr_t) d;

	return (uintptr_t) urcu_txn_read((void **) &nc->d_iparent,
					  DC_IPARENT_TAG);
#else
	struct dentry *nc = (struct dentry *) (uintptr_t) d;

	/* RELAXED ATOMIC, not a plain load.  The fold's TRANSFER cmpxchgs this
	 * word (dc_transfer_iparent) while readers sample it for pos/neg, so a
	 * plain load here IS a data race -- TSAN says so, and it is UB even
	 * though x86 emits the identical instruction.  No ordering is claimed:
	 * the walk-causality bracket carries that, not this load. */
	return (uintptr_t) uatomic_load(&nc->d_iparent, CMM_RELAXED);
#endif
}


static inline struct dentry *iparent_of(const struct dentry *d)
{
	return (struct dentry *) (iparent_raw(d) & ~DC_TAG_MASK);
}
static inline int node_is_shell(const struct dentry *d)
{
	return (iparent_raw(d) & DC_TAG_SHELL) != 0;
}
static inline int node_is_positive(const struct dentry *d)
{
	return (iparent_raw(d) & DC_TAG_NEG) == 0;
}
#define DC_IPARENT(d)     iparent_of(d)
#define DC_IS_POSITIVE(d) node_is_positive(d)
#ifdef DC_HOT1CL_SPLIT
#ifdef DC_SPLIT_KEEPID
/* validation build: read the (cold) d_id and return it as the logical id, so a
 * harness's id==gid / torn-read checks keep working.  Same 1-CL hot LAYOUT as the
 * default -- it just pays one extra cold read for the return value -- so it
 * validates the layout while the default (address) build measures it. */
#define DC_FAST_ID(node)  ((node)->d_id)
#else
/* DEFAULT 1-CL identity = the host ADDRESS: a real dentry's identity IS its
 * address (there is no logical id in a kernel dcache -- d_id is a benchmark
 * artifact).  The reader returns it directly, never touching the cold d_id line.
 * Harnesses detect this via the dc_lookup_id_is_address capability and check the
 * seed-time host-address table instead; the census (dc_walk) reads the real
 * cold d_id. */
#define DC_FAST_ID(node)  ((uint64_t) (uintptr_t) (node))
#endif
#else
#define DC_FAST_ID(node)  ((node)->d_id)
#endif
#else
#define DC_IPARENT(d)     ((d)->d_iparent)
#define DC_IS_POSITIVE(d) ((d)->d_inode)
#define DC_FAST_ID(node)  ((node)->d_id)
#endif

#ifdef DC_STRESS_DEBUG
/* Coarse counters for diagnosing fold drain / chain growth (not thread-exact).
 * dc_dbg_fold_retries = total loop attempts (both loops); dc_dbg_fold_aborts =
 * commit-level ABORTs only (a genuine MCAS race, distinct from the deterministic
 * main->reclaim second loop the harness's 25% mid-unlink drives). */
unsigned long dc_dbg_renames, dc_dbg_folds, dc_dbg_fold_retries, dc_dbg_fold_aborts;
# define DC_DBG_FOLD_ATTEMPT() uatomic_inc(&dc_dbg_fold_retries)
# define DC_DBG_FOLD_ABORT()   uatomic_inc(&dc_dbg_fold_aborts)
#else
# define DC_DBG_FOLD_ATTEMPT() ((void) 0)
# define DC_DBG_FOLD_ABORT()   ((void) 0)
#endif

/*
 * fold takes the age-0 MW fast path: measured, fold's commits ABORT only ~0.001%
 * of the time (1-2 in >1M folds, flat across 4..48 writers -- see dc_dbg_fold_aborts).
 * Its frequent extra work is the deterministic main->reclaim second loop an unlink
 * drives, NOT contention, so expect_conflict (skip age-0) was a pure pessimization:
 * it forced the sorted, blocking path for ~99.999% of folds that commit clean first
 * try.  The rare genuine conflict (a concurrent SPLICE of m) is handled by the retry
 * loop.  -DDC_FOLD_EXPECT_CONFLICT restores the old hint for A/B.
 */
#ifdef DC_FOLD_EXPECT_CONFLICT
# define DC_FOLD_CONFLICT_HINT(txn) urcu_txn_expect_conflict(txn)
#else
# define DC_FOLD_CONFLICT_HINT(txn) ((void) (txn))
#endif

/*
 * A rename/exchange skips age-0 ONLY on the GLOBAL arm, whose single shared
 * rename_gen every rename bumps -- a hot slot whose install fail-fasts under
 * concurrent renames, so age-0 is genuinely doomed there.  The per-node arm bumps
 * the moved host's OWN d_seq (uncontended across renames) and the mark arm bumps
 * no gen at all, so both keep the age-0 fast path.  (The child-head del+insert can
 * still alias when top is first, but that is a single coincidence the default RYW
 * handle chains at commit -- not the skiplist-style dense self-abort expect_conflict
 * is for.)
 */
#if !defined(DC_PER_NODE_GEN) && !defined(DC_MARK_GEN)
# define DC_RENAME_CONFLICT_HINT(txn) urcu_txn_expect_conflict(txn)
#else
# define DC_RENAME_CONFLICT_HINT(txn) ((void) (txn))
#endif

#define DCACHE_LRU_TYPES
#include "dcache_lru.h"		/* PHASE 3: shard types + axis arms */
/* Debug-gated lifecycle assertions.  Included HERE, above the reclaim sites
 * that use them; the R5 check reads lru_listed() at its use site. */
#include "dcache_state.h"
#undef DCACHE_LRU_TYPES

struct dcache {
	struct urcu_txn_hlist_head *buckets;
	unsigned long mask;			/* nbuckets - 1 (power of two) */
	struct dentry *root;
	struct urcu_txn_domain domain;

	/*
	 * Walk-causality generation (rename_lock's job, NOT d_seq's -- see
	 * rename-shell-transition.md).  A single global counter bumped inside
	 * every rename's shell-stack MCAS commit; a reader brackets its whole
	 * path walk on it and retries if it moved (a rename touched the tree
	 * mid-walk).  Stored as a transacted void* slot so the bump composes
	 * atomically with the structural edge change; the reader resolves it
	 * with urcu_txn_read().  Kept EVEN (stepped by 2) so bit 0 -- the
	 * engine proxy tag -- is always clear on a plain value.
	 */
	void *rename_gen;
#ifndef DC_NO_LRU
	struct dc_lru_shard *lru;		/* phase 3; see dcache_lru.h */
	unsigned int nlru;
#ifdef DC_LRU_MCAS
	/* SEPARATE from the index domain: the LRU is not part of the namespace
	 * index, and sharing a fair-mutex lane would let an escalation raised by
	 * a rename capture every concurrent LRU commit. */
	struct urcu_txn_domain lru_domain;
#endif
#endif
};

/* Engine proxy tag for the rename_gen slot (bit 0; values stay even). */
#define DC_GEN_TAG	URCU_TXN_TAG

/*
 * Engine proxy tag for the transacted transition chain (d_fwd/d_back).  The
 * splice MCASes both links of a middle relay in one commit, so a concurrent
 * fold sees a consistent chain; readers following d_fwd resolve the slot with
 * urcu_txn_read() (it may briefly hold a commit descriptor).  Node addresses
 * are >= 8-byte aligned, so bit 0 is clear on every live value the slot holds.
 */
#define DC_FWD_TAG	URCU_TXN_TAG

/*
 * Engine proxy tag for the transacted d_parent slot (bit 0; hosts are aligned).
 * Writer-only: read via the txn in the cross-dir loop check and via
 * urcu_txn_read() in the quiescent walk; never on the downward reader path.
 */
#define DC_PARENT_TAG	URCU_TXN_TAG

#define hnode_dentry(n) caa_container_of((n), struct dentry, d_hash)

/* ---- hashing (same mix as the seqlock engine) -------------------------- */

static inline struct urcu_txn_hlist_head *bucket_of(struct dcache *dc,
						    const struct dentry *parent,
						    uint32_t name_hash)
{
	unsigned long h = (unsigned long) name_hash * 0x9e3779b97f4a7c15UL;

	h ^= (unsigned long) (uintptr_t) parent >> 6;
	h *= 0xff51afd7ed558ccdUL;
	return &dc->buckets[(h >> 32) & dc->mask];
}

/* ---- lifecycle --------------------------------------------------------- */

const char *dc_engine_name(void)
{
#if defined(DC_MARK_GEN)
	return "txn-mark";
#elif defined(DC_PER_NODE_GEN)
	return "txn-pernode";
#else
	return "txn";
#endif
}

/*
 * Capability flag for harnesses: 1 when dc_lookup returns the host ADDRESS as the
 * id (the DEFAULT 1-CL split build), 0 when it returns the logical d_id (legacy
 * layouts, or split -DDC_SPLIT_KEEPID).  A harness reads it via a weak reference
 * (absent => 0 => logical id) and, when set, checks the seed-time host-address
 * table instead of id==value; the dc_walk census (which reads the real cold
 * d_id) remains the conservation gate.  See bench_dcache.c.
 */
/* rmdir-to-negative: see dcache.h.  lock-free engine: nothing spans the check and the flip */
#ifdef DC_TXN_STATS
const int dc_txn_stats_supported = 1;
#else
const int dc_txn_stats_supported = 0;
void dc_txn_stats_dump(void *stream) { (void) stream; }
void dc_txn_stats_last(void *stream) { (void) stream; }
#endif

/* phase 3: the shared LRU (dcache_lru.h); lock arm by default, -DDC_LRU_MCAS */
#ifdef DC_NO_LRU
const int dc_lru_supported = 0;
#else
const int dc_lru_supported = 1;
#endif


#ifdef DC_IPARENT_TXN
const int dc_delete_dir_supported = 1;	/* guard/write pair, no lock: see
					 * dc_set_negative_txn */
#else
const int dc_delete_dir_supported = 0;	/* d_iparent untransacted on this arm */
#endif

#if defined(DC_HOT1CL_SPLIT) && !defined(DC_SPLIT_KEEPID)
const int dc_lookup_id_is_address = 1;
#else
const int dc_lookup_id_is_address = 0;
#endif

static struct dentry *dentry_alloc(struct dcache *dc, struct dentry *parent,
				   const struct qstr *name, uint64_t id,
				   int isdir, int positive)
{
	struct dentry *d;

	/*
	 * Cacheline-align the dentry.  The reader's hot fields are laid out to
	 * occupy CL0 (d_iparent/d_name + d_seq + d_hash.next), but calloc only
	 * guarantees 16-byte alignment -- so at 3 of every 4 base addresses that
	 * "1-CL" line actually STRADDLES two cachelines, and a lookup pays two
	 * misses per hop instead of one.  posix_memalign(64) makes the 1-CL
	 * layout real (and makes it robust to struct size: an 8-byte shrink that
	 * shifts the allocation pattern otherwise swings reader throughput ~2x).
	 */
	if (posix_memalign((void **) &d, 64, sizeof(*d)) != 0)
		return NULL;
	memset(d, 0, sizeof(*d));
	d->d_iparent = parent;
	d->d_iname = *name;
	d->d_fwd = NULL;
	d->d_back = NULL;
	d->d_parent = parent;
	d->d_dc = dc;
	urcu_txn_hlist_init(&d->d_child_head);
	d->d_id = id;
	d->d_inode = positive ? 1 : 0;
	d->d_isdir = (unsigned char) (isdir != 0);
#ifdef DC_HOT1CL
	if (!positive)			/* phase 2: this name is known ABSENT */
		d->d_iparent = (struct dentry *)
			((uintptr_t) d->d_iparent | DC_TAG_NEG);
#endif
#ifndef DC_IPARENT_TXN
	d->d_seq = NULL;		/* per-node gen (DC_PER_NODE_GEN); even */
#endif
	return d;
}

#ifndef DC_NO_LRU
static int lru_shards_init(struct dcache *dc);
#endif

struct dcache *dc_create(unsigned int nbuckets)
{
	struct dcache *dc = calloc(1, sizeof(*dc));
	unsigned int n = 1, i;
	struct qstr rootname;

	if (!dc)
		return NULL;
	while (n < nbuckets)
		n <<= 1;
	dc->buckets = calloc(n, sizeof(*dc->buckets));
	if (!dc->buckets) {
		free(dc);
		return NULL;
	}
	for (i = 0; i < n; i++)
		urcu_txn_hlist_init(&dc->buckets[i]);
	dc->mask = n - 1;
	urcu_txn_domain_init(&dc->domain);
#ifndef DC_NO_LRU
	if (lru_shards_init(dc) != 0) {
		free(dc->buckets);
		free(dc);
		return NULL;
	}
#endif

	dc_qstr_init(&rootname, "");
	dc->root = dentry_alloc(dc, NULL, &rootname, 0, 1, 1); /* root: dir, positive */
	dc->root->d_parent = dc->root;		/* root is its own parent */
	dc->root->d_iparent = dc->root;
	return dc;
}

/* Quiescent teardown: recurse the child-hlist, free every node. */
static void free_subtree(struct dentry *d)
{
	struct urcu_txn_hlist_node *n = urcu_txn_hlist_first_rcu(&d->d_child_head);

	while (n) {
		struct urcu_txn_hlist_node *next = urcu_txn_hlist_next_rcu(n);

		free_subtree(sib_dentry(n));
		n = next;
	}
	free(d);
}

void dc_destroy(struct dcache *dc)
{
	if (!dc)
		return;
	/*
	 * Two barriers: the first drains every queued fold worker (each folds a
	 * shell out of its chain, then call_rcu's the shell's own free); the
	 * second drains those shell frees.  After both, every chain is settled
	 * (each content host is its own named top again) and no shell remains, so
	 * the child-hlists hold only hosts for free_subtree to reclaim directly.
	 */
	rcu_barrier();				/* run pending folds */
	rcu_barrier();				/* run the frees they queued */
	free_subtree(dc->root);
	free(dc->buckets);
#ifndef DC_NO_LRU
	free(dc->lru);
#endif
	free(dc);
}

void dc_register_thread(void)   { rcu_register_thread(); }
void dc_unregister_thread(void) { rcu_unregister_thread(); }
void dc_quiescent(void)         { rcu_quiescent_state(); }

/* ---- lock-free lookup (inline name, no d_seq, no rename_lock) ----------- */


/*
 * Resolve @top's content host in O(1) (readers).  A settled top (d_fwd == NULL)
 * IS its own host.  A shell (d_fwd != NULL) holds a WRITE-ONCE skip pointer to
 * the tail host in its d_id/d_host union slot -- read as a pointer only because
 * d_fwd != NULL proves @top is a shell (a stable per-node property).  The d_fwd
 * load is the discriminator and is ordered before the union access, so a host's
 * id is never dereferenced as a pointer.  Unlike chain_host_rcu()'s O(depth)
 * walk, this is one hop regardless of chain depth.
 */
static inline struct dentry *host_of_rcu(struct dentry *top)
{
#ifdef DC_HOT1CL
	/* discriminate from the tag on the already-loaded d_iparent -- no d_fwd
	 * read, so a settled hop never leaves the hot cacheline. */
	return node_is_shell(top) ? rcu_dereference(top->d_host) : top;
#else
	struct dentry *fwd = urcu_txn_read((void **) &top->d_fwd, DC_FWD_TAG);

	return fwd ? rcu_dereference(top->d_host) : top;
#endif
}

/*
 * Scan @parent's name-hash bucket for the named TOP matching (d_iparent,
 * d_iname) -- the node currently in the index, which is a rename shell while a
 * move is folding, or the content host once settled.  Does NOT follow d_fwd.
 * Call within an RCU read-side section.
 */
static struct dentry *find_top_rcu(struct dcache *dc, struct dentry *parent,
				   const struct qstr *name)
{
	struct urcu_txn_hlist_head *b = bucket_of(dc, parent, name->hash);
	struct urcu_txn_hlist_node *n;

	for (n = urcu_txn_hlist_first_rcu(b); n; n = urcu_txn_hlist_next_rcu(n)) {
		struct dentry *d = hnode_dentry(n);

		if (DC_IPARENT(d) == parent &&
		    DC_INAME_EQ(DC_MATCH_NAME_SRC(d), name))
			return d;
	}
	return NULL;
}

/*
 * Same scan, but hand the matched node's RAW d_iparent back to the caller so the
 * host/shell and pos/neg tests reuse that ONE load instead of re-reading the
 * field twice more.  This matters under DC_IPARENT_TXN, where the slot is
 * transacted and every read is a uatomic_load(CMM_ACQUIRE) the compiler may not
 * CSE -- three accessor calls became three real loads, costing ~4-5% of lookup.
 * The plain build folds it back to the same code the accessors emitted.
 */
#ifdef DC_HOT1CL
static struct dentry *find_top_raw_rcu(struct dcache *dc, struct dentry *parent,
				       const struct qstr *name, uintptr_t *raw_out)
{
	struct urcu_txn_hlist_head *b = bucket_of(dc, parent, name->hash);
	struct urcu_txn_hlist_node *n;

	for (n = urcu_txn_hlist_first_rcu(b); n; n = urcu_txn_hlist_next_rcu(n)) {
		struct dentry *d = hnode_dentry(n);
		uintptr_t raw = iparent_raw(d);

		if ((struct dentry *) (raw & ~DC_TAG_MASK) == parent &&
		    DC_INAME_EQ(DC_MATCH_NAME_SRC(d), name)) {
			*raw_out = raw;
			return d;
		}
	}
	return NULL;
}

static inline struct dentry *host_of_raw(struct dentry *top, uintptr_t raw)
{
	return (raw & DC_TAG_SHELL) ? rcu_dereference(top->d_host) : top;
}
/*
 * PHASE 2: inode-ness is authoritative on the content HOST, not the named top.
 *
 * An inode is CONTENT.  A rename replaces the name -- it stacks a shell as the
 * new top -- and must not disturb it, so keeping the state on the host makes
 * rename correct for free instead of by copying the bit into every shell (and
 * into both shells of an exchange).  It is also the node d_instantiate mutates,
 * so writer and reader agree on one owner.
 *
 * Costs the reader nothing in the common case: with no rename in flight the top
 * IS the host, and @raw is already that node's word, loaded for the match.  Only
 * while a shell is stacked does this reach for the host's own word -- and that
 * read must resolve the transacted slot, because the fold's TRANSFER publishes
 * d_iparent by COMMIT rather than storing it in place.  Reading it in place is
 * what the old top-authoritative comment was avoiding; transacting the slot is
 * what makes the host readable at all.
 */
static inline int host_is_positive(uintptr_t raw, struct dentry *host)
{
	if (caa_likely(!(raw & DC_TAG_SHELL)))
		return (raw & DC_TAG_NEG) == 0;	/* top == host: reuse the load */
	return (iparent_raw(host) & DC_TAG_NEG) == 0;
}
#define DC_HOST_IS_POSITIVE(top, raw, host)	host_is_positive((raw), (host))
#else
static inline struct dentry *find_top_raw_rcu(struct dcache *dc,
					      struct dentry *parent,
					      const struct qstr *name,
					      uintptr_t *raw_out)
{
	*raw_out = 0;
	return find_top_rcu(dc, parent, name);
}
static inline struct dentry *host_of_raw(struct dentry *top, uintptr_t raw)
{
	(void) raw;
	return host_of_rcu(top);
}
#define DC_HOST_IS_POSITIVE(top, raw, host)	DC_IS_POSITIVE(host)
#endif

/*
 * Find parent's child named `name`: the named top, resolved to its content
 * host. Call within an RCU read-side section.
 */
static struct dentry *txn_child_lookup_rcu(struct dcache *dc,
					   struct dentry *parent,
					   const struct qstr *name)
{
	struct dentry *top = find_top_rcu(dc, parent, name);

	return top ? host_of_rcu(top) : NULL;		/* O(1) skip to host */
}

/*
 * Bump the walk-causality generation inside an OPEN commit.  Default build: one
 * global counter (dc->rename_gen) the reader brackets its whole walk on.  Under
 * DC_PER_NODE_GEN: the MOVED entry's own host counter (host->d_seq), so a rename
 * only trips walks that pass through that entry.  Stepped by 2 to keep bit 0 (the
 * proxy tag) clear.  The bump rides the SAME MCAS as the structural edge change
 * (the caller's txn), so a reader sees (gen, index-membership) as one atom.
 */
static inline void txn_bump_gen(struct urcu_txn *txn, struct dcache *dc,
				struct dentry *host)
{
#ifdef DC_IPARENT_TXN
	/*
	 * No counter at all: causality rides the hlist deletion MARK, which a
	 * rename's demote sets on the outgoing top in this same commit and the
	 * localized reader observes via top_unhashed_rcu.  So a rename needs no
	 * separate bump here -- the structural edit is the signal -- and this is
	 * a no-op.  (Unlink already does not call this; its del sets the same
	 * mark, which is why unlink needs nothing either.)
	 */
	(void) txn; (void) dc; (void) host;
#else
	void **slot;
	void *g;

#ifdef DC_PER_NODE_GEN
	slot = &host->d_seq;
	(void) dc;
#else
	slot = &dc->rename_gen;
	(void) host;
#endif
	g = urcu_txn_load(txn, slot, DC_GEN_TAG);
	(void) urcu_txn_store_mw(txn, slot, g, (void *) ((uintptr_t) g + 2),
			      DC_GEN_TAG);
#endif
}

#ifdef DC_LOCALIZED_GEN
/*
 * Is @top no longer the current indexed top for its name?  A rename (demote) and
 * an unlink both MARK the node's own d_hash.next -- atomically, in the same
 * commit that bumps the host gen (the hlist del contract + txn_bump_gen).  The
 * per-node reader tests this right AFTER sampling the host gen: it is the
 * "re-verify the edge under the sample" step a single host counter needs, since
 * the counter is reached only after the name match + chain resolve (there is no
 * pre-navigation point at which to sample it).  Call under rcu_read_lock().
 */
static inline int top_unhashed_rcu(struct dentry *top)
{
	void *raw = (void *) rcu_dereference(top->d_hash.next);

	if (caa_unlikely((uintptr_t) raw &
			 (URCU_TXN_HLIST_TAG | URCU_TXN_HLIST_MARK)))
		return urcu_txn_hlist_is_marked(
				urcu_txn_resolve(raw, URCU_TXN_HLIST_TAG));
	return 0;			/* clean unmarked next: still hashed */
}
#endif

#ifdef DC_LOCALIZED_GEN
/*
 * Sample the per-entry stamp for a hop.  @raw is the matched top's d_iparent,
 * already loaded by the bucket compare -- under the skip arm a SETTLED entry has
 * host == top, so the stamp costs no load at all; only a shelled entry (a fold
 * outstanding) reaches for the host's own line, and that state is transient.
 */
static inline dc_stamp_t dc_stamp_of(struct dentry *host, struct dentry *top,
				     uintptr_t raw)
{
#if defined(DC_MARK_GEN)
	/*
	 * The stamp IS the latched top: the up-pass re-tests whether it is still
	 * the indexed top for its name.  Nothing to sample -- the descent's
	 * top_unhashed_rcu(top) already established that it was, which is the
	 * first half of the double-collect.  Costs no load at all.
	 */
	(void) host; (void) raw;
	return (uintptr_t) top;
#else
	(void) top; (void) raw;
	return (uintptr_t) urcu_txn_read(&host->d_seq, DC_GEN_TAG);
#endif
}

/* Which node the up-pass re-examines: the content host for the counter arms,
 * the named top for the mark arm. */
static inline struct dentry *dc_latch_node(struct dentry *host,
					   struct dentry *top)
{
#ifdef DC_MARK_GEN
	(void) host;
	return top;
#else
	(void) top;
	return host;
#endif
}

static inline dc_stamp_t dc_stamp_reread(struct dentry *node)
{
#if defined(DC_MARK_GEN)
	/* re-TEST, not re-read: still unmarked means still the indexed top */
	return top_unhashed_rcu(node) ? 0 : (uintptr_t) node;
#else
	return (uintptr_t) urcu_txn_read(&node->d_seq, DC_GEN_TAG);
#endif
}
#endif

enum dc_result dc_lookup(struct dcache *dc, const struct dc_path *p,
			 uint64_t *out_id)
{
	enum dc_result res;
	uint64_t id;
#ifdef DC_LOCALIZED_GEN
	struct dentry *hosts[DC_PATH_MAX];	/* latched node per hop */
	dc_stamp_t     seqs[DC_PATH_MAX];	/* its version, sampled on descent */
	uint32_t nlatched;
#endif

	rcu_read_lock();
#ifndef DC_LOCALIZED_GEN
	/*
	 * GLOBAL bracket over the WHOLE walk: read the rename generation before
	 * touching any node and again after, and retry the walk if it moved.  A
	 * concurrent rename bumps rename_gen inside its commit, so a changed
	 * generation means the multi-component walk may have straddled a move (an
	 * interior dir relocating out from under us) -- discard it.  The generation
	 * is read before the first node so there is no per-hop observe-then-sample
	 * window to close.  Cost: every reader reads (and every rename writes) one
	 * whole-tree cacheline -- the bottleneck the per-node build removes.
	 */
	for (;;) {
		struct dentry *cur = dc->root;
		void *g0, *g1;
		uint32_t i;

		g0 = urcu_txn_read(&dc->rename_gen, DC_GEN_TAG);	/* acquire */
		res = DC_POSITIVE;
		id = DC_FAST_ID(cur);
		for (i = 0; i < p->ndepth; i++) {
			uintptr_t raw;
			struct dentry *top = find_top_raw_rcu(dc, cur,
							      &p->comp[i], &raw);
			struct dentry *d;

			if (!top) {
				res = DC_ABSENT;
				break;
			}
			d = host_of_raw(top, raw);	/* O(1) skip to host */
			cur = d;
			id = DC_FAST_ID(d);
			/* pos/neg is authoritative on the HOST (phase 2): an
			 * inode is content, and a rename must not disturb it.
			 * Free when no rename is in flight -- top IS the host and
			 * @raw is already its word; only a stacked shell reaches
			 * for the host's own, through the transacted slot. */
			res = DC_HOST_IS_POSITIVE(top, raw, d) ? DC_POSITIVE
							       : DC_NEGATIVE;
#ifdef DC_TEST_HOOKS
			if (dc_test_walk_hook)
				dc_test_walk_hook((int) i);
#endif
		}
		cmm_smp_rmb();			/* walk loads before re-reading gen */
		g1 = urcu_txn_read(&dc->rename_gen, DC_GEN_TAG);
		if (g0 == g1)
			break;			/* no rename crossed the walk */
		/* a rename touched the tree mid-walk: re-walk from the root */
	}
#else
	/*
	 * PER-NODE walk: no global bracket.  At each hop, match the child's TOP by
	 * name, resolve to its content host, SAMPLE that host's gen, then (rmb)
	 * confirm the top is still the current indexed top -- the "re-verify the
	 * edge under the sample" step a single host counter needs, since the gen is
	 * reached only after the name match + chain resolve (there is no
	 * pre-navigation point to sample it).  Remember (host, gen) per hop; on the
	 * way UP re-read every latched host's gen.  All unchanged means the whole
	 * path was simultaneously live at the leaf-turnaround instant -- each hop's
	 * gen brackets [sample, up-read] and the turnaround lies in every window.
	 * A rename bumps ONLY the moved entry's host gen (txn_bump_gen), so a walk
	 * down a disjoint path re-reads a disjoint set of gens: no shared counter,
	 * no whole-tree cacheline.
	 */
	for (;;) {
		struct dentry *cur = dc->root;
		uint32_t i, j;
		int stale = 0, moved = 0;

		res = DC_POSITIVE;
		id = DC_FAST_ID(cur);
		nlatched = 0;
		for (i = 0; i < p->ndepth; i++) {
			uintptr_t raw;
			struct dentry *top = find_top_raw_rcu(dc, cur,
							      &p->comp[i], &raw);
			struct dentry *host;
			dc_stamp_t s;

			if (!top) {
				res = DC_ABSENT;
				break;
			}
			host = host_of_raw(top, raw);	/* O(1) skip to host */
			s = dc_stamp_of(host, top, raw);		/* sample */
			cmm_smp_rmb();		/* sample stamp before the confirm */
			if (top_unhashed_rcu(top)) {	/* top left the index */
				stale = 1;
				break;
			}
			hosts[nlatched] = dc_latch_node(host, top);
			seqs[nlatched] = s;
			nlatched++;
			cur = host;
			id = DC_FAST_ID(host);
			/* pos/neg off the HOST -- see the global arm above. */
			res = DC_HOST_IS_POSITIVE(top, raw, host) ? DC_POSITIVE
								  : DC_NEGATIVE;
#ifdef DC_TEST_HOOKS
			if (dc_test_walk_hook)
				dc_test_walk_hook((int) i);
#endif
		}
		if (stale)
			continue;		/* a top was demoted/unlinked: re-walk */
		cmm_smp_rmb();			/* walk loads before the up-pass */
		for (j = 0; j < nlatched; j++)
			if (dc_stamp_reread(hosts[j]) != seqs[j]) {
				moved = 1;
				break;
			}
		if (!moved)
			break;			/* every latched host stable: done */
		/* a rename touched a node on the path mid-walk: re-walk */
	}
#endif
	rcu_read_unlock();

	if (res == DC_POSITIVE && out_id)
		*out_id = id;
	return res;
}

/* ---- writer-side resolution -------------------------------------------- */

static struct dentry *__child_lookup(struct dcache *dc, struct dentry *parent,
				     const struct qstr *name)
{
	struct dentry *d;

	rcu_read_lock();
	d = txn_child_lookup_rcu(dc, parent, name);
	rcu_read_unlock();
	return d;
}

static struct dentry *resolve(struct dcache *dc, const struct dc_path *p,
			      uint32_t depth)
{
	struct dentry *cur = dc->root;
	uint32_t i;

	for (i = 0; i < depth; i++) {
		cur = __child_lookup(dc, cur, &p->comp[i]);
		if (!cur)
			return NULL;
		/* retain_dentry: the WRITER-side walk, so it stands in for the
		 * ref-walk's dget/dput of each component.  dc_lookup does not
		 * come through here, which is why the reader pays nothing. */
		lru_retain(dc, cur);
	}
	return cur;
}

/* Resolve the transacted d_parent slot (RCU-side; call within a read section). */
static inline struct dentry *parent_of_rcu(struct dentry *d)
{
	return urcu_txn_read((void **) &d->d_parent, DC_PARENT_TAG);
}

/* Is @d's child-hlist empty?  Call within an RCU read-side section. */
static int children_empty(struct dentry *d)
{
	return urcu_txn_hlist_first_rcu(&d->d_child_head) == NULL;
}

static void dentry_free_cb(struct rcu_head *rh);

/*
 * ⭐⭐ QUEUE THE RECLAIM -- and the CALLER does it, AFTER the victim is off the
 * LRU.  This used to be the last line of lru_evict_settled(), which put the
 * call_rcu BEFORE the deque removal / shrink-release and left a window in which
 * the node was simultaneously condemned and still reachable from a shard.  That
 * window was safe only because the sweeper's read-side section happened to span
 * it -- i.e. MEMORY SAFETY rested on the extent of an rcu_read_lock(), with
 * nothing in the code's shape saying so.
 *
 * It was never a designed ordering: the delegated-kill branch of the very same
 * sweeper loop already sealed first and called call_rcu second.  Two adjacent
 * branches disagreed, and the only reason was that one of them inherited the
 * free from the tail of a helper.  Now both go through here.
 *
 * ⚠ The SEAL is still required and this does not replace it: between a plain
 * remove and this call a concurrent lru_retain() could push the node back onto
 * a deque, and then the grace period would free queued storage.  The seal makes
 * the node unpushable in the SAME commit as the unlink, which is what closes
 * that; see lru_del_can_free().
 */
static inline void dc_reclaim(struct dentry *d)
{
	DC_LC_TO_DEAD(d);
	call_rcu(&d->d_rcu, dentry_free_cb);
}

#ifndef DC_NO_LRU
/*
 * Evict one SETTLED dentry: remove it from BOTH indexes in one commit and defer
 * the free, i.e. dc_unlink's core taking the dentry instead of a path.
 *
 * It takes the dentry rather than rebuilding a path because a host's own
 * d_iname is stale while a shell is stacked above it -- the live name is on the
 * top -- so naming a victim mid-rename would name it wrongly.  Settled nodes
 * have no such gap.  Anything on a transition chain is SKIPPED; renames are
 * transient, so it is a candidate again next pass (mainline does the same
 * whenever it cannot get d_lock).  Returns 0, or -EAGAIN to skip.
 */
static int lru_evict_settled(struct dcache *dc, struct dentry *d)
{
	struct dentry *parent;
	struct urcu_txn txn;
	int p_, ret = -EAGAIN;

	if (urcu_txn_read((void **) &d->d_back, DC_FWD_TAG) ||
	    urcu_txn_read((void **) &d->d_fwd, DC_FWD_TAG))
		return -EAGAIN;			/* on a transition chain */
	parent = parent_of_rcu(d);
	if (!parent || parent == d)
		return -EAGAIN;			/* the root anchors the tree */

	urcu_txn_init(&txn, &dc->domain);
	for (;;) {
		enum urcu_txn_status st;

		urcu_txn_begin(&txn);
		if (!children_empty(d) ||
		    urcu_txn_read((void **) &d->d_back, DC_FWD_TAG)) {
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			return -EAGAIN;
		}
#ifdef DC_TXN_PARENT_DELAY
		/*
		 * TARGETED REPRODUCER, never shipped.  children_empty() above is
		 * a plain RCU read and the commit below is what acts on it, so
		 * the defect lives in the gap between them -- a dc_add that
		 * publishes a child into that gap leaves it hashed under a
		 * directory this eviction is about to call_rcu.  At natural
		 * timings the gap is a few instructions and 48w/48r under TSAN
		 * did not reproduce it in 4 runs, so widen the gap itself.
		 *
		 * ⚠ Argues ONE WAY ONLY: firing proves the race exists and
		 * identifies which build closes it.  It says NOTHING about the
		 * rate at shipped timings.
		 */
		/*
		 * ⚠ SWEEP THIS, do not just crank it.  The delay sits on EVERY
		 * eviction attempt, so a large one starves the shrinker: fewer
		 * evictions means fewer chances for an add to land in the gap,
		 * and past some width the reproducer detects LESS, not more.
		 * -DDC_TXN_PARENT_DELAY_US selects the width.
		 */
#ifndef DC_TXN_PARENT_DELAY_US
#define DC_TXN_PARENT_DELAY_US 200
#endif
		{
			struct timespec ts = {
				DC_TXN_PARENT_DELAY_US / 1000000,
				(DC_TXN_PARENT_DELAY_US % 1000000) * 1000L,
			};

			(void) nanosleep(&ts, NULL);
		}
#endif
		/*
		 * ⭐ GUARD THE CHILD LIST AS STILL EMPTY at the install point --
		 * the other half of the pair with dc_add (see the long note
		 * there).  children_empty() above is a plain RCU read, so on its
		 * own it is a check-then-act: an add can commit between it and
		 * this commit, and then this eviction frees a parent that has
		 * just gained a child.  Recording the slot makes dc_add's write
		 * to it abort us instead.
		 *
		 * The rule this is an instance of, and the same one behind
		 * 750572af and b69b4a53: AN OPERATION THAT READS A SLOT IT DOES
		 * NOT WRITE MUST VALIDATE THAT READ.
		 */
#ifdef DC_TXN_PARENT_GUARD
		urcu_txn_validate(&txn, (void **) &d->d_child_head.first,
				  NULL, URCU_TXN_HLIST_TAG);
#endif
#ifdef DC_TXN_PARENT_SEAL
		/*
		 * ⭐⭐ THE SAME EXCLUSION, BOUGHT WITH A SLOT THIS COMMIT
		 * ALREADY HAS TO TOUCH -- and therefore FREE on the add side,
		 * which is where DC_TXN_PARENT_GUARD's measured cost lives.
		 *
		 * SEAL the victim's child head instead of guarding it: the same
		 * slot, the same expected old (NULL == "still empty"), but
		 * written rather than read.  dc_add publishes a child by writing
		 * &parent->d_child_head.first, so the two now contend on ONE
		 * slot with ONE expected old and the MCAS admits exactly one:
		 *
		 *   add wins   -> this store's old-value check fails, this
		 *                 eviction ABORTS, retries, sees a non-empty
		 *                 child list and answers -EAGAIN;
		 *   evict wins -> the head is MARKED, and the add's own
		 *                 urcu_txn_hlist_insert_head_prepare() answers
		 *                 -ENOENT ("head sealed") with NO guard record
		 *                 of its own.
		 *
		 * ⭐ The marked head is not an invention: <urcu/rcu-txn-hlist.h>
		 * reserves it as the sealing primitive and insert_head_prepare
		 * already refuses one.  urcu_txn_hlist_resolve() STRIPS the
		 * mark, so a sealed head still reads as EMPTY to every walker --
		 * children_empty(), dc_readdir() and the census are unaffected.
		 *
		 * Terminal per LIFETIME, not per address: dentry_alloc() memsets,
		 * so a recycled dentry starts unsealed.  Same rule as
		 * DC_LRU_DEAD and URCU_TXN_DEQUE_POISON, and sealed in the SAME
		 * commit as the two unlinks for the same reason -- there must be
		 * no instant at which @d is gone from an index but still
		 * accepting children.
		 */
		if (urcu_txn_store_mw(&txn, (void **) &d->d_child_head.first,
				      NULL, urcu_txn_hlist_set_mark(NULL),
				      URCU_TXN_HLIST_TAG)) {
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			return -EAGAIN;		/* -ENOMEM: nothing published */
		}
#endif
		p_ = urcu_txn_hlist_del_prepare(&txn, &d->d_hash);
		if (!p_)
			p_ = urcu_txn_hlist_del_prepare(&txn, &d->d_sib);
		if (p_) {			/* -ENOENT (gone) / -EAGAIN */
			/*
			 * ⚠⚠ ABANDON, NOT CONFLICT -- THIS PATH RETURNS.
			 * urcu_txn_conflict() "keeps the FIFO turn exactly as a
			 * commit ABORT does", which is right only when the
			 * caller then re-attempts with the SAME handle.  This
			 * one hands -EAGAIN back to lru_shrink_range, which
			 * rotates and moves on -- so the lane was kept by a
			 * handle nobody ever re-attempts, i.e. HELD FOR EVER
			 * with every other writer parked behind it.
			 */
			DC_LANE_GIVE_UP(&txn);
			urcu_txn_end(&txn);
			return -EAGAIN;
		}
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		if (st < 0)
			return -EAGAIN;
		break;
	}
	/* ⭐ The caller queues the reclaim, once the victim is off the LRU --
	 * see dc_reclaim().  Returning 0 means "committed; the free is yours". */
	ret = 0;
	return ret;
}

/*
 * LIVENESS FOR THE LRU -- mainline retain_dentry's first test, transacted.
 *
 * &d->d_hash.next is an MCAS-managed slot on this engine: urcu_txn_hlist_del
 * MARKS it inside the commit that unhashes @d.  So a load_validate here puts
 * "still hashed" into the LRU push's conflict set, and the two possible
 * interleavings both come out right:
 *
 *   unhash commits FIRST  -> our guard's re-check fails, the push aborts, the
 *                            retry reads the mark and answers -ENOENT;
 *   our push commits FIRST -> lru_evict_settled's caller runs lru_del() AFTER
 *                            its unhash commit, so it finds the node we just
 *                            pushed and takes it off before call_rcu can fire.
 *
 * There is no third order: the guard is installed atomically with the edges.
 *
 * Cross-DOMAIN (the LRU commits on lru_domain, the unhash on dc->domain) is
 * fine -- a domain only owns the escalation lane, while conflict detection is
 * per-slot.  The cost is that our guard can abort an unlink's commit, which
 * that unlink simply retries.
 */
#ifdef DC_LRU_MCAS
/* 1: d_hash.next is MCAS-managed here, so the guard joins the push's conflict
 * set and the push is ATOMIC with it.  The header branches on this -- it is why
 * this engine keeps lru_retain's re-arm where bucketlock cannot. */
#define DC_LRU_ALIVE_TRANSACTED 1
static int lru_alive_validate(struct urcu_txn *txn, struct dentry *d)
{
	void *raw = urcu_txn_load_validate(txn, (void **) &d->d_hash.next,
					   URCU_TXN_HLIST_TAG);

	return urcu_txn_hlist_is_marked(raw) ? -ENOENT : 0;
}

/*
 * The same question with no transaction to put it in -- the LOCK arm's only
 * option, and a HINT: the unhash can land between this and the enqueue.  Not
 * spelled via top_unhashed_rcu(), which exists only under DC_LOCALIZED_GEN.
 */
#else
static int lru_alive_hint(struct dentry *d)
{
	void *raw = (void *) rcu_dereference(d->d_hash.next);

	if (caa_unlikely((uintptr_t) raw &
			 (URCU_TXN_HLIST_TAG | URCU_TXN_HLIST_MARK)))
		return !urcu_txn_hlist_is_marked(
				urcu_txn_resolve(raw, URCU_TXN_HLIST_TAG));
	return 1;			/* clean unmarked next: still hashed */
}
#endif
#endif

#include "dcache_lru.h"			/* PHASE 3: the shared LRU */
#ifndef DC_NO_LRU
#include "dcache_lru_shrink.h"	/* PHASE 3: the shared CLOCK shrinker */
#endif

/* ---- add / unlink ------------------------------------------------------ */

static int dc_add_typed(struct dcache *dc, const struct dc_path *path,
			uint64_t id, int isdir, int positive)
{
	struct dentry *parent, *d;
	const struct qstr *name;
	struct urcu_txn_hlist_head *bucket;
	struct urcu_txn txn;
	enum urcu_txn_status st;
	int ret = 0, p;

	if (path->ndepth == 0)
		return -EEXIST;

	parent = resolve(dc, path, path->ndepth - 1);
	if (!parent)
		return -ENOENT;
	/*
	 * A FILE has no children (enforced here): adding under one is -ENOTDIR.
	 * This is the invariant the file-rename bump-skip relies on -- without it,
	 * a "file" that had gained a child could misdirect a straddling reader.
	 */
	if (!parent->d_isdir)
		return -ENOTDIR;
	name = &path->comp[path->ndepth - 1];
	if (__child_lookup(dc, parent, name))
		return -EEXIST;
	d = dentry_alloc(dc, parent, name, id, isdir, positive);
	if (!d)
		return -ENOMEM;
	bucket = bucket_of(dc, parent, name->hash);

	/*
	 * ONE composed commit publishes the new dentry into BOTH indexes at once --
	 * the name hash (d_hash) and the parent's child list (d_sib) -- exactly like
	 * dc_unlink and stack_shell.  The two heads are distinct memory, so the
	 * write set is disjoint (fast path).  This replaces two separate
	 * urcu_txn_hlist_add_rcu commits + a hand-rolled rollback, which left a fresh
	 * dentry hash-visible (to lookup) before child-visible (to readdir) and paid
	 * two commit cycles instead of one.
	 */
	urcu_txn_init(&txn, &dc->domain);
	urcu_txn_declare_disjoint(&txn);	/* two distinct heads: no same-slot WAW */
	for (;;) {
#ifdef DC_IPARENT_TXN
		uintptr_t parent_raw;
#endif

		urcu_txn_begin(&txn);
#ifdef DC_IPARENT_TXN
		/* Read the parent's state INSIDE the transaction, so the guard
		 * below records what this add actually decided on.  A negative
		 * parent is refused outright: the name is not there. */
		parent_raw = iparent_raw(parent);
		if (parent_raw & DC_TAG_NEG) {
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			free(d);		/* never published */
			return -ENOENT;
		}
#endif
#ifdef DC_IPARENT_TXN
		/*
		 * GUARD the parent's state word.  A concurrent d_delete of an
		 * empty DIRECTORY flips DC_TAG_NEG in this very slot, and a
		 * negative must never gain a child -- so make that flip and this
		 * insert mutually exclusive by recording the value this add
		 * decided on.  d_delete guards the child head we WRITE below, so
		 * each side's write set hits the other's read set and the two
		 * cannot both commit, in either order.
		 *
		 * The cost is one read-set entry on the add path, and one real
		 * consequence worth measuring: this slot also carries the parent
		 * POINTER and the shell tag, so an add under a directory aborts
		 * when that directory is itself renamed or its fold runs -- not
		 * only when it goes negative.
		 */
		urcu_txn_validate(&txn, (void **) &parent->d_iparent,
				  (void *) parent_raw, DC_IPARENT_TAG);
		DC_TEST_ADD_HOOK();		/* repro parks here */
#endif
#if defined(DC_TXN_PARENT_GUARD) && !defined(DC_NO_LRU)
		/*
		 * ⛔ OFF BY DEFAULT -- IMPLEMENTED, CORRECT, AND MEASURED TOO
		 * EXPENSIVE ON THIS ENGINE.  Enable with -DDC_TXN_PARENT_GUARD.
		 *
		 * It works: the txn engine's TSAN heap-use-after-free goes 1/8
		 * -> 0/8 with both halves on.  What it costs is LIVENESS.  Both
		 * halves add read-set entries to hot paths (this one to every
		 * add, the eviction's to every sweep).
		 *
		 * RE-MEASURED at 20 trials/cell (the original figures were 6),
		 * --evict bursty, 48w/48r, runs not finishing in 120s:
		 *
		 *   guard off, dup-recheck on (shipped)     1/20
		 *   guard ON,  dup-recheck on               9/20
		 *   guard off, dup-recheck off              0/20
		 *   guard ON,  dup-recheck off              8/20
		 *
		 * +40 points in BOTH rows (Fisher 0/20 vs 8/20, p ~ 0.003), and
		 * the dc_add duplicate re-check contributes nothing (1/20 vs
		 * 0/20).  So the cost is the guard's, it is real, and it is
		 * bigger than the 2/6 that first shelved it.
		 *
		 * ⛔⭐⭐ BUT THE REASON RECORDED HERE WAS WRONG, AND THIS IS THE
		 * RETRACTION.  This comment used to promise "re-measure once the
		 * park-while-online defect is fixed in liburcu, which is where
		 * that cost actually lives."  BOTH candidate mechanisms have
		 * since been eliminated and the cost did not move:
		 *
		 *   - park-while-online was ALREADY fixed (urcu-txn-dev
		 *     dcf1310c brackets the escalation-lane wait with
		 *     thread_offline/online), so grace periods do advance while
		 *     writers queue;
		 *   - every rcu-txn-hlist.h bracket leaked the lane on its
		 *     terminal bail until fc663f5a, which is the header this
		 *     engine runs on -- also fixed, also no change.
		 *
		 * So the cost is NOT a liburcu bug waiting to be fixed.  It is
		 * the intrinsic price of extra conflict-set entries on this
		 * engine's hot paths, and the mechanism is now UNATTRIBUTED
		 * rather than explained.  Do not re-shelve this behind another
		 * pending fix without measuring first.
		 *
		 * Against that, the defect it closes is 1/8 here versus 6/8 on
		 * bucketlock, where the fix is a third bucket lock and costs no
		 * liveness at all.  Kept as a build arm.
		 *
		 * ✅ THAT DIRECTION HAS NOW BEEN FOUND: -DDC_TXN_PARENT_SEAL.
		 * The eviction SEALS &d->d_child_head.first (marked NULL) in its
		 * own commit instead of guarding it, so the add's existing write
		 * to that slot is the exclusion and the add needs NO record of
		 * its own.  Measured 0/20 timeouts against this guard's 9/20,
		 * and it closes the same use-after-free.  See lru_evict_settled.
		 * This validate is kept only as the A/B partner.
		 *
		 * ⭐ GUARD THE PARENT'S LIVENESS -- half of a GUARD PAIR with
		 * lru_evict_settled(), and a DIFFERENT slot from the state word
		 * guarded above.
		 *
		 * The shrinker can evict the parent itself: it removes it from
		 * both indexes and hands it to call_rcu.  Nothing above notices
		 * -- an eviction marks d_hash.next and d_sib.next, not
		 * d_iparent -- so without this the add commits under a parent
		 * that is one grace period from being freed.  The child is then
		 * hashed, on the LRU, and naming dead memory; a later sweeper
		 * reads its stale d_parent and CASes into the freed parent from
		 * inside lru_evict_settled's own commit.
		 *
		 * The pair, and BOTH halves are needed -- exactly the argument
		 * dc_set_negative_txn makes for the rmdir-to-negative pair:
		 *
		 *   this   WRITES parent->d_child_head, GUARDS parent->d_hash
		 *   evict  WRITES parent->d_hash,       GUARDS parent->d_child_head
		 *
		 * so each side's write set hits the other's read set and they
		 * cannot both commit, in EITHER order.  One half alone is
		 * one-directional: this guard alone still lets an eviction
		 * commit after it read the child list empty, and the eviction's
		 * guard alone still lets this add land under an already-marked
		 * parent.
		 *
		 * ⚠ The bucketlock engine needs only ONE explicit check for the
		 * same race, because lru_evict_settled re-verifies emptiness
		 * while holding &parent->d_child_head -- the lock IS the pair
		 * there.  Here there is no such lock, so both halves are
		 * written out.
		 *
		 * The root reads as hashed (its d_hash.next is NULL, hence
		 * unmarked), so adds directly under it are unaffected; the cost
		 * is one read-set entry on the add path.
		 */
		/*
		 * ⚠⚠ ONLY FOR A SETTLED PARENT, exactly as on the bucketlock
		 * engine.  A host with a shell stacked above it is legitimately
		 * absent from the index -- the shell carries the entry -- so its
		 * own d_hash reads MARKED while the directory is alive.  Testing
		 * unconditionally rejected every add under a renamed directory
		 * (test_dcache "name recreated over a moved directory", 8
		 * failures on both engines).  A chained parent needs no test
		 * from the shrinker's side: lru_evict_settled bails on
		 * d_back/d_fwd, so it is not the one that could be killing it.
		 *
		 * ⚠ RESIDUAL, stated rather than papered over: a parent that is
		 * chained HERE and settles before this commit is not covered by
		 * this half, and the other half (the eviction's guard on
		 * d_child_head.first) only aborts the eviction if this add
		 * commits FIRST.  That needs a rename to complete inside the
		 * window; it is narrower than the race being closed, not zero.
		 */
		if (!urcu_txn_read((void **) &parent->d_back, DC_FWD_TAG) &&
		    !urcu_txn_read((void **) &parent->d_fwd, DC_FWD_TAG) &&
		    urcu_txn_hlist_is_marked(
			    urcu_txn_load_validate(&txn,
						   (void **) &parent->d_hash.next,
						   URCU_TXN_HLIST_TAG))) {
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			free(d);		/* never published */
			return -ENOENT;		/* the prefix went */
		}
#endif
#ifndef DC_NO_ADD_DUP_RECHECK
		/*
		 * ⭐ RE-CHECK THE NAME INSIDE THE TRANSACTION.  The
		 * __child_lookup() before the loop is a CHECK-THEN-ACT: it holds
		 * nothing, so two concurrent adds of one name both pass it and
		 * both publish, leaving the bucket with TWO dentries spelled
		 * alike.  A lookup then resolves whichever the chain reaches
		 * first while a child-list walk descends the other, so
		 * everything under the loser is reachable by dc_walk/dc_readdir
		 * and ABSENT to dc_lookup -- permanently, with nothing to unhash
		 * it.  The kernel-faithful seqlock baseline never had this: its
		 * exists-test and its insert are both under the bucket lock.
		 *
		 * ⭐⭐ WHAT MAKES IT ATOMIC HERE IS A SLOT, NOT A LOCK, AND IT
		 * COSTS NO NEW READ-SET ENTRY.  Read the bucket head ONCE, scan
		 * for the name AFTER that read, and hand that SAME value to the
		 * insert as its expected old.  Then:
		 *
		 *   a peer published BEFORE our head read -> the scan runs after
		 *     it and sees the name              -> -EEXIST;
		 *   a peer publishes AFTER our head read -> every insert of this
		 *     name targets this same head slot, so the committed value
		 *     no longer matches @fn            -> ABORT, and the retry
		 *                                         re-scans and sees it.
		 *
		 * That is why insert_head_prepare() is opened up here rather
		 * than called: it does its OWN load of the head, and a scan
		 * placed before that load would only NARROW the window -- the
		 * peer's publish would land between the two and be adopted as
		 * this insert's expected old, which validates cleanly.
		 *
		 * ⚠ urcu_txn_load(), not the optimistic form: this slot enters
		 * the write set, so the read policy requires the waiting load --
		 * which also settles an undecided peer before we scan, so we
		 * never read its logical-old and miss a name it is committing.
		 *
		 * A concurrent DELETE of the name can make this answer -EEXIST
		 * for a name that is going away; that is the same benign
		 * staleness the pre-check always had, and it cannot create a
		 * duplicate, which is the property being bought.
		 */
		{
			void *fn = urcu_txn_load(&txn, (void **) &bucket->first,
						 URCU_TXN_HLIST_TAG);

			if (urcu_txn_hlist_is_marked(fn)) {
				p = -ENOENT;	/* head sealed: bucket is gone */
			} else if (__child_lookup(dc, parent, name)) {
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				free(d);	/* never published */
				return -EEXIST;
			} else {
				p = urcu_txn_hlist_insert_at_slot_prepare(&txn,
					&d->d_hash, &bucket->first,
					(struct urcu_txn_hlist_node *) fn);
			}
		}
#else
		p = urcu_txn_hlist_insert_head_prepare(&txn, &d->d_hash, bucket);
#endif
		if (!p)
			p = urcu_txn_hlist_insert_head_prepare(&txn, &d->d_sib,
							&parent->d_child_head);
		if (p == -EAGAIN) {		/* an old first node is mid-delete */
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;
		}
		if (p) {			/* -ENOENT: a head is sealed */
			DC_LANE_GIVE_UP(&txn);	/* terminal: release the lane */
			urcu_txn_end(&txn);
			ret = p;
			goto fail;
		}
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		if (st < 0) {			/* -ENOMEM: nothing published */
			ret = -ENOMEM;
			goto fail;
		}
		break;				/* published into both indexes */
	}
	/* PHASE 3: on the LRU at the TAIL (newest), after publishing and outside
	 * the commit -- the LRU has no reader, so it need not be atomic with the
	 * index edit. */
	DC_LC_SET(d, DC_LC_M(DC_LC_NEW), DC_LC_LIVE);	/* published into both indexes */
	lru_add(dc, d);
	return 0;
fail:
	free(d);
	return ret;
}

/*
 * dc_add creates a DIRECTORY (historical: every node could have children).
 * dc_add_file creates a FILE -- a leaf that can never gain children, so a
 * rename of it skips the walk-causality bump (only global/per-node bump; the
 * mark arm is unaffected).  Both reject a child under a file with -ENOTDIR.
 */
int dc_add(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	return dc_add_typed(dc, path, id, 1, 1);
}

int dc_add_file(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	return dc_add_typed(dc, path, id, 0, 1);
}

/*
 * Phase 2.  A negative dentry caches the ABSENCE of a name, so it is a leaf by
 * construction (file type): a name that is not there has no children, and an
 * add beneath it must fail -ENOTDIR rather than invent them.
 */
int dc_add_negative(struct dcache *dc, const struct dc_path *path)
{
	return dc_add_typed(dc, path, 0, 0, 0);
}

/*
 * Phase 2: flip the content HOST between positive and negative in place.  Both
 * d_instantiate (negative -> positive, @id is the new inode) and d_delete
 * (positive -> negative, @id ignored) are this one word change; the dentry
 * keeps its address, its bucket, its children and its chain position.
 *
 * This is the one shape the txn engines' simplification did NOT already cover.
 * The seqlock baseline brackets it in the per-dentry d_seq it still has, which
 * is exactly what that seqcount is for.  These engines deleted d_seq and paid
 * for it by treating pos/neg as write-once-per-identity, so a plain store here
 * is precisely the mutation that assumption forbids.
 *
 * WHAT REPLACES d_seq IS AN ATOMIC RMW, NOT A TRANSACTION, and getting that
 * wrong is the interesting part of phase 2.  The obvious move -- publish the
 * flip as a single-slot commit on d_iparent -- does not work, for a reason the
 * library states outright: urcu_txn_store_sw() "parks it with a plain store
 * that never fails", an SW-only commit never contention-aborts, and SW is a
 * PROMISE OF EXCLUSION that must hold across every writer of the slot.  The
 * fold's TRANSFER is another writer of exactly this word (it pulls the outgoing
 * top's parent down into the host), running on a call_rcu worker, so the
 * promise is false and two plain stores race: the fold reads the host positive,
 * this publishes NEGATIVE, the fold writes back the bit it read, and the delete
 * is silently gone.
 *
 * d0e7955 saw this coming and said so -- the fold's in-place identity write was
 * "benign today only because rename preserves inode-ness ... but it is UB and a
 * latent correctness bug once phase-2 negative dentries land."  It landed.
 *
 * store_mw() would be correct, but it installs a descriptor that every reader
 * must resolve, and the global and per-node arms deliberately leave d_iparent
 * untransacted (d0e7955 rejected paying resolving reads on the hottest field).
 * A cmpxchg is correct on EVERY arm and costs the reader nothing: the loop
 * re-reads and re-decides if the fold moved the parent bits underneath it, and
 * the reader's load stays exactly as plain as it was.  The CAS is against the
 * RAW word rather than iparent_raw()'s resolved value, which is sound because
 * after this change NO transacted writer targets d_iparent, so it never holds
 * a proxy.
 *
 * @id is stored before the flip: it is cold, read only by validation builds
 * (DC_SPLIT_KEEPID) and the census, and a reader that sees the node positive
 * must already see the id that came with it.
 */
#ifdef DC_IPARENT_TXN
/*
 * TRANSACTED flip, for the arms that already transact d_iparent (the mark arm).
 * @isdir additionally GUARDS the host's child list as still EMPTY at the install
 * point, which is what makes rmdir-to-negative possible without a lock:
 *
 *   dc_add   WRITES parent->d_child_head and GUARDS parent->d_iparent
 *   this     GUARDS host->d_child_head   and WRITES host->d_iparent
 *
 * Each one's write set hits the other's read set, so the two cannot both
 * commit -- in EITHER order.  A guard on only one side would be one-directional:
 * dc_add's guard alone lets a child land after this checked emptiness, and this
 * guard alone lets a child land after dc_add read the parent positive.
 *
 * Costs the READER nothing on this arm: iparent_raw() already resolves this slot
 * (DC_IPARENT_TXN), which until now resolved a proxy that was never installed.
 * Making the fold's handover and this flip real MW records is what makes that
 * resolve earn its keep.
 */
static int dc_set_negative_txn(struct urcu_txn *txn, struct dentry *host,
			       int negative, int isdir, uint64_t id)
{
	uintptr_t raw = iparent_raw(host);

	if (!!(raw & DC_TAG_NEG) == !!negative)
		return negative ? -ENOENT : -EEXIST;
	if (isdir) {
		if (!children_empty(host))
			return -ENOTEMPTY;
		urcu_txn_validate(txn, (void **) &host->d_child_head.first,
				  NULL, URCU_TXN_HLIST_TAG);
		DC_TEST_DEL_HOOK();		/* repro parks here */
	}
	if (!negative) {
		host->d_id = id;
		cmm_smp_wmb();			/* id before positive */
	}
	if (urcu_txn_store_mw(txn, (void **) &host->d_iparent,
			      (void *) raw,
			      (void *) (negative ? (raw | DC_TAG_NEG)
					         : (raw & ~DC_TAG_NEG)),
			      DC_IPARENT_TAG))
		return -ENOMEM;
	host->d_inode = negative ? 0 : 1;	/* cold bookkeeping; tag rules */
	return 0;
}
#endif

#ifndef DC_IPARENT_TXN		/* the transacted arms use dc_set_negative_txn */
static int dc_set_negative(struct dentry *host, int negative, uint64_t id)
{
#ifdef DC_HOT1CL
	for (;;) {
		uintptr_t raw = (uintptr_t) uatomic_load(&host->d_iparent,
							 CMM_RELAXED);
		uintptr_t want;

		if (!!(raw & DC_TAG_NEG) == !!negative)
			return negative ? -ENOENT : -EEXIST;
		if (!negative) {
			host->d_id = id;
			cmm_smp_wmb();		/* id before positive */
		}
		want = negative ? (raw | DC_TAG_NEG) : (raw & ~DC_TAG_NEG);
		if ((uintptr_t) uatomic_cmpxchg(&host->d_iparent,
						(struct dentry *) raw,
						(struct dentry *) want) == raw)
			break;
		/* lost the word to the fold's TRANSFER (new parent bits) or to
		 * another state change: re-read and re-decide */
	}
	host->d_inode = negative ? 0 : 1;	/* cold bookkeeping; the tag rules */
	return 0;
#else
	/* Legacy 3-CL layout: pos/neg is its OWN word (d_inode), which the fold
	 * never touches -- so this arm has no competing writer and one aligned
	 * store to a plainly-sampled word is enough.  Old-or-new, never a tear.
	 * It keeps d_seq too, so it is the arm closest to the baseline: it
	 * never needed anything here. */
	if ((!DC_IS_POSITIVE(host)) == (!!negative))
		return negative ? -ENOENT : -EEXIST;
	if (!negative) {
		host->d_id = id;
		cmm_smp_wmb();
	}
	CMM_STORE_SHARED(host->d_inode, negative ? 0 : 1);
	return 0;
#endif
}
#endif

int dc_instantiate(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	struct dentry *parent, *top, *host;
	const struct qstr *name;
	int ret = 0;

	if (path->ndepth == 0)
		return -EEXIST;			/* the root is always positive */

	rcu_read_lock();
	parent = resolve(dc, path, path->ndepth - 1);
	if (!parent) {
		rcu_read_unlock();
		return -ENOENT;
	}
	name = &path->comp[path->ndepth - 1];

	top = find_top_rcu(dc, parent, name);
	if (!top) {
		ret = -ENOENT;
		goto out;
	}
	host = host_of_rcu(top);
#ifdef DC_IPARENT_TXN
	{
		struct urcu_txn txn;

		urcu_txn_init(&txn, &dc->domain);
		for (;;) {
			enum urcu_txn_status st;

			urcu_txn_begin(&txn);
			ret = dc_set_negative_txn(&txn, host, 0,
						  host->d_isdir, id);
			if (ret) {
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				break;
			}
			st = urcu_txn_commit(&txn);
			urcu_txn_end(&txn);
			if (st == URCU_TXN_STATUS_ABORT)
				continue;	/* a racing add/fold: re-decide */
			if (st < 0)
				ret = -ENOMEM;
			break;
		}
	}
#else
	ret = dc_set_negative(host, 0, id);
#endif
out:
	rcu_read_unlock();
	return ret;
}

/*
 * Phase 2: d_delete with a surviving reference -- the kernel's "if the dentry
 * still has users, make it negative instead of unhashing it".  The exact
 * INVERSE of dc_instantiate, published the same way, and together they are the
 * state-change-in-place pair this engine did not previously have.
 *
 * It matters separately from dc_unlink because unlink-to-negative is the LARGER
 * source of negative dentries in a real dcache, and because it puts a live,
 * reachable node through a state change on a COMMON operation rather than on a
 * lookup miss.
 *
 * WALK CAUSALITY: still no bump -- but NOT for the reason dc_unlink gives, and
 * the difference is the interesting part.  dc_unlink argues "unlink REMOVES,
 * and the removed node is EMPTY, hence a TERMINAL a reader can only straddle at
 * the leaf".  Neither clause holds here: the node survives, hashed, in both
 * indexes, and a reader can hold it as an interior waypoint.
 *
 * What actually carries both proofs is weaker than "removed", and is the
 * property the two operations share: THE NODE'S LOCATION DOES NOT CHANGE.  A
 * bump is owed when a reader's stale PREFIX can be combined with a node's NEW
 * location to name a path that never existed -- which needs a relocation.
 * dc_delete leaves the host at the same (parent, name) it already had, so a
 * reader holding it mid-walk holds it exactly where it still is; anything it
 * subsequently finds beneath it is a real path at that time.  That is a LATE
 * reader, not a phantom, and late is a valid linearization.  Remove is the
 * degenerate case of the same rule, with the new location being "nowhere" --
 * so the honest statement of the rule is RELOCATION, not removal, and
 * dc_unlink's proof is a corollary rather than the general case.
 *
 * FILES ONLY (-EISDIR on a directory), and that restriction is load-bearing
 * rather than a simplification.  The invariant a negative must hold is that it
 * has no children and CANNOT GAIN ANY: a reader walking through a negative must
 * find nothing beneath it.  Checking children_empty here would not establish
 * that -- a concurrent dc_add could commit a child after the check and before
 * this commit, since the two touch different slots and so do not conflict.
 * Enforcing it would cost dc_add a read-set entry on the parent's state word,
 * on a hot path, to protect a rare operation.  Restricting to files gets it for
 * free and race-free instead: d_isdir is write-once at allocation and dc_add
 * already rejects a child under a file with -ENOTDIR.  So EVERY negative in
 * this engine is a file, by construction, at no cost to the common path.  It is
 * also the faithful scope -- unlink(2) is the non-directory call.  rmdir-to-
 * negative would need the atomic check above and is deliberately not here.
 */
int dc_delete(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *parent, *top, *host;
	const struct qstr *name;
	int ret = 0;

	if (path->ndepth == 0)
		return -EISDIR;			/* the root is a directory */

	rcu_read_lock();
	parent = resolve(dc, path, path->ndepth - 1);
	if (!parent) {
		rcu_read_unlock();
		return -ENOENT;
	}
	name = &path->comp[path->ndepth - 1];

	top = find_top_rcu(dc, parent, name);
	if (!top) {
		ret = -ENOENT;
		goto out;
	}
	host = host_of_rcu(top);
#ifndef DC_IPARENT_TXN
	if (host->d_isdir) {
		/*
		 * rmdir-to-negative is NOT implemented on this ARM, and the
		 * reason is structural rather than an omission (dc_delete_dir_
		 * supported == 0, see dcache.h).  A negative directory could
		 * gain a child, so children_empty must still hold at the flip
		 * -- and nothing this lock-free engine already holds spans the
		 * check and the flip.  The flip is a cmpxchg, not a commit,
		 * because the fold writes this same word non-transactionally,
		 * so it cannot just join a guarded transaction; making it one
		 * would force d_iparent MW-transacted on every arm and put a
		 * resolving read on the hottest field.  The alternative is a
		 * per-parent lock in dc_add.  Both are real costs the seqlock
		 * and bucketlock engines do not pay, so the choice is left to
		 * be made deliberately.
		 */
		ret = -ENOTSUP;
		goto out;
	}
#endif
	/*
	 * d_id is deliberately NOT cleared.  Clearing it after the flip would
	 * race a reader that latched the node positive and then read the id;
	 * clearing it before would expose 0 to a reader that still sees it
	 * positive.  Neither is needed: no reader reads a negative's id, and
	 * dc_instantiate rewrites it before republishing.  The census skips
	 * negatives for the same reason (walk_rec) -- it counts OBJECTS, and a
	 * negative holds a name, not an object.
	 */
#ifdef DC_IPARENT_TXN
	{
		struct urcu_txn txn;

		urcu_txn_init(&txn, &dc->domain);
		for (;;) {
			enum urcu_txn_status st;

			urcu_txn_begin(&txn);
			ret = dc_set_negative_txn(&txn, host, 1,
						  host->d_isdir, 0);
			if (ret) {
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				break;
			}
			st = urcu_txn_commit(&txn);
			urcu_txn_end(&txn);
			if (st == URCU_TXN_STATUS_ABORT)
				continue;	/* a racing add/fold: re-decide */
			if (st < 0)
				ret = -ENOMEM;
			break;
		}
	}
#else
	ret = dc_set_negative(host, 1, 0);
#endif
out:
	rcu_read_unlock();
	return ret;
}

static void dentry_free_cb(struct rcu_head *rh)
{
	struct dentry *d = caa_container_of(rh, struct dentry, d_rcu);

	/* -DDC_LRU_FREE_ASSERT; inert otherwise.  ⚠ THIS ENGINE HAD NO PROBE AT
	 * ALL until now -- the callback was a bare free() while the identical
	 * question was being asked, and answered, only in dcache_bucketlock.c.
	 * Every "txn ... 0 hits" figure taken before this line existed is a
	 * vacuous zero.  See lru_assert_not_queued() in dcache_lru.h. */
	lru_assert_not_queued(d);
	/* ...and the lifecycle side of the same moment: a dentry must not reach
	 * reclaim without having passed through the states.  The membership
	 * question is the line above; this one is "did it get here legally". */
	DC_LC_ASSERT(d, DC_LC_M(DC_LC_DEAD));
	free(d);
}

/*
 * UNLINK.  Remove the current named top from BOTH indexes so the entry is
 * immediately unreachable, and bump the walk-causality generation (all in one
 * commit) so a concurrent walker retries rather than straddling the removal.
 * Works whether or not a rename is mid-fold:
 *
 *   SETTLED (top == host, top->d_fwd == NULL): the named top IS the content host
 *   and has no fold queued, so unlink frees it directly after a grace period.
 *
 *   MID-TRANSITION (top is a rename shell, top->d_fwd != NULL): removing the top
 *   from the index without demoting it (d_back stays NULL) is exactly the signal
 *   the shell's pending fold reads as an unlink -- it then RECLAIMs the whole
 *   orphaned chain (see fold()).  Unlink must NOT free the shell (its fold does)
 *   nor the host (the reclaim cascade does).
 *
 * Loops re-finding the top: a concurrent fold that transfers (promotes the
 * successor into the index) between the find and the del makes our del -ENOENT,
 * so we re-find the new top and remove that instead.
 */
int dc_unlink(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *parent, *top, *host;
	const struct qstr *name;
	struct urcu_txn txn;
	int settled, ret, can_free;

	if (path->ndepth == 0)
		return -EINVAL;

	parent = resolve(dc, path, path->ndepth - 1);
	if (!parent)
		return -ENOENT;
	name = &path->comp[path->ndepth - 1];

	rcu_read_lock();			/* keeps top/host alive across the commit */
	urcu_txn_init(&txn, &dc->domain);
	/*
	 * The two dels touch DISTINCT chains (top's d_hash vs d_sib), distinct node
	 * fields, with no read-your-own-write -- a disjoint write set, so the fast
	 * path (blind-append, no RYW filter) applies, exactly like dc_add.  A
	 * CROSS-parent rename/exchange cannot use this: its loop check
	 * load_validates the d_parent chain it also reparents (read-your-own-write),
	 * forcing the default RYW handle in EVERY scheme.  (That is the scheme-
	 * independent reason -- distinct from the rename_gen aliasing, which is
	 * GLOBAL-scheme-only: DC_MARK_GEN has no gen at all.)  Unlink has neither, so
	 * declare_disjoint is valid and cheaper than the RYW handle it used before.
	 */
	urcu_txn_declare_disjoint(&txn);
	for (;;) {
		enum urcu_txn_status st;
		int p;

		top = find_top_rcu(dc, parent, name);
		if (!top) {
			ret = -ENOENT;
			goto out;
		}
		host = host_of_rcu(top);	/* O(1) */
		if (!children_empty(host)) {	/* children live on the host */
			ret = -ENOTEMPTY;
			goto out;
		}
		/* top == host iff top is settled (no forwarding chain below it) --
		 * host_of_rcu() already answered this; no d_fwd read needed. */
		settled = (top == host);

		urcu_txn_begin(&txn);
		/*
		 * NO walk-causality bump on unlink -- and this is exact, not an
		 * optimization that trades a corner.  The bump exists so a reader
		 * that straddled a relocation cannot report a path that never
		 * existed; the axis is REMOVE vs RELOCATE, not leaf vs directory.
		 * A rename RELOCATES a live, address-stable host that can gain a
		 * child at its new name, so a reader holding it mid-walk sees a
		 * phantom -- rename bumps (stack_shell).  Unlink REMOVES the host,
		 * and requires it be EMPTY (children_empty above), so the removed
		 * node is always a TERMINAL, never an interior waypoint: a reader
		 * can only straddle it at the leaf, where present (stale, before)
		 * and absent (after) are both valid linearizations, and a re-add
		 * is a DIFFERENT host the reader never latched.  No phantom is
		 * constructible, so no bump is owed.  (The del below still MARKS
		 * the node, which the localized reader's top_unhashed_rcu observes
		 * -- that detection is independent of the gen and remains.)
		 */
#ifdef DC_TXN_PARENT_SEAL
		/*
		 * SEAL the host's child head, for the same reason the shrinker
		 * does (see lru_evict_settled): children_empty(host) above is a
		 * plain RCU read, so on its own it is a check-then-act, and an
		 * add can commit between it and this commit -- leaving a child
		 * hashed under a directory this call is about to call_rcu.
		 *
		 * ⚠ ONLY WHEN SETTLED.  When top != host the host is NOT freed
		 * here (a pending fold reclaims the chain), so sealing it would
		 * refuse children to a directory that is still alive.
		 */
		if (settled &&
		    urcu_txn_store_mw(&txn, (void **) &host->d_child_head.first,
				      NULL, urcu_txn_hlist_set_mark(NULL),
				      URCU_TXN_HLIST_TAG)) {
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			ret = -ENOMEM;
			goto out;
		}
#endif
		p = urcu_txn_hlist_del_prepare(&txn, &top->d_hash);
		if (!p)
			p = urcu_txn_hlist_del_prepare(&txn, &top->d_sib);
		if (p) {			/* -ENOENT: top changed; -EAGAIN */
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;		/* re-find the current top */
		}
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		if (st < 0) {			/* -ENOMEM */
			ret = st;
			goto out;
		}
		break;				/* removed from both indexes */
	}
	/* PHASE 3: off the LRU IMMEDIATELY, never lazily -- the call_rcu free
	 * below cannot fire while a shard still points at the node, so deferring
	 * this to the shrinker would gate reclaim on memory pressure instead of
	 * on the grace period (design/dcache-lru-txn.md section 6).
	 *
	 * ⭐ ... EXCEPT when the shrinker is already holding @host, in which case
	 * the free is DELEGATED, not taken: lru_del_can_free() answers 0 and the
	 * shrinker call_rcu's it.  Mainline __dentry_kill's `can_free = false`
	 * for a dentry on a shrink list; disowning it here instead is the
	 * free-while-queued defect.  (LOCK arm only -- the MCAS arm's deque
	 * supplies the same ownership from inside the commit and always
	 * answers 1.) */
	DC_LC_SET(top, DC_LC_M(DC_LC_NEW) | DC_LC_M(DC_LC_LIVE), DC_LC_DYING);
	can_free = lru_del_can_free(dc, host, settled);
	if (top != host)
		lru_del(dc, top);
	rcu_read_unlock();
	if (settled && can_free) {		/* host has no fold queued: free it */
		DC_LC_TO_DEAD(top);		/* R5: must be off the LRU by now */
		call_rcu(&top->d_rcu, dentry_free_cb);
	}
	/* else: top is a shell; its pending fold RECLAIMs the orphaned chain */
	return 0;
out:
	/* the retry loop may have exited while holding the lane */
	dc_lane_giveback(&txn);
	rcu_read_unlock();
	return ret;
}

/* ---- the shell-vehicle move (async stack + fold) ----------------------- */

#define DC_LOOP_MAX 256		/* ancestry-walk cap: a transient cycle from an
				 * in-flight concurrent reparent -> re-walk */

/*
 * STACK one entry into an ALREADY-OPEN txn (records only -- no gen bump, no
 * commit).  Removes @top from BOTH indexes and inserts @shell -- the new named
 * top, which must already forward to @top (shell->d_fwd = top) -- into
 * @new_bucket + @new_parent's child-hlist, demoting @top (d_back = shell)
 * atomically with its removal, so a fold worker always reads a d_back consistent
 * with whether @top is still indexed.
 *
 * When @cross_parent, the same records also (a) validate the loop check -- walk
 * new_parent -> root over the TRANSACTED d_parent chain via
 * urcu_txn_load_validate(), -EINVAL if @host appears (moving a dir under its own
 * descendant) -- and (b) store host->d_parent = new_parent.  Folding the walk
 * into the validate set makes cycle prevention ATOMIC with the move: a
 * concurrent reparent of any ancestor mutates a validated edge and aborts us, so
 * two moves that would jointly form a cycle cannot both commit.
 *
 * The caller owns urcu_txn_begin/commit/end, the walk-causality gen bump and the
 * retry loop, so two entries can share ONE commit (the atomic exchange).
 * Returns 0, -ENOENT/-EAGAIN (a link moved: caller aborts, re-finds, retries),
 * or -EINVAL (the move would create a directory cycle).
 */
static int stack_one_prepare(struct urcu_txn *txn, struct dcache *dc,
			     struct dentry *top, struct dentry *host,
			     struct dentry *new_parent,
			     struct urcu_txn_hlist_head *new_bucket,
			     struct dentry *shell, int cross_parent,
			     struct dentry *expect_dest)
{
	int p;

	/*
	 * The root anchors every path, so relocating it under ANY destination
	 * makes it its own descendant -- a guaranteed cycle regardless of where
	 * it lands, so no walk is needed.  This is also load-bearing, not just a
	 * shortcut: the ancestry walk below is bounded `cur != dc->root` and so
	 * exits BEFORE its `cur == host` test could fire on the root, so a
	 * root-as-host would slip through uncaught.  Reject up front, before
	 * recording anything.  (Unreachable in normal use -- the root is never a
	 * named entry -- but cheap insurance against a caller that builds one.)
	 */
	if (cross_parent && host == dc->root)
		return -EINVAL;

#ifdef DC_IPARENT_TXN
	/*
	 * Same guard dc_add carries, for the OTHER way a directory gains a
	 * child: this commit inserts the shell into new_parent's child list, so
	 * a concurrent d_delete of new_parent must conflict with it.
	 *
	 * ONLY the guard belongs here.  A permanent refusal must NOT be returned
	 * from this function -- a non-zero return means "retry" to the caller,
	 * so refusing a negative parent here spins forever (the parent stays
	 * negative until someone instantiates it).  The caller makes that
	 * decision; this just makes the two commits conflict, after which the
	 * caller re-reads and refuses.
	 */
	urcu_txn_validate(txn, (void **) &new_parent->d_iparent,
			  (void *) iparent_raw(new_parent), DC_IPARENT_TAG);
#endif
	p = urcu_txn_hlist_del_prepare(txn, &top->d_hash);
	if (p)					/* -ENOENT: top demoted; -EAGAIN */
		return p;
#ifndef DC_NO_RENAME_DEST_RECHECK
	/*
	 * ⭐ RE-CHECK THE DESTINATION NAME INSIDE THE TRANSACTION.  dc_rename's
	 * __child_lookup(to_parent, to_name) is a CHECK-THEN-ACT holding nothing
	 * -- the same shape dc_add had -- so two renames to one destination, or
	 * a rename racing a dc_add of that name, both pass it and both publish,
	 * leaving TWO dentries spelled alike in this bucket.
	 *
	 * ⚠ dc_add's own re-check does NOT cover this: it makes the ADD notice a
	 * publish, and does nothing to make the RENAME notice one.  The pair was
	 * closed in one direction only.
	 *
	 * Same construction as dc_add: read the head ONCE, scan for the name
	 * AFTER that read, and hand that SAME value to the insert as its
	 * expected old.  A peer that published earlier is seen by the scan; a
	 * peer that publishes later changes the head, so the install's old-value
	 * check fails and the retry re-scans.  Opening up insert_head_prepare()
	 * is the point -- it does its own load, and a scan placed before that
	 * load only NARROWS the window.
	 *
	 * ⛔⭐⭐ @expect_dest IS LOAD-BEARING, and omitting it was a LIVELOCK, not
	 * a missing check.  An EXCHANGE moves A onto B's name and B onto A's, so
	 * the destination name is OCCUPIED BY DESIGN -- by the counterpart.  A
	 * bare "the name exists -> -EEXIST" therefore fires on every exchange,
	 * every retry, for ever; dc_rename_exchange treats only -EINVAL as
	 * terminal, so it span in conflict-and-continue inside the escalation
	 * lane and parked every other writer behind it (deterministic hang in
	 * test_midtransition).  So the question is not "is the name taken" but
	 * "is it taken by someone OTHER than the entry I am swapping with":
	 * @expect_dest is NULL for a rename (any occupant is a duplicate) and the
	 * counterpart's current top for an exchange.
	 */
	{
		void *fn = urcu_txn_load(txn, (void **) &new_bucket->first,
					 URCU_TXN_HLIST_TAG);
		struct dentry *cur;

		if (urcu_txn_hlist_is_marked(fn))
			return -ENOENT;		/* destination bucket sealed */
		cur = find_top_rcu(dc, new_parent, &shell->d_iname);
		if (cur && cur != expect_dest)
			return -EEXIST;		/* the name appeared under us */
		p = urcu_txn_hlist_insert_at_slot_prepare(txn, &shell->d_hash,
				&new_bucket->first,
				(struct urcu_txn_hlist_node *) fn);
	}
#else
	p = urcu_txn_hlist_insert_head_prepare(txn, &shell->d_hash, new_bucket);
#endif
	if (p)
		return p;
	p = urcu_txn_hlist_del_prepare(txn, &top->d_sib);
	if (p)
		return p;
	p = urcu_txn_hlist_insert_head_prepare(txn, &shell->d_sib,
					       &new_parent->d_child_head);
	if (p)
		return p;
	/* Demote the old top atomically with its removal (d_back: NULL -> shell). */
	(void) urcu_txn_store_mw(txn, (void **) &top->d_back, NULL, shell,
			      DC_FWD_TAG);
	if (cross_parent) {
		void *oldp = urcu_txn_load(txn, (void **) &host->d_parent,
					   DC_PARENT_TAG);
		struct dentry *cur = new_parent;
		int hops = 0;

		/*
		 * Cross-dir cycle check via the move-in-progress flag (struct
		 * dentry d_moving).  Moving @host under @new_parent loops iff @host
		 * is an ancestor of @new_parent, so walk @new_parent -> root looking
		 * for @host.  The reads are PLAIN resolving loads (parent_of_rcu),
		 * NOT the proxy-installing load_validate that pinned the shared
		 * spine (perf c2c: those proxy writes were the cross-CCD ping-pong).
		 * Concurrency safety comes from the flag instead: the caller set
		 * host->d_moving with a fenced RMW BEFORE this walk, and if any
		 * ancestor carries the flag a concurrent move is in flight on this
		 * ancestry -> retry.  Dekker set-before-check: two moves that would
		 * jointly form a cycle cannot both pass, because at least one sees
		 * the other's flag (else the happens-before order is cyclic).  A
		 * committed concurrent move is instead caught by the plain read of
		 * its new edge + the cur == host test.  The reparent stays transacted.
		 */
		while (cur != dc->root) {
			if (cur == host)		/* committed ancestry cycle */
				return -EINVAL;
			if (uatomic_load(&cur->d_moving, CMM_RELAXED))
				return -EAGAIN;		/* concurrent move on the ancestry */
			if (++hops > DC_LOOP_MAX)
				return -EAGAIN;		/* transient cycle: re-walk */
			cur = parent_of_rcu(cur);	/* plain resolving load, no validate */
		}
		(void) urcu_txn_store_mw(txn, (void **) &host->d_parent, oldp,
				      new_parent, DC_PARENT_TAG);
	}
	return 0;
}

/*
 * STACK.  Move the entry named (@from_parent, @from_name) so it becomes named
 * (@new_parent, @new_name), preserving its content host (children key on the
 * host's address, so they never rehash).  ONE MCAS commit stacks a fresh shell
 * (stack_one_prepare) and bumps the walk-causality generation, so a concurrent
 * walker's dc_lookup bracket sees the whole move atomically.  Skips age-0 only on
 * the GLOBAL arm, whose shared rename_gen every rename bumps (a hot slot that
 * dooms the fast install) -- DC_RENAME_CONFLICT_HINT; per-node/mark keep age-0.
 * The entry can never del+insert its OWN links -- the shell carries the new name.
 * Compression (fold) is deferred to a call_rcu worker (see fold()); this returns
 * as soon as the entry is reachable under its new name.  When @cross_parent, the
 * loop check + d_parent reparent ride the same commit.
 *
 * Returns 0 (shell in *out_shell, host in *out_host), -ENOMEM, -ENOENT (the
 * entry vanished), or -EINVAL (the move would create a directory cycle).  Loops
 * internally, re-finding the top, so a concurrent fold that demotes the top
 * between attempts is retried rather than lost.
 */
static int stack_shell(struct dcache *dc,
		struct dentry *from_parent, const struct qstr *from_name,
		struct dentry *new_parent, const struct qstr *new_name,
		int cross_parent,
		struct dentry **out_shell, struct dentry **out_host)
{
	struct urcu_txn_hlist_head *new_bucket =
		bucket_of(dc, new_parent, new_name->hash);
	struct dentry *shell = dentry_alloc(dc, new_parent, new_name, 0, 0, 1);
	struct dentry *top = NULL, *host = NULL;
	struct urcu_txn txn;
	int ret;

	if (!shell)
		return -ENOMEM;
#ifdef DC_HOT1CL
	shell->d_iparent = (struct dentry *)
		((uintptr_t) shell->d_iparent | DC_TAG_SHELL);
#endif

	rcu_read_lock();			/* keeps top/host alive across commits */
	urcu_txn_init(&txn, &dc->domain);
	for (;;) {
		enum urcu_txn_status st;
		int p;

		/*
		 * begin() FIRST, so the whole attempt -- lookup included -- is
		 * bracketed by one txn.  This is what makes every terminal exit below
		 * able to release the escalation lane: once a prior attempt escalated
		 * (retry kept the FIFO turn), a bail that skipped begin/end would carry
		 * the domain's fair mutex out of the function and stall every writer
		 * (see urcu_txn_abandon).  Each terminal path here pairs abandon+end.
		 */
		urcu_txn_begin(&txn);
#ifdef DC_IPARENT_TXN
		/* A NEGATIVE destination directory refuses the move outright --
		 * the decision, as opposed to the guard inside the commit. */
		if (iparent_raw(new_parent) & DC_TAG_NEG) {
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			ret = -ENOENT;
			goto out_free;
		}
#endif
		DC_RENAME_CONFLICT_HINT(&txn);

		top = find_top_rcu(dc, from_parent, from_name);
		if (!top) {			/* concurrently removed (deferred case) */
			urcu_txn_abandon(&txn);	/* forfeit the turn -> end() releases the lane */
			urcu_txn_end(&txn);
			ret = -ENOENT;
			goto out_free;
		}
		host = host_of_rcu(top);	/* O(1): write-once d_host skip pointer */
		shell->d_host = host;		/* union slot = skip pointer to the tail host */
		shell->d_fwd = top;		/* new top forwards to the old top */

		/*
		 * ACQUIRE exclusive move-ownership of this host, AFTER begin but
		 * BEFORE the ancestry walk (Dekker set-before-check).
		 *
		 * test-and-set, not a plain OR: two writers must not both drive a move
		 * of the SAME node -- a plain OR lets the second in, and the first's
		 * clear then wipes the second's flag, so a third writer walks the
		 * still-moving node unflagged and misses a cycle.  A loser retries;
		 * only the owner clears.  The cmpxchg is a full barrier -> the
		 * StoreLoad fence the flag walk needs.  Held until the reparent commits
		 * (or aborts), so a concurrent move sees either the flag or the
		 * committed edge, never a gap.
		 *
		 * AFTER begin is load-bearing: when contention escalates a writer into
		 * the domain's fallback lane, urcu_txn_begin PARKS it on the fair
		 * mutex.  Were the flag taken before begin, a parked writer would hold
		 * its flag while blocked, and the one writer running under the mutex
		 * would find those parked flags on its walk and back off forever (a
		 * livelock that presents as one-running/many-parked).  Taken after
		 * begin, a parked writer holds no flag, so the running writer's walk is
		 * clean and the escalation serializes moves to real progress.
		 *
		 * Same-parent moves take no ancestry walk, so they need no flag.
		 */
		if (cross_parent &&
		    uatomic_cmpxchg(&host->d_moving, 0UL, 1UL) != 0UL) {
			urcu_txn_abandon(&txn);	/* forfeit the turn -> end() releases the lane */
			urcu_txn_end(&txn);
			continue;		/* another writer owns this host's move */
		}
		/*
		 * Bump only when the moved entry is a DIRECTORY.  A file is never
		 * an interior waypoint (no children, enforced), so its rename can
		 * misdirect no reader -- same exemption as unlink.  Files are the
		 * common case; skipping their bump is what keeps the global arm
		 * viable on file rename/move (a whole-tree bump per file rename
		 * would disrupt every reader).  Per-node's bump is already
		 * localized and the mark arm's is a no-op, so both are unaffected;
		 * the gate matters for global.
		 */
		if (host->d_isdir)
			txn_bump_gen(&txn, dc, host);	/* dir move: host gen */
		p = stack_one_prepare(&txn, dc, top, host, new_parent, new_bucket,
				      shell, cross_parent, NULL);
		if (p) {
			/*
			 * ⚠ -EEXIST IS TERMINAL, and getting that wrong is a
			 * LIVELOCK not a wrong answer: the destination name is
			 * still there on every retry, so falling into the
			 * conflict-and-continue path below would spin for ever
			 * (inside the escalation lane, parking every other
			 * writer behind it).  Same shape as -EINVAL.
			 */
			if (p == -EINVAL || p == -EEXIST) {
				urcu_txn_abandon(&txn);	/* forfeit turn -> end() releases lane */
				urcu_txn_end(&txn);
				if (cross_parent)
					uatomic_and(&host->d_moving, ~1UL);
				ret = p;
				goto out_free;
			}
			urcu_txn_conflict(&txn);	/* -ENOENT/-EAGAIN: keep the turn */
			urcu_txn_end(&txn);
			if (cross_parent)	/* release the flag before retry */
				uatomic_and(&host->d_moving, ~1UL);
			continue;		/* re-find + retry */
		}
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (cross_parent)		/* reparent committed (or aborted): clear */
			uatomic_and(&host->d_moving, ~1UL);
		if (st == URCU_TXN_STATUS_ABORT)
			continue;		/* a neighbour changed: re-find + retry */
		if (st < 0) {			/* -ENOMEM */
			ret = -ENOMEM;
			goto out_free;
		}
		break;				/* committed: entry now named anew */
	}
	/*
	 * Relief valve: if the host-walk above found the chain already deep (the
	 * async fold worker is behind -- GPs stalled), splice its middle in-line
	 * so the chain stays bounded.  @depth is the pre-stack length; the new
	 * shell makes it depth + 1, so trip at depth >= HI.  Still under the RCU
	 * read lock; reuses the walk we already paid for.
	 */
	rcu_read_unlock();
	*out_shell = shell;
	if (out_host)
		*out_host = host;
	return 0;
out_free:
	/* the retry loop may have exited while holding the lane */
	dc_lane_giveback(&txn);
	rcu_read_unlock();
	free(shell);
	return ret;
}

/*
 * FOLD.  Compress shell @n out of its transition chain by exactly one hop.
 * Runs from a call_rcu callback a grace period after @n was stacked, so any
 * reader that observed @n's OLD sibling threading (its d_sib in the previous
 * directory) has drained -- which is what lets the transfer re-link the child
 * list without a concurrent readdir jumping directories.  The branch is
 * re-decided every attempt, because a concurrent re-rename can demote @n from
 * top to middle relay between attempts:
 *
 *   @n is still the named top (d_back == NULL): TRANSFER.  Copy @n's identity
 *   one hop down into m = @n->d_fwd, atomically replace @n by m in BOTH indexes,
 *   and promote m (m->d_back = NULL).  m takes @n's place; if m is itself a
 *   relay, its own fold continues the compression toward the host.  The
 *   identity copy into m is safe pre-publish: m is in no index until the
 *   replace, and readers reaching m through the chain read only its d_id.
 *
 *   @n was demoted to a middle relay (d_back != NULL): SPLICE.  @n is in NO
 *   index (the stack that demoted it removed it); rewire the doubly linked chain
 *   past @n (back->d_fwd = fwd, fwd->d_back = back) in one MCAS, touching no
 *   index.  A reader positioned at @n still follows @n->d_fwd (untouched) to the
 *   host until @n is reclaimed.  @n is always still linked here: nothing splices a
 *   chain out-of-band, so @n's own fold is the only thing that removes it.
 *
 *   @n is the top but GONE from the index (the TRANSFER's replace returns -ENOENT
 *   while d_back is STILL NULL): an unlink removed the named top without demoting
 *   it (a re-rename demotes AND sets d_back in one commit, so a still-NULL d_back
 *   rules that out).  RECLAIM: dismantle the orphaned chain from @n down.  Detach
 *   @n (store @n->d_fwd = NULL -- which conflicts with a concurrent SPLICE of the
 *   successor so the two cannot commit inconsistently) and promote the successor
 *   m WITHOUT re-indexing, so m stays out of every index and its own fold
 *   reclaims in turn; the content host at the tail (m->d_fwd == NULL) has no fold
 *   queued, so whoever reaches it frees it here.
 *
 * @n is then reclaimed after a further grace period.  Self-free: each shell is
 * folded exactly once, by the fold its own rename queued, so no double free.
 */
static void fold(struct dcache *dc, struct dentry *n)
{
	struct dentry *host_to_free = NULL;
	struct urcu_txn txn;
#ifdef DC_IPARENT_TXN
	struct dentry *xfer_from = NULL;
#endif

	rcu_read_lock();
	urcu_txn_init(&txn, &dc->domain);
	for (;;) {
		struct dentry *back, *fwd;
		enum urcu_txn_status st;
		int p;

		DC_DBG_FOLD_ATTEMPT();
		back = urcu_txn_read((void **) &n->d_back, DC_FWD_TAG);
		fwd = urcu_txn_read((void **) &n->d_fwd, DC_FWD_TAG);

		if (back == NULL) {
			/* TRANSFER: @n is the top; pull identity down into m.
			 * m is reachable by concurrent readers (as the content
			 * host, via the d_host skip pointer).  The d_iname copy
			 * still races no reader -- no reader reads a host's name;
			 * matching uses the write-once TOP and readdir reads the
			 * top's d_iname + host's d_id -- and the name guard
			 * (-DDC_DEBUG_NAME_GUARD) is what holds that invariant.
			 * d_iparent is a different story since phase 2: readers
			 * take pos/neg off the host, and d_delete WRITES it from
			 * another thread, so the handover below is an atomic RMW. */
			struct dentry *m = fwd;
#if   defined(DC_HOT1CL)
			/*
			 * Adopt n's PARENT; keep m's own host/shell bit (m may
			 * still be a middle relay of the remaining chain) AND its
			 * own pos/neg.
			 *
			 * pos/neg is preserved, not adopted, because phase 2 made
			 * the HOST authoritative for it: the fold walks identity
			 * DOWN toward the host, so adopting n's bit here would let
			 * a name-carrying node overwrite the content node's own
			 * state -- and d_delete/d_instantiate write the host, so
			 * the top's copy is exactly the one that can be stale.
			 *
			 * ATOMIC read-modify-write, and this is load-bearing.
			 * Until phase 2 the fold was the ONLY writer of a host's
			 * d_iparent, so a plain store was safe -- d0e7955 says so
			 * in as many words, and predicted its own expiry: "benign
			 * today only because rename preserves inode-ness ... but
			 * it is UB and a latent correctness bug once phase-2
			 * negative dentries land."  It landed.  d_delete and
			 * d_instantiate now write this same word from another
			 * thread, so a plain read-then-store LOSES one: the fold
			 * reads m positive, a concurrent d_delete publishes
			 * NEGATIVE, the fold stores back the positive bit it read
			 * and the delete is gone with no error anywhere.
			 *
			 * A transaction does NOT fix it, which is the part worth
			 * recording.  urcu_txn_store_sw() "parks it with a plain
			 * store that never fails" and an SW-only commit never
			 * contention-aborts -- SW is a PROMISE of exclusion, and
			 * the fold breaks that promise.  store_mw() would work,
			 * but only where the slot is transacted: it installs a
			 * descriptor, so every reader must resolve it, and the
			 * global and per-node arms deliberately do not (d0e7955
			 * rejected transacting the hottest field to fix one cold
			 * read).  A cmpxchg is correct on EVERY arm and costs the
			 * reader nothing -- it is the same plain load it already
			 * did.  The three writers of this word therefore all use
			 * atomics on it and no update can be lost.
			 */
#ifdef DC_IPARENT_TXN
			/* The handover is PUBLISHED BY THE COMMIT (recorded
			 * below, after urcu_txn_begin) rather than stored here.
			 * That is what lets d_delete/d_instantiate guard this
			 * slot from their own transactions -- a cmpxchg here
			 * could not be conflicted against a descriptor install.
			 * The comment at the top of this file has claimed this
			 * since DC_IPARENT_TXN was introduced; it is now true. */
			xfer_from = n;
#else
			{
				uintptr_t nam = (uintptr_t) n->d_iparent &
					~(DC_TAG_SHELL | DC_TAG_NEG);
				uintptr_t own, want;

				for (;;) {
					own = (uintptr_t) uatomic_load(
						&m->d_iparent, CMM_RELAXED);
					DC_TEST_TRANSFER_HOOK();
					want = nam | (own & (DC_TAG_SHELL |
							     DC_TAG_NEG));
					if ((uintptr_t) uatomic_cmpxchg(
							&m->d_iparent,
							(struct dentry *) own,
							(struct dentry *) want)
					    == own)
						break;
				}
			}
#endif
#else
			m->d_iparent = n->d_iparent;	/* pre-publish: m unindexed */
#endif
			DC_NAME_XFER_BEGIN(m);
			m->d_iname = n->d_iname;
			DC_NAME_XFER_END(m);

			urcu_txn_begin(&txn);
			DC_FOLD_CONFLICT_HINT(&txn);
#ifdef DC_IPARENT_TXN
			{	/* identity handover, as an MW record */
				uintptr_t nam = iparent_raw(xfer_from) &
					~(DC_TAG_SHELL | DC_TAG_NEG);
				uintptr_t own = iparent_raw(m);

				DC_TEST_TRANSFER_HOOK();
				if (urcu_txn_store_mw(&txn,
						(void **) &m->d_iparent,
						(void *) own,
						(void *) (nam | (own &
						  (DC_TAG_SHELL | DC_TAG_NEG))),
						DC_IPARENT_TAG))
					goto transfer_retry;
			}
#endif
			p = urcu_txn_hlist_replace_prepare(&txn, &n->d_hash,
							   &m->d_hash);
			if (p == -ENOENT) {
				/* @n out of the index; a still-NULL d_back means an
				 * unlink removed it (a re-rename would have set
				 * d_back) -> tear the orphaned chain down. */
				int orphan;

				/* ⚠ DECIDE BEFORE ENDING.  One of these two exits
				 * re-attempts and one does not, so which of
				 * conflict/abandon is owed depends on the branch --
				 * taking the decision after end() is what left the
				 * `goto reclaim` side holding the lane for ever. */
				orphan = urcu_txn_read((void **) &n->d_back,
						       DC_FWD_TAG) == NULL;
				if (orphan)
					DC_LANE_GIVE_UP(&txn);	/* terminal */
				else
					urcu_txn_conflict(&txn); /* re-attempting */
				urcu_txn_end(&txn);
				if (orphan)
					goto reclaim;
				continue;	/* re-rename demoted @n: re-read -> SPLICE */
			}
			if (p)			/* -EAGAIN: a neighbour is mid-op */
				goto transfer_retry;
			p = urcu_txn_hlist_replace_prepare(&txn, &n->d_sib,
							   &m->d_sib);
			if (p)
				goto transfer_retry;
			/* promote m atomically with the index swap */
			(void) urcu_txn_store_mw(&txn, (void **) &m->d_back, n, NULL,
					      DC_FWD_TAG);
			st = urcu_txn_commit(&txn);
			urcu_txn_end(&txn);
			if (st == URCU_TXN_STATUS_ABORT) {
				DC_DBG_FOLD_ABORT();
				continue;
			}
			break;
transfer_retry:
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;		/* re-read d_back: may now be a relay */
		} else {
			/* SPLICE: @n is a middle relay, in no index.  Nothing
			 * splices a chain out-of-band any more (the synchronous
			 * fold-ahead is retired), so @n is necessarily still linked
			 * and @back is a live chain node -- safe to dereference. */
			urcu_txn_begin(&txn);
			DC_FOLD_CONFLICT_HINT(&txn);
			(void) urcu_txn_store_mw(&txn, (void **) &back->d_fwd, n,
					      fwd, DC_FWD_TAG);
			(void) urcu_txn_store_mw(&txn, (void **) &fwd->d_back, n,
					      back, DC_FWD_TAG);
			st = urcu_txn_commit(&txn);
			urcu_txn_end(&txn);
			if (st == URCU_TXN_STATUS_ABORT) {
				DC_DBG_FOLD_ABORT();
				continue;
			}
			break;
		}
	}
	goto done;

reclaim:
	/*
	 * @n is an orphan top: d_back == NULL and @n is in no index because an
	 * unlink removed the named top without demoting it.  Detach @n and promote
	 * its successor m in ONE commit; storing @n->d_fwd = NULL conflicts with a
	 * concurrent SPLICE of m (which stores @n->d_fwd), so they serialize.  m
	 * either continues the cascade (its own fold reclaims) or is the content
	 * host (m->d_fwd == NULL, no fold queued) and is freed here.
	 */
	for (;;) {
		struct dentry *m = urcu_txn_read((void **) &n->d_fwd, DC_FWD_TAG);
		enum urcu_txn_status st;

		DC_DBG_FOLD_ATTEMPT();
		host_to_free = urcu_txn_read((void **) &m->d_fwd,
					      DC_FWD_TAG) == NULL ? m : NULL;
		urcu_txn_begin(&txn);
		DC_FOLD_CONFLICT_HINT(&txn);
		(void) urcu_txn_store_mw(&txn, (void **) &n->d_fwd, m, NULL,
				      DC_FWD_TAG);	/* detach; conflicts w/ a splice of m */
		(void) urcu_txn_store_mw(&txn, (void **) &m->d_back, n, NULL,
				      DC_FWD_TAG);	/* promote m (harmless if host) */
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (st == URCU_TXN_STATUS_ABORT) {
			DC_DBG_FOLD_ABORT();
			continue;		/* n->d_fwd moved (splice) or m->d_back changed */
		}
		break;
	}
	/* ⭐ OFF THE LRU BEFORE THE FREE; see the bucketlock fold for why the
	 * fold frees hosts and why hosts are on the LRU. */
	if (host_to_free) {
		(void) lru_del_can_free(dc, host_to_free, 1);
		DC_LC_TO_DEAD(host_to_free);	/* R5 */
		call_rcu(&host_to_free->d_rcu, dentry_free_cb);
	}
done:
	rcu_read_unlock();
#ifdef DC_STRESS_DEBUG
	uatomic_inc(&dc_dbg_folds);
#endif
	(void) lru_del_can_free(dc, n, 1);
	DC_LC_TO_DEAD(n);			/* R5 */
	call_rcu(&n->d_rcu, dentry_free_cb);	/* reclaim @n after a GP */
}

static void fold_cb(struct rcu_head *rh)
{
	struct dentry *n = caa_container_of(rh, struct dentry, d_rcu);

	fold(n->d_dc, n);
}

int dc_rename(struct dcache *dc, const struct dc_path *from,
	      const struct dc_path *to)
{
	struct dentry *from_parent, *to_parent, *victim, *host, *shell;
	const struct qstr *from_name, *to_name;
	int cross, ret;

	if (from->ndepth == 0 || to->ndepth == 0)
		return -EINVAL;

	from_parent = resolve(dc, from, from->ndepth - 1);
	if (!from_parent)
		return -ENOENT;
	from_name = &from->comp[from->ndepth - 1];
	victim = __child_lookup(dc, from_parent, from_name);
	if (!victim)
		return -ENOENT;
	to_parent = resolve(dc, to, to->ndepth - 1);
	if (!to_parent)
		return -ENOENT;
	to_name = &to->comp[to->ndepth - 1];
	if (__child_lookup(dc, to_parent, to_name))
		return -EEXIST;

	rcu_read_lock();
	cross = parent_of_rcu(victim) != to_parent;
	rcu_read_unlock();

	/*
	 * Cross-parent: the loop check (reject moving a dir under its own
	 * descendant) and the d_parent reparent are folded into the stack commit,
	 * so cycle prevention is ATOMIC with the move (stack_shell).  Same-parent
	 * is a pure rename -- d_parent unchanged, no ancestry walk.
	 */
	ret = stack_shell(dc, from_parent, from_name, to_parent, to_name,
			  cross, &shell, &host);
	if (ret)
		return ret;		/* -ENOMEM / -ENOENT / -EINVAL (loop) */

#ifdef DC_TEST_HOOKS
	if (dc_test_fold_hook)			/* repro pauses here, post-stack */
		dc_test_fold_hook();
#endif
#ifdef DC_STRESS_DEBUG
	uatomic_inc(&dc_dbg_renames);
#endif
	call_rcu(&shell->d_rcu, fold_cb);	/* fold-ahead already ran in stack_shell */
	return 0;
}

/*
 * EXCHANGE.  Atomically swap the entries named (pa, na) and (pb, nb): A moves to
 * B's slot, B moves to A's slot.  Both shell stacks (stack_one_prepare) ride ONE
 * MCAS commit -- both index del/insert pairs, both demotes, both reparents, the
 * gen bumps -- so no concurrent walker ever observes only half the swap.
 *
 * A cycle-forming exchange (one host an ancestor of the other) is rejected
 * -EINVAL by the SAME move-in-progress flag protocol the single move uses
 * (stack_one_prepare): each move's plain-read walk over the CURRENT tree finds
 * its own host on its new_parent -> root path (cur == host).  This needs no
 * read-your-own-writes, because a swap cycles iff one host is ALREADY an ancestor
 * of the other in the pre-swap tree -- "A under B" is caught by B's walk, "B
 * under A" by A's.  Concurrent movers that would jointly cycle are excluded by
 * the flags: BOTH hosts are grayed (address-ordered cmpxchg) before either walk,
 * so a peer's overlapping walk sees a flag (Dekker).  Two folds are queued, one
 * per shell.
 *
 * Returns 0, -ENOENT (an entry vanished), -EINVAL (the swap would create a
 * directory cycle), or -ENOMEM.  (-EEXIST cannot arise: each freed name is
 * re-taken by the other entry's shell in the same commit.)
 */
int dc_rename_exchange(struct dcache *dc, const struct dc_path *ap,
		       const struct dc_path *bp)
{
	struct dentry *pa, *pb, *hosta, *hostb, *sa, *sb;
	struct urcu_txn_hlist_head *bucket_a, *bucket_b;
	const struct qstr *na, *nb;
	struct urcu_txn txn;
	int cross, ret;

	if (ap->ndepth == 0 || bp->ndepth == 0)
		return -EINVAL;

	pa = resolve(dc, ap, ap->ndepth - 1);
	if (!pa)
		return -ENOENT;
	na = &ap->comp[ap->ndepth - 1];
	pb = resolve(dc, bp, bp->ndepth - 1);
	if (!pb)
		return -ENOENT;
	nb = &bp->comp[bp->ndepth - 1];

	rcu_read_lock();
	hosta = txn_child_lookup_rcu(dc, pa, na);
	hostb = txn_child_lookup_rcu(dc, pb, nb);
	rcu_read_unlock();
	if (!hosta || !hostb)
		return -ENOENT;
	if (hosta == hostb)			/* same entry: exchange is a no-op */
		return 0;

	cross = pa != pb;
	bucket_a = bucket_of(dc, pa, na->hash);
	bucket_b = bucket_of(dc, pb, nb->hash);
	sa = dentry_alloc(dc, pb, nb, 0, 0, 1); /* A's new top */
	sb = dentry_alloc(dc, pa, na, 0, 0, 1); /* B's new top */
	if (!sa || !sb) {
		free(sa);
		free(sb);
		return -ENOMEM;
	}
#ifdef DC_HOT1CL
	sa->d_iparent = (struct dentry *) ((uintptr_t) sa->d_iparent | DC_TAG_SHELL);
	sb->d_iparent = (struct dentry *) ((uintptr_t) sb->d_iparent | DC_TAG_SHELL);
#endif

	rcu_read_lock();			/* keeps tops/hosts alive across commits */
	urcu_txn_init(&txn, &dc->domain);
	for (;;) {
		struct dentry *topa, *topb;
		enum urcu_txn_status st;
		int p;

		/* begin FIRST so every terminal bail can release the escalation
		 * lane (abandon+end); see stack_shell. */
		urcu_txn_begin(&txn);
		DC_RENAME_CONFLICT_HINT(&txn);

		topa = find_top_rcu(dc, pa, na);
		topb = find_top_rcu(dc, pb, nb);
		if (!topa || !topb) {		/* an entry vanished */
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			ret = -ENOENT;
			goto out_free;
		}
		hosta = host_of_rcu(topa);	/* O(1) */
		hostb = host_of_rcu(topb);	/* O(1) */
		sa->d_host = hosta;		/* union slot: A's new top -> A's tail host */
		sa->d_fwd = topa;
		sb->d_host = hostb;		/* union slot: B's new top -> B's tail host */
		sb->d_fwd = topb;

		/*
		 * ACQUIRE move-ownership of BOTH hosts before either walk (gray both
		 * roots, THEN probe -- see stack_one_prepare's Dekker note).  The two
		 * cmpxchg need not be atomic w.r.t. each other: each move's cycle
		 * safety is a pairwise Dekker on its OWN host's flag, published before
		 * this exchange's walks, and no cycle-check read runs between the two
		 * sets.  Acquire in ADDRESS ORDER so two exchanges of the same pair
		 * cannot deadlock, and release a partial acquire before retrying.  The
		 * two hosts are always distinct here (same-entry was rejected up
		 * front), but the hi != lo guard keeps a degenerate re-resolve safe.
		 * cross only: a same-parent exchange is a pure name swap -- d_parent
		 * unchanged, no ancestry walk (like stack_shell).
		 */
		if (cross) {
			struct dentry *lo = hosta < hostb ? hosta : hostb;
			struct dentry *hi = hosta < hostb ? hostb : hosta;

			if (uatomic_cmpxchg(&lo->d_moving, 0UL, 1UL) != 0UL) {
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				continue;	/* lo owned by another mover: retry */
			}
			if (hi != lo &&
			    uatomic_cmpxchg(&hi->d_moving, 0UL, 1UL) != 0UL) {
				uatomic_and(&lo->d_moving, ~1UL);	/* release the partial acquire */
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				continue;	/* hi owned by another mover: retry */
			}
		}

		/* both entries move: bump BOTH hosts' gens (one global bump when
		 * DC_PER_NODE_GEN is off -- harmless double-step of rename_gen). */
		if (hosta->d_isdir)		/* dir only -- see stack_shell */
			txn_bump_gen(&txn, dc, hosta);
		if (hostb->d_isdir)
			txn_bump_gen(&txn, dc, hostb);
		p = stack_one_prepare(&txn, dc, topa, hosta, pb, bucket_b, sa,
				      cross, topb);	/* A -> (pb, nb) */
		if (!p)
			p = stack_one_prepare(&txn, dc, topb, hostb, pa, bucket_a,
					      sb, cross, topa); /* B -> (pa, na) */
		if (p) {
			/* Terminal, both of them: a cycle and an occupant that is
			 * not the counterpart are STATES, not races, so retrying
			 * re-derives the same answer for ever.  See the
			 * @expect_dest note in stack_one_prepare. */
			if (p == -EINVAL || p == -EEXIST) {
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				if (cross) {
					uatomic_and(&hosta->d_moving, ~1UL);
					uatomic_and(&hostb->d_moving, ~1UL);
				}
				ret = p;
				goto out_free;
			}
			urcu_txn_conflict(&txn);	/* -ENOENT/-EAGAIN: keep the turn */
			urcu_txn_end(&txn);
			if (cross) {		/* release both flags before retry */
				uatomic_and(&hosta->d_moving, ~1UL);
				uatomic_and(&hostb->d_moving, ~1UL);
			}
			continue;		/* re-find + retry */
		}
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (cross) {			/* swap committed (or aborted): clear */
			uatomic_and(&hosta->d_moving, ~1UL);
			uatomic_and(&hostb->d_moving, ~1UL);
		}
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		if (st < 0) {			/* -ENOMEM */
			ret = -ENOMEM;
			goto out_free;
		}
		break;
	}
	rcu_read_unlock();
#ifdef DC_STRESS_DEBUG
	uatomic_add(&dc_dbg_renames, 2);
#endif
	call_rcu(&sa->d_rcu, fold_cb);
	call_rcu(&sb->d_rcu, fold_cb);
	return 0;
out_free:
	/* the retry loop may have exited while holding the lane */
	dc_lane_giveback(&txn);
	rcu_read_unlock();
	free(sa);
	free(sb);
	return ret;
}

/* ---- verification walk (quiescent) ------------------------------------- */

/* @d is a content host; each child-hlist entry is a named top -> follow to its
 * host and recurse there, since a mid-transition entry's children live on the
 * host, not on the shell that currently tops the child list. */
static void walk_rec(struct dentry *d, struct dc_path *path, dc_visit_fn fn,
		     void *arg)
{
	struct urcu_txn_hlist_node *n;

	/* Skip NEGATIVES: the census counts OBJECTS, and a negative dentry holds
	 * a name without one -- its d_id is stale by construction (dc_delete
	 * cannot clear it without racing a reader; dc_add_negative never set
	 * it).  Reporting it would make a conservation gate read a cached
	 * absence as a surviving object. */
	if ((path->ndepth > 0 || parent_of_rcu(d) != d) && DC_IS_POSITIVE(d))
		fn(d->d_id, path, arg);

	for (n = urcu_txn_hlist_first_rcu(&d->d_child_head); n;
	     n = urcu_txn_hlist_next_rcu(n)) {
		struct dentry *top = sib_dentry(n);
		struct dentry *host = host_of_rcu(top);		/* O(1) */

		if (path->ndepth >= DC_PATH_MAX)
			continue;
		/* current name, read off the write-once TOP (never a host) */
		DC_INAME_COPY(&path->comp[path->ndepth++], top);
		walk_rec(host, path, fn, arg);
		path->ndepth--;
	}
}

void dc_walk(struct dcache *dc, dc_visit_fn fn, void *arg)
{
	struct dc_path path;

	dc_path_reset(&path);
	rcu_read_lock();			/* chain_host_rcu resolves d_fwd */
	walk_rec(dc->root, &path, fn, arg);
	rcu_read_unlock();
}

/*
 * List a directory: resolve @path to the dir, then traverse its child-hlist
 * under RCU, reporting each named top's current inline name and its content
 * host's id (follow d_fwd -- same rule as lookup).  Lock-free, POSIX-soft.
 *
 * The shell is the vehicle in the child-hlist too (the stack del+inserts d_sib
 * exactly as it does d_hash), so a listing that runs during a move sees either
 * the old top or the new shell -- each carrying a coherent (name, host) pair --
 * never a torn one.  A moved-away entry may still appear (an old-directory
 * straggler) or a moved-in one may not yet (soft membership), but no wrong name
 * and no directory jump.  Path resolution here is soft too (no rename_gen
 * bracket): listing is not a whole-walk causal read.
 */
long dc_readdir(struct dcache *dc, const struct dc_path *path,
		dc_dirent_fn fn, void *arg)
{
	struct urcu_txn_hlist_node *n;
	struct dentry *dir;
	long count = 0;
	uint32_t i;

	rcu_read_lock();
	dir = dc->root;
	for (i = 0; i < path->ndepth; i++) {
		dir = txn_child_lookup_rcu(dc, dir, &path->comp[i]);
		if (!dir) {
			rcu_read_unlock();
			return -ENOENT;
		}
	}
	for (n = urcu_txn_hlist_first_rcu(&dir->d_child_head); n;
	     n = urcu_txn_hlist_next_rcu(n)) {
		struct dentry *top = sib_dentry(n);
		struct dentry *host = host_of_rcu(top);		/* O(1) */

		if (fn) {
#ifdef DC_DEBUG_NAME_GUARD
			/* Debug builds copy through the guard (and
			 * DC_DEBUG_NAME_GUARD_MUTATE aims it at the HOST, the
			 * deliberate violation that proves the guard fires).
			 * The shipped path below is untouched: handing out a
			 * pointer instead of a 48-byte copy is what makes
			 * readdir cheap, and the readdir panels measure it. */
			struct qstr nm;

			DC_INAME_COPY(&nm, DC_READDIR_NAME_SRC(top, host));
			fn(host->d_id, &nm, arg);
#else
			fn(host->d_id, &top->d_iname, arg);
#endif
		}
		count++;
	}
	rcu_read_unlock();
	return count;
}
