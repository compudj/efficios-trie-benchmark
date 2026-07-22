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

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu-qsbr.h>			/* generic rcu_* names => QSBR flavor */
#include <urcu-call-rcu.h>
#include <urcu/rcu-txn.h>		/* AFTER the RCU flavor */
#include <urcu/rcu-txn-hlist.h>

#include "dcache.h"

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
	return (uintptr_t) d->d_iparent;
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
#if defined(DC_HOT1CL_SPLIT) && !defined(DC_SPLIT_KEEPID)
const int dc_lookup_id_is_address = 1;
#else
const int dc_lookup_id_is_address = 0;
#endif

static struct dentry *dentry_alloc(struct dcache *dc, struct dentry *parent,
				   const struct qstr *name, uint64_t id,
				   int isdir)
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
	d->d_inode = 1;
	d->d_isdir = (unsigned char) (isdir != 0);
#ifndef DC_IPARENT_TXN
	d->d_seq = NULL;		/* per-node gen (DC_PER_NODE_GEN); even */
#endif
	return d;
}

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

	dc_qstr_init(&rootname, "");
	dc->root = dentry_alloc(dc, NULL, &rootname, 0, 1);	/* root is a directory */
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

		if (DC_IPARENT(d) == parent && dc_qstr_eq(&d->d_iname, name))
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
		    dc_qstr_eq(&d->d_iname, name)) {
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
#define DC_IS_POSITIVE_RAW(top, raw)	(((raw) & DC_TAG_NEG) == 0)
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
#define DC_IS_POSITIVE_RAW(top, raw)	DC_IS_POSITIVE(top)
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
			/* pos/neg is read off the WRITE-ONCE top, not the host: a
			 * fold's TRANSFER mutates the host's d_iparent in place, so
			 * reading it there would race that write (the host is
			 * reachable via the d_host skip pointer, not just the
			 * index).  The top is never mutated and carries the same
			 * pos/neg (rename preserves inode-ness). */
			res = DC_IS_POSITIVE_RAW(top, raw) ? DC_POSITIVE
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
			/* pos/neg off the WRITE-ONCE top (not the host, whose
			 * d_iparent a fold's TRANSFER mutates in place) -- see the
			 * global arm above. */
			res = DC_IS_POSITIVE_RAW(top, raw) ? DC_POSITIVE
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

/* ---- add / unlink ------------------------------------------------------ */

static int dc_add_typed(struct dcache *dc, const struct dc_path *path,
			uint64_t id, int isdir)
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
	d = dentry_alloc(dc, parent, name, id, isdir);
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
		urcu_txn_begin(&txn);
		p = urcu_txn_hlist_insert_head_prepare(&txn, &d->d_hash, bucket);
		if (!p)
			p = urcu_txn_hlist_insert_head_prepare(&txn, &d->d_sib,
							&parent->d_child_head);
		if (p == -EAGAIN) {		/* an old first node is mid-delete */
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;
		}
		if (p) {			/* -ENOENT: a head is sealed */
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
	return dc_add_typed(dc, path, id, 1);
}

int dc_add_file(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	return dc_add_typed(dc, path, id, 0);
}

static void dentry_free_cb(struct rcu_head *rh)
{
	free(caa_container_of(rh, struct dentry, d_rcu));
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
	int settled, ret;

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
	rcu_read_unlock();
	if (settled)				/* host has no fold queued: free it */
		call_rcu(&top->d_rcu, dentry_free_cb);
	/* else: top is a shell; its pending fold RECLAIMs the orphaned chain */
	return 0;
out:
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
			     struct dentry *shell, int cross_parent)
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

	p = urcu_txn_hlist_del_prepare(txn, &top->d_hash);
	if (p)					/* -ENOENT: top demoted; -EAGAIN */
		return p;
	p = urcu_txn_hlist_insert_head_prepare(txn, &shell->d_hash, new_bucket);
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
	struct dentry *shell = dentry_alloc(dc, new_parent, new_name, 0, 0);
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
				      shell, cross_parent);
		if (p) {
			if (p == -EINVAL) {	/* the move would create a cycle: terminal */
				urcu_txn_abandon(&txn);	/* forfeit turn -> end() releases lane */
				urcu_txn_end(&txn);
				if (cross_parent)
					uatomic_and(&host->d_moving, ~1UL);
				ret = -EINVAL;
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
			 * These are plain in-place stores to a node (m) that
			 * concurrent readers CAN reach (as the content host, via
			 * the d_host skip pointer) -- but no reader reads a host's
			 * d_iparent/d_iname: matching and pos/neg are taken off the
			 * write-once TOP (dc_lookup), and readdir reads the top's
			 * d_iname + host's d_id.  So this write races no reader. */
			struct dentry *m = fwd;
#if   defined(DC_HOT1CL)
			/* adopt n's parent + pos/neg, keep m's OWN host/shell bit
			 * (m may still be a middle relay of the remaining chain) */
			m->d_iparent = (struct dentry *) (
				((uintptr_t) n->d_iparent & ~DC_TAG_SHELL) |
				((uintptr_t) m->d_iparent & DC_TAG_SHELL));
#else
			m->d_iparent = n->d_iparent;	/* pre-publish: m unindexed */
#endif
			m->d_iname = n->d_iname;

			urcu_txn_begin(&txn);
			DC_FOLD_CONFLICT_HINT(&txn);
			p = urcu_txn_hlist_replace_prepare(&txn, &n->d_hash,
							   &m->d_hash);
			if (p == -ENOENT) {
				/* @n out of the index; a still-NULL d_back means an
				 * unlink removed it (a re-rename would have set
				 * d_back) -> tear the orphaned chain down. */
				urcu_txn_conflict(&txn);
				urcu_txn_end(&txn);
				if (urcu_txn_read((void **) &n->d_back,
						   DC_FWD_TAG) == NULL)
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
	if (host_to_free)
		call_rcu(&host_to_free->d_rcu, dentry_free_cb);
done:
	rcu_read_unlock();
#ifdef DC_STRESS_DEBUG
	uatomic_inc(&dc_dbg_folds);
#endif
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
	sa = dentry_alloc(dc, pb, nb, 0, 0); /* A's new top */
	sb = dentry_alloc(dc, pa, na, 0, 0); /* B's new top */
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
				      cross);		/* A -> (pb, nb) */
		if (!p)
			p = stack_one_prepare(&txn, dc, topb, hostb, pa, bucket_a,
					      sb, cross);	/* B -> (pa, na) */
		if (p) {
			if (p == -EINVAL) {	/* the swap would create a cycle: terminal */
				urcu_txn_abandon(&txn);
				urcu_txn_end(&txn);
				if (cross) {
					uatomic_and(&hosta->d_moving, ~1UL);
					uatomic_and(&hostb->d_moving, ~1UL);
				}
				ret = -EINVAL;
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

	if (path->ndepth > 0 || parent_of_rcu(d) != d)
		fn(d->d_id, path, arg);

	for (n = urcu_txn_hlist_first_rcu(&d->d_child_head); n;
	     n = urcu_txn_hlist_next_rcu(n)) {
		struct dentry *top = sib_dentry(n);
		struct dentry *host = host_of_rcu(top);		/* O(1) */

		if (path->ndepth >= DC_PATH_MAX)
			continue;
		path->comp[path->ndepth++] = top->d_iname;	/* current name */
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

		if (fn)
			fn(host->d_id, &top->d_iname, arg);
		count++;
	}
	rcu_read_unlock();
	return count;
}
