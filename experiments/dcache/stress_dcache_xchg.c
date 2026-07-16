// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * stress_dcache_xchg.c -- concurrent stress + conservation harness for the txn
 * dcache engine's ATOMIC exchange (dc_rename_exchange composing two shell stacks
 * in ONE commit).  Complements stress_dcache.c (renames + mid-transition unlink);
 * run under ASan and, on a compiler-atomics liburcu, TSAN.
 *
 * Workload -- permutation over fixed slots
 * ----------------------------------------
 * N fixed slot PATHS /d{k%D}/S{k} are seeded so slot k initially holds id k.  An
 * exchange swaps the two ENTRIES at a pair of slot paths, so over time slot k
 * holds a permuted id perm[k]; the multiset of ids is invariant.  Each writer
 * owns a disjoint block of slots and only exchanges two of its OWN slots, so no
 * two writers touch the same entry (conservation is per-writer), yet they still
 * contend hard on SHARED state: exchanging across directories reparents both
 * entries between the fixed dir child-hlists (a real cross-writer race on those
 * shared list heads) and every exchange aliases the global rename_gen.  Cross-dir
 * pairs exercise the cross-parent path (both d_parent reparents + both loop-check
 * walks fold into the one commit); same-dir pairs exercise the pure-name swap.
 *
 * The atomicity assertion the sequential 3-stack placeholder could NOT pass: an
 * exchange is one commit, so a slot path is NEVER momentarily empty -- a reader
 * looking up any valid slot MUST get POSITIVE (with an id in range).  An ABSENT
 * or out-of-range id is an atomicity / torn-read failure.
 *
 * Final check (quiescent): each id appears exactly once, and dc_lookup of each
 * slot returns the owner-recorded final perm[k] -- conservation across both
 * indexes and a correct permutation.
 *
 * Usage: ./stress_dcache_xchg [writers [readers [dirs [slots_per_writer [iters]]]]]
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
static int M = 16;			/* slots per writer */
static long ITERS = 50000;		/* ops per thread */

static struct dcache *g_dc;
static int *g_final_perm;		/* [W*M]: owner-recorded final id at each slot */
static uint64_t g_seed_base = 0x9e3779b97f4a7c15ULL;

static inline uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return (*s = x);
}

/* Slot k lives at the fixed path /d{k%D}/S{k}. */
static void slotpath(struct dc_path *p, int k)
{
	char buf[DC_NAME_MAX * 2];

	snprintf(buf, sizeof(buf), "/d%d/S%d", k % D, k);
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
	int base = wa->w * M;
	int *perm = calloc(M, sizeof(*perm));	/* id currently at each owned slot */
	uint64_t s = g_seed_base ^ ((uint64_t) (wa->w + 1) * 0x100000001b3ULL);
	long it;
	int i;

	dc_register_thread();
	for (i = 0; i < M; i++)
		perm[i] = base + i;		/* seeded identity permutation */

	for (it = 0; it < ITERS; it++) {
		struct dc_path pa, pb;
		int a = (int) (xrand(&s) % (uint64_t) M);
		int b = (int) (xrand(&s) % (uint64_t) M);
		int tmp;

		if (a == b) {
			b = (b + 1) % M;
			if (M == 1)		/* nothing to swap with */
				break;
		}
		slotpath(&pa, base + a);
		slotpath(&pb, base + b);
		if (dc_rename_exchange(g_dc, &pa, &pb) != 0) {
			wa->errs++;		/* single-owner: exchange must succeed */
		} else {
			tmp = perm[a];		/* slots swapped their entries */
			perm[a] = perm[b];
			perm[b] = tmp;
		}
		dc_quiescent();			/* let grace periods (folds) advance */
	}

	for (i = 0; i < M; i++)
		g_final_perm[base + i] = perm[i];
	free(perm);
	dc_unregister_thread();
	return NULL;
}

/* ---- reader ------------------------------------------------------------- */

struct rarg { int r; long absent; long bad; long hits; };

static void count_cb(uint64_t id, const struct qstr *name, void *arg)
{
	(void) id; (void) name;
	(*(long *) arg)++;
}

static void *reader(void *arg)
{
	struct rarg *ra = arg;
	uint64_t s = g_seed_base ^ ((uint64_t) (ra->r + 101) * 0x100000001b3ULL);
	int total = W * M;
	long it;

	dc_register_thread();
	for (it = 0; it < ITERS; it++) {
		if (xrand(&s) & 1) {
			/*
			 * A valid slot is NEVER empty under atomic exchange: the
			 * swap is one commit, so the slot path always holds exactly
			 * one entry.  POSITIVE with id in range is required; ABSENT
			 * would mean a non-atomic (torn) exchange.
			 */
			int k = (int) (xrand(&s) % (uint64_t) total);
			struct dc_path p;
			uint64_t id = ~0ULL;
			enum dc_result res;

			slotpath(&p, k);
			res = dc_lookup(g_dc, &p, &id);
			if (res == DC_POSITIVE) {
				ra->hits++;
				if (id >= (uint64_t) total)
					ra->bad++;	/* torn / wrong-host read */
			} else {
				ra->absent++;		/* atomicity violation */
			}
		} else {
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
	uint8_t *seen;		/* [W*M] times each id appears in the tree */
	int total;
	long stray;
};

#define DIR_ID_BASE 1000000ULL

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= DIR_ID_BASE)
		return;			/* a fixed directory */
	if (id >= (uint64_t) c->total) {
		c->stray++;
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
	long werrs = 0, rabsent = 0, rbad = 0, rhits = 0;

	if (argc > 1) W = atoi(argv[1]);
	if (argc > 2) R = atoi(argv[2]);
	if (argc > 3) D = atoi(argv[3]);
	if (argc > 4) M = atoi(argv[4]);
	if (argc > 5) ITERS = atol(argv[5]);
	if (W < 1 || R < 0 || D < 2 || M < 2) {
		fprintf(stderr, "bad config (need W>=1 R>=0 D>=2 slots/writer>=2)\n");
		return 2;
	}
	total = W * M;

	rcu_register_thread();
	g_dc = dc_create(4096);
	g_final_perm = calloc(total, sizeof(*g_final_perm));
	printf("== stress_dcache_xchg (engine: %s) ==\n", dc_engine_name());
	printf("writers=%d readers=%d dirs=%d slots/writer=%d iters=%ld total_slots=%d\n",
	       W, R, D, M, ITERS, total);

	/* Build the fixed directories, then seed the identity permutation. */
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
	for (i = 0; i < total; i++) {
		struct dc_path p;

		slotpath(&p, i);
		if (dc_add(g_dc, &p, (uint64_t) i)) {
			fprintf(stderr, "seed slot %d failed\n", i);
			return 2;
		}
	}

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
	/* Offline while joining: a registered thread that never quiesces in
	 * pthread_join would stall every grace period and starve the folds. */
	rcu_thread_offline();
	for (i = 0; i < W; i++) {
		pthread_join(wt[i], NULL);
		werrs += wargs[i].errs;
	}
	for (i = 0; i < R; i++) {
		pthread_join(rt[i], NULL);
		rabsent += rargs[i].absent;
		rbad += rargs[i].bad;
		rhits += rargs[i].hits;
	}
	rcu_thread_online();

	rcu_quiescent_state();
	synchronize_rcu();

	/* Census: every id exactly once, at its owner-recorded final slot. */
	memset(&c, 0, sizeof(c));
	c.total = total;
	c.seen = calloc(total, 1);
	dc_walk(g_dc, census_cb, &c);

	for (i = 0; i < total; i++) {
		struct dc_path p;
		uint64_t id = ~0ULL;

		if (c.seen[i] != 1)
			anomaly++;
		slotpath(&p, i);
		if (dc_lookup(g_dc, &p, &id) != DC_POSITIVE ||
		    id != (uint64_t) g_final_perm[i])
			anomaly++;
	}

#ifdef DC_STRESS_DEBUG
	{
		extern unsigned long dc_dbg_renames, dc_dbg_folds, dc_dbg_max_chain;

		printf("DEBUG renames=%lu folds=%lu max_chain=%lu\n",
		       dc_dbg_renames, dc_dbg_folds, dc_dbg_max_chain);
	}
#endif
	printf("writer exchange errors : %ld (expect 0)\n", werrs);
	printf("reader POSITIVE hits   : %ld, ABSENT (atomicity) : %ld, bad-id : %ld (expect 0/0)\n",
	       rhits, rabsent, rbad);
	printf("census stray/dup ids   : %ld (expect 0)\n", c.stray);
	printf("misplaced/missing      : %d slots (expect 0)\n", anomaly);

	anomaly += (werrs != 0) + (rabsent != 0) + (rbad != 0) + (c.stray != 0);
	if (anomaly)
		printf("RESULT: FAIL\n");
	else
		printf("RESULT: PASS (permutation conserved, exchange atomic)\n");

	free(c.seen);
	free(wt); free(rt); free(wargs); free(rargs);
	dc_destroy(g_dc);
	free(g_final_perm);
	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
