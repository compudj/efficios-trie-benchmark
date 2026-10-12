// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * stress_dcache_scan.c -- SUBTREE SCAN vs MOVES: dc_scan() of a directory
 * while renames and exchanges move objects around below it.
 *
 * Why it exists.  dc_lookup() and dc_dentry_path() read one path, and two
 * passes over that path make the read exact.  dc_scan() -- the kernel's
 * d_walk(), behind shrink_dcache_parent(), d_invalidate() and
 * path_has_submounts() -- reads a SET: every object below a directory.  Its
 * guarantee is about what it did not reach (dcache.h): an object that stays
 * below the root for the whole call must be reported.  A move that takes an
 * object from a directory the scan has yet to read into one it has already
 * read breaks that without disturbing anything the scan holds, so nothing the
 * scan can re-test tells it.  Until this harness nothing here scanned a tree
 * while anything moved: dc_walk() runs at quiescence only, and no test looks
 * at what a dc_readdir() under renames left out.
 *
 * Workload.  One root /r with D directories /r/d<i>.  Each MOVER owns F files
 * and one directory g<m> holding a file h<m>; all of them are TRACKED, and
 * the mover keeps every one of them below /r for the whole run.  It picks
 * among:
 *   - a file to another directory;
 *   - its directory g<m>, with what is in it, to another directory;
 *   - an exchange of two of its files, in two directories or in one;
 *   - a file renamed where it is;
 *   - a FRESH directory n<m> created in some /r/d<i>, and a file moved into
 *     it; next time round the file leaves it and n<m> is unlinked (one
 *     operation in ten, or the share given as the last argument);
 *   - a file of no account moved between /r and /o, outside the root.
 * Nothing else touches a mover's objects, so every one of its operations must
 * succeed.  SCANNERS call dc_scan(/r) in a loop.
 *
 * The detector is deterministic: every tracked id must be in every scan.  A
 * missing one is a MISS -- an object that was below /r throughout and was not
 * reported.  An id reported twice is counted, and is allowed.
 *
 * Three rounds, to see when a move has to tell the scans:
 *   A  moves, no scan has ever run;
 *   B  moves and scans;
 *   C  moves, the scans are over.
 * The sequence a validated scan watches (dc_scan_stats(): signals) is printed
 * after each.  On the seq arm it moves with every move that changes a parent
 * and every exchange.  On the mark arm it should not move in A, and in C only
 * as often as B left marks behind.
 *
 * The gate (make check-scan) runs the correct arms at natural timings and
 * with -DDC_TEST_SCAN_MOVE_DELAY_US=N, which sleeps in a move between its look
 * at the marks and its commit, and each MUTATION ARM once, which must FAIL or
 * the harness proves nothing:
 *   -DDC_SCAN                      the soft arm: one pass, no validation
 *   -DDC_TEST_SCAN_NO_INHERIT      a new directory is not born marked
 *   -DDC_TEST_SCAN_NO_XCHG         an exchange tells the scans nothing
 *   -DDC_TEST_SCAN_NO_LOCKWAIT     the scan does not wait for the child head
 *                                  (with the move delay: the window is a few
 *                                  instructions otherwise)
 *
 * Why the last argument.  A move into a fresh directory is caught through the
 * mark that the directory was born with, and a miss by that road takes a pass
 * that holds both the creation and the move and no other signal.  With the
 * usual mix nearly every other operation signals: the arm without the
 * inherited mark then loses 1 to 4 objects in a run, and none at all with a
 * single mover, whose passes are too short to hold both.  With six operations
 * in ten on the fresh directory and 400000 per mover it loses a hundred or
 * more (make check-scan runs it that way).
 *
 * Usage: ./stress_dcache_scan [movers [scanners [dirs [files [moves [fresh%]]]]]]
 * Exit 0 = no miss, census clean; 1 = anomaly.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <urcu-qsbr.h>
#include <urcu/uatomic.h>

#include "dcache.h"
#include "dcache_bench_rand.h"

static int M = 3;			/* mover threads */
static int S = 2;			/* scanner threads */
static int D = 6;			/* directories /r/d0../r/d(D-1) */
static int F = 4;			/* tracked files per mover */
static long MOVES = 40000;		/* operations per mover, over the 3 rounds */
static int FRESH_PCT = 10;		/* of them, on the fresh directory */

static struct dcache *g_dc;
static int g_stop;
static struct dc_path g_root;

#define ROOT_ID		1000000ULL
#define OUT_ID		1000001ULL
#define DIR_ID_BASE	2000000ULL
#define TRACK_ID_BASE	3000000ULL
#define FRESH_ID_BASE	4000000ULL
#define NOISE_ID_BASE	5000000ULL

/* Tracked per mover: F files, then g<m>, then h<m>. */
#define PER_MOVER	(F + 2)
#define NTRACKED	(M * PER_MOVER)

static void mkpath(struct dc_path *p, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void mkpath(struct dc_path *p, const char *fmt, ...)
{
	char buf[DC_NAME_MAX * 4];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (dc_path_parse(p, buf) != 0) {
		fprintf(stderr, "bad path %s\n", buf);
		exit(2);
	}
}

struct mover {
	int m;
	uint64_t rng;
	long todo;			/* operations this round */
	/* Where each file is: a directory index, or -1 for the fresh one. */
	int *loc;
	unsigned char *alt;		/* which of its two names it has */
	int g_loc;			/* where g<m> is */
	int fresh_in;			/* which directory holds n<m>, or -1 */
	int fresh_k;			/* which file's name is in it */
	int noise_in;			/* where z<m> is, or -1 for /o */
	long moves, dirmoves, xchgs, renames, fresh, noise, errs;
};

static void file_path(const struct mover *mv, int k, struct dc_path *p)
{
	const char *fmt = mv->alt[k] ? "F%d_%d" : "f%d_%d";
	char name[DC_NAME_MAX];

	snprintf(name, sizeof(name), fmt, mv->m, k);
	if (mv->loc[k] >= 0)
		mkpath(p, "/r/d%d/%s", mv->loc[k], name);
	else
		mkpath(p, "/r/d%d/n%d/%s", mv->fresh_in, mv->m, name);
}

/* Somewhere else than @here among the D directories. */
static int other_dir(struct mover *mv, int here)
{
	int j = (int) xrange(&mv->rng, (uint32_t) (D - 1));

	return j >= here ? j + 1 : j;
}

/* Move file @k to directory @to; out of the fresh directory, which then goes. */
static void move_file(struct mover *mv, int k, int to)
{
	struct dc_path from, dst;
	int was_fresh = mv->loc[k] < 0;

	file_path(mv, k, &from);
	mv->loc[k] = to;
	file_path(mv, k, &dst);
	if (dc_rename(g_dc, &from, &dst) != 0)
		mv->errs++;
	mv->moves++;
	if (was_fresh) {
		mkpath(&from, "/r/d%d/n%d", mv->fresh_in, mv->m);
		if (dc_unlink(g_dc, &from) != 0)
			mv->errs++;
		mv->fresh_in = -1;
	}
}

static void *mover(void *arg)
{
	struct mover *mv = arg;
	struct call_rcu_data *crdp;
	long it;

	dc_register_thread();
	crdp = create_call_rcu_data(0, -1);
	if (crdp)
		set_thread_call_rcu_data(crdp);
	for (it = 0; it < mv->todo; it++) {
		int on_fresh = (int) xrange(&mv->rng, 100) < FRESH_PCT;
		unsigned int op = xrange(&mv->rng, 9);
		int k = (int) xrange(&mv->rng, (uint32_t) F);
		struct dc_path a, b;

		if (on_fresh && mv->fresh_in >= 0) {	/* out of it again */
			move_file(mv, mv->fresh_k,
				  other_dir(mv, mv->fresh_in));
		} else if (on_fresh) {		/* into a fresh directory */
			mv->fresh_in = (int) xrange(&mv->rng, (uint32_t) D);
			mv->fresh_k = k;
			mkpath(&a, "/r/d%d/n%d", mv->fresh_in, mv->m);
			if (dc_add(g_dc, &a, FRESH_ID_BASE + (uint64_t) mv->m) != 0)
				mv->errs++;
			file_path(mv, k, &a);
			mv->loc[k] = -1;
			file_path(mv, k, &b);
			if (dc_rename(g_dc, &a, &b) != 0)
				mv->errs++;
			mv->fresh++;
		} else if (op < 4) {		/* a file to another directory */
			int here = mv->loc[k] >= 0 ? mv->loc[k] : mv->fresh_in;

			move_file(mv, k, other_dir(mv, here));
		} else if (op < 6) {		/* exchange two files */
			int k2 = (int) xrange(&mv->rng, (uint32_t) (F - 1));

			if (k2 >= k)
				k2++;
			/*
			 * The two objects swap places and names: each name stays
			 * where it was, so the bookkeeping does not change.
			 */
			file_path(mv, k, &a);
			file_path(mv, k2, &b);
			if (dc_rename_exchange(g_dc, &a, &b) != 0)
				mv->errs++;
			mv->xchgs++;
		} else if (op == 6) {		/* the directory, with its file */
			int to = other_dir(mv, mv->g_loc);

			mkpath(&a, "/r/d%d/g%d", mv->g_loc, mv->m);
			mkpath(&b, "/r/d%d/g%d", to, mv->m);
			if (dc_rename(g_dc, &a, &b) != 0)
				mv->errs++;
			mv->g_loc = to;
			mv->dirmoves++;
		} else if (op == 7) {		/* a file renamed where it is */
			file_path(mv, k, &a);
			mv->alt[k] = !mv->alt[k];
			file_path(mv, k, &b);
			if (dc_rename(g_dc, &a, &b) != 0)
				mv->errs++;
			mv->renames++;
		} else {			/* in and out of the root */
			int to = mv->noise_in < 0 ?
				 (int) xrange(&mv->rng, (uint32_t) D) : -1;

			if (mv->noise_in < 0)
				mkpath(&a, "/o/z%d", mv->m);
			else
				mkpath(&a, "/r/d%d/z%d", mv->noise_in, mv->m);
			if (to < 0)
				mkpath(&b, "/o/z%d", mv->m);
			else
				mkpath(&b, "/r/d%d/z%d", to, mv->m);
			if (dc_rename(g_dc, &a, &b) != 0)
				mv->errs++;
			mv->noise_in = to;
			mv->noise++;
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

struct scanner {
	long scans, missed, miss_file, miss_dir, miss_child, dups, errs;
	long first_miss;		/* index of the first one, or -1 */
};

/* Check one scan's @n ids: every tracked object, at least once. */
static void check_scan(struct scanner *sc, const uint64_t *ids, long n,
		       unsigned char *seen)
{
	long i;

	memset(seen, 0, (size_t) NTRACKED);
	for (i = 0; i < n; i++) {
		uint64_t id = ids[i];

		if (id < TRACK_ID_BASE || id >= TRACK_ID_BASE + (uint64_t) NTRACKED)
			continue;
		if (seen[id - TRACK_ID_BASE]++)
			sc->dups++;
	}
	for (i = 0; i < NTRACKED; i++) {
		int what = (int) (i % PER_MOVER);

		if (seen[i])
			continue;
		if (!sc->missed++)
			sc->first_miss = i;
		if (what < F)
			sc->miss_file++;
		else if (what == F)
			sc->miss_dir++;
		else
			sc->miss_child++;
	}
}

static void *scanner(void *arg)
{
	struct scanner *sc = arg;
	long cap = 4L * (NTRACKED + 2 * M + D + 8);
	uint64_t *ids = calloc((size_t) cap, sizeof(*ids));
	unsigned char *seen = calloc((size_t) NTRACKED, 1);

	if (!ids || !seen) {
		sc->errs++;
		return NULL;
	}
	dc_register_thread();
	while (!uatomic_load(&g_stop, CMM_RELAXED)) {
		long n = dc_scan(g_dc, &g_root, ids, cap);

		sc->scans++;
		if (n < 0)
			sc->errs++;
		else
			check_scan(sc, ids, n, seen);
		dc_quiescent();
	}
	dc_unregister_thread();
	free(ids);
	free(seen);
	return NULL;
}

struct census { long tracked, dups, stray; unsigned char *seen; };

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= TRACK_ID_BASE && id < TRACK_ID_BASE + (uint64_t) NTRACKED) {
		if (c->seen[id - TRACK_ID_BASE]++)
			c->dups++;
		else
			c->tracked++;
	} else if (id != ROOT_ID && id != OUT_ID &&
		   !(id >= DIR_ID_BASE && id < DIR_ID_BASE + (uint64_t) D) &&
		   !(id >= FRESH_ID_BASE && id < FRESH_ID_BASE + (uint64_t) M) &&
		   !(id >= NOISE_ID_BASE && id < NOISE_ID_BASE + (uint64_t) M)) {
		c->stray++;
	}
}

static void drain(void)
{
	rcu_quiescent_state();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();
}

/* One round: every mover makes @todo operations, with @scanners running. */
static void round_run(struct mover *mv, struct scanner *sc, int scanners,
		      long todo)
{
	pthread_t *mt = calloc((size_t) M, sizeof(*mt));
	pthread_t *st = calloc((size_t) (scanners ? scanners : 1), sizeof(*st));
	int i;

	uatomic_store(&g_stop, 0, CMM_RELAXED);
	for (i = 0; i < scanners; i++)
		pthread_create(&st[i], NULL, scanner, &sc[i]);
	for (i = 0; i < M; i++) {
		mv[i].todo = todo;
		pthread_create(&mt[i], NULL, mover, &mv[i]);
	}
	rcu_thread_offline();		/* main only blocks in join */
	for (i = 0; i < M; i++)
		pthread_join(mt[i], NULL);
	uatomic_store(&g_stop, 1, CMM_RELAXED);
	for (i = 0; i < scanners; i++)
		pthread_join(st[i], NULL);
	rcu_thread_online();
	drain();
	free(mt);
	free(st);
}

static unsigned long signals_now(void)
{
	unsigned long scans, passes, signals;

	dc_scan_stats(g_dc, &scans, &passes, &signals);
	return signals;
}

int main(int argc, char **argv)
{
	struct mover *mv;
	struct scanner *sc;
	struct census c;
	struct dc_path p;
	unsigned long scans, passes, sig_a, sig_b, sig_c;
	long moves = 0, dirmoves = 0, xchgs = 0, renames = 0, fresh = 0;
	long noise = 0, merrs = 0, per_round[3];
	long nscans = 0, missed = 0, mfile = 0, mdir = 0, mchild = 0, dups = 0;
	long serrs = 0, first_miss = -1, rest_n, cap;
	uint64_t *ids;
	unsigned char *seen;
	struct scanner rest = { .first_miss = -1 };
	int i, k, anomaly = 0;

	if (argc > 1) M = atoi(argv[1]);
	if (argc > 2) S = atoi(argv[2]);
	if (argc > 3) D = atoi(argv[3]);
	if (argc > 4) F = atoi(argv[4]);
	if (argc > 5) MOVES = atol(argv[5]);
	if (argc > 6) FRESH_PCT = atoi(argv[6]);
	setvbuf(stdout, NULL, _IOLBF, 0);	/* a sanitizer exit skips stdio's flush */
	if (M < 1 || S < 1 || D < 2 || F < 2 || MOVES < 4 || FRESH_PCT < 1 ||
	    FRESH_PCT > 99) {
		fprintf(stderr, "bad config\n");
		return 2;
	}
	rcu_register_thread();
	g_dc = dc_create(4096);
	printf("== stress_dcache_scan (engine: %s, scan arm: %s) ==\n",
	       dc_engine_name(), dc_scan_arm());
	printf("movers=%d scanners=%d dirs=%d files/mover=%d moves/mover=%ld"
	       " fresh=%d%%\n", M, S, D, F, MOVES, FRESH_PCT);

	mkpath(&g_root, "/r");
	mkpath(&p, "/o");
	if (dc_add(g_dc, &g_root, ROOT_ID) || dc_add(g_dc, &p, OUT_ID)) {
		fprintf(stderr, "mkdir /r or /o failed\n");
		return 2;
	}
	for (i = 0; i < D; i++) {
		mkpath(&p, "/r/d%d", i);
		if (dc_add(g_dc, &p, DIR_ID_BASE + (uint64_t) i)) {
			fprintf(stderr, "mkdir /r/d%d failed\n", i);
			return 2;
		}
	}
	mv = calloc((size_t) M, sizeof(*mv));
	sc = calloc((size_t) S, sizeof(*sc));
	for (i = 0; i < M; i++) {
		uint64_t base = TRACK_ID_BASE + (uint64_t) (i * PER_MOVER);
		int err = 0;

		mv[i].m = i;
		mv[i].rng = 0x9e3779b97f4a7c15ULL ^
			    ((uint64_t) (i + 1) * 0x100000001b3ULL);
		mv[i].loc = calloc((size_t) F, sizeof(*mv[i].loc));
		mv[i].alt = calloc((size_t) F, 1);
		mv[i].fresh_in = -1;
		mv[i].noise_in = -1;
		for (k = 0; k < F; k++) {
			mv[i].loc[k] = (i + k) % D;
			file_path(&mv[i], k, &p);
			err |= dc_add_file(g_dc, &p, base + (uint64_t) k);
		}
		mv[i].g_loc = i % D;
		mkpath(&p, "/r/d%d/g%d", mv[i].g_loc, i);
		err |= dc_add(g_dc, &p, base + (uint64_t) F);
		mkpath(&p, "/r/d%d/g%d/h%d", mv[i].g_loc, i, i);
		err |= dc_add_file(g_dc, &p, base + (uint64_t) F + 1);
		mkpath(&p, "/o/z%d", i);
		err |= dc_add_file(g_dc, &p, NOISE_ID_BASE + (uint64_t) i);
		if (err) {
			fprintf(stderr, "populating mover %d failed\n", i);
			return 2;
		}
	}
	for (i = 0; i < S; i++)
		sc[i].first_miss = -1;

	per_round[0] = MOVES / 4;
	per_round[1] = MOVES / 2;
	per_round[2] = MOVES - per_round[0] - per_round[1];
	round_run(mv, sc, 0, per_round[0]);	/* A: no scan has ever run */
	sig_a = signals_now();
	round_run(mv, sc, S, per_round[1]);	/* B: moves and scans */
	sig_b = signals_now();
	round_run(mv, sc, 0, per_round[2]);	/* C: the scans are over */
	dc_scan_stats(g_dc, &scans, &passes, &sig_c);
	(void) scans;

	for (i = 0; i < M; i++) {
		moves += mv[i].moves;
		dirmoves += mv[i].dirmoves;
		xchgs += mv[i].xchgs;
		renames += mv[i].renames;
		fresh += mv[i].fresh;
		noise += mv[i].noise;
		merrs += mv[i].errs;
	}
	for (i = 0; i < S; i++) {
		nscans += sc[i].scans;
		missed += sc[i].missed;
		mfile += sc[i].miss_file;
		mdir += sc[i].miss_dir;
		mchild += sc[i].miss_child;
		dups += sc[i].dups;
		serrs += sc[i].errs;
		if (first_miss < 0)
			first_miss = sc[i].first_miss;
	}

	/* At rest: the census, and a scan that must be exact and without repeats. */
	seen = calloc((size_t) NTRACKED, 1);
	memset(&c, 0, sizeof(c));
	c.seen = seen;
	dc_walk(g_dc, census_cb, &c);
	if (c.tracked != NTRACKED || c.dups || c.stray)
		anomaly++;
	cap = 4L * (NTRACKED + 2 * M + D + 8);
	ids = calloc((size_t) cap, sizeof(*ids));
	rest_n = dc_scan(g_dc, &g_root, ids, cap);
	if (rest_n < 0) {
		anomaly++;
	} else {
		check_scan(&rest, ids, rest_n, seen);
		if (rest.missed || rest.dups)
			anomaly++;
	}

	printf("file moves           : %ld (into a fresh directory: %ld)\n",
	       moves, fresh);
	printf("directory moves      : %ld\n", dirmoves);
	printf("exchanges            : %ld\n", xchgs);
	printf("renames in place     : %ld\n", renames);
	printf("moves across the root: %ld\n", noise);
	printf("scans                : %ld, %lu passes\n", nscans, passes);
	printf("signals              : %lu before any scan, %lu during, %lu after\n",
	       sig_a, sig_b - sig_a, sig_c - sig_b);
	printf("reported twice       : %ld (allowed)\n", dups);
	printf("MISSED               : %ld (expect 0): %ld files, %ld directories,"
	       " %ld files in them\n", missed, mfile, mdir, mchild);
	if (first_miss >= 0)
		printf("first miss           : mover %ld, %s\n",
		       first_miss / PER_MOVER,
		       first_miss % PER_MOVER < F ? "a file" :
		       first_miss % PER_MOVER == F ? "its directory" :
		       "the file in its directory");
	printf("errors               : %ld mover, %ld scanner (expect 0)\n",
	       merrs, serrs);
	printf("at rest              : census %ld of %d tracked, %ld twice, %ld stray;"
	       " scan missed %ld, repeated %ld (expect %d, 0, 0; 0, 0)\n",
	       c.tracked, NTRACKED, c.dups, c.stray, rest.missed, rest.dups,
	       NTRACKED);
	free(ids);
	free(seen);
	for (i = 0; i < M; i++) {
		free(mv[i].loc);
		free(mv[i].alt);
	}
	free(mv);
	free(sc);
	if (!nscans || !moves || !xchgs || !dirmoves || !fresh) {
		printf("RESULT: VACUOUS (no scan, or a kind of move never ran)\n");
		return 1;
	}
	if (missed || merrs || serrs || anomaly) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS (every object that stayed below the root was in every scan)\n");
	return 0;
}
