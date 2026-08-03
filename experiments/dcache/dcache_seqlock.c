// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_seqlock.c -- faithful kernel-style userspace dentry cache.
 *
 * This is the BASELINE the urcu-txn port (dcache_txn, S2) must beat and simplify.
 * It reproduces the kernel's actual RCU-walk consistency scheme:
 *
 *   - a (parent, name-hash) hash table of RCU hlists (the dentry_hashtable /
 *     hlist_bl analog); lockless readers traverse a bucket with rcu_dereference;
 *   - a GLOBAL rename_lock seqlock, bumped by every d_move, that brackets the
 *     whole multi-component walk: any rename anywhere forces the walk to retry;
 *   - a per-dentry d_seq seqcount, validated as the walk steps from a dentry to
 *     its child, so a single-component match (parent + name) is self-consistent
 *     even while that dentry is being renamed.
 *
 * The mechanism the txn port deletes is exactly (rename_lock + d_seq): global +
 * per-object sequence counters read on the fast path.
 *
 * Kernel-faithful write-side locking (see README): the write path mirrors the
 * kernel's granularity so writer THROUGHPUT is a fair comparison too, not only
 * the reader path.  A structural mutator takes the per-directory rwsem of the
 * dir(s) it touches (the i_rwsem analog, guarding each dir's child list and
 * serializing same-dir mutators) and the per-bucket lock of the hash bucket(s)
 * it edits (an hlist_bl bit lock in bit 0 of the bucket head word, guarding each
 * hash chain) -- NOT one global lock, so add/unlink in different dirs and buckets
 * proceed in parallel exactly as they do in the kernel.  Every rename / exchange
 * takes rename_lock (the seqlock the reader validates on); a CROSS-directory one
 * additionally takes a single global s_vfs_rename_mutex, as the kernel's
 * lock_rename does for p1 != p2, making the loop check atomic with the reparent
 * (a same-dir rename takes neither the mutex nor the loop check).  Lock ordering,
 * outermost first: vfs_rename_mutex -> rename_lock -> dir rwsems (address-ordered)
 * -> bucket locks (address-ordered); no mutator takes a dir lock while already
 * holding a bucket lock, so the two-level hierarchy cannot cycle.  d_seq is written under the bucket lock
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
#endif

#include "dcache.h"
#include "dcache_txn_stats.h"
#include "seqcount.h"

/*
 * Per-directory lock TYPE.  Default is the glibc pthread_rwlock (reader- or
 * writer-preferring, see dir_lock_init below).  -DDC_DIR_LOCK_KRWSEM swaps in
 * the VENDORED Linux kernel rw_semaphore (krwsem/, GPL-2.0) -- the FAITHFUL
 * fair/writer-non-starving lock the kernel actually uses for inode->i_rwsem --
 * sized (56 B) to match pthread_rwlock_t so the dentry footprint is unchanged.
 */
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
 * Total lookup-walk restarts forced by a concurrent rename (global rename_lock
 * or a stepped-into d_seq).  The benchmark reads this via a WEAK reference so it
 * stays engine-agnostic: the txn engine, which never retries a walk, simply does
 * not define the symbol and the harness reports it as N/A.  Only touched on the
 * retry slow path (zero cost at rename-fraction 0).
 */
unsigned long dc_seq_walk_retries;

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

struct dcache {
	struct dc_lru_one s_dentry_lru[DC_LRU_NODES];
	struct dc_bucket *buckets;	/* each head word carries its own bit lock */
	unsigned long mask;		/* nbuckets - 1 (power of two) */
	struct dentry *root;
	seqlock_t rename_lock;		/* GLOBAL: the walk's consistency anchor */
	pthread_mutex_t vfs_rename_mutex; /* s_vfs_rename_mutex: cross-dir moves only */
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
 */
static inline void bl_lock(struct dc_bucket *b)
{
	uintptr_t *p = (uintptr_t *) &b->first;

	while (__atomic_fetch_or(p, DC_BL_LOCK, __ATOMIC_ACQUIRE) & DC_BL_LOCK)
		caa_cpu_relax();
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
#ifndef DC_NO_LRU

/* list_lru_one.lock -- a plain spinlock, as in the kernel. */
static inline void lru_lock(struct dc_lru_one *l)
{
	while (uatomic_cmpxchg(&l->lock, 0UL, 1UL) != 0UL)
		caa_cpu_relax();
	cmm_smp_mb();
}

static inline void lru_unlock(struct dc_lru_one *l)
{
	uatomic_store(&l->lock, 0UL, CMM_RELEASE);
}

/*
 * The shard: the NUMA node, exactly as the kernel indexes `lru->node[nid]`.
 * Read from the rseq ABI page -- a plain load, no syscall.  The kernel derives
 * nid from the OBJECT's memory (page_to_nid); under first-touch the enqueueing
 * thread's node is that same node, and it is captured once and remembered so a
 * later del from another node still finds the list the dentry is actually on.
 */
static inline unsigned int lru_nid(void)
{
	unsigned int nid = rseq_node_id_available() ? rseq_current_node_id() : 0u;

	return nid < DC_LRU_NODES ? nid : nid % DC_LRU_NODES;
}

/* list_lru_add: at the TAIL.  @d must not already be on a list. */
static void lru_add(struct dcache *dc, struct dentry *d)
{
	unsigned int nid = lru_nid();
	struct dc_lru_one *l = &dc->s_dentry_lru[nid];

	lru_lock(l);
	d->d_lru.prev = l->tail;
	d->d_lru.next = NULL;
	if (l->tail)
		l->tail->d_lru.next = d;
	else
		l->head = d;
	l->tail = d;
	l->nr_items++;
	d->d_lru.shard = nid + 1;
	lru_unlock(l);
}

static void lru_del_locked(struct dc_lru_one *l, struct dentry *d)
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
	d->d_lru.shard = 0;
	l->nr_items--;
}

/* list_lru_del: IMMEDIATE physical removal, from anywhere in the list. */
static void lru_del(struct dcache *dc, struct dentry *d)
{
	unsigned int idx = uatomic_load(&d->d_lru.shard, CMM_RELAXED);
	struct dc_lru_one *l;

	if (!idx)
		return;
	l = &dc->s_dentry_lru[idx - 1];
	lru_lock(l);
	if (d->d_lru.shard)
		lru_del_locked(l, d);
	lru_unlock(l);
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
	if (caa_likely(uatomic_load(&d->d_lru.shard, CMM_RELAXED))) {
		if (!uatomic_load(&d->d_lru.referenced, CMM_RELAXED))
			uatomic_store(&d->d_lru.referenced, 1, CMM_RELAXED);
		return;
	}
	lru_add(dc, d);				/* re-arm after an LRU_REMOVED */
}

unsigned long dc_lru_count(struct dcache *dc)
{
	unsigned long n = 0;
	unsigned int i;

	for (i = 0; i < DC_LRU_NODES; i++)
		n += uatomic_load(&dc->s_dentry_lru[i].nr_items, CMM_RELAXED);
	return n;
}

const char *dc_lru_arm(void) { return "pernode"; }

#else	/* DC_NO_LRU */
static inline void lru_add(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline void lru_del(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
static inline void lru_retain(struct dcache *dc, struct dentry *d)
{ (void) dc; (void) d; }
unsigned long dc_lru_count(struct dcache *dc) { (void) dc; return 0; }
long dc_shrink(struct dcache *dc, long nr) { (void) dc; (void) nr; return 0; }
long dc_shrink_local(struct dcache *dc, long nr) { (void) dc; (void) nr; return 0; }
const char *dc_lru_arm(void) { return "none"; }
#endif	/* DC_NO_LRU */

static void children_add(struct dentry *parent, struct dentry *child)
{
	child->d_sib = parent->d_children;
	parent->d_children = child;
}

static void children_remove(struct dentry *parent, struct dentry *child)
{
	struct dentry **pp = &parent->d_children;

	while (*pp && *pp != child)
		pp = &(*pp)->d_sib;
	if (*pp == child)
		*pp = child->d_sib;
	child->d_sib = NULL;
}

/*
 * Per-directory rwsem.  readdir read-locks the dir it lists; every mutator that
 * changes a dir's child listing -- add/remove a child, or rename a child in
 * place (name change under the same parent) -- write-locks that dir.  Concurrent
 * readdirs of a dir thus share, and a readdir of one dir never serializes against
 * a rename of another: the honest per-directory-inode-rwsem analogue, not one
 * global lock.  Writer-vs-writer cannot deadlock because the two-dir case is
 * address-ordered (dirs_wlock2) and multi-dir mutators are serialized by
 * rename_lock, so at most one holds two dir locks at a time; readdir takes only a
 * read lock, so there is no cycle either.  Ordering, outermost first:
 * rename_lock -> these dir rwsems -> bucket bit locks.
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
	return "seqlock";
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
	d->d_isdir = (unsigned char) (isdir != 0);
	dir_lock_init(&d->d_lock);
	return d;
}

struct dcache *dc_create(unsigned int nbuckets)
{
	struct dcache *dc = calloc(1, sizeof(*dc));
	unsigned int n = 1;
	struct qstr rootname;

	if (!dc)
		return NULL;
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
	rcu_unregister_thread();
}

void dc_quiescent(void)
{
	rcu_quiescent_state();
}

/* ---- lockless lookup (the RCU-walk fast path) --------------------------- */

/*
 * __d_lookup_rcu: find parent's child named `name` in the hash, lockless.  Fills
 * *seqp with the dentry's d_seq sampled BEFORE the parent/name compare; the
 * caller re-validates it (read_seqcount_retry) before trusting the dentry to
 * step to the next component.  A mismatch just `continue`s: if the miss is due to
 * an in-flight rename, the outer rename_lock check forces a full retry.
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
		seq = raw_read_seqcount(&d->d_seq);
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

enum dc_result dc_lookup(struct dcache *dc, const struct dc_path *p,
			 uint64_t *out_id)
{
	unsigned long m_seq;
	unsigned long retries = 0;

retry:
	rcu_read_lock();
	m_seq = read_seqbegin(&dc->rename_lock);	/* global walk anchor */
	{
		struct dentry *cur = dc->root;
		enum dc_result res = DC_POSITIVE;
		uint64_t id = DC_FAST_ID(cur);
		uint32_t i;

		for (i = 0; i < p->ndepth; i++) {
			unsigned long seq;
			struct dentry *d = __d_lookup_rcu(dc, cur, &p->comp[i],
							  &seq);

			if (!d) {
				res = DC_ABSENT;
				break;
			}
			/* Validate this component before stepping into it. */
			if (read_seqcount_retry(&d->d_seq, seq)) {
				rcu_read_unlock();
				goto retry_check;
			}
			cur = d;
			id = DC_FAST_ID(d);
			res = DC_IS_POSITIVE(d) ? DC_POSITIVE : DC_NEGATIVE;
		}

		/* Global anchor: any rename during the walk => redo it. */
		if (read_seqretry(&dc->rename_lock, m_seq)) {
			rcu_read_unlock();
			goto retry_check;
		}
		rcu_read_unlock();
		if (res == DC_POSITIVE && out_id)
			*out_id = id;
		return res;
	}

retry_check:
	/*
	 * Bounded-retry guard: normal contention resolves in a handful of
	 * spins.  A runaway count signals a livelock/bug, not a hot workload;
	 * we keep retrying but make it loud in debug builds.
	 */
	if (++retries == (1UL << 24))
		__asm__ __volatile__("" ::: "memory");	/* placeholder tap */
	/*
	 * Walk-retry accounting -- the mechanism the benchmark charts against
	 * the txn engine (which never retries a walk).  This is the SLOW path:
	 * it fires only when a concurrent rename bumped the global rename_lock
	 * or the stepped-into dentry's d_seq, so at rename-fraction 0 it costs
	 * nothing.  A relaxed add is enough (a diagnostic total, not ordering).
	 */
	__atomic_fetch_add(&dc_seq_walk_retries, 1, __ATOMIC_RELAXED);
	rcu_quiescent_state();
	goto retry;
}

/*
 * Lock-free resolve of `path`'s first `depth` components to its dentry.  The
 * caller holds rcu_read_lock, so the returned dentry cannot be freed under it.
 * Mirrors dc_lookup's d_seq + rename_lock discipline but returns the dentry
 * rather than an id, and retries internally (bounded) on a racing rename -- so a
 * concurrent rename of ANOTHER node in a chain the walk crosses forces a re-walk
 * instead of a spurious miss.  Used by readdir and by every writer to locate its
 * target(s); NULL if genuinely absent.
 */
static struct dentry *resolve_dentry_rcu(struct dcache *dc,
					 const struct dc_path *p, uint32_t depth)
{
	unsigned long retries = 0;

	for (;;) {
		unsigned long m_seq = read_seqbegin(&dc->rename_lock);
		struct dentry *cur = dc->root;
		uint32_t i;
		int ok = 1;

		for (i = 0; i < depth; i++) {
			unsigned long seq;
			struct dentry *d = __d_lookup_rcu(dc, cur, &p->comp[i],
							  &seq);

			if (!d) {
				if (!read_seqretry(&dc->rename_lock, m_seq))
					return NULL;	/* genuine miss */
				ok = 0;			/* racing miss -> retry */
				break;
			}
			if (read_seqcount_retry(&d->d_seq, seq)) {
				ok = 0;			/* identity moved -> retry */
				break;
			}
			cur = d;
			/* retain_dentry: this is the WRITER-side walk (dc_lookup
			 * does not come through here), so it stands in for the
			 * ref-walk's dget/dput of each component.  __d_lookup_rcu
			 * takes no reference and so marks nothing -- which is
			 * exactly why the reader costs the LRU nothing. */
			lru_retain(dc, cur);
		}
		if (ok && !read_seqretry(&dc->rename_lock, m_seq))
			return cur;			/* clean walk */
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
	 */
	if (!DC_IS_POSITIVE(parent)) {
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
	/* A brand-new node has no readers yet: hlist_add_head_rcu is its one
	 * publish (release).  No rename_lock bump -- add doesn't move anything. */
	hlist_add_head_rcu(b, &d->d_hash);
	children_add(parent, d);
	lru_add(dc, d);			/* d_lru_add: the first last-put, at the tail */
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
	int ret = 0;

	if (path->ndepth == 0)
		return -EINVAL;			/* cannot unlink the root */

	rcu_read_lock();
	victim = resolve_dentry_rcu(dc, path, path->ndepth);
	if (!victim) {
		rcu_read_unlock();
		return -ENOENT;
	}
	parent = DC_DPARENT(victim);
	b = bucket_of(dc, parent, victim->d_name.hash);
	dir_wlock(parent);
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
	dir_wunlock(parent);
	rcu_read_unlock();
	return ret;
}

/*
 * __d_move: relocate `victim` so its parent becomes `new_parent` and its name
 * becomes `new_name`.  Runs inside write_seqlock(&rename_lock) so the whole move
 * is one even->odd->even transition to lockless walkers, and bumps the victim's
 * own d_seq so a walker mid-compare on it retries.
 */
static int __d_move(struct dcache *dc, struct dentry *victim,
		     struct dentry *new_parent, const struct qstr *new_name)
{
	struct dentry *old_parent = DC_DPARENT(victim);
	struct dc_bucket *ob = bucket_of(dc, old_parent, victim->d_name.hash);
	struct dc_bucket *nb = bucket_of(dc, new_parent, new_name->hash);

	/*
	 * Write-lock both affected dirs (one if unchanged) for the whole identity
	 * change: excludes concurrent readdir of the old dir (child leaving), the
	 * new dir (child arriving), AND the same-dir case where only the child's
	 * name changes in place -- a readdir of that dir must not see a torn name.
	 * Then both hash buckets (the old chain it leaves, the new it enters) so
	 * the del + add is atomic against a concurrent add/unlink on either chain.
	 */
	dirs_wlock2(old_parent, new_parent);
	/*
	 * A NEGATIVE destination directory must not gain a child, and a rename
	 * INTO it is the SECOND way that can happen -- dc_add is the first, and
	 * guarding only dc_add left this hole.  Checked under new_parent's OWN
	 * dir lock, which is the lock dc_delete holds while it verifies
	 * d_children and flips the state; that is what makes the two atomic.
	 * Only cross-parent can hit it: a negative directory has no children, so
	 * a same-dir rename under one has nothing to rename.
	 */
	if (old_parent != new_parent && !DC_IS_POSITIVE(new_parent)) {
		dirs_wunlock2(old_parent, new_parent);
		return -ENOENT;
	}
	bl_lock2(ob, nb);
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
	dirs_wunlock2(old_parent, new_parent);
	return 0;
}

int dc_rename(struct dcache *dc, const struct dc_path *from,
	      const struct dc_path *to)
{
	struct dentry *victim, *to_parent;
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
	cross = (DC_DPARENT(victim) != to_parent);
	if (cross)
		pthread_mutex_lock(&dc->vfs_rename_mutex);
	to_name = &to->comp[to->ndepth - 1];
	if (!DC_IS_POSITIVE(to_parent)) {
		/* advisory; __d_move re-checks under to_parent's dir lock */
		ret = -ENOENT;
		goto out_unlock;
	}
	if (__child_lookup(dc, to_parent, to_name)) {
		ret = -EEXIST;			/* phase 1: no replace */
		goto out_unlock;
	}
	if (cross && is_subdir(to_parent, victim)) {
		ret = -EINVAL;			/* would create a loop */
		goto out_unlock;
	}
	/*
	 * __d_move takes the dir rwsems and both bucket locks; rename_lock seals
	 * the whole move for the reader seqbracket and serializes it against other
	 * renames.  For a same-dir rename the EXISTS check is not under the target
	 * bucket lock, so it races a concurrent add of the same (to_parent, to_name)
	 * -- which the benchmark's disjoint-slot ownership precludes (a writer owns
	 * its target slot), the same ownership resolve's returned nodes rely on.
	 */
	write_seqlock(&dc->rename_lock);
	ret = __d_move(dc, victim, to_parent, to_name);
	write_sequnlock(&dc->rename_lock);
out_unlock:
	if (cross)
		pthread_mutex_unlock(&dc->vfs_rename_mutex);
out:
	rcu_read_unlock();
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
	write_seqlock(&dc->rename_lock);
	dirs_wlock2(pa, pb);
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
	rcu_assign_pointer(da->d_parent, pb);
	rcu_assign_pointer(db->d_parent, pa);
	hlist_add_head_rcu(bb, &da->d_hash);		/* da enters pb's bucket */
	hlist_add_head_rcu(ba, &db->d_hash);		/* db enters pa's bucket */
	if (pa != pb) {
		children_add(pb, da);
		children_add(pa, db);
	}
	write_seqcount_end(&db->d_seq);
	write_seqcount_end(&da->d_seq);
	bl_unlock2(ba, bb);
	dirs_wunlock2(pa, pb);
	write_sequnlock(&dc->rename_lock);
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
		call_rcu(&d->d_rcu, dentry_free_cb);
		ret = 0;
	}
	bl_unlock(b);
	dir_wunlock(parent);
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
				if (d->d_lru.referenced) {
					d->d_lru.referenced = 0;
					lru_del_locked(l, d);	/* LRU_ROTATE */
					d->d_lru.prev = l->tail;
					if (l->tail)
						l->tail->d_lru.next = d;
					else
						l->head = d;
					l->tail = d;
					l->nr_items++;
					d->d_lru.shard = i + 1;
					continue;
				}
				if (d->d_children) {
					lru_del_locked(l, d);	/* LRU_REMOVED */
					continue;
				}
				lru_del_locked(l, d);
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
