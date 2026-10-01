// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * stress_dcache_shared.c -- SHARED-renamer stress for the folds: several
 * writers rename the SAME objects, so the shells stacked on one object come
 * from different threads and their folds run on different call_rcu workers,
 * CONCURRENTLY, against one host.
 *
 * Why it exists.  Every other harness gives each object a single owner (and
 * stress_dcache even runs every fold on liburcu's one default worker), so the
 * folds of one object always ran one after the other.  It was written against
 * the transition chain this design had until 2026-10-01 and found the race
 * that retired it: two adjacent middle relays spliced at once by lock-free
 * folds on different workers both committed, leaving a freed relay linked
 * (the txn engine and the bucket lock's DC_CHAIN_SWMW; the per-host-lock
 * builds were immune).  Without a chain, what it races now is a fold's
 * TRANSFER against a re-rename and an unlink of the same entry, and many
 * demoted shells' frees against the host they all point at.
 *
 * Workload.  D fixed directories, N leaves (few, so chains grow deep: the
 * writers rename flat out, far faster than a fold drains).  A rename picks a
 * random leaf, takes that leaf's mutex (so the census stays exact: the holder
 * knows the leaf's current dir), moves it to another dir and releases.  The
 * mutex serializes the renames of a leaf, never its folds.  Each writer has its
 * own call_rcu worker, unpinned.
 *
 * Readers (default arm): a lookup of a leaf's current path must, on a hit,
 * carry the leaf's id; a reverse walk (dc_dentry_path) from a handle resolved in
 * the same read-side section must report /d<k>/L<gid> -- a wrong d_top shows
 * up there first.  -DSTRESS_UNLINK instead makes a quarter of the renames
 * rename + unlink + re-add (an unlink freeing the host while shells of it
 * still wait for their folds); no reverse walks in that arm, since a handle
 * can then be freed.
 *
 * Final check (quiescent, every fold drained): each leaf exactly once in a
 * dc_walk census and positive at its recorded path with its own id.  Run it
 * under ASan: a fold that touches a freed host, or frees a live top, is a
 * use-after-free.
 *
 * Usage: ./stress_dcache_shared [writers [readers [dirs [leaves [iters]]]]]
 * Exit 0 = conserved and clean; 1 = anomaly.
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
#include <urcu/uatomic.h>

#include "dcache.h"
#include "dcache_bench_rand.h"

static int W = 8;			/* writer threads */
static int R = 4;			/* reader threads */
static int D = 8;			/* directories /d0../d(D-1) */
static int N = 4;			/* shared leaves */
static long ITERS = 50000;		/* renames per writer */

static struct dcache *g_dc;
static int *g_pos;			/* [N] current dir, under g_lock[gid] */
static pthread_mutex_t *g_lock;		/* [N] per-leaf rename serialization */
static int g_stop;

#define DIR_ID_BASE 1000000ULL

/* Address-identity builds return the host address, not the logical id; the
 * gates build with -DDC_SPLIT_KEEPID, where the id check is live. */
extern const int dc_lookup_id_is_address __attribute__((weak));
static inline int id_is_address(void)
{
	return &dc_lookup_id_is_address && dc_lookup_id_is_address;
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

/* Take a leaf's mutex RCU-offline: a QSBR thread blocked while online would
 * hold off every grace period, and with it the folds this harness races. */
static void leaf_lock(int gid)
{
	rcu_thread_offline();
	pthread_mutex_lock(&g_lock[gid]);
	rcu_thread_online();
}

static void leaf_unlock(int gid)
{
	pthread_mutex_unlock(&g_lock[gid]);
}

struct warg { int w; long errs, renames; };

static void *writer(void *arg)
{
	struct warg *wa = arg;
	uint64_t s = 0x9e3779b97f4a7c15ULL ^ ((uint64_t) (wa->w + 1) * 0x100000001b3ULL);
	struct call_rcu_data *crdp;
	long it;

	dc_register_thread();
	crdp = create_call_rcu_data(0, -1);	/* this writer's own fold worker */
	if (crdp)
		set_thread_call_rcu_data(crdp);
	for (it = 0; it < ITERS; it++) {
		int gid = (int) xrange(&s, (uint32_t) N);
		int nd = (int) xrange(&s, (uint32_t) D);
		struct dc_path from, to;
		int ret;

		leaf_lock(gid);
		if (nd == g_pos[gid])
			nd = (nd + 1) % D;
		mkpath(&from, g_pos[gid], gid);
		mkpath(&to, nd, gid);
		ret = dc_rename(g_dc, &from, &to);
#ifdef STRESS_UNLINK
		if (ret == 0 && xtop(&s, 2) == 0) {
			/* unlink the fresh top before its fold: the host is
			 * freed while this shell and older ones still wait */
			ret = dc_unlink(g_dc, &to);
			if (ret == 0)
				ret = dc_add(g_dc, &to, (uint64_t) gid);
		}
#endif
		if (ret == 0) {
			/* the readers' racy peek is an atomic load: pair it */
			uatomic_store(&g_pos[gid], nd, CMM_RELAXED);
			wa->renames++;
		} else {
			wa->errs++;
		}
		leaf_unlock(gid);
		dc_quiescent();
	}
	/* this writer's worker outlives it; main's rcu_barrier() drains it */
	dc_unregister_thread();
	return NULL;
}

struct rarg { int r; long hits, bad, walks; };

static void *reader(void *arg)
{
	struct rarg *ra = arg;
	uint64_t s = 0x2545f4914f6cdd1dULL ^ ((uint64_t) (ra->r + 101) * 0x100000001b3ULL);

	dc_register_thread();
	while (!uatomic_load(&g_stop, CMM_RELAXED)) {
		int gid = (int) xrange(&s, (uint32_t) N);
		int dir = uatomic_load(&g_pos[gid], CMM_RELAXED);	/* racy by design */
		struct dc_path p;
		uint64_t id = ~0ULL;

		mkpath(&p, dir, gid);
		if (dc_lookup(g_dc, &p, &id) == DC_POSITIVE) {
			ra->hits++;
			if (!id_is_address() && id != (uint64_t) gid)
				ra->bad++;
		}
#ifndef STRESS_UNLINK
		{
			/* reverse walk from a handle taken in this same read-side
			 * section (no quiescent state in between: QSBR keeps it) */
			struct dentry *h = dc_lookup_dentry(g_dc, &p);
			struct dc_path out;
			char want[DC_NAME_MAX];

			if (h) {
				snprintf(want, sizeof(want), "L%d", gid);
				if (dc_dentry_path(g_dc, h, &out) != 0 ||
				    out.ndepth != 2 ||
				    strcmp(out.comp[1].name, want) != 0 ||
				    out.comp[0].name[0] != 'd')
					ra->bad++;
				ra->walks++;
			}
		}
#endif
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

struct census { uint8_t *seen; long stray; };

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= DIR_ID_BASE)
		return;
	if (id >= (uint64_t) N || c->seen[id]++)
		c->stray++;
}

int main(int argc, char **argv)
{
	pthread_t *wt, *rt;
	struct warg *wa;
	struct rarg *ra;
	struct census c;
	long werrs = 0, renames = 0, rbad = 0, rhits = 0, walks = 0;
	int i, anomaly = 0;

	if (argc > 1) W = atoi(argv[1]);
	if (argc > 2) R = atoi(argv[2]);
	if (argc > 3) D = atoi(argv[3]);
	if (argc > 4) N = atoi(argv[4]);
	if (argc > 5) ITERS = atol(argv[5]);
	setvbuf(stdout, NULL, _IOLBF, 0);	/* a sanitizer exit skips stdio's flush */
	if (W < 2 || R < 0 || D < 2 || N < 1) {
		fprintf(stderr, "bad config (needs >= 2 writers)\n");
		return 2;
	}
	rcu_register_thread();
	g_dc = dc_create(4096);
	g_pos = calloc(N, sizeof(*g_pos));
	g_lock = calloc(N, sizeof(*g_lock));
	printf("== stress_dcache_shared (engine: %s%s) ==\n", dc_engine_name(),
#ifdef STRESS_UNLINK
	       ", rename+unlink+re-add"
#else
	       ""
#endif
	       );
	printf("writers=%d readers=%d dirs=%d shared_leaves=%d renames/writer=%ld\n",
	       W, R, D, N, ITERS);
	for (i = 0; i < D; i++) {
		struct dc_path p;
		char buf[DC_NAME_MAX];

		snprintf(buf, sizeof(buf), "/d%d", i);
		dc_path_parse(&p, buf);
		if (dc_add(g_dc, &p, DIR_ID_BASE + i)) {
			fprintf(stderr, "mkdir /d%d failed\n", i);
			return 2;
		}
	}
	for (i = 0; i < N; i++) {
		struct dc_path p;

		pthread_mutex_init(&g_lock[i], NULL);
		g_pos[i] = i % D;
		mkpath(&p, g_pos[i], i);
		if (dc_add(g_dc, &p, (uint64_t) i)) {
			fprintf(stderr, "seed leaf %d failed\n", i);
			return 2;
		}
	}

	wt = calloc(W, sizeof(*wt));
	rt = calloc(R, sizeof(*rt));
	wa = calloc(W, sizeof(*wa));
	ra = calloc(R, sizeof(*ra));
	for (i = 0; i < W; i++) {
		wa[i].w = i;
		pthread_create(&wt[i], NULL, writer, &wa[i]);
	}
	for (i = 0; i < R; i++) {
		ra[i].r = i;
		pthread_create(&rt[i], NULL, reader, &ra[i]);
	}
	rcu_thread_offline();		/* main only blocks in join */
	for (i = 0; i < W; i++) {
		pthread_join(wt[i], NULL);
		werrs += wa[i].errs;
		renames += wa[i].renames;
	}
	uatomic_store(&g_stop, 1, CMM_RELAXED);
	for (i = 0; i < R; i++) {
		pthread_join(rt[i], NULL);
		rbad += ra[i].bad;
		rhits += ra[i].hits;
		walks += ra[i].walks;
	}
	rcu_thread_online();

	/* every fold drained: grace periods, then the callback barrier, twice
	 * (a fold queues its own reclaim) */
	rcu_quiescent_state();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();

	memset(&c, 0, sizeof(c));
	c.seen = calloc(N, 1);
	dc_walk(g_dc, census_cb, &c);
	for (i = 0; i < N; i++) {
		struct dc_path p;
		uint64_t id = ~0ULL;

		if (c.seen[i] != 1)
			anomaly++;
		mkpath(&p, g_pos[i], i);
		if (dc_lookup(g_dc, &p, &id) != DC_POSITIVE ||
		    (!id_is_address() && id != (uint64_t) i))
			anomaly++;
	}
	printf("renames committed    : %ld\n", renames);
	printf("writer errors        : %ld (expect 0)\n", werrs);
	printf("reader hits / walks  : %ld / %ld\n", rhits, walks);
	printf("reader bad results   : %ld (expect 0)\n", rbad);
	printf("census anomalies     : %d (expect 0)\n", anomaly);
	printf("census stray/dup ids : %ld (expect 0)\n", c.stray);
	free(c.seen);
	free(wt);
	free(rt);
	free(wa);
	free(ra);
	if (werrs || rbad || anomaly || c.stray) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS (shared renames conserved, reverse walks consistent)\n");
	return 0;
}
