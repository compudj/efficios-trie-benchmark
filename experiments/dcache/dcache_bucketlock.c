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
 * MCAS commit (both indexes + gen bump + demote) and defers the rest to a fold
 * worker a grace period later, which either moves the shell's name into the
 * host (the shell is still the named top) or frees the shell (a later rename or
 * an unlink removed it).  There is no chain between top and host.  The cross-dir
 * loop check is now lock-free
 * too: d_parent is transacted, and a cross-parent rename folds the whole
 * new_parent->root ancestry walk into its commit's validate set via
 * urcu_txn_load_validate() (reject -EINVAL if the victim appears), so two moves
 * that would jointly form a cycle cannot both commit (see stack_shell).  Unlink
 * works mid-fold too: it removes the current named top and frees the host; a
 * shell top is left to its pending fold, which finds it out of the index and
 * frees it (see fold()/dc_unlink).  The atomic exchange composes TWO shell stacks
 * -- both index del/insert pairs, both loop checks + reparents -- in ONE commit
 * (dc_rename_exchange).  Readers reach the host in ONE hop through the
 * write-once d_host skip pointer (host_of_rcu), the reverse walk reaches the top
 * through the host's d_top, and demoted shells cost memory, not time: the async
 * fold -- which call_rcu already
 * batches per grace period -- frees them at the rate renames create them
 * (steady state ~ churn x GP latency).  A synchronous fold-ahead relief valve used to cap
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
#include <urcu/rcu-txn.h>		/* the canonical (mixed SW/MW) front-end + URCU_TXN_TAG */
#include <urcu/rcu-txn-hlist.h>		/* URCU_TXN_HLIST_TAG / _MARK / is_marked (reused) */
#include <urcu/rcu-txn-sw.h>		/* bucket lock: single-writer flip-proxy commit */
#include <urcu/rcu-txn-sw-hlist.h>	/* bucket lock: SW hlist node type + _prepare forms */
/*
 * One build.  The chain-serialization variants (DC_CHAIN_LOCK, DC_CHAIN_SWMW,
 * DC_SWMW_PAD, the DC_CHAIN_FOLDLOCK default) compared ways of serializing the
 * folds of a transition chain.  There has been no chain since 2026-10-01 (see
 * fold()) -- a fold now only touches the index and the host's d_top, under the
 * bucket lock it already takes -- so they are retired.  The index commits
 * through the canonical urcu_txn_* front-end's SW records (store_sw).
 */
#if defined(DC_CHAIN_LOCK) || defined(DC_CHAIN_SWMW) || defined(DC_SWMW_PAD) || \
    defined(DC_CHAIN_FOLDLOCK)
# error "the bucket-lock chain variants are retired: there is no chain to serialize"
#endif

#include "dcache.h"
#define DC_TXN_STATS_IMPL
#include "dcache_txn_stats.h"

/*
 * ---- engine: the mixed front-end's SW records ------------------------------
 *
 * The shell ops (stack, exchange, the fold's TRANSFER) commit SW records
 * (store_sw, under the bucket locks) through the canonical urcu_txn_* front-end
 * (rcu-txn.h); add/unlink stay plain locked stores.  The front-end resolves SW
 * proxies and MW records through one header, so ONE reader (dc_proxy_resolve)
 * serves both.
 */
typedef struct urcu_txn	dc_swtxn_t;
typedef struct urcu_txn_domain	dc_domain_t;
#define dc_sw_record(txn, slot, o, n, tag) \
	urcu_txn_store_sw((txn), (slot), (o), (n), (tag))
#define dc_proxy_resolve(p) \
	urcu_txn_resolve_record((struct urcu_txn_record *) (p))

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
 * the fold's still-top-vs-demoted branch.  NULL and never compiled into normal
 * builds.
 */
void (*dc_test_fold_hook)(void);
/*
 * Test-only rendezvous fired INSIDE the fold's TRANSFER, between the read of the
 * host's d_iparent and the cmpxchg that writes it back -- the window a
 * concurrent d_delete / d_instantiate would be lost in if that write-back were
 * not atomic.  NULL and never compiled into normal builds.
 */
void (*dc_test_transfer_hook)(void);
#define DC_TEST_TRANSFER_HOOK()	do {					\
		if (dc_test_transfer_hook)				\
			dc_test_transfer_hook();			\
	} while (0)
#else
#define DC_TEST_TRANSFER_HOOK()	do { } while (0)
#endif

/*
 * TEST-ONLY: -DDC_TEST_UNLINK_DELAY_US=N sleeps N us inside the unlink of a
 * DIRECTORY, after its last check that the victim is empty and before the
 * removal is published -- the window an add under the victim must not land in
 * (stress_dcache_rmdir.c, make check-rmdir).  At natural timings that window
 * is a few instructions.  Never set in a measured build.
 */
#ifdef DC_TEST_UNLINK_DELAY_US
#include <time.h>
static void dc_test_unlink_delay(void)
{
	struct timespec ts = { DC_TEST_UNLINK_DELAY_US / 1000000,
			       (DC_TEST_UNLINK_DELAY_US % 1000000) * 1000L };

	(void) nanosleep(&ts, NULL);
}
#define DC_TEST_UNLINK_DELAY()	dc_test_unlink_delay()
#else
#define DC_TEST_UNLINK_DELAY()	do { } while (0)
#endif

/*
 * TEST-ONLY mutation arms for make check-rmdir, each dropping one half of the
 * exclusion between a directory's unlink and an add under it; each must make
 * stress_dcache_rmdir FAIL, or the harness proves nothing.
 *   -DDC_TEST_NO_RMDIR_LOCK  dc_unlink does not lock the victim's child head
 *   -DDC_TEST_NO_ADD_ALIVE   dc_add does not re-check its parent is alive
 */

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
 * The bucket lock engine is MARK-ONLY for now: its shell ops (rename/exchange/fold) owe
 * no walk-causality gen bump -- a rename's demote MARK is the signal -- so they
 * do not implement the GLOBAL / PER-NODE counter arms.  A bare build therefore
 * defaults to the mark arm; any explicit non-mark selection hits the #error in
 * the shell-op section.  (Carrying the counter arms later = restore an
 * SW-recording txn_bump_gen and drop this default + the #error.)
 */
#if !defined(DC_MARK_GEN) && !defined(DC_PER_NODE_GEN) && \
    !defined(DC_HOT1CL) && !defined(DC_NO_HOT1CL_SPLIT)
# define DC_MARK_GEN 1
#endif

/*
 * DC_MARK_GEN (rename-shell-transition.md): carry walk causality on the hlist
 * DELETION MARK -- d_hash.next bit 1, which every operation that changes the
 * named top already sets -- instead of the d_seq counter.  Needs the d_iparent
 * tag encoding, so it implies the SPLIT layout.  Retiring d_seq hands its 8
 * bytes to the name (DC_NAME_MAX 40).
 */
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
	struct urcu_txn_sw_hlist_node d_hash;	/* transacted; live only while named top */
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
	struct urcu_txn_sw_hlist_node d_hash;	/* straddle: next@56 CL0, pprev@64 cold */
#elif defined(DC_HOT1CL)
	/* payload joins the hot line: d_iparent(8)+d_iname(40)+union(8) = 56 B */
	union {
		uint64_t       d_id;
		struct dentry *d_host;
	};
#endif

	/*
	 * ---- cold cache-line layout -------------------------------------------
	 * CL0 above is the reader-hot line (identity + mark).  The cold fields are
	 * split so that CL1 carries ONLY writer-side and read-only words, never a
	 * field another core reads concurrently:
	 *   CL1: d_fwd (write-once), d_top (written by the demote and a fold's
	 *     TRANSFER, both under the top's bucket lock), d_dc (read-only),
	 *     d_inode/d_isdir (set at creation), d_rcu (reclaim).
	 *   CL2 (the structural-reader line): d_parent + d_moving (read cross-core by
	 *     a peer mover's cycle check), d_child_head + d_sib + the d_id/d_host
	 *     union (read cross-core by readdir / host_of_rcu).  Kept off CL1 so a
	 *     readdir or a peer's ancestry walk does not bounce the writer's line.
	 * (CL1 used to be "the fold-lock line": it held a per-host lock that
	 * serialized a chain's folds; there is no chain since 2026-10-01, so no
	 * lock -- see fold().)
	 *
	 * d_fwd: WRITE-ONCE.  A shell's points at its host (the value d_host holds),
	 * a host's is NULL: the host/shell discriminator of the non-HOT1CL layouts,
	 * and what lru_evict_settled() uses to skip shells.  No longer a chain link.
	 *
	 * d_top, meaningful on the HOST only: its entry's current named top -- the
	 * shell that names it -- or NULL while the host names its entry itself.  A
	 * stack or exchange moves it to the new shell, a fold's TRANSFER resets it
	 * to NULL, together with the index change; so the reverse walk reads an
	 * entry's name in two loads and an indexed shell is always its host's
	 * d_top.  (Until 2026-10-01: a doubly linked d_fwd/d_back chain.)
	 */
	struct dentry *d_fwd;			/* write-once: shell -> host; NULL on a host */
	struct dentry *d_top;			/* host: current named top; NULL = itself */
	struct dcache *d_dc;			/* owner, so a call_rcu fold reaches the domain */
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
	struct urcu_txn_sw_hlist_node d_hash;
#endif
	struct rcu_head d_rcu;

	/*
	 * ---- CL2: the structural-reader line (see the cold-layout note above) --
	 * Every field here is read cross-core by an operation OTHER than a fold, so
	 * it is kept off CL1.
	 */

	/* writer-side bookkeeping.  d_parent + d_moving are read cross-core by a peer
	 * mover's ancestry cycle check; keep them ADJACENT so a walk hop reads both.
	 * ALIGNED: this is where CL2 starts, and it must not depend on CL1 summing
	 * to 64 bytes -- removing the 8-byte fold lock (2026-10-01) slid d_parent
	 * onto CL1, splitting it from d_moving.  (No benchmark here moved either
	 * way; the alignment keeps the layout the comments describe.) */
	struct dentry *d_parent		/* logical parent; TRANSACTED (DC_PARENT_TAG) */
		__attribute__((aligned(64)));
	/*
	 * MOVE-IN-PROGRESS flag (cross-dir cycle prevention).  A cross-parent move
	 * sets this on its host before validating the ancestry, and clears it after
	 * the reparent commits (or aborts).  The loop check walks new_parent -> root
	 * with PLAIN loads (not the proxy-installing load_validate that pinned the
	 * shared spine) and aborts (-EAGAIN) if any ancestor carries this flag: a
	 * Dekker set-before-check, so two moves that would jointly form a cycle
	 * cannot both proceed (one sees the other's flag).  Only written when THIS
	 * node is itself the host of a move, so spine nodes keep it 0 forever (their
	 * reads stay S-state -- no cross-CCD ping-pong). */
	unsigned long d_moving;

	/*
	 * Child index (readdir fast path + -ENOTEMPTY): a per-directory
	 * rcu-txn-hlist, mutated by MCAS and traversed under RCU.  d_child_head
	 * heads THIS node's children; d_sib links this node into its parent's
	 * child-hlist.  Distinct from d_hash (the name-bucket link).  Read cross-core
	 * by readdir, so on CL2 -- off CL1.
	 */
	struct urcu_txn_sw_hlist_head d_child_head;
	struct urcu_txn_sw_hlist_node d_sib;

	/*
	 * Identity id (HOST) OR skip pointer to the content host (SHELL), overlaid:
	 * a host reads it as d_id, a shell as d_host.  Which is live is fixed by the
	 * STABLE per-node property d_fwd==NULL (host) vs !=NULL (shell) -- a node is
	 * born a host or a shell and never crosses over, so each node only ever
	 * touches ONE member (no type-punning).  A reader resolves the host in O(1)
	 * with host_of_rcu().  The shell's d_host is WRITE-ONCE (a fold never moves
	 * the host), so it's a plain rcu_dereference.  This reuses the old
	 * "cosmetic" shell d_id slot -- shells never needed their own id (readers use
	 * the host's) and hosts never need a self skip pointer.  (Under DC_HOT1CL the
	 * union is hoisted onto the hot line above instead.)
	 */
#if !defined(DC_HOT1CL) || defined(DC_HOT1CL_SPLIT)
	union {
		uint64_t       d_id;	/* host: stable identity (SPLIT: cold) */
		struct dentry *d_host;	/* shell: skip pointer to the tail host */
	};
#endif

#ifndef DC_NO_LRU
	/*
	 * ---- PHASE 3: LRU, ON ITS OWN CACHELINE --------------------------------
	 *
	 * Off CL0 because a lookup never touches the LRU (this port takes no
	 * reference, exactly as the kernel's RCU walk does not).  But "not CL0"
	 * was not enough, and the first cut got it wrong.
	 *
	 * The reason is the INTRUSIVE LIST: splicing a node out writes its two
	 * NEIGHBOURS' link fields, so every add / unlink / rotate dirties a line
	 * belonging to two ARBITRARY OTHER dentries -- whichever happened to be
	 * enqueued near it in time, which under churn are nodes other cores are
	 * actively using.  The shard lock does not help: it serializes the
	 * operation, not the coherence traffic.
	 *
	 * So these links must not share a line with anything another core touches
	 * for a different reason -- and BOTH existing cold lines fail that test:
	 *   CL1 is the writer line, whose stated contract is "ONLY writer-side
	 *       and read-only words, never a field another core reads
	 *       concurrently" -- and it holds d_fwd, which the non-HOT1CL
	 *       layouts' READERS test as the host/shell discriminator;
	 *   CL2 is the structural-reader line -- d_child_head / d_sib for readdir,
	 *       and d_host, which host_of_rcu resolves on every shelled LOOKUP.
	 *
	 * Hence a dedicated line.  It costs sizeof(dentry) 192 -> 256, which is
	 * the LRU's own honest price, and is why -DDC_NO_LRU exists: the A/B
	 * control that MEASURES that price instead of asserting it is small.
	 */
	struct {
#ifdef DC_LRU_MCAS
		/*
		 * Transacted edges AND transacted membership: `owner` points at
		 * the shard's deque and is written by the same commit that moves
		 * the edges, so there is NO shard word on this arm.  A word and
		 * the links can disagree; a pointer inside the commit cannot.
		 */
		struct urcu_txn_deque_node dnode;
#else
		struct dentry *prev;		/* NULL when not on a shard */
		struct dentry *next;
		unsigned int   shard;		/* DC_LRU_OFF / DC_LRU_ON(i) */
#endif
		unsigned char  referenced;	/* DCACHE_REFERENCED analog */
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

/* ==== bucket lock engine: per-bucket write lock (bit 2) + SW-proxy resolve ========= *
 *
 * The bucket lock engine keeps dcache_txn.c's algorithm intact (shell rename, async
 * fold, host_of_rcu skip, walk-causality gen, the deletion mark) but swaps the
 * writer commit from the multi-writer MCAS to the single-writer SW flip-proxy
 * engine (rcu-txn-sw.h): a writer LOCKS the affected bucket head(s) -- one or
 * two CAS on bit 2 of the head word -- then commits with plain stores + one
 * selector.  The lock supplies the single-writer-per-slot exclusion the SW form
 * assumes; readers never take it.
 *
 * Bit budget of a transacted word (nodes are 8-aligned, bits 0..2 free):
 *   bit 0  proxy TAG   -- every transacted slot, mid-commit
 *   bit 1  MARK        -- a node's "next" (deletion mark); SHELL on d_iparent
 *   bit 2  DC_BL_LOCK  -- a HEAD word (bucket / d_child_head); DC_TAG_NEG on
 *                        d_iparent.  A head is never marked and an iparent word
 *                        is never a head, so the per-slot-class overloading is
 *                        unambiguous -- the same trick bit 1 already uses.
 */
#define DC_BL_LOCK	((uintptr_t) 0x4)	/* bit 2: bucket-head write lock */

/* Masked chain head (drop the lock bit): the real, possibly-proxy first. */
static inline struct urcu_txn_sw_hlist_node *
bl_first(struct urcu_txn_sw_hlist_head *h)
{
	return (struct urcu_txn_sw_hlist_node *)
		(__atomic_load_n((uintptr_t *) &h->first, __ATOMIC_RELAXED)
		 & ~DC_BL_LOCK);
}

/*
 * Test-and-TEST-and-set, as the kernel's bit_spin_lock is: one fetch_or, then
 * wait with plain loads until the bit clears.  Readers load this word on every
 * walk through the bucket, so a waiter that RMWs it on every spin (plain
 * test-and-set, what this was until 2026-09-30) invalidates it under all of
 * them and under the holder.  Measured: stale readers caching the same negative
 * at once (bench_dcache --hit-current, 184 readers) cost this engine 33% of its
 * lookups and its renamers 12% of their paced rate; half of that was the spin.
 */
static inline void bl_lock(struct urcu_txn_sw_hlist_head *h)
{
	uintptr_t *p = (uintptr_t *) &h->first;

	while (__atomic_fetch_or(p, DC_BL_LOCK, __ATOMIC_ACQUIRE) & DC_BL_LOCK)
		do {
			caa_cpu_relax();
		} while (__atomic_load_n(p, __ATOMIC_RELAXED) & DC_BL_LOCK);
}

static inline void bl_unlock(struct urcu_txn_sw_hlist_head *h)
{
	__atomic_fetch_and((uintptr_t *) &h->first, ~DC_BL_LOCK,
			   __ATOMIC_RELEASE);
}

/* Two-head acquire/release in ADDRESS ORDER (deadlock-free) for rename/exchange
 * and folds that span two buckets. */
static inline void bl_lock2(struct urcu_txn_sw_hlist_head *x,
			    struct urcu_txn_sw_hlist_head *y)
{
	if (x == y)	{ bl_lock(x); return; }
	if (x < y)	{ bl_lock(x); bl_lock(y); }
	else		{ bl_lock(y); bl_lock(x); }
}
static inline void bl_unlock2(struct urcu_txn_sw_hlist_head *x,
			      struct urcu_txn_sw_hlist_head *y)
{
	bl_unlock(x);
	if (x != y)
		bl_unlock(y);
}

/*
 * Lock up to @n bucket heads in ADDRESS ORDER, de-duplicating coincident heads
 * (a same-parent rename shares one child head; a hash collision can share a
 * bucket).  Sorts @h in place (@n is tiny -- <= 4); pass the SAME array, still
 * sorted, to bl_unlock_n.
 */
static inline void bl_lock_n(struct urcu_txn_sw_hlist_head **h, int n)
{
	int i, j;

	for (i = 1; i < n; i++) {		/* insertion sort */
		struct urcu_txn_sw_hlist_head *k = h[i];

		for (j = i - 1; j >= 0 && h[j] > k; j--)
			h[j + 1] = h[j];
		h[j + 1] = k;
	}
	for (i = 0; i < n; i++)
		if (i == 0 || h[i] != h[i - 1])	/* skip a coincident head */
			bl_lock(h[i]);
}
static inline void bl_unlock_n(struct urcu_txn_sw_hlist_head **h, int n)
{
	int i;

	for (i = 0; i < n; i++)			/* @h already sorted by bl_lock_n */
		if (i == 0 || h[i] != h[i - 1])
			bl_unlock(h[i]);
}

/*
 * Resolve a transacted SINGLE-pointer slot (d_fwd, d_parent, d_iparent, d_seq,
 * the gen) to its current value: strip the proxy tag (bit 0) through the SW
 * selector, else pass through.  Any OTHER low bits (iparent SHELL/NEG) are
 * returned intact for the caller.  Drop-in for bl_read().  NOT for head
 * words -- those carry the lock; use the hlist accessors below.
 */
static inline void *bl_read(void **slot, uintptr_t tag)
{
	uintptr_t v = (uintptr_t) rcu_dereference(*slot);

	if (caa_unlikely((v & tag) == tag))
		return dc_proxy_resolve(v & ~tag);
	return (void *) v;
}

/*
 * DC_OPAQUE(v): make @v opaque to the optimizer, at no cost.  The empty asm
 * claims to read and rewrite @v in a register; it emits no instruction, but the
 * compiler can no longer see what the branch it sits in did to @v, so it cannot
 * merge that branch with the path that skips it.  Used to keep
 *
 *	if (unlikely(v & BITS)) { v -= <bits set>; DC_OPAQUE(v); }
 *
 * from being folded into the equivalent, unconditional "v &= ~BITS" -- which
 * GCC does whenever the branch body is simple enough.
 *
 * Why the branch is worth keeping.  On a POINTER the next load goes through, a
 * mask puts an ALU op between the load and the loads that follow it, on the
 * walk's dependency chain; a test with a well-predicted branch sits beside the
 * chain and the next load issues on the raw value.  The test is also fewer
 * instructions here: the fast path is one test-and-branch, and a caller that
 * only wants the mark (top_unhashed_rcu) collapses to it.  Measured on
 * bl_hlist_resolve (2026-10-01, 16 readers, interleaved medians), masking cost
 * the bucket lock 9% of its positive hits at rest and 8% under 100k renames/s,
 * and 16% of its reverse walks -- which only TEST the mark through it, so that
 * part is fewer instructions (309 -> 265 per walk), not the dependency chain.
 *
 * ONLY WHERE THE BITS ARE ALMOST NEVER SET.  A hash-chain word qualifies: the
 * lock bit is set only while a writer holds that bucket, the proxy tag only
 * mid-commit, the mark only on a removed node -- with it, branch misses (all
 * branches) stayed at 0.003 per reverse walk under 100k renames/s.  d_iparent
 * does not: its SHELL / NEG bits are data (a leaf's named top is a shell most
 * of the time under renames).  The same check on iparent_of() measured -1..-2%
 * at rest and 11-15% SLOWER reverse walks under 100k renames/s (txn-global,
 * txn-mark, bucket lock), branch misses going from ~0.01 to 0.31 per walk.  Why
 * that loss is so large is NOT established: the slot breakdown shows most of
 * the mispredicted work overlapping the load wait the mask pays anyway.
 * Measure, per site, before choosing a test over a mask.
 */
#define DC_OPAQUE(v)	__asm__ ("" : "+r" (v))

/*
 * Resolve a head-first / node-next hlist slot to the node it denotes.
 *
 * Fast path: a clean word -- no lock, proxy tag or mark bit, which is every
 * live node's next and every head no writer holds -- IS the node address, and
 * is returned as loaded (see DC_OPAQUE for why that is a test and not a mask).
 * Otherwise strip the bucket LOCK (bit 2) FIRST -- a first-position insert can
 * leave a head holding proxy|TAG|LOCK, and the proxy address is only
 * recoverable once the lock bit is cleared -- then resolve the proxy (bit 0),
 * then strip the deletion MARK (bit 1) from the result, reporting it via
 * *marked (NULL to ignore).
 */
static inline struct urcu_txn_sw_hlist_node *
bl_hlist_resolve(struct urcu_txn_sw_hlist_node *ptr, int *marked)
{
	uintptr_t v = (uintptr_t) ptr;

	if (marked)
		*marked = 0;
	if (caa_unlikely(v & (DC_BL_LOCK | URCU_TXN_HLIST_TAG |
			      URCU_TXN_HLIST_MARK))) {
		if (v & DC_BL_LOCK)
			v -= DC_BL_LOCK;
		/*
		 * A head-slot proxy carries the lock bit in its OLD/NEW targets
		 * too (the settle must keep bit 2 on the slot), so strip it
		 * again off the resolved value.
		 */
		if (v & URCU_TXN_HLIST_TAG)
			v = (uintptr_t) dc_proxy_resolve(
				v - (uintptr_t) URCU_TXN_HLIST_TAG) & ~DC_BL_LOCK;
		if (v & URCU_TXN_HLIST_MARK) {
			if (marked)
				*marked = 1;
			v -= URCU_TXN_HLIST_MARK;
		}
		DC_OPAQUE(v);		/* keep the branch: no fold into a mask */
	}
	return (struct urcu_txn_sw_hlist_node *) v;
}

/* Resolved first / next step (call under rcu_read_lock()). */
static inline struct urcu_txn_sw_hlist_node *
bl_hlist_first_rcu(struct urcu_txn_sw_hlist_head *head)
{
	return bl_hlist_resolve(rcu_dereference(head->first), NULL);
}
static inline struct urcu_txn_sw_hlist_node *
bl_hlist_next_rcu(struct urcu_txn_sw_hlist_node *node)
{
	return bl_hlist_resolve(rcu_dereference(node->next), NULL);
}

/*
 * ---- shell-FREE ops (add / unlink): plain marked stores under the lock -----
 *
 * A single reader-visible edge each, so no selector is needed -- a lone pointer
 * publish is already atomic to a reader.  The writer holds the bucket head
 * lock, so it is the only writer; readers resolve the plain (possibly marked)
 * pointer exactly as today.  The head store keeps the lock bit set; the del
 * store keeps whatever lock bit the naming slot already held (a head keeps it,
 * a node next has none).
 */

/* Publish n as the new first of a LOCKED head, keeping the lock bit set. */
static inline void bl_set_first(struct urcu_txn_sw_hlist_head *h,
				struct urcu_txn_sw_hlist_node *n)
{
	__atomic_store_n((uintptr_t *) &h->first,
			 (uintptr_t) n | DC_BL_LOCK, __ATOMIC_RELEASE);
}

/* Add n at the head of a LOCKED bucket. */
static inline void bl_hlist_add_head_locked(struct urcu_txn_sw_hlist_head *h,
					     struct urcu_txn_sw_hlist_node *n)
{
	struct urcu_txn_sw_hlist_node *first = bl_first(h);

	n->next = first;
	n->pprev = &h->first;
	if (first)
		first->pprev = &n->next;
	bl_set_first(h, n);		/* release: publish n, keep the lock */
}

/*
 * Remove n from its LOCKED chain.  MARK n's own next FIRST (logical delete, so a
 * reader standing on n observes the removal via top_unhashed_rcu), THEN unlink
 * *n->pprev past it (physical), preserving a lock bit if the naming slot is a
 * head.  n's own next/pprev are left pointing forward (marked), so a reader on n
 * still escapes into the live chain; the caller reclaims n after a grace period.
 */
static inline void bl_hlist_del_locked(struct urcu_txn_sw_hlist_node *n)
{
	struct urcu_txn_sw_hlist_node **ppv = n->pprev;
	struct urcu_txn_sw_hlist_node *next =
		(struct urcu_txn_sw_hlist_node *)
		((uintptr_t) n->next & ~(uintptr_t) URCU_TXN_HLIST_MARK);
	uintptr_t lockbit = __atomic_load_n((uintptr_t *) ppv,
					    __ATOMIC_RELAXED) & DC_BL_LOCK;

	__atomic_store_n((uintptr_t *) &n->next,
			 (uintptr_t) next | URCU_TXN_HLIST_MARK, __ATOMIC_RELEASE);
	__atomic_store_n((uintptr_t *) ppv, (uintptr_t) next | lockbit,
			 __ATOMIC_RELEASE);
	if (next)
		next->pprev = ppv;	/* pprev is writer-only bookkeeping */
}

/*
 * ---- shell ops (rename / exchange / fold): bucket-lock + SW selector --------
 *
 * These need reader-atomic MULTI-edge flips, so they record into a
 * urcu_txn_sw_txn and commit its selector under the bucket lock(s).  Two
 * wrinkles vs the raw SW hlist:
 *   HEAD slots carry the bucket lock (bit 2).  install parks proxy|tag and
 *   settle stores new -- both would drop the lock -- so a head record uses tag
 *   DC_HLOCKTAG and its old/new carry the lock bit, keeping bit 2 set across the
 *   whole install..settle window (bl_hlist_resolve strips it, twice).
 *   a DEL must MARK the removed node's next (the reader's unlink signal), which
 *   the raw SW del omits, so we record that edge too.
 * pprev backpointers are writer-only (no reader reads them); recording them
 * keeps a mutation all-or-nothing under an OOM abort, matching the SW hlist.
 */
#define DC_HTAG		((uintptr_t) URCU_TXN_HLIST_TAG)  /* node/head proxy tag */
#define DC_HLOCKTAG	(DC_HTAG | DC_BL_LOCK)		  /* head-slot record tag */

/* Record insert of newp at the head of a LOCKED bucket into txn. */
static inline void bl_sw_add_head(dc_swtxn_t *txn,
		struct urcu_txn_sw_hlist_node *newp,
		struct urcu_txn_sw_hlist_head *head)
{
	struct urcu_txn_sw_hlist_node *succ = bl_first(head);	/* masked first */

	newp->next = succ;			/* pre-publish: newp not reachable yet */
	newp->pprev = &head->first;
	(void) dc_sw_record(txn, (void **) &head->first,
		(void *) ((uintptr_t) succ | DC_BL_LOCK),
		(void *) ((uintptr_t) newp | DC_BL_LOCK), DC_HLOCKTAG);
	if (succ)
		(void) dc_sw_record(txn, (void **) &succ->pprev,
			(void *) &head->first, (void *) &newp->next, DC_HTAG);
}

/* Record marked del of elem from its LOCKED chain into txn. */
static inline void bl_sw_del_marked(dc_swtxn_t *txn,
		struct urcu_txn_sw_hlist_node *elem)
{
	struct urcu_txn_sw_hlist_node **ppv = elem->pprev;
	struct urcu_txn_sw_hlist_node *next =
		(struct urcu_txn_sw_hlist_node *)
		((uintptr_t) elem->next & ~(uintptr_t) URCU_TXN_HLIST_MARK);
	uintptr_t lk = (uintptr_t) __atomic_load_n((uintptr_t *) ppv,
			__ATOMIC_RELAXED) & DC_BL_LOCK;		/* head? preserve it */

	(void) dc_sw_record(txn, (void **) ppv,
		(void *) ((uintptr_t) elem | lk),
		(void *) ((uintptr_t) next | lk), lk ? DC_HLOCKTAG : DC_HTAG);
	if (next)
		(void) dc_sw_record(txn, (void **) &next->pprev,
			(void *) &elem->next, (void *) ppv, DC_HTAG);
	(void) dc_sw_record(txn, (void **) &elem->next, (void *) next,
		(void *) ((uintptr_t) next | URCU_TXN_HLIST_MARK), DC_HTAG);
}

/*
 * Record an IN-PLACE replace of @oldn by @newn into txn: @newn takes @oldn's
 * exact slot, and @oldn's own next is MARKED so top_unhashed_rcu(@oldn) reports
 * it left the index (a fold TRANSFER, like a del, is a walk-causality event for
 * @oldn -- it is no longer the indexed top).  @newn must be UNLINKED (a fresh
 * shell or the content host, out of every index) -- its next/pprev are set here
 * pre-publish.  Unlike a del + add-head, only *oldn->pprev is rewritten (to
 * @newn), so it never double-writes a first-slot bucket head (no same-slot
 * record conflict).  The caller holds @oldn's bucket-head lock. */
static inline void bl_sw_replace(dc_swtxn_t *txn,
		struct urcu_txn_sw_hlist_node *oldn,
		struct urcu_txn_sw_hlist_node *newn)
{
	struct urcu_txn_sw_hlist_node **ppv = oldn->pprev;
	struct urcu_txn_sw_hlist_node *next =
		(struct urcu_txn_sw_hlist_node *)
		((uintptr_t) oldn->next & ~(uintptr_t) URCU_TXN_HLIST_MARK);
	uintptr_t lk = (uintptr_t) __atomic_load_n((uintptr_t *) ppv,
			__ATOMIC_RELAXED) & DC_BL_LOCK;		/* head? preserve it */

	newn->next = next;			/* pre-publish: newn takes oldn's succ */
	newn->pprev = ppv;
	(void) dc_sw_record(txn, (void **) ppv,	/* predecessor -> newn */
		(void *) ((uintptr_t) oldn | lk),
		(void *) ((uintptr_t) newn | lk), lk ? DC_HLOCKTAG : DC_HTAG);
	if (next)
		(void) dc_sw_record(txn, (void **) &next->pprev,
			(void *) &oldn->next, (void *) &newn->next, DC_HTAG);
	(void) dc_sw_record(txn, (void **) &oldn->next, (void *) next,
		(void *) ((uintptr_t) next | URCU_TXN_HLIST_MARK), DC_HTAG);
}

/*
 * Record replace of an ADJACENT pair @first -> @second (first->next == second)
 * by @newfirst -> @newsecond in one combined edit, MARKing both olds.  Two plain
 * bl_sw_replace()s would double-write the shared linking slot (first->next is
 * also second->pprev's target); this writes it once.  The news are UNLINKED;
 * their next/pprev are set here pre-publish.  Caller holds the chain's head lock.
 */
static inline void bl_sw_replace_adj(dc_swtxn_t *txn,
		struct urcu_txn_sw_hlist_node *first,
		struct urcu_txn_sw_hlist_node *newfirst,
		struct urcu_txn_sw_hlist_node *second,
		struct urcu_txn_sw_hlist_node *newsecond)
{
	struct urcu_txn_sw_hlist_node **pp = first->pprev;
	struct urcu_txn_sw_hlist_node *succ =
		(struct urcu_txn_sw_hlist_node *)
		((uintptr_t) second->next & ~(uintptr_t) URCU_TXN_HLIST_MARK);
	uintptr_t lk = (uintptr_t) __atomic_load_n((uintptr_t *) pp,
			__ATOMIC_RELAXED) & DC_BL_LOCK;

	newfirst->pprev = pp;			/* pred -> newfirst -> newsecond -> succ */
	newfirst->next = newsecond;
	newsecond->pprev = &newfirst->next;
	newsecond->next = succ;
	(void) dc_sw_record(txn, (void **) pp,		/* pred -> newfirst */
		(void *) ((uintptr_t) first | lk),
		(void *) ((uintptr_t) newfirst | lk), lk ? DC_HLOCKTAG : DC_HTAG);
	if (succ)
		(void) dc_sw_record(txn, (void **) &succ->pprev,
			(void *) &second->next, (void *) &newsecond->next, DC_HTAG);
	(void) dc_sw_record(txn, (void **) &first->next,	/* mark first */
		(void *) second,
		(void *) ((uintptr_t) second | URCU_TXN_HLIST_MARK), DC_HTAG);
	(void) dc_sw_record(txn, (void **) &second->next,	/* mark second */
		(void *) succ,
		(void *) ((uintptr_t) succ | URCU_TXN_HLIST_MARK), DC_HTAG);
}

/*
 * Record replace of TWO nodes in the SAME locked chain (olda->newa, oldb->newb),
 * both olds MARKed.  A same-directory exchange puts both tops in one child head
 * (a hash collision, one bucket); two independent bl_sw_replace()s would then
 * conflict on a shared linking slot iff the olds are ADJACENT.  Disjoint: two
 * independent replaces; adjacent: the combined edit above (correct order).
 */
static inline void bl_sw_replace2(dc_swtxn_t *txn,
		struct urcu_txn_sw_hlist_node *olda,
		struct urcu_txn_sw_hlist_node *newa,
		struct urcu_txn_sw_hlist_node *oldb,
		struct urcu_txn_sw_hlist_node *newb)
{
	struct urcu_txn_sw_hlist_node *na = (struct urcu_txn_sw_hlist_node *)
		((uintptr_t) olda->next & ~(uintptr_t) URCU_TXN_HLIST_MARK);
	struct urcu_txn_sw_hlist_node *nb = (struct urcu_txn_sw_hlist_node *)
		((uintptr_t) oldb->next & ~(uintptr_t) URCU_TXN_HLIST_MARK);

	if (na == oldb)				/* olda directly precedes oldb */
		bl_sw_replace_adj(txn, olda, newa, oldb, newb);
	else if (nb == olda)			/* oldb directly precedes olda */
		bl_sw_replace_adj(txn, oldb, newb, olda, newa);
	else {					/* disjoint slots: two plain replaces */
		bl_sw_replace(txn, olda, newa);
		bl_sw_replace(txn, oldb, newb);
	}
}

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
 * overwhelming common case -- bl_read() is a tag test and a
 * well-predicted branch on a word already loaded for the compare, the same shape
 * top_unhashed_rcu() already pays on this path.
 */
#define DC_IPARENT_TAG	URCU_TXN_TAG
#endif

static inline uintptr_t iparent_raw(const struct dentry *d)
{
#ifdef DC_IPARENT_TXN
	struct dentry *nc = (struct dentry *) (uintptr_t) d;

	return (uintptr_t) bl_read((void **) &nc->d_iparent,
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

/*
 * The two writers of a reachable host's d_iparent, and why both use atomics.
 *
 * Until phase 2 the fold's TRANSFER was the ONLY one, so its read-modify-write
 * could be plain: it adopts the outgoing top's PARENT while preserving the
 * host's own shell and pos/neg bits, and rename preserves inode-ness, so the
 * preserved bits never actually changed.  d0e7955 recorded exactly that, and
 * recorded its expiry date: "benign today only because rename preserves
 * inode-ness ... but it is UB and a latent correctness bug once phase-2
 * negative dentries land."  It landed -- d_delete and d_instantiate now flip
 * pos/neg on a live host from another thread.
 *
 * Two plain read-modify-writes on one word lose an update: the fold reads the
 * host positive, a concurrent d_delete publishes NEGATIVE, the fold writes back
 * the positive bit it read, and the delete is gone with no error raised
 * anywhere.  Both writers therefore go through a cmpxchg on the word.
 *
 * A transaction does NOT substitute for this.  urcu_txn_store_sw() "parks it
 * with a plain store that never fails" and an SW-only commit never
 * contention-aborts -- SW is a PROMISE OF EXCLUSION across every writer of the
 * slot, and the fold breaks it.  store_mw() would be correct but installs a
 * descriptor every reader must resolve, which d0e7955 rejected for this field.
 * The cmpxchg costs the reader nothing: its load stays plain.
 */
#ifdef DC_HOT1CL
/*
 * TRANSFER: adopt @n's parent into @m, preserving @m's own shell and pos/neg.
 *
 * CALLED WITH @n's BUCKET HEAD LOCKED, which is what makes the read-modify-write
 * safe against the other writer of this word.  It is not a cmpxchg because it
 * does not need to be: dc_delete/dc_instantiate take the same bucket lock, so
 * the two cannot interleave.  See dc_set_negative() for why that single lock
 * covers every case.
 *
 * The STORE is still a relaxed atomic, for the reader rather than the writer:
 * readers sample this word plainly for pos/neg (host_is_positive), so a plain
 * store here would be a data race against them -- which is exactly what TSAN
 * caught when phase 2 first reintroduced that read.  Same instruction on x86.
 */
static inline void dc_transfer_iparent(struct dentry *m, struct dentry *n)
{
	uintptr_t nam = (uintptr_t) n->d_iparent & ~(DC_TAG_SHELL | DC_TAG_NEG);
	uintptr_t own;

	own = (uintptr_t) uatomic_load(&m->d_iparent, CMM_RELAXED);
	DC_TEST_TRANSFER_HOOK();
	uatomic_store(&m->d_iparent,
		      (struct dentry *) (nam | (own & (DC_TAG_SHELL |
						       DC_TAG_NEG))),
		      CMM_RELAXED);
}
#endif

/*
 * Flip the content HOST between positive and negative in place: d_instantiate
 * (@negative 0, @id is the new inode) and d_delete (@negative 1, @id ignored).
 * The dentry keeps its address, bucket, children and named top.  @id is
 * stored before the flip -- a reader that sees the node positive must already
 * see the id that came with it.
 *
 * CALLER HOLDS THE NAMED TOP'S BUCKET HEAD LOCK, and has re-verified under it
 * that the top is still hashed.  That one lock is the whole exclusion argument,
 * so this is a plain read-modify-write rather than a cmpxchg:
 *
 *   - The only other writer of a reachable host's d_iparent is the fold's
 *     TRANSFER, which writes it only while its shell is still hashed -- still
 *     the named top -- and holds that shell's bucket lock across the
 *     handover: the very lock held here, since both derive it from the top.
 *   - A fold whose shell is no longer hashed only frees the shell; it never
 *     touches the host.
 *
 * The STORE stays a relaxed atomic: readers sample this word plainly for pos/neg
 * and take no lock, so writer-writer exclusion does not make a plain store legal.
 */
static int dc_set_negative(struct dentry *host, int negative, uint64_t id)
{
#ifdef DC_HOT1CL
	uintptr_t raw = (uintptr_t) uatomic_load(&host->d_iparent, CMM_RELAXED);

	if (!!(raw & DC_TAG_NEG) == !!negative)
		return negative ? -ENOENT : -EEXIST;
	if (!negative) {
		host->d_id = id;
		cmm_smp_wmb();			/* id before positive */
	}
	uatomic_store(&host->d_iparent, (struct dentry *)
		      (negative ? (raw | DC_TAG_NEG) : (raw & ~DC_TAG_NEG)),
		      CMM_RELAXED);
	host->d_inode = negative ? 0 : 1;	/* cold bookkeeping; the tag rules */
	return 0;
#else
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

#ifdef DC_STRESS_DEBUG
/* Coarse counters for diagnosing fold drain (not thread-exact).
 * dc_dbg_fold_retries = fold attempts; dc_dbg_fold_aborts = commit-level
 * ABORTs only. */
unsigned long dc_dbg_renames, dc_dbg_folds, dc_dbg_fold_retries, dc_dbg_fold_aborts;
# define DC_DBG_FOLD_ATTEMPT() uatomic_inc(&dc_dbg_fold_retries)
# define DC_DBG_FOLD_ABORT()   uatomic_inc(&dc_dbg_fold_aborts)
#else
# define DC_DBG_FOLD_ATTEMPT() ((void) 0)
# define DC_DBG_FOLD_ABORT()   ((void) 0)
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

/*
 * PHASE 3: a sharded LRU, the mainline-shaped arm.
 *
 * The kernel shards by (NUMA node x memcg) and takes that shard's spinlock for
 * every list mutation.  Sharding is what keeps producer-vs-producer off one
 * lock; the remaining contention is producer-vs-shrinker, which mainline bounds
 * (batch-isolate under the lock, process outside it) rather than eliminating.
 * This arm reproduces that shape.  The MCAS-bidir alternative -- which removes
 * producer/consumer contention outright at the price of a descriptor on every
 * enqueue and unlink -- is the second arm, and design/dcache-lru-txn.md section 7
 * says which wins is decided by RECLAIM CADENCE, not by readers: bursty reclaim
 * favours the lock, continuous eviction favours MCAS.
 *
 * Insert at TAIL, evict from HEAD (oldest first), rotate to tail on second
 * chance -- the kernel's order.
 */
#define DCACHE_LRU_TYPES
#include "dcache_lru.h"		/* PHASE 3: shard types + axis arms */
#undef DCACHE_LRU_TYPES


/*
 * Placement mirrors dcache_seqlock's (and the kernel's): the read-mostly geometry
 * every hop reads on a line no writer touches, each written word on its own line
 * (rename_gen is written by the counter arms).  dc_create allocates the struct
 * 64-byte aligned.
 */
struct dcache {
	struct dc_lru_shard *lru;		/* nlru shards; see lru_shard_index */
	unsigned int nlru;
	struct urcu_txn_sw_hlist_head *buckets;
	unsigned long mask;			/* nbuckets - 1 (power of two) */
	struct dentry *root;
	/*
	 * Escalation domain (fair-mutex fallback lane) for the NAMESPACE INDEX.
	 * Vestigial: every index commit is pure SW (commit_sw takes no domain --
	 * it cannot contention-abort, so it has no lane).  urcu_txn_init() still
	 * wants one.  (It was live under the retired DC_CHAIN_SWMW, whose shell
	 * ops carried MW records.)
	 */
	dc_domain_t domain __attribute__((aligned(64)));
#if defined(DC_LRU_MCAS) && !defined(DC_NO_LRU)
	/*
	 * A SEPARATE domain for the LRU, and the separation is load-bearing.
	 *
	 * rcu-txn-list.h says a domain should be shared by lists "that form ONE
	 * logical structure" -- and the LRU is not part of the namespace index.
	 * They share no slots, and one has no business funnelling the other
	 * through a fair mutex.  Sharing dc->domain would have done exactly that
	 * under the retired DC_CHAIN_SWMW, whose shell ops were also MW and could
	 * escalate: an escalation raised by a rename would capture the LRU's
	 * commits too.
	 *
	 * That failure mode is not hypothetical here.  f9b6901a fixed a bug where
	 * every lane holder re-asserted domain->active, so ONE escalation captured
	 * the whole domain -- worth 2.65x at 192 writers.  Widening a domain to
	 * cover unrelated structures is the same mistake with extra steps.
	 */
	dc_domain_t lru_domain __attribute__((aligned(64)));
#endif

	/*
	 * Walk-causality generation (rename_lock's job, NOT d_seq's -- see
	 * rename-shell-transition.md).  A single global counter bumped inside
	 * every rename's shell-stack MCAS commit; a reader brackets its whole
	 * path walk on it and retries if it moved (a rename touched the tree
	 * mid-walk).  Stored as a transacted void* slot so the bump composes
	 * atomically with the structural edge change; the reader resolves it
	 * with bl_read().  Kept EVEN (stepped by 2) so bit 0 -- the
	 * engine proxy tag -- is always clear on a plain value.
	 */
	void *rename_gen __attribute__((aligned(64)));
};

/* Engine proxy tag for the rename_gen slot (bit 0; values stay even). */
#define DC_GEN_TAG	URCU_TXN_TAG

/*
 * Engine proxy tag for the host's d_top, written by store_sw in the stack and
 * exchange commits; readers resolve the slot with bl_read() (it may briefly
 * hold a commit record).  Node addresses are >= 8-byte aligned, so bit 0 is
 * clear on every live value the slot holds.  (d_fwd, write-once and set before
 * publication, never holds one; its reads keep bl_read() for uniformity.)
 */
#define DC_FWD_TAG	URCU_TXN_TAG

/*
 * Engine proxy tag for the transacted d_parent slot (bit 0; hosts are aligned).
 * Writer-only: read via the txn in the cross-dir loop check and via
 * bl_read() in the quiescent walk; never on the downward reader path.
 */
#define DC_PARENT_TAG	URCU_TXN_TAG

#define hnode_dentry(n) caa_container_of((n), struct dentry, d_hash)

/* ---- hashing (same mix as the seqlock engine) -------------------------- */

static inline struct urcu_txn_sw_hlist_head *bucket_of(struct dcache *dc,
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
	return "bucketlock-mark";
#elif defined(DC_PER_NODE_GEN)
	return "bucketlock-pernode";
#else
	return "bucketlock";
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
/* phase 3: sharded-lock LRU + CLOCK shrinker (per-NUMA-node shards) */
#ifdef DC_TXN_STATS
const int dc_txn_stats_supported = 1;
#else
const int dc_txn_stats_supported = 0;
void dc_txn_stats_dump(void *stream) { (void) stream; }
void dc_txn_stats_last(void *stream) { (void) stream; }
#endif

#ifdef DC_NO_LRU
const int dc_lru_supported = 0;
#else
const int dc_lru_supported = 1;
#endif

/* rmdir-to-negative: see dcache.h.  free here -- the lock dc_add takes is the one the invariant needs */
const int dc_delete_dir_supported = 1;

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
	d->d_top = NULL;
	d->d_parent = parent;
	d->d_dc = dc;
	urcu_txn_sw_hlist_init(&d->d_child_head);
	d->d_id = id;
	d->d_inode = positive ? 1 : 0;
#ifdef DC_HOT1CL
	if (!positive)			/* phase 2: this name is known ABSENT */
		d->d_iparent = (struct dentry *)
			((uintptr_t) d->d_iparent | DC_TAG_NEG);
#endif
	d->d_isdir = (unsigned char) (isdir != 0);
#ifndef DC_IPARENT_TXN
	d->d_seq = NULL;		/* per-node gen (DC_PER_NODE_GEN); even */
#endif
	return d;
}

#ifndef DC_NO_LRU
/*
 * LIVENESS FOR THE LRU -- mainline retain_dentry's first test (d_unhashed),
 * which this port omitted.  See lru_push_prepare() in dcache_lru.h.
 *
 * ⛔ AND ON THIS ENGINE IT CANNOT BE TRANSACTED, which is the whole reason the
 * two engines differ here.  &d->d_hash.next is NOT an MCAS-managed slot: it is
 * written by bl_hlist_del_locked() with a PLAIN __atomic_store_n under the
 * bucket lock.  Recording an MCAS guard on it would plant a proxy that the
 * bucket-lock writer's plain store then overwrites -- and this transaction's
 * settle would afterwards store the pre-mark value back over that writer's
 * mark, RESURRECTING a node the index has already deleted.  A guard that
 * corrupts the thing it guards is worse than no guard.
 *
 * So this is a plain resolved read and records nothing: it NARROWS the window
 * (the unhash can still land between the read and the commit) rather than
 * closing it, and saying so is the point of having two spellings.
 *
 * Closing it here needs the bucket lock -- the analogue of mainline's d_lock,
 * and the same lock lru_evict_settled() takes to unhash -- on the enqueue path.
 * That is a real cost on a hot path and a separate decision; measure the
 * residual with -DDC_LRU_FREE_ASSERT before paying it.
 */
#ifdef DC_LRU_MCAS
/* 0: this engine CANNOT transact the guard (see the comment above).  The header
 * branches on it -- lru_retain's re-arm is compiled out here as a result. */
#define DC_LRU_ALIVE_TRANSACTED 0
static int lru_alive_validate(struct urcu_txn *txn, struct dentry *d)
{
	int marked = 0;

	(void) txn;
	(void) bl_hlist_resolve(rcu_dereference(d->d_hash.next), &marked);
	return marked ? -ENOENT : 0;
}

#else
static int lru_alive_hint(struct dentry *d)
{
	int marked = 0;

	(void) bl_hlist_resolve(rcu_dereference(d->d_hash.next), &marked);
	return !marked;
}
#endif
#endif	/* DC_NO_LRU */

#include "dcache_lru.h"		/* PHASE 3: the shared LRU (see the header) */


struct dcache *dc_create(unsigned int nbuckets)
{
	struct dcache *dc;
	unsigned int n = 1, i;
	struct qstr rootname;

	/* 64-byte aligned, or struct dcache's per-line placement is fiction. */
	if (posix_memalign((void **) &dc, 64, sizeof(*dc)) != 0)
		return NULL;
	memset(dc, 0, sizeof(*dc));
	while (n < nbuckets)
		n <<= 1;
	dc->buckets = calloc(n, sizeof(*dc->buckets));
	if (!dc->buckets) {
		free(dc);
		return NULL;
	}
	for (i = 0; i < n; i++)
		urcu_txn_sw_hlist_init(&dc->buckets[i]);
	dc->mask = n - 1;
#ifndef DC_NO_LRU
	if (lru_shards_init(dc) != 0) {		/* shared; see dcache_lru.h */
		free(dc->buckets);
		free(dc);
		return NULL;
	}
#endif
	/* vestigial (pure-SW commits), see struct dcache */
	urcu_txn_domain_init(&dc->domain);

	dc_qstr_init(&rootname, "");
	dc->root = dentry_alloc(dc, NULL, &rootname, 0, 1, 1); /* root: dir, positive */
	dc->root->d_parent = dc->root;		/* root is its own parent */
	dc->root->d_iparent = dc->root;
	return dc;
}

/* Quiescent teardown: recurse the child-hlist, free every node. */
static void free_subtree(struct dentry *d)
{
	struct urcu_txn_sw_hlist_node *n = bl_hlist_first_rcu(&d->d_child_head);

	while (n) {
		struct urcu_txn_sw_hlist_node *next = bl_hlist_next_rcu(n);

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
	 * Two barriers: the first drains every queued fold worker (each moves
	 * its shell's name into the host or finds the shell unhashed, then
	 * call_rcu's the shell's own free); the second drains those shell frees.
	 * After both, every content host is its own named top again and no shell
	 * remains, so the child-hlists hold only hosts for free_subtree to
	 * reclaim directly.
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
 * The value @host's d_top holds while @top names its entry: NULL when the host
 * is its own top.  The expected-old (and new) of every d_top write.
 */
static inline struct dentry *dtop_val(struct dentry *host, struct dentry *top)
{
	return top == host ? NULL : top;
}


/*
 * Resolve @top's content host in O(1) (readers).  A settled top (d_fwd == NULL)
 * IS its own host.  A shell (d_fwd != NULL) holds a WRITE-ONCE skip pointer to
 * its host in its d_id/d_host union slot -- read as a pointer only because
 * d_fwd != NULL proves @top is a shell (a stable per-node property).  The d_fwd
 * load is the discriminator and is ordered before the union access, so a host's
 * id is never dereferenced as a pointer.
 */
static inline struct dentry *host_of_rcu(struct dentry *top)
{
#ifdef DC_HOT1CL
	/* discriminate from the tag on the already-loaded d_iparent -- no d_fwd
	 * read, so a settled hop never leaves the hot cacheline. */
	return node_is_shell(top) ? rcu_dereference(top->d_host) : top;
#else
	struct dentry *fwd = bl_read((void **) &top->d_fwd, DC_FWD_TAG);

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
	struct urcu_txn_sw_hlist_head *b = bucket_of(dc, parent, name->hash);
	struct urcu_txn_sw_hlist_node *n;

	for (n = bl_hlist_first_rcu(b); n; n = bl_hlist_next_rcu(n)) {
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
	struct urcu_txn_sw_hlist_head *b = bucket_of(dc, parent, name->hash);
	struct urcu_txn_sw_hlist_node *n;

	for (n = bl_hlist_first_rcu(b); n; n = bl_hlist_next_rcu(n)) {
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
/* PHASE 2: pos/neg is authoritative on the content HOST (see dcache.h).  Free
 * when no rename is in flight -- top IS the host and @raw is already its word. */
static inline int host_is_positive(uintptr_t raw, struct dentry *host)
{
	if (caa_likely(!(raw & DC_TAG_SHELL)))
		return (raw & DC_TAG_NEG) == 0;
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
 * Walk causality (bucket lock engine is MARK-ONLY for now): there is no generation
 * counter and no bump.  A rename's demote sets the hlist deletion MARK on the
 * outgoing top in the same SW commit that removes it from the index, and the
 * localized reader observes it via top_unhashed_rcu -- the structural edit IS
 * the signal.  So the bucket lock shell path owes no gen bump (the counter arms
 * GLOBAL/PER-NODE would SW-record on dc->rename_gen / host->d_seq here; carry
 * them by restoring an SW txn_bump_gen).  Enforce the mark arm for the shell
 * ops, whose reader-side causality this file only implements for the mark.
 */
#ifndef DC_MARK_GEN
# error "bucket lock engine shell ops (rename/exchange/fold) are DC_MARK_GEN-only for now"
#endif

#ifdef DC_LOCALIZED_GEN
/*
 * Is @top no longer the current indexed top for its name?  A rename (demote) and
 * an unlink both MARK the node's own d_hash.next -- atomically, in the same
 * commit that bumps the host gen (the hlist del contract + txn_bump_gen).  The
 * per-node reader tests this right AFTER sampling the host gen: it is the
 * "re-verify the edge under the sample" step a single host counter needs, since
 * the counter is reached only after the name match + host resolve (there is no
 * pre-navigation point at which to sample it).  Call under rcu_read_lock().
 */
static inline int top_unhashed_rcu(struct dentry *top)
{
	int marked = 0;

	/* Resolve d_hash.next (an SW proxy mid-commit, else a plain pointer) and
	 * report its deletion MARK: a del sets it, so a marked next means the top
	 * has left the name index. */
	(void) bl_hlist_resolve(rcu_dereference(top->d_hash.next), &marked);
	return marked;
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
	return (uintptr_t) bl_read(&host->d_seq, DC_GEN_TAG);
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
	return (uintptr_t) bl_read(&node->d_seq, DC_GEN_TAG);
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

		g0 = bl_read(&dc->rename_gen, DC_GEN_TAG);	/* acquire */
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
			/* pos/neg is authoritative on the HOST (phase 2, see
			 * dcache.h): an inode is content and a rename must not
			 * disturb it.  Free when no rename is in flight -- top IS
			 * the host and @raw is already its word. */
			res = DC_HOST_IS_POSITIVE(top, raw, d) ? DC_POSITIVE
							   : DC_NEGATIVE;
#ifdef DC_TEST_HOOKS
			if (dc_test_walk_hook)
				dc_test_walk_hook((int) i);
#endif
		}
		cmm_smp_rmb();			/* walk loads before re-reading gen */
		g1 = bl_read(&dc->rename_gen, DC_GEN_TAG);
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
	 * reached only after the name match + host resolve (there is no
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

/*
 * WRITER-side path resolve.  Distinct from dc_lookup's walk, which is the
 * lockless reader and does not come through here -- and that separation is what
 * lets this mark LRU recency without touching the reader at all.
 *
 * The kernel sets DCACHE_REFERENCED on the ref-taking paths, never on the RCU
 * walk: __d_lookup_rcu takes no reference, so it never dputs and never marks
 * anything.  A ref-walk dgets and dputs each component, so the PREFIX it passed
 * through is what gets marked.  This is that, exactly: the reader stays free and
 * the writers supply the second-chance signal.
 *
 * TEST-THEN-SET rather than an unconditional store.  The bit lives on the
 * dedicated LRU line, which LRU splices already bounce between cores; storing
 * unconditionally would invalidate every ancestor's line on every write op,
 * whereas a hot directory is already marked, so the common case is a shared-state
 * LOAD and no invalidation.
 */
static struct dentry *resolve(struct dcache *dc, const struct dc_path *p,
			      uint32_t depth)
{
	struct dentry *cur = dc->root;
	uint32_t i;

	for (i = 0; i < depth; i++) {
		cur = __child_lookup(dc, cur, &p->comp[i]);
		if (!cur)
			return NULL;
		lru_retain(dc, cur);
	}
	return cur;
}

/* Resolve the transacted d_parent slot (RCU-side; call within a read section). */
static inline struct dentry *parent_of_rcu(struct dentry *d)
{
	return bl_read((void **) &d->d_parent, DC_PARENT_TAG);
}

/* Is @d's child-hlist empty?  Call within an RCU read-side section. */
static int children_empty(struct dentry *d)
{
	return bl_hlist_first_rcu(&d->d_child_head) == NULL;
}

static void dentry_free_cb(struct rcu_head *rh);

/*
 * Queue the reclaim.  The CALLER does this, after the victim is off the LRU;
 * it used to be the tail of lru_evict_settled(), which condemned the node while
 * a shard could still reach it.  See the long note in dcache_txn.c.
 */
static inline void dc_reclaim(struct dentry *d)
{
	call_rcu(&d->d_rcu, dentry_free_cb);
}

/* ---- add / unlink ------------------------------------------------------ */

/*
 * Is @host's entry still named?  Its named top is the host's d_top, or the host
 * itself when that is NULL; the entry is alive while that top is in the index.
 * A rename moves d_top and marks the old top in one commit, so a top found
 * marked with d_top MOVED meanwhile is a rename, not a death: look again.  A
 * marked top that is still d_top is an unlink (which leaves d_top alone) or an
 * eviction.  Call with @host's child head locked (dc_add_typed), which the
 * unlink and the shrinker hold while they remove the top.
 */
static int entry_alive(struct dentry *host)
{
	for (;;) {
		struct dentry *t = bl_read((void **) &host->d_top, DC_FWD_TAG);
		struct dentry *top = t ? t : host;
		int marked = 0;

		(void) bl_hlist_resolve(rcu_dereference(top->d_hash.next),
					&marked);
		if (!marked)
			return 1;
		if (bl_read((void **) &host->d_top, DC_FWD_TAG) == t)
			return 0;
	}
}

static int dc_add_typed(struct dcache *dc, const struct dc_path *path,
			uint64_t id, int isdir, int positive)
{
	struct dentry *parent, *d;
	const struct qstr *name;
	struct urcu_txn_sw_hlist_head *bucket;

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
	/* CHEAP REJECT ONLY.  The binding test is the one under the bucket lock
	 * below; this one exists to answer -EEXIST without allocating. */
	if (__child_lookup(dc, parent, name))
		return -EEXIST;
	d = dentry_alloc(dc, parent, name, id, isdir, positive);
	if (!d)
		return -ENOMEM;
	bucket = bucket_of(dc, parent, name->hash);

	/*
	 * Publish into BOTH indexes -- name hash (d_hash) + parent child list
	 * (d_sib) -- with plain marked stores under the two head locks (address-
	 * ordered).  Single-writer under the lock, so no selector: each add is a
	 * lone pointer publish, already atomic to a reader.  NOT atomic across the
	 * two heads (a reader may see hash- before child-visible), matching the
	 * kernel-faithful seqlock baseline.  __child_lookup above already rejected a
	 * duplicate name (racy under concurrent same-name adds -- disjoint in the
	 * churn workload; harden with a re-check under the lock if ever needed).
	 */
	bl_lock2(bucket, &parent->d_child_head);
	/*
	 * RE-CHECK the parent under the lock: a concurrent dc_delete of an empty
	 * DIRECTORY makes it negative, and a negative must never gain a child --
	 * a walk through it has to find nothing beneath a name that is not there.
	 * dc_delete holds this same d_child_head while it checks children_empty
	 * and flips the state, so this test under this lock is what makes the two
	 * atomic with respect to each other.
	 *
	 * The cost is ONE PREDICTED LOAD-AND-BRANCH inside a critical section the
	 * add already entered -- no new lock, no read-set entry, nothing on the
	 * reader.  That is the whole price of rmdir-to-negative on this engine,
	 * and it is only this cheap because the lock the fold and the add already
	 * share happens to be the one the invariant needs.  (A FILE parent needs
	 * none of this: d_isdir is write-once, so -ENOTDIR above already answers
	 * and every negative file is unreachable as a parent by construction.)
	 */
	if (!DC_IS_POSITIVE(parent)) {
		bl_unlock2(bucket, &parent->d_child_head);
		free(d);			/* never published */
		return -ENOENT;
	}
	/*
	 * ⭐ AND RE-CHECK THAT THE PARENT'S ENTRY IS STILL ALIVE -- a DIFFERENT
	 * question from "is it positive".  Two paths free a directory's host
	 * while an add may be resolving under it: the shrinker (a settled host)
	 * and an unlink (since 2026-10-01 an unlink frees the host even when a
	 * rename shell names the entry -- there is no chain left to do it later).
	 * Each holds the host's OWN child head (this lock) while it checks
	 * children_empty and removes the entry's top, so:
	 *
	 *   add first    -> the remover blocks on this child head, then sees a
	 *                   non-empty child list and refuses (or skips);
	 *   remove first -> the entry's top is already marked, and THIS test is
	 *                   what stops the add.
	 *
	 * Without it, publishing here leaves a child hashed under a host one
	 * grace period from being freed.  The test is on the entry's TOP, not
	 * the host: a host a shell names is legitimately out of the index (its
	 * own d_hash reads MARKED while the directory is alive -- testing the
	 * host rejected every add under a renamed directory, test_dcache "name
	 * recreated over a moved directory").  See entry_alive().
	 *
	 * ⚠ TSAN finds the shrinker race; ASan does NOT, because the churn
	 * recycles the parent's storage before the sweeper's write lands.  Do
	 * not read an ASan pass as coverage for it.
	 *
	 * -ENOENT is the right answer and callers already expect it: it means
	 * "the prefix went".  The root reads as alive (d_hash.next NULL, hence
	 * unmarked), so adds directly under it are unaffected.
	 */
#ifndef DC_TEST_NO_ADD_ALIVE
	if (!entry_alive(parent)) {
		bl_unlock2(bucket, &parent->d_child_head);
		free(d);		/* never published */
		return -ENOENT;
	}
#endif
#ifndef DC_NO_ADD_DUP_RECHECK
	/*
	 * ⭐ RE-CHECK THE NAME UNDER THE BUCKET LOCK -- the check above is a
	 * CHECK-THEN-ACT and two concurrent adds of one name both pass it.
	 *
	 * The pre-check runs with no lock held, so nothing stops a peer from
	 * publishing the same (parent, name) between it and this critical
	 * section: both then reach the publish below and the bucket ends up
	 * holding TWO dentries spelled alike.  A lookup resolves whichever the
	 * chain reaches first while a child-list walk descends the other, so
	 * everything added under the loser is reachable by dc_walk/dc_readdir
	 * and ABSENT to dc_lookup -- the two indexes disagreeing permanently,
	 * with nothing to unhash them.
	 *
	 * The old comment here said the race was "disjoint in the churn
	 * workload".  That stopped being true when the churn bench gained
	 * rebuild_prefix(), which has every writer recreate the SAME directory
	 * names after an eviction and relies on -EEXIST to arbitrate.  Measured
	 * on this tree: /p0/p1 held up to 33 children where 16 exist, on BOTH
	 * LRU arms.
	 *
	 * The bucket lock is the right place for it, and this is what the
	 * kernel-faithful seqlock baseline already does -- its exists-test and
	 * its insert are both under the bucket lock, with a comment saying so.
	 * Same (parent, name) hashes to the same bucket by construction, so one
	 * lock covers every racer for this name.  Cost is one more walk of a
	 * chain this thread already holds locked and just touched, on the miss
	 * path only.
	 *
	 * ⚠ NOT the child head: siblings share a parent but scatter across
	 * buckets, so scanning the child list would be O(children) instead of
	 * O(chain) and would still be the WRONG lock for a name.
	 */
	if (__child_lookup(dc, parent, name)) {
		bl_unlock2(bucket, &parent->d_child_head);
		free(d);			/* never published */
		return -EEXIST;
	}
#endif
	bl_hlist_add_head_locked(bucket, &d->d_hash);
	bl_hlist_add_head_locked(&parent->d_child_head, &d->d_sib);
	bl_unlock2(bucket, &parent->d_child_head);
	/* PHASE 3: on the LRU at the TAIL (newest), AFTER publishing and outside
	 * the bucket locks -- the LRU has no reader, so it need not be atomic with
	 * the index edit, and taking a shard lock under a bucket lock would add an
	 * ordering edge between two lock classes for nothing. */
	lru_add(dc, d);
	return 0;
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

/* Phase 2 -- see dcache.h.  A negative dentry is a leaf by construction. */
int dc_add_negative(struct dcache *dc, const struct dc_path *path)
{
	return dc_add_typed(dc, path, 0, 0, 0);
}

/*
 * Phase 2: d_instantiate.  Publishes inode-ness on the content HOST through a
 * single-slot commit on the transacted d_iparent -- the same discipline the fold
 * uses to hand identity down, so a reader resolving that slot sees old or new
 * and never a tear.  This engine deleted d_seq like the txn engines did, so a
 * plain in-place store here would be exactly the write-once-per-identity
 * violation the layout assumes away.
 */
int dc_instantiate(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	struct dentry *parent, *top, *host;
	const struct qstr *name;
	struct urcu_txn_sw_hlist_head *bucket;
	int ret = 0;

	if (path->ndepth == 0)
		return -EEXIST;			/* the root is always positive */;

	rcu_read_lock();
	parent = resolve(dc, path, path->ndepth - 1);
	if (!parent) {
		rcu_read_unlock();
		return -ENOENT;
	}
	name = &path->comp[path->ndepth - 1];
	bucket = bucket_of(dc, parent, name->hash);

	for (;;) {
		int marked = 0;

		top = find_top_rcu(dc, parent, name);
		if (!top) {
			ret = -ENOENT;
			goto out;
		}
		/*
		 * Take the NAMED TOP'S bucket -- the same head the fold's
		 * TRANSFER holds across its identity handover -- then RE-VERIFY
		 * the top under it.  A concurrent fold can transfer between the
		 * find and the acquire, which leaves the old top unhashed and
		 * makes a different node the top (of this same bucket, since a
		 * transfer preserves the name), so an unverified acquire would
		 * lock the right bucket and then edit the wrong host.  Same
		 * re-verify dc_unlink does, for the same reason.
		 */
		bl_lock(bucket);
		(void) bl_hlist_resolve(rcu_dereference(top->d_hash.next),
					&marked);
		if (marked) {
			bl_unlock(bucket);
			continue;		/* re-find the current top */
		}
		host = host_of_rcu(top);	/* O(1) */
		ret = dc_set_negative(host, 0, id);
		bl_unlock(bucket);
		break;
	}
out:
	rcu_read_unlock();
	return ret;
}

int dc_delete(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *parent, *top, *host;
	const struct qstr *name;
	struct urcu_txn_sw_hlist_head *bucket;
	struct dentry *isdir_locked = NULL;	/* host whose child head we hold */
	int ret = 0;

	if (path->ndepth == 0)
		return -EISDIR;			/* the root cannot be removed */

	rcu_read_lock();
	parent = resolve(dc, path, path->ndepth - 1);
	if (!parent) {
		rcu_read_unlock();
		return -ENOENT;
	}
	name = &path->comp[path->ndepth - 1];
	bucket = bucket_of(dc, parent, name->hash);

	for (;;) {
		int marked = 0;

		top = find_top_rcu(dc, parent, name);
		if (!top) {
			ret = -ENOENT;
			goto out;
		}
		/*
		 * Take the NAMED TOP'S bucket -- the same head the fold's
		 * TRANSFER holds across its identity handover -- then RE-VERIFY
		 * the top under it.  A concurrent fold can transfer between the
		 * find and the acquire, which leaves the old top unhashed and
		 * makes a different node the top (of this same bucket, since a
		 * transfer preserves the name), so an unverified acquire would
		 * lock the right bucket and then edit the wrong host.  Same
		 * re-verify dc_unlink does, for the same reason.
		 */
		if (isdir_locked)
			bl_lock2(bucket, &isdir_locked->d_child_head);
		else
			bl_lock(bucket);
		(void) bl_hlist_resolve(rcu_dereference(top->d_hash.next),
					&marked);
		if (marked) {
			if (isdir_locked)
				bl_unlock2(bucket,
					   &isdir_locked->d_child_head);
			else
				bl_unlock(bucket);
			isdir_locked = NULL;
			continue;		/* re-find the current top */
		}
		host = host_of_rcu(top);	/* O(1) */
		if (host->d_isdir && host != isdir_locked) {
			/*
			 * A DIRECTORY needs its OWN child-list head locked too,
			 * and that is the whole cost of rmdir-to-negative here:
			 * a negative must not be able to GAIN a child, and for a
			 * file d_isdir gives that free (dc_add answers -ENOTDIR),
			 * but a directory can legitimately take one.  So
			 * children_empty has to still hold at the flip -- which
			 * means excluding dc_add, which locks its parent's
			 * d_child_head.  That is the head below, so THE LOCK IS
			 * ALREADY IN THE RIGHT PLACE and dc_add pays nothing.
			 *
			 * Re-acquire both address-ordered rather than grabbing
			 * the child head with the bucket already in hand: both
			 * are bucket-head-class locks and that class is
			 * address-ordered (bl_lock2), so taking them out of
			 * order would deadlock against a dc_add that wants the
			 * same pair.  Cheap, because it only happens for
			 * directories and only on the first pass.
			 */
			bl_unlock(bucket);
			isdir_locked = host;
			continue;
		}
		if (host->d_isdir && !children_empty(host)) {
			bl_unlock2(bucket, &host->d_child_head);
			ret = -ENOTEMPTY;
			goto out;
		}
		/* d_id deliberately NOT cleared: clearing it either side of
		 * the flip races a reader that sampled the other side, no
		 * reader reads a negative's id, and dc_instantiate rewrites
		 * it before republishing.  The census skips negatives. */
		ret = dc_set_negative(host, 1, 0);
		if (host->d_isdir)
			bl_unlock2(bucket, &host->d_child_head);
		else
			bl_unlock(bucket);
		break;
	}
out:
	rcu_read_unlock();
	return ret;
}

static void dentry_free_cb(struct rcu_head *rh)
{
	struct dentry *d = caa_container_of(rh, struct dentry, d_rcu);

	/* -DDC_LRU_FREE_ASSERT; inert otherwise.  ONE definition, shared with
	 * dcache_txn.c -- see lru_assert_not_queued() in dcache_lru.h for why
	 * that matters (this probe used to live here, and its absence from the
	 * other engine turned two handoff rows into vacuous zeros). */
	lru_assert_not_queued(d);
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
 *   MID-TRANSITION (top is a rename shell): the entry dies all the same, so
 *   unlink frees the HOST after a grace period and leaves the shell to the fold
 *   its rename queued, which finds it out of the index and frees it without
 *   touching the host (see fold()).
 *
 * The host's OWN child head is locked too, with children_empty re-checked
 * under it: that is the lock dc_add publishes a child under, so an add either
 * lands first (and this refuses -ENOTEMPTY) or finds the entry dead
 * (entry_alive).  Checked only unlocked, an add could slip in between and
 * leave a child hashed under the freed host.
 *
 * Loops re-finding the top: a concurrent fold that transfers (promotes the host
 * into the index) between the find and the del makes our re-verify fail, so we
 * re-find the new top and remove that instead.
 */
int dc_unlink(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *parent, *top, *host;
	const struct qstr *name;
	struct urcu_txn_sw_hlist_head *bucket;
	struct urcu_txn_sw_hlist_head *heads[3];
	int settled, ret, can_free, nheads;

	if (path->ndepth == 0)
		return -EINVAL;

	parent = resolve(dc, path, path->ndepth - 1);
	if (!parent)
		return -ENOENT;
	name = &path->comp[path->ndepth - 1];
	bucket = bucket_of(dc, parent, name->hash);

	rcu_read_lock();			/* keeps top/host alive across the unlink */
	for (;;) {
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
		/* top == host iff top is settled (not a shell) -- host_of_rcu()
		 * already answered this; no d_fwd read needed. */
		settled = (top == host);

		/*
		 * Lock the two heads (address-ordered) and remove top from BOTH
		 * indexes with plain marked stores.  RE-VERIFY top is still the
		 * hashed top under the lock: a concurrent fold also takes the bucket
		 * lock, so it can only have transferred top between our find and our
		 * acquire, and a transfer leaves the old top unhashed -> re-find.
		 *
		 * NO walk-causality bump -- unlink REMOVES, never RELOCATES: the
		 * removed node is EMPTY (children_empty), so it is a terminal a reader
		 * can only straddle at the leaf (present-before / absent-after are both
		 * valid), never an interior waypoint.  The del's MARK is what a
		 * straddling reader's top_unhashed_rcu observes; no gen is owed.
		 */
		heads[0] = bucket;
		heads[1] = &parent->d_child_head;
		nheads = 2;
		/*
		 * A directory's OWN child head too, which excludes a dc_add
		 * under it (dc_add locks its parent's child head) across the
		 * children_empty re-check below: the host is freed whether or
		 * not a shell names it.  The kernel pays the same, vfs_rmdir
		 * locks the victim's inode.  Measured: an empty-directory
		 * add/unlink churn against 32-64 concurrent listers runs ~5%
		 * slower for it (the lock word shares a line with the d_sib
		 * the listers read); without listers ~1%.  Not for a FILE:
		 * dc_add answers -ENOTDIR under one and d_isdir is write-once,
		 * so no child can land.
		 */
#ifndef DC_TEST_NO_RMDIR_LOCK
		if (host->d_isdir)
			heads[nheads++] = &host->d_child_head;
#endif
		bl_lock_n(heads, nheads);
		{	/* re-verify top is still hashed under the lock: the deletion
			 * MARK on d_hash.next fires if a concurrent unlink or a fold
			 * transfer removed it between our find and our acquire. */
			int marked = 0;

			(void) bl_hlist_resolve(
				rcu_dereference(top->d_hash.next), &marked);
			if (marked) {
				bl_unlock_n(heads, nheads);
				continue;	/* re-find the current top */
			}
		}
		if (!children_empty(host)) {	/* an add landed first */
			bl_unlock_n(heads, nheads);
			ret = -ENOTEMPTY;
			goto out;
		}
		if (host->d_isdir)
			DC_TEST_UNLINK_DELAY();	/* test-only window widener */
		bl_hlist_del_locked(&top->d_hash);
		bl_hlist_del_locked(&top->d_sib);
		bl_unlock_n(heads, nheads);
		break;				/* removed from both indexes */
	}
	/* PHASE 3: off the LRU IMMEDIATELY, never lazily.  The node's call_rcu
	 * free cannot fire while a shard still points at it, so deferring this to
	 * the shrinker would gate reclaim on memory pressure instead of on the
	 * grace period -- unbounded live memory under churn without pressure.
	 * See design/dcache-lru-txn.md section 6.
	 *
	 * ⭐ ... EXCEPT when the shrinker is already holding @host, and then the
	 * free is DELEGATED rather than taken: lru_del_can_free() answers 0, and
	 * the shrinker call_rcu's it the moment its own eviction attempt lands.
	 * This is mainline __dentry_kill's `can_free = false` for a dentry on a
	 * shrink list.  Taking it off here instead would disown a victim mid-
	 * eviction, which is exactly the free-while-queued defect. */
	can_free = lru_del_can_free(dc, host, 1);
	if (!settled)
		lru_del(dc, top);	/* the shell is its fold's to free */
	rcu_read_unlock();
	if (can_free)			/* the entry dies: free its host */
		call_rcu(&host->d_rcu, dentry_free_cb);
	return 0;
out:
	rcu_read_unlock();
	return ret;
}

/* ---- the shell-vehicle move (async stack + fold) ----------------------- */

#define DC_LOOP_MAX 256		/* ancestry-walk cap: a transient cycle from an
				 * in-flight concurrent reparent -> re-walk */

/*
 * Cross-dir cycle check: moving @host under @new_parent loops iff @host is an
 * ancestor of @new_parent, so walk @new_parent -> root looking for @host, using
 * PLAIN resolving loads (parent_of_rcu).  Concurrency safety is the move-in-
 * progress flag (the caller grayed @host->d_moving with a fenced RMW BEFORE
 * calling this; a flagged ancestor means a concurrent move is in flight ->
 * -EAGAIN).  Dekker set-before-check: two moves that would jointly cycle cannot
 * both pass.  A committed concurrent move is caught by the plain read + the
 * cur == host test.  The root anchors every path, so relocating it is always a
 * cycle -- and rejecting it up front is also load-bearing: the walk is bounded
 * `cur != root` and exits BEFORE its `cur == host` test could fire on the root,
 * so a root-as-host would otherwise slip through.  Returns 0, -EINVAL (cycle),
 * or -EAGAIN (concurrent move on the ancestry: caller re-finds and retries).
 */
static int cross_cycle_check(struct dcache *dc, struct dentry *host,
			     struct dentry *new_parent)
{
	struct dentry *cur = new_parent;
	int hops = 0;

	if (host == dc->root)
		return -EINVAL;
	while (cur != dc->root) {
		if (cur == host)			/* committed ancestry cycle */
			return -EINVAL;
		if (uatomic_load(&cur->d_moving, CMM_RELAXED))
			return -EAGAIN;			/* concurrent move on the ancestry */
		if (++hops > DC_LOOP_MAX)
			return -EAGAIN;			/* transient cycle: re-walk */
		cur = parent_of_rcu(cur);		/* plain resolving load, no validate */
	}
	return 0;
}

/*
 * STACK one entry into an ALREADY-OPEN txn (records only -- no cycle check, no
 * commit).  Removes @top from BOTH indexes and inserts @shell -- the new named
 * top, a shell of @top's entry (shell->d_host = shell->d_fwd = host) -- into
 * @new_bucket + @new_parent's child-hlist, and records the demote (the host's
 * d_top = shell) in the same commit.
 *
 * The CYCLE CHECK is the caller's job, run via cross_cycle_check() BEFORE taking
 * the bucket locks -- so the (bounded, O(depth)) ancestry walk does not
 * lengthen the lock hold (it needs only host->d_moving, grayed earlier).  This
 * records only INDEX edges (+ the cross-parent reparent); when @cross_parent it
 * also stores host->d_parent = new_parent.
 *
 * The caller owns the SW txn (init + commit) and holds the four bucket-head
 * locks, so the records are single-writer.
 */
static void stack_one_prepare(dc_swtxn_t *txn,
			      struct dentry *top, struct dentry *host,
			      struct dentry *new_parent,
			      struct urcu_txn_sw_hlist_head *new_bucket,
			      struct urcu_txn_sw_hlist_head *from_bucket,
			      struct dentry *shell, int cross_parent)
{
	/*
	 * Record the reader-visible INDEX edges into the SW selector: swap top for
	 * the shell in both indexes and (cross-parent) reparent host.  One selector
	 * flip makes the index change atomic to readers.  The caller holds the
	 * bucket-head locks for all four heads, so these are the only writers of
	 * those chains.
	 *
	 * When the old and new heads COINCIDE (same-dir rename shares the child head;
	 * a hash collision shares the bucket), a del(top) + add(shell) would record
	 * TWO edges on that one head->first slot when top is first -- a same-slot
	 * conflict the SW engine cannot reconcile (no RYW).  So detect the coincidence
	 * and use an in-place bl_sw_replace (shell takes top's exact slot), which
	 * rewrites only *top->pprev and still MARKs top; otherwise del + add-head into
	 * two distinct heads.
	 *
	 * The DEMOTE (the host's d_top moves from top to shell) is a store_SW in
	 * THIS commit, so a fold resolves (index mark, d_top) against ONE control
	 * word -- never a torn pair.  d_top's writers are this demote, the
	 * exchange's and a fold's TRANSFER, all holding the bucket lock of the
	 * entry's current top, so the slot is single-writer and a plain park
	 * suffices.  With the index edits also store_sw, the stack stays a pure-SW
	 * commit (commit_sw).
	 */
	if (from_bucket == new_bucket) {		/* same hash bucket */
		bl_sw_replace(txn, &top->d_hash, &shell->d_hash);
	} else {
		bl_sw_del_marked(txn, &top->d_hash);
		bl_sw_add_head(txn, &shell->d_hash, new_bucket);
	}
	if (!cross_parent) {				/* same parent -> same child head */
		bl_sw_replace(txn, &top->d_sib, &shell->d_sib);
	} else {
		bl_sw_del_marked(txn, &top->d_sib);
		bl_sw_add_head(txn, &shell->d_sib, &new_parent->d_child_head);
	}
	(void) urcu_txn_store_sw(txn, (void **) &host->d_top,
				      dtop_val(host, top), shell, DC_FWD_TAG);
	if (cross_parent)
		(void) dc_sw_record(txn, (void **) &host->d_parent,
					  parent_of_rcu(host), new_parent,
					  DC_PARENT_TAG);
}

/*
 * The rename's destination, re-checked UNDER new_bucket and new_parent's child
 * head -- the caller's __child_lookup() is a check-then-act holding nothing, so
 * two renames to one name, or a rename racing a dc_add of it, would otherwise
 * both publish and leave two dentries spelled alike in one bucket.
 *
 * A NEGATIVE, settled (not a shell) FILE occupant is REPLACED, as the
 * kernel's d_move drops a negative target: it is removed from both indexes
 * here, with the same marked stores dc_unlink uses, and handed back in *@neg
 * for the caller to reclaim after a grace period.  It is removed just before
 * the commit that publishes the shell rather than inside it, so a lockless
 * reader of this name in between sees ABSENT where it saw NEGATIVE -- the same
 * answer (the name does not exist), at the miss path's cost.  Holding
 * new_bucket excludes dc_instantiate of the occupant (it takes the named top's
 * bucket), so the state judged here cannot change under us.  Files only, which
 * every negative is: no child can be added under a file, so the freed node
 * needs no child-head seal.  Any other occupant: -EEXIST.
 */
#ifndef DC_NO_RENAME_DEST_RECHECK
static int dest_claim_locked(struct dcache *dc, struct dentry *new_parent,
			     const struct qstr *new_name, struct dentry *self,
			     struct dentry **neg)
{
	struct dentry *cur = find_top_rcu(dc, new_parent, new_name);

	if (!cur)
		return 0;
	if (cur == self || host_of_rcu(cur) != cur || DC_IS_POSITIVE(cur) ||
	    cur->d_isdir)
		return -EEXIST;
	bl_hlist_del_locked(&cur->d_hash);
	bl_hlist_del_locked(&cur->d_sib);
	*neg = cur;
	return 0;
}
#endif

/* Reclaim a negative dest_claim_locked() dropped (dc_unlink's settled tail). */
static void dest_reclaim(struct dcache *dc, struct dentry *neg)
{
	if (neg && lru_del_can_free(dc, neg, 1))
		call_rcu(&neg->d_rcu, dentry_free_cb);
}


/*
 * STACK.  The index edits (store_sw, bucket-locked) and the DEMOTE (the host's
 * d_top = shell) ride ONE commit (stack_one_prepare), so a fold reads (index,
 * d_top) resolved against one control word.  The commit is pure-SW, abort-free
 * under the bucket locks.  DISCIPLINE (inherited from the mixed front-end):
 * begin FIRST; d_moving + bucket locks AFTER begin; every terminal bail
 * abandon()+end(), every retry conflict()+end().
 *
 * Returns 0 (shell in *out_shell, host in *out_host), -ENOMEM, -ENOENT, or -EINVAL
 * (directory cycle).  Loops internally, re-finding the top.
 */
static int stack_shell(struct dcache *dc,
		struct dentry *from_parent, const struct qstr *from_name,
		struct dentry *new_parent, const struct qstr *new_name,
		int cross_parent,
		struct dentry **out_shell, struct dentry **out_host)
{
	struct urcu_txn_sw_hlist_head *from_bucket =
		bucket_of(dc, from_parent, from_name->hash);
	struct urcu_txn_sw_hlist_head *new_bucket =
		bucket_of(dc, new_parent, new_name->hash);
	struct dentry *shell = dentry_alloc(dc, new_parent, new_name, 0, 0, 1);
	struct dentry *top = NULL, *host = NULL, *neg = NULL;
	struct urcu_txn txn;
	int ret;

	if (!shell)
		return -ENOMEM;
#ifdef DC_HOT1CL
	shell->d_iparent = (struct dentry *)
		((uintptr_t) shell->d_iparent | DC_TAG_SHELL);
#endif

	rcu_read_lock();			/* keeps top/host alive across the commit */
	urcu_txn_init(&txn, &dc->domain);
	/* The stack writes only DISTINCT slots (both indexes + the demote + the
	 * cross-parent reparent are disjoint), and it is a PURE store_sw commit (the
	 * SPMC enqueue is single-producer under the bucket lock), so declare disjoint
	 * and commit through the branch-lean commit_sw. */
	urcu_txn_declare_disjoint(&txn);
	for (;;) {
		struct urcu_txn_sw_hlist_head *heads[4];
		enum urcu_txn_status st;
		int marked = 0;

		/*
		 * begin() FIRST: an escalated writer PARKS on the fair mutex inside
		 * begin, so d_moving / the bucket locks must be taken AFTER it (else a
		 * parked writer holds them and stalls the lane -- the discipline the
		 * pure-SW build could drop because SW has no lane).
		 */
		urcu_txn_begin(&txn);
		DC_RENAME_CONFLICT_HINT(&txn);

		top = find_top_rcu(dc, from_parent, from_name);
		if (!top) {			/* concurrently removed */
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			ret = -ENOENT;
			goto out_free;
		}
		host = host_of_rcu(top);	/* O(1); invariant across renames/folds */
		shell->d_host = host;		/* union slot = skip pointer to tail host */
		shell->d_fwd = host;		/* write-once: marks a shell (see struct) */

		/*
		 * Gray host for the cross-dir cycle Dekker, AFTER begin.  test-and-set
		 * (not a plain OR): two writers must not both drive a move of the SAME
		 * node; the cmpxchg's full barrier is the StoreLoad the walk needs.
		 */
		if (cross_parent &&
		    uatomic_cmpxchg(&host->d_moving, 0UL, 1UL) != 0UL) {
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			continue;		/* another writer owns this host's move */
		}

		/*
		 * Cross-dir cycle check BEFORE any lock (flag-based plain-read walk --
		 * the ping-pong win; engine-agnostic).  Its result is independent of top
		 * (host is rename-invariant), so it need not be re-run under the lock.
		 */
		if (cross_parent) {
			int c = cross_cycle_check(dc, host, new_parent);

			if (c) {		/* -EINVAL cycle (terminal) / -EAGAIN (retry) */
				uatomic_and(&host->d_moving, ~1UL);
				if (c == -EINVAL) {
					urcu_txn_abandon(&txn);
					urcu_txn_end(&txn);
					ret = -EINVAL;
					goto out_free;
				}
				urcu_txn_conflict(&txn);
				urcu_txn_end(&txn);
				continue;	/* re-find + retry */
			}
		}

		/*
		 * Bucket heads (address-ordered) AFTER begin, then RE-VERIFY top is
		 * still hashed under the lock (a concurrent unlink / fold transfer
		 * leaves the old top's d_hash.next MARKED -> re-find the current top).
		 */
		heads[0] = from_bucket;
		heads[1] = &from_parent->d_child_head;
		heads[2] = new_bucket;
		heads[3] = &new_parent->d_child_head;
		bl_lock_n(heads, 4);
		(void) bl_hlist_resolve(rcu_dereference(top->d_hash.next), &marked);
		if (marked) {
			bl_unlock_n(heads, 4);
			if (cross_parent)
				uatomic_and(&host->d_moving, ~1UL);
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;		/* re-find the current top */
		}
		/* See the other stack_shell: a negative destination directory
		 * must refuse a rename INTO it, checked under its child head. */
		if (!DC_IS_POSITIVE(new_parent)) {
			bl_unlock_n(heads, 4);
			if (cross_parent)
				uatomic_and(&host->d_moving, ~1UL);
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			ret = -ENOENT;
			goto out_free;
		}
#ifndef DC_NO_RENAME_DEST_RECHECK
		/* The destination re-check under new_bucket (this variant had
		 * none: see dest_claim_locked), replacing a negative occupant. */
		if (dest_claim_locked(dc, new_parent, new_name, top, &neg)) {
			bl_unlock_n(heads, 4);
			if (cross_parent)
				uatomic_and(&host->d_moving, ~1UL);
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			ret = -EEXIST;
			goto out_free;
		}
#endif

		stack_one_prepare(&txn, top, host, new_parent, new_bucket,
				  from_bucket, shell, cross_parent);
		st = urcu_txn_commit_sw(&txn);	/* pure-SW: index + demote, one commit */
		DC_TS_COMMIT(DC_TS_STACK, &txn, st);
		bl_unlock_n(heads, 4);
		urcu_txn_end(&txn);
		if (cross_parent)
			uatomic_and(&host->d_moving, ~1UL);
		if (st == URCU_TXN_STATUS_ABORT)
			continue;		/* abort-free under the lock; defensive */
		if (st < 0) {			/* MEMORY_ERROR: nothing published */
			ret = -ENOMEM;
			goto out_free;
		}
		break;				/* committed: entry now named anew */
	}
	rcu_read_unlock();
	dest_reclaim(dc, neg);
	*out_shell = shell;
	if (out_host)
		*out_host = host;
	return 0;
out_free:
	rcu_read_unlock();
	dest_reclaim(dc, neg);		/* dropped before an OOM commit */
	free(shell);
	return ret;
}


/*
 * FOLD.  Runs from a call_rcu callback a grace period after shell @n was
 * stacked.  There is no chain between an entry's named top and its host (see
 * the txn engine's fold() for the history and the argument): a fold either
 * moves @n's name into the host or frees @n, decided by whether @n is still in
 * the index.  Every step is serialized by the bucket lock of @n's slot, which a
 * rename (its demote) or an unlink of the entry also takes:
 *
 *   @n still indexed: it is the entry's named top (the host's d_top).
 *   TRANSFER: copy @n's identity into the host, replace @n by the host in BOTH
 *   indexes (one SW commit, atomic to readers), and reset d_top to NULL -- a
 *   plain store under @n's bucket lock, which excludes the demote.  The
 *   identity copy is safe pre-publish for the reasons the txn fold gives; the
 *   handover is an ATOMIC RMW because d_delete writes the host's pos/neg from
 *   another thread without this lock (dc_transfer_iparent()).
 *
 *   @n no longer indexed: demoted by a later rename, or removed by an unlink.
 *   Garbage either way, and the host is not this fold's: free @n, touch neither
 *   the host nor @n's old directory.
 *
 * The membership test runs FIRST without a lock, and only an indexed @n has its
 * index locks taken (and the test repeated under them): a demoted @n's old
 * directory may have emptied, been unlinked and been freed before this fold
 * runs, so locking its child head could touch freed memory.  An indexed @n sits
 * in its parent's child list, so the parent cannot be unlinked meanwhile, and
 * this RCU read-side section keeps it allocated across a race with that.
 */
static void fold(struct dcache *dc, struct dentry *n)
{
	struct dentry *host = host_of_rcu(n);	/* write-once d_host */
	int reclaim_n = 1;
	int marked = 0;

	rcu_read_lock();
	DC_DBG_FOLD_ATTEMPT();
	(void) bl_hlist_resolve(rcu_dereference(n->d_hash.next), &marked);
	if (!marked) {
		struct dentry *parent = parent_of_rcu(n);
		struct urcu_txn_sw_hlist_head *heads[2];

		heads[0] = bucket_of(dc, parent, n->d_iname.hash);
		heads[1] = &parent->d_child_head;
		bl_lock_n(heads, 2);
		(void) bl_hlist_resolve(rcu_dereference(n->d_hash.next), &marked);
		if (!marked) {
#if defined(DC_HOT1CL)
			dc_transfer_iparent(host, n);
#else
			host->d_iparent = n->d_iparent;
#endif
			DC_NAME_XFER_BEGIN(host);
			host->d_iname = n->d_iname;
			DC_NAME_XFER_END(host);
			{
				struct urcu_txn txn;
				enum urcu_txn_status st;

				/* no lane: an abort-free disjoint commit already
				 * serialized by the bucket locks, run with them held */
				urcu_txn_init(&txn, NULL);
				urcu_txn_declare_disjoint(&txn);
				urcu_txn_begin(&txn);
				bl_sw_replace(&txn, &n->d_hash, &host->d_hash);
				bl_sw_replace(&txn, &n->d_sib, &host->d_sib);
				st = urcu_txn_commit_sw(&txn);
				DC_TS_COMMIT(DC_TS_FOLD, &txn, st);
				urcu_txn_end(&txn);
				if (st == URCU_TXN_STATUS_OK)
					uatomic_store(&host->d_top, NULL,
						      CMM_RELEASE);	/* host names itself */
				else		/* OOM: nothing published, n still indexed */
					reclaim_n = 0;	/* do NOT free a live top */
			}
		}
		bl_unlock_n(heads, 2);
	}
	rcu_read_unlock();
#ifdef DC_STRESS_DEBUG
	uatomic_inc(&dc_dbg_folds);
#endif
	/*
	 * ⭐ OFF THE LRU BEFORE THE FREE.  resolve() marks recency on the HOST,
	 * and lru_del_can_free(.., 1) is the same kill the unlink and the shrinker
	 * use.  Its answer is not consulted: the shrinker cannot be holding a
	 * shell (lru_evict_settled skips anything with d_fwd set).
	 */
	if (reclaim_n) {
		(void) lru_del_can_free(dc, n, 1);
		call_rcu(&n->d_rcu, dentry_free_cb);	/* reclaim @n after a GP */
	}
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
	{
		struct dentry *dest = __child_lookup(dc, to_parent, to_name);

		/* a negative is replaced (d_move); itself is not a target */
		if (dest && (dest == victim || DC_IS_POSITIVE(dest)))
			return -EEXIST;
	}
	/*
	 * dc_add is NOT the only way a directory gains a child -- a rename INTO
	 * it is the other, and a NEGATIVE directory must refuse both (see
	 * dc_delete).  Advisory here; stack_shell re-checks under the destination
	 * child head lock, which is where it is made atomic against dc_delete.
	 */
	if (!DC_IS_POSITIVE(to_parent))
		return -ENOENT;

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
	call_rcu(&shell->d_rcu, fold_cb);	/* hands the name back to the host */
	return 0;
}


/*
 * EXCHANGE.  The four index REPLACES (store_sw, bucket-locked) and the two
 * DEMOTES (hosta->d_top = sa, hostb->d_top = sb) ride ONE commit, so a fold
 * reads (index, d_top) resolved against one control word.  store_sw, so the
 * commit is abort-free under the bucket heads (see stack_one_prepare).
 * DISCIPLINE as stack_shell: begin FIRST; d_moving + bucket locks AFTER begin;
 * every terminal bail abandon()+end(), every retry conflict()+end(), releasing
 * both flags on all paths.
 */
int dc_rename_exchange(struct dcache *dc, const struct dc_path *ap,
		       const struct dc_path *bp)
{
	struct dentry *pa, *pb, *hosta, *hostb, *sa, *sb;
	struct urcu_txn_sw_hlist_head *bucket_a, *bucket_b;
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
	sa = dentry_alloc(dc, pb, nb, 0, 0, 1); /* A's new top (name nb, parent pb) */
	sb = dentry_alloc(dc, pa, na, 0, 0, 1); /* B's new top (name na, parent pa) */
	if (!sa || !sb) {
		free(sa);
		free(sb);
		return -ENOMEM;
	}
#ifdef DC_HOT1CL
	sa->d_iparent = (struct dentry *) ((uintptr_t) sa->d_iparent | DC_TAG_SHELL);
	sb->d_iparent = (struct dentry *) ((uintptr_t) sb->d_iparent | DC_TAG_SHELL);
#endif

	rcu_read_lock();			/* keeps tops/hosts alive across the commit */
	urcu_txn_init(&txn, &dc->domain);
	urcu_txn_declare_disjoint(&txn);	/* pure-SW: 4 replaces + 2 demotes + 2 reparents, all distinct */
	for (;;) {
		struct urcu_txn_sw_hlist_head *heads[4];
		struct dentry *topa, *topb;
		enum urcu_txn_status st;
		int ma = 0, mb = 0, ca, cb;

		urcu_txn_begin(&txn);

		topa = find_top_rcu(dc, pa, na);
		topb = find_top_rcu(dc, pb, nb);
		if (!topa || !topb) {		/* an entry vanished */
			urcu_txn_abandon(&txn);
			urcu_txn_end(&txn);
			ret = -ENOENT;
			goto out_free;
		}
		hosta = host_of_rcu(topa);	/* O(1); invariant per entry */
		hostb = host_of_rcu(topb);
		sa->d_host = hosta;		/* A's shell forwards to A's old top */
		sa->d_fwd = hosta;	/* write-once shell flag (see struct) */
		sb->d_host = hostb;		/* B's shell forwards to B's old top */
		sb->d_fwd = hostb;

		/*
		 * Gray BOTH hosts (address-ordered) AFTER begin (Dekker): two exchanges
		 * of the same pair cannot livelock; release a partial acquire on retry.
		 */
		if (cross) {
			struct dentry *lo = hosta < hostb ? hosta : hostb;
			struct dentry *hi = hosta < hostb ? hostb : hosta;

			if (uatomic_cmpxchg(&lo->d_moving, 0UL, 1UL) != 0UL) {
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				continue;	/* lo owned by another mover: retry */
			}
			if (uatomic_cmpxchg(&hi->d_moving, 0UL, 1UL) != 0UL) {
				uatomic_and(&lo->d_moving, ~1UL);
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				continue;	/* hi owned by another mover: retry */
			}
		}

		/*
		 * Cycle checks BEFORE any lock (both hosts grayed): hosta must not be an
		 * ancestor of pb, nor hostb of pa.  Independent of the tops, so no re-run
		 * under the lock.
		 */
		ca = cross ? cross_cycle_check(dc, hosta, pb) : 0;
		cb = (cross && !ca) ? cross_cycle_check(dc, hostb, pa) : 0;
		if (ca || cb) {
			if (cross) {
				uatomic_and(&hosta->d_moving, ~1UL);
				uatomic_and(&hostb->d_moving, ~1UL);
			}
			if (ca == -EINVAL || cb == -EINVAL) {
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				ret = -EINVAL;
				goto out_free;
			}
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;		/* -EAGAIN: re-find + retry */
		}

		/*
		 * Bucket heads (address-ordered) AFTER begin, then RE-VERIFY both tops
		 * are still hashed under the lock (a marked top means a concurrent
		 * unlink / fold transfer moved it -> re-find).
		 */
		heads[0] = bucket_a;
		heads[1] = bucket_b;
		heads[2] = &pa->d_child_head;
		heads[3] = &pb->d_child_head;
		bl_lock_n(heads, 4);
		(void) bl_hlist_resolve(rcu_dereference(topa->d_hash.next), &ma);
		(void) bl_hlist_resolve(rcu_dereference(topb->d_hash.next), &mb);
		if (ma || mb) {
			bl_unlock_n(heads, 4);
			if (cross) {
				uatomic_and(&hosta->d_moving, ~1UL);
				uatomic_and(&hostb->d_moving, ~1UL);
			}
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;		/* re-find + retry (marked top) */
		}

		/*
		 * Record the four index replaces (sb takes topa's slot, sa takes
		 * topb's), coalescing to a two-node replace when the tops share a head.
		 */
		if (bucket_a == bucket_b)
			bl_sw_replace2(&txn, &topa->d_hash, &sb->d_hash,
					&topb->d_hash, &sa->d_hash);
		else {
			bl_sw_replace(&txn, &topa->d_hash, &sb->d_hash);
			bl_sw_replace(&txn, &topb->d_hash, &sa->d_hash);
		}
		if (!cross)			/* same parent -> same child head */
			bl_sw_replace2(&txn, &topa->d_sib, &sb->d_sib,
					&topb->d_sib, &sa->d_sib);
		else {
			bl_sw_replace(&txn, &topa->d_sib, &sb->d_sib);
			bl_sw_replace(&txn, &topb->d_sib, &sa->d_sib);
		}
		/* Each entry's new shell becomes its top in the SAME commit: a
		 * store_SW, single-writer under topX's bucket heads (see
		 * stack_one_prepare). */
		(void) urcu_txn_store_sw(&txn, (void **) &hosta->d_top,
					      dtop_val(hosta, topa), sa, DC_FWD_TAG);
		(void) urcu_txn_store_sw(&txn, (void **) &hostb->d_top,
					      dtop_val(hostb, topb), sb, DC_FWD_TAG);
		if (cross) {			/* reparent both hosts (store_sw) */
			(void) urcu_txn_store_sw(&txn, (void **) &hosta->d_parent,
						      parent_of_rcu(hosta), pb,
						      DC_PARENT_TAG);
			(void) urcu_txn_store_sw(&txn, (void **) &hostb->d_parent,
						      parent_of_rcu(hostb), pa,
						      DC_PARENT_TAG);
		}
		st = urcu_txn_commit_sw(&txn);	/* atomic swap: pure store_sw */
		DC_TS_COMMIT(DC_TS_XCHG, &txn, st);
		bl_unlock_n(heads, 4);
		urcu_txn_end(&txn);
		if (cross) {
			uatomic_and(&hosta->d_moving, ~1UL);
			uatomic_and(&hostb->d_moving, ~1UL);
		}
		if (st == URCU_TXN_STATUS_ABORT)
			continue;		/* abort-free under the locks; defensive */
		if (st < 0) {			/* MEMORY_ERROR: nothing published */
			ret = -ENOMEM;
			goto out_free;
		}
		break;				/* swap committed */
	}
	rcu_read_unlock();
#ifdef DC_STRESS_DEBUG
	uatomic_add(&dc_dbg_renames, 2);
#endif
	call_rcu(&sa->d_rcu, fold_cb);
	call_rcu(&sb->d_rcu, fold_cb);
	return 0;
out_free:
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
	struct urcu_txn_sw_hlist_node *n;

	/* Skip NEGATIVES: the census counts OBJECTS, and a negative dentry holds
	 * a name without one -- its d_id is stale by construction (dc_delete
	 * cannot clear it without racing a reader; dc_add_negative never set
	 * it).  Reporting it would make a conservation gate read a cached
	 * absence as a surviving object. */
	if ((path->ndepth > 0 || parent_of_rcu(d) != d) && DC_IS_POSITIVE(d))
		fn(d->d_id, path, arg);

	for (n = bl_hlist_first_rcu(&d->d_child_head); n;
	     n = bl_hlist_next_rcu(n)) {
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

#ifndef DC_NO_LRU
/*
 * Evict one SETTLED dentry: remove it from both indexes and RCU-defer the free,
 * which is exactly dc_unlink's core taking the dentry instead of a path.
 *
 * It takes the dentry rather than reconstructing a path on purpose.  A host's
 * OWN d_iname is stale whenever a shell is stacked above it -- the current name
 * lives on the top -- so building a path from a victim would name it wrongly
 * mid-rename.  Settled nodes have no such gap: a settled node IS its own top, so
 * its name and parent are current and the bucket is computable directly.
 *
 * Anything mid-transition is therefore SKIPPED rather than handled.  That is a
 * real policy choice and a cheap one: renames are transient, so the node is a
 * candidate again on the next pass, and mainline does the same thing whenever it
 * cannot get d_lock (LRU_SKIP).  Returns 0, or -EAGAIN to skip.
 */
static int lru_evict_settled(struct dcache *dc, struct dentry *d)
{
	struct dentry *parent;
	struct urcu_txn_sw_hlist_head *bucket;
	struct urcu_txn_sw_hlist_head *heads[3];
	int marked = 0;

	if (bl_read((void **) &d->d_top, DC_FWD_TAG) ||
	    uatomic_load(&d->d_fwd, CMM_RELAXED))
		return -EAGAIN;			/* renamed host, or a shell */
	parent = parent_of_rcu(d);
	if (!parent || parent == d)
		return -EAGAIN;			/* the root anchors the tree */
	bucket = bucket_of(dc, parent, d->d_iname.hash);

	/*
	 * THREE heads, not two, and the third is the point: @d's OWN child head.
	 *
	 * children_empty(d) below decides whether a whole subtree is about to be
	 * orphaned, and dc_add publishes a new child by taking exactly that lock
	 * -- so without it this is a check-then-act and an add can land between
	 * the test and the del.  The victim is then freed while a child of it is
	 * hashed and on the LRU, naming memory one grace period from release.
	 *
	 * ⚠ THE TWO LOCKS ARE NOT THE SAME ONE.  This function holds the
	 * PARENT'S child head (it is removing @d from that list); dc_add holds
	 * @d's.  They only look alike.  Reading them as one lock is what hid
	 * this: it makes the pair appear complete when neither half exists.
	 *
	 * bl_lock_n sorts by address and de-duplicates, so adding a third head
	 * introduces no new deadlock edge -- the whole class stays
	 * address-ordered.
	 */
	heads[0] = bucket;
	heads[1] = &parent->d_child_head;
	heads[2] = &d->d_child_head;
	bl_lock_n(heads, 3);
	/* RE-VERIFY under the locks, the same way dc_unlink does: still hashed,
	 * still childless, still settled.  A rename or an unlink can have landed
	 * between the isolate and this acquire. */
	(void) bl_hlist_resolve(rcu_dereference(d->d_hash.next), &marked);
	if (marked || !children_empty(d) ||
	    bl_read((void **) &d->d_top, DC_FWD_TAG)) {
		bl_unlock_n(heads, 3);
		return -EAGAIN;
	}
	bl_hlist_del_locked(&d->d_hash);
	bl_hlist_del_locked(&d->d_sib);
	bl_unlock_n(heads, 3);
	/* ⭐ The caller queues the reclaim, once the victim is off the LRU --
	 * see dc_reclaim().  Returning 0 means "committed; the free is yours". */
	return 0;
}

#include "dcache_lru_shrink.h"	/* PHASE 3: the shared CLOCK shrinker */

#endif	/* DC_NO_LRU */


void dc_walk(struct dcache *dc, dc_visit_fn fn, void *arg)
{
	struct dc_path path;

	dc_path_reset(&path);
	rcu_read_lock();			/* host_of_rcu resolves d_host */
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
	struct urcu_txn_sw_hlist_node *n;
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
	for (n = bl_hlist_first_rcu(&dir->d_child_head); n;
	     n = bl_hlist_next_rcu(n)) {
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

/* ---- reverse walk (dentry_path_raw) ------------------------------------- */

/*
 * The object behind @p, as a handle for dc_dentry_path(): its content HOST, the
 * node a rename never relocates.  Resolved the way the mutators locate their
 * target (the soft per-component resolve), under one read-side section.
 */
struct dentry *dc_lookup_dentry(struct dcache *dc, const struct dc_path *p)
{
	struct dentry *d = dc->root;
	uint32_t i;

	rcu_read_lock();
	for (i = 0; i < p->ndepth && d; i++)
		d = txn_child_lookup_rcu(dc, d, &p->comp[i]);
	if (d && !DC_IS_POSITIVE(d))
		d = NULL;
	rcu_read_unlock();
	return d;
}

/*
 * The node that currently NAMES @host: its d_top, or @host itself when that is
 * NULL -- one load.  Read through bl_read, since the stack and exchange commit
 * it as a record.  Call under rcu_read_lock: a shell is freed a grace period
 * after its fold.  A name read off the returned top is never mid-TRANSFER: the
 * fold copies the shell's name into the host before the commit that publishes
 * the host, and resets d_top only after it, under the same bucket lock; a walk
 * that still reads the (now unhashed) shell in between reads the same name and
 * parent the host now carries.
 */
static inline struct dentry *named_top_rcu(struct dentry *host)
{
	struct dentry *t = bl_read((void **) &host->d_top, DC_FWD_TAG);

	return t ? t : host;
}

/*
 * dentry_path_raw, with this engine's walk causality -- the MARK double collect
 * its forward reader runs: each component's (name, parent) read off its named
 * top, the top confirmed still unmarked on the way up and re-tested on the way
 * back.  Marks are monotone, so every top named its host across the turnaround
 * and the path existed then.  See dcache_txn.c's dc_dentry_path for the arms.
 */
int dc_dentry_path(struct dcache *dc, const struct dentry *d,
		   struct dc_path *out)
{
	struct dentry *handle = (struct dentry *) (uintptr_t) d;
#ifdef DC_MARK_GEN
	struct dentry *latch[DC_PATH_MAX];
#endif
	uint32_t n, k;
	int ret;

	rcu_read_lock();
	for (;;) {
		struct dentry *host = handle;
		int stale = 0;
#ifndef DC_MARK_GEN
		void *g0 = bl_read(&dc->rename_gen, DC_GEN_TAG);
#endif

		n = 0;
		ret = 0;
		while (host != dc->root) {
			struct dentry *top;

			if (n == DC_PATH_MAX) {
				ret = -ENAMETOOLONG;
				break;
			}
			top = named_top_rcu(host);
			DC_INAME_COPY(&out->comp[DC_PATH_MAX - 1 - n], top);
#ifdef DC_MARK_GEN
			cmm_smp_rmb();		/* identity before the confirm */
			if (top_unhashed_rcu(top)) {
				stale = 1;	/* demoted under us: re-climb */
				break;
			}
			latch[n] = top;
#endif
			host = DC_IPARENT(top);
			n++;
		}
		if (ret)
			break;
		if (stale)
			continue;
		cmm_smp_rmb();			/* the climb before the check */
#ifdef DC_MARK_GEN
		for (k = 0; k < n; k++)
			if (top_unhashed_rcu(latch[k]))
				stale = 1;
		if (!stale)
			break;
#else
		if (bl_read(&dc->rename_gen, DC_GEN_TAG) == g0)
			break;
#endif
	}
	rcu_read_unlock();
	if (ret)
		return ret;
	(void) k;
	memmove(&out->comp[0], &out->comp[DC_PATH_MAX - n],
		n * sizeof(out->comp[0]));
	out->ndepth = n;
	return 0;
}
