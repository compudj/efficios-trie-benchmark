/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * dcache_lru_shrink.h -- PHASE 3: the CLOCK shrinker, shared by the txn engines.
 *
 * Split from dcache_lru.h only because of ORDER: this half needs
 * children_empty() and the engine's own lru_evict_settled(), which are defined
 * far below struct dcache.  Include it after both exist.  The POLICY here is
 * the thing worth sharing -- second chance, LRU_REMOVED for an in-use entry,
 * skip anything mid-transition -- and it had already had to be corrected in two
 * places at once once before.
 */

#ifdef DC_LRU_MCAS
/*
 * PHASE 3, MCAS ARM: the same CLOCK policy as the lock arm, with no lock at any
 * point.  Each step reads the OLDEST node (the sentinel's successor) and then:
 *
 *   REFERENCED   -> clear the bit, del + add_tail (LRU_ROTATE)
 *   has children -> del (LRU_REMOVED; retain_dentry re-arms it later)
 *   otherwise    -> del, then kill
 *
 * There is no isolate/drop-the-lock dance here because there is no lock to drop:
 * the del IS the isolation, and it is atomic on both edges.  That is the whole
 * difference this arm exists to price -- the shrinker never shares a lock with
 * dentry ops, at the cost of a descriptor and a multi-CAS on every enqueue and
 * every unlink.
 *
 * lru_del_claimed() returning 0 means a peer changed the node's state under us
 * (an unlink, or another shrinker); the pass simply moves on.
 */
static long lru_shrink_range(struct dcache *dc, long nr,
			     unsigned int lo, unsigned int hi)
{
	long freed = 0;
	unsigned int i;

	for (i = lo; i < hi && freed < nr; i++) {
		struct dc_lru_shard *sh = &dc->lru[i];
		unsigned long scanned = 0, budget;

		budget = uatomic_load(&sh->count, CMM_RELAXED);
		while (freed < nr && scanned++ < budget) {
			struct urcu_txn_list_node *n;
			struct dentry *victim;

			rcu_read_lock();
			n = urcu_txn_list_next_rcu(&sh->list.node);
			if (n == &sh->list.node) {	/* empty */
				rcu_read_unlock();
				break;
			}
			victim = lru_dentry(n);
			if (victim->d_lru.referenced) {
				victim->d_lru.referenced = 0;	/* LRU_ROTATE */
				/* ONE move, not del + add: the node never leaves
				 * the list, so no traverser can be standing on
				 * a node that is about to be re-inserted. */
				(void) lru_move_tail(dc, victim, i);
				rcu_read_unlock();
				continue;
			}
			if (!children_empty(victim)) {
				/*
				 * IN USE.  The kernel answers LRU_REMOVED here
				 * and lets a later retain_dentry re-add it, and
				 * the LOCK arm does the same -- safely, because
				 * re-adding under the lock cannot race a
				 * traverser.
				 *
				 * THIS ARM MOVES IT INSTEAD, and the divergence
				 * is forced by the mechanism rather than chosen.
				 * A remove here is a GENUINE removal, so the
				 * re-add that follows is a genuine insert, and
				 * an insert rewrites `next` on a node a lockless
				 * traverser may still be standing on -- the one
				 * hazard a move exists to avoid.  Moving costs
				 * the same single commit and leaves nothing to
				 * re-arm: lru_retain()'s re-add path becomes
				 * reachable only after an allocation failure.
				 *
				 * The price is that an in-use entry keeps
				 * circulating instead of leaving the list, so it
				 * spends scan budget -- bounded per pass, and
				 * the same cost the referenced-bit rotate above
				 * already pays.
				 */
				(void) lru_move_tail(dc, victim, i);
				rcu_read_unlock();
				continue;
			}
			if (!lru_del_claimed(dc, victim)) {
				rcu_read_unlock();
				continue;	/* a peer took it */
			}
			if (lru_evict_settled(dc, victim) == 0)
				freed++;
			else
				lru_add_at(dc, victim, i);	/* THIS shard */
			rcu_read_unlock();
		}
	}
	return freed;
}
#else
/* Move @d to the tail of @sh (second chance).  Caller holds @sh. */
/* Link @d at the TAIL of @sh.  @d must be OFF the list.  Caller holds @sh. */
static void lru_link_tail_locked(struct dc_lru_shard *sh, struct dentry *d,
				 unsigned int idx)
{
	d->d_lru.prev = sh->tail;
	d->d_lru.next = NULL;
	if (sh->tail)
		sh->tail->d_lru.next = d;
	else
		sh->head = d;
	sh->tail = d;
	sh->count++;
	d->d_lru.shard = DC_LRU_ON(idx);
}

/*
 * LRU_ROTATE: move @d, which IS on the list, to the tail.  Unlink then link --
 * and keeping those two separable matters: the shrinker's isolate path has
 * ALREADY unlinked its victim, so re-adding it must use link_tail alone.
 * Calling rotate there instead unlinks a node that is off the list, which
 * decrements the count a second time (it underflowed to -78 within 300ms of
 * churn) and writes sh->head from the node's NULL next -- corrupting the list.
 */
static void lru_rotate_locked(struct dc_lru_shard *sh, struct dentry *d,
			      unsigned int idx)
{
	lru_unlink_locked(sh, d);
	lru_link_tail_locked(sh, d, idx);
}

/*
 * PHASE 3: the shrinker -- a CLOCK / second-chance pass, the kernel's
 * dentry_lru_isolate policy:
 *
 *   REFERENCED set  -> clear the bit and ROTATE to the tail (second chance)
 *   has children    -> rotate; it is not a candidate WHILE populated
 *   mid-rename      -> rotate; try again once the fold settles it
 *   otherwise       -> EVICT
 *
 * "Has children" stands in for the kernel's "in use": a cached child pins its
 * parent's refcount there, so a populated directory is never a candidate at all.
 * Stating it directly is not an optimisation -- evicting a populated directory
 * would orphan a live subtree, and the conservation census would be right to
 * call that corruption.  Mainline answers LRU_REMOVED for the in-use case
 * because a later last-put re-adds it; this port has no last-put, so removing it
 * would make it permanently un-evictable.  It rotates instead, and every
 * rotation is paid for out of a budget fixed at the start of the pass, so a
 * shard full of skips terminates rather than spinning.
 *
 * The victim is unlinked with the shard lock DROPPED: the unlink takes bucket
 * locks, and holding a shard lock across it would both invert against a
 * concurrent enqueue and stall the hot enqueue path for the length of an
 * unlink.  Isolate under the lock, release, then unlink -- mainline's
 * batch-isolate shape at batch size one.
 */
static long lru_shrink_range(struct dcache *dc, long nr,
			     unsigned int lo, unsigned int hi)
{
	long freed = 0;
	unsigned int i;

	for (i = lo; i < hi && freed < nr; i++) {
		struct dc_lru_shard *sh = &dc->lru[i];
		unsigned long scanned = 0, budget;

		budget = uatomic_load(&sh->count, CMM_RELAXED);
		while (freed < nr && scanned++ < budget) {
			struct dentry *victim;
			int rot;

			rcu_read_lock();
			lru_lock(sh);
			victim = sh->head;
			if (!victim) {
				lru_unlock(sh);
				rcu_read_unlock();
				break;
			}
			rot = 0;
			if (victim->d_lru.referenced) {
				victim->d_lru.referenced = 0;	/* second chance */
				rot = 1;
			}
			if (rot) {
				lru_rotate_locked(sh, victim, i);	/* LRU_ROTATE */
				lru_unlock(sh);
				rcu_read_unlock();
				continue;
			}
			if (!children_empty(victim)) {
				/* IN USE -> LRU_REMOVED, as the kernel does: a
				 * cached child pins its parent, so it is not a
				 * candidate at all.  Safe to drop rather than
				 * rotate because lru_retain re-arms it on the
				 * next touch -- and dropping is what stops
				 * un-evictable entries circulating forever. */
				lru_unlink_locked(sh, victim);
				lru_unlock(sh);
				rcu_read_unlock();
				continue;
			}
			lru_unlink_locked(sh, victim);	/* ISOLATE */
			lru_unlock(sh);
			if (lru_evict_settled(dc, victim) == 0) {
				freed++;
			} else {
				/* not evictable right now -- put it BACK, or it
				 * would be silently un-evictable forever */
				lru_lock(sh);
				lru_link_tail_locked(sh, victim, i);
				lru_unlock(sh);
			}
			rcu_read_unlock();
		}
	}
	return freed;
}

#endif	/* DC_LRU_MCAS */

/*
 * dc_shrink: sweep EVERY shard -- the pressure-driven shrinker's shape, where a
 * single consumer is responsible for the whole cache.
 *
 * dc_shrink_local: sweep only the CALLER'S OWN shard.  This is what an
 * evict-on-insert bounded cache wants, and the distinction is not cosmetic: a
 * continuous evictor running dc_shrink on every op makes every producer a
 * consumer of every OTHER producer's shard, which destroys exactly the
 * isolation sharding exists to provide.  Measured, that is not a slowdown but a
 * collapse -- on the MCAS arm the cross-shard collisions drove the transaction
 * front-end into its fair-mutex fallback and the benchmark stopped making
 * progress.  Evict where you allocate.
 */
long dc_shrink(struct dcache *dc, long nr)
{
	if (nr <= 0)
		return 0;
	return lru_shrink_range(dc, nr, 0, dc->nlru);
}

long dc_shrink_local(struct dcache *dc, long nr)
{
	unsigned int i;

	if (nr <= 0)
		return 0;
	i = lru_shard_index(dc);
	return lru_shrink_range(dc, nr, i, i + 1);
}

