// SPDX-FileCopyrightText: 2026 EfficiOS Inc.
// SPDX-License-Identifier: MIT
/*
 * repro_rename_dup: two dentries spelled alike at a RENAME DESTINATION.
 *
 * dc_rename checks the destination name with __child_lookup() and then calls
 * stack_shell(), which publishes the shell later.  Between those two the caller
 * holds nothing, so it is a CHECK-THEN-ACT: several renames aimed at ONE
 * destination name all pass the test and all publish, and the destination
 * bucket ends up with two dentries carrying the same (parent, name).  A lookup
 * then resolves whichever the chain reaches first while a child-list walk
 * descends the other -- the same permanent index disagreement dc_add's version
 * of this bug produced.
 *
 * ⚠ WHY NO EXISTING TEST FINDS IT.  bench_dcache_churn has no renames at all,
 * and stress_dcache's renames are SINGLE-OWNER: each writer moves its own gid,
 * so two writers never aim at one destination name.  That is exactly the
 * uncovered axis, and it is why this defect outlived the dc_add one.
 *
 * MODEL.  W writers, each owning a source name of its own under /d0.  Every
 * round they all try to rename their own source to the SAME destination
 * /d1/X<round>.  At most ONE may succeed; the rest must get -EEXIST.  Then the
 * destination directory is enumerated and any name appearing twice is the
 * defect -- dc_readdir walks the CHILD LIST, so it sees both copies even though
 * dc_lookup can only ever reach one.
 *
 * The winner's entry is deliberately left in place: a duplicate is permanent
 * (nothing unhashes the loser), but unlinking the destination between rounds
 * would remove one copy and hide it.
 *
 * Build the control with -DDC_NO_RENAME_DEST_RECHECK; it MUST report
 * duplicates, or this repro proves nothing about the fixed build.
 */
#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <urcu-qsbr.h>
#include <urcu-call-rcu.h>

#include "dcache.h"

#ifndef NWRITERS
#define NWRITERS	8
#endif
#ifndef RUNSECS
#define RUNSECS		5
#endif

static struct dcache *g_dc;
static volatile int g_stop;
static long g_dupseen;			/* duplicates observed, atomically */
static long g_wins, g_eexist, g_other;

#define NDEST	8			/* destination names contended for */

struct warg { int idx; long wins, eexist, other, dup; };

static void mk2(struct dc_path *p, const char *a, const char *b)
{
	dc_path_reset(p);
	dc_path_push(p, a);
	dc_path_push(p, b);
}

/* Count how many children of @dir carry @name.  dc_readdir walks the CHILD
 * LIST, so it sees BOTH copies of a duplicate even though dc_lookup can only
 * ever reach one -- which is the whole point. */
struct cnt { const char *want; uint32_t len; int n; };

static void cnt_cb(uint64_t id, const struct qstr *nm, void *arg)
{
	struct cnt *c = arg;

	(void) id;
	if (nm->len == c->len && !memcmp(nm->name, c->want, c->len))
		c->n++;
}

static int count_named(const char *dirname, const char *name)
{
	struct dc_path p;
	struct cnt c;

	c.want = name;
	c.len = (uint32_t) strlen(name);
	c.n = 0;
	dc_path_reset(&p);
	dc_path_push(&p, dirname);
	if (dc_readdir(g_dc, &p, cnt_cb, &c) < 0)
		return -1;
	return c.n;
}

static uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return *s = x;
}

/*
 * NO BARRIER, deliberately.  An earlier cut synchronised the writers per round
 * with pthread_barrier_wait(), which parks threads RCU-ONLINE and stalls every
 * grace period -- the run then deadlocked at exit in
 * urcu_qsbr_unregister_thread() and looked exactly like a dc_rename livelock.
 * Hammering freely removes the hazard entirely instead of bracketing it.
 */
static void *writer(void *arg)
{
	struct warg *me = arg;
	uint64_t s = 0x9e3779b97f4a7c15ULL ^ (uint64_t) (me->idx + 1);
	char src[64];

	snprintf(src, sizeof(src), "s%d", me->idx);
	dc_register_thread();
	while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
		struct dc_path from, to;
		char dest[64];
		int r;

		mk2(&from, "d0", src);
		(void) dc_add(g_dc, &from, (uint64_t) (1000 + me->idx));

		snprintf(dest, sizeof(dest), "X%u",
			 (unsigned) (xrand(&s) % NDEST));
		mk2(&to, "d1", dest);
		r = dc_rename(g_dc, &from, &to);
		if (r == 0) {
			int n;

			me->wins++;
			/*
			 * CHECK AT THE MOMENT OF SUCCESS, before freeing the
			 * name again.  A duplicate is permanent, but the unlink
			 * below would remove one copy and hide it.
			 */
			n = count_named("d1", dest);
			if (n > 1) {
				me->dup++;
				__atomic_fetch_add(&g_dupseen, 1,
						   __ATOMIC_RELAXED);
			}
			(void) dc_unlink(g_dc, &to);	/* free the name again */
		} else if (r == -EEXIST) {
			me->eexist++;
		} else {
			me->other++;
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

int main(void)
{
	pthread_t th[NWRITERS];
	struct warg wa[NWRITERS];
	struct dc_path p;
	struct timespec ts = { RUNSECS, 0 };
	int i;

	rcu_register_thread();
	g_dc = dc_create(4096);
	if (!g_dc) { fprintf(stderr, "dc_create\n"); return 2; }
	dc_register_thread();

	dc_path_reset(&p); dc_path_push(&p, "d0");
	if (dc_add(g_dc, &p, 1000000ULL)) { fprintf(stderr, "mkdir d0\n"); return 2; }
	dc_path_reset(&p); dc_path_push(&p, "d1");
	if (dc_add(g_dc, &p, 1000001ULL)) { fprintf(stderr, "mkdir d1\n"); return 2; }

	printf("== repro_rename_dup: %d writers, %d contended destination names, "
	       "%ds ==\n", NWRITERS, NDEST, RUNSECS);
	for (i = 0; i < NWRITERS; i++) {
		wa[i].idx = i;
		wa[i].wins = wa[i].eexist = wa[i].other = wa[i].dup = 0;
		pthread_create(&th[i], NULL, writer, &wa[i]);
	}
	/* ⚠ OFFLINE across the sleep AND the join: main must not sit
	 * RCU-online-and-not-quiescent while writers need grace periods. */
	rcu_thread_offline();
	nanosleep(&ts, NULL);
	__atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
	for (i = 0; i < NWRITERS; i++) {
		pthread_join(th[i], NULL);
		g_wins += wa[i].wins; g_eexist += wa[i].eexist;
		g_other += wa[i].other;
	}
	rcu_thread_online();

	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();

	printf("renames: %ld won, %ld -EEXIST, %ld other\n",
	       g_wins, g_eexist, g_other);
	printf("DUPLICATE destination names observed: %ld\n", g_dupseen);
	if (g_dupseen || g_other) {
		printf("RESULT: FAIL (%ld duplicates, %ld unexpected errors)\n",
		       g_dupseen, g_other);
		return 1;
	}
	printf("RESULT: PASS (no destination name published twice)\n");
	return 0;
}
