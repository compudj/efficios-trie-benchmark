// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * skiplist_detect -- read-only 1-2-3 balance DETECTION instruments for
 * urcu_txn_skiplist.  Step 1 of the self-heal prototype
 * (design/rcu-txn-skiplist-selfheal.md): quantify balance defects WITHOUT any
 * repair machinery.  Two instruments, both pure read-side:
 *
 *   sl_audit()          GROUND TRUTH.  Walk every level; histogram the gap = the
 *                       number of level-L-only nodes between two consecutive
 *                       level-(L+1) nodes.  A gap > GAP_MAX (=3) violates the
 *                       1-2-3 band.  This is the real structural defect, whatever
 *                       caused it (bad RNG, adversarial order, cluster deletes).
 *
 *   sl_lookup_detect()  WHAT A LOOKUP OBSERVES.  Mirror urcu_txn_skiplist_lookup_rcu,
 *                       counting the inner-while hops at each level.  A descent
 *                       drops to level L-1 as soon as cur.key >= key, so it walks
 *                       only the PREFIX of the L->L+1 gap that precedes @key: a
 *                       single lookup's hop count is a LOWER BOUND on the true gap
 *                       the key falls into.  That prefix property is exactly why
 *                       the auditor (ground truth) is kept alongside the on-descent
 *                       hook -- the hook says "how often reads trip", the auditor
 *                       says "how bad it really is".
 *
 * Neither instrument mutates the structure, so per the design's "balance is
 * performance, not correctness" property they cannot affect the map.  main() runs
 * a DETERMINISTIC single-thread correctness check of both instruments against
 * hand-built towers with KNOWN gaps.  The scale characterization (build a large
 * poisoned vs healthy structure, print the gap histogram + the fraction of
 * lookups that trip) is a benchmark and is GATED: it runs only when the env var
 * SL_DETECT_CHARACTERIZE is set (off by default -- the box stays free until then).
 * Knobs (env): SL_N size, SL_MODE poison|healthy, SL_STRIDE express spacing,
 * SL_LOOKUPS descent-sweep count, SL_SEED PRNG seed.
 *
 * Build (from the userspace-rcu-txn root, mirroring the skiplist TAP recipe):
 *   gcc -O2 -pthread -w -D_GNU_SOURCE -Iinclude -Isrc -Itests/utils -Itests/common \
 *     <this file> -o /tmp/sld \
 *     src/.libs/liburcu-qsbr.so src/.libs/liburcu-common.so tests/utils/libtap.a \
 *     -lpthread -Wl,-rpath,"$PWD/src/.libs"
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <urcu/compiler.h>
#include <urcu-qsbr.h>
#include <urcu-call-rcu.h>
#include <urcu/rcu-txn-skiplist.h>
#include <urcu/rcu-txn.h>

#include "tap.h"

#ifndef SL_GAP_MAX
#define SL_GAP_MAX	3u	/* 1-2-3 band: a gap of > 3 level-L nodes is a violation */
#endif

#define NR_TESTS	9

/* --------------------------------------------------------------------- */
/* Element type + comparator (same idiom as tests/unit/test_rcu_txn_skiplist.c) */
/* --------------------------------------------------------------------- */

struct node {
	unsigned long key;
	struct urcu_txn_skiplist_node sl;	/* last: flexible next[] */
};

static struct urcu_txn_domain g_dom;

static int node_cmp(struct urcu_txn_skiplist_node *n, void *key)
{
	unsigned long a = caa_container_of(n, struct node, sl)->key;
	unsigned long b = *(unsigned long *) key;

	return (a > b) - (a < b);
}

static struct node *node_alloc(unsigned long key, unsigned int toplevel)
{
	struct node *e = (struct node *) malloc(sizeof(*e)
			+ (toplevel + 1) * sizeof(struct urcu_txn_skiplist_node *));

	if (!e)
		abort();
	e->key = key;
	urcu_txn_skiplist_node_init(&e->sl, toplevel);
	return e;
}

/* Insert @key at an explicitly chosen tower height (deterministic build). */
static void sl_put(struct urcu_txn_skiplist *sl, unsigned long key,
		unsigned int toplevel)
{
	struct node *e = node_alloc(key, toplevel);

	if (urcu_txn_skiplist_add_rcu(sl, &e->sl, &key, &g_dom) != 0)
		abort();
}

static void sl_free_all(struct urcu_txn_skiplist *sl)
{
	struct urcu_txn_skiplist_node *n = urcu_txn_skiplist_next_rcu(sl->head, 0);

	while (n) {
		struct urcu_txn_skiplist_node *nx = urcu_txn_skiplist_next_rcu(n, 0);
		free(caa_container_of(n, struct node, sl));
		n = nx;
	}
	urcu_txn_skiplist_destroy(sl);
}

/* --------------------------------------------------------------------- */
/* Instrument 1: sl_audit -- ground-truth per-level gap histogram.        */
/* --------------------------------------------------------------------- */

struct sl_audit_result {
	unsigned int  top;					/* head->toplevel */
	unsigned long nodes;					/* level-0 node count */
	unsigned long gaps[URCU_TXN_SKIPLIST_MAX_LEVELS];	/* # segments at level L */
	unsigned long max_gap[URCU_TXN_SKIPLIST_MAX_LEVELS];	/* longest run at level L */
	unsigned long violations[URCU_TXN_SKIPLIST_MAX_LEVELS];	/* segments > SL_GAP_MAX */
	unsigned long total_violations;
	unsigned long worst_gap;				/* max over all levels */
};

/*
 * Follow next[L] from head: every node it visits exists at level L (toplevel >=
 * L).  A visited node with toplevel >= L+1 is the next level-(L+1) node -- it
 * CLOSES the current gap; a node with toplevel == L is an interior level-L node
 * that lengthens it.  The final segment (last express node -> end of list) is
 * closed after the walk.  Single-threaded / quiescent (no concurrent mutators).
 */
static void sl_audit(struct urcu_txn_skiplist *sl, struct sl_audit_result *out)
{
	unsigned int L;
	struct urcu_txn_skiplist_node *n;

	memset(out, 0, sizeof(*out));
	out->top = sl->head->toplevel;
	for (n = urcu_txn_skiplist_next_rcu(sl->head, 0); n;
			n = urcu_txn_skiplist_next_rcu(n, 0))
		out->nodes++;

	for (L = 0; L < out->top; L++) {			/* boundary L -> L+1 */
		struct urcu_txn_skiplist_node *cur;
		unsigned long run = 0;

		for (cur = urcu_txn_skiplist_next_rcu(sl->head, L); cur;
				cur = urcu_txn_skiplist_next_rcu(cur, L)) {
			if (cur->toplevel >= L + 1) {		/* closes the gap */
				out->gaps[L]++;
				if (run > out->max_gap[L])
					out->max_gap[L] = run;
				if (run > SL_GAP_MAX)
					out->violations[L]++;
				run = 0;
			} else {
				run++;				/* interior level-L node */
			}
		}
		/* tail segment: last level-(L+1) node -> end */
		out->gaps[L]++;
		if (run > out->max_gap[L])
			out->max_gap[L] = run;
		if (run > SL_GAP_MAX)
			out->violations[L]++;

		out->total_violations += out->violations[L];
		if (out->max_gap[L] > out->worst_gap)
			out->worst_gap = out->max_gap[L];
	}
}

/* --------------------------------------------------------------------- */
/* Instrument 2: sl_lookup_detect -- what a real lookup descent observes. */
/* --------------------------------------------------------------------- */

struct sl_descent {
	unsigned int  hops[URCU_TXN_SKIPLIST_MAX_LEVELS];	/* level-L hops the walk made */
	unsigned int  max_hops;					/* over all levels */
	int           worst_level;				/* argmax hops, or -1 if none */
	unsigned long total_hops;
	int           tripped;					/* any level with hops > SL_GAP_MAX */
	int           found;					/* key present at level 0 */
};

/*
 * Mirror urcu_txn_skiplist_lookup_rcu, counting the inner-while hops at each
 * level.  hops[L] = level-L nodes stepped over before dropping = the prefix of
 * the L->L+1 gap containing @key that lies before @key.  A trip (hops > GAP_MAX)
 * is what a self-healing reader would act on.  Call within an RCU read section.
 */
static void sl_lookup_detect(struct urcu_txn_skiplist *sl, void *key,
		struct sl_descent *d)
{
	struct urcu_txn_skiplist_node *pred = sl->head, *cur;
	int level;

	memset(d, 0, sizeof(*d));
	d->worst_level = -1;
	for (level = (int) sl->head->toplevel; level >= 0; level--) {
		unsigned int hops = 0;

		cur = urcu_txn_skiplist_next_rcu(pred, (unsigned int) level);
		while (cur != NULL && sl->cmp(cur, key) < 0) {
			pred = cur;
			cur = urcu_txn_skiplist_next_rcu(pred, (unsigned int) level);
			hops++;
		}
		d->hops[level] = hops;
		d->total_hops += hops;
		if (hops > d->max_hops) {
			d->max_hops = hops;
			d->worst_level = level;
		}
		if (hops > SL_GAP_MAX)
			d->tripped = 1;
	}
	cur = urcu_txn_skiplist_next_rcu(pred, 0);
	d->found = (cur != NULL && sl->cmp(cur, key) == 0);
}

/* --------------------------------------------------------------------- */
/* Correctness tests: hand-built towers with KNOWN gaps.                  */
/* --------------------------------------------------------------------- */

#define POISON_N	48u	/* keys 0..47 */
#define POISON_STRIDE	8u	/* express (toplevel 1) at multiples of 8 => gap 7 */

/* Build the poisoned structure: only multiples of STRIDE get a level-1 tower. */
static void build_poisoned(struct urcu_txn_skiplist *sl)
{
	unsigned long k;

	assert(!urcu_txn_skiplist_init(sl, node_cmp));
	for (k = 0; k < POISON_N; k++)
		sl_put(sl, k, (k % POISON_STRIDE == 0) ? 1u : 0u);
}

/* Build the same keys with a healthy geometric height distribution. */
static void build_healthy(struct urcu_txn_skiplist *sl)
{
	unsigned long k, r = 0x9e3779b97f4a7c15UL;

	assert(!urcu_txn_skiplist_init(sl, node_cmp));
	for (k = 0; k < POISON_N; k++) {
		unsigned int lvl;
		r = r * 6364136223846793005UL + 1442695040888963407UL;
		lvl = urcu_txn_skiplist_random_level(r >> 17);
		sl_put(sl, k, lvl);
	}
}

static void audit_tests(void)
{
	struct urcu_txn_skiplist sl;
	struct sl_audit_result a;

	build_poisoned(&sl);
	rcu_read_lock();
	sl_audit(&sl, &a);
	rcu_read_unlock();

	/*
	 * Level 0, express at 0,8,16,24,32,40 (six of them): the six interior
	 * segments [0,8) .. [40,end) each hold exactly STRIDE-1 == 7 nodes; the
	 * head->0 segment holds 0.  So max gap 7, and 6 segments violate (> 3).
	 */
	ok(a.nodes == POISON_N, "audit: level-0 node count == %u (got %lu)",
		POISON_N, a.nodes);
	ok(a.max_gap[0] == POISON_STRIDE - 1,
		"audit: level-0 worst gap == %u (got %lu)",
		POISON_STRIDE - 1, a.max_gap[0]);
	ok(a.violations[0] == 6,
		"audit: level-0 has 6 gaps > GAP_MAX=%u (got %lu)",
		SL_GAP_MAX, a.violations[0]);
	ok(a.worst_gap == POISON_STRIDE - 1,
		"audit: overall worst gap == %u (got %lu)",
		POISON_STRIDE - 1, a.worst_gap);

	sl_free_all(&sl);
}

static void descent_tests(void)
{
	struct urcu_txn_skiplist sl;
	struct sl_audit_result a;
	struct sl_descent d;
	unsigned long k;
	int prefix_le_gap = 1, some_full = 0;

	build_poisoned(&sl);
	rcu_read_lock();
	sl_audit(&sl, &a);

	/*
	 * Key at the FAR boundary of a gap: lookup(8) steps over 1..7 at level 0
	 * (7 hops) before landing on express node 8 -- the descent observes the
	 * FULL gap.  max_gap[0] == 7 confirms it.
	 */
	k = POISON_STRIDE;
	sl_lookup_detect(&sl, &k, &d);
	ok(d.found && d.hops[0] == a.max_gap[0] && d.tripped,
		"descent: lookup(%lu) observes the full gap (hops[0]=%u == max_gap=%lu)",
		k, d.hops[0], a.max_gap[0]);

	/*
	 * Key mid-gap: lookup(7) stops AT node 7 having stepped over 1..6 -- a
	 * strict PREFIX (6 < 7), demonstrating a single descent under-counts.
	 */
	k = POISON_STRIDE - 1;
	sl_lookup_detect(&sl, &k, &d);
	ok(d.found && d.hops[0] < a.max_gap[0] && d.hops[0] == POISON_STRIDE - 2,
		"descent: lookup(%lu) observes a prefix (hops[0]=%u < gap=%lu)",
		k, d.hops[0], a.max_gap[0]);

	/* Over EVERY present key: descent level-0 hops never exceed the true gap. */
	for (k = 0; k < POISON_N; k++) {
		unsigned long key = k;
		sl_lookup_detect(&sl, &key, &d);
		if (d.hops[0] > a.max_gap[0])
			prefix_le_gap = 0;
		if (d.hops[0] == a.max_gap[0])
			some_full = 1;
	}
	ok(prefix_le_gap && some_full,
		"descent: every lookup's hops[0] <= true max gap, some reach it");
	rcu_read_unlock();

	sl_free_all(&sl);
}

/* The auditor must DISCRIMINATE a poisoned structure from a healthy one. */
static void discriminate_test(void)
{
	struct urcu_txn_skiplist poisoned, healthy;
	struct sl_audit_result pa, ha;

	build_poisoned(&poisoned);
	build_healthy(&healthy);
	rcu_read_lock();
	sl_audit(&poisoned, &pa);
	sl_audit(&healthy, &ha);
	rcu_read_unlock();

	ok(pa.total_violations >= 6 && ha.total_violations < pa.total_violations,
		"discriminate: poisoned %lu violations >> healthy %lu",
		pa.total_violations, ha.total_violations);

	sl_free_all(&poisoned);
	sl_free_all(&healthy);
}

/* --------------------------------------------------------------------- */
/* Scale characterization -- GATED by env SL_DETECT_CHARACTERIZE (a bench). */
/* --------------------------------------------------------------------- */

static uint64_t xs64(uint64_t x)
{
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return x;
}

static unsigned long env_ul(const char *name, unsigned long dflt)
{
	const char *s = getenv(name);
	return s && *s ? strtoul(s, NULL, 0) : dflt;
}

static void characterize(void)
{
	unsigned long N      = env_ul("SL_N", 100000);
	unsigned long stride = env_ul("SL_STRIDE", 8);
	unsigned long nlook  = env_ul("SL_LOOKUPS", 200000);
	uint64_t      seed   = env_ul("SL_SEED", 0x9e3779b97f4a7c15UL);
	const char   *mode   = getenv("SL_MODE");
	int           poison = !(mode && strcmp(mode, "healthy") == 0);
	struct urcu_txn_skiplist sl;
	struct sl_audit_result a;
	unsigned long k, tripped = 0, hop_sum = 0, hop_max = 0;
	uint64_t r = seed;
	unsigned int L;

	assert(!urcu_txn_skiplist_init(&sl, node_cmp));
	for (k = 0; k < N; k++) {
		unsigned int lvl;
		if (poison) {
			lvl = (k % stride == 0) ? 1u : 0u;	/* degenerate/biased RNG model */
		} else {
			r = xs64(r);
			lvl = urcu_txn_skiplist_random_level(r >> 11);
		}
		sl_put(&sl, k, lvl);
	}

	rcu_read_lock();
	sl_audit(&sl, &a);
	rcu_read_unlock();

	printf("# characterize mode=%s N=%lu stride=%lu\n",
		poison ? "poison" : "healthy", N, stride);
	printf("# level  gaps  max_gap  violations(>%u)\n", SL_GAP_MAX);
	for (L = 0; L < a.top; L++) {
		if (a.gaps[L] == 1 && a.max_gap[L] == 0)
			continue;			/* empty level */
		printf("  %5u %6lu %8lu %10lu\n",
			L, a.gaps[L], a.max_gap[L], a.violations[L]);
	}
	printf("# worst_gap=%lu total_violations=%lu nodes=%lu\n",
		a.worst_gap, a.total_violations, a.nodes);

	/* Descent sweep: how often would a real lookup TRIP the GAP_MAX threshold? */
	rcu_read_lock();
	for (k = 0; k < nlook; k++) {
		struct sl_descent d;
		unsigned long key;
		r = xs64(r);
		key = r % N;
		sl_lookup_detect(&sl, &key, &d);
		if (d.tripped)
			tripped++;
		hop_sum += d.max_hops;
		if (d.max_hops > hop_max)
			hop_max = d.max_hops;
	}
	rcu_read_unlock();
	printf("# lookups=%lu tripped=%lu (%.1f%%) mean_max_hops=%.2f peak_hops=%lu\n",
		nlook, tripped, 100.0 * (double) tripped / (double) nlook,
		(double) hop_sum / (double) nlook, hop_max);

	sl_free_all(&sl);
}

int main(void)
{
	if (getenv("SL_DETECT_CHARACTERIZE")) {
		urcu_txn_domain_init(&g_dom);
		rcu_register_thread();
		characterize();
		rcu_unregister_thread();
		return 0;
	}

	plan_tests(NR_TESTS);
	urcu_txn_domain_init(&g_dom);
	rcu_register_thread();

	audit_tests();		/* 4 */
	descent_tests();	/* 3 */
	discriminate_test();	/* 1 */

	/*
	 * The 9th test: the instruments are read-only, so re-auditing after all
	 * the lookups must be identical to the first audit -- a cheap proof that
	 * detection did not perturb the structure ("balance is performance, not
	 * correctness": detection cannot even change balance).
	 */
	{
		struct urcu_txn_skiplist sl;
		struct sl_audit_result a1, a2;
		unsigned long k;

		build_poisoned(&sl);
		rcu_read_lock();
		sl_audit(&sl, &a1);
		for (k = 0; k < POISON_N; k++) {
			struct sl_descent d;
			unsigned long key = k;
			sl_lookup_detect(&sl, &key, &d);
		}
		sl_audit(&sl, &a2);
		rcu_read_unlock();
		ok(memcmp(&a1, &a2, sizeof(a1)) == 0,
			"read-only: audit unchanged after a full lookup sweep");
		sl_free_all(&sl);
	}

	rcu_unregister_thread();
	return exit_status();
}
