// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * stress_dcache_dirs.c -- concurrent DIRECTORY-move stress for the txn dcache.
 *
 * The leaf harness (stress_dcache.c) never races the transacted d_parent slot
 * nor the cross-dir loop check on a non-trivial ancestry.  This one does, on two
 * fronts run concurrently:
 *
 *  (1) Subtree relocation + conservation.  Each mover thread owns a set of
 *      directory entries M<gid>, each holding a leaf child L<gid>, and shuffles
 *      them among fixed anchor directories /a0../a(K-1).  A move relocates the
 *      WHOLE subtree (the leaf keys on the mover's stable address, so it follows
 *      without rehash), and d_parent of the mover is transacted-stored in the
 *      move's commit.  Conservation: every mover AND its leaf ends reachable at
 *      exactly the path its owner last recorded.
 *
 *  (2) Mutual-cycle race.  For each pair, X and Y start as siblings under a
 *      private anchor /c<p>.  One thread repeatedly nests/unnests X under Y, the
 *      other Y under X.  The loop check must make it IMPOSSIBLE for both to be
 *      nested at once (that is a detached X<->Y cycle): nesting X under Y folds
 *      the Y->root ancestry into the commit's validate set, so if Y is already
 *      under X the commit sees X on the path and returns -EINVAL, and two moves
 *      that would jointly form the cycle mutually invalidate each other's
 *      validated d_parent edge so at most one commits.  A checker thread
 *      continuously asserts X and Y each remain reachable from the root at one
 *      of their two possible spots -- a cycle would DETACH them (neither is a
 *      child of the anchor any more), which the checker catches as "vanished",
 *      and the final census catches as missing.  At least one -EINVAL must occur
 *      (else the loop check was never actually exercised).
 *
 * Exit 0 = conserved, acyclic, race-clean; 1 = anomaly.  Run under ASan / TSAN.
 * Usage: ./stress_dcache_dirs [movers [anchors [movers_each [pairs [iters]]]]]
 *
 * LIVENESS CLIFF (not a correctness bug): pushing write pressure far past the
 * defaults -- many movers over few anchors (K<=4) AND many cycle pairs -- can
 * collapse throughput.  The async fold is grace-period-bound; under saturating
 * escalation the worker threads park in the engine's fair-mutex lane while
 * RCU-online (not quiescing), grace periods stall, folds stop draining,
 * transition chains grow without bound (chain depth into the thousands), and
 * renames go O(chain) -> a feedback collapse.  No cycle ever forms and every
 * completing run conserves; it is the GP-bound-fold property (see the design
 * note) tripped by lane-parking rather than a blocked main thread.
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

/*
 * Defaults sit in the reliable regime.  Cranking the write pressure far past it
 * -- many movers over FEW anchors (K<=4) AND many cycle pairs -- drives the
 * async fold's grace-period-bound reclamation into collapse: worker threads park
 * in the engine's fair-mutex escalation lane (RCU-online, not quiescing), so
 * grace periods stall, folds stop draining, transition chains grow without
 * bound, and rename throughput craters.  That is a LIVENESS cliff (the same
 * GP-bound-fold property documented for the leaf harness, here tripped by
 * lane-parking rather than a blocked main), NOT a correctness failure -- no
 * cycle ever forms and every completing run conserves.  Keep the defaults below
 * the cliff; see the header comment.
 */
static int W = 4;		/* mover threads */
static int K = 8;		/* anchor dirs /a0../a(K-1) */
static int Me = 4;		/* movers per mover thread */
static int NPAIR = 2;		/* mutual-cycle pairs */
static long ITERS = 20000;	/* ops per thread */

#define DIR_ID_BASE   1000000ULL	/* anchors + structural dirs */
#define MOVER_ID_BASE 0ULL		/* mover dir M<gid>: id = gid */
#define LEAF_ID_BASE  500000ULL		/* leaf L<gid>: id = LEAF_ID_BASE + gid */

static struct dcache *g_dc;
static int *g_final_anchor;		/* [W*Me] recorded final anchor of each mover */
static long g_einval;			/* total loop-check rejections (must be > 0) */
static long g_cycle_detected;		/* checker: X or Y ever vanished (must be 0) */

static inline uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return (*s = x);
}

static struct dc_path *mkp(struct dc_path *p, const char *s)
{
	if (dc_path_parse(p, s) != 0) {
		fprintf(stderr, "bad path %s\n", s);
		exit(2);
	}
	return p;
}

/* ---- mover thread: relocate subtrees among anchors, with conservation ---- */

struct marg { int w; long errs; };

static void *mover(void *arg)
{
	struct marg *ma = arg;
	int base = ma->w * Me;
	int *pos = calloc(Me, sizeof(*pos));
	uint64_t s = 0x9e3779b9ULL ^ ((uint64_t) (ma->w + 1) * 0x100000001b3ULL);
	long it;
	int i;

	dc_register_thread();
	for (i = 0; i < Me; i++)
		pos[i] = (base + i) % K;

	for (it = 0; it < ITERS; it++) {
		struct dc_path from, to;
		char fb[64], tb[64];
		int i = (int) (xrand(&s) % (uint64_t) Me);
		int gid = base + i;
		int nd = (int) (xrand(&s) % (uint64_t) K);
		int ret;

		if (nd == pos[i])
			nd = (nd + 1) % K;
		snprintf(fb, sizeof(fb), "/a%d/M%d", pos[i], gid);
		snprintf(tb, sizeof(tb), "/a%d/M%d", nd, gid);
		ret = dc_rename(g_dc, mkp(&from, fb), mkp(&to, tb));
		if (ret == 0)
			pos[i] = nd;
		else
			ma->errs++;		/* single-owner mover: must succeed */
		dc_quiescent();
	}
	for (i = 0; i < Me; i++)
		g_final_anchor[base + i] = pos[i];
	free(pos);
	dc_unregister_thread();
	return NULL;
}

/* ---- cycle pair: one thread nests/unnests X under Y (self==0) or Y under X -- */

struct carg { int pair; int self; long einval; };

static void *cycler(void *arg)
{
	struct carg *ca = arg;
	int p = ca->pair;
	/* self 0 drives X (nest under Y); self 1 drives Y (nest under X). */
	const char *me = ca->self ? "Y" : "X";
	const char *other = ca->self ? "X" : "Y";
	int nested = 0;			/* belief: is `me` nested under `other`? */
	long it;

	/*
	 * `me` is moved only by this thread, and while `me` is nested under
	 * `other` the peer cannot move `other` (its nest is a cycle, rejected), so
	 * `nested` stays exactly accurate as long as we flip it ONLY on success.
	 */
	dc_register_thread();
	for (it = 0; it < ITERS; it++) {
		struct dc_path from, to;
		char fb[64], tb[64];
		int ret;

		if (!nested) {
			/* nest me under other: /c<p>/<me> -> /c<p>/<other>/<me> */
			snprintf(fb, sizeof(fb), "/c%d/%s", p, me);
			snprintf(tb, sizeof(tb), "/c%d/%s/%s", p, other, me);
			ret = dc_rename(g_dc, mkp(&from, fb), mkp(&to, tb));
			if (ret == 0)
				nested = 1;
			else if (ret == -EINVAL)
				ca->einval++;	/* other under me: cycle refused */
			/* -ENOENT (other not at depth 1) / -EEXIST: leave nested=0 */
		} else {
			/* unnest me back to the anchor */
			snprintf(fb, sizeof(fb), "/c%d/%s/%s", p, other, me);
			snprintf(tb, sizeof(tb), "/c%d/%s", p, me);
			ret = dc_rename(g_dc, mkp(&from, fb), mkp(&to, tb));
			if (ret == 0)
				nested = 0;
		}
		dc_quiescent();
	}
	uatomic_add(&g_einval, ca->einval);
	dc_unregister_thread();
	return NULL;
}

/* ---- checker: X and Y must each stay root-reachable (no detached cycle) ---- */

struct chkarg { long iters; };

/* Is `name` at /c<p>/<name> (depth 1) or /c<p>/<other>/<name> (nested) RIGHT NOW? */
static int reachable_now(int p, const char *name, const char *other)
{
	struct dc_path pa;
	char b[80];

	snprintf(b, sizeof(b), "/c%d/%s", p, name);
	if (dc_lookup(g_dc, mkp(&pa, b), NULL) == DC_POSITIVE)
		return 1;
	snprintf(b, sizeof(b), "/c%d/%s/%s", p, other, name);
	return dc_lookup(g_dc, mkp(&pa, b), NULL) == DC_POSITIVE;
}

/*
 * A node in flight is momentarily absent from BOTH spots between the two
 * lookups (it moved in the window) -- a false positive.  A REAL detached cycle
 * is permanent (the pair is unreachable, so no thread can ever move it back),
 * so it stays absent forever.  Distinguish by persistence: re-check across many
 * consecutive quiescent instants; only all-absent is a real detachment.
 */
static int detached_persistent(int p, const char *name, const char *other)
{
	int k;

	for (k = 0; k < 64; k++) {
		if (reachable_now(p, name, other))
			return 0;		/* reappeared: it was just in flight */
		dc_quiescent();
	}
	return 1;				/* absent 64 instants running: cycle */
}

static void *checker(void *arg)
{
	struct chkarg *cka = arg;
	long it;

	dc_register_thread();
	for (it = 0; it < cka->iters; it++) {
		int p;

		for (p = 0; p < NPAIR; p++) {
			if (!reachable_now(p, "X", "Y") &&
			    detached_persistent(p, "X", "Y"))
				uatomic_inc(&g_cycle_detected);
			if (!reachable_now(p, "Y", "X") &&
			    detached_persistent(p, "Y", "X"))
				uatomic_inc(&g_cycle_detected);
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

/* ---- conservation census ------------------------------------------------- */

struct census {
	uint8_t *mover_seen;	/* [W*Me] */
	uint8_t *leaf_seen;	/* [W*Me] */
	int total_movers;
	long stray;
};

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= DIR_ID_BASE)
		return;					/* anchors / structural */
	if (id >= LEAF_ID_BASE) {			/* a leaf L<gid> */
		uint64_t g = id - LEAF_ID_BASE;

		if (g < (uint64_t) c->total_movers && !c->leaf_seen[g]++)
			return;
		c->stray++;
	} else {					/* a mover M<gid> */
		if (id < (uint64_t) c->total_movers && !c->mover_seen[id]++)
			return;
		c->stray++;
	}
}

static int add_dir(const char *path, uint64_t id)
{
	struct dc_path p;

	return dc_add(g_dc, mkp(&p, path), id);
}

int main(int argc, char **argv)
{
	pthread_t *mt, ct[64], chk;
	struct marg *margs;
	struct carg cargs[64] = { { 0, 0, 0 } };	/* zero .einval etc. */
	struct chkarg chka;
	struct census c;
	int total, i, p, anomaly = 0;
	long werrs = 0;

	if (argc > 1) W = atoi(argv[1]);
	if (argc > 2) K = atoi(argv[2]);
	if (argc > 3) Me = atoi(argv[3]);
	if (argc > 4) NPAIR = atoi(argv[4]);
	if (argc > 5) ITERS = atol(argv[5]);
	if (W < 1 || K < 2 || Me < 1 || NPAIR < 1 || NPAIR > 32) {
		fprintf(stderr, "bad config\n");
		return 2;
	}
	total = W * Me;

	rcu_register_thread();
	g_dc = dc_create(4096);
	g_final_anchor = calloc(total, sizeof(*g_final_anchor));
	printf("== stress_dcache_dirs (engine: %s) ==\n", dc_engine_name());
	printf("movers=%d anchors=%d movers/thr=%d pairs=%d iters=%ld total_movers=%d\n",
	       W, K, Me, NPAIR, ITERS, total);

	/* Anchors, then seed each mover subtree (mover dir + its leaf). */
	for (i = 0; i < K; i++) {
		char b[32];
		snprintf(b, sizeof(b), "/a%d", i);
		if (add_dir(b, DIR_ID_BASE + i)) { fprintf(stderr, "mkanchor\n"); return 2; }
	}
	for (i = 0; i < total; i++) {
		char b[64];
		snprintf(b, sizeof(b), "/a%d/M%d", i % K, i);
		if (add_dir(b, MOVER_ID_BASE + i)) { fprintf(stderr, "mkmover\n"); return 2; }
		snprintf(b, sizeof(b), "/a%d/M%d/L%d", i % K, i, i);
		if (add_dir(b, LEAF_ID_BASE + i)) { fprintf(stderr, "mkleaf\n"); return 2; }
	}
	/* Cycle pairs: private anchor /c<p> with siblings X and Y. */
	for (p = 0; p < NPAIR; p++) {
		char b[32];
		snprintf(b, sizeof(b), "/c%d", p);
		if (add_dir(b, DIR_ID_BASE + K + p)) { fprintf(stderr, "mkc\n"); return 2; }
		snprintf(b, sizeof(b), "/c%d/X", p);
		if (add_dir(b, DIR_ID_BASE + 100 + p * 2)) { fprintf(stderr, "mkX\n"); return 2; }
		snprintf(b, sizeof(b), "/c%d/Y", p);
		if (add_dir(b, DIR_ID_BASE + 101 + p * 2)) { fprintf(stderr, "mkY\n"); return 2; }
	}

	mt = calloc(W, sizeof(*mt));
	margs = calloc(W, sizeof(*margs));

	rcu_thread_offline();			/* main must not stall grace periods */
	for (i = 0; i < W; i++) { margs[i].w = i; pthread_create(&mt[i], NULL, mover, &margs[i]); }
	for (p = 0; p < NPAIR; p++) {
		cargs[p * 2].pair = p; cargs[p * 2].self = 0;
		cargs[p * 2 + 1].pair = p; cargs[p * 2 + 1].self = 1;
		pthread_create(&ct[p * 2], NULL, cycler, &cargs[p * 2]);
		pthread_create(&ct[p * 2 + 1], NULL, cycler, &cargs[p * 2 + 1]);
	}
	chka.iters = ITERS;
	pthread_create(&chk, NULL, checker, &chka);

	for (i = 0; i < W; i++) { pthread_join(mt[i], NULL); werrs += margs[i].errs; }
	for (p = 0; p < NPAIR * 2; p++) pthread_join(ct[p], NULL);
	pthread_join(chk, NULL);
	rcu_thread_online();

	rcu_quiescent_state();
	synchronize_rcu();

	/* Census: every mover + its leaf exactly once, at the recorded anchor. */
	memset(&c, 0, sizeof(c));
	c.total_movers = total;
	c.mover_seen = calloc(total, 1);
	c.leaf_seen = calloc(total, 1);
	dc_walk(g_dc, census_cb, &c);

	for (i = 0; i < total; i++) {
		struct dc_path pa;
		char b[64];
		uint64_t id = ~0ULL;

		if (c.mover_seen[i] != 1 || c.leaf_seen[i] != 1)
			anomaly++;
		snprintf(b, sizeof(b), "/a%d/M%d/L%d", g_final_anchor[i], i, i);
		if (dc_lookup(g_dc, mkp(&pa, b), &id) != DC_POSITIVE ||
		    id != LEAF_ID_BASE + (uint64_t) i)
			anomaly++;
	}
	/* Cycle pairs: X and Y of every pair must still be root-reachable
	 * (quiescent now, so a single check is definitive). */
	for (p = 0; p < NPAIR; p++) {
		if (!reachable_now(p, "X", "Y")) anomaly++;
		if (!reachable_now(p, "Y", "X")) anomaly++;
	}

	printf("mover rename errors    : %ld (expect 0)\n", werrs);
	printf("census stray ids       : %ld (expect 0)\n", c.stray);
	printf("misplaced/missing subs : %d (expect 0)\n", anomaly);
	printf("loop-check -EINVALs     : %ld (expect > 0: the check fired)\n", g_einval);
	printf("cycle-detached events  : %ld (expect 0)\n", g_cycle_detected);

	anomaly += (werrs != 0) + (c.stray != 0) + (g_cycle_detected != 0) +
		   (g_einval == 0);
	if (anomaly)
		printf("RESULT: FAIL\n");
	else
		printf("RESULT: PASS (subtrees conserved, no cycle formed, loop check active)\n");

	free(c.mover_seen); free(c.leaf_seen);
	free(mt); free(margs);
	dc_destroy(g_dc);
	free(g_final_anchor);
	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
