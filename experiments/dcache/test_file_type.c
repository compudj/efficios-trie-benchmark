// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * test_file_type.c -- the file/directory distinction: ENOTDIR enforcement, and
 * that a FILE rename/move (whose walk-causality bump the txn engine skips) is
 * sound.  Single-threaded semantics + a concurrent file-rename conservation run.
 *
 * The skip is sound because a file has no children (ENOTDIR), so it is never an
 * interior waypoint and its rename can misdirect no reader.  The concurrent run
 * renames FILE leaves under readers and checks conservation -- if the skipped
 * bump were unsafe, a reader would see a wrong id or a leaf would go missing.
 */
#define _GNU_SOURCE
#define _LGPL_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <urcu-qsbr.h>
#include "dcache.h"

extern const int dc_lookup_id_is_address __attribute__((weak));
static inline int id_is_addr(void){ return &dc_lookup_id_is_address && dc_lookup_id_is_address; }

static int fails;
#define CK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static struct dc_path *P(struct dc_path *p, const char *s)
{ if (dc_path_parse(p, s)) { fprintf(stderr, "bad %s\n", s); exit(2); } return p; }

static void test_semantics(void)
{
	struct dcache *dc = dc_create(1024);
	struct dc_path a, b, c, d;

	CK(!dc_add(dc, P(&a, "/A"), 1), "mkdir /A (dir)");
	CK(!dc_add_file(dc, P(&b, "/A/f"), 2), "create file /A/f");

	/* a child under a file must be ENOTDIR */
	CK(dc_add(dc, P(&c, "/A/f/x"), 3) == -ENOTDIR, "dir under file -> ENOTDIR");
	CK(dc_add_file(dc, P(&c, "/A/f/y"), 4) == -ENOTDIR, "file under file -> ENOTDIR");

	/* a child under a directory is fine */
	CK(!dc_add(dc, P(&c, "/A/d"), 5), "mkdir /A/d");
	CK(!dc_add_file(dc, P(&d, "/A/d/g"), 6), "file /A/d/g under dir");

	/* file RENAME (same dir) and MOVE (cross dir): still resolvable */
	CK(!dc_add(dc, P(&a, "/B"), 7), "mkdir /B");
	CK(!dc_rename(dc, P(&a, "/A/f"), P(&b, "/A/f2")), "rename file /A/f -> /A/f2");
	CK(dc_lookup(dc, P(&a, "/A/f2"), NULL) == DC_POSITIVE, "/A/f2 present");
	CK(dc_lookup(dc, P(&a, "/A/f"), NULL) == DC_ABSENT, "/A/f gone");
	CK(!dc_rename(dc, P(&a, "/A/f2"), P(&b, "/B/f2")), "move file /A/f2 -> /B/f2");
	CK(dc_lookup(dc, P(&a, "/B/f2"), NULL) == DC_POSITIVE, "/B/f2 present");

	dc_destroy(dc);
}

/* ---- concurrent file-rename conservation ------------------------------ */

#define NW 4
#define NR 4
#define NLEAF 16
#define ITERS 30000
static struct dcache *g;
static int go, stop;

static void mkfp(struct dc_path *p, int dir, int gid)
{ char b[64]; snprintf(b, sizeof(b), "/d%d/f%d", dir, gid); P(p, b); }

static void *wr(void *arg)
{
	long w = (long) arg; uint64_t s = 0x1234 ^ (w + 1) * 0x9e37;
	int *pos = calloc(NLEAF, sizeof(int));
	long it;
	dc_register_thread();
	for (int i = 0; i < NLEAF; i++) pos[i] = (w * NLEAF + i) % 8;
	while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) sched_yield();
	for (it = 0; it < ITERS && !__atomic_load_n(&stop, __ATOMIC_ACQUIRE); it++) {
		s ^= s << 13; s ^= s >> 7; s ^= s << 17;
		int i = s % NLEAF, gid = w * NLEAF + i, nd = (s >> 8) % 8;
		if (nd == pos[i]) nd = (nd + 1) % 8;
		struct dc_path from, to;
		mkfp(&from, pos[i], gid); mkfp(&to, nd, gid);
		if (dc_rename(g, &from, &to) == 0) pos[i] = nd;
		dc_quiescent();
	}
	dc_unregister_thread(); free(pos); return NULL;
}

static void *rd(void *arg)
{
	long r = (long) arg; uint64_t s = 0xbeef ^ (r + 1) * 0x2545;
	long wrong = 0;
	dc_register_thread();
	while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) sched_yield();
	while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
		s ^= s << 13; s ^= s >> 7; s ^= s << 17;
		int gid = s % (NW * NLEAF), dr = (s >> 8) % 8;
		struct dc_path p; uint64_t id = ~0ULL;
		mkfp(&p, dr, gid);
		if (dc_lookup(g, &p, &id) == DC_POSITIVE &&
		    !id_is_addr() && id != (uint64_t) gid)
			wrong++;
		dc_quiescent();
	}
	dc_unregister_thread();
	return (void *) wrong;
}


static int test_concurrent(void)
{
	pthread_t wt[NW], rt[NR]; struct dc_path p;
	long wrong = 0; int miss = 0;
	g = dc_create(1u << 16);
	for (int d = 0; d < 8; d++) { char b[32]; snprintf(b, sizeof(b), "/d%d", d);
		if (dc_add(g, P(&p, b), 1000 + d)) return 2; }
	for (long w = 0; w < NW; w++) for (int i = 0; i < NLEAF; i++) {
		int gid = w * NLEAF + i; mkfp(&p, gid % 8, gid);
		if (dc_add_file(g, &p, (uint64_t) gid)) { fprintf(stderr, "seed file failed\n"); return 2; } }
	__atomic_store_n(&go, 0, __ATOMIC_RELAXED); __atomic_store_n(&stop, 0, __ATOMIC_RELAXED);
	for (long i = 0; i < NW; i++) pthread_create(&wt[i], NULL, wr, (void *) i);
	for (long i = 0; i < NR; i++) pthread_create(&rt[i], NULL, rd, (void *) i);
	__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
	struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
	rcu_thread_offline();
	for (int i = 0; i < NW; i++) pthread_join(wt[i], NULL);
	for (int i = 0; i < NR; i++) { void *rv; pthread_join(rt[i], &rv); wrong += (long) rv; }
	rcu_thread_online();
	synchronize_rcu(); rcu_barrier();
	/* every seeded file must still resolve, somewhere */
	for (int gid = 0; gid < NW * NLEAF; gid++) {
		int found = 0;
		for (int d = 0; d < 8 && !found; d++) { mkfp(&p, d, gid);
			if (dc_lookup(g, &p, NULL) == DC_POSITIVE) found = 1; }
		if (!found) miss++;
	}
	CK(wrong == 0, "concurrent reader wrong-id: %ld", wrong);
	CK(miss == 0, "files missing after churn: %d", miss);
	dc_destroy(g);
	return 0;
}

int main(void)
{
	rcu_register_thread();
	printf("== test_file_type (engine: %s) ==\n", dc_engine_name());
	test_semantics();
	test_concurrent();
	printf("failures: %d\nRESULT: %s\n", fails, fails ? "FAIL" : "PASS");
	rcu_unregister_thread();
	return fails ? 1 : 0;
}
