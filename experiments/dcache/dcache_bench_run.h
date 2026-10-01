// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_bench_run.h -- the harnesses' run phases: an untimed WARM-up, then
 * the timed RUN window.
 *
 * Without a warm-up the clock started the moment the workers were released,
 * so every window opened on cold per-worker caches, first-touch faults for
 * every new shell and negative, cold allocator caches and a grace-period /
 * fold pipeline still filling.  The setup's own warm pass (one lookup per
 * leaf) runs on the main thread and warms none of that.  Measured, the
 * transient is small (every engine within ~3% between 0.25 s and 4 s
 * windows, 2026-10-01), but a benchmark should measure a steady state by
 * construction, not by luck: now every worker runs its real, paced workload
 * for --warmup ms first, and a worker restarts its THROUGHPUT counters when
 * it sees the window open.  Correctness counters (wrong ids, errors, misses
 * on names no writer moves) stay cumulative, so a fault during the warm-up is
 * still reported.
 */
#ifndef DCACHE_BENCH_RUN_H
#define DCACHE_BENCH_RUN_H

#include <poll.h>
#include <urcu/compiler.h>
#include <urcu/uatomic.h>

#define GOFLAG_INIT	0
#define GOFLAG_RUN	1	/* the timed window */
#define GOFLAG_STOP	2
#define GOFLAG_WARM	3	/* workers run, nothing is counted */

#define WARMUP_MS_DEFAULT	200

/*
 * Worker side, once per iteration, with the phase this thread last saw
 * (start it at GOFLAG_WARM).  Returns 0 once the run is over, 2 on the
 * iteration where the timed window opened (restart the throughput counters),
 * else 1.  One load of a line that only changes twice, as the plain
 * `while (goflag == GOFLAG_RUN)` it replaces.
 */
static inline int run_continue(volatile int *goflag, int *phase)
{
	int g = uatomic_read(goflag);

	if (caa_likely(g == *phase))
		return 1;
	*phase = g;
	return g == GOFLAG_STOP ? 0 : 2;
}

/* Main side: release the workers into the warm-up, then open the window.
 * Main must already be RCU-offline: it sleeps through both phases. */
static inline void run_start(volatile int *goflag, long warmup_ms)
{
	if (warmup_ms > 0) {
		uatomic_set(goflag, GOFLAG_WARM);
		(void) poll(NULL, 0, (int) warmup_ms);
	}
	uatomic_set(goflag, GOFLAG_RUN);
}

#endif /* DCACHE_BENCH_RUN_H */
