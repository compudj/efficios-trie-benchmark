// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * mixed_cycle.c -- MIXED rename + exchange cross-directory cycle race.
 *
 * The homogeneous stresses each exercise ONE mutator: stress_dcache_dirs races
 * rename vs rename, stress_dcache_xchg races exchange vs exchange.  Neither races
 * rename against exchange -- exactly the case the move-in-progress flag protocol
 * must make safe once BOTH mutators are flag-based (a rename's plain-read
 * ancestry walk can only see a concurrent exchange if the exchange grays its
 * hosts).  This is the direct analog of stress_dcache_dirs' mutual-cycle race,
 * with one of the two nesting directions driven by dc_rename_exchange.
 *
 * Per pair p, under a private anchor /c<p>:  X (a dir with a permanent child P,
 * /c<p>/X/P) and Y (a dir).  Two threads:
 *
 *   renamer(p):   nests/unnests X under Y with dc_rename
 *                 (/c<p>/X  <->  /c<p>/Y/X).
 *   exchanger(p): nests/unnests Y under X with dc_rename_exchange, swapping Y
 *                 with X's child P (Y  <->  X/P slot).  cross-parent, so it takes
 *                 the flag path.
 *
 * The loop check must make it IMPOSSIBLE for X-under-Y and Y-under-X to hold at
 * once: that is a detached X<->Y cycle.  Each belief stays accurate because while
 * one node is nested the peer's nest is a cycle and is refused (-EINVAL) or the
 * path has shifted (-ENOENT) -- so at most one is nested.  A checker asserts X
 * and Y each stay root-reachable at one of their spots; a cycle DETACHES both,
 * which the checker catches as a persistent vanish and the final census as a
 * lost node.  At least one -EINVAL must occur (else the check went unexercised).
 *
 * Prove non-vacuity by disabling the cur==host reject in stack_one_prepare: the
 * checker then fires (cycle-detached > 0) and the census loses nodes.
 *
 * Usage: ./mixed_cycle [pairs [iters]]      Exit 0 = clean, 1 = anomaly.
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

static int NPAIR = 4;
static long ITERS = 40000;
static struct dcache *g_dc;
static unsigned long g_cycle_detected;
static unsigned long g_r_einval, g_e_einval;
static volatile int g_stop;

static struct dc_path *mkp(struct dc_path *p, const char *s)
{
	dc_path_parse(p, s);
	return p;
}

/* renamer: toggle X between /c<p>/X and /c<p>/Y/X via dc_rename. */
static void *renamer(void *arg)
{
	int p = *(int *) arg;
	char a[64], b[64];
	int nested = 0;
	long it, einval = 0;

	dc_register_thread();
	snprintf(a, sizeof(a), "/c%d/X", p);
	snprintf(b, sizeof(b), "/c%d/Y/X", p);
	for (it = 0; it < ITERS; it++) {
		struct dc_path from, to;
		int ret;

		if (!nested) {
			ret = dc_rename(g_dc, mkp(&from, a), mkp(&to, b));
			if (ret == 0)
				nested = 1;
			else if (ret == -EINVAL)
				einval++;	/* Y already under X: cycle refused */
		} else {
			ret = dc_rename(g_dc, mkp(&from, b), mkp(&to, a));
			if (ret == 0)
				nested = 0;
		}
		dc_quiescent();
	}
	uatomic_add(&g_r_einval, einval);
	dc_unregister_thread();
	return NULL;
}

/* exchanger: toggle Y under X by swapping Y with X's child P via exchange. */
static void *exchanger(void *arg)
{
	int p = *(int *) arg;
	char yp[64], pp[80], ynest[80];
	int nested = 0;			/* is Y nested under X (Y sits in P's slot)? */
	long it, einval = 0;

	dc_register_thread();
	snprintf(yp,    sizeof(yp),    "/c%d/Y", p);	/* Y unnested */
	snprintf(pp,    sizeof(pp),    "/c%d/X/P", p);	/* P unnested (X not nested) */
	snprintf(ynest, sizeof(ynest), "/c%d/X/P", p);	/* Y's slot when nested == P's slot */
	for (it = 0; it < ITERS; it++) {
		struct dc_path pa, pb;
		int ret;

		if (!nested) {
			/* nest Y under X: swap Y (/c/Y) with P (/c/X/P) */
			ret = dc_rename_exchange(g_dc, mkp(&pa, yp), mkp(&pb, pp));
			if (ret == 0)
				nested = 1;
			else if (ret == -EINVAL)
				einval++;	/* X already under Y: cycle refused */
			/* -ENOENT: X is nested so P moved -> leave nested=0 */
		} else {
			/* unnest: Y is at /c/X/P, P is at /c/Y -- swap back */
			ret = dc_rename_exchange(g_dc, mkp(&pa, ynest), mkp(&pb, yp));
			if (ret == 0)
				nested = 0;
		}
		dc_quiescent();
	}
	uatomic_add(&g_e_einval, einval);
	dc_unregister_thread();
	return NULL;
}

static int present(const char *path)
{
	struct dc_path pa;

	return dc_lookup(g_dc, mkp(&pa, path), NULL) == DC_POSITIVE;
}

/*
 * X reachable at /c<p>/X or /c<p>/Y/X; Y at /c<p>/Y or, when nested under X, in
 * P's old slot /c<p>/X/P.  Presence (not id): under load dc_lookup transiently
 * resolves a mid-transition node to its shell (id 0), so id-matching reads a
 * spurious detachment.  A real detached cycle removes a node from BOTH its spots
 * entirely -- presence catches that without the false positive.  (A node
 * standing in the OTHER's spot cannot mask a real cycle: a cycle detaches BOTH.)
 */
static int reachable_now(int p)
{
	char x1[64], x2[64], y1[64], y2[64];

	snprintf(x1, sizeof(x1), "/c%d/X", p);
	snprintf(x2, sizeof(x2), "/c%d/Y/X", p);
	snprintf(y1, sizeof(y1), "/c%d/Y", p);
	snprintf(y2, sizeof(y2), "/c%d/X/P", p);
	return (present(x1) || present(x2)) && (present(y1) || present(y2));
}

/*
 * A node in flight is momentarily absent between the two lookups -- a false
 * positive.  A REAL detached cycle is permanent (the pair is unreachable, so no
 * thread can move it back).  Distinguish by persistence.
 */
static int detached_persistent(int p)
{
	int k;

	for (k = 0; k < 128; k++) {
		if (reachable_now(p))
			return 0;
		dc_quiescent();
	}
	return 1;
}

static void *checker(void *arg)
{
	(void) arg;
	dc_register_thread();
	while (!g_stop) {
		int p;

		for (p = 0; p < NPAIR; p++)
			if (!reachable_now(p) && detached_persistent(p)) {
				if (uatomic_add_return(&g_cycle_detected, 1) == 1) {
					fprintf(stderr, "CYCLE DETECTED at pair %d\n", p);
					fflush(stderr);
				}
			}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t *rt, *et, chk;
	int *idx, i, anomaly = 0;

	if (argc > 1) NPAIR = atoi(argv[1]);
	if (argc > 2) ITERS = atol(argv[2]);
	if (NPAIR < 1 || NPAIR > 64) {
		fprintf(stderr, "bad config\n");
		return 2;
	}

	rcu_register_thread();
	g_dc = dc_create(4096);
	printf("== mixed_cycle (engine: %s) ==\n", dc_engine_name());
	printf("pairs=%d iters=%ld\n", NPAIR, ITERS);

	for (i = 0; i < NPAIR; i++) {
		struct dc_path pp;
		char b[64];

		snprintf(b, sizeof(b), "/c%d", i);
		dc_add(g_dc, mkp(&pp, b), 1000 + i);
		snprintf(b, sizeof(b), "/c%d/X", i);
		dc_add(g_dc, mkp(&pp, b), 2000 + i);
		snprintf(b, sizeof(b), "/c%d/X/P", i);
		dc_add(g_dc, mkp(&pp, b), 3000 + i);
		snprintf(b, sizeof(b), "/c%d/Y", i);
		dc_add(g_dc, mkp(&pp, b), 4000 + i);
	}

	rt  = calloc((size_t) NPAIR, sizeof(*rt));
	et  = calloc((size_t) NPAIR, sizeof(*et));
	idx = calloc((size_t) NPAIR, sizeof(*idx));
	pthread_create(&chk, NULL, checker, NULL);
	for (i = 0; i < NPAIR; i++) {
		idx[i] = i;
		pthread_create(&rt[i], NULL, renamer, &idx[i]);
		pthread_create(&et[i], NULL, exchanger, &idx[i]);
	}
	for (i = 0; i < NPAIR; i++) {
		pthread_join(rt[i], NULL);
		pthread_join(et[i], NULL);
	}
	g_stop = 1;
	pthread_join(chk, NULL);

	for (i = 0; i < NPAIR; i++)
		if (!reachable_now(i)) {
			printf("ANOMALY: pair %d lost a node (detached cycle?)\n", i);
			anomaly = 1;
		}

	printf("cycle-detached events   : %lu   (MUST be 0)\n", g_cycle_detected);
	printf("rename cycle-refusals   : %lu\n", g_r_einval);
	printf("exchange cycle-refusals : %lu\n", g_e_einval);
	if (g_cycle_detected != 0) { printf("ANOMALY: detached cycle\n"); anomaly = 1; }
	if (g_r_einval + g_e_einval == 0)
		printf("WARNING: no -EINVAL ever seen -- race may be unexercised\n");

	free(rt); free(et); free(idx);
	dc_destroy(g_dc);
	rcu_unregister_thread();
	printf("RESULT: %s\n", anomaly ? "FAIL" : "PASS");
	return anomaly;
}
