/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * dcache_state.h -- the per-dentry LIFECYCLE state machine, debug-gated.
 *
 * ⭐⭐ WHAT THIS IS FOR.  The four terminal markers this cache already has
 * (the marked child head, the d_hash.next deletion mark, DC_LRU_DEAD, and the
 * deque poison) are each written INTO the slot that their own structure's
 * insert must CAS, in the SAME commit as the removal -- so within a structure
 * they are not racy and they cost zero extra conflict-set entries.  What none
 * of them witnesses is the rule that spans structures:
 *
 *	R5: a dentry must be OFF THE LRU before it is freed.
 *
 * That rule has been broken twice here and both times it was found the hard
 * way -- a re-add inside the unlink's own grace period, and fold() freeing
 * dentries that were still on the LRU.  Neither had a witness; both corrupted
 * silently.  This header gives R5 one.
 *
 * ⚠⚠ IT DOES NOT CACHE MEMBERSHIP.  dcache_lru.h says it outright: "DO NOT
 * cache this in a second word.  A separately-maintained membership record next
 * to a transacted one is precisely the defect the deque exists to remove."
 * So @d_lc holds LIFECYCLE only -- a monotone, once-or-twice-per-lifetime
 * property -- and every check below READS the real membership through
 * lru_listed() rather than mirroring it.  A second copy of a transacted fact
 * is the bug; a derived assertion about it is the check.
 *
 * ⚠ DEBUG-GATED ON PURPOSE, and this is step 1 of design/dcache-lifecycle-state.md:
 * the transition table is a CLAIM about what the code does, and it has to be
 * cross-checked against the markers before anything is allowed to depend on
 * it.  If the table is wrong, the assertions below are where that shows.
 * Nothing in the shipped build reads or writes @d_lc.
 *
 * The state is per LIFETIME, not per address: dentry_alloc() memsets, so a
 * recycled allocation restarts at NEW.  Monotone increasing, so there is no
 * ABA -- the same argument DC_MARK_GEN already relies on.
 *
 *	NEW --publish--> LIVE --unhash/seal--> DYING --off everything--> DEAD
 */

#ifndef _DCACHE_STATE_H
#define _DCACHE_STATE_H

#define DC_LC_NEW	0u	/* memset() lands here: allocated, in nothing */
#define DC_LC_LIVE	1u	/* named: in the hash bucket and the sib list */
#define DC_LC_DYING	2u	/* unhashed and/or sealed; reclaim not yet queued */
#define DC_LC_DEAD	3u	/* off every index AND off the LRU; call_rcu owed */

#define DC_LC_M(s)	(1u << (s))

#ifdef DC_LIFECYCLE_STATE

#include <stdio.h>
#include <stdlib.h>

/*
 * ⛔⭐⭐ MEMBERSHIP, NOT lru_listed().  The first version of this check asked
 * lru_listed() and fired instantly on a healthy tree.  That was the CHECK being
 * wrong, not the code: lru_listed() deliberately answers "queued OR SEALED",
 * because both mean "do not re-arm" and that is what lru_retain wants -- so a
 * node correctly removed AND sealed still answers yes.  dcache_lru.h says it in
 * as many words ("anything asking the MEMBERSHIP question must use
 * urcu_txn_deque_queued()"), and rcu-txn-deque.h repeats it.
 *
 * R5 is a membership question -- "is a shard still pointing at this node" --
 * so it must ask the form that maps POISON to NULL.  Getting this wrong in the
 * SAFE direction (over-reporting) is why it was caught in one run; getting it
 * wrong the other way would have made the check silently vacuous.
 */
#ifdef DC_NO_LRU
#define DC_LC_QUEUED(d)		0	/* no LRU built: R5 is vacuous */
#elif defined(DC_LRU_MCAS)
#define DC_LC_QUEUED(d)		(urcu_txn_deque_queued(&(d)->d_lru.dnode) != NULL)
#else
#define DC_LC_QUEUED(d)							\
	DC_LRU_IS_LINKED(uatomic_load(&(d)->d_lru.shard, CMM_RELAXED))
#endif

static const char *dc_lc_name(unsigned int s)
{
	switch (s) {
	case DC_LC_NEW:		return "NEW";
	case DC_LC_LIVE:	return "LIVE";
	case DC_LC_DYING:	return "DYING";
	case DC_LC_DEAD:	return "DEAD";
	default:		return "GARBAGE";
	}
}

static void dc_lc_die(const struct dentry *d, const char *what,
		      unsigned int have, const char *site)
{
	fprintf(stderr,
		"\nLIFECYCLE VIOLATION: %s\n"
		"  dentry %p  state=%s(%u)  site=%s\n",
		what, (const void *) d, dc_lc_name(have), have, site);
	fflush(stderr);
	abort();
}

/*
 * A transition, with the legal predecessors named.  @from_mask is a bitmask of
 * (1u << state) so an illegal edge -- DYING -> LIVE is the re-add race -- traps
 * at the edge instead of surfacing later as a use-after-free.
 */
static inline void dc_lc_set(struct dentry *d, unsigned int from_mask,
			     unsigned int to, const char *site)
{
	unsigned int have = uatomic_load(&d->d_lc, CMM_RELAXED);

	if (!((1u << have) & from_mask))
		dc_lc_die(d, "illegal transition", have, site);
	uatomic_store(&d->d_lc, to, CMM_RELAXED);
}

static inline void dc_lc_assert(const struct dentry *d, unsigned int ok_mask,
				const char *site)
{
	unsigned int have = uatomic_load(&d->d_lc, CMM_RELAXED);

	if (!((1u << have) & ok_mask))
		dc_lc_die(d, "unexpected state", have, site);
}

/*
 * ⛔⭐⭐ R5 IS ALREADY WITNESSED -- I was wrong that it was not, and this is the
 * correction that matters most here.  dentry_free_cb() already calls
 * lru_assert_not_queued() under -DDC_LRU_FREE_ASSERT, which is the SAME check
 * this header set out to add, in the right place (post-grace-period) and
 * already wired into check-lru-arms.
 *
 * ⚠ And "before call_rcu" -- where this file first put the check -- is the
 * WRONG place, not merely a redundant one.  The shrinker deliberately queues
 * the reclaim FIRST and drops LRU ownership after, both inside one read-side
 * section, "which is the only reason the free cannot have landed before this
 * store" (dcache_lru_shrink.h).  So the rule is "off the LRU before the GRACE
 * PERIOD completes", not before the reclaim is queued, and a check at the
 * call_rcu site fires on a perfectly healthy tree.  It did, on run 1.
 *
 * What is left for this header is therefore NOT membership -- that is covered
 * twice over -- but the thing no single marker sees: the LIFECYCLE ITSELF, and
 * in particular illegal EDGES (a resurrection, DYING -> LIVE) and reclaim of a
 * dentry that never passed through the states at all.
 */
/*
 * ⚠ -DDC_LC_SELFTEST is the MUST-FAIL control.  It drops the transition, so a
 * reclaimed dentry reaches dentry_free_cb() still LIVE and the assertion there
 * must abort.  Without it a clean run proves nothing: it is equally consistent
 * with "no violations" and "the assertions never executed".
 */
#ifdef DC_LC_SELFTEST
#define DC_LC_TO_DEAD(d)		do { (void) (d); } while (0)
#else
#define DC_LC_TO_DEAD(d)						\
	DC_LC_SET((d), DC_LC_M(DC_LC_NEW) | DC_LC_M(DC_LC_LIVE) |	\
		       DC_LC_M(DC_LC_DYING), DC_LC_DEAD)
#endif

#define DC_LC_SET(d, from_mask, to)	dc_lc_set((d), (from_mask), (to), __func__)
#define DC_LC_ASSERT(d, ok_mask)	dc_lc_assert((d), (ok_mask), __func__)

#else	/* !DC_LIFECYCLE_STATE */

#define DC_LC_SET(d, from_mask, to)	do { } while (0)
#define DC_LC_ASSERT(d, ok_mask)	do { } while (0)
#define DC_LC_TO_DEAD(d)		do { } while (0)

#endif	/* DC_LIFECYCLE_STATE */

#endif	/* _DCACHE_STATE_H */
