/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * dcache_txn_stats.h -- per-call-site transaction instrumentation (-DDC_TXN_STATS).
 *
 * WHY THIS EXISTS.  Chasing why --evict bursty wedges on the MCAS LRU arm cost
 * five hypotheses, every one of them plausible and every one falsified by the
 * next experiment (see REVIEW.md).  Guessing which commit is starving is what
 * failed; this measures it.
 *
 * WHAT IT ANSWERS.  For each commit site, separately: how many attempts, how
 * many contention ABORTs, how deep the aging got, and -- the one that matters
 * -- how often the site ENTERED THE FALLBACK LANE.  Escalation is a domain-wide
 * event, so once one site starts escalating every site's begin() pays for it;
 * only per-site attribution can tell the initiator from the victims.
 *
 * OFF BY DEFAULT, and it must stay that way: the counters are cheap but they
 * are not free, and the wedge is timing-sensitive enough that a heavier commit
 * path could hide it.  That is also why the counters are THREAD-LOCAL rather
 * than shared atomics -- a shared counter per commit would add exactly the kind
 * of cross-core traffic the thing under investigation is made of, and would
 * measure the instrument instead of the engine.
 */

#ifndef DCACHE_TXN_STATS_H
#define DCACHE_TXN_STATS_H

#ifdef DC_TXN_STATS

#include <stdio.h>
#include <string.h>
#include <urcu/uatomic.h>

enum dc_ts_site {
	DC_TS_ADD,		/* dc_add / dc_add_typed index publish */
	DC_TS_UNLINK,		/* dc_unlink index removal */
	DC_TS_STATE,		/* d_instantiate / d_delete pos-neg flip */
	DC_TS_STACK,		/* rename: stack_shell */
	DC_TS_FOLD,		/* the fold's TRANSFER / SPLICE / RECLAIM */
	DC_TS_XCHG,		/* exchange */
	DC_TS_LRU_ADD,		/* LRU enqueue (MCAS arm) */
	DC_TS_LRU_DEL,		/* LRU removal (MCAS arm) */
	DC_TS_LRU_EVICT,	/* shrinker's index removal */
	DC_TS_NR
};

struct dc_ts_row {
	unsigned long attempts;		/* commit attempts */
	unsigned long aborts;		/* URCU_TXN_STATUS_ABORT */
	unsigned long escalations;	/* attempts that held the fallback lane */
	unsigned long published;	/* attempts that RAISED domain->active */
	unsigned long eagain;		/* prepare returned -EAGAIN (successor
					 * mid-delete) -- a RETRY, not a conflict */
	unsigned long casfail[6];	/* failing install CAS, by RECORD INDEX --
					 * which EDGE of the committed structure
					 * lost, which localises a self-conflict
					 * (always the same index) apart from a
					 * live competitor (spread) */
	/*
	 * Outcome of a list del, which is what separates the two candidate
	 * causes of a guard that never validates:
	 *   ok    we unlinked it -- re-adding it is correct
	 *   peer  a peer had already unlinked it -- re-adding is correct too
	 *   fail  NOT unlinked and STILL LINKED.  Re-adding after this links the
	 *         node a SECOND time, and a doubly-linked node's neighbours can
	 *         never validate their guards again -- absorbing by construction.
	 * relink_after_fail counts exactly that mistake being made.
	 */
	unsigned long del_ok, del_peer, del_fail, relink_after_fail;
	/*
	 * What the slot actually held when the CAS lost, versus the expected
	 * old.  This is the classification that separates the causes:
	 *   same    seen == old.  The CAS "failed" against the value it wanted
	 *           -- ABA, or a spurious failure.  Nothing else can explain it.
	 *   marked  a deletion tombstone (bit 1) -- the neighbour really is
	 *           marked-but-linked, i.e. mode A leaking into mode B.
	 *   proxy   another descriptor (bit 0) is parked on the slot -- a live
	 *           competitor after all.
	 *   other   a plain, different pointer -- the slot moved on.
	 */
	unsigned long seen_same, seen_marked, seen_proxy, seen_other;
	/*
	 * When the losing CAS saw a PROXY, what state is the descriptor behind
	 * it in?  Under -DDC_LRU_MCAS_LOCKED no concurrent mutator exists, so a
	 * proxy in a slot cannot be work in flight -- it is a LEAK, and its
	 * status says which kind: a FAILED or SUCCEEDED descriptor still parked
	 * means settle did not clean up after itself.
	 */
	unsigned long proxy_undecided, proxy_succeeded, proxy_failed;
	unsigned long ins_cleared_mark;	/* insert ABORTED after its plain store had
					 * already cleared the node's tombstone --
					 * leaves it unmarked AND unlinked */
	unsigned long ins_edge_bad;	/* insert committed OK but the tail edge
					 * does not name it */
	unsigned long audit_no_mark;	/* commit OK but the victim is NOT marked */
	unsigned long audit_still_linked;/* commit OK but prev->next STILL names it */
	unsigned long claimed_unlinked;	/* shard word says ON, but the node is NOT
					 * reachable in that shard's list */
	unsigned long audit_back_stale;	/* commit OK but next->prev STILL names it --
					 * the BACKWARD unlink did not take */
	unsigned long stale_unmarked;	/* memory says MARKED, prepare proceeded */
	unsigned long marked_seen;	/* memory says MARKED, prepare agreed */
	unsigned long poison_set;	/* two records on one slot that do not
					 * chain -- the SILENT merge del_prepare
					 * warns about */
	unsigned long begins;		/* urcu_txn_begin() calls that RETURNED */
	unsigned long begin_in_lane;	/* ... of those, already holding the lane */
	unsigned long begin_funnel_on;	/* ... of those, with domain->active set */
	unsigned long poison;		/* the ABORT was a POISONED descriptor, not
					 * contention: a mis-built expected-old that
					 * can never match, so every retry aborts
					 * forever.  A bug signature, not a load
					 * signature. */
	unsigned long max_retry;	/* deepest aging seen */
	unsigned long pad[3];
};

#define DC_TS_MAX_THREADS	512

/*
 * One row block per thread, so the fast path is a thread-local increment with
 * no shared line.  Slots are handed out on first use and never returned -- this
 * is a diagnostic build, and recycling would need a registry the engines do not
 * otherwise have.
 */
extern struct dc_ts_row dc_ts_tab[DC_TS_MAX_THREADS][DC_TS_NR];
extern unsigned long dc_ts_next_slot;
extern __thread int dc_ts_slot;
extern __thread int dc_ts_cur_site;
/* NOT thread-local: the signal handler that prints these runs on whichever
 * thread takes the signal, which is not the one that lost the CAS.  A racy
 * last-writer-wins snapshot is exactly right for a diagnostic. */
extern void *dc_ts_last_slot;
extern void *dc_ts_last_old;
extern void *dc_ts_last_seen;


static inline struct dc_ts_row *dc_ts_row(enum dc_ts_site s)
{
	if (caa_unlikely(dc_ts_slot < 0)) {
		unsigned long i = uatomic_add_return(&dc_ts_next_slot, 1) - 1;

		dc_ts_slot = (int) (i < DC_TS_MAX_THREADS ? i
						         : DC_TS_MAX_THREADS - 1);
	}
	return &dc_ts_tab[dc_ts_slot][s];
}

/*
 * Record one commit attempt.  @txn must still be live (before urcu_txn_end),
 * because in_fallback and retry are cleared by end().
 */
static inline void dc_ts_commit(enum dc_ts_site s, struct urcu_txn *txn,
				enum urcu_txn_status st)
{
	struct dc_ts_row *r = dc_ts_row(s);

	r->attempts++;
	if (st == URCU_TXN_STATUS_ABORT) {
		r->aborts++;
		if (urcu_txn_abort_was_poison(txn))
			r->poison++;
	}
	if (txn->in_fallback)
		r->escalations++;
	if (txn->fb_published)
		r->published++;
	if (txn->retry > r->max_retry)
		r->max_retry = txn->retry;
}

/*
 * Record that a begin() RETURNED.  Counting the funnel at commit time is not
 * enough and was actively misleading: a transaction that funnels PARKS inside
 * begin(), so until it is granted the lane it reaches no commit and shows up as
 * "never escalated" -- which is how a first reading of these counters concluded
 * the fast path was ignoring domain->active when it was in fact blocked in it.
 *
 * begin_funnel_on samples domain->active as this attempt saw it, so "the funnel
 * was on and I still did not enter" is distinguishable from "the funnel was off".
 *
 * ⚠ STILL POST-PARK, and read the numbers accordingly.  This runs after begin()
 * RETURNS, so a thread currently blocked in cds_fair_mutex_park() inside begin()
 * is counted nowhere at all.  A site showing "begins N, in-lane 0, funnel-on 0"
 * therefore does NOT mean it ignored the funnel -- it can equally mean its
 * threads are parked in the funnel right now and will be counted only if they
 * are ever granted the lane.  Confirmed by gdb: with lru_add reporting
 * funnel-on 0, both writer threads were sitting in cds_fair_mutex_park() inside
 * urcu_txn__enter_fallback() from lru_add.  Counting the DECISION rather than
 * its outcome needs a hook inside begin(), i.e. in liburcu.
 */
static inline void dc_ts_begin(enum dc_ts_site s, const struct urcu_txn *txn)
{
	struct dc_ts_row *r = dc_ts_row(s);

	dc_ts_cur_site = (int) s;
	r->begins++;
	if (txn->in_fallback)
		r->begin_in_lane++;
	if (txn->domain && uatomic_load(&txn->domain->active, CMM_RELAXED))
		r->begin_funnel_on++;
}

#define DC_TS_COMMIT(site, txnp, st)	dc_ts_commit((site), (txnp), (st))
#define DC_TS_BEGIN(site, txnp)		dc_ts_begin((site), (txnp))

static inline void dc_ts_del_ret(enum dc_ts_site s, int ret)
{
	struct dc_ts_row *r = dc_ts_row(s);

	if (ret == 1)
		r->del_ok++;
	else if (ret == 0)
		r->del_peer++;
	else
		r->del_fail++;
}

static inline void dc_ts_relink_after_fail(enum dc_ts_site s)
{
	dc_ts_row(s)->relink_after_fail++;
}

static inline void dc_ts_poison_set(void *slot, void *want, void *got)
{
	if (dc_ts_cur_site < 0)
		return;
	dc_ts_row((enum dc_ts_site) dc_ts_cur_site)->poison_set++;
	uatomic_store(&dc_ts_last_slot, slot, CMM_RELAXED);
	uatomic_store(&dc_ts_last_old, want, CMM_RELAXED);
	uatomic_store(&dc_ts_last_seen, got, CMM_RELAXED);
}

static inline void dc_ts_delaudit(enum dc_ts_site s, int no_mark, int linked)
{
	struct dc_ts_row *r = dc_ts_row(s);

	if (no_mark)
		r->audit_no_mark++;
	if (linked)
		r->audit_still_linked++;
}

#define DC_TS_UNLINKED(site)	(dc_ts_row(site)->claimed_unlinked++)
#define DC_TS_BACKEDGE(site)	(dc_ts_row(site)->audit_back_stale++)
#define DC_TS_INSCLEAR(site)	(dc_ts_row(site)->ins_cleared_mark++)
#define DC_TS_INSEDGE(site)	(dc_ts_row(site)->ins_edge_bad++)
#define DC_TS_DELAUDIT(site, nm, lk)	dc_ts_delaudit((site), (nm), (lk))
#define DC_TS_STALE(site)	(dc_ts_row(site)->stale_unmarked++)
#define DC_TS_MARKEDSEEN(site)	(dc_ts_row(site)->marked_seen++)
#define DC_TS_DEL_RET(site, ret)	dc_ts_del_ret((site), (ret))
#define DC_TS_RELINK_BAD(site)		dc_ts_relink_after_fail(site)

/*
 * The failing install CAS, routed here from liburcu's URCU_TXN_CAS_FAIL hook.
 * Attributed to whichever site is CURRENTLY committing on this thread -- the
 * hook fires deep inside the engine, which does not know the caller, so the
 * site is stashed at begin() and read back here.
 */

static inline void dc_ts_cas_fail(unsigned int idx, void *slot, void *old,
				  void *seen)
{
	struct dc_ts_row *r;

	if (dc_ts_cur_site < 0)
		return;
	r = dc_ts_row((enum dc_ts_site) dc_ts_cur_site);
	r->casfail[idx < 6 ? idx : 5]++;
	if (seen == old)
		r->seen_same++;
	else if ((uintptr_t) seen & 0x2UL)	/* list deletion MARK */
		r->seen_marked++;
	else if ((uintptr_t) seen & 0x1UL) {	/* engine proxy tag */
		struct urcu_txn_record *fr = urcu_txn_untag(seen, URCU_TXN_TAG);
		unsigned long st_desc = fr && fr->desc
			? urcu_txn_desc_status(fr->desc) : ~0UL;

		r->seen_proxy++;
		if (st_desc == URCU_TXN_DESC_UNDECIDED)
			r->proxy_undecided++;
		else if (st_desc == URCU_TXN_DESC_SUCCEEDED)
			r->proxy_succeeded++;
		else if (st_desc == URCU_TXN_DESC_FAILED)
			r->proxy_failed++;
	}
	else
		r->seen_other++;
	uatomic_store(&dc_ts_last_slot, slot, CMM_RELAXED);
	uatomic_store(&dc_ts_last_old, old, CMM_RELAXED);
	uatomic_store(&dc_ts_last_seen, seen, CMM_RELAXED);
}
#define DC_TS_EAGAIN(site)		(dc_ts_row(site)->eagain++)

/* dcache.h declares this as void dc_txn_stats_dump(void *stream); the void*
 * keeps <stdio.h> out of the engine-agnostic interface. */

#ifdef DC_TXN_STATS_IMPL
struct dc_ts_row dc_ts_tab[DC_TS_MAX_THREADS][DC_TS_NR];
unsigned long dc_ts_next_slot;
__thread int dc_ts_slot = -1;
__thread int dc_ts_cur_site = -1;
void *dc_ts_last_slot;
void *dc_ts_last_old;
void *dc_ts_last_seen;

/* The most recent losing CAS, raw.  One concrete triple is worth more than a
 * histogram when the question is "what is actually in that slot". */
void dc_txn_stats_last(void *stream)
{
	FILE *f = stream;

	if (!dc_ts_last_slot) {
		fprintf(f, "TXNSTATS last-cas-loss: none recorded\n");
		return;
	}
	fprintf(f, "TXNSTATS last-cas-loss slot=%p old=%p seen=%p%s%s%s\n",
		dc_ts_last_slot, dc_ts_last_old, dc_ts_last_seen,
		dc_ts_last_seen == dc_ts_last_old ? "  (SAME AS OLD)" : "",
		((uintptr_t) dc_ts_last_seen & 0x2UL) ? "  (MARKED)" : "",
		((uintptr_t) dc_ts_last_seen & 0x1UL) ? "  (PROXY)" : "");
}

void dc_txn_stats_dump(void *stream)
{
	FILE *f = stream;
	static const char *const name[DC_TS_NR] = {
		"add", "unlink", "state", "stack", "fold", "exchange",
		"lru_add", "lru_del", "lru_evict"
	};
	unsigned long nthr = uatomic_load(&dc_ts_next_slot, CMM_RELAXED);
	unsigned long t, i, k;

	if (nthr > DC_TS_MAX_THREADS)
		nthr = DC_TS_MAX_THREADS;
	fprintf(f, "TXNSTATS %-9s %12s %12s %12s %12s %9s %12s\n",
		"site", "attempts", "aborts", "escalated", "published", "maxretry",
		"eagain/poison");
	(void) 0;
	for (i = 0; i < DC_TS_NR; i++) {
		struct dc_ts_row a;

		memset(&a, 0, sizeof(a));
		for (t = 0; t < nthr; t++) {
			struct dc_ts_row *r = &dc_ts_tab[t][i];

			a.attempts += r->attempts;
			a.aborts += r->aborts;
			a.escalations += r->escalations;
			a.published += r->published;
			a.eagain += r->eagain;
			a.poison += r->poison;
			a.begins += r->begins;
			a.begin_in_lane += r->begin_in_lane;
			a.begin_funnel_on += r->begin_funnel_on;
			for (k = 0; k < 6; k++)
				a.casfail[k] += r->casfail[k];
			a.del_ok += r->del_ok;
			a.del_peer += r->del_peer;
			a.del_fail += r->del_fail;
			a.relink_after_fail += r->relink_after_fail;
			a.seen_same += r->seen_same;
			a.seen_marked += r->seen_marked;
			a.seen_proxy += r->seen_proxy;
			a.seen_other += r->seen_other;
			a.proxy_undecided += r->proxy_undecided;
			a.proxy_succeeded += r->proxy_succeeded;
			a.proxy_failed += r->proxy_failed;
			a.poison_set += r->poison_set;
			a.stale_unmarked += r->stale_unmarked;
			a.audit_no_mark += r->audit_no_mark;
			a.ins_cleared_mark += r->ins_cleared_mark;
			a.ins_edge_bad += r->ins_edge_bad;
			a.audit_still_linked += r->audit_still_linked;
			a.audit_back_stale += r->audit_back_stale;
			a.claimed_unlinked += r->claimed_unlinked;
			a.marked_seen += r->marked_seen;
			if (r->max_retry > a.max_retry)
				a.max_retry = r->max_retry;
		}
		if (!a.attempts)
			continue;
		fprintf(f, "TXNSTATS %-9s %12lu %12lu %12lu %12lu %9lu %12lu\n",
			name[i], a.attempts, a.aborts, a.escalations,
			a.published, a.max_retry, a.eagain);
		fprintf(f, "TXNSTATS %-9s begins %lu  in-lane %lu  funnel-on %lu  "
			"poisoned %lu\n", name[i], a.begins, a.begin_in_lane,
			a.begin_funnel_on, a.poison);
		if (a.del_ok || a.del_peer || a.del_fail || a.relink_after_fail)
			fprintf(f, "TXNSTATS %-9s del ok %lu  peer %lu  "
				"FAIL(still-linked) %lu  RELINK-AFTER-FAIL %lu\n",
				name[i], a.del_ok, a.del_peer, a.del_fail,
				a.relink_after_fail);
		fprintf(f, "TXNSTATS %-9s cas-fail by record idx:", name[i]);
		for (k = 0; k < 6; k++)
			fprintf(f, " [%lu]=%lu", k, a.casfail[k]);
		fprintf(f, "\n");
		if (a.ins_cleared_mark || a.ins_edge_bad)
			fprintf(f, "TXNSTATS %-9s INSERT AUDIT: aborted-after-clearing-mark "
				"%lu  bad-tail-edge %lu\n", name[i],
				a.ins_cleared_mark, a.ins_edge_bad);
		if (a.claimed_unlinked)
			fprintf(f, "TXNSTATS %-9s CLAIMED-BUT-NOT-LINKED: %lu\n",
				name[i], a.claimed_unlinked);
		if (a.audit_no_mark || a.audit_still_linked || a.audit_back_stale)
			fprintf(f, "TXNSTATS %-9s POST-COMMIT AUDIT: not-marked %lu  "
				"STILL-LINKED %lu  BACK-STALE %lu\n", name[i],
				a.audit_no_mark, a.audit_still_linked,
				a.audit_back_stale);
		if (a.stale_unmarked || a.marked_seen)
			fprintf(f, "TXNSTATS %-9s raw-MARKED: prepare-agreed %lu  "
				"PREPARE-PROCEEDED(stale) %lu\n", name[i],
				a.marked_seen, a.stale_unmarked);
		if (a.poison_set)
			fprintf(f, "TXNSTATS %-9s POISON-SET (same-slot records "
				"that do not chain): %lu\n", name[i], a.poison_set);
		if (a.seen_same || a.seen_marked || a.seen_proxy || a.seen_other)
			fprintf(f, "TXNSTATS %-9s cas-lost-to: same-as-old %lu  "
				"MARKED %lu  proxy %lu  other %lu\n", name[i],
				a.seen_same, a.seen_marked, a.seen_proxy,
				a.seen_other);
		if (a.seen_proxy)
			fprintf(f, "TXNSTATS %-9s proxy descriptor state: "
				"UNDECIDED %lu  SUCCEEDED %lu  FAILED %lu\n",
				name[i], a.proxy_undecided, a.proxy_succeeded,
				a.proxy_failed);
	}
}
#endif	/* DC_TXN_STATS_IMPL */

#else	/* !DC_TXN_STATS */

#define DC_TS_COMMIT(site, txnp, st)	do { } while (0)
#define DC_TS_BEGIN(site, txnp)		do { } while (0)
#define DC_TS_EAGAIN(site)		do { } while (0)
#define DC_TS_DEL_RET(site, ret)	do { } while (0)
#define DC_TS_RELINK_BAD(site)		do { } while (0)
#define DC_TS_STALE(site)		do { } while (0)
#define DC_TS_MARKEDSEEN(site)		do { } while (0)
#define DC_TS_DELAUDIT(site, nm, lk)	do { } while (0)
#define DC_TS_INSCLEAR(site)		do { } while (0)
#define DC_TS_INSEDGE(site)		do { } while (0)
#define DC_TS_BACKEDGE(site)		do { } while (0)
#define DC_TS_UNLINKED(site)		do { } while (0)

#endif	/* DC_TXN_STATS */
#endif	/* DCACHE_TXN_STATS_H */
