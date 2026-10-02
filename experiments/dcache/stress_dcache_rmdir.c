// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * stress_dcache_rmdir.c -- INSERT-vs-UNLINK stress: adds of children under a
 * directory racing the unlink of that directory.
 *
 * Why it exists.  A directory's unlink frees its host, and must not free one an
 * add has just published a child under.  Each engine excludes that: the bucket
 * lock's unlink takes the victim directory's own child head (which every add
 * under it takes) and re-checks it empty under the lock, and dc_add re-checks
 * the parent entry is still alive under its locks; the txn engine's unlink
 * commit SEALS the victim's child head, the slot an add's insert writes, so the
 * two commits contend on one slot.  Until this harness, no concurrent test
 * unlinked a directory anyone was adding under -- every harness unlinks files,
 * or empty directories nobody adds under -- so neither exclusion had ever been
 * seen to fire: with the bucket lock's taken out, every gate that builds that
 * engine still passed.
 *
 * Workload.  P fixed parents /p<i>, each with V victim directories /p<i>/v<j>.
 * UNLINKERS pick a victim, dc_unlink it (succeeds only when it is empty) and
 * re-add it at once, holding a per-victim mutex so the census knows each
 * victim's state; adders never take that mutex.  ADDERS pick a victim and add
 * their own child /p<i>/v<j>/a<w> under it, then remove it, so a victim keeps
 * going empty while other adds are in flight -- the race window.
 *
 * The detector is deterministic: a child pins its parent (unlink answers
 * -ENOTEMPTY), so once dc_add returned 0 the adder's child MUST still be
 * reachable at its path and its unlink MUST succeed.  A child published under a
 * directory whose unlink went through is not: it is hashed under a freed host,
 * unreachable, and its name answers ABSENT (or, once the allocator hands the
 * freed host's address to the re-added victim, -EEXIST).  Run under ASan too.
 *
 * The gate (make check-rmdir) runs every engine arm's correct build twice --
 * at natural timings, and with -DDC_TEST_UNLINK_DELAY_US=50, which sleeps
 * inside a directory unlink's critical section so that nearly every unlink
 * races an add (the unlinks refused non-empty rise 5-12x) -- and each MUTATION
 * ARM once, which must FAIL, or the harness proves nothing:
 *   txn         -DDC_TXN_NO_PARENT_SEAL   (no seal on the victim's child head)
 *   bucket lock -DDC_TEST_NO_RMDIR_LOCK   (unlink skips the victim's child head)
 *               -DDC_TEST_NO_ADD_ALIVE    (dc_add skips its parent-alive check)
 * Measured (2026-10-01, defaults): each mutation arm fails 5/5 runs at natural
 * timings -- ~7-11k lost children per run without the seal, ~350 without the
 * unlink's lock, ~7-8k without the add's check -- and every correct build
 * passes 5/5, under ASan and TSAN.  ASan stays silent on the mutants: the
 * orphaned child is never dereferenced, so the lost-child check is the
 * detector.
 *
 * The same traffic races the LRU, which is why the seqlock baseline runs it
 * too: an adder's walk re-arms each victim directory it passes through while
 * unlinkers kill and re-add those victims.  dc_lru_check() then verifies, at
 * quiescence, that no killed dentry was re-armed (and freed while listed) and
 * that no dentry was linked twice -- the two races seqlock's LRU had until
 * 2026-10-02.  Its -DDC_TEST_RETAIN_DELAY_US=N widens the re-arm window, and
 * -DDC_TEST_SEQ_LRU_NO_SEAL / -DDC_TEST_SEQ_LRU_ADD_LATE restore the two bugs.
 *
 * Usage: ./stress_dcache_rmdir [unlinkers [adders [parents [victims [iters]]]]]
 * Exit 0 = no lost child, census clean; 1 = anomaly.
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

static int U = 2;			/* unlinker threads */
static int A = 6;			/* adder threads */
static int P = 2;			/* parents /p0../p(P-1) */
static int V = 4;			/* victim directories per parent */
static long ITERS = 20000;		/* adds per adder */

static struct dcache *g_dc;
static pthread_mutex_t *g_vlock;	/* [P*V] unlinkers only */
static int g_stop;

#define PARENT_ID_BASE	1000000ULL
#define VICTIM_ID_BASE	2000000ULL
#define CHILD_ID_BASE	3000000ULL

extern const int dc_lookup_id_is_address __attribute__((weak));
static inline int id_is_address(void)
{
	return &dc_lookup_id_is_address && dc_lookup_id_is_address;
}

static void mkpath(struct dc_path *p, const char *fmt, int a, int b, int c)
{
	char buf[DC_NAME_MAX * 3];

	snprintf(buf, sizeof(buf), fmt, a, b, c);
	if (dc_path_parse(p, buf) != 0) {
		fprintf(stderr, "bad path %s\n", buf);
		exit(2);
	}
}

/* A QSBR thread blocked while online holds off every grace period. */
static void victim_lock(int v)
{
	rcu_thread_offline();
	pthread_mutex_lock(&g_vlock[v]);
	rcu_thread_online();
}

struct uarg { int u; long unlinks, busy, errs; };

static void *unlinker(void *arg)
{
	struct uarg *ua = arg;
	uint64_t s = 0x9e3779b97f4a7c15ULL ^ ((uint64_t) (ua->u + 1) * 0x100000001b3ULL);
	struct call_rcu_data *crdp;

	dc_register_thread();
	crdp = create_call_rcu_data(0, -1);
	if (crdp)
		set_thread_call_rcu_data(crdp);
	while (!uatomic_load(&g_stop, CMM_RELAXED)) {
		int v = (int) xrange(&s, (uint32_t) (P * V));
		struct dc_path p;
		int ret;

		mkpath(&p, "/p%d/v%d", v / V, v % V, 0);
		victim_lock(v);
		ret = dc_unlink(g_dc, &p);
		if (ret == 0) {
			ua->unlinks++;
			if (dc_add(g_dc, &p, VICTIM_ID_BASE + (uint64_t) v) != 0)
				ua->errs++;	/* we hold its mutex: must succeed */
		} else if (ret == -ENOTEMPTY) {
			ua->busy++;
		} else {
			ua->errs++;
		}
		pthread_mutex_unlock(&g_vlock[v]);
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

struct aarg { int w; long added, gone, lost, eexist, errs; };

static void *adder(void *arg)
{
	struct aarg *aa = arg;
	uint64_t s = 0x2545f4914f6cdd1dULL ^ ((uint64_t) (aa->w + 101) * 0x100000001b3ULL);
	uint64_t id = CHILD_ID_BASE + (uint64_t) aa->w;
	long it;

	dc_register_thread();
	for (it = 0; it < ITERS; it++) {
		int v = (int) xrange(&s, (uint32_t) (P * V));
		struct dc_path c;
		uint64_t got = ~0ULL;
		int ret;

		mkpath(&c, "/p%d/v%d/a%d", v / V, v % V, aa->w);
		ret = dc_add(g_dc, &c, id);
		if (ret == -ENOENT) {
			aa->gone++;		/* victim absent or dying: fine */
		} else if (ret == -EEXIST) {
			aa->eexist++;		/* we own this name: never fine */
		} else if (ret != 0) {
			aa->errs++;
		} else {
			aa->added++;
			/* the child pins the victim: both must hold */
			if (dc_lookup(g_dc, &c, &got) != DC_POSITIVE ||
			    (!id_is_address() && got != id))
				aa->lost++;
			else if (dc_unlink(g_dc, &c) != 0)
				aa->lost++;
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

struct census { long parents, victims, children, stray; };

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= CHILD_ID_BASE)
		c->children++;
	else if (id >= VICTIM_ID_BASE)
		c->victims++;
	else if (id >= PARENT_ID_BASE)
		c->parents++;
	else
		c->stray++;
}

int main(int argc, char **argv)
{
	pthread_t *ut, *at;
	struct uarg *ua;
	struct aarg *aa;
	struct census c;
	long unlinks = 0, busy = 0, uerrs = 0;
	long added = 0, gone = 0, lost = 0, eexist = 0, aerrs = 0, lru_bad;
	int i, anomaly = 0;

	if (argc > 1) U = atoi(argv[1]);
	if (argc > 2) A = atoi(argv[2]);
	if (argc > 3) P = atoi(argv[3]);
	if (argc > 4) V = atoi(argv[4]);
	if (argc > 5) ITERS = atol(argv[5]);
	setvbuf(stdout, NULL, _IOLBF, 0);	/* a sanitizer exit skips stdio's flush */
	if (U < 1 || A < 1 || P < 1 || V < 1) {
		fprintf(stderr, "bad config\n");
		return 2;
	}
	rcu_register_thread();
	g_dc = dc_create(4096);
	g_vlock = calloc(P * V, sizeof(*g_vlock));
	printf("== stress_dcache_rmdir (engine: %s) ==\n", dc_engine_name());
	printf("unlinkers=%d adders=%d parents=%d victims/parent=%d adds/adder=%ld\n",
	       U, A, P, V, ITERS);
	for (i = 0; i < P; i++) {
		struct dc_path p;

		mkpath(&p, "/p%d", i, 0, 0);
		if (dc_add(g_dc, &p, PARENT_ID_BASE + (uint64_t) i)) {
			fprintf(stderr, "mkdir /p%d failed\n", i);
			return 2;
		}
	}
	for (i = 0; i < P * V; i++) {
		struct dc_path p;

		pthread_mutex_init(&g_vlock[i], NULL);
		mkpath(&p, "/p%d/v%d", i / V, i % V, 0);
		if (dc_add(g_dc, &p, VICTIM_ID_BASE + (uint64_t) i)) {
			fprintf(stderr, "mkdir victim %d failed\n", i);
			return 2;
		}
	}

	ut = calloc(U, sizeof(*ut));
	at = calloc(A, sizeof(*at));
	ua = calloc(U, sizeof(*ua));
	aa = calloc(A, sizeof(*aa));
	for (i = 0; i < U; i++) {
		ua[i].u = i;
		pthread_create(&ut[i], NULL, unlinker, &ua[i]);
	}
	for (i = 0; i < A; i++) {
		aa[i].w = i;
		pthread_create(&at[i], NULL, adder, &aa[i]);
	}
	rcu_thread_offline();		/* main only blocks in join */
	for (i = 0; i < A; i++) {
		pthread_join(at[i], NULL);
		added += aa[i].added;
		gone += aa[i].gone;
		lost += aa[i].lost;
		eexist += aa[i].eexist;
		aerrs += aa[i].errs;
	}
	uatomic_store(&g_stop, 1, CMM_RELAXED);
	for (i = 0; i < U; i++) {
		pthread_join(ut[i], NULL);
		unlinks += ua[i].unlinks;
		busy += ua[i].busy;
		uerrs += ua[i].errs;
	}
	rcu_thread_online();

	/* drain: grace periods, then the callback barrier, twice */
	rcu_quiescent_state();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();

	lru_bad = dc_lru_check(g_dc);
	memset(&c, 0, sizeof(c));
	dc_walk(g_dc, census_cb, &c);
	if (c.parents != P || c.victims != (long) P * V || c.children || c.stray)
		anomaly++;
	for (i = 0; i < P * V; i++) {
		struct dc_path p;
		uint64_t id = ~0ULL;

		mkpath(&p, "/p%d/v%d", i / V, i % V, 0);
		if (dc_lookup(g_dc, &p, &id) != DC_POSITIVE ||
		    (!id_is_address() && id != VICTIM_ID_BASE + (uint64_t) i))
			anomaly++;
	}
	printf("victim unlinks       : %ld (refused non-empty: %ld)\n", unlinks, busy);
	printf("child adds           : %ld ok, %ld refused (victim gone)\n", added, gone);
	printf("LOST children        : %ld (expect 0)\n", lost);
	printf("-EEXIST on own name  : %ld (expect 0)\n", eexist);
	printf("errors               : %ld unlinker, %ld adder (expect 0)\n", uerrs, aerrs);
	printf("LRU check            : %ld anomalies (expect 0)\n", lru_bad);
	printf("census               : %ld parents, %ld victims, %ld children, %ld stray"
	       " (expect %d, %d, 0, 0)\n", c.parents, c.victims, c.children, c.stray,
	       P, P * V);
	free(ut);
	free(at);
	free(ua);
	free(aa);
	free(g_vlock);
	if (!unlinks || !added) {
		printf("RESULT: VACUOUS (no victim unlink or no child add completed)\n");
		return 1;
	}
	if (lost || eexist || uerrs || aerrs || anomaly || lru_bad) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS (no child published under an unlinked directory)\n");
	return 0;
}
