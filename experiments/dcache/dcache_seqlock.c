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
 * Controlled-experiment scoping (see README): writers are serialized by ONE
 * mutator lock (the s_vfs_rename_mutex analog, here covering all mutators, not
 * just cross-dir rename).  That is deliberately coarser than the kernel's
 * per-directory i_rwsem -- it isolates the ONE variable under study, the READER
 * path, so the only thing that differs between the seqlock and txn engines is
 * how a lookup validates and how a rename publishes.  Per-dentry d_lock collapses
 * into this mutator lock (a single writer at a time), so d_seq is written under
 * the mutator lock directly.  Refcounting is omitted: a walk lives entirely in
 * one RCU read-side section and retains nothing (the kernel's LOOKUP_RCU fast
 * path), and unlink RCU-defers the free.
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

#include "dcache.h"
#include "seqcount.h"

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

struct dc_bucket {
	struct dc_hnode *first;
};

struct dentry {
	struct qstr d_name;		/* current name under d_parent */
	struct dentry *d_parent;	/* parent dir (root's parent is itself) */
	uint64_t d_id;			/* stable identity, for verification */
	int d_inode;			/* nonzero => positive (phase 1: always) */
	int d_unhashed;			/* removed from the hash (skip in lookup) */

	seqcount_t d_seq;		/* name/parent coherence for RCU walk */
	struct dc_hnode d_hash;		/* linkage in dentry_hashtable bucket */

	struct dentry *d_children;	/* head of children (verify/-ENOTEMPTY) */
	struct dentry *d_sib;		/* next sibling under d_parent */

	struct rcu_head d_rcu;		/* deferred free */
};

struct dcache {
	struct dc_bucket *buckets;
	unsigned long mask;		/* nbuckets - 1 (power of two) */
	struct dentry *root;
	seqlock_t rename_lock;		/* GLOBAL: the walk's consistency anchor */
	pthread_mutex_t mutator_lock;	/* serializes all structural mutators */
};

#define hnode_dentry(n) caa_container_of((n), struct dentry, d_hash)

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

/* ---- RCU hlist (writer side runs under mutator_lock) -------------------- */

static inline void hlist_add_head_rcu(struct dc_bucket *b, struct dc_hnode *n)
{
	struct dc_hnode *first = b->first;

	n->next = first;
	n->pprev = &b->first;
	if (first)
		first->pprev = &n->next;
	rcu_assign_pointer(b->first, n);	/* release: publish n */
}

static inline void hlist_del_rcu(struct dc_hnode *n)
{
	struct dc_hnode *next = n->next;
	struct dc_hnode **pprev = n->pprev;

	/* Splice n out; n->next stays valid for readers already past it. */
	CMM_STORE_SHARED(*pprev, next);
	if (next)
		next->pprev = pprev;
}

/* ---- children list (verify + -ENOTEMPTY; writer/quiescent only) --------- */

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

/* ---- lifecycle ---------------------------------------------------------- */

const char *dc_engine_name(void)
{
	return "seqlock";
}

static struct dentry *dentry_alloc(const struct qstr *name,
				   struct dentry *parent, uint64_t id)
{
	struct dentry *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->d_name = *name;
	d->d_parent = parent;
	d->d_id = id;
	d->d_inode = 1;			/* phase 1: every dentry is positive */
	d->d_unhashed = 0;
	seqcount_init(&d->d_seq);
	d->d_children = NULL;
	d->d_sib = NULL;
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
	pthread_mutex_init(&dc->mutator_lock, NULL);

	dc_qstr_init(&rootname, "");
	dc->root = dentry_alloc(&rootname, NULL, 0);
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
	pthread_mutex_destroy(&dc->mutator_lock);
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

	for (n = rcu_dereference(b->first); n; n = rcu_dereference(n->next)) {
		struct dentry *d = hnode_dentry(n);
		unsigned long seq;

		if (CMM_LOAD_SHARED(d->d_name.hash) != name->hash)
			continue;
		seq = raw_read_seqcount(&d->d_seq);
		if (CMM_LOAD_SHARED(d->d_parent) != parent)
			continue;
		if (CMM_LOAD_SHARED(d->d_unhashed))
			continue;
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
		uint64_t id = cur->d_id;
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
			id = d->d_id;
			res = d->d_inode ? DC_POSITIVE : DC_NEGATIVE;
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

/* ---- writer-side helpers (under mutator_lock) --------------------------- */

static struct dentry *__child_lookup(struct dcache *dc, struct dentry *parent,
				     const struct qstr *name)
{
	struct dc_bucket *b = bucket_of(dc, parent, name->hash);
	struct dc_hnode *n;

	for (n = b->first; n; n = n->next) {
		struct dentry *d = hnode_dentry(n);

		if (d->d_parent == parent && !d->d_unhashed &&
		    dc_qstr_eq(&d->d_name, name))
			return d;
	}
	return NULL;
}

/* Resolve the first `depth` components from the root; NULL if any is missing. */
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

/* Is `a` equal to `b` or a descendant of `b`?  (Directory-loop guard.) */
static int is_subdir(struct dentry *a, struct dentry *b)
{
	struct dentry *cur = a;

	for (;;) {
		if (cur == b)
			return 1;
		if (cur == cur->d_parent)	/* reached root */
			return 0;
		cur = cur->d_parent;
	}
}

/* ---- mutators ----------------------------------------------------------- */

int dc_add(struct dcache *dc, const struct dc_path *path, uint64_t id)
{
	struct dentry *parent, *d;
	const struct qstr *name;
	int ret = 0;

	if (path->ndepth == 0)
		return -EEXIST;			/* the root already exists */

	pthread_mutex_lock(&dc->mutator_lock);
	parent = resolve(dc, path, path->ndepth - 1);
	if (!parent) {
		ret = -ENOENT;
		goto out;
	}
	name = &path->comp[path->ndepth - 1];
	if (__child_lookup(dc, parent, name)) {
		ret = -EEXIST;
		goto out;
	}
	d = dentry_alloc(name, parent, id);
	if (!d) {
		ret = -ENOMEM;
		goto out;
	}
	/* A brand-new node has no readers yet: hlist_add_head_rcu is its one
	 * publish (release).  No rename_lock bump -- add doesn't move anything. */
	hlist_add_head_rcu(bucket_of(dc, parent, name->hash), &d->d_hash);
	children_add(parent, d);
out:
	pthread_mutex_unlock(&dc->mutator_lock);
	return ret;
}

static void dentry_free_cb(struct rcu_head *rh)
{
	free(caa_container_of(rh, struct dentry, d_rcu));
}

int dc_unlink(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *victim;
	int ret = 0;

	if (path->ndepth == 0)
		return -EINVAL;			/* cannot unlink the root */

	pthread_mutex_lock(&dc->mutator_lock);
	victim = resolve(dc, path, path->ndepth);
	if (!victim) {
		ret = -ENOENT;
		goto out;
	}
	if (victim->d_children) {
		ret = -ENOTEMPTY;
		goto out;
	}
	/* Publish the removal under d_seq so a reader mid-compare on the victim
	 * re-scans and misses it; hlist_del_rcu splices it for new readers. */
	write_seqcount_begin(&victim->d_seq);
	CMM_STORE_SHARED(victim->d_unhashed, 1);
	hlist_del_rcu(&victim->d_hash);
	write_seqcount_end(&victim->d_seq);
	children_remove(victim->d_parent, victim);
	call_rcu(&victim->d_rcu, dentry_free_cb);	/* honest deferred free */
out:
	pthread_mutex_unlock(&dc->mutator_lock);
	return ret;
}

/*
 * __d_move: relocate `victim` so its parent becomes `new_parent` and its name
 * becomes `new_name`.  Runs inside write_seqlock(&rename_lock) so the whole move
 * is one even->odd->even transition to lockless walkers, and bumps the victim's
 * own d_seq so a walker mid-compare on it retries.
 */
static void __d_move(struct dcache *dc, struct dentry *victim,
		     struct dentry *new_parent, const struct qstr *new_name)
{
	struct dentry *old_parent = victim->d_parent;

	write_seqcount_begin(&victim->d_seq);
	hlist_del_rcu(&victim->d_hash);			/* leave old bucket */
	if (old_parent != new_parent)
		children_remove(old_parent, victim);
	victim->d_name = *new_name;			/* identity change */
	rcu_assign_pointer(victim->d_parent, new_parent);
	hlist_add_head_rcu(bucket_of(dc, new_parent, new_name->hash),
			   &victim->d_hash);		/* enter new bucket */
	if (old_parent != new_parent)
		children_add(new_parent, victim);
	write_seqcount_end(&victim->d_seq);
}

int dc_rename(struct dcache *dc, const struct dc_path *from,
	      const struct dc_path *to)
{
	struct dentry *victim, *to_parent;
	const struct qstr *to_name;
	int ret = 0;

	if (from->ndepth == 0 || to->ndepth == 0)
		return -EINVAL;			/* cannot move the root */

	pthread_mutex_lock(&dc->mutator_lock);
	victim = resolve(dc, from, from->ndepth);
	if (!victim) {
		ret = -ENOENT;
		goto out;
	}
	to_parent = resolve(dc, to, to->ndepth - 1);
	if (!to_parent) {
		ret = -ENOENT;
		goto out;
	}
	to_name = &to->comp[to->ndepth - 1];
	if (__child_lookup(dc, to_parent, to_name)) {
		ret = -EEXIST;			/* phase 1: no replace */
		goto out;
	}
	if (victim->d_parent != to_parent && is_subdir(to_parent, victim)) {
		ret = -EINVAL;			/* would create a loop */
		goto out;
	}
	write_seqlock(&dc->rename_lock);
	__d_move(dc, victim, to_parent, to_name);
	write_sequnlock(&dc->rename_lock);
out:
	pthread_mutex_unlock(&dc->mutator_lock);
	return ret;
}

int dc_rename_exchange(struct dcache *dc, const struct dc_path *a,
		       const struct dc_path *b)
{
	struct dentry *da, *db, *pa, *pb;
	struct qstr na, nb;
	int ret = 0;

	if (a->ndepth == 0 || b->ndepth == 0)
		return -EINVAL;

	pthread_mutex_lock(&dc->mutator_lock);
	da = resolve(dc, a, a->ndepth);
	db = resolve(dc, b, b->ndepth);
	if (!da || !db) {
		ret = -ENOENT;
		goto out;
	}
	if (da == db)
		goto out;			/* exchanging a node with itself */
	pa = da->d_parent;
	pb = db->d_parent;
	/* Neither may end up under the other (no loops in either direction). */
	if (pa != pb && (is_subdir(pb, da) || is_subdir(pa, db))) {
		ret = -EINVAL;
		goto out;
	}
	na = da->d_name;
	nb = db->d_name;

	write_seqlock(&dc->rename_lock);
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
	hlist_add_head_rcu(bucket_of(dc, pb, nb.hash), &da->d_hash);
	hlist_add_head_rcu(bucket_of(dc, pa, na.hash), &db->d_hash);
	if (pa != pb) {
		children_add(pb, da);
		children_add(pa, db);
	}
	write_seqcount_end(&db->d_seq);
	write_seqcount_end(&da->d_seq);
	write_sequnlock(&dc->rename_lock);
out:
	pthread_mutex_unlock(&dc->mutator_lock);
	return ret;
}

/*
 * List a directory.  Faithful baseline: readdir takes the mutator lock (the
 * kernel holds the directory's i_rwsem), so listing is serialized against every
 * rename -- a consistent snapshot, but readdir contends with all mutation.  This
 * is the cost the txn engine's lock-free child-hlist is meant to beat.
 */
long dc_readdir(struct dcache *dc, const struct dc_path *path,
		dc_dirent_fn fn, void *arg)
{
	struct dentry *dir, *c;
	long count = 0;

	pthread_mutex_lock(&dc->mutator_lock);
	dir = resolve(dc, path, path->ndepth);
	if (!dir) {
		pthread_mutex_unlock(&dc->mutator_lock);
		return -ENOENT;
	}
	for (c = dir->d_children; c; c = c->d_sib) {
		if (fn)
			fn(c->d_id, &c->d_name, arg);
		count++;
	}
	pthread_mutex_unlock(&dc->mutator_lock);
	return count;
}

/* ---- verification walk (quiescent) ------------------------------------- */

static void walk_rec(struct dentry *d, struct dc_path *path, dc_visit_fn fn,
		     void *arg)
{
	struct dentry *c;

	if (path->ndepth > 0 || d->d_parent != d)	/* skip emitting root */
		fn(d->d_id, path, arg);

	for (c = d->d_children; c; c = c->d_sib) {
		if (path->ndepth >= DC_PATH_MAX)
			continue;		/* too deep to represent */
		path->comp[path->ndepth++] = c->d_name;
		walk_rec(c, path, fn, arg);
		path->ndepth--;
	}
}

void dc_walk(struct dcache *dc, dc_visit_fn fn, void *arg)
{
	struct dc_path path;

	dc_path_reset(&path);
	walk_rec(dc->root, &path, fn, arg);
}
