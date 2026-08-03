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

#define DC_TS_COMMIT(site, txnp, st)	dc_ts_commit((site), (txnp), (st))
#define DC_TS_EAGAIN(site)		(dc_ts_row(site)->eagain++)

/* dcache.h declares this as void dc_txn_stats_dump(void *stream); the void*
 * keeps <stdio.h> out of the engine-agnostic interface. */

#ifdef DC_TXN_STATS_IMPL
struct dc_ts_row dc_ts_tab[DC_TS_MAX_THREADS][DC_TS_NR];
unsigned long dc_ts_next_slot;
__thread int dc_ts_slot = -1;

void dc_txn_stats_dump(void *stream)
{
	FILE *f = stream;
	static const char *const name[DC_TS_NR] = {
		"add", "unlink", "state", "stack", "fold", "exchange",
		"lru_add", "lru_del", "lru_evict"
	};
	unsigned long nthr = uatomic_load(&dc_ts_next_slot, CMM_RELAXED);
	unsigned long t, i;

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
			if (r->max_retry > a.max_retry)
				a.max_retry = r->max_retry;
		}
		if (!a.attempts)
			continue;
		fprintf(f, "TXNSTATS %-9s %12lu %12lu %12lu %12lu %9lu %12lu\n",
			name[i], a.attempts, a.aborts, a.escalations,
			a.published, a.max_retry, a.eagain);
		fprintf(f, "TXNSTATS %-9s poisoned-aborts: %lu\n", name[i], a.poison);
	}
}
#endif	/* DC_TXN_STATS_IMPL */

#else	/* !DC_TXN_STATS */

#define DC_TS_COMMIT(site, txnp, st)	do { } while (0)
#define DC_TS_EAGAIN(site)		do { } while (0)

#endif	/* DC_TXN_STATS */
#endif	/* DCACHE_TXN_STATS_H */
