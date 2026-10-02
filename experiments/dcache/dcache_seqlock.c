// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_seqlock.c -- faithful kernel-style userspace dentry cache.
 *
 * This is the BASELINE the urcu-txn port (dcache_txn, S2) must beat and simplify.
 * It reproduces the kernel's actual RCU-walk consistency scheme (checked against
 * Linux v7.3 fs/namei.c + fs/dcache.c):
 *
 *   - a (parent, name-hash) hash table of RCU hlists (the dentry_hashtable /
 *     hlist_bl analog); lockless readers traverse a bucket with rcu_dereference;
 *   - a per-dentry d_seq seqcount, validated HAND-OVER-HAND: the child's d_seq
 *     is sampled (odd bit masked) before its name/parent compare, the parent's
 *     is re-checked once the child is found (lookup_fast) and the child's once
 *     more before stepping into it (step_into).  That is the fast path's ONLY
 *     per-hop validation;
 *   - a GLOBAL rename_lock seqlock, bumped by every d_move.  The fast path does
 *     NOT validate it on a hit ("Rename seqlock is not required here", lookup_
 *     fast): path_init only SAMPLES it (waiting out an in-flight d_move), and a
 *     MISS falls back to d_lookup, which retries while it moved -- a rename can
 *     make the lockless chain scan miss a dentry that is there.
 *
 * So a kernel walk guarantees that consecutive components overlapped in time,
 * NOT that the whole path existed at one instant.  That stronger SNAPSHOT
 * guarantee is what the kernel reserves for its reverse walks (d_path,
 * dentry_path_raw, d_walk, is_subdir), which bracket the whole walk on
 * rename_lock with read_seqbegin_or_lock -- lockless first, then a retry that
 * takes rename_lock and so cannot fail again.
 *
 * Two build arms, because the txn engines give the snapshot guarantee on every
 * lookup and a fair comparison needs both anchors:
 *
 *   default            the kernel's forward fast path, as above.  Pairs with
 *                      the txn per-node / mark arms (which give MORE: a
 *                      snapshot) and is the "versus the kernel's lookup" line.
 *   -DDC_SEQ_SNAPSHOT  dc_lookup brackets the whole forward walk on rename_lock
 *                      the way the kernel's reverse walks do (bounded retry),
 *                      on top of the per-component d_seq check.  Same guarantee
 *                      as every txn arm; pairs with txn-global.  No kernel
 *                      operation does a forward walk this way -- it prices the
 *                      snapshot guarantee using the kernel's own tool for it.
 *
 * Writers and readdir resolve their paths with the kernel's fast-path walk in
 * BOTH arms: the snapshot arm is about the lookup, not the writers.  dc_dentry_
 * path() is the reverse walk (dentry_path_raw), the same in both arms.
 *
 * Kernel-faithful write-side locking (see README): the write path mirrors the
 * kernel's granularity so writer THROUGHPUT is a fair comparison too, not only
 * the reader path.  A structural mutator takes the per-directory rwsem of the
 * dir(s) it touches (the i_rwsem analog, guarding each dir's child list and
 * serializing same-dir mutators) and the per-bucket lock of the hash bucket(s)
 * it edits (an hlist_bl bit lock in bit 0 of the bucket head word, guarding each
 * hash chain) -- NOT one global lock, so add/unlink in different dirs and buckets
 * proceed in parallel exactly as they do in the kernel.  Every rename / exchange
 * takes rename_lock's write side around the move itself (d_move); a CROSS-
 * directory one additionally takes a single global s_vfs_rename_mutex, as the
 * kernel's lock_rename does for p1 != p2, making the loop check atomic with the
 * reparent (a same-dir rename takes neither the mutex nor the loop check).  Lock
 * ordering, outermost first: vfs_rename_mutex -> dir rwsems (address-ordered)
 * -> rename_lock -> bucket locks (address-ordered).  The dir rwsems are taken
 * BEFORE rename_lock, as the kernel's lock_rename takes i_rwsem before vfs_rename
 * reaches d_move: rename_lock's write section -- the window in which every new
 * walk waits in path_init -- then covers only the hash/name/parent edit, never a
 * sleeping-lock acquisition behind a readdir or an add.  No mutator takes a dir
 * lock while already holding rename_lock or a bucket lock, so the hierarchy
 * cannot cycle.  d_seq is written under the bucket lock
 * of the dentry's own chain.  Refcounting is omitted: a walk lives entirely in
 * one RCU read-side section and retains nothing (the kernel's LOOKUP_RCU fast
 * path), unlink RCU-defers the free, and every mutator brackets its resolve +
 * edit in rcu_read_lock so a node it observes cannot be reclaimed under it (the
 * benchmark's disjoint-slot ownership already means no two writers target the
 * same leaf, and directory nodes are stable for a run's duration).
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu-qsbr.h>			/* generic rcu_* names => QSBR flavor */
#include <urcu-call-rcu.h>
#ifndef DC_NO_LRU
#include <rseq/rseq.h>			/* phase 3: NUMA node id (list_lru sharding) */
#include "dcache_node.h"		/* phase 3: this CPU's node, rseq or getcpu */
#endif

#include "dcache.h"
#include "dcache_txn_stats.h"
#include "seqcount.h"

/*
 * Per-directory lock TYPE.  DEFAULT: the VENDORED Linux kernel rw_semaphore
 * (krwsem/, GPL-2.0) -- the FAITHFUL fair/writer-non-starving lock the kernel
 * actually uses for inode->i_rwsem -- sized (56 B) to match pthread_rwlock_t so
 * the dentry footprint is unchanged.  -DDC_DIR_LOCK_PTHREAD selects the glibc
 * pthread_rwlock instead (reader-preferring; -DDC_DIR_LOCK_WRITER_PREF, which
 * implies it, makes it writer-preferring -- see dir_lock_init below).
 *
 * The kernel rwsem became the default on 2026-09-29: this lock is part of what
 * the txn engines dissolve (their readdir takes no lock, so no writer need
 * exclude a reader), and that axis has to be measured against the kernel's own
 * lock.  glibc's cost 1.13-1.16x more in-place churn throughput at 16-48
 * writers than the kernel's, which had inflated the axis.
 */
#if !defined(DC_DIR_LOCK_PTHREAD) && !defined(DC_DIR_LOCK_WRITER_PREF) && \
    !defined(DC_DIR_LOCK_KRWSEM)
#define DC_DIR_LOCK_KRWSEM 1
#endif
#ifdef DC_DIR_LOCK_KRWSEM
#include "krwsem/krwsem.h"
typedef struct krwsem dc_dirlock_t;
#else
typedef pthread_rwlock_t dc_dirlock_t;
#endif

/*
 * Fair 1-CL reader layout, DEFAULT-ON.  The reference baseline must carry the
 * SAME cacheline-quality hot line as the txn port, so the footprint A/B is
 * mechanism-vs-mechanism (seqlock vs rcu-txn), not layout-vs-layout.  Opt out
 * with -DDC_NO_HOT1CL_SPLIT to measure the legacy 3-CL struct.
 */
#if !defined(DC_NO_HOT1CL_SPLIT) && !defined(DC_HOT1CL_SPLIT)
#define DC_HOT1CL_SPLIT 1
#endif

/*
 * Total dc_lookup walk restarts: a failed d_seq check on a component the walk
 * used (both arms), or -- DC_SEQ_SNAPSHOT only -- a rename ANYWHERE during the
 * walk (rename_lock moved).  The benchmark reads this via a WEAK reference so it
 * stays engine-agnostic: the txn engine, which never retries a walk, simply does
 * not define the symbol and the harness reports it as N/A.  Only touched on the
 * retry slow path (zero cost at rename-fraction 0).
 */
unsigned long dc_seq_walk_retries;

/*
 * Restarts are counted per thread and folded into dc_seq_walk_retries when the
 * thread unregisters (the harnesses read it after joining).  A shared counter
 * bumped on every restart would be a contended line of the HARNESS's making,
 * charged to exactly the arm and the moment -- a retry storm -- being measured.
 */
static __thread unsigned long walk_retries_tls;

/* ---- structures --------------------------------------------------------- */

/* RCU hlist node with pprev, so removal is O(1) and reader-safe (hlist_bl-like). */
struct dc_hnode {
	struct dc_hnode *next;
	struct dc_hnode **pprev;
};

/*
 * One machine word per bucket -- exactly the kernel's hlist_bl_head.  Bit 0 of
 * `first` is the bucket's spinlock (the hlist_bl bit lock); the chain head is the
 * pointer with that bit masked off.  Embedding the lock in the head word keeps a
 * bucket at 8 bytes (8 per cacheline, as in the kernel) and adds ZERO cache
 * footprint or second-line traffic to a write -- which a side table of locks
 * would, distorting the very write throughput this baseline measures.
 */
#define DC_BL_LOCK 1UL

struct dc_bucket {
	struct dc_hnode *first;		/* chain head | bit0 lock */
};

struct dentry {
#if defined(DC_HOT1CL_SPLIT)
	/*
	 * Fair 1-CL reader hot line -- the fields a lockless walk touches per hop,
	 * clustered like the kernel's RCU-walk-touched dentry head:
	 *   d_name(40) + d_parent(8) + d_seq(8) + d_hash.next(8) = 64 B = CL0.
	 * d_parent's low bits carry the unhashed / negative tags (dparent_of /
	 * d_is_unhashed / d_is_positive), so the per-hop compare reads them off the
	 * already-loaded parent word rather than the cold d_inode / d_unhashed
	 * fields.  d_seq lives ON the hot line (sampled every hop -- the seqlock's
	 * whole mechanism).  d_hash straddles: next@56 stays in CL0 (collision walk
	 * hot), pprev@64 spills cold.  d_id is a benchmark artifact (a real dentry's
	 * identity IS its address) read cold only by the census / readdir /
	 * -DDC_SPLIT_KEEPID; the lookup returns the dentry address.
	 */
	struct qstr d_name;		/* @0:  inline name (match) */
	struct dentry *d_parent;	/* @40: parent addr + unhashed/neg tags */
	seqcount_t d_seq;		/* @48: name/parent coherence -- ON CL0 */
	struct dc_hnode d_hash;		/* @56: next@56 CL0, pprev@64 cold */

	/* --- cold, below the reader hot line --- */
	uint64_t d_id;			/* identity artifact; census/readdir/KEEPID */
	struct dentry *d_children;	/* head of children (verify/-ENOTEMPTY) */
	struct dentry *d_sib;		/* next sibling under d_parent */
	struct dentry **d_sib_pprev;	/* slot naming this node (O(1) del) */
	dc_dirlock_t     d_lock;	/* per-dir readdir/child-list exclusion */
	unsigned char d_isdir;		/* file vs directory (dc_add ENOTDIR).
					 * Kernel-faithful: tracked for -ENOTDIR
					 * but rename_lock bumps regardless of
					 * type, as the kernel does. */

	struct rcu_head d_rcu;		/* deferred free */

#ifndef DC_NO_LRU
	/*
	 * ---- PHASE 3: s_dentry_lru membership, on its OWN cacheline ------------
	 * Off the reader's line because __d_lookup_rcu touches the LRU zero times
	 * (it takes no reference, so it never dputs), and off every OTHER line
	 * because splicing a node out writes its NEIGHBOURS' links -- dirtying a
	 * line belonging to two arbitrary other dentries on every add/del/rotate.
	 */
	struct {
		struct dentry *prev;
		struct dentry *next;
		unsigned int   shard;		/* owning node +1; 0 = off the list */
		unsigned char  referenced;	/* DCACHE_REFERENCED */
	} d_lru __attribute__((aligned(64)));
#endif
#else
	/* legacy fat layout (3 CL): the A/B baseline, -DDC_NO_HOT1CL_SPLIT */
	struct qstr d_name;		/* current name under d_parent */
	struct dentry *d_parent;	/* parent dir (root's parent is itself) */
	uint64_t d_id;			/* stable identity, for verification */
	int d_inode;			/* nonzero => positive (phase 1: always) */
	int d_unhashed;			/* removed from the hash (skip in lookup) */

	seqcount_t d_seq;		/* name/parent coherence for RCU walk */
	struct dc_hnode d_hash;		/* linkage in dentry_hashtable bucket */

	struct dentry *d_children;	/* head of children (verify/-ENOTEMPTY) */
	struct dentry *d_sib;		/* next sibling under d_parent */
	struct dentry **d_sib_pprev;	/* slot naming this node (O(1) del) */
	dc_dirlock_t     d_lock;	/* per-dir readdir/child-list exclusion */
	unsigned char d_isdir;		/* file vs directory (dc_add ENOTDIR).
					 * Kernel-faithful: tracked for -ENOTDIR
					 * but rename_lock bumps regardless of
					 * type, as the kernel does. */

	struct rcu_head d_rcu;		/* deferred free */

#ifndef DC_NO_LRU
	/* s_dentry_lru membership; own cacheline (see the split branch above) */
	struct {
		struct dentry *prev;
		struct dentry *next;
		unsigned int   shard;
		unsigned char  referenced;
	} d_lru __attribute__((aligned(64)));
#endif
#endif
};

/*
 * PHASE 3: `struct list_lru` -- the BASELINE's LRU, kept as close to
 * fs/dcache.c + mm/list_lru.c as a userspace port can be, because the whole
 * point of this engine is to be the thing the txn port must beat.
 *
 * WHAT THE KERNEL ACTUALLY HAS (Linux 7.0, design/dcache-lru-txn.md section 1):
 *   sb->s_dentry_lru is a `struct list_lru` -- an array indexed by NUMA NODE id
 *   (`&lru->node[nid]`), each node holding a `struct list_lru_one { list,
 *   nr_items, spinlock_t lock; }`.  So the sharding is per node, and EVERY list
 *   mutation takes that shard's spinlock.  Add is at the TAIL; the shrinker
 *   walks from the HEAD.
 *
 * The `s_dentry_lru_lock` named in fs/dcache.c's comment is stale pre-list_lru
 * documentation -- the real lock is list_lru_one.lock, which is why this is
 * per-node and not global.
 *
 * DELIBERATELY NOT per-CPU here.  The txn engine offers per-CPU and mm_cid
 * arms; giving them to the baseline too would let it borrow an improvement the
 * kernel does not have, and the comparison it exists for would measure nothing.
 */
#define DC_LRU_NODES	64		/* >= nr_node_ids anywhere we run */

struct dc_lru_one {			/* struct list_lru_one */
	unsigned long lock;		/* spinlock_t: "protects all fields above" */
	struct dentry *head;		/* oldest -- the shrinker's end */
	struct dentry *tail;		/* newest -- the add end */
	unsigned long nr_items;
	char pad[64 - (2 * sizeof(unsigned long) + 2 * sizeof(void *)) % 64];
};

/*
 * Field placement follows the kernel's, because it decides what a rename costs
 * every concurrent walk.  The hash-table geometry and the root are read on every
 * hop and never written after dc_create -- the kernel keeps dentry_hashtable /
 * d_hash_shift __ro_after_init -- so they get a line no writer touches.
 * rename_lock is written by every d_move, and the kernel makes it
 * __cacheline_aligned_in_smp; s_vfs_rename_mutex lives in the superblock, near
 * neither.  Packed together (as this struct once was), every rename invalidates
 * the line every walk reads on every hop: a false-sharing tax the kernel does
 * not pay.  dc_create allocates the struct 64-byte aligned so these hold.
 */
struct dcache {
	struct dc_bucket *buckets;	/* each head word carries its own bit lock */
	unsigned long mask;		/* nbuckets - 1 (power of two) */
	struct dentry *root;
	seqlock_t rename_lock		/* GLOBAL: bumped by every d_move */
		__attribute__((aligned(64)));
	pthread_mutex_t vfs_rename_mutex /* s_vfs_rename_mutex: cross-dir moves */
		__attribute__((aligned(64)));
	struct dc_lru_one s_dentry_lru[DC_LRU_NODES]
		__attribute__((aligned(64)));
};

#define hnode_dentry(n) caa_container_of((n), struct dentry, d_hash)

/*
 * 1-CL tag encoding in d_parent's low bits.  A dentry is 64-byte aligned
 * (posix_memalign, for the 1-CL reader line), so bits 0-5 are free; unhashed and
 * negative ride bits 0 and 1.  Both
 * are read off the already-loaded parent word, keeping the removed-from-hash and
 * positive/negative tests on CL0 instead of the cold d_unhashed / d_inode fields.
 * The legacy (non-split) build falls back to those plain fields.
 */
#if defined(DC_HOT1CL_SPLIT)
#define DC_TAG_UNHASHED	((uintptr_t) 0x1)	/* bit 0: removed from the hash */
#define DC_TAG_NEG	((uintptr_t) 0x2)	/* bit 1: negative dentry */
#define DC_TAG_MASK	((uintptr_t) 0x3)

static inline struct dentry *dparent_of(const struct dentry *d)
{
	return (struct dentry *) ((uintptr_t) d->d_parent & ~DC_TAG_MASK);
}
static inline int d_is_unhashed(const struct dentry *d)
{
	return ((uintptr_t) d->d_parent & DC_TAG_UNHASHED) != 0;
}
static inline int d_is_positive(const struct dentry *d)
{
	return ((uintptr_t) d->d_parent & DC_TAG_NEG) == 0;
}
#define DC_DPARENT(d)      dparent_of(d)
#define DC_IS_UNHASHED(d)  d_is_unhashed(d)
#define DC_IS_POSITIVE(d)  d_is_positive(d)
/* Mark a live node as removed from the hash: OR the tag into its parent word,
 * under the node's d_seq bracket (the fat build stores the d_unhashed field). */
#define DC_SET_UNHASHED(d) \
	CMM_STORE_SHARED((d)->d_parent, \
		(struct dentry *) ((uintptr_t) (d)->d_parent | DC_TAG_UNHASHED))
#else
#define DC_DPARENT(d)      ((d)->d_parent)
#define DC_IS_UNHASHED(d)  ((d)->d_unhashed)
#define DC_IS_POSITIVE(d)  ((d)->d_inode)
#define DC_SET_UNHASHED(d) CMM_STORE_SHARED((d)->d_unhashed, 1)
/* No tags in this layout (the state lives in its own fields), so a rename's
 * tag-preserving mask is a no-op rather than a special case. */
#define DC_TAG_MASK        ((uintptr_t) 0)
#endif

/*
 * 1-CL identity = the dentry ADDRESS (a real kernel dentry has no logical id;
 * d_id is a benchmark artifact read cold by the census / readdir).  The
 * -DDC_SPLIT_KEEPID validation build returns the cold d_id instead so a harness's
 * id==gid torn-read checks keep working -- same hot LAYOUT, one extra cold read.
 */
#if defined(DC_HOT1CL_SPLIT) && !defined(DC_SPLIT_KEEPID)
#define DC_FAST_ID(d)      ((uint64_t) (uintptr_t) (d))
#else
#define DC_FAST_ID(d)      ((d)->d_id)
#endif

/*
 * Capability flag for harnesses: 1 when dc_lookup returns the dentry ADDRESS as
 * the id (the DEFAULT 1-CL build), 0 when it returns the logical d_id.  Read via
 * a weak reference (absent => 0 => logical id); mirrors the txn engine so the
 * bench treats both identically.  See bench_dcache.c.
 */
/* phase 3: struct list_lru, per NUMA node -- the kernel's own shape */
#ifdef DC_TXN_STATS
const int dc_txn_stats_supported = 1;
#else
const int dc_txn_stats_supported = 0;
void dc_txn_stats_dump(void *stream) { (void) stream; }
void dc_txn_stats_last(void *stream) { (void) stream; }
void dc_lru_validate(void *stream) { (void) stream; }
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

/* ---- hashing ------------------------------------------------------------ */

static inline struct dc_bucket *bucket_of(struct dcache *dc,
					  const struct dentry *parent,
					  uint32_t name_hash)
{
	unsigned long h = (unsigned long) name_hash * 0x9e3779b97f4a7c15UL;

	h ^= (unsigned long) (uintptr_t) parent >> 6;
	h *= 0xff51afd7ed558ccdUL;
	return &dc->buckets[(h >> 32) & dc->mask];
}

/* ---- bit-locked bucket head (hlist_bl) --------------------------------- */

/*
 * Chain head with the lock bit masked off -- what a traversal actually walks.
 * Every access to the head word is __atomic (matching bl_lock's fetch_or), so
 * the lock bit and the chain pointer share a word without a data race on it.
 */
static inline struct dc_hnode *bl_first(struct dc_bucket *b)
{
	uintptr_t v = __atomic_load_n((uintptr_t *) &b->first, __ATOMIC_RELAXED);

	return (struct dc_hnode *) (v & ~DC_BL_LOCK);
}

static inline struct dc_hnode *bl_first_rcu(struct dc_bucket *b)
{
	return (struct dc_hnode *)
		((uintptr_t) rcu_dereference(b->first) & ~DC_BL_LOCK);
}

/*
 * Publish a new head, keeping the lock bit set.  Only the lock holder calls this
 * (from hlist_add_head_rcu, under bl_lock), so the bit is definitionally 1 -- OR
 * it in without reading the word, avoiding a race with concurrent lock spinners.
 */
static inline void bl_set_first_rcu(struct dc_bucket *b, struct dc_hnode *n)
{
	rcu_assign_pointer(b->first,
			   (struct dc_hnode *) ((uintptr_t) n | DC_BL_LOCK));
}

/*
 * Bit spinlock on bit 0 of the head word (the kernel's bit_spin_lock(0, &first)).
 * The atomic fetch_or/fetch_and touch the whole word but only ever flip bit 0;
 * the holder's chain stores (bl_set_first_rcu, hlist_del_rcu's *pprev) preserve
 * that bit by value, so lock and data never clobber each other.
 *
 * Test-and-TEST-and-set, as bit_spin_lock is: one test_and_set_bit, then wait
 * with plain loads until the bit clears.  A waiter that RMWs the word on every
 * spin (plain test-and-set, what this was until 2026-09-30) steals the line
 * from the holder and from every reader walking the chain; a herd of readers
 * caching the same negative made that measurable (bench_dcache --hit-current,
 * 128-184 readers).
 */
static inline void bl_lock(struct dc_bucket *b)
{
	uintptr_t *p = (uintptr_t *) &b->first;

	while (__atomic_fetch_or(p, DC_BL_LOCK, __ATOMIC_ACQUIRE) & DC_BL_LOCK)
		do {
			caa_cpu_relax();
		} while (__atomic_load_n(p, __ATOMIC_RELAXED) & DC_BL_LOCK);
}

static inline void bl_unlock(struct dc_bucket *b)
{
	uintptr_t *p = (uintptr_t *) &b->first;

	__atomic_fetch_and(p, ~DC_BL_LOCK, __ATOMIC_RELEASE);
}

/* ---- RCU hlist (writer side runs under the bucket's bit lock) ----------- */

static inline void hlist_add_head_rcu(struct dc_bucket *b, struct dc_hnode *n)
{
	struct dc_hnode *first = bl_first(b);	/* masked: the real head */

	n->next = first;
	n->pprev = &b->first;
	if (first)
		first->pprev = &n->next;
	bl_set_first_rcu(b, n);			/* release: publish n, keep bit */
}

static inline void hlist_del_rcu(struct dc_hnode *n)
{
	struct dc_hnode *next = n->next;
	uintptr_t *pprev = (uintptr_t *) n->pprev;
	/* Set iff pprev is a (locked) bucket head; a node's ->next never carries it.
	 * Read atomically: if pprev is the head, spinners fetch_or the same word. */
	uintptr_t bit = __atomic_load_n(pprev, __ATOMIC_RELAXED) & DC_BL_LOCK;

	/* Splice n out (release-publish the forward link readers dereference); n->next
	 * stays valid for readers already past it.  Preserve whatever lock bit lives
	 * in the slot -- the head word carries one, a node's ->next does not. */
	__atomic_store_n(pprev, (uintptr_t) next | bit, __ATOMIC_RELEASE);
	if (next)
		next->pprev = n->pprev;
}

/* ---- children list (verify + -ENOTEMPTY; writer/quiescent only) --------- */

/* ---- PHASE 3: struct list_lru (s_dentry_lru) --------------------------- */
/*
 * TEST-ONLY: -DDC_TEST_RETAIN_DELAY_US=N sleeps N us in a writer's walk between
 * a component's d_seq check and its LRU re-arm -- the window a concurrent kill
 * must land in to race the re-arm -- and, under -DDC_TEST_SEQ_LRU_ADD_LATE,
 * between dc_add's publish and its enqueue.  check-rmdir widens with it.
 */
#ifdef DC_TEST_RETAIN_DELAY_US
#include <time.h>
static void dc_test_retain_delay(void)
{
	struct timespec ts = { DC_TEST_RETAIN_DELAY_US / 1000000,
			       (DC_TEST_RETAIN_DELAY_US % 1000000) * 1000L };

	nanosleep(&ts, NULL);
}
#define DC_TEST_RETAIN_DELAY()	dc_test_retain_delay()
#else
#define DC_TEST_RETAIN_DELAY()	do { } while (0)
#endif

#ifndef DC_NO_LRU

/*
 * list_lru_one.lock -- a plain spinlock, as in the kernel.
 *
 * Test-and-TEST-and-set: one cmpxchg, then wait with plain loads until the word
 * reads free.  The kernel's spinlock_t (a qspinlock) never RMWs the lock word
 * while it waits either; a waiter that cmpxchg'd on every spin (what this was
 * until 2026-09-30) stole the line from the holder on every iteration.  Not a
 * FIFO lock like the qspinlock: a queued lock hands the lock to a waiter that
 * may be descheduled, and the kernel prevents that by disabling preemption
 * while spinning -- this harness cannot, and co-pins each writer's call_rcu
 * worker (which takes this lock on the fold and free paths) on its writer's
 * CPU, so a FIFO handoff would convoy behind it for a timeslice.
 */
static inline void lru_lock(struct dc_lru_one *l)
{
	while (uatomic_cmpxchg(&l->lock, 0UL, 1UL) != 0UL)
		do {
			caa_cpu_relax();
		} while (uatomic_load(&l->lock, CMM_RELAXED) != 0UL);
	cmm_smp_mb();
}

static inline void lru_unlock(struct dc_lru_one *l)
{
	uatomic_store(&l->lock, 0UL, CMM_RELEASE);
}

/*
 * The shard: the NUMA node, exactly as the kernel indexes `lru->node[nid]`.
 * Read by dc_current_node() -- the rseq ABI page when it exposes node ids (a
 * plain load, no syscall), else getcpu() from the vDSO; never a default of
 * node 0, which on this 24-node machine is one lock across every CCD (see
 * dcache_node.h).  The kernel derives nid from the OBJECT's memory
 * (page_to_nid); under first-touch the enqueueing thread's node is that same
 * node, and it is captured once and remembered so a later del from another
 * node still finds the list the dentry is actually on.
 */
static inline unsigned int lru_nid(void)
{
	unsigned int nid = dc_current_node();

	return nid < DC_LRU_NODES ? nid : nid % DC_LRU_NODES;
}

/*
 * The membership word, d_lru.shard.  The kernel makes LRU membership atomic
 * with a dentry's death through d_lock, which retain_dentry's d_lru_add and
 * __dentry_kill's d_lru_del both hold, plus the reference a ref-walk holds
 * across its dput.  This port has neither on the paths that race -- a writer's
 * walk re-arms a dentry (lru_retain) holding no lock on it -- so the word
 * carries the exclusion instead:
 *
 *   OFF      not on a list, alive, re-armable (never added, or LRU_REMOVED)
 *   ON(i)    linked on node shard i; changed only under that shard's lock
 *   SHRINK   isolated by the shrinker, about to be killed or put back: a
 *            re-arm must not touch it (mainline's DCACHE_SHRINK_LIST)
 *   DEAD     killed: never re-armed, never linked again
 *
 * Without it, two races were open.  A walk that passed its d_seq check before
 * a concurrent unlink re-armed the dentry after the unlink's lru_del, so the
 * LRU kept a pointer past the call_rcu free; and dc_add published the dentry
 * before enqueueing it, so a walk finding it in between linked it onto its own
 * shard and the add linked it again -- one node on two lists.
 *
 * The hot paths stay free of atomic RMWs.  dc_add enqueues BEFORE publishing
 * (lru_add_new: nobody can see the dentry, so plain stores suffice), and a kill
 * of a listed dentry is a plain store under the shard lock it already takes.
 * Only the rare transitions out of OFF and SHRINK -- a re-arm's claim, a kill
 * of an unlisted dentry, the shrinker's put-back -- use cmpxchg, and they race
 * exactly each other.
 */
#define DC_SEQ_LRU_OFF		0u
#define DC_SEQ_LRU_ON(nid)	((nid) + 1u)	/* 1 .. DC_LRU_NODES */
#define DC_SEQ_LRU_SHRINK	0xfffffffeu
#define DC_SEQ_LRU_DEAD		0xffffffffu
#define DC_SEQ_LRU_IS_ON(st)	((st) != DC_SEQ_LRU_OFF && (st) <= DC_LRU_NODES)

/* Link @d at @l's tail.  Caller holds @l and owns @d's membership word. */
static void lru_link_tail_locked(struct dc_lru_one *l, struct dentry *d)
{
	d->d_lru.prev = l->tail;
	d->d_lru.next = NULL;
	if (l->tail)
		l->tail->d_lru.next = d;
	else
		l->head = d;
	l->tail = d;
	l->nr_items++;
}

/*
 * list_lru_add for a dentry nobody else can see yet: dc_add calls it BEFORE
 * hlist_add_head_rcu publishes @d, so no walk, unlink or shrinker can race the
 * claim and a plain store does it.
 */
static void lru_add_new(struct dcache *dc, struct dentry *d)
{
	unsigned int nid = lru_nid();
	struct dc_lru_one *l = &dc->s_dentry_lru[nid];

	lru_lock(l);
	lru_link_tail_locked(l, d);
	uatomic_store(&d->d_lru.shard, DC_SEQ_LRU_ON(nid), CMM_RELAXED);
	lru_unlock(l);
}

/*
 * Re-arm a published dentry (lru_retain).  CLAIM the word OFF -> ON under the
 * shard lock: the cmpxchg is what a concurrent kill's OFF -> DEAD seal races,
 * so a dying dentry is never linked, and holding the lock across the claim and
 * the link means a killer that reads ON(i) finds it linked once it gets the
 * lock.  Losing the claim (ON elsewhere, SHRINK, DEAD) leaves nothing to do.
 */
static void lru_add_claim(struct dcache *dc, struct dentry *d)
{
	unsigned int nid = lru_nid();
	struct dc_lru_one *l = &dc->s_dentry_lru[nid];

	lru_lock(l);
	if (uatomic_cmpxchg(&d->d_lru.shard, DC_SEQ_LRU_OFF,
			    DC_SEQ_LRU_ON(nid)) == DC_SEQ_LRU_OFF)
		lru_link_tail_locked(l, d);
	lru_unlock(l);
}

/* Unlink @d from @l, leaving its word at @newst.  Caller holds @l. */
static void lru_del_locked(struct dc_lru_one *l, struct dentry *d,
			   unsigned int newst)
{
	if (d->d_lru.prev)
		d->d_lru.prev->d_lru.next = d->d_lru.next;
	else
		l->head = d->d_lru.next;
	if (d->d_lru.next)
		d->d_lru.next->d_lru.prev = d->d_lru.prev;
	else
		l->tail = d->d_lru.prev;
	d->d_lru.prev = d->d_lru.next = NULL;
	uatomic_store(&d->d_lru.shard, newst, CMM_RELAXED);
	l->nr_items--;
}

/*
 * list_lru_del for a dentry being KILLED: IMMEDIATE physical removal, and seal
 * the word DEAD so no re-arm can link it after the caller's call_rcu.
 *
 *   ON(i)   unlink under shard i's lock, store DEAD (re-derived under it: a
 *           shrinker may have isolated it, or a re-arm moved it, meanwhile)
 *   OFF     cmpxchg to DEAD; a lost race means a re-arm claimed it -- retry,
 *           and take it off the shard it named
 *   SHRINK  cmpxchg to DEAD: the shrinker's lru_kill() re-verifies the
 *           dentry under the dir and bucket locks the caller holds or held,
 *           so it skips a dentry this kill already unhashed, and its put-back
 *           (SHRINK -> OFF) loses to this seal
 *   DEAD    nothing to do
 */
static void lru_del(struct dcache *dc, struct dentry *d)
{
#ifdef DC_TEST_SEQ_LRU_NO_SEAL
	/* MUTATION (check-rmdir must fail): the pre-2026-10-02 kill -- unlink
	 * and leave the word OFF, so a racing walk re-arms a dentry being
	 * freed. */
	unsigned int st0 = uatomic_load(&d->d_lru.shard, CMM_RELAXED);

	if (DC_SEQ_LRU_IS_ON(st0)) {
		struct dc_lru_one *l0 = &dc->s_dentry_lru[st0 - 1];

		lru_lock(l0);
		if (uatomic_load(&d->d_lru.shard, CMM_RELAXED) == st0)
			lru_del_locked(l0, d, DC_SEQ_LRU_OFF);
		lru_unlock(l0);
	}
	return;
#endif
	for (;;) {
		unsigned int st = uatomic_load(&d->d_lru.shard, CMM_RELAXED);
		struct dc_lru_one *l;

		if (st == DC_SEQ_LRU_DEAD)
			return;
		if (!DC_SEQ_LRU_IS_ON(st)) {	/* OFF or SHRINK */
			if (uatomic_cmpxchg(&d->d_lru.shard, st,
					    DC_SEQ_LRU_DEAD) == st)
				return;
			continue;
		}
		l = &dc->s_dentry_lru[st - 1];
		lru_lock(l);
		if (uatomic_load(&d->d_lru.shard, CMM_RELAXED) != st) {
			lru_unlock(l);
			continue;
		}
		lru_del_locked(l, d, DC_SEQ_LRU_DEAD);
		lru_unlock(l);
		return;
	}
}

/*
 * retain_dentry (fs/dcache.c) -- what the kernel does on the LAST dput:
 *
 *	if the dentry is not on the LRU:  d_lru_add()      (tail)
 *	else:                             d_flags |= DCACHE_REFERENCED
 *
 * It does NOT move an already-listed dentry, and that is the load-bearing
 * decision: recency becomes a per-object bit instead of a shared list-head
 * write, which is the only reason a single per-node list survives a busy cache.
 *
 * Called from the WRITER-side resolve, never from a lookup -- __d_lookup_rcu
 * takes no reference, so it never dputs and never reaches here.  A ref-walk
 * dgets/dputs each component, so the prefix it passed through is what gets
 * marked; that is what this reproduces.
 */
static void lru_retain(struct dcache *dc, struct dentry *d)
{
	unsigned int st = uatomic_load(&d->d_lru.shard, CMM_RELAXED);

	if (caa_likely(DC_SEQ_LRU_IS_ON(st))) {
		if (!uatomic_load(&d->d_lru.referenced, CMM_RELAXED))
			uatomic_store(&d->d_lru.referenced, 1, CMM_RELAXED);
		return;
	}
	if (st == DC_SEQ_LRU_OFF)
		lru_add_claim(dc, d);		/* re-arm after an LRU_REMOVED */
	/* SHRINK: the shrinker owns it; DEAD: it is being freed */
}

unsigned long dc_lru_count(struct dcache *dc)
{
	unsigned long n = 0;
	unsigned int i;

	for (i = 0; i < DC_LRU_NODES; i++)
		n += uatomic_load(&dc->s_dentry_lru[i].nr_items, CMM_RELAXED);
	return n;
}

long dc_lru_check(struct dcache *dc)
{
	long bad = 0;
	unsigned int i;

	for (i = 0; i < DC_LRU_NODES; i++) {
		struct dc_lru_one *l = &dc->s_dentry_lru[i];
		struct dentry *d, *prev = NULL;
		unsigned long n = 0, want = l->nr_items;

		for (d = l->head; d && n <= want; prev = d, d = d->d_lru.next) {
			unsigned int st = uatomic_load(&d->d_lru.shard,
						       CMM_RELAXED);
			const char *why = NULL;

			n++;
			if (st != DC_SEQ_LRU_ON(i))
				why = "word does not name this list";
			else if (DC_IS_UNHASHED(d))
				why = "listed but unhashed (killed)";
			else if (d->d_lru.prev != prev)
				why = "prev link disagrees";
			if (why) {
				if (bad < 8)
					fprintf(stderr, "LRUCHK node %u pos %lu "
						"%p word=%#x: %s\n", i, n - 1,
						(void *) d, st, why);
				bad++;
			}
		}
		if (n != want || l->tail != prev) {
			if (bad < 8)
				fprintf(stderr, "LRUCHK node %u: walked %lu of "
					"nr_items %lu, tail %s\n", i, n, want,
					l->tail == prev ? "ok" : "WRONG");
			bad++;
		}
	}
	return bad;
}

const char *dc_lru_arm(void) { return "pernode"; }
const int dc_lru_inuse_is_removed = 1;	/* kernel-faithful: LRU_REMOVED */

#else	/* DC_NO_LRU */
static inline void lru_add_new(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline void lru_del(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline void lru_retain(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
unsigned long dc_lru_count(struct dcache *dc) { (void) dc; return 0; }
long dc_lru_check(struct dcache *dc) { (void) dc; return 0; }
long dc_shrink(struct dcache *dc, long nr) { (void) dc; (void) nr; return 0; }
long dc_shrink_local(struct dcache *dc, long nr) { (void) dc; (void) nr; return 0; }
const char *dc_lru_arm(void) { return "none"; }
const int dc_lru_inuse_is_removed = 1;
#endif	/* DC_NO_LRU */

/*
 * The child list is the kernel's d_children / d_sib hlist: a pprev back-link
 * makes removal O(1).  A singly linked list here made every cross-directory
 * move and exchange walk the old parent's children INSIDE rename_lock's write
 * section -- holding it odd, and so every new walk waiting in path_init, for
 * O(fanout) -- which the kernel's __hlist_del(&dentry->d_sib) never does.
 * Plain stores: the list is only read under the parent's dir lock (readdir,
 * the -ENOTEMPTY checks) or quiescent (dc_walk, dc_destroy).
 */
static void children_add(struct dentry *parent, struct dentry *child)
{
	child->d_sib = parent->d_children;
	child->d_sib_pprev = &parent->d_children;
	if (parent->d_children)
		parent->d_children->d_sib_pprev = &child->d_sib;
	parent->d_children = child;
}

static void children_remove(struct dentry *parent, struct dentry *child)
{
	(void) parent;
	*child->d_sib_pprev = child->d_sib;
	if (child->d_sib)
		child->d_sib->d_sib_pprev = child->d_sib_pprev;
	child->d_sib = NULL;
	child->d_sib_pprev = NULL;
}

/*
 * Per-directory rwsem.  readdir read-locks the dir it lists; every mutator that
 * changes a dir's child listing -- add/remove a child, or rename a child in
 * place (name change under the same parent) -- write-locks that dir.  Concurrent
 * readdirs of a dir thus share, and a readdir of one dir never serializes against
 * a rename of another: the honest per-directory-inode-rwsem analogue, not one
 * global lock.  Writer-vs-writer cannot deadlock because EVERY two-dir
 * acquisition (rename, exchange, rmdir-to-negative) goes through dirs_wlock2's
 * address order, a total order over all dir locks; several renamers may each
 * hold two dir locks while they queue on rename_lock, which is fine for the same
 * reason.  readdir takes only a read lock, so there is no cycle either.
 * Ordering, outermost first: vfs_rename_mutex -> these dir rwsems -> rename_lock
 * -> bucket bit locks.
 */
/*
 * Per-directory lock BIAS -- a fidelity knob, because it decides who wins the
 * readdir-vs-churn contention on a directory's child list (readdir takes it
 * shared, dc_add/dc_unlink take it exclusive).  The glibc DEFAULT (NULL attr,
 * the -DDC_DIR_LOCK_READER_PREF / unset case) is PTHREAD_RWLOCK_PREFER_READER_NP:
 * readers barge past a waiting writer, so a stream of readdir readers can STARVE
 * add/unlink indefinitely.  That is NOT the kernel: a directory op takes
 * inode->i_rwsem, a FAIR FIFO rw_semaphore where a queued writer blocks later
 * readers, so writers are not starved.  Two closer analogues:
 *   -DDC_DIR_LOCK_WRITER_PREF  glibc PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
 *                              (writer-non-starving; a waiting writer holds off
 *                              new readers -- brackets the fair case from the
 *                              writer side)
 * The truly faithful FAIR arm uses the ISC phase-fair rwlock (see the
 * DC_DIR_LOCK_ISC build); this pthread path covers the two glibc biases.
 */
#ifdef DC_DIR_LOCK_KRWSEM
/* The faithful arm: the vendored Linux kernel rw_semaphore (fair, writer-non-
 * starving).  read and write UNLOCK differ (up_read vs up_write), so the readdir
 * path must use dir_runlock, not a generic unlock. */
static void dir_lock_init(dc_dirlock_t *l)    { krwsem_init(l); }
static void dir_lock_destroy(dc_dirlock_t *l) { (void) l; }
static void dir_wlock(struct dentry *d)   { krwsem_wrlock(&d->d_lock); }
static void dir_wunlock(struct dentry *d) { krwsem_wrunlock(&d->d_lock); }
static void dir_rlock(struct dentry *d)   { krwsem_rdlock(&d->d_lock); }
static void dir_runlock(struct dentry *d) { krwsem_rdunlock(&d->d_lock); }
#else
static void dir_lock_init(dc_dirlock_t *l)
{
#ifdef DC_DIR_LOCK_WRITER_PREF
	pthread_rwlockattr_t a;

	pthread_rwlockattr_init(&a);
	pthread_rwlockattr_setkind_np(
		&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	pthread_rwlock_init(l, &a);
	pthread_rwlockattr_destroy(&a);
#else
	pthread_rwlock_init(l, NULL);		/* glibc default: reader-preferring */
#endif
}
static void dir_lock_destroy(dc_dirlock_t *l) { pthread_rwlock_destroy(l); }
static void dir_wlock(struct dentry *d)   { pthread_rwlock_wrlock(&d->d_lock); }
static void dir_wunlock(struct dentry *d) { pthread_rwlock_unlock(&d->d_lock); }
static void dir_rlock(struct dentry *d)   { pthread_rwlock_rdlock(&d->d_lock); }
static void dir_runlock(struct dentry *d) { pthread_rwlock_unlock(&d->d_lock); }
#endif

/* Lock two (possibly equal) dirs in address order to keep the discipline tidy. */
static void dirs_wlock2(struct dentry *a, struct dentry *b)
{
	if (a == b) {
		dir_wlock(a);
	} else if ((uintptr_t) a < (uintptr_t) b) {
		dir_wlock(a);
		dir_wlock(b);
	} else {
		dir_wlock(b);
		dir_wlock(a);
	}
}

static void dirs_wunlock2(struct dentry *a, struct dentry *b)
{
	dir_wunlock(a);
	if (a != b)
		dir_wunlock(b);
}

/*
 * Lock two buckets' bit locks in address order (once if they coincide).  The
 * outer rename_lock already serializes multi-bucket mutators, but the address
 * order keeps the discipline uniform with dirs_wlock2 and safe against a
 * single-bucket add/unlink contending for one of the two.
 */
static void bl_lock2(struct dc_bucket *x, struct dc_bucket *y)
{
	if (x == y) {
		bl_lock(x);
	} else if ((uintptr_t) x < (uintptr_t) y) {
		bl_lock(x);
		bl_lock(y);
	} else {
		bl_lock(y);
		bl_lock(x);
	}
}

static void bl_unlock2(struct dc_bucket *x, struct dc_bucket *y)
{
	bl_unlock(x);
	if (x != y)
		bl_unlock(y);
}

/* ---- lifecycle ---------------------------------------------------------- */

const char *dc_engine_name(void)
{
#ifdef DC_SEQ_SNAPSHOT
	return "seqlock-snapshot";
#else
	return "seqlock";
#endif
}

static struct dentry *dentry_alloc(const struct qstr *name,
				   struct dentry *parent, uint64_t id,
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
	d->d_name = *name;
	d->d_parent = parent;		/* clean low bits => hashed + positive */
	d->d_id = id;
#if defined(DC_HOT1CL_SPLIT)
	if (!positive)			/* phase 2: cache the ABSENCE of this name */
		d->d_parent = (struct dentry *)
			((uintptr_t) d->d_parent | DC_TAG_NEG);
#else
	d->d_inode = positive ? 1 : 0;
	d->d_unhashed = 0;
#endif
	seqcount_init(&d->d_seq);
	d->d_children = NULL;
	d->d_sib = NULL;
	d->d_sib_pprev = NULL;
	d->d_isdir = (unsigned char) (isdir != 0);
	dir_lock_init(&d->d_lock);
	return d;
}

struct dcache *dc_create(unsigned int nbuckets)
{
	struct dcache *dc;
	unsigned int n = 1;
	struct qstr rootname;

	/* 64-byte aligned, or struct dcache's per-line placement is fiction. */
	if (posix_memalign((void **) &dc, 64, sizeof(*dc)) != 0)
		return NULL;
	memset(dc, 0, sizeof(*dc));
	while (n < nbuckets)		/* round up to a power of two */
		n <<= 1;
	dc->buckets = calloc(n, sizeof(*dc->buckets));
	if (!dc->buckets) {
		free(dc);
		return NULL;
	}
	dc->mask = n - 1;
	seqlock_init(&dc->rename_lock);
	pthread_mutex_init(&dc->vfs_rename_mutex, NULL);
	/* calloc left every bucket head NULL with bit 0 clear -- all unlocked. */

	dc_qstr_init(&rootname, "");
	dc->root = dentry_alloc(&rootname, NULL, 0, 1, 1);	/* root: directory, positive */
	dc->root->d_parent = dc->root;	/* root is its own parent */
	return dc;
}

static void free_subtree(struct dentry *d)
{
	struct dentry *c = d->d_children, *next;

	while (c) {
		next = c->d_sib;
		free_subtree(c);
		c = next;
	}
	free(d);
}

void dc_destroy(struct dcache *dc)
{
	if (!dc)
		return;
	rcu_barrier();			/* drain outstanding call_rcu frees */
	free_subtree(dc->root);
	free(dc->buckets);
	seqlock_destroy(&dc->rename_lock);
	pthread_mutex_destroy(&dc->vfs_rename_mutex);
	free(dc);
}

/* ---- RCU thread registration ------------------------------------------- */

void dc_register_thread(void)
{
	rcu_register_thread();
}

void dc_unregister_thread(void)
{
	if (walk_retries_tls) {
		__atomic_fetch_add(&dc_seq_walk_retries, walk_retries_tls,
				   __ATOMIC_RELAXED);
		walk_retries_tls = 0;
	}
	rcu_unregister_thread();
}

void dc_quiescent(void)
{
	rcu_quiescent_state();
}

/* ---- lockless lookup (the RCU-walk fast path) --------------------------- */

static inline void walk_retry_count(void)
{
	walk_retries_tls++;
}

/*
 * __d_lookup_rcu: find parent's child named `name` in the hash, lockless.  Fills
 * *seqp with the dentry's d_seq sampled BEFORE the parent/name compare, odd bit
 * masked (raw_seqcount_begin), so a compare that overlapped an in-flight
 * __d_move can never validate; the caller re-validates it (read_seqcount_retry)
 * before trusting the dentry.  A rename can move a chain neighbour to another
 * bucket mid-scan and make this MISS a dentry that is there -- a false negative
 * the caller must not trust (kernel: lookup_fast falls back to d_lookup).
 */
static struct dentry *__d_lookup_rcu(struct dcache *dc, struct dentry *parent,
				     const struct qstr *name,
				     unsigned long *seqp)
{
	struct dc_bucket *b = bucket_of(dc, parent, name->hash);
	struct dc_hnode *n;

	for (n = bl_first_rcu(b); n; n = rcu_dereference(n->next)) {
		struct dentry *d = hnode_dentry(n);
		unsigned long seq;

		if (CMM_LOAD_SHARED(d->d_name.hash) != name->hash)
			continue;
		seq = raw_seqcount_begin(&d->d_seq);
#if defined(DC_HOT1CL_SPLIT)
		{
			/* one load of the CL0 parent word: mask for the parent
			 * edge, test the unhashed tag off the same bits. */
			uintptr_t pw = (uintptr_t) CMM_LOAD_SHARED(d->d_parent);

			if ((struct dentry *) (pw & ~DC_TAG_MASK) != parent)
				continue;
			if (pw & DC_TAG_UNHASHED)
				continue;
		}
#else
		if (CMM_LOAD_SHARED(d->d_parent) != parent)
			continue;
		if (CMM_LOAD_SHARED(d->d_unhashed))
			continue;
#endif
		/*
		 * Optimistic name compare: the bytes may be torn by a concurrent
		 * rename, but the caller's read_seqcount_retry(d_seq, *seqp)
		 * rejects any dentry whose identity moved under us -- exactly the
		 * kernel's dentry_cmp-under-d_seq contract.
		 */
		if (d->d_name.len != name->len ||
		    memcmp(d->d_name.name, name->name, name->len) != 0)
			continue;
		*seqp = seq;
		return d;
	}
	return NULL;
}

/*
 * d_lookup (fs/dcache.c): the non-racy single-component lookup a fast-path MISS
 * falls back to.  It retries the lockless scan only while rename_lock moved
 * underneath a MISS; a hit is returned as is, for the caller to validate.  The
 * kernel gets here through try_to_unlazy + lookup_slow (d_alloc_parallel has
 * the same rename_lock-on-miss bracket) and carries on in ref-walk; this port
 * stays lockless, which can only flatter the baseline.
 */
static struct dentry *d_lookup_rcu(struct dcache *dc, struct dentry *parent,
				   const struct qstr *name, unsigned long *seqp)
{
	struct dentry *d;
	unsigned long r_seq;

	do {
		r_seq = read_seqbegin(&dc->rename_lock);
		d = __d_lookup_rcu(dc, parent, name, seqp);
		if (d)
			break;
	} while (read_seqretry(&dc->rename_lock, r_seq));
	return d;
}

/* Outcome of one walk attempt. */
enum walk_ret {
	WALK_DONE,		/* *dp / *resp / *idp hold the answer */
	WALK_RESTART,		/* a d_seq check failed: walk again from the root */
};

/*
 * The kernel's RCU path walk -- path_init, then per component link_path_walk ->
 * walk_component -> lookup_fast -> step_into, then complete_walk -- over the
 * first @depth components of @p.  Per hop, exactly the kernel's d_seq touches:
 *
 *   __d_lookup_rcu  samples the child's d_seq (odd masked) before its compare;
 *   lookup_fast     re-checks the PARENT's d_seq: it did not move while we
 *                   looked up its child (a miss legitimizes it the same way,
 *                   in try_to_unlazy);
 *   step_into       re-checks the CHILD's d_seq before stepping into it.
 *
 * rename_lock is SAMPLED once, as path_init does -- which waits out a d_move in
 * flight, the one way a rename that does not touch the path still delays a
 * kernel walk -- and read again only on a MISS (d_lookup_rcu).  A rename
 * elsewhere in the tree does not restart this walk; one that moves a component
 * it is using does, through that component's d_seq.
 *
 * On a failed check the kernel drops to ref-walk and redoes the path with
 * references and locks; this port restarts the lockless walk, which is cheaper
 * and so, again, only flatters the baseline.  Mounts are not modelled, so
 * path_init's mount_lock sample and legitimize_mnt's check have no analogue.
 * @writer marks each component referenced for the LRU (lru_retain): the writer
 * resolve stands in for a ref-walk's dget/dput, while dc_lookup passes 0 and,
 * like __d_lookup_rcu, touches the LRU zero times.  Call under rcu_read_lock.
 */
static inline enum walk_ret path_walk_rcu(struct dcache *dc,
					  const struct dc_path *p,
					  uint32_t depth, int writer,
					  struct dentry **dp,
					  enum dc_result *resp, uint64_t *idp)
{
	struct dentry *cur = dc->root;
	enum dc_result res = DC_POSITIVE;
	unsigned long seq, next_seq;
	uint64_t id;
	uint32_t i;

	(void) read_seqbegin(&dc->rename_lock);		/* path_init: nd->r_seq */
	seq = read_seqcount_begin(&cur->d_seq);		/* set_root: root_seq */
	for (i = 0; i < depth; i++) {
		struct dentry *d = __d_lookup_rcu(dc, cur, &p->comp[i],
						  &next_seq);

		/* lookup_fast / try_to_unlazy: the parent held still */
		if (read_seqcount_retry(&cur->d_seq, seq))
			return WALK_RESTART;
		if (!d) {
			d = d_lookup_rcu(dc, cur, &p->comp[i], &next_seq);
			if (!d) {
				*dp = NULL;
				*resp = DC_ABSENT;
				return WALK_DONE;
			}
		}
		res = DC_IS_POSITIVE(d) ? DC_POSITIVE : DC_NEGATIVE;
		/* step_into: the child is still the one we matched */
		if (read_seqcount_retry(&d->d_seq, next_seq))
			return WALK_RESTART;
		if (writer) {
			/* the re-arm races a kill that lands right here */
			DC_TEST_RETAIN_DELAY();
			lru_retain(dc, d);
		}
		cur = d;
		seq = next_seq;
	}
	id = DC_FAST_ID(cur);
	/*
	 * complete_walk -> legitimize_path: the terminal once more.  The kernel
	 * takes its reference (lockref) between step_into and this check; this
	 * port takes none, so the two are back to back -- kept so the state and
	 * the (KEEPID) id read just above are covered even at depth 0.
	 */
	if (read_seqcount_retry(&cur->d_seq, seq))
		return WALK_RESTART;
	*dp = cur;
	*resp = res;
	*idp = id;
	return WALK_DONE;
}

#ifdef DC_SEQ_SNAPSHOT
/*
 * The SNAPSHOT arm's lookup: the kernel's forward walk -- the same per-hop
 * d_seq checks as path_walk_rcu() -- bracketed on rename_lock the way the
 * kernel brackets its REVERSE walks (__dentry_path, prepend_path, d_walk):
 * read_seqbegin_or_lock, one lockless pass and, if a rename ran during it, a
 * second pass HOLDING rename_lock, which no rename can then disturb.  A miss
 * needs no d_lookup fallback: the bracket covers the false negative a rename
 * can cause.
 *
 * A d_seq failure escalates to the locked pass only if rename_lock moved too,
 * as the kernel escalates only on need_seqretry(); otherwise it came from a
 * non-rename d_seq writer (unlink, delete, instantiate) and the lockless pass
 * simply re-runs.  The d_seq checks stay in the locked pass: rename_lock does
 * not exclude those writers, and they change the state (and, KEEPID, the id)
 * this lookup returns.  A failure there re-walks without dropping the lock --
 * it lasts only as long as one such writer's d_seq section.
 */
static inline void path_walk_snapshot(struct dcache *dc,
				      const struct dc_path *p,
				      enum dc_result *resp, uint64_t *idp)
{
	unsigned long seq = 0;			/* even: lockless first pass */
	unsigned long cseq, dseq;
	struct dentry *cur;
	enum dc_result res;
	uint64_t id;
	uint32_t i;

	rcu_read_lock();
restart:
	read_seqbegin_or_lock(&dc->rename_lock, &seq);
rewalk:
	cur = dc->root;
	res = DC_POSITIVE;
	id = DC_FAST_ID(cur);
	cseq = read_seqcount_begin(&cur->d_seq);	/* set_root */
	for (i = 0; i < p->ndepth; i++) {
		struct dentry *d = __d_lookup_rcu(dc, cur, &p->comp[i],
						  &dseq);

		if (read_seqcount_retry(&cur->d_seq, cseq))	/* lookup_fast */
			goto dseq_fail;
		if (!d) {
			res = DC_ABSENT;
			break;
		}
		res = DC_IS_POSITIVE(d) ? DC_POSITIVE : DC_NEGATIVE;
		id = DC_FAST_ID(d);
		if (read_seqcount_retry(&d->d_seq, dseq))	/* step_into */
			goto dseq_fail;
		cur = d;
		cseq = dseq;
	}
	/* complete_walk -> legitimize_path (see path_walk_rcu) */
	if (res != DC_ABSENT && read_seqcount_retry(&cur->d_seq, cseq))
		goto dseq_fail;
	if (need_seqretry(&dc->rename_lock, seq)) {
		walk_retry_count();
		seq = 1;			/* a rename ran: take the lock */
		goto restart;
	}
	done_seqretry(&dc->rename_lock, seq);
	rcu_read_unlock();
	*resp = res;
	*idp = id;
	return;

dseq_fail:
	walk_retry_count();
	if (seq & 1UL)
		goto rewalk;			/* locked: a non-rename writer */
	if (read_seqretry(&dc->rename_lock, seq))
		seq = 1;			/* a rename ran: take the lock */
	goto restart;				/* else lockless again */
}
#endif

enum dc_result dc_lookup(struct dcache *dc, const struct dc_path *p,
			 uint64_t *out_id)
{
	enum dc_result res;
	uint64_t id;

#ifdef DC_SEQ_SNAPSHOT
	path_walk_snapshot(dc, p, &res, &id);
#else
	unsigned long retries = 0;

	for (;;) {
		struct dentry *d;
		enum walk_ret w;

		rcu_read_lock();
		w = path_walk_rcu(dc, p, p->ndepth, 0, &d, &res, &id);
		rcu_read_unlock();
		if (caa_likely(w == WALK_DONE))
			break;
		/*
		 * Slow path: a d_seq check failed on a component this walk used,
		 * so it fires only for a rename (or unlink / state change) ON the
		 * path, never for one elsewhere -- zero cost at rename-fraction 0.
		 * A runaway count signals a livelock, not a hot workload; keep
		 * retrying but leave a tap for debug builds.
		 */
		walk_retry_count();
		if (++retries == (1UL << 24))
			__asm__ __volatile__("" ::: "memory");	/* placeholder tap */
		rcu_quiescent_state();
	}
#endif
	if (res == DC_POSITIVE && out_id)
		*out_id = id;
	return res;
}

/*
 * Lock-free resolve of `path`'s first `depth` components to its dentry, with the
 * kernel's fast-path walk in BOTH arms: a writer (or readdir) resolves its
 * targets the way the kernel's filename_parentat does, and the snapshot arm is
 * a statement about the lookup, not about the writers.  A rename of ANOTHER
 * node in a chain the walk crosses cannot produce a spurious miss here -- the
 * miss falls back to d_lookup_rcu's rename_lock retry -- and a rename of a
 * component it uses restarts it.  Caller holds rcu_read_lock, so the returned
 * dentry cannot be freed under it.  NULL if genuinely absent.
 */
static struct dentry *resolve_dentry_rcu(struct dcache *dc,
					 const struct dc_path *p, uint32_t depth)
{
	unsigned long retries = 0;

	for (;;) {
		struct dentry *d;
		enum dc_result res;
		uint64_t id;

		if (path_walk_rcu(dc, p, depth, 1, &d, &res, &id) == WALK_DONE)
			return d;
		if (++retries >= (1UL << 24))
			return NULL;			/* livelock guard */
	}
}

/* ---- writer-side helpers (under dir rwsem + bucket bit lock) ----------- */

/*
 * Writer-side exact lookup.  Two of its three callers -- resolve() and the
 * rename EXISTS check -- run WITHOUT the bucket lock, concurrent with another
 * writer's hlist_add_head_rcu / hlist_del_rcu on the same chain, so the traversal
 * must be a correct RCU reader (rcu_dereference), exactly like __d_lookup_rcu; a
 * plain-load walk can transiently lose a continuously-present node and miss it.
 * The match fields it reads (d_parent, d_name) belong to a node it is not itself
 * editing, and a node mid-rename can only fail to match (never falsely match a
 * different name), so no d_seq bracket is needed here.
 */
static struct dentry *__child_lookup(struct dcache *dc, struct dentry *parent,
				     const struct qstr *name)
{
	struct dc_bucket *b = bucket_of(dc, parent, name->hash);
	struct dc_hnode *n;

	for (n = bl_first_rcu(b); n; n = rcu_dereference(n->next)) {
		struct dentry *d = hnode_dentry(n);

		if (DC_DPARENT(d) == parent && !DC_IS_UNHASHED(d) &&
		    dc_qstr_eq(&d->d_name, name))
			return d;
	}
	return NULL;
}

/* Resolve the first `depth` components from the root; NULL if any is missing. */

/* Is `a` equal to `b` or a descendant of `b`?  (Directory-loop guard.) */
static int is_subdir(struct dentry *a, struct dentry *b)
{
	struct dentry *cur = a;

	for (;;) {
		if (cur == b)
			return 1;
		if (cur == DC_DPARENT(cur))	/* reached root */
			return 0;
		cur = DC_DPARENT(cur);
	}
}

/* ---- mutators ----------------------------------------------------------- */

static int dc_add_typed_state(struct dcache *dc, const struct dc_path *path,
			uint64_t id, int isdir, int positive)
{
	struct dentry *parent, *d;
	const struct qstr *name;
	struct dc_bucket *b;
	int ret = 0;

	if (path->ndepth == 0)
		return -EEXIST;			/* the root already exists */

	rcu_read_lock();
	parent = resolve_dentry_rcu(dc, path, path->ndepth - 1);
	if (!parent) {
		rcu_read_unlock();
		return -ENOENT;
	}
	if (!parent->d_isdir) {			/* a file has no children */
		rcu_read_unlock();
		return -ENOTDIR;
	}
	name = &path->comp[path->ndepth - 1];
	b = bucket_of(dc, parent, name->hash);
	/*
	 * dir rwsem (child list + same-dir serialization) then the bucket lock
	 * (hash chain).  The EXISTS check and the insert are both under the bucket
	 * lock, so a racing add of the same (parent, name) -- which hashes to this
	 * same bucket -- sees one or the other atomically.
	 */
	dir_wlock(parent);
	bl_lock(b);
	/*
	 * RE-CHECK the parent under the lock: a concurrent dc_delete of an empty
	 * DIRECTORY makes it negative, and a negative must never gain a child.
	 * dc_delete holds this same dir lock (the victim's own) while it checks
	 * d_children and flips the state, so this test under this lock is what
	 * makes the two atomic.  One predicted load-and-branch inside a critical
	 * section the add already entered -- no new lock.
	 *
	 * The same goes for an UNLINKED parent: the walk above may have found
	 * it before a concurrent dc_unlink removed it (empty at the time) and
	 * re-added the name as a NEW dentry, and a child linked under the old
	 * one is unreachable.  dc_unlink unhashes a directory holding its own
	 * dir lock -- this one -- so the test below and that unhash exclude each
	 * other.  It is the kernel's IS_DEADDIR(dir) check in may_create(), for
	 * the S_DEAD vfs_rmdir sets under the victim's i_rwsem.
	 */
	if (!DC_IS_POSITIVE(parent)
#ifndef DC_TEST_NO_ADD_ALIVE	/* MUTATION (check-rmdir must fail) */
	    || DC_IS_UNHASHED(parent)
#endif
	   ) {
		ret = -ENOENT;
		goto unlock;
	}
	if (__child_lookup(dc, parent, name)) {
		ret = -EEXIST;
		goto unlock;
	}
	d = dentry_alloc(name, parent, id, isdir, positive);
	if (!d) {
		ret = -ENOMEM;
		goto unlock;
	}
#ifndef DC_TEST_SEQ_LRU_ADD_LATE
	/* d_lru_add at the tail BEFORE the publish below: until it is hashed no
	 * walk can re-arm it, so the enqueue needs no claim (see lru_add_new). */
	lru_add_new(dc, d);
#endif
	/* A brand-new node has no readers yet: hlist_add_head_rcu is its one
	 * publish (release).  No rename_lock bump -- add doesn't move anything. */
	hlist_add_head_rcu(b, &d->d_hash);
	children_add(parent, d);
#ifdef DC_TEST_SEQ_LRU_ADD_LATE
	/* MUTATION (check-rmdir must fail): the pre-2026-10-02 order -- enqueue
	 * AFTER the publish, so a walk that finds @d first re-arms it onto its
	 * own shard and this links it a second time. */
	DC_TEST_RETAIN_DELAY();
	lru_add_new(dc, d);
#endif
unlock:
	bl_unlock(b);
	dir_wunlock(parent);
	rcu_read_unlock();
	return ret;
}

/* dc_add => directory; dc_add_file => file.  Kernel-faithful: the type gates
 * -ENOTDIR only; rename_lock still bumps regardless of type (see dcache.h). */
int dc_add(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	return dc_add_typed_state(dc, path, id, 1, 1);
}

int dc_add_file(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	return dc_add_typed_state(dc, path, id, 0, 1);
}

/*
 * Phase 2.  A negative dentry is a leaf by construction -- it caches the absence
 * of a name, and a name that is not there has no children -- so it is created
 * with the FILE type, which also makes an add-under-it return -ENOTDIR rather
 * than inventing children below a name that does not exist.
 */
int dc_add_negative(struct dcache *dc, const struct dc_path *path)
{
	return dc_add_typed_state(dc, path, 0, 0, 0);
}

/*
 * Phase 2: d_instantiate.  The dentry keeps its address, its bucket and its
 * place in the child list; only its state changes.
 *
 * This is the case the per-dentry seqcount already exists for.  The lockless
 * reader validates d_seq before consuming a component, so bracketing the state
 * change in write_seqcount_begin/end makes a walk that straddles it retry rather
 * than mix an old inode-ness with a new one -- no new mechanism, which is
 * precisely the baseline's advantage here and the thing the txn engines had to
 * replace after deleting d_seq.
 *
 * Taken under the bucket lock, like every other state change on a hashed dentry,
 * so a concurrent unlink or add of the same name serializes against it.
 */
int dc_instantiate(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	struct dentry *parent, *d;
	const struct qstr *name;
	struct dc_bucket *b;
	int ret = 0;

	if (path->ndepth == 0)
		return -EEXIST;			/* the root is always positive */

	rcu_read_lock();
	parent = resolve_dentry_rcu(dc, path, path->ndepth - 1);
	if (!parent) {
		rcu_read_unlock();
		return -ENOENT;
	}
	name = &path->comp[path->ndepth - 1];
	b = bucket_of(dc, parent, name->hash);
	dir_wlock(parent);
	bl_lock(b);
	d = __child_lookup(dc, parent, name);
	if (!d) {
		ret = -ENOENT;
		goto unlock;
	}
	if (DC_IS_POSITIVE(d)) {
		ret = -EEXIST;
		goto unlock;
	}
	write_seqcount_begin(&d->d_seq);
#if defined(DC_HOT1CL_SPLIT)
	CMM_STORE_SHARED(d->d_parent, (struct dentry *)
			 ((uintptr_t) d->d_parent & ~DC_TAG_NEG));
#else
	CMM_STORE_SHARED(d->d_inode, 1);
#endif
	d->d_id = id;
	write_seqcount_end(&d->d_seq);
unlock:
	bl_unlock(b);
	dir_wunlock(parent);
	rcu_read_unlock();
	return ret;
}

/*
 * Phase 2: d_delete with a surviving reference -- make the dentry NEGATIVE in
 * place rather than unhashing it.  The inverse of dc_instantiate, and in this
 * engine it is the same three lines inside the same bracket: d_seq is what
 * makes a state change on a live, hashed dentry coherent to a lockless reader,
 * so there is nothing to add.  That is the comparison this operation exists to
 * draw -- the txn engines must publish it through a commit because they deleted
 * the counter that does this here for free.
 *
 * FILES ONLY (-EISDIR).  The restriction is not needed for THIS engine's safety
 * -- the bucket lock and the parent's dir lock would serialize a concurrent add
 * against the check -- but the invariant "every negative is a file" has to hold
 * ENGINE-WIDE or the harness could not assert it across the matrix, and in the
 * txn engines it is the only race-free way to get it (see dcache.h).  So the
 * baseline is deliberately restricted to what its partners can also promise.
 *
 * d_id is likewise left stale, which this engine alone need not do: the bracket
 * makes clearing it atomic with the state.  The txn engines cannot -- a
 * single-slot commit publishes the state and nothing else -- so the census
 * skips negatives everywhere (walk_rec) and every engine behaves the same.
 */
int dc_delete(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *parent, *d, *peek;
	const struct qstr *name;
	struct dc_bucket *b;
	int isdir, ret = 0;

	if (path->ndepth == 0)
		return -EISDIR;			/* the root cannot be removed */

	rcu_read_lock();
	parent = resolve_dentry_rcu(dc, path, path->ndepth - 1);
	if (!parent) {
		rcu_read_unlock();
		return -ENOENT;
	}
	name = &path->comp[path->ndepth - 1];
	b = bucket_of(dc, parent, name->hash);

	for (;;) {
		/*
		 * PEEK the victim locklessly to learn its TYPE, because the type
		 * decides which locks to take and the locks are address-ordered
		 * (dirs_wlock2) -- so the pair has to be acquired together, not
		 * escalated once we are already holding one.  d_isdir is
		 * write-once, so the peek cannot be stale about the type; what
		 * it CAN be stale about is which node holds the name, which the
		 * re-find under the lock catches.
		 */
		peek = resolve_dentry_rcu(dc, path, path->ndepth);
		isdir = peek && peek->d_isdir;

		if (isdir)
			dirs_wlock2(parent, peek);
		else
			dir_wlock(parent);
		bl_lock(b);
		d = __child_lookup(dc, parent, name);
		if (isdir && d != peek) {	/* raced: re-peek and re-lock */
			bl_unlock(b);
			dirs_wunlock2(parent, peek);
			continue;
		}
		break;
	}

	if (!d) {
		ret = -ENOENT;
		goto unlock;
	}
	if (!DC_IS_POSITIVE(d)) {		/* already caches an absence */
		ret = -ENOENT;
		goto unlock;
	}
	/*
	 * A DIRECTORY must be EMPTY, and must stay empty across the flip -- a
	 * negative that could gain a child would let a walk find something
	 * beneath a name that is not there.  Holding the VICTIM's own dir lock
	 * is what makes that stick, because dc_add takes its parent's dir lock,
	 * and the victim is that parent.  Exactly why the kernel's rmdir holds
	 * the victim's i_rwsem.  A FILE needs none of it: d_isdir is write-once
	 * and dc_add answers -ENOTDIR under one.
	 */
	if (d->d_isdir && d->d_children) {
		ret = -ENOTEMPTY;
		goto unlock;
	}
	write_seqcount_begin(&d->d_seq);
#if defined(DC_HOT1CL_SPLIT)
	CMM_STORE_SHARED(d->d_parent, (struct dentry *)
			 ((uintptr_t) d->d_parent | DC_TAG_NEG));
#else
	CMM_STORE_SHARED(d->d_inode, 0);
#endif
	write_seqcount_end(&d->d_seq);
unlock:
	bl_unlock(b);
	if (isdir)
		dirs_wunlock2(parent, peek);
	else
		dir_wunlock(parent);
	rcu_read_unlock();
	return ret;
}

static void dentry_free_cb(struct rcu_head *rh)
{
	struct dentry *d = caa_container_of(rh, struct dentry, d_rcu);

	dir_lock_destroy(&d->d_lock);
	free(d);
}

int dc_unlink(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *victim, *parent;
	struct dc_bucket *b;
	int isdir, ret = 0;

	if (path->ndepth == 0)
		return -EINVAL;			/* cannot unlink the root */

	rcu_read_lock();
	for (;;) {
		victim = resolve_dentry_rcu(dc, path, path->ndepth);
		if (!victim) {
			rcu_read_unlock();
			return -ENOENT;
		}
		parent = DC_DPARENT(victim);
		/*
		 * A DIRECTORY victim is locked too, with its parent, address-
		 * ordered as dc_delete does: the kernel's rmdir holds the victim's
		 * i_rwsem, and dc_add holds its parent's dir lock -- the victim's
		 * own -- across its parent check and link, so the emptiness test
		 * below and every add under the victim exclude each other.  With
		 * only the parent's lock, an add could link a child after the test
		 * passed and the child was lost under a freed directory
		 * (stress_dcache_rmdir found 6726).  d_isdir is write-once, so the
		 * lockless peek cannot be stale about the type.
		 */
		isdir = victim->d_isdir;
#ifdef DC_TEST_NO_RMDIR_LOCK	/* MUTATION (check-rmdir must fail) */
		isdir = 0;
#endif
		if (isdir)
			dirs_wlock2(parent, victim);
		else
			dir_wlock(parent);
		/*
		 * RE-VERIFY under the parent's lock, which pins the victim's
		 * parent and name (rename takes it): still hashed -- or a
		 * concurrent unlink got there first and this one would unhash and
		 * free it a second time -- and still under @parent, at the name
		 * the bucket is taken from.
		 */
		if (!DC_IS_UNHASHED(victim) && DC_DPARENT(victim) == parent)
			break;
		if (isdir)
			dirs_wunlock2(parent, victim);
		else
			dir_wunlock(parent);
	}
	b = bucket_of(dc, parent, victim->d_name.hash);
	bl_lock(b);
	if (victim->d_children) {
		ret = -ENOTEMPTY;
		goto unlock;
	}
	/* Publish the removal under d_seq so a reader mid-compare on the victim
	 * re-scans and misses it; hlist_del_rcu splices it for new readers. */
	write_seqcount_begin(&victim->d_seq);
	DC_SET_UNHASHED(victim);
	hlist_del_rcu(&victim->d_hash);
	write_seqcount_end(&victim->d_seq);
	children_remove(parent, victim);
	/* list_lru_del, IMMEDIATELY: the call_rcu free below cannot fire while a
	 * shard still points at this node, so deferring the removal to the
	 * shrinker would gate reclaim on memory pressure instead of on the grace
	 * period.  See design/dcache-lru-txn.md section 6. */
	lru_del(dc, victim);
	call_rcu(&victim->d_rcu, dentry_free_cb);	/* honest deferred free */
unlock:
	bl_unlock(b);
	if (isdir)
		dirs_wunlock2(parent, victim);
	else
		dir_wunlock(parent);
	rcu_read_unlock();
	return ret;
}

/*
 * __d_move: relocate `victim` so its parent becomes `new_parent` and its name
 * becomes `new_name`.  The caller holds both dirs' rwsems (lock_rename) and
 * rename_lock's write side (d_move), so the whole move is one even->odd->even
 * transition to lockless walkers; this takes the two bucket locks and bumps the
 * victim's own d_seq so a walker mid-compare on it retries.
 *
 * `target` is a NEGATIVE dentry already holding (new_parent, new_name), or
 * NULL.  As in the kernel's __d_move, it is dropped from the hash inside the
 * same write sections (its own d_seq bracketed, as the kernel's
 * write_seqcount_begin_nested(&target->d_seq) does) and off its parent's child
 * list; the caller frees it after a grace period.  It sits in `nb`.
 */
static void __d_move(struct dcache *dc, struct dentry *victim,
		     struct dentry *new_parent, const struct qstr *new_name,
		     struct dentry *target)
{
	struct dentry *old_parent = DC_DPARENT(victim);
	struct dc_bucket *ob = bucket_of(dc, old_parent, victim->d_name.hash);
	struct dc_bucket *nb = bucket_of(dc, new_parent, new_name->hash);

	/* Both hash buckets (the old chain it leaves, the new it enters), so the
	 * del + add is atomic against a concurrent add/unlink on either chain. */
	bl_lock2(ob, nb);
	if (target) {
		write_seqcount_begin(&target->d_seq);
		DC_SET_UNHASHED(target);
		hlist_del_rcu(&target->d_hash);
		children_remove(new_parent, target);
		write_seqcount_end(&target->d_seq);
	}
	write_seqcount_begin(&victim->d_seq);
	hlist_del_rcu(&victim->d_hash);			/* leave old bucket */
	if (old_parent != new_parent)
		children_remove(old_parent, victim);
	victim->d_name = *new_name;			/* identity change */
	/*
	 * PRESERVE the low-bit tags.  Under the 1-CL split layout d_parent
	 * carries DC_TAG_UNHASHED and DC_TAG_NEG, so assigning the bare pointer
	 * here would silently make a renamed dentry POSITIVE (and hashed).  A
	 * rename changes the name, never the inode-ness -- phase 2 is what makes
	 * that observable, since before it every dentry was born positive and the
	 * lost bit could not be seen.
	 */
	rcu_assign_pointer(victim->d_parent, (struct dentry *)
			   ((uintptr_t) new_parent |
			    ((uintptr_t) victim->d_parent & DC_TAG_MASK)));
	hlist_add_head_rcu(nb, &victim->d_hash);	/* enter new bucket */
	if (old_parent != new_parent)
		children_add(new_parent, victim);
	write_seqcount_end(&victim->d_seq);
	bl_unlock2(ob, nb);
}

int dc_rename(struct dcache *dc, const struct dc_path *from,
	      const struct dc_path *to)
{
	struct dentry *victim, *to_parent, *from_parent, *target = NULL;
	const struct qstr *to_name;
	int cross, ret = 0;

	if (from->ndepth == 0 || to->ndepth == 0)
		return -EINVAL;			/* cannot move the root */

	rcu_read_lock();
	victim = resolve_dentry_rcu(dc, from, from->ndepth);
	if (!victim) {
		ret = -ENOENT;
		goto out;
	}
	to_parent = resolve_dentry_rcu(dc, to, to->ndepth - 1);
	if (!to_parent) {
		ret = -ENOENT;
		goto out;
	}
	/*
	 * Cross-directory move: take the global s_vfs_rename_mutex, exactly as the
	 * kernel's lock_rename does for p1 != p2 (a same-dir rename takes only the
	 * one dir's rwsem, no global lock).  It makes the loop check + reparent
	 * atomic against another cross-move -- without it two moves can each pass
	 * is_subdir and splice a cycle -- and it is the kernel-faithful cost of any
	 * cross-directory rename (file or directory alike).
	 */
	from_parent = DC_DPARENT(victim);
	cross = (from_parent != to_parent);
	if (cross)
		pthread_mutex_lock(&dc->vfs_rename_mutex);
	to_name = &to->comp[to->ndepth - 1];
	if (!DC_IS_POSITIVE(to_parent)) {
		/* advisory; __d_move re-checks under to_parent's dir lock */
		ret = -ENOENT;
		goto out_unlock;
	}
	if (cross && is_subdir(to_parent, victim)) {
		ret = -EINVAL;			/* would create a loop */
		goto out_unlock;
	}
	/*
	 * lock_rename: write-lock both affected dirs (one if unchanged) BEFORE
	 * rename_lock, as the kernel takes i_rwsem long before vfs_rename reaches
	 * d_move.  They exclude a concurrent readdir of the old dir (child
	 * leaving), of the new dir (child arriving), AND of the same dir when only
	 * the name changes in place -- a readdir must not see a torn name.  Taken
	 * inside rename_lock instead, they would hold rename_lock odd -- stalling
	 * every new walk in path_init -- for as long as a readdir or an add held
	 * either dir.
	 */
	dirs_wlock2(from_parent, to_parent);
	/*
	 * A NEGATIVE destination directory must not gain a child, and a rename
	 * INTO it is the SECOND way that can happen -- dc_add is the first, and
	 * guarding only dc_add left this hole.  Checked under to_parent's OWN dir
	 * lock, which is the lock dc_delete holds while it verifies d_children
	 * and flips the state; that is what makes the two atomic.  Only cross-
	 * parent can hit it: a negative directory has no children, so a same-dir
	 * rename under one has nothing to rename.
	 */
	if (cross && !DC_IS_POSITIVE(to_parent)) {
		ret = -ENOENT;
		goto out_dirs;
	}
	/*
	 * The destination, looked up UNDER to_parent's dir lock -- as the
	 * kernel looks the target up under i_rwsem after lock_rename -- so no
	 * add or rename can change it before the move.  A positive occupant is
	 * -EEXIST (no rename-over-positive in this model); a NEGATIVE one is
	 * replaced, as d_move drops a negative target.
	 */
	target = __child_lookup(dc, to_parent, to_name);
	if (target && (target == victim || DC_IS_POSITIVE(target))) {
		ret = -EEXIST;
		target = NULL;
		goto out_dirs;
	}
	/* d_move: rename_lock seals the move for the lockless walkers and
	 * serializes it against every other rename. */
	write_seqlock(&dc->rename_lock);
	__d_move(dc, victim, to_parent, to_name, target);
	write_sequnlock(&dc->rename_lock);
out_dirs:
	dirs_wunlock2(from_parent, to_parent);
out_unlock:
	if (cross)
		pthread_mutex_unlock(&dc->vfs_rename_mutex);
out:
	rcu_read_unlock();
	if (target) {			/* the dropped negative: dc_unlink's tail */
		lru_del(dc, target);
		call_rcu(&target->d_rcu, dentry_free_cb);
	}
	return ret;
}

int dc_rename_exchange(struct dcache *dc, const struct dc_path *a,
		       const struct dc_path *b)
{
	struct dentry *da, *db, *pa, *pb;
	struct dc_bucket *ba, *bb;
	struct qstr na, nb;
	int cross = 0, ret = 0;

	if (a->ndepth == 0 || b->ndepth == 0)
		return -EINVAL;

	rcu_read_lock();
	da = resolve_dentry_rcu(dc, a, a->ndepth);
	db = resolve_dentry_rcu(dc, b, b->ndepth);
	if (!da || !db) {
		ret = -ENOENT;
		goto out;
	}
	if (da == db)
		goto out;			/* exchanging a node with itself */
	pa = DC_DPARENT(da);
	pb = DC_DPARENT(db);
	/* Cross-directory exchange takes the global s_vfs_rename_mutex too, so the
	 * two-directional loop check below is atomic with the swap (see dc_rename). */
	cross = (pa != pb);
	if (cross)
		pthread_mutex_lock(&dc->vfs_rename_mutex);
	/* Neither may end up under the other (no loops in either direction). */
	if (cross && (is_subdir(pb, da) || is_subdir(pa, db))) {
		ret = -EINVAL;
		goto out_unlock;
	}
	na = da->d_name;
	nb = db->d_name;

	ba = bucket_of(dc, pa, na.hash);	/* da leaves here, db enters */
	bb = bucket_of(dc, pb, nb.hash);	/* db leaves here, da enters */
	dirs_wlock2(pa, pb);			/* lock_rename, BEFORE d_exchange */
	write_seqlock(&dc->rename_lock);
	bl_lock2(ba, bb);
	/*
	 * Drop both, then re-add both at swapped positions -- one rename_lock
	 * section, so a walker sees the exchange atomically.  d_seq on each is
	 * bumped by the two __d_move-style brackets below.
	 */
	write_seqcount_begin(&da->d_seq);
	write_seqcount_begin(&db->d_seq);
	hlist_del_rcu(&da->d_hash);
	hlist_del_rcu(&db->d_hash);
	if (pa != pb) {
		children_remove(pa, da);
		children_remove(pb, db);
	}
	da->d_name = nb;
	db->d_name = na;
	/* Keep each node's own low-bit tags (unhashed / negative), as __d_move
	 * does: an exchange trades positions, never inode-ness. */
	rcu_assign_pointer(da->d_parent, (struct dentry *)
			   ((uintptr_t) pb |
			    ((uintptr_t) da->d_parent & DC_TAG_MASK)));
	rcu_assign_pointer(db->d_parent, (struct dentry *)
			   ((uintptr_t) pa |
			    ((uintptr_t) db->d_parent & DC_TAG_MASK)));
	hlist_add_head_rcu(bb, &da->d_hash);		/* da enters pb's bucket */
	hlist_add_head_rcu(ba, &db->d_hash);		/* db enters pa's bucket */
	if (pa != pb) {
		children_add(pb, da);
		children_add(pa, db);
	}
	write_seqcount_end(&db->d_seq);
	write_seqcount_end(&da->d_seq);
	bl_unlock2(ba, bb);
	write_sequnlock(&dc->rename_lock);
	dirs_wunlock2(pa, pb);
out_unlock:
	if (cross)
		pthread_mutex_unlock(&dc->vfs_rename_mutex);
out:
	rcu_read_unlock();
	return ret;
}

/*
 * List a directory.  Faithful baseline: readdir read-locks the directory's own
 * rwsem (the kernel holds that dir's i_rwsem), so listing is serialized only
 * against mutations of THIS dir's child list -- a consistent snapshot that
 * contends with add/remove/rename in the same dir.  This is the cost the txn
 * engine's lock-free child-hlist is meant to beat.
 */
long dc_readdir(struct dcache *dc, const struct dc_path *path,
		dc_dirent_fn fn, void *arg)
{
	struct dentry *dir, *c;
	long count = 0;

	/*
	 * Kernel-faithful readdir: navigate to the directory with the lock-free
	 * RCU walk (no writer lock), then take that directory's rwsem read-side
	 * for the child enumeration -- the analogue of iterate_dir() under the
	 * inode rwsem.  Concurrent readdirs of the same dir share; only a mutator
	 * changing THIS dir's child listing is excluded.  rcu_read_lock keeps the
	 * resolved dir alive across the lock acquisition (frees are call_rcu'd).
	 */
	rcu_read_lock();
	dir = resolve_dentry_rcu(dc, path, path->ndepth);
	if (!dir) {
		rcu_read_unlock();
		return -ENOENT;
	}
	dir_rlock(dir);
	for (c = dir->d_children; c; c = c->d_sib) {
		if (fn)
			fn(c->d_id, &c->d_name, arg);
		count++;
	}
	dir_runlock(dir);
	rcu_read_unlock();
	return count;
}

/* ---- reverse walk (dentry_path_raw) ------------------------------------- */

struct dentry *dc_lookup_dentry(struct dcache *dc, const struct dc_path *p)
{
	struct dentry *d;
	enum dc_result res;
	uint64_t id;

	for (;;) {
		enum walk_ret w;

		rcu_read_lock();
		w = path_walk_rcu(dc, p, p->ndepth, 0, &d, &res, &id);
		rcu_read_unlock();
		if (w == WALK_DONE)
			break;
		rcu_quiescent_state();
	}
	return res == DC_POSITIVE ? d : NULL;
}

/*
 * dentry_path_raw (fs/d_path.c __dentry_path), step for step: climb d_parent
 * from @d to the root copying each name, bracketed on rename_lock with
 * read_seqbegin_or_lock -- one lockless pass, and if a rename ran during it, a
 * second pass holding rename_lock, which cannot fail.  No d_seq is consulted,
 * on the kernel's own argument (prepend_path): every change that could tear
 * this climb -- a name or parent change, i.e. a __d_move -- happens inside
 * rename_lock's write section.  An unlink / delete / instantiate changes
 * neither.  Ancestors of a live object cannot be unlinked (they are not empty),
 * so the climb never reaches freed memory.
 *
 * The name copy is optimistic, as prepend_name's is: a racing __d_move may tear
 * it, and the rename_lock retry discards that pass.  The length is clamped so a
 * torn one can never over-read the inline buffer.  Components are filled from
 * the END of @out (the climb meets them leaf first, as the kernel's prepend
 * buffer does) and moved to the front once the snapshot is known good.
 */
int dc_dentry_path(struct dcache *dc, const struct dentry *d,
		   struct dc_path *out)
{
	unsigned long seq = 0;			/* even: lockless first pass */
	const struct dentry *cur;
	uint32_t n;
	int ret;

	rcu_read_lock();
restart:
	n = 0;
	ret = 0;
	read_seqbegin_or_lock(&dc->rename_lock, &seq);
	for (cur = d;;) {
		const struct dentry *parent = (const struct dentry *)
			((uintptr_t) CMM_LOAD_SHARED(cur->d_parent) &
			 ~DC_TAG_MASK);
		struct qstr *q;
		uint32_t len;

		if (parent == cur)
			break;				/* IS_ROOT */
		if (n == DC_PATH_MAX) {
			ret = -ENAMETOOLONG;
			break;
		}
		q = &out->comp[DC_PATH_MAX - 1 - n];
		len = CMM_LOAD_SHARED(cur->d_name.len);
		if (len > DC_NAME_MAX - 1)
			len = DC_NAME_MAX - 1;
		q->hash = CMM_LOAD_SHARED(cur->d_name.hash);
		q->len = len;
		memcpy(q->name, cur->d_name.name, len);
		q->name[len] = '\0';
		n++;
		cur = parent;
	}
	if (need_seqretry(&dc->rename_lock, seq)) {
		walk_retry_count();
		seq = 1;
		goto restart;
	}
	done_seqretry(&dc->rename_lock, seq);
	rcu_read_unlock();
	if (ret)
		return ret;
	memmove(&out->comp[0], &out->comp[DC_PATH_MAX - n],
		n * sizeof(out->comp[0]));
	out->ndepth = n;
	return 0;
}

/* ---- verification walk (quiescent) ------------------------------------- */

static void walk_rec(struct dentry *d, struct dc_path *path, dc_visit_fn fn,
		     void *arg)
{
	struct dentry *c;

	/* Skip the root, and skip NEGATIVES: the census counts OBJECTS, and a
	 * negative dentry holds a name without one -- its d_id is stale by
	 * construction (kept stale here to match the txn engines, which cannot
	 * clear it atomically with the state).  Reporting it would make a
	 * conservation gate read a cached absence as a surviving object. */
	if ((path->ndepth > 0 || DC_DPARENT(d) != d) && DC_IS_POSITIVE(d))
		fn(d->d_id, path, arg);

	for (c = d->d_children; c; c = c->d_sib) {
		if (path->ndepth >= DC_PATH_MAX)
			continue;		/* too deep to represent */
		path->comp[path->ndepth++] = c->d_name;
		walk_rec(c, path, fn, arg);
		path->ndepth--;
	}
}

#ifndef DC_NO_LRU
/*
 * PHASE 3: prune_dcache_sb / dentry_lru_isolate (fs/dcache.c:1179).
 *
 * The kernel's isolate is a CLOCK:
 *   d_lockref.count != 0   -> in use  -> LRU_REMOVED (a later last-put re-adds
 *                                        it; retain_dentry does that, and so
 *                                        does lru_retain here)
 *   DCACHE_REFERENCED      -> clear it -> LRU_ROTATE (second chance, to tail)
 *   otherwise              -> move to the shrink list -> killed
 *
 * "In use" maps to HAS CHILDREN: a cached child pins its parent's refcount in
 * the kernel, so a populated directory is never a candidate.  LRU_REMOVED is
 * the faithful answer precisely BECAUSE lru_retain re-arms it -- which is why
 * the baseline can do what the txn engine could not and had to rotate instead.
 *
 * BATCH-ISOLATE, which is the shape that matters for contention.  Mainline
 * isolates victims onto a private list under the shard lock (DCACHE_SHRINK_LIST)
 * and then kills them WITHOUT it, so the foreground never waits on a shard lock
 * for the length of an unlink.  It bounds contention rather than removing it --
 * and that bound is exactly what the txn engine's arms are measured against, so
 * reproducing it is the difference between a fair comparison and a straw man.
 */
#define DC_SHRINK_BATCH	16

/*
 * __dentry_kill for an isolated victim: unhash it, unlink it from its parent's
 * child list and RCU-defer the free.  This is dc_unlink's core taking the
 * DENTRY rather than a path, which is also what the kernel's shrinker does --
 * it kills the dentry it isolated, it does not re-look-it-up by name.
 *
 * Re-verified under the locks, because the shard lock was dropped before we got
 * here: a rename could have reparented it, an unlink could have taken it, and a
 * create could have given it a child.  Returns 0 if it was killed.
 */
static int lru_kill(struct dcache *dc, struct dentry *d)
{
	struct dentry *parent;
	struct dc_bucket *b;
	int ret = -1;

	rcu_read_lock();
	parent = DC_DPARENT(d);
	if (!parent || parent == d) {		/* the root anchors the tree */
		(void) uatomic_cmpxchg(&d->d_lru.shard, DC_SEQ_LRU_SHRINK,
				       DC_SEQ_LRU_OFF);
		rcu_read_unlock();
		return -1;
	}
	b = bucket_of(dc, parent, d->d_name.hash);
	dir_wlock(parent);
	bl_lock(b);
	if (DC_DPARENT(d) == parent && !DC_IS_UNHASHED(d) && !d->d_children) {
		write_seqcount_begin(&d->d_seq);
		DC_SET_UNHASHED(d);
		hlist_del_rcu(&d->d_hash);
		write_seqcount_end(&d->d_seq);
		children_remove(parent, d);
		/* SHRINK -> DEAD: only a kill moves a SHRINK word, and it would
		 * have unhashed @d under these locks first */
		uatomic_store(&d->d_lru.shard, DC_SEQ_LRU_DEAD, CMM_RELAXED);
		call_rcu(&d->d_rcu, dentry_free_cb);
		ret = 0;
	}
	bl_unlock(b);
	dir_wunlock(parent);
	/*
	 * Not killed: still alive (it gained a child or moved), so put it back
	 * to OFF for the next walk to re-arm -- unless an unlink killed it in the
	 * meantime and sealed it DEAD, which this cmpxchg then leaves alone.
	 * Inside the read-side section, so @d cannot have been freed yet.
	 */
	if (ret)
		(void) uatomic_cmpxchg(&d->d_lru.shard, DC_SEQ_LRU_SHRINK,
				       DC_SEQ_LRU_OFF);
	rcu_read_unlock();
	return ret;
}

static long lru_shrink_nodes(struct dcache *dc, long nr,
			     unsigned int lo, unsigned int hi)
{
	long freed = 0;
	unsigned int i;

	for (i = lo; i < hi && freed < nr; i++) {
		struct dc_lru_one *l = &dc->s_dentry_lru[i];
		struct dentry *batch[DC_SHRINK_BATCH];
		unsigned long scanned, budget;
		int n, k;

		do {
			n = 0;
			budget = uatomic_load(&l->nr_items, CMM_RELAXED);
			scanned = 0;

			/* ---- isolate, under the shard lock ---- */
			lru_lock(l);
			while (n < DC_SHRINK_BATCH && (long) (freed + n) < nr &&
			       scanned++ < budget) {
				struct dentry *d = l->head;

				if (!d)
					break;
				if (uatomic_load(&d->d_lru.referenced,
						 CMM_RELAXED)) {
					uatomic_store(&d->d_lru.referenced, 0,
						      CMM_RELAXED);
					/* LRU_ROTATE: stays ON(i) */
					lru_del_locked(l, d, DC_SEQ_LRU_ON(i));
					lru_link_tail_locked(l, d);
					continue;
				}
				if (d->d_children) {
					/* LRU_REMOVED: alive, re-armable */
					lru_del_locked(l, d, DC_SEQ_LRU_OFF);
					continue;
				}
				/* isolated: no re-arm until killed or put back */
				lru_del_locked(l, d, DC_SEQ_LRU_SHRINK);
				batch[n++] = d;
			}
			lru_unlock(l);

			/* ---- kill, with the lock DROPPED ---- */
			for (k = 0; k < n; k++)
				if (lru_kill(dc, batch[k]) == 0)
					freed++;
		} while (n == DC_SHRINK_BATCH && freed < nr);
	}
	return freed;
}

long dc_shrink(struct dcache *dc, long nr)
{
	if (nr <= 0)
		return 0;
	return lru_shrink_nodes(dc, nr, 0, DC_LRU_NODES);
}

/* Only the caller's own node shard; see dcache.h. */
long dc_shrink_local(struct dcache *dc, long nr)
{
	unsigned int nid = lru_nid();

	if (nr <= 0)
		return 0;
	return lru_shrink_nodes(dc, nr, nid, nid + 1);
}
#endif	/* DC_NO_LRU */

void dc_walk(struct dcache *dc, dc_visit_fn fn, void *arg)
{
	struct dc_path path;

	dc_path_reset(&path);
	walk_rec(dc->root, &path, fn, arg);
}
