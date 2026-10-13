// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_mount_inline.h -- the count of mountpoints, kept in the dentry.
 *
 * -DDC_MOUNT_INLINE: the same question and the same operations as
 * dcache_mount.h, which see, with the count where the kernel could keep it
 * without a table: in the directory's inode, 32 bits that are there already.
 * This model has no inode, so the field is in the host, d_mnt_n.
 *
 * With the count in place there is nothing to allocate, so nothing to pin, to
 * hash or to get ready, and the count is its own flag: a dentry that counts
 * nothing has nothing mounted at or below it.  What is left is four rules.
 *
 *   1. ONE LOCK FOR THOSE WHO CHANGE A COUNT, dc->mnt_lock: a mount, an
 *      unmount, a rename or an exchange that takes a count to another
 *      directory.  A rename with nothing mounted below does not take it.
 *
 *   2. A COUNT CHANGES UNDER THE LOCK THAT A MOVE OF ITS DENTRY HOLDS, and the
 *      dentry above it is read under that same lock (mnt_add_up()).  A rename
 *      reads the count of what it moves under that lock too.  So a rename
 *      either committed before the count went up, and whoever raises the
 *      counts goes on from where the dentry is now; or it sees a count, lets
 *      go, and waits for rule 1's lock.
 *
 *   3. ADD BEFORE, TAKE AWAY AFTER.  A mount raises every count from the
 *      mountpoint up before it sets the flag.  A rename adds what it carries
 *      to everything above the new parent before its commit, and takes it
 *      from everything above the old parent after.  An unmount clears the
 *      flag first.  A count is never short: a reader that finds zero has an
 *      instant at which nothing was mounted there.  It may be too high for
 *      the length of one operation, in the directories both parents have
 *      above them, in the new ones before the commit and in the old ones
 *      after.
 *
 *   4. A REMOVAL waits for rule 1's lock when it finds a count on an empty
 *      directory: a rename has added to it and is about to land in it.
 *
 * Nothing is looked at twice and nothing starts over; the commit of a rename
 * cannot fail for a count.  That is what the kernel asks of d_move_commit().
 *
 * There the lock of rule 2 is the dentry's own d_lock, which a move holds.
 * Rule 1's lock would be a spinlock of the super block; the renames it keeps
 * apart are kept apart already by s_vfs_rename_mutex, and mounts by
 * namespace_sem.  And 32 bits are not a slot of the transaction engine, which
 * is why the count does not go with the move in one commit here, and why rule
 * 3 is an order and not a transaction.
 *
 * The readers are dcache_mount.h's: dc_mount_count() is one load, and
 * dc_mount_find() goes down through the children that have a count and checks
 * what it comes to.
 */
#ifndef DCACHE_MOUNT_OPS

#define DC_MNT_HERE	1UL	/* this object is a mountpoint */

#ifdef DC_MNT_NO_STATS
#define DC_MNT_STAT_INC(dc, f)	((void) (dc))
#define DC_MNT_STAT_ADD(dc, f, n)	((void) (dc))
#else
#define DC_MNT_STAT_INC(dc, f)	uatomic_inc(&(dc)->f)
#define DC_MNT_STAT_ADD(dc, f, n)	uatomic_add(&(dc)->f, (n))
#endif

/* Mountpoints at or below @host.  One load; never short (rule 3). */
static inline long mnt_n(const struct dentry *host)
{
	return uatomic_load(&host->d_mnt_n, CMM_RELAXED);
}

static inline int mnt_counted(const struct dentry *host)
{
	return mnt_n(host) != 0;
}

static inline int mnt_here(const struct dentry *host)
{
	return (uatomic_load(&host->d_mnt, CMM_RELAXED) & DC_MNT_HERE) != 0;
}

/* What a move to another directory asks of the host it moves: rule 2. */
static inline int mnt_move_counted(const struct dentry *host)
{
#ifdef DC_TEST_MNT_NO_FLAG_CHECK
	(void) host;
	return 0;
#else
	return mnt_counted(host);
#endif
}

/* Rule 1.  Taken on line, and not with the engine's fallback lane held. */
static inline void mnt_lock(struct dcache *dc)
{
	pthread_mutex_lock(&dc->mnt_lock);
}

static inline void mnt_unlock(struct dcache *dc)
{
	pthread_mutex_unlock(&dc->mnt_lock);
}

/* Rule 4. */
static void mnt_wait(struct dcache *dc)
{
	DC_MNT_STAT_INC(dc, mnt_waits);
	mnt_lock(dc);
	mnt_unlock(dc);
}

/* The caller holds dc->mnt_lock and what keeps @a where it is. */
static inline void mnt_bump(struct dcache *dc, struct dentry *a, long n)
{
	long v = uatomic_add_return(&a->d_mnt_n, n);

	DC_MNT_STAT_ADD(dc, mnt_sum, n);
	if (n > 0 && v == n)
		DC_MNT_STAT_INC(dc, mnt_objs);
	else if (n < 0 && v == 0)
		DC_MNT_STAT_ADD(dc, mnt_objs, -1L);
}

/*
 * Add @n, which may be negative, to the count of @from and of everything
 * above it, up to @stop, which is left alone, or to the root.  Rule 2: each
 * under the child head of its parent, the lock that a move of it holds, with
 * the parent read under that lock.
 *
 * @check_alive: @from may be gone (-ENOENT, nothing added).  @bar: what moves;
 * to come to it is a cycle (-EINVAL, nothing left added).  @hook: these are
 * the new ancestors of something, for the tests.
 */
static int mnt_add_up(struct dcache *dc, struct dentry *from, long n,
		      struct dentry *stop, int check_alive, struct dentry *bar,
		      int hook)
{
	struct dentry *a, *p;

	for (a = from; a != stop; a = p) {
		if (a == bar) {
			(void) mnt_add_up(dc, from, -n, a, 0, NULL, 0);
			return -EINVAL;
		}
		if (a == dc->root) {		/* does not move */
			mnt_bump(dc, a, n);
			p = stop;
		} else {
#ifdef DC_TEST_MNT_ADD_NO_LOCK
			p = parent_of_rcu(a);
			mnt_bump(dc, a, n);
			(void) check_alive;
#else
			for (;;) {
				p = parent_of_rcu(a);
				bl_lock(&p->d_child_head);
				if (parent_of_rcu(a) == p)
					break;
				bl_unlock(&p->d_child_head);	/* it moved */
			}
			if (check_alive && a == from && !entry_alive(a)) {
				bl_unlock(&p->d_child_head);
				return -ENOENT;
			}
			mnt_bump(dc, a, n);
			bl_unlock(&p->d_child_head);
#endif
		}
		if (hook) {
			DC_MNT_STAT_INC(dc, mnt_frozen);
#ifdef DC_TEST_HOOKS
			if (dc_test_mnt_frozen_hook)
				dc_test_mnt_frozen_hook(a->d_id);
#endif
		}
		if (a == dc->root)
			break;
	}
	return 0;
}

/* What one rename or exchange carries, and how far it has got. */
struct mnt_shift {
	int held;			/* dc->mnt_lock */
	int locked;			/* about to commit with a count */
	int added;			/* @n is on what is above @to */
	struct dentry *for_a, *for_b;	/* the hosts it was worked out for */
	long c_a, c_b;			/* what each counted */
	struct dentry *from, *to;	/* @n goes from above the one to the other */
	long n;
};
#define MNT_SHIFT_INIT	{ .held = 0 }

/* Rule 3, the first half: on what is above @to before the commit. */
static int mnt_shift_add(struct dcache *dc, struct mnt_shift *ms,
			 struct dentry *from, struct dentry *to, long n,
			 struct dentry *bar, int check)
{
	int ret = 0;

	ms->from = from;
	ms->to = to;
	ms->n = n;
#ifndef DC_TEST_MNT_ADD_AFTER
	ret = mnt_add_up(dc, to, n, NULL, check, bar, 1);
#else
	(void) bar;
	(void) check;
#endif
	if (!ret) {
		ms->added = 1;
#ifdef DC_TEST_MNT_SUB_BEFORE
		(void) mnt_add_up(dc, from, -n, NULL, 0, NULL, 0);
#endif
	}
	return ret;
}

/* The operation did not happen: take back what was added for it. */
static void mnt_shift_undo(struct dcache *dc, struct mnt_shift *ms)
{
	if (ms->added) {
#ifndef DC_TEST_MNT_ADD_AFTER
		(void) mnt_add_up(dc, ms->to, -ms->n, NULL, 0, NULL, 0);
#endif
#ifdef DC_TEST_MNT_SUB_BEFORE
		(void) mnt_add_up(dc, ms->from, ms->n, NULL, 0, NULL, 0);
#endif
		ms->added = 0;
	}
	ms->for_a = ms->for_b = NULL;
	ms->c_a = ms->c_b = 0;
}

/*
 * A rename is about to take @host, which had a count, from @from to @to.  The
 * caller holds no lock and has no transaction open.  Takes dc->mnt_lock, which
 * mnt_shift_end() drops: under it the count of @host cannot change and nobody
 * else moves @host.
 */
static int mnt_move_begin(struct dcache *dc, struct mnt_shift *ms,
			  struct dentry *host, struct dentry *from,
			  struct dentry *to)
{
	int ret = 0;

	if (!ms->held) {
		mnt_lock(dc);
		ms->held = 1;
	}
	mnt_shift_undo(dc, ms);
	ms->c_a = mnt_n(host);
	if (ms->c_a)
		ret = mnt_shift_add(dc, ms, from, to, ms->c_a, host, 1);
	if (!ret)
		ms->for_a = host;
	return ret;
}

/*
 * An exchange of @ha, in @pa, with @hb, in @pb.  What changes hands is the
 * difference of the two counts, towards the side that had the smaller.
 */
static int __attribute__((unused))
mnt_xchg_begin(struct dcache *dc, struct mnt_shift *ms,
	       struct dentry *ha, struct dentry *hb,
	       struct dentry *pa, struct dentry *pb)
{
	int ret = 0;

	if (!ms->held) {
		mnt_lock(dc);
		ms->held = 1;
	}
	mnt_shift_undo(dc, ms);
	ms->c_a = mnt_n(ha);
	ms->c_b = mnt_n(hb);
	if (ms->c_a > ms->c_b)
		ret = mnt_shift_add(dc, ms, pa, pb, ms->c_a - ms->c_b, ha, 0);
	else if (ms->c_b > ms->c_a)
		ret = mnt_shift_add(dc, ms, pb, pa, ms->c_b - ms->c_a, hb, 0);
	if (!ret) {
		ms->for_a = ha;
		ms->for_b = hb;
	}
	return ret;
}

/*
 * About to commit, under the locks of the move.  Nothing to take and nothing
 * to look at again: the counts were added under dc->mnt_lock, which is held.
 */
static inline int mnt_shift_lock(struct dcache *dc, struct mnt_shift *ms)
{
	(void) dc;
	ms->locked = 1;
	return 1;
}

static inline void mnt_shift_unlock(struct mnt_shift *ms)
{
	ms->locked = 0;
}

/* The count does not go with the move in a transaction: see the header. */
static inline void mnt_shift_record(dc_swtxn_t *txn, struct mnt_shift *ms)
{
	(void) txn;
	(void) ms;
}

static inline int mnt_shift_commit(struct mnt_shift *ms)
{
	(void) ms;
	return 0;
}

/* Rule 3, the second half: the move has committed. */
static void mnt_shift_done(struct dcache *dc, struct mnt_shift *ms)
{
	if (ms->added) {
#ifdef DC_TEST_MNT_ADD_AFTER
		(void) mnt_add_up(dc, ms->to, ms->n, NULL, 0, NULL, 0);
#endif
#ifndef DC_TEST_MNT_SUB_BEFORE
		(void) mnt_add_up(dc, ms->from, -ms->n, NULL, 0, NULL, 0);
#endif
		ms->added = 0;
	}
	ms->for_a = ms->for_b = NULL;
}

/* The operation is over, committed or not. */
static void mnt_shift_end(struct dcache *dc, struct mnt_shift *ms)
{
	if (!ms->held)
		return;
	mnt_shift_undo(dc, ms);
	mnt_unlock(dc);
	ms->held = 0;
}

/* dc_mount_check() needs to know what is mounted; nothing else does. */
struct dc_mnt_here {
	struct cds_list_head node;
	struct dentry *host;
};

static void mnt_destroy(struct dcache *dc)
{
	struct dc_mnt_here *h, *t;

	cds_list_for_each_entry_safe(h, t, &dc->mnt_list, node)
		free(h);
	pthread_mutex_destroy(&dc->mnt_lock);
}

#else /* DCACHE_MOUNT_OPS: the operations, after everything they use */

const char *dc_mount_arm(void)
{
	return "inline";
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
	struct dc_mnt_here *here = malloc(sizeof(*here));
	struct dentry *parent, *host, *stop = NULL;
	const struct qstr *name;
	int ret;

	if (path->ndepth == 0 || !here) {
		free(here);
		return path->ndepth ? -ENOMEM : -EINVAL;
	}
	name = &path->comp[path->ndepth - 1];
	mnt_lock(dc);
	rcu_read_lock();
	host = mnt_resolve(dc, path, &parent);
	for (;;) {
		struct urcu_txn_sw_hlist_head *heads[2];
		struct dentry *top = NULL;
		int marked = 0;

		/* The path names the object once: see dcache_mount.h. */
		if (host) {
			parent = parent_of_rcu(host);
			top = find_top_rcu(dc, parent, name);
		}
		if (!host || !top || host_of_rcu(top) != host) {
			ret = -ENOENT;
			goto out;
		}
		if (mnt_here(host)) {
			ret = -EBUSY;
			goto out;
		}
		/*
		 * The mountpoint counts itself first, which keeps it where it
		 * is (rule 2): under its bucket as well as under the child head
		 * of its parent, since dc_delete() holds only the first.
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
			goto out;
		}
		mnt_bump(dc, host, 1);
		bl_unlock_n(heads, 2);
		break;
	}
#ifdef DC_TEST_HOOKS
	if (dc_test_mnt_frozen_hook)
		dc_test_mnt_frozen_hook(host->d_id);
#endif
#ifdef DC_TEST_MNT_MOUNT_SHORT
	stop = parent == dc->root ? NULL : parent_of_rcu(parent);
#endif
	(void) mnt_add_up(dc, parent, 1, stop, 0, NULL, 1);
	uatomic_or(&host->d_mnt, DC_MNT_HERE);		/* rule 3: last */
	here->host = host;
	cds_list_add(&here->node, &dc->mnt_list);
	here = NULL;
	DC_MNT_STAT_INC(dc, mnt_mounts);
	ret = 0;
out:
	rcu_read_unlock();
	mnt_unlock(dc);
	free(here);
	return ret;
}

int dc_umount(struct dcache *dc, const struct dc_path *path)
{
	struct dc_mnt_here *h, *found = NULL;
	struct dentry *host;
	int ret = 0;

	mnt_lock(dc);
	rcu_read_lock();
	host = mnt_resolve(dc, path, NULL);
	if (!host) {
		ret = -ENOENT;
	} else if (!mnt_here(host)) {
		ret = -EINVAL;
	} else {
		uatomic_and(&host->d_mnt, ~DC_MNT_HERE);	/* rule 3: first */
		(void) mnt_add_up(dc, host, -1, NULL, 0, NULL, 0);
		cds_list_for_each_entry(h, &dc->mnt_list, node)
			if (h->host == host)
				found = h;
		if (found)
			cds_list_del(&found->node);
		DC_MNT_STAT_INC(dc, mnt_umounts);
	}
	rcu_read_unlock();
	mnt_unlock(dc);
	free(found);
	return ret;
}

long dc_mount_count(struct dcache *dc, const struct dc_path *path)
{
	struct dentry *d;
	long v;

	rcu_read_lock();
	d = mnt_resolve(dc, path, NULL);
	v = d ? mnt_n(d) : -ENOENT;
	rcu_read_unlock();
	return v;
}

/* Is @host at or below @root, at an instant during the call?  See dcache_mount.h. */
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

/* A mountpoint at or below @path, under RCU alone: see dcache_mount.h. */
long dc_mount_find(struct dcache *dc, const struct dc_path *path, uint64_t *id)
{
	struct dentry *top, *cur, *next;
	long ret;

	rcu_read_lock();
	top = mnt_resolve(dc, path, NULL);
	if (!top) {
		ret = -ENOENT;
		goto out;
	}
	for (;;) {
		int hops = 0, below;

		if (!mnt_n(top)) {
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
				if (mnt_n(h) > 0) {
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
 * Count again, from the mountpoints up, and compare: how many dentries count
 * something else than what is at or below them.  Under dc->mnt_lock, so it
 * can run with everything else: a dentry that counts something stays where
 * it is (rule 2), and what moves meanwhile counts nothing.
 */
long dc_mount_check(struct dcache *dc, void *stream)
{
	FILE *f = stream;
	struct dc_mnt_here *h;
	long bad = 0, total = 0;
	int pass;

	mnt_lock(dc);
	rcu_read_lock();
	/* 0: count again; 1: compare each dentry once; 2: forget. */
	for (pass = 0; pass < 3; pass++) {
		cds_list_for_each_entry(h, &dc->mnt_list, node) {
			struct dentry *a;
			int hops = 0;

			for (a = h->host; ; a = parent_of_rcu(a)) {
				if (pass == 0) {
					a->d_mnt_shadow++;
					total++;
				} else if (pass == 1 && a->d_mnt_shadow > 0) {
					if (mnt_n(a) != a->d_mnt_shadow) {
						bad++;
						if (f)
							fprintf(f, "  %llu: count %ld, %ld mountpoints at or below it\n",
								(unsigned long long) a->d_id,
								mnt_n(a), a->d_mnt_shadow);
					}
					a->d_mnt_shadow = -a->d_mnt_shadow;
				} else if (pass == 2) {
					a->d_mnt_shadow = 0;
				}
				if (a == dc->root || ++hops > DC_LOOP_MAX)
					break;
			}
		}
	}
#ifndef DC_MNT_NO_STATS
	/* And nothing is counted anywhere else. */
	if (uatomic_load(&dc->mnt_sum, CMM_RELAXED) != total) {
		bad++;
		if (f)
			fprintf(f, "  the counts add up to %ld, the mountpoints and what is above them to %ld\n",
				uatomic_load(&dc->mnt_sum, CMM_RELAXED), total);
	}
#endif
	rcu_read_unlock();
	mnt_unlock(dc);
	return bad;
}

void dc_mount_stats(struct dcache *dc, struct dc_mount_stats *st)
{
	st->mounts = uatomic_load(&dc->mnt_mounts, CMM_RELAXED);
	st->umounts = uatomic_load(&dc->mnt_umounts, CMM_RELAXED);
	st->moves = uatomic_load(&dc->mnt_moves, CMM_RELAXED);
	st->frozen = uatomic_load(&dc->mnt_frozen, CMM_RELAXED);
	st->again = 0;
	st->find_again = uatomic_load(&dc->mnt_find_again, CMM_RELAXED);
	st->waits = uatomic_load(&dc->mnt_waits, CMM_RELAXED);
	st->counts = (unsigned long) uatomic_load(&dc->mnt_objs, CMM_RELAXED);
}

#endif /* DCACHE_MOUNT_OPS */
