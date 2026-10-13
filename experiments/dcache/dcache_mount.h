// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_mount.h -- a count of mountpoints, in place of the search of a walk.
 *
 * Included twice by dcache_bucketlock.c under -DDC_MOUNT_COUNT: before the
 * rename for what the rename uses, and at the end, with DCACHE_MOUNT_OPS, for
 * the operations.  The kernel asks "is there a mountpoint below this dentry"
 * by walking the subtree (d_invalidate(), path_has_submounts()), and has to
 * watch rename_lock for it, because a rename can take a subtree from a
 * directory the walk has yet to read into one it has left.  Here the answer is
 * kept instead of searched for.
 *
 * THE COUNT.  A dentry with a mountpoint at or below it has a struct dc_mnt,
 * found by the dentry in dc->mnt_bkt[] as the kernel finds a struct mountpoint
 * in mountpoint_hashtable, which holds how many.  Nothing in the dentry but two
 * flags: DC_MNT_HERE, "this one is a mountpoint", and DC_MNT_COUNTED, "it has a
 * struct dc_mnt".  A dentry without the second has none below it, and that is
 * all that an operation on it reads.
 *
 * WHY THIS IS EXACT WITH NOTHING TO DETECT.  A rename that keeps a subtree
 * below a dentry does not change that dentry's count.  The counts that change
 * are those of the ancestors between the two parents and the first ancestor
 * they share, and they change in the transaction that moves the subtree: the
 * count is a transacted slot, read through bl_read() like d_parent.  A reader
 * of one count sees it before the move or after, never a move without it.
 *
 * LOCKS: ONE FOR EACH BUCKET OF THE TABLE, AND NO OTHER.  A bucket's lock is
 * held to change its chain and to write the count of a struct dc_mnt in it,
 * which makes each count slot single-writer, as the engine asks.  An operation
 * that changes counts -- a mount, an unmount, a rename or an exchange that
 * takes a count to another directory -- holds the buckets of the counts it
 * changes and of what it moves, taken in the order of their addresses, for its
 * commit and no longer.  Two renames whose ancestors do not meet below the
 * ones they share take different buckets.  A rename with nothing mounted below
 * takes none: it reads DC_MNT_COUNTED of the host it moves, under the child
 * head of the directory it leaves, which it holds, and goes on as it always
 * did.
 *
 * HOW AN OPERATION GETS ITS COUNTS RIGHT.  Three steps, because which counts
 * change depends on who the ancestors are, and they move.
 *
 *   1. It works out its set with no lock held: what it moves, the ancestors of
 *      the old parent, the ancestors of the new one up to the first that both
 *      have.  Each struct dc_mnt is PINNED as it goes, made with a count of
 *      zero where there is none; a pinned one is not unhashed.  And each new
 *      ancestor is FROZEN (mnt_freeze()): DC_MNT_COUNTED is set on it under
 *      the child head of its parent, the lock that a move of it holds.  From
 *      then on no rename moves that ancestor without holding its bucket.
 *   2. Under the locks of its own commit it takes the buckets of the set and
 *      looks again (mnt_shift_lock()): is each still the child of the one
 *      noted above it, and is the count of what moves the one it read.  If
 *      not, something moved meanwhile: it lets go and starts over.
 *   3. It adds the changes to its transaction, commits, lets go, unpins.
 *
 * Step 1's flag is what makes step 2 enough.  A rename of an ancestor that
 * read no flag had the child head before the flag was set, so it has committed
 * by the time the flag is: step 2 sees where the ancestor is now.  One that
 * comes later sees the flag, and has to take the bucket, which step 2 holds.
 *
 * WHAT IS LEFT BEHIND: nothing.  A struct dc_mnt whose count is zero and that
 * no operation has pinned is unhashed, and its flag cleared, by the one that
 * unpins it last.
 *
 * THE READERS, under RCU alone.  dc_mount_count(): the flag, and if it is set
 * the struct dc_mnt and its slot; one pass.  The flag is set before the commit
 * that makes a count more than zero and cleared after the one that makes it
 * zero, and the struct dc_mnt is hashed before the flag is set and unhashed
 * after it is cleared: a clear flag, and a missing struct dc_mnt, both mean
 * that the count was zero at an instant during the call.  dc_mount_find(): down
 * through the children that have a count to a mountpoint, which it then checks
 * to be below where it started, since the way down may have moved under it;
 * it goes again if not.
 *
 * In the kernel the two flags are bits of d_flags, the lock that the freeze
 * and the rename share is the d_lock of the dentry that moves (a move holds
 * it), and the struct dc_mnt of a mountpoint is its struct mountpoint.  Here
 * the lock is the child head of the parent, which is what this engine has.
 */
#ifndef DCACHE_MOUNT_OPS

/*
 * The statistics, dc_mount_stats(), are counters that every operation with a
 * count writes: one line for all of them.  -DDC_MNT_NO_STATS leaves them out,
 * for a build that is timed.
 */
#ifdef DC_MNT_NO_STATS
#define DC_MNT_STAT_INC(dc, f)	((void) (dc))
#define DC_MNT_STAT_DEC(dc, f)	((void) (dc))
#else
#define DC_MNT_STAT_INC(dc, f)	uatomic_inc(&(dc)->f)
#define DC_MNT_STAT_DEC(dc, f)	uatomic_dec(&(dc)->f)
#endif

#define DC_MNT_HERE	1UL	/* this object is a mountpoint */
#define DC_MNT_COUNTED	2UL	/* it has a struct dc_mnt */

/* Engine proxy tag of the count slot (bit 0; the count is kept shifted). */
#define DC_MNT_TAG	URCU_TXN_TAG

struct dc_mnt {
	struct cds_hlist_node node;	/* its bucket's chain; the bucket's lock */
	struct dentry *host;
	void *count;			/* TRANSACTED (DC_MNT_TAG); the bucket's lock */
	unsigned long pins;		/* operations that have it in their set */
	int dead;			/* unhashed */
	long shadow;			/* dc_mount_check() only */
	struct rcu_head rcu;
};

static inline void *mnt_ptr(long n)
{
	return (void *) ((uintptr_t) n << 1);
}

/* The count in @m.  Under RCU; stable for whoever holds its bucket. */
static inline long mnt_read(struct dc_mnt *m)
{
	return (long) ((uintptr_t) bl_read(&m->count, DC_MNT_TAG) >> 1);
}

static inline int mnt_counted(const struct dentry *host)
{
	return (uatomic_load(&host->d_mnt, CMM_RELAXED) & DC_MNT_COUNTED) != 0;
}

static inline int mnt_here(const struct dentry *host)
{
	return (uatomic_load(&host->d_mnt, CMM_RELAXED) & DC_MNT_HERE) != 0;
}

/*
 * What a move to another directory asks of the host it moves, under the child
 * head of the directory it leaves.
 */
static inline int mnt_move_counted(const struct dentry *host)
{
#ifdef DC_TEST_MNT_NO_FLAG_CHECK
	(void) host;
	return 0;
#else
	return mnt_counted(host);
#endif
}

static inline struct dc_mnt_bkt *mnt_bkt(struct dcache *dc,
					 const struct dentry *host)
{
	unsigned long h = (unsigned long) (uintptr_t) host >> 6;

	h *= 0x9e3779b97f4a7c15UL;
	return &dc->mnt_bkt[h >> (64 - DC_MNT_HASH_BITS)];
}

static inline void mnt_bkt_lock(struct dc_mnt_bkt *b)
{
	while (uatomic_cmpxchg(&b->lock, 0UL, 1UL) != 0UL)
		caa_cpu_relax();
}

static inline void mnt_bkt_unlock(struct dc_mnt_bkt *b)
{
	uatomic_store(&b->lock, 0UL, CMM_RELEASE);
}

static struct dc_mnt *mnt_find_rcu(struct dcache *dc, const struct dentry *host)
{
	struct dc_mnt *m;

	cds_hlist_for_each_entry_rcu_2(m, &mnt_bkt(dc, host)->head, node)
		if (m->host == host)
			return m;
	return NULL;
}

/* The struct dc_mnt of @host, pinned; NULL if it has none. */
static struct dc_mnt *mnt_pin(struct dcache *dc, struct dentry *host)
{
	struct dc_mnt_bkt *b = mnt_bkt(dc, host);
	struct dc_mnt *m;

	mnt_bkt_lock(b);
	m = mnt_find_rcu(dc, host);
	if (m)
		m->pins++;
	mnt_bkt_unlock(b);
	return m;
}

/* The same, made with a count of zero if @host has none; NULL: no memory. */
static struct dc_mnt *mnt_get(struct dcache *dc, struct dentry *host)
{
	struct dc_mnt_bkt *b = mnt_bkt(dc, host);
	struct dc_mnt *m, *fresh = NULL;

	for (;;) {
		mnt_bkt_lock(b);
		m = mnt_find_rcu(dc, host);
		if (!m && fresh) {
			cds_hlist_add_head_rcu(&fresh->node, &b->head);
			m = fresh;
			fresh = NULL;
			DC_MNT_STAT_INC(dc, mnt_objs);
		} else if (m) {
			m->pins++;
		}
		mnt_bkt_unlock(b);
		if (m) {
			free(fresh);
			return m;
		}
		fresh = calloc(1, sizeof(*fresh));
		if (!fresh)
			return NULL;
		fresh->host = host;
		fresh->count = mnt_ptr(0);
		fresh->pins = 1;
	}
}

static void mnt_free_cb(struct rcu_head *rh)
{
	free(caa_container_of(rh, struct dc_mnt, rcu));
}

/*
 * Let go of @m.  The last to do so of one that counts nothing takes it away:
 * the flag first, then the chain.
 */
static void mnt_unpin_locked(struct dcache *dc, struct dc_mnt *m)
{
	int last = --m->pins == 0;

#ifdef DC_TEST_MNT_NO_PIN
	last = 1;		/* whoever else has it in a set loses it */
#endif
	if (last && !m->dead && mnt_read(m) == 0) {
		uatomic_and(&m->host->d_mnt, ~DC_MNT_COUNTED);
		cds_hlist_del_rcu(&m->node);
		m->dead = 1;
		DC_MNT_STAT_DEC(dc, mnt_objs);
		call_rcu(&m->rcu, mnt_free_cb);
	}
}

static void mnt_unpin(struct dcache *dc, struct dc_mnt *m)
{
	struct dc_mnt_bkt *b = mnt_bkt(dc, m->host);

	mnt_bkt_lock(b);
	mnt_unpin_locked(dc, m);
	mnt_bkt_unlock(b);
}

/*
 * A removal found a directory that an operation has frozen and is about to
 * land in: it looks again, until that operation is over.
 */
static void mnt_wait(struct dcache *dc)
{
	DC_MNT_STAT_INC(dc, mnt_waits);
	caa_cpu_relax();
}

/*
 * Freeze @a where it is, and say where that is: set DC_MNT_COUNTED on it under
 * the child head of its parent, and return the parent read under that lock.
 * The caller has pinned @a's struct dc_mnt, so the flag, once set, stays.
 * @first: @a is the first of a climb, with nothing below it that the climb
 * has frozen, so it may be gone: NULL then.  One above the first has a frozen
 * child, and a directory that is not empty stays.
 */
static struct dentry *mnt_freeze(struct dcache *dc, struct dentry *a, int first)
{
	struct dentry *p __attribute__((unused));

	if (a == dc->root) {			/* does not move */
		uatomic_or(&a->d_mnt, DC_MNT_COUNTED);
		return a;
	}
	if (mnt_counted(a))			/* frozen already, and pinned */
		return parent_of_rcu(a);
#ifdef DC_TEST_MNT_FREEZE_NO_LOCK
	(void) first;
	p = parent_of_rcu(a);
	uatomic_or(&a->d_mnt, DC_MNT_COUNTED);
	return p;
#else
	for (;;) {
		p = parent_of_rcu(a);
		bl_lock(&p->d_child_head);
		if (parent_of_rcu(a) != p) {	/* it moved: look again */
			bl_unlock(&p->d_child_head);
			continue;
		}
		if (first && !entry_alive(a)) {
			bl_unlock(&p->d_child_head);
			return NULL;
		}
		uatomic_or(&a->d_mnt, DC_MNT_COUNTED);
		bl_unlock(&p->d_child_head);
		DC_MNT_STAT_INC(dc, mnt_frozen);
		return p;
	}
#endif
}

/*
 * One operation's set: the struct dc_mnt it has pinned, by how much the count
 * of each changes, and who was above each dentry when the set was worked out.
 * An entry with @by zero and @up NULL is above everything that changes; it is
 * neither locked nor looked at again.
 */
struct mnt_ent {
	struct dentry *host;
	struct dentry *up;		/* its parent then; NULL: not to be checked */
	struct dc_mnt *m;
	long by;
	int self;			/* what the operation moves, or mounts */
};

struct mnt_shift {
	struct mnt_ent *e;
	int n, cap;
	struct dc_mnt_bkt **b;		/* the buckets to hold, in order */
	int nb;
	int active;			/* mnt_shift_enter() was called */
	int locked;
	int tries;			/* sets worked out for this operation */
	struct dentry *for_a, *for_b;	/* the hosts it was worked out for */
	struct dc_mnt *self_a, *self_b;	/* theirs, if they had one then */
	long c_a, c_b;			/* and what those counted then */
};
#define MNT_SHIFT_INIT	{ .e = NULL }

/*
 * How often one operation works its set out before it gives up (-ESTALE).
 * Each time but the first, another operation has moved something that it had
 * read, so the others make progress meanwhile; the bound is for counts that
 * are wrong, which no amount of looking again puts right.
 */
#ifndef DC_MNT_TRIES_MAX
#define DC_MNT_TRIES_MAX	(1 << 20)
#endif

static int mnt_shift_push(struct mnt_shift *ms, struct dentry *host,
			  struct dc_mnt *m, long by, struct dentry *up, int self)
{
	if (ms->n == ms->cap) {
		int cap = ms->cap ? 2 * ms->cap : 16;
		struct mnt_ent *e = realloc(ms->e, (size_t) cap * sizeof(*e));
		struct dc_mnt_bkt **b;

		if (!e)
			return -ENOMEM;
		ms->e = e;
		b = realloc(ms->b, (size_t) cap * sizeof(*b));
		if (!b)
			return -ENOMEM;
		ms->b = b;
		ms->cap = cap;
	}
	ms->e[ms->n].host = host;
	ms->e[ms->n].up = up;
	ms->e[ms->n].m = m;
	ms->e[ms->n].by = by;
	ms->e[ms->n].self = self;
	ms->n++;
	return 0;
}

/*
 * An operation that changes counts begins and ends here.  Nothing in a normal
 * build.  A build for the tests has dc_mount_check() shut every such operation
 * out while it counts again, which is what lets it run with everything else:
 * a lock taken shared here, and not with the engine's fallback lane held,
 * since it is held over begin(), which may park for the lane.
 */
static inline void mnt_shift_enter(struct dcache *dc, struct mnt_shift *ms)
{
	if (ms->active)
		return;
	ms->active = 1;
#ifdef DC_TEST_MNT_CHECK_ONLINE
	pthread_rwlock_rdlock(&dc->mnt_check_lock);
#else
	(void) dc;
#endif
}

static inline void mnt_shift_unlock(struct mnt_shift *ms)
{
	int i;

	if (!ms->locked)
		return;
	for (i = 0; i < ms->nb; i++)
		mnt_bkt_unlock(ms->b[i]);
	ms->locked = 0;
}

/*
 * Forget the set: unpin everything in it.  Under the buckets that are held,
 * if they are, so that an operation takes each bucket twice and not three
 * times.
 */
static void mnt_shift_drop(struct dcache *dc, struct mnt_shift *ms)
{
	int i, j;

	for (i = 0; i < ms->n; i++) {
		struct dc_mnt *m = ms->e[i].m;
		struct dc_mnt_bkt *b;

		if (!m)
			continue;
		b = mnt_bkt(dc, m->host);
		for (j = 0; ms->locked && j < ms->nb; j++)
			if (ms->b[j] == b)
				break;
		if (ms->locked && j < ms->nb)
			mnt_unpin_locked(dc, m);
		else
			mnt_unpin(dc, m);
	}
	ms->n = 0;
	ms->for_a = ms->for_b = NULL;
	ms->self_a = ms->self_b = NULL;
	ms->c_a = ms->c_b = 0;
}

/* The operation is over, committed or not. */
static void mnt_shift_end(struct dcache *dc, struct mnt_shift *ms)
{
	mnt_shift_drop(dc, ms);
	mnt_shift_unlock(ms);
	if (ms->active) {
#ifdef DC_TEST_MNT_CHECK_ONLINE
		pthread_rwlock_unlock(&dc->mnt_check_lock);
#endif
		ms->active = 0;
	}
	free(ms->e);
	free(ms->b);
	ms->e = NULL;
	ms->b = NULL;
	ms->cap = 0;
}

/*
 * Step 2: take the buckets of the set, and look again.  Returns 1 with them
 * held if the set is still right, 0 with none held if it is not.  For a move
 * the caller holds the locks of its commit, the child heads of both parents
 * among them, so the flags of what it moves cannot be set under it.
 */
static int mnt_shift_lock(struct dcache *dc, struct mnt_shift *ms)
{
	int i, j, k, ok = 1;

	ms->nb = 0;
	for (i = 0; i < ms->n; i++) {
		struct dc_mnt_bkt *b;

		if (!ms->e[i].m || (!ms->e[i].by && !ms->e[i].self))
			continue;
#ifdef DC_TEST_MNT_NO_SELF_LOCK
		if (ms->e[i].self && !ms->e[i].by)
			continue;
#endif
		b = mnt_bkt(dc, ms->e[i].host);
		for (j = 0; j < ms->nb && ms->b[j] < b; j++)
			;
		if (j < ms->nb && ms->b[j] == b)	/* two in one bucket */
			continue;
		for (k = ms->nb; k > j; k--)
			ms->b[k] = ms->b[k - 1];
		ms->b[j] = b;
		ms->nb++;
	}
	for (i = 0; i < ms->nb; i++)
		mnt_bkt_lock(ms->b[i]);
	ms->locked = 1;
#ifndef DC_TEST_MNT_NO_VALIDATE
	for (i = 0; i < ms->n && ok; i++)
		if (ms->e[i].up && parent_of_rcu(ms->e[i].host) != ms->e[i].up)
			ok = 0;
	/* What moves: the count that was read, and a count if it has the flag. */
	if (ms->for_a && (mnt_counted(ms->for_a) != (ms->self_a != NULL) ||
			  (ms->self_a && mnt_read(ms->self_a) != ms->c_a)))
		ok = 0;
	if (ms->for_b && (mnt_counted(ms->for_b) != (ms->self_b != NULL) ||
			  (ms->self_b && mnt_read(ms->self_b) != ms->c_b)))
		ok = 0;
#endif
	if (!ok)
		mnt_shift_unlock(ms);
	return ok;
}

/*
 * Step 1 for @d mountpoints that are about to go from below @from to below
 * @to, two directories, with what moves.  The ancestors of @from count the @d
 * that move, so each has a struct dc_mnt; those of @to are frozen here, up to
 * the first that @from has too.  From that one up nothing changes.
 *
 * @bar_a, @bar_b: what moves.
 *
 * Returns 0; 1 if what it read does not hold together, which is something
 * moving under it (the caller goes again); -ENOENT (@to is gone), -ENOMEM.
 */
static int mnt_shift_chains(struct dcache *dc, struct mnt_shift *ms,
			    struct dentry *from, struct dentry *to, long d,
			    struct dentry *bar_a, struct dentry *bar_b)
{
	struct dentry *a, *p;
	int old0 = ms->n, nold, shared = -1, i, first = 1, hops = 0;

	/*
	 * Who is above @from: read, and no more.  Only those below the first
	 * that @to has too are pinned, further down, and looked at again under
	 * their buckets; an operation leaves the counts above that one alone,
	 * the root's first of all, or every operation would meet there.
	 */
	for (a = from; ; a = p) {
		if (a == bar_a || a == bar_b || ++hops > DC_LOOP_MAX)
			return 1;
		p = a == dc->root ? NULL : parent_of_rcu(a);
		if (mnt_shift_push(ms, a, NULL, -d, p, 0))
			return -ENOMEM;
		if (!p)
			break;
	}
	nold = ms->n;
	for (a = to; ; a = p) {
		struct dc_mnt *m;

		for (i = old0; i < nold; i++)
			if (ms->e[i].host == a)
				break;
		if (i < nold) {
			shared = i;
			break;
		}
		if (a == bar_a || a == bar_b || ++hops > 2 * DC_LOOP_MAX)
			return 1;
		m = mnt_get(dc, a);
		if (!m)
			return -ENOMEM;
		if (mnt_shift_push(ms, a, m, d, NULL, 0)) {
			mnt_unpin(dc, m);
			return -ENOMEM;
		}
		p = mnt_freeze(dc, a, first);
		if (!p)
			return -ENOENT;
		ms->e[ms->n - 1].up = p;
#ifdef DC_TEST_HOOKS
		if (dc_test_mnt_frozen_hook)
			dc_test_mnt_frozen_hook(a->d_id);
#endif
		first = 0;
	}
	/* From the one they share up: no change, no lock, no second look. */
	for (i = shared; i < nold; i++) {
		ms->e[i].by = 0;
		ms->e[i].up = NULL;
	}
	for (i = old0; i < shared; i++) {
		ms->e[i].m = mnt_pin(dc, ms->e[i].host);
		if (!ms->e[i].m)
			return 1;	/* it counts what moves no more */
	}
	return 0;
}

/* Add the changes to @txn.  The buckets are held: single writer. */
static void mnt_shift_record(dc_swtxn_t *txn, struct mnt_shift *ms)
{
	int i;

	for (i = 0; i < ms->n; i++) {
		struct dc_mnt *m = ms->e[i].m;
		long v;

		if (!m || !ms->e[i].by)
			continue;
		v = mnt_read(m);
		if (v + ms->e[i].by < 0)	/* a mutant's; the check says so */
			continue;
		(void) urcu_txn_store_sw(txn, &m->count, mnt_ptr(v),
					 mnt_ptr(v + ms->e[i].by), DC_MNT_TAG);
	}
}

/* The changes in a transaction of their own. */
static int mnt_shift_commit(struct mnt_shift *ms)
{
	struct urcu_txn txn;
	enum urcu_txn_status st;

	urcu_txn_init(&txn, NULL);	/* no lane: single writer, cannot abort */
	urcu_txn_declare_disjoint(&txn);
	do {
		urcu_txn_begin(&txn);
		mnt_shift_record(&txn, ms);
		st = urcu_txn_commit_sw(&txn);
		urcu_txn_end(&txn);
	} while (st == URCU_TXN_STATUS_ABORT);	/* defensive */
	return st == URCU_TXN_STATUS_OK ? 0 : -ENOMEM;
}

/* What moves: its struct dc_mnt, pinned, and the count read off it. */
static int mnt_shift_self(struct dcache *dc, struct mnt_shift *ms,
			  struct dentry *host, struct dentry *parent,
			  struct dc_mnt **self, long *c)
{
	struct dc_mnt *m = mnt_pin(dc, host);

	*self = m;
	*c = m ? mnt_read(m) : 0;
	if (!m)
		return 0;
	if (mnt_shift_push(ms, host, m, 0, parent, 1)) {
		mnt_unpin(dc, m);
		*self = NULL;
		return -ENOMEM;
	}
	return 0;
}

/*
 * Step 1 for a rename that takes @host, which had the flag, from @from to @to.
 * The caller holds no lock and has no transaction open.  On 0 the set is
 * @ms's, or @ms is empty and the caller looks again; an error is the rename's.
 */
static int mnt_move_begin(struct dcache *dc, struct mnt_shift *ms,
			  struct dentry *host, struct dentry *from,
			  struct dentry *to)
{
	int ret;

	mnt_shift_enter(dc, ms);
	mnt_shift_drop(dc, ms);
	if (ms->tries++)
		DC_MNT_STAT_INC(dc, mnt_again);
	if (ms->tries > DC_MNT_TRIES_MAX)
		return -ESTALE;
	ret = mnt_shift_self(dc, ms, host, from, &ms->self_a, &ms->c_a);
	if (!ret && ms->c_a)
		ret = mnt_shift_chains(dc, ms, from, to, ms->c_a, host, NULL);
	if (ret) {
		mnt_shift_drop(dc, ms);
		return ret < 0 ? ret : 0;
	}
	ms->for_a = host;
	return 0;
}

/*
 * The same for an exchange of @ha, in @pa, with @hb, in @pb.  What changes is
 * the difference of the two counts, between the ancestors of the one parent
 * and those of the other; the side with the larger count has them all.
 */
static int __attribute__((unused))
mnt_xchg_begin(struct dcache *dc, struct mnt_shift *ms,
	       struct dentry *ha, struct dentry *hb,
	       struct dentry *pa, struct dentry *pb)
{
	int ret;

	mnt_shift_enter(dc, ms);
	mnt_shift_drop(dc, ms);
	if (ms->tries++)
		DC_MNT_STAT_INC(dc, mnt_again);
	if (ms->tries > DC_MNT_TRIES_MAX)
		return -ESTALE;
	ret = mnt_shift_self(dc, ms, ha, pa, &ms->self_a, &ms->c_a);
	if (!ret)
		ret = mnt_shift_self(dc, ms, hb, pb, &ms->self_b, &ms->c_b);
	if (!ret && ms->c_a > ms->c_b)
		ret = mnt_shift_chains(dc, ms, pa, pb, ms->c_a - ms->c_b, ha, hb);
	else if (!ret && ms->c_b > ms->c_a)
		ret = mnt_shift_chains(dc, ms, pb, pa, ms->c_b - ms->c_a, ha, hb);
	if (ret) {
		mnt_shift_drop(dc, ms);
		return ret < 0 ? ret : 0;
	}
	ms->for_a = ha;
	ms->for_b = hb;
	return 0;
}

/* Quiescent teardown: what is still mounted has a count. */
static void mnt_destroy(struct dcache *dc)
{
	unsigned int b;

	for (b = 0; b < (1U << DC_MNT_HASH_BITS); b++) {
		struct cds_hlist_node *n, *next;

		for (n = dc->mnt_bkt[b].head.next; n; n = next) {
			next = n->next;
			free(caa_container_of(n, struct dc_mnt, node));
		}
	}
	free(dc->mnt_bkt);
#ifdef DC_TEST_MNT_CHECK_ONLINE
	pthread_rwlock_destroy(&dc->mnt_check_lock);
#endif
}

#else /* DCACHE_MOUNT_OPS: the operations, after everything they use */

const char *dc_mount_arm(void)
{
	return "count";
}

/* The host that @path names, its parent in *@parent.  Under RCU. */
static struct dentry *mnt_resolve(struct dcache *dc, const struct dc_path *path,
				  struct dentry **parent)
{
	struct dentry *d = dc->root, *p = dc->root;
	uint32_t i;

	for (i = 0; i < path->ndepth && d; i++) {
		p = d;
		d = txn_child_lookup_rcu(dc, d, &path->comp[i]);
	}
	if (parent)
		*parent = p;
	return d;
}

int dc_mount(struct dcache *dc, const struct dc_path *path)
{
	struct mnt_shift ms = MNT_SHIFT_INIT;
	struct dentry *parent, *host = NULL, *a, *p;
	const struct qstr *name;
	struct dc_mnt *m;
	int ret;

	if (path->ndepth == 0)
		return -EINVAL;
	name = &path->comp[path->ndepth - 1];
	mnt_shift_enter(dc, &ms);
	rcu_read_lock();
	host = mnt_resolve(dc, path, &parent);
	for (;;) {
		struct urcu_txn_sw_hlist_head *heads[2];
		struct dentry *top;
		int marked = 0;

		mnt_shift_drop(dc, &ms);
		if (ms.tries++)
			DC_MNT_STAT_INC(dc, mnt_again);
		if (ms.tries > DC_MNT_TRIES_MAX) {
			ret = -ESTALE;
			break;
		}
		/*
		 * The path names the object once.  What is above it may move
		 * while this goes on, and it is still the one to mount: found
		 * again by where it is now, under the name it had.
		 */
		if (host) {
			parent = parent_of_rcu(host);
			top = find_top_rcu(dc, parent, name);
		}
		if (!host || !top || host_of_rcu(top) != host) {
			ret = -ENOENT;
			break;
		}
		if (mnt_here(host)) {
			ret = -EBUSY;
			break;
		}
		m = mnt_get(dc, host);
		if (!m || mnt_shift_push(&ms, host, m, 1, parent, 1)) {
			if (m)
				mnt_unpin(dc, m);
			ret = -ENOMEM;
			break;
		}
		/*
		 * The mountpoint itself is frozen under its bucket as well as
		 * under the child head of its parent: dc_delete() holds only
		 * the first.
		 */
		heads[0] = bucket_of(dc, parent, name->hash);
		heads[1] = &parent->d_child_head;
		bl_lock_n(heads, 2);
		(void) bl_hlist_resolve(rcu_dereference(top->d_hash.next), &marked);
		if (marked || !DC_IS_POSITIVE(host)) {
			bl_unlock_n(heads, 2);
			if (marked)
				continue;	/* look for the top again */
			ret = -ENOENT;
			break;
		}
		uatomic_or(&host->d_mnt, DC_MNT_COUNTED);
		bl_unlock_n(heads, 2);
#ifdef DC_TEST_HOOKS
		if (dc_test_mnt_frozen_hook)
			dc_test_mnt_frozen_hook(host->d_id);
#endif
		ret = 0;
		for (a = parent; ; a = p) {
			m = mnt_get(dc, a);
			if (!m || mnt_shift_push(&ms, a, m, 1, NULL, 0)) {
				if (m)
					mnt_unpin(dc, m);
				ret = -ENOMEM;
				break;
			}
			p = mnt_freeze(dc, a, 0);
			if (a != dc->root)
				ms.e[ms.n - 1].up = p;
#ifdef DC_TEST_HOOKS
			if (dc_test_mnt_frozen_hook)
				dc_test_mnt_frozen_hook(a->d_id);
#endif
			if (a == dc->root)
				break;
#ifdef DC_TEST_MNT_MOUNT_SHORT
			break;			/* counts the parent and no more */
#endif
		}
		if (ret)
			break;
		if (!mnt_shift_lock(dc, &ms))
			continue;		/* something moved: start over */
		if (mnt_here(host))
			ret = -EBUSY;		/* another mount was first */
		else
			ret = mnt_shift_commit(&ms);
		if (!ret) {
			uatomic_or(&host->d_mnt, DC_MNT_HERE);
			DC_MNT_STAT_INC(dc, mnt_mounts);
		}
		break;
	}
	rcu_read_unlock();
	mnt_shift_end(dc, &ms);
	return ret;
}

int dc_umount(struct dcache *dc, const struct dc_path *path)
{
	struct mnt_shift ms = MNT_SHIFT_INIT;
	struct dentry *host, *a, *p;
	int ret;

	mnt_shift_enter(dc, &ms);
	rcu_read_lock();
	host = mnt_resolve(dc, path, NULL);	/* named once: see dc_mount() */
	for (;;) {
		int hops = 0, torn = 0;

		mnt_shift_drop(dc, &ms);
		if (ms.tries++)
			DC_MNT_STAT_INC(dc, mnt_again);
		if (ms.tries > DC_MNT_TRIES_MAX) {
			ret = -ESTALE;
			break;
		}
		if (!host) {
			ret = -ENOENT;
			break;
		}
		if (!mnt_here(host)) {
			ret = -EINVAL;
			break;
		}
		/* Everything above a mountpoint counts it. */
		ret = 0;
		for (a = host; ; a = p) {
			struct dc_mnt *m = mnt_pin(dc, a);

			p = a == dc->root ? NULL : parent_of_rcu(a);
			if (!m || ++hops > DC_LOOP_MAX) {
				if (m)
					mnt_unpin(dc, m);
				torn = 1;	/* it moved, or was unmounted */
				break;
			}
			if (mnt_shift_push(&ms, a, m, -1, p, a == host)) {
				mnt_unpin(dc, m);
				ret = -ENOMEM;
				break;
			}
			if (!p)
				break;
		}
		if (ret)
			break;
		if (torn || !mnt_shift_lock(dc, &ms))
			continue;
		if (!mnt_here(host))
			ret = -EINVAL;		/* another unmount was first */
		else
			ret = mnt_shift_commit(&ms);
		if (!ret) {
			uatomic_and(&host->d_mnt, ~DC_MNT_HERE);
			DC_MNT_STAT_INC(dc, mnt_umounts);
		}
		break;
	}
	rcu_read_unlock();
	mnt_shift_end(dc, &ms);
	return ret;
}

long dc_mount_count(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *d;
	struct dc_mnt *m;
	long v = 0;

	rcu_read_lock();
	d = mnt_resolve(dc, path, NULL);
	if (!d) {
		v = -ENOENT;
	} else if (mnt_counted(d)) {
		m = mnt_find_rcu(dc, d);
		if (m)
			v = mnt_read(m);
	}
	rcu_read_unlock();
	return v;
}

/*
 * Is @host at or below @root, at an instant during the call?  The way up, by
 * the tops that name each entry, each confirmed in the index when it is read
 * and all of them again when the top is reached: a top that was in the index
 * both times was in it in between, so the whole way stood at the instant the
 * climb ended.  -1: too deep to say.
 */
#define DC_MNT_CLIMB_MAX	(4 * DC_PATH_MAX)
static int mnt_below_rcu(struct dcache *dc, struct dentry *host,
			 struct dentry *root)
{
	struct dentry *latch[DC_MNT_CLIMB_MAX];

	for (;;) {
		struct dentry *h = host;
		int n = 0, k, stale = 0, below = 0;

		for (;;) {
			struct dentry *top;

			if (h == root) {
				below = 1;
				break;
			}
			if (h == dc->root)
				break;
			if (n == DC_MNT_CLIMB_MAX)
				return -1;
			top = named_top_rcu(h);
			cmm_smp_rmb();		/* identity before the confirm */
			if (top_unhashed_rcu(top)) {
				if (!entry_alive(h))
					return 0;	/* removed */
				stale = 1;	/* renamed: look again */
				break;
			}
			latch[n++] = top;
			h = DC_IPARENT(top);
		}
		if (!stale) {
			cmm_smp_rmb();		/* the climb before the check */
			for (k = 0; k < n; k++)
				if (top_unhashed_rcu(latch[k]))
					stale = 1;
			if (!stale)
				return below;
		}
		caa_cpu_relax();
	}
}

/*
 * A mountpoint at or below @path: what d_invalidate() needs in order to detach
 * it.  Under RCU alone.  Down through the children that have a count; what it
 * comes to may have got there by a way that was never whole, a directory
 * having moved while it was being gone through, so it is checked to be below
 * @path before it is handed back.  It goes again when the way down ends short
 * or the check says no, and says "none" only on a count of zero at @path.
 */
long dc_mount_find(struct dcache *dc, const struct dc_path *path, uint64_t *id)
{
	struct dentry *top, *cur, *next;
	struct dc_mnt *m;
	long ret;

	rcu_read_lock();
	top = mnt_resolve(dc, path, NULL);
	if (!top) {
		ret = -ENOENT;
		goto out;
	}
	for (;;) {
		int hops = 0, below;

		m = mnt_counted(top) ? mnt_find_rcu(dc, top) : NULL;
		if (!m || mnt_read(m) == 0) {
			ret = 0;
			goto out;
		}
		for (cur = top; cur && !mnt_here(cur); cur = next) {
			struct urcu_txn_sw_hlist_node *n;

			next = NULL;
			if (++hops > DC_LOOP_MAX)
				break;
#ifdef DC_TEST_HOOKS
			if (dc_test_mnt_found_hook)
				dc_test_mnt_found_hook(cur->d_id);
#endif
			for (n = bl_hlist_first_rcu(&cur->d_child_head); n;
			     n = bl_hlist_next_rcu(n)) {
				struct dentry *h = sib_dentry(n);

#ifdef DC_SCAN_LINGER
				if (node_lingers(h))
					continue;
#endif
				h = host_of_rcu(h);
				m = mnt_counted(h) ? mnt_find_rcu(dc, h) : NULL;
				if (m && mnt_read(m) > 0) {
					next = h;
					break;
				}
			}
		}
#ifdef DC_TEST_MNT_FIND_NO_CHECK
		below = 1;
#else
		below = cur ? mnt_below_rcu(dc, cur, top) : 0;
#endif
		if (cur && below > 0 && mnt_here(cur)) {
			*id = cur->d_id;
			ret = 1;
			goto out;
		}
		if (below < 0) {
			ret = -ELOOP;
			goto out;
		}
		DC_MNT_STAT_INC(dc, mnt_find_again);
		caa_cpu_relax();
	}
out:
	rcu_read_unlock();
	return ret;
}

/*
 * Count again, from the mountpoints up, and compare: how many struct dc_mnt
 * hold something else than what is below their dentry, or should not be there.
 * With nothing else running; or, in a build with -DDC_TEST_MNT_CHECK_ONLINE,
 * with everything else, every operation that changes a count being shut out
 * for the time it takes (mnt_shift_enter()).
 */
long dc_mount_check(struct dcache *dc, void *stream)
{
	FILE *f = stream;
	struct dc_mnt *m;
	unsigned long nobj = 0;
	long bad = 0;
	unsigned int b;

#ifdef DC_TEST_MNT_CHECK_ONLINE
	pthread_rwlock_wrlock(&dc->mnt_check_lock);
#endif
	rcu_read_lock();
	for (b = 0; b < (1U << DC_MNT_HASH_BITS); b++)
		cds_hlist_for_each_entry_rcu_2(m, &dc->mnt_bkt[b].head, node)
			m->shadow = 0;
	for (b = 0; b < (1U << DC_MNT_HASH_BITS); b++) {
		cds_hlist_for_each_entry_rcu_2(m, &dc->mnt_bkt[b].head, node) {
			struct dentry *a;
			int hops = 0;

			if (!mnt_here(m->host))
				continue;
			for (a = m->host; ; a = parent_of_rcu(a)) {
				struct dc_mnt *am = mnt_find_rcu(dc, a);

				if (am) {
					am->shadow++;
				} else {
					bad++;
					if (f)
						fprintf(f, "  no count in %llu, above the mountpoint %llu\n",
							(unsigned long long) a->d_id,
							(unsigned long long) m->host->d_id);
				}
				if (a == dc->root || ++hops > DC_LOOP_MAX)
					break;
			}
		}
	}
	for (b = 0; b < (1U << DC_MNT_HASH_BITS); b++) {
		cds_hlist_for_each_entry_rcu_2(m, &dc->mnt_bkt[b].head, node) {
			long v = mnt_read(m);

			nobj++;
			if (v == m->shadow && v > 0 && !m->pins &&
			    mnt_counted(m->host))
				continue;
			bad++;
			if (f)
				fprintf(f, "  %llu: count %ld, %ld mountpoints at or below it%s%s\n",
					(unsigned long long) m->host->d_id, v,
					m->shadow,
					mnt_counted(m->host) ? "" : ", flag clear",
					m->pins ? ", still pinned" : "");
		}
	}
#ifndef DC_MNT_NO_STATS
	if (nobj != uatomic_load(&dc->mnt_objs, CMM_RELAXED)) {
		bad++;
		if (f)
			fprintf(f, "  %lu counts hashed, %lu accounted\n", nobj,
				uatomic_load(&dc->mnt_objs, CMM_RELAXED));
	}
#else
	(void) nobj;
#endif
	rcu_read_unlock();
#ifdef DC_TEST_MNT_CHECK_ONLINE
	pthread_rwlock_unlock(&dc->mnt_check_lock);
#endif
	return bad;
}

void dc_mount_stats(struct dcache *dc, struct dc_mount_stats *st)
{
	st->mounts = uatomic_load(&dc->mnt_mounts, CMM_RELAXED);
	st->umounts = uatomic_load(&dc->mnt_umounts, CMM_RELAXED);
	st->moves = uatomic_load(&dc->mnt_moves, CMM_RELAXED);
	st->frozen = uatomic_load(&dc->mnt_frozen, CMM_RELAXED);
	st->again = uatomic_load(&dc->mnt_again, CMM_RELAXED);
	st->find_again = uatomic_load(&dc->mnt_find_again, CMM_RELAXED);
	st->waits = uatomic_load(&dc->mnt_waits, CMM_RELAXED);
	st->counts = uatomic_load(&dc->mnt_objs, CMM_RELAXED);
}

#endif /* DCACHE_MOUNT_OPS */
