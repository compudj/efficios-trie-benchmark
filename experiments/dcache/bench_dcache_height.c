// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * bench_dcache_height -- the ADVERSARIAL move-height sweep for the per-node arm.
 *
 * The S3 role-split sweep (bench_dcache.c) moves only LEAVES.  That is the
 * per-node counter's BEST case: a leaf has fan-in 1, so bumping the moved entry's
 * host generation invalidates essentially no concurrent walk.  This bench measures
 * the honest opposite -- how the per-node localization DEGRADES as the moved node
 * climbs the tree -- so the S3 headline is bounded, not cherry-picked.
 *
 * Geometry -- a balanced B-ary forest, one band per writer
 * -------------------------------------------------------
 * Each of `writers` writers owns a band `/b{w}`, a balanced tree of branching
 * `branch` and depth `tree-depth` D: every interior node has B children named
 * 0..B-1, and all B^D leaves are present.  A leaf is the full digit path
 * /b{w}/g1/g2/.../gD -- so ANY such path is a real leaf and a reader walk always
 * descends the full depth D (+1 for the band), sampling every ancestor's counter.
 *
 * The swept variable -- move HEIGHT
 * ---------------------------------
 * Writer w repeatedly RENAME_EXCHANGEs two sibling subtrees at height H inside its
 * own band: it picks a random parent P at band-depth D-H-1 and swaps two of P's
 * children (each the root of a height-H subtree of B^H leaves).  H=0 swaps two
 * leaves (the S3 leaf case); H=D-1 swaps two subtrees just under the band root
 * (nearly the whole band).  Bands are disjoint, so a single owner's exchange never
 * fails and never conflicts with another writer -- the reader path is the only
 * variable, exactly as in the S3 split sweep.
 *
 * A move at height H is an ancestor of B^H of the band's B^D... wait, B^(D)? the
 * band has B^D leaves; a height-H node dominates B^H of them, a fraction
 * B^H / B^D of the band's reader walks.  So the per-node gen-bump invalidates a
 * fraction ~B^(H-D) of walks -- tiny at H=0 (per-node localizes), ->1 as H->D
 * (every walk passes through it, per-node degenerates to the global counter).  The
 * global rename_gen and the seqlock rename_lock, bumped by EVERY move regardless of
 * height, are the flat reference the per-node curve descends toward.
 *
 * Metric: reader Mlookups/s vs move-height, three arms (seqlock / txn-global /
 * txn-per-node).  Every run is gated on namespace conservation (a dc_walk census
 * shows each leaf id exactly once -- exchanges are permutations, so the census
 * stays exact without tracking the permutation).  Reader torn-read id-checks are
 * census-only here: an exchange permutes which id sits at a path, so a per-op
 * id==expected check is meaningless (as on the address builds of bench_dcache).
 *
 * Usage: bench_dcache_height --writers W --nthreads N --move-height H
 *          [--branch B] [--tree-depth D] [--duration MS]
 *          [--cpulist c0,..] [--cpustride N] [--nbuckets N] [--quiesce N]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE
#endif

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <urcu/uatomic.h>
#include <urcu-qsbr.h>
#include <urcu-call-rcu.h>

#include "dcache.h"

extern unsigned long dc_seq_walk_retries __attribute__((weak));

/* ---- knobs -------------------------------------------------------------- */
static int    nthreads   = 40;
static int    nwriters   = 8;
static int    branch     = 2;		/* B: children per interior node */
static int    tree_depth = 8;		/* D: band depth (leaves at depth D) */
static int    move_height = -1;		/* H: swept; REQUIRED (0..D-1) */
static long   duration_ms = 1000;
static int    cpustride  = 1;
static int   *cpulist    = NULL;
static int    cpulist_len = 0;
static unsigned int nbuckets = 4096;
static unsigned int quiesce_mask = 15;

static long   g_bandleaves = 0;		/* B^D: leaves per band */
static long   g_total      = 0;		/* writers * B^D: total leaves */
#define DIR_ID_BASE 1000000ULL		/* interior dirs carry ids >= this */

static struct dcache *g_dc;
static struct qstr *g_band_q;		/* [writers]  "b{w}" */
static struct qstr *g_digit_q;		/* [branch]   "0".."B-1" */

/* ---- start gate / timing ------------------------------------------------ */
#define GOFLAG_INIT 0
#define GOFLAG_RUN  1
#define GOFLAG_STOP 2
static volatile int goflag = GOFLAG_INIT;
static int nthreads_running;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void pin_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void) sched_setaffinity(0, sizeof(set), &set);
}

static void parse_cpulist(const char *s)
{
	int cap = 16;

	cpulist = malloc(cap * sizeof(*cpulist));
	cpulist_len = 0;
	while (*s) {
		char *end;
		long v = strtol(s, &end, 10);

		if (end == s)
			break;
		if (cpulist_len == cap) {
			cap *= 2;
			cpulist = realloc(cpulist, cap * sizeof(*cpulist));
		}
		cpulist[cpulist_len++] = (int) v;
		s = end;
		while (*s == ',' || *s == ' ')
			s++;
	}
}

static inline uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return (*s = x);
}

/* ---- path construction (from precomputed qstrs) ------------------------- */

/* Full leaf path for band w, leaf index x in [0, B^D): /b{w}/g1/.../gD, digits
 * most-significant first so the DFS build order == the numeric leaf index. */
static void mk_leaf_path(struct dc_path *p, int w, long x)
{
	long div = g_bandleaves;
	int k;

	dc_path_reset(p);
	p->comp[p->ndepth++] = g_band_q[w];
	for (k = 0; k < tree_depth; k++) {
		div /= branch;
		p->comp[p->ndepth++] = g_digit_q[(x / div) % branch];
	}
}

/* Parent-of-a-height-H-pair path: /b{w} + `plen` digits from `digs`. */
static void mk_parent_path(struct dc_path *p, int w, const int *digs, int plen)
{
	int k;

	dc_path_reset(p);
	p->comp[p->ndepth++] = g_band_q[w];
	for (k = 0; k < plen; k++)
		p->comp[p->ndepth++] = g_digit_q[digs[k]];
}

/* ---- worker ------------------------------------------------------------- */

struct warg {
	int id, cpu;
	long long nlookups;	/* reader: completed walks */
	long long nabsent;	/* reader: walks that went ABSENT (should be ~0) */
	long long nrenames;	/* writer: successful exchanges */
	long long errs;		/* writer: exchange failures (should be 0) */
};

static void *worker(void *arg)
{
	struct warg *me = arg;
	/* Writers are the LAST nwriters ids, so the reader set keeps the same low
	 * cpus regardless of writer count (stable NUMA placement across the sweep). */
	int is_writer = me->id >= nthreads - nwriters;
	int wband = is_writer ? me->id - (nthreads - nwriters) : 0;
	int plen = tree_depth - move_height - 1;	/* band-depth of the pair's parent */
	uint64_t s = 0x9e3779b97f4a7c15ULL ^ ((uint64_t) (me->id + 1) * 0x100000001b3ULL);
	struct call_rcu_data *crdp;
	int *digs = malloc((plen > 0 ? plen : 1) * sizeof(*digs));
	long long ops = 0;

	pin_cpu(me->cpu);
	dc_register_thread();
	crdp = create_call_rcu_data(URCU_CALL_RCU_RT, me->cpu);
	if (crdp)
		set_thread_call_rcu_data(crdp);

	uatomic_inc(&nthreads_running);
	rcu_thread_offline();
	while (uatomic_read(&goflag) == GOFLAG_INIT)
		(void) poll(NULL, 0, 1);
	rcu_thread_online();

	while (uatomic_read(&goflag) == GOFLAG_RUN) {
		if (is_writer) {
			/* Exchange two sibling height-H subtrees under a random
			 * parent at band-depth `plen`, inside this writer's band. */
			struct dc_path a, b;
			int i, ca, cb;

			for (i = 0; i < plen; i++)
				digs[i] = (int) (xrand(&s) % (uint64_t) branch);
			ca = (int) (xrand(&s) % (uint64_t) branch);
			cb = (int) (xrand(&s) % (uint64_t) (branch - 1));
			if (cb >= ca)
				cb++;			/* cb != ca, uniform */
			mk_parent_path(&a, wband, digs, plen);
			a.comp[a.ndepth++] = g_digit_q[ca];
			mk_parent_path(&b, wband, digs, plen);
			b.comp[b.ndepth++] = g_digit_q[cb];
			if (dc_rename_exchange(g_dc, &a, &b) == 0)
				me->nrenames++;
			else
				me->errs++;
		} else {
			/* Full-depth walk to a random leaf in a random band. */
			int w = (int) (xrand(&s) % (uint64_t) nwriters);
			long x = (long) (xrand(&s) % (uint64_t) g_bandleaves);
			struct dc_path p;
			uint64_t id = 0;

			mk_leaf_path(&p, w, x);
			if (dc_lookup(g_dc, &p, &id) == DC_POSITIVE)
				me->nlookups++;
			else
				me->nabsent++;
		}
		if ((++ops & quiesce_mask) == 0)
			dc_quiescent();
	}

	free(digs);
	dc_unregister_thread();
	if (crdp) {
		set_thread_call_rcu_data(NULL);
		call_rcu_data_free(crdp);
	}
	return NULL;
}

/* ---- setup: build the balanced B-ary bands ------------------------------ */

static uint64_t g_dir_id = DIR_ID_BASE;

/* Recursively create band `w`'s subtree under path `p` (already added).  At
 * band-depth D the node is a leaf; leaves are numbered in DFS (== numeric) order
 * by *leaf_ctr, so id b{w}/x = w*B^D + x. */
static void build_subtree(struct dc_path *p, int w, int dcur, long *leaf_ctr)
{
	int c;

	if (dcur == tree_depth)
		return;				/* leaf: no children */
	for (c = 0; c < branch; c++) {
		uint64_t id;

		p->comp[p->ndepth++] = g_digit_q[c];
		if (dcur + 1 == tree_depth)
			id = (uint64_t) (w * g_bandleaves + (*leaf_ctr)++);
		else
			id = g_dir_id++;
		if (dc_add(g_dc, p, id)) {
			fprintf(stderr, "build: add at band %d depth %d failed\n",
				w, dcur + 1);
			exit(2);
		}
		build_subtree(p, w, dcur + 1, leaf_ctr);
		p->ndepth--;
	}
}

static void build_tree(void)
{
	char buf[DC_NAME_MAX];
	struct dc_path p;
	int w, c;

	g_band_q = calloc((size_t) nwriters, sizeof(*g_band_q));
	g_digit_q = calloc((size_t) branch, sizeof(*g_digit_q));
	for (c = 0; c < branch; c++) {
		snprintf(buf, sizeof(buf), "%d", c);
		dc_qstr_init(&g_digit_q[c], buf);
	}
	for (w = 0; w < nwriters; w++) {
		long leaf_ctr = 0;

		snprintf(buf, sizeof(buf), "b%d", w);
		dc_qstr_init(&g_band_q[w], buf);
		dc_path_reset(&p);
		p.comp[p.ndepth++] = g_band_q[w];
		if (dc_add(g_dc, &p, g_dir_id++)) {
			fprintf(stderr, "build: add band b%d failed\n", w);
			exit(2);
		}
		build_subtree(&p, w, 0, &leaf_ctr);
	}
}

static void warm(void)
{
	int w;
	long x;

	for (w = 0; w < nwriters; w++)
		for (x = 0; x < g_bandleaves; x++) {
			struct dc_path p;
			uint64_t id = 0;

			mk_leaf_path(&p, w, x);
			(void) dc_lookup(g_dc, &p, &id);
		}
}

/* ---- conservation census ------------------------------------------------ */

struct census { uint8_t *seen; long total, stray; };

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= DIR_ID_BASE)
		return;
	if (id >= (uint64_t) c->total) { c->stray++; return; }
	if (c->seen[id]++)
		c->stray++;
}

/* ---- main --------------------------------------------------------------- */

static void usage(const char *p)
{
	fprintf(stderr,
	    "usage: %s --writers W --nthreads N --move-height H\n"
	    "         [--branch B] [--tree-depth D] [--duration MS]\n"
	    "         [--cpulist c0,..] [--cpustride N] [--nbuckets N] [--quiesce N]\n"
	    "  Balanced B-ary band per writer (B^D leaves each); writers RENAME_EXCHANGE\n"
	    "  two sibling subtrees at height H; readers walk random full leaf paths.\n"
	    "  H=0 moves leaves (the S3 best case); H=D-1 moves near the band root.\n", p);
	exit(2);
}

int main(int argc, char **argv)
{
	pthread_t *tid;
	struct warg *wa;
	struct census c;
	long long t0, t1, total_lk = 0, total_rn = 0, total_absent = 0, total_err = 0;
	unsigned long retries0 = 0, retries1 = 0;
	double secs, mlk_s, mrn_s;
	int i, anomaly = 0;
	long d;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--nthreads"))         nthreads = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--writers"))     nwriters = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--move-height"))  move_height = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--branch"))      branch = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--tree-depth"))  tree_depth = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--duration"))    duration_ms = atol(argv[++i]);
		else if (!strcmp(argv[i], "--cpustride"))   cpustride = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cpulist"))     parse_cpulist(argv[++i]);
		else if (!strcmp(argv[i], "--nbuckets"))    nbuckets = (unsigned) atoi(argv[++i]);
		else if (!strcmp(argv[i], "--quiesce")) {
			int q = atoi(argv[++i]);
			if (q < 1 || (q & (q - 1)) != 0)
				usage(argv[0]);
			quiesce_mask = (unsigned) (q - 1);
		}
		else usage(argv[0]);
	}
	if (nthreads < 2 || nwriters < 1 || nwriters >= nthreads || branch < 2 ||
	    tree_depth < 1 || move_height < 0 || move_height >= tree_depth)
		usage(argv[0]);
	if (1 + tree_depth > DC_PATH_MAX) {
		fprintf(stderr, "tree-depth %d + band exceeds DC_PATH_MAX %d\n",
			tree_depth, DC_PATH_MAX);
		exit(2);
	}
	if (cpulist && nthreads > cpulist_len) {
		fprintf(stderr, "--cpulist has %d cpus but --nthreads=%d\n",
			cpulist_len, nthreads);
		exit(2);
	}
	for (g_bandleaves = 1, d = 0; d < tree_depth; d++)
		g_bandleaves *= branch;			/* B^D */
	g_total = (long) nwriters * g_bandleaves;
	if (g_total >= (long) DIR_ID_BASE) {
		fprintf(stderr, "namespace %ld too large (writers*B^D >= %llu)\n",
			g_total, DIR_ID_BASE);
		exit(2);
	}

	rcu_register_thread();
	g_dc = dc_create(nbuckets);

	printf("== bench_dcache_height (engine: %s) ==\n", dc_engine_name());
	printf("threads=%d writers=%d readers=%d branch=%d tree_depth=%d "
	       "move_height=%d band_leaves=%ld total_leaves=%ld duration_ms=%ld\n",
	       nthreads, nwriters, nthreads - nwriters, branch, tree_depth,
	       move_height, g_bandleaves, g_total, duration_ms);

	build_tree();
	warm();

	tid = calloc(nthreads, sizeof(*tid));
	wa = calloc(nthreads, sizeof(*wa));
	for (i = 0; i < nthreads; i++) {
		wa[i].id = i;
		wa[i].cpu = cpulist ? cpulist[i] : i * cpustride;
		pthread_create(&tid[i], NULL, worker, &wa[i]);
	}
	while (uatomic_read(&nthreads_running) < nthreads)
		(void) poll(NULL, 0, 1);
	cmm_smp_mb();

	if (&dc_seq_walk_retries)
		retries0 = uatomic_read(&dc_seq_walk_retries);
	rcu_thread_offline();
	t0 = now_ns();
	uatomic_set(&goflag, GOFLAG_RUN);
	(void) poll(NULL, 0, (int) duration_ms);
	uatomic_set(&goflag, GOFLAG_STOP);
	t1 = now_ns();

	for (i = 0; i < nthreads; i++) {
		pthread_join(tid[i], NULL);
		total_lk     += wa[i].nlookups;
		total_rn     += wa[i].nrenames;
		total_absent += wa[i].nabsent;
		total_err    += wa[i].errs;
	}
	rcu_thread_online();
	if (&dc_seq_walk_retries)
		retries1 = uatomic_read(&dc_seq_walk_retries);

	rcu_quiescent_state();
	synchronize_rcu();
	rcu_barrier();

	secs = (t1 - t0) / 1e9;
	mlk_s = secs > 0 ? (double) total_lk / secs / 1e6 : 0.0;
	mrn_s = secs > 0 ? (double) total_rn / secs / 1e6 : 0.0;

	memset(&c, 0, sizeof(c));
	c.total = g_total;
	c.seen = calloc((size_t) g_total, 1);
	dc_walk(g_dc, census_cb, &c);
	for (d = 0; d < g_total; d++)
		if (c.seen[d] != 1)
			anomaly++;

	printf("duration (s): %g\n", secs);
	printf("LOOKUP  lookups: %lld  Mlookups/s: %g  absent: %lld\n",
	       total_lk, mlk_s, total_absent);
	printf("RENAME  exchanges: %lld  Mrenames/s: %g  errors: %lld\n",
	       total_rn, mrn_s, total_err);
	if (&dc_seq_walk_retries) {
		unsigned long dr = retries1 - retries0;

		printf("MECH    walk-retries: %lu  (%.4f per lookup)\n",
		       dr, total_lk ? (double) dr / (double) total_lk : 0.0);
	} else {
		printf("MECH    walk-retries: N/A (engine never retries a walk)\n");
	}

	anomaly += (total_err != 0) + (c.stray != 0);
	if (anomaly)
		printf("CONSERVATION FAILED: %d anomalies (stray/dup %ld, rename-err %lld) "
		       "-- run is CORRUPT, ignore the numbers above\n",
		       anomaly, c.stray, total_err);
	else
		printf("CHECK   conservation: OK (all %ld leaves accounted for)\n", g_total);

	free(c.seen);
	free(tid); free(wa);
	dc_destroy(g_dc);
	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
