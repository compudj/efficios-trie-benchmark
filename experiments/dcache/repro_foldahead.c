// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_foldahead.c -- deterministic demonstration that SYNCHRONOUS FOLD-AHEAD
 * bounds the transition chain when grace periods stall (the liveness cliff).
 *
 * The async fold worker drains only as grace periods advance.  Here main hammers
 * renames on a SINGLE entry and never reports a quiescent state -- so (as a
 * registered QSBR thread staying RCU-online, exactly the "main blocked in
 * pthread_join" trigger from the design note) it pins every grace period open and
 * the fold worker cannot run.  Each rename then stacks one shell that never
 * folds:
 *   - WITHOUT fold-ahead (build with a huge DC_FOLD_AHEAD_HI): the chain grows
 *     ~O(renames); chain_host_rcu() -- called by every reader AND writer -- goes
 *     O(chain), so renames go O(n) and the whole thing is O(n^2): the collapse.
 *   - WITH fold-ahead (default HI): each over-threshold rename splices the middle
 *     in-line, so the peak chain depth stays near DC_FOLD_AHEAD_HI regardless of
 *     the stall.
 * Then main quiesces, grace periods resume, every deferred fold/free drains, and
 * the namespace is conserved (one entry) -- ASan-clean either way.
 *
 * Reads dc_dbg_max_chain (needs -DDC_STRESS_DEBUG).  Build/run both variants via
 * `make repro-foldahead` (compares the capped vs uncapped peak depth).
 *
 * Usage: ./repro_foldahead [renames]
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <urcu-qsbr.h>

#include "dcache.h"

/* Engine debug counters (compiled in with -DDC_STRESS_DEBUG). */
extern unsigned long dc_dbg_max_chain, dc_dbg_renames, dc_dbg_folds;

static void mkp(struct dc_path *p, const char *s)
{
	if (dc_path_parse(p, s) != 0) {
		fprintf(stderr, "bad path %s\n", s);
		exit(2);
	}
}

int main(int argc, char **argv)
{
	long N = argc > 1 ? atol(argv[1]) : 20000;
	struct dcache *dc;
	struct dc_path pa, px, py;
	unsigned long peak;
	uint64_t id = ~0ULL;
	int toggle = 0, bounded;
	long i;

	rcu_register_thread();
	dc = dc_create(1024);
	printf("== repro_foldahead (engine: %s) ==\n", dc_engine_name());
	printf("renames=%ld\n", N);

	mkp(&pa, "/a");
	mkp(&px, "/a/x");
	mkp(&py, "/a/y");
	if (dc_add(dc, &pa, 1) || dc_add(dc, &px, 2)) {
		fprintf(stderr, "seed failed\n");
		return 2;
	}

	/*
	 * Hammer renames on the single entry WITHOUT ever quiescing: main stays
	 * RCU-online and reports no quiescent state, so no grace period completes
	 * and the async fold worker never drains -- the chain would grow without
	 * bound but for the in-line fold-ahead.
	 */
	for (i = 0; i < N; i++) {
		int ret = dc_rename(dc, toggle ? &py : &px, toggle ? &px : &py);

		if (ret != 0) {
			fprintf(stderr, "rename %ld failed: %d\n", i, ret);
			return 2;
		}
		toggle ^= 1;
	}

	peak = dc_dbg_max_chain;
	printf("peak chain depth while GP-stalled: %lu (renames=%lu folds=%lu)\n",
	       peak, dc_dbg_renames, dc_dbg_folds);

	/* Release the stall: quiesce so grace periods resume and folds drain. */
	rcu_quiescent_state();
	synchronize_rcu();

	/* Conservation: exactly the one entry, at its final name (N even => /a/x). */
	if (dc_lookup(dc, (toggle ? &py : &px), &id) != DC_POSITIVE || id != 2) {
		printf("RESULT: FAIL (entry not conserved: id=%llu)\n",
		       (unsigned long long) id);
		dc_destroy(dc);
		rcu_unregister_thread();
		return 1;
	}

	dc_destroy(dc);			/* rcu_barriers drain the deferred frees */
	rcu_unregister_thread();

	/*
	 * With fold-ahead the peak stays far below the rename count; without it,
	 * the peak tracks it (each un-drained rename adds a shell).  A 4x margin
	 * cleanly separates the capped (~1024) from the uncapped (~N) case.
	 */
	bounded = (unsigned long) N > 4 * peak;
	printf("RESULT: PASS -- namespace conserved; chain %s (peak %lu, renames %ld)\n",
	       bounded ? "BOUNDED by fold-ahead" : "tracks the rename count (uncapped)",
	       peak, N);
	return 0;
}
