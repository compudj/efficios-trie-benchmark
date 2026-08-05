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
 *   REFERENCED   -> clear the bit, rotate head to tail (LRU_ROTATE)
 *   has children -> rotate (see below: this arm diverges from the kernel here)
 *   otherwise    -> evict, then remove
 *
 * There is no isolate/drop-the-lock dance here because there is no lock to drop:
 * the remove IS the isolation, and it is atomic on all its edges.  That is the
 * whole difference this arm exists to price -- the shrinker never shares a lock
 * with dentry ops, at the cost of a descriptor and a multi-CAS on every enqueue
 * and every unlink.
 *
 * THE HEAD IS A HINT, RE-READ EVERY ITERATION, and that is what keeps the deque's
 * no-traversal contract true: this loop never steps from one node to its
 * successor.  It also means the rotate below rotates whatever is at the head
 * NOW, which may no longer be @victim if a peer got in between -- acceptable for
 * a CLOCK, whose order is fuzzy on purpose, and the referenced bit we cleared
 * belongs to @victim either way.
 *
 * TWO SWEEPERS CAN REACH lru_evict_settled() FOR ONE VICTIM, because there is no
 * claim any more.  That is safe, but it MOVES THE SERIALIZATION POINT from the
 * LRU word to the eviction itself, so it is stated rather than discovered: both
 * engines' lru_evict_settled re-verifies under the bucket lock (bucketlock: the
 * hlist mark; txn: hlist_del_prepare answering -ENOENT, and two commits on the
 * same slot where one must abort), so exactly one caller reaches call_rcu and
 * the other answers -EAGAIN.  The loser then rotates, which is a no-op on a
 * node the winner already removed.
 */
static long lru_shrink_range(struct dcache *dc, long nr,
			     unsigned int lo, unsigned int hi)
{
	long freed = 0;
	unsigned int i;

	for (i = lo; i < hi && freed < nr; i++) {
		struct urcu_txn_deque *q = &dc->lru[i].deque;
		unsigned long scanned = 0, budget;

		budget = uatomic_load(&q->count, CMM_RELAXED);
		while (freed < nr && scanned++ < budget) {
			struct urcu_txn_deque_node *n;
			struct dentry *victim;

			rcu_read_lock();
			n = urcu_txn_deque_head(q);
			if (!n) {			/* empty */
				rcu_read_unlock();
				break;
			}
			victim = lru_dentry(n);
			if (victim->d_lru.referenced) {
				victim->d_lru.referenced = 0;	/* LRU_ROTATE */
				(void) lru_dq_rotate(dc, q);
				rcu_read_unlock();
				continue;
			}
			if (!children_empty(victim)) {
				/*
				 * IN USE.  The kernel answers LRU_REMOVED here
				 * and lets a later retain_dentry re-add it, and
				 * the LOCK arm does the same.
				 *
				 * THIS ARM ROTATES INSTEAD.  Not because a
				 * removal would be unsafe -- with the deque it
				 * would be -- but because rotating costs the
				 * same single commit and leaves nothing to
				 * re-arm, so lru_retain()'s re-add path stays
				 * cold.  The price is that an in-use entry keeps
				 * circulating instead of leaving the deque, so
				 * it spends scan budget: bounded per pass, and
				 * the same cost the referenced-bit rotate above
				 * already pays.  dc_lru_inuse_is_removed tells
				 * test_dcache.c which of the two it is looking
				 * at.
				 */
				(void) lru_dq_rotate(dc, q);
				rcu_read_unlock();
				continue;
			}
#ifndef DC_LRU_READD_LEGACY
			/*
			 * EVICT BEFORE UNLINKING.  lru_evict_settled() returns
			 * -EAGAIN without having mutated anything, so it is safe
			 * to attempt while @victim is still queued, and the deque
			 * is left alone until the eviction has actually
			 * committed.  A failed attempt therefore has nothing to
			 * put back: the same-grace-period re-add disappears by
			 * construction rather than by timing, which matters
			 * because a real grace period is not open to us here --
			 * the shrinker holds its victim by RCU alone, so leaving
			 * the read-side section to wait is exactly what would let
			 * dentry_free_cb reclaim the node it means to re-add.
			 *
			 * The remove AFTER the eviction is also why this whole
			 * loop must stay inside rcu_read_lock(): lru_evict_settled
			 * has already call_rcu'd the dentry, and only the
			 * read-side section keeps that free from landing before
			 * the deque stops pointing at it.
			 *
			 * On failure, ROTATE rather than leave it at the head, or
			 * the sweeper re-examines the same unevictable victim for
			 * the whole budget.
			 *
			 * -DDC_LRU_READD_LEGACY restores the unlink-then-maybe-
			 * re-add shape below.  On the list that shape live-locked;
			 * it is kept as the A/B control that says whether the
			 * deque removed the cause or merely the symptom.
			 */
			if (lru_evict_settled(dc, victim) == 0) {
				lru_del(dc, victim);
				freed++;
			} else {
				(void) lru_dq_rotate(dc, q);
			}
			rcu_read_unlock();
			continue;
#else
			if (!lru_try_del(dc, victim)) {
				rcu_read_unlock();
				continue;	/* a peer took it */
			}
			if (lru_evict_settled(dc, victim) == 0)
				freed++;
#if defined(DC_LRU_NO_READD) || defined(DC_LRU_NO_SHRINK_READD)
			/*
			 * PROBE (-DDC_LRU_NO_SHRINK_READD): drop the SAME-GRACE-
			 * PERIOD re-add.  The unlink above and this re-add sit in
			 * one read-side critical section, so no grace period can
			 * separate them.  Leaving the victim off the deque costs
			 * LRU accuracy and nothing else.
			 *
			 * ⚠ -DDC_LRU_NO_READD kills lru_retain's re-arm TOO, so
			 * it cannot tell the two re-adds apart.  Use the split
			 * spellings; see lru_retain().
			 */
#else
			else
				lru_add_at(dc, victim, i);	/* THIS shard */
#endif
			rcu_read_unlock();
#endif	/* DC_LRU_READD_LEGACY */
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
	/* SHRINK(i) -> ON(i) (the put-back) or ON(i) -> ON(i) (a rotate).  Never
	 * from OFF: an adder's claim owns that transition, and it cannot be
	 * racing us here precisely because the word is not OFF. */
	uatomic_store(&d->d_lru.shard, DC_LRU_ON(idx), CMM_RELAXED);
}

/*
 * ISOLATE WITHOUT DISOWNING -- mainline's d_lru_shrink_move().
 *
 * @d comes OFF the shard's list (so no other sweeper and no lru_del can reach
 * it through the links) while shard @idx keeps owning it.  That single fact is
 * what the whole handoff rests on:
 *
 *   lru_retain sees OWNED and does not re-arm it            (DCACHE_LRU_LIST)
 *   lru_del_can_free sees SHRINK and delegates the free     (DCACHE_SHRINK_LIST)
 *
 * so the put-back below can never link a dentry that is already queued for
 * reclaim.  Isolating by UNLINKING -- which is what this did before -- gives up
 * both, and -DDC_LRU_FREE_ASSERT fired 5/5 on both cadences as a result.
 *
 * -DDC_LRU_NO_SHRINK_OWN restores the disowning isolate.  It is the MUTATION
 * TEST for the guard: if removing the ownership changes nothing, the ownership
 * is not what fixed it and this comment is a story.
 */
static void lru_shrink_move_locked(struct dc_lru_shard *sh, struct dentry *d,
				   unsigned int idx)
{
	/* ⚠ ONE store to the word -- unlinking to OFF and then overwriting with
	 * SHRINK would expose a transient OFF to an adder holding a DIFFERENT
	 * shard's lock, which would claim @d and link it somewhere we are not
	 * looking.  See lru_unlink_locked_to(). */
#ifndef DC_LRU_NO_SHRINK_OWN
	lru_unlink_locked_to(sh, d, DC_LRU_SHRINK(idx));
#else
	(void) idx;
	lru_unlink_locked(sh, d);
#endif
}

/*
 * Give up ownership of a victim that is about to be freed -- either we unhashed
 * it ourselves (lru_evict_settled committed) or a killer did and handed us the
 * debt.  Either way @d is already queued for reclaim.
 *
 * ⚠ IT SEALS, it does not merely release.  DC_LRU_OFF here would be an open
 * invitation: lru_retain on any other thread would find the dentry unowned,
 * win the claim, and enqueue it on ITS shard while call_rcu is already pending.
 * The "still hashed" test in lru_add cannot be relied on to catch that -- it
 * reads a bucket-locked slot without the bucket lock.  DC_LRU_DEAD is terminal
 * for the dentry's lifetime, and a recycled allocation starts from zero
 * (memset in dentry_alloc), i.e. DC_LRU_OFF again.
 */
static void lru_shrink_release_locked(struct dc_lru_shard *sh, struct dentry *d)
{
	(void) sh;
	uatomic_store(&d->d_lru.shard, DC_LRU_DEAD, CMM_RELAXED);
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
	/* ⚠ The word must NOT dip to OFF between the two halves.  A rotate
	 * holds shard i's lock, but an adder claims on the CALLER'S shard, so a
	 * transient OFF here lets it win the claim and splice @d into shard j
	 * while we are re-linking it into shard i -- one node, two lists. */
	lru_unlink_locked_to(sh, d, DC_LRU_ON(idx));
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
			/* ISOLATE, KEEPING OWNERSHIP.  See lru_shrink_move_locked:
			 * the victim leaves the list but shard i still owns it,
			 * which is what makes every branch below safe. */
			lru_shrink_move_locked(sh, victim, i);
			lru_unlock(sh);
			if (lru_evict_settled(dc, victim) == 0) {
				/*
				 * WE unhashed it and call_rcu'd it, so drop
				 * ownership -- still inside this iteration's
				 * read-side section, which is the only reason
				 * the free cannot have landed before this
				 * store.  No killer can be racing us: dc_unlink
				 * re-verifies "still hashed" under the bucket
				 * lock, and we just cleared that, so it re-finds
				 * and answers -ENOENT rather than reaching
				 * lru_del_can_free at all.
				 */
				lru_lock(sh);
				lru_shrink_release_locked(sh, victim);
				lru_unlock(sh);
				freed++;
				rcu_read_unlock();
				continue;
			}
			lru_lock(sh);
			if (victim->d_lru.shard & DC_LRU_KILL_BIT) {
				/*
				 * THE HANDOFF FIRED.  A concurrent dc_unlink
				 * removed @victim from both indexes while we
				 * held it and declined to free it, because we
				 * did.  This is mainline's shrink_dentry_list
				 * completing a kill that __dentry_kill left at
				 * can_free = false.
				 *
				 * Counted as freed: the eviction happened, it
				 * just was not us who unhashed it.
				 */
				lru_shrink_release_locked(sh, victim);
				lru_unlock(sh);
				call_rcu(&victim->d_rcu, dentry_free_cb);
				freed++;
				rcu_read_unlock();
				continue;
			}
			/*
			 * ⛔ TRIED AND MEASURED TO CHANGE NOTHING: refusing the
			 * put-back when @victim is no longer hashed -- i.e.
			 * retain_dentry's d_unhashed test applied to the
			 * shrinker's own re-add, which has no liveness test at
			 * all.  It was written to close the stale-d_parent
			 * use-after-free below, on the theory that the UAF came
			 * from a detached dentry being relinked.
			 *
			 * TSAN, --evict continuous, 8 runs each: without it
			 * 4/8, with it 3/8 -- noise.  The actual cause was an
			 * ADD publishing a child under an already-evicted
			 * parent (see the parent re-check in dc_add), and with
			 * that fixed the guard's own mutation arm measured
			 * 0/8 either way.  Not kept: a guard whose removal
			 * changes nothing is not a fix, and this one carried a
			 * comment asserting a mechanism the measurement
			 * refuted.
			 */
#if defined(DC_LRU_NO_READD) || defined(DC_LRU_NO_SHRINK_READD)
			/*
			 * PROBE (-DDC_LRU_NO_SHRINK_READD): drop the put-back and
			 * leave the victim off the LRU.  It costs eviction
			 * accuracy -- an entry that failed once is un-evictable
			 * until its next touch re-arms it -- and nothing else.
			 *
			 * ⚠ -DDC_LRU_NO_READD kills lru_retain's re-arm TOO, so
			 * it cannot tell the two re-adds apart.  Use the split
			 * spellings; see lru_retain().
			 *
			 * With the shrink-list handoff in place this is expected
			 * to be a NO-OP for -DDC_LRU_FREE_ASSERT, and that is
			 * the point of keeping it: it says the fix is the
			 * OWNERSHIP, not the removal of the put-back.
			 *
			 * OFF, not DEAD: @victim survived the eviction attempt
			 * and must stay re-armable.
			 */
			uatomic_store(&victim->d_lru.shard, DC_LRU_OFF,
				      CMM_RELAXED);
			lru_unlock(sh);
#else
			/* not evictable right now -- put it BACK, or it would be
			 * silently un-evictable forever.  Safe now, and only
			 * now: @victim was never ownerless, so no killer can
			 * have freed it behind us. */
			lru_link_tail_locked(sh, victim, i);
			lru_unlock(sh);
#endif
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

