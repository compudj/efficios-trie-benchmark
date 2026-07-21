// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * stress_dcache.c -- concurrent stress + conservation harness for the txn dcache
 * engine's ASYNC fold.  The deterministic repro (repro_fold) went latent once the
 * fold moved into a call_rcu worker, so THIS is the primary validator for the
 * concurrent stack/fold/splice machinery.  Run it under ASan (UAF / double-free /
 * torn read) and, on a liburcu built with compiler atomic builtins, TSAN (data
 * races).
 *
 * Workload -- disjoint-owner leaves over fixed directories
 * --------------------------------------------------------
 * A fixed set of directories /d0../d(D-1) is created once and never moved (so the
 * writer-side is_subdir/d_parent walk touches only immutable dirs plus a leaf's
 * OWN parent -- no cross-writer d_parent race, which is the still-plain
 * CONCURRENCY-TODO field).  Every leaf id is owned by exactly one writer, so no
 * two writers ever rename the same entry; a leaf is therefore ALWAYS reachable at
 * exactly the path its owner last recorded.  This still exercises the hard races:
 *
 *   - a writer re-renaming its own leaf faster than the fold worker drains it
 *     builds a multi-node chain and races its next STACK against the pending
 *     fold on the SAME chain (transfer-vs-splice re-classification, stack
 *     re-find on -ENOENT);
 *   - a fraction of iterations rename-then-UNLINK a leaf with no quiesce between,
 *     so the unlink lands on a live shell (top != host) and the pending fold
 *     RECLAIMs the orphaned chain while it races the writer's own and earlier
 *     folds draining on the worker; a re-add restores the leaf (conservation);
 *   - two leaves moving into the same directory concurrently contend on that
 *     dir's child-hlist head and on the global rename_gen bump;
 *   - readers resolve /d{k}/L{gid} concurrently with all of it: a POSITIVE
 *     result MUST carry id == gid (names encode ids), catching any torn read or
 *     wrong-host resolution live.
 *
 * Final check (quiescent): a dc_walk census must show every leaf id exactly once
 * at exactly its owner's recorded final path, and a dc_lookup of that path must
 * return POSITIVE with the right id -- conservation across both indexes.
 *
 * Usage: ./stress_dcache [writers [readers [dirs [leaves_per_writer [iters]]]]]
 * Exit 0 = conserved and race-clean (per the sanitizer); 1 = anomaly.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <urcu-qsbr.h>

#include "dcache.h"

/* ---- config (argv-overridable) ----------------------------------------- */
static int W = 4;			/* writer threads */
static int R = 4;			/* reader threads */
static int D = 8;			/* directories /d0../d(D-1) */
static int L = 16;			/* leaves per writer */
static long ITERS = 50000;		/* ops per thread (override via argv[5]) */

static struct dcache *g_dc;
static int *g_final_pos;		/* [W*L]: owner-recorded final dir of each leaf */
static uint64_t g_seed_base = 0x9e3779b97f4a7c15ULL;

/* Per-thread xorshift so we need no Math.random-style shared state. */
static inline uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return (*s = x);
}

static void mkpath(struct dc_path *p, int dir, int gid)
{
	char buf[DC_NAME_MAX * 2];

	snprintf(buf, sizeof(buf), "/d%d/L%d", dir, gid);
	if (dc_path_parse(p, buf) != 0) {
		fprintf(stderr, "bad path %s\n", buf);
		exit(2);
	}
}

/* ---- writer ------------------------------------------------------------- */

struct warg { int w; long errs; };

static void *writer(void *arg)
{
	struct warg *wa = arg;
	int base = wa->w * L;
	int *pos = calloc(L, sizeof(*pos));	/* current dir of each owned leaf */
	uint64_t s = g_seed_base ^ ((uint64_t) (wa->w + 1) * 0x100000001b3ULL);
	long it;
	int i;

	dc_register_thread();
	for (i = 0; i < L; i++)
		pos[i] = (base + i) % D;	/* mirror the setup distribution */

	for (it = 0; it < ITERS; it++) {
		struct dc_path from, to;
		int i = (int) (xrand(&s) % (uint64_t) L);
		int gid = base + i;
		int nd = (int) (xrand(&s) % (uint64_t) D);
		int ret;

		if (nd == pos[i])
			nd = (nd + 1) % D;
		mkpath(&from, pos[i], gid);
		mkpath(&to, nd, gid);

		if ((xrand(&s) & 3) == 0) {
			/*
			 * MID-TRANSITION UNLINK: the rename builds a shell, then the
			 * unlink lands ON it (top != host) with no quiesce between,
			 * so it removes the current top without demoting it and the
			 * pending fold RECLAIMs the orphaned chain (fold()); the
			 * re-add restores the leaf (same gid) so conservation holds.
			 * The unlink races the writer's OWN pending fold and the
			 * folds of its earlier chains draining on the call_rcu
			 * worker.  A single owner keeps -ENOENT impossible.
			 */
			ret = dc_rename(g_dc, &from, &to);
			if (ret != 0)
				wa->errs++;
			else if (dc_unlink(g_dc, &to) != 0)
				wa->errs++;
			else if (dc_add(g_dc, &to, (uint64_t) gid) != 0)
				wa->errs++;
			else
				pos[i] = nd;
		} else {
			ret = dc_rename(g_dc, &from, &to);
			if (ret == 0)
				pos[i] = nd;
			else
				wa->errs++;	/* single-owner: rename must succeed */
		}
		dc_quiescent();			/* let grace periods (folds) advance */
	}

	for (i = 0; i < L; i++)
		g_final_pos[base + i] = pos[i];
	free(pos);
	dc_unregister_thread();
	return NULL;
}

/* ---- reader ------------------------------------------------------------- */

struct rarg { int r; long bad; long hits; };

static void count_cb(uint64_t id, const struct qstr *name, void *arg)
{
	(void) id; (void) name;
	(*(long *) arg)++;
}

static void *reader(void *arg)
{
	struct rarg *ra = arg;
	uint64_t s = g_seed_base ^ ((uint64_t) (ra->r + 101) * 0x100000001b3ULL);
	int total = W * L;
	long it;

	dc_register_thread();
	for (it = 0; it < ITERS; it++) {
		if (xrand(&s) & 1) {
			/* lookup a random leaf at a random dir: POSITIVE => id==gid */
			int gid = (int) (xrand(&s) % (uint64_t) total);
			int dir = (int) (xrand(&s) % (uint64_t) D);
			struct dc_path p;
			uint64_t id = ~0ULL;

			mkpath(&p, dir, gid);
			if (dc_lookup(g_dc, &p, &id) == DC_POSITIVE) {
				ra->hits++;
				if (id != (uint64_t) gid)
					ra->bad++;	/* torn / wrong-host read */
			}
		} else {
			/* readdir a random dir: soft, just must not tear/crash */
			int dir = (int) (xrand(&s) % (uint64_t) D);
			struct dc_path p;
			char buf[DC_NAME_MAX];
			long n;

			snprintf(buf, sizeof(buf), "/d%d", dir);
			dc_path_parse(&p, buf);
			n = dc_readdir(g_dc, &p, count_cb, &n);
			(void) n;
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

/* ---- conservation census ----------------------------------------------- */

struct census {
	uint8_t *seen;		/* [W*L] times each gid appears in the tree */
	int total;
	long stray;		/* reachable ids outside [0, total) or dup */
};

#define DIR_ID_BASE 1000000ULL		/* directories carry ids >= this */

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= DIR_ID_BASE)
		return;			/* a fixed directory, not a leaf */
	if (id >= (uint64_t) c->total) {
		c->stray++;		/* a leaf-range id we never seeded */
		return;
	}
	if (c->seen[id]++)
		c->stray++;		/* duplicate reachability */
}

int main(int argc, char **argv)
{
	pthread_t *wt, *rt;
	struct warg *wargs;
	struct rarg *rargs;
	struct census c;
	int total, i, anomaly = 0;
	long werrs = 0, rbad = 0, rhits = 0;

	if (argc > 1) W = atoi(argv[1]);
	if (argc > 2) R = atoi(argv[2]);
	if (argc > 3) D = atoi(argv[3]);
	if (argc > 4) L = atoi(argv[4]);
	if (argc > 5) ITERS = atol(argv[5]);
	if (W < 1 || R < 0 || D < 2 || L < 1) {
		fprintf(stderr, "bad config\n");
		return 2;
	}
	total = W * L;

	rcu_register_thread();
	g_dc = dc_create(4096);
	g_final_pos = calloc(total, sizeof(*g_final_pos));
	printf("== stress_dcache (engine: %s) ==\n", dc_engine_name());
	printf("writers=%d readers=%d dirs=%d leaves/writer=%d iters=%ld total_leaves=%d\n",
	       W, R, D, L, ITERS, total);

	/* Build the fixed directories, then distribute the leaves. */
	for (i = 0; i < D; i++) {
		struct dc_path p;
		char buf[DC_NAME_MAX];

		snprintf(buf, sizeof(buf), "/d%d", i);
		dc_path_parse(&p, buf);
		if (dc_add(g_dc, &p, 1000000ULL + i)) {
			fprintf(stderr, "mkdir /d%d failed\n", i);
			return 2;
		}
	}
	for (i = 0; i < total; i++) {
		struct dc_path p;

		mkpath(&p, i % D, i);
		if (dc_add(g_dc, &p, (uint64_t) i)) {
			fprintf(stderr, "seed leaf %d failed\n", i);
			return 2;
		}
	}

	/* Fire the threads. */
	wt = calloc(W, sizeof(*wt));
	rt = calloc(R, sizeof(*rt));
	wargs = calloc(W, sizeof(*wargs));
	rargs = calloc(R, sizeof(*rargs));
	for (i = 0; i < W; i++) {
		wargs[i].w = i;
		pthread_create(&wt[i], NULL, writer, &wargs[i]);
	}
	for (i = 0; i < R; i++) {
		rargs[i].r = i;
		pthread_create(&rt[i], NULL, reader, &rargs[i]);
	}
	/*
	 * Go RCU-offline while blocked in join: main is a registered QSBR thread
	 * but reports no quiescent state inside pthread_join, so leaving it online
	 * would stall EVERY grace period and the async folds would never drain
	 * until the workers exit (fold backlog -> unbounded chain growth).
	 */
	rcu_thread_offline();
	for (i = 0; i < W; i++) {
		pthread_join(wt[i], NULL);
		werrs += wargs[i].errs;
	}
	for (i = 0; i < R; i++) {
		pthread_join(rt[i], NULL);
		rbad += rargs[i].bad;
		rhits += rargs[i].hits;
	}
	rcu_thread_online();

	/* Drive grace periods so any in-flight folds settle before the census. */
	rcu_quiescent_state();
	synchronize_rcu();

	/* Census: every leaf exactly once, at its owner-recorded final path. */
	memset(&c, 0, sizeof(c));
	c.total = total;
	c.seen = calloc(total, 1);
	dc_walk(g_dc, census_cb, &c);

	for (i = 0; i < total; i++) {
		struct dc_path p;
		uint64_t id = ~0ULL;

		if (c.seen[i] != 1)
			anomaly++;
		mkpath(&p, g_final_pos[i], i);
		if (dc_lookup(g_dc, &p, &id) != DC_POSITIVE || id != (uint64_t) i)
			anomaly++;
	}

#ifdef DC_STRESS_DEBUG
	{
		extern unsigned long dc_dbg_renames, dc_dbg_folds,
			dc_dbg_fold_retries, dc_dbg_fold_aborts;

		printf("DEBUG renames=%lu folds=%lu attempts=%lu "
		       "commit_ABORTS=%lu (%.3f%% of folds truly raced)\n",
		       dc_dbg_renames, dc_dbg_folds, dc_dbg_fold_retries,
		       dc_dbg_fold_aborts,
		       dc_dbg_folds ? 100.0 * dc_dbg_fold_aborts / dc_dbg_folds : 0.0);
	}
#endif
	printf("writer rename errors : %ld (expect 0)\n", werrs);
	printf("reader POSITIVE hits : %ld, wrong-id reads : %ld (expect 0)\n",
	       rhits, rbad);
	printf("census stray/dup ids : %ld (expect 0)\n", c.stray);
	printf("misplaced/missing    : %d leaves (expect 0)\n", anomaly);

	anomaly += (werrs != 0) + (rbad != 0) + (c.stray != 0);
	if (anomaly) {
		printf("RESULT: FAIL\n");
	} else {
		printf("RESULT: PASS (namespace conserved, both indexes agree)\n");
	}

	free(c.seen);
	free(wt); free(rt); free(wargs); free(rargs);
	dc_destroy(g_dc);
	free(g_final_pos);
	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
