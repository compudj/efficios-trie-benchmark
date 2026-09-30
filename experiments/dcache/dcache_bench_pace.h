// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_bench_pace.h -- hold a writer to a fixed operation rate.
 *
 * Why the harnesses need it.  Unpaced, every writer mutates back to back, so
 * each engine's readers are measured against whatever rename rate THAT
 * engine's writers reach -- and those differ by two orders of magnitude (the
 * seqlock engine serializes every rename on rename_lock; a reader-heavy
 * seqlock readdir or dentry_path starves its renamers to ~10k/s while a txn
 * engine's run at millions).  A slower writer writes the lines readers share
 * less often, so the engine with the SLOWER writers gets its readers measured
 * on a QUIETER machine.  Reader throughput is only comparable across engines
 * at one offered rename load, which is what this provides.
 *
 * Mechanism.  An absolute schedule: op k of a writer is released no earlier
 * than start + k * period.  Waiting is a spin on CLOCK_MONOTONIC (a vDSO read
 * of a read-only page) announcing QSBR quiescent states -- no store to any
 * line a reader or another writer shares, so the pacing itself adds no
 * coherence traffic, and grace periods (which the txn engines' folds wait on)
 * never stall on a waiting writer.  Sleeping instead would be coarse (timer
 * slack) and would hand the co-pinned call_rcu worker a cpu the unpaced
 * writer never gave it.
 *
 * A writer that falls behind (preempted, or an op slower than the period)
 * catches up at full speed, but only for PACE_SLACK_NS of schedule: older debt
 * is forfeited, so one preemption cannot turn into a long unpaced burst.  An
 * engine that cannot sustain the rate therefore shows it as an achieved rate
 * below the target, which the sweep reports rather than hides.
 */
#ifndef DCACHE_BENCH_PACE_H
#define DCACHE_BENCH_PACE_H

#include <stdlib.h>
#include <time.h>
#include <urcu/arch.h>

#include "dcache.h"

#define PACE_SLACK_NS	1000000LL	/* catch up at most 1 ms of schedule */

struct pace {
	long long period_ns;		/* 0 => unpaced */
	long long next_ns;		/* release time of the next op; 0 = unset */
};

static inline long long pace_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* One writer's share of an aggregate @rate (ops/s) split over @nwriters. */
static inline void pace_init(struct pace *p, double rate, int nwriters)
{
	p->period_ns = (rate > 0.0 && nwriters > 0)
		? (long long) (1e9 * (double) nwriters / rate + 0.5) : 0;
	if (rate > 0.0 && p->period_ns < 1)
		p->period_ns = 1;
	p->next_ns = 0;
}

/*
 * Wait for the next op's release time.  Returns 0 if *@goflag left @run while
 * waiting (the window closed: do not start the op), else 1.
 */
static inline int pace_wait(struct pace *p, volatile int *goflag, int run)
{
	long long now;

	if (!p->period_ns)
		return 1;
	now = pace_now_ns();
	if (!p->next_ns)
		p->next_ns = now;
	if (now < p->next_ns) {
		do {
			if (CMM_LOAD_SHARED(*goflag) != run)
				return 0;
			dc_quiescent();
			caa_cpu_relax();
		} while ((now = pace_now_ns()) < p->next_ns);
	} else if (now - p->next_ns > PACE_SLACK_NS) {
		p->next_ns = now - PACE_SLACK_NS;
	}
	p->next_ns += p->period_ns;
	return 1;
}

/*
 * Parse an ops/s rate: a plain or scientific number with an optional k/K or
 * m/M suffix ("250k", "1.5M", "2e5").  Returns -1.0 on a malformed value.
 */
static inline double pace_parse_rate(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s || v < 0.0)
		return -1.0;
	if (*end == 'k' || *end == 'K')
		v *= 1e3, end++;
	else if (*end == 'm' || *end == 'M')
		v *= 1e6, end++;
	return *end ? -1.0 : v;
}

#endif /* DCACHE_BENCH_PACE_H */
