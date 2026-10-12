// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_scan.c -- dc_scan() against moves that go behind it, one interleaving
 * at a time.
 *
 * stress_dcache_scan.c finds a scan that lost an object by running many; it
 * cannot say which move the scan lost it to, and on the linger arm most of
 * what could go wrong is hidden by the lingering entries that earlier moves
 * left (see its header).  Here each case is one scan, parked by a hook where
 * the case wants it while another thread makes one or two operations, and the
 * tree is at rest before: no lingering entry but the ones the case makes.
 *
 * The scan reads the directories of /r in an order that a first scan records;
 * call them EARLY, MID, NEXT and LATE.  "At MID" below is the scan about to
 * read MID's list, with EARLY's read and LATE's to come.
 *
 *   move-behind      f is in LATE.  At MID, f moves to EARLY.
 *   exchange-behind  a is in EARLY, b in LATE.  At MID, they are exchanged.
 *   out-then-rmdir   f is in the directory n, in LATE.  At MID, f moves to
 *                    EARLY and n is removed.
 *   dir-behind       h is in the directory g, in LATE.  At MID, g moves to
 *                    EARLY.
 *   into-fresh       f is in LATE.  At MID, a directory n is made in EARLY and
 *                    f moves into it.
 *   dead-end-climb   f is in LATE.  At LATE, n is made in EARLY and f moves
 *                    into it.  The linger arm then finds f through what it
 *                    left in LATE and climbs from f to check it: when the
 *                    climb has read that f is in n, f moves to MID and n is
 *                    removed.
 *   left-before      z is in LATE and moves out of /r before the scan, whose
 *                    thread stays in its read-side section from before the
 *                    move: what the move left in LATE is still there.
 *   linger-loop      the directory b is in the directory a, in EARLY, and
 *                    holds c.  Before the scan, as above, b moves to LATE and
 *                    then a moves into b: what b left in a leads to b, and b
 *                    holds a.
 *
 * In every case but one, each object named was below /r for the whole scan and
 * has to be reported.  In left-before, z was outside for the whole scan and
 * must not be.  linger-loop also bounds how many ids the scan reports.
 *
 * Which arm fails which (make check-scan runs them all):
 *   soft                              move-behind, and the four after it
 *   -DDC_TEST_SCAN_NO_XCHG            exchange-behind
 *   -DDC_TEST_SCAN_NO_INHERIT         into-fresh
 *   -DDC_TEST_LINGER_NO_WAIT          move-behind
 *   -DDC_TEST_LINGER_NO_XCHG          exchange-behind
 *   -DDC_TEST_LINGER_NO_RMDIR         out-then-rmdir
 *   -DDC_TEST_LINGER_DEAD_NO_LOOK     dead-end-climb
 *   -DDC_TEST_LINGER_NO_CHECK         left-before
 *   -DDC_TEST_LINGER_NO_LOOPCHECK     linger-loop
 * The two mutations of a window inside a commit (the sequence counted once,
 * the scan not waiting for a locked list) are out of the reach of these
 * hooks: the stress test has them.
 *
 * Usage: ./repro_scan [case]
 * Exit 0 = every case passed; with a case named, 0 = that case FAILED (the
 * must-fail controls of the gate), whatever the others did.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <urcu-qsbr.h>

#include "dcache.h"

extern void (*dc_test_scan_dir_hook)(uint64_t dir_id);
extern void (*dc_test_scan_climb_hook)(uint64_t dir_id);

#define NDIRS		4
#define ROOT_ID		1ULL
#define OUT_ID		2ULL
#define DIR_ID_BASE	10ULL
#define F_ID		100ULL
#define A_ID		101ULL
#define B_ID		102ULL
#define N_ID		200ULL
#define G_ID		300ULL
#define H_ID		301ULL
#define Z_ID		400ULL
#define LA_ID		500ULL
#define LB_ID		501ULL
#define LC_ID		502ULL

#define SCAN_CAP	4096

static struct dcache *g_dc;
static struct dc_path g_root;
static int g_errs;			/* operations that failed */

/* /r/d<i> in the order a scan reads them. */
static char EARLY[16], MID[16], NEXT[16], LATE[16];
static uint64_t early_id, mid_id, next_id, late_id;

/* ---- the other thread ---- */

static sem_t g_go, g_done;
static void (*g_step)(void);
static int g_quit;

/* Parked with nothing in hand: off line, or no grace period would end. */
static void sem_wait_quiescent(sem_t *s)
{
	rcu_thread_offline();
	sem_wait(s);
	rcu_thread_online();
}

static void *mover(void *arg)
{
	(void) arg;
	dc_register_thread();
	for (;;) {
		sem_wait_quiescent(&g_go);
		if (g_quit)
			break;
		g_step();
		dc_quiescent();
		sem_post(&g_done);
	}
	dc_unregister_thread();
	return NULL;
}

/*
 * Have the other thread make @step, and wait for it ON LINE: the caller is a
 * scan, or is about to be one, and what it has read stays readable.
 */
static void mover_do(void (*step)(void))
{
	g_step = step;
	sem_post(&g_go);
	sem_wait(&g_done);
}

/* ---- the hooks ---- */

static uint64_t g_order[NDIRS];
static int g_norder, g_probe;
static uint64_t g_dir_at, g_climb_at;
static void (*g_dir_step)(void), (*g_climb_step)(void);
static int g_dir_fired, g_climb_fired;

static void dir_hook(uint64_t id)
{
	if (g_probe) {
		if (id >= DIR_ID_BASE && id < DIR_ID_BASE + NDIRS &&
		    g_norder < NDIRS)
			g_order[g_norder++] = id;
		return;
	}
	if (g_dir_step && id == g_dir_at) {	/* once: a scan may go again */
		void (*step)(void) = g_dir_step;

		g_dir_step = NULL;
		g_dir_fired = 1;
		mover_do(step);
	}
}

static void climb_hook(uint64_t id)
{
	if (g_climb_step && id == g_climb_at) {
		void (*step)(void) = g_climb_step;

		g_climb_step = NULL;
		g_climb_fired = 1;
		mover_do(step);
	}
}

/* ---- operations ---- */

static struct dc_path *P(struct dc_path *p, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static struct dc_path *P(struct dc_path *p, const char *fmt, ...)
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
	return p;
}

static void must(int ret, const char *what)
{
	if (ret) {
		fprintf(stderr, "  %s: %d\n", what, ret);
		g_errs++;
	}
}

#define MKDIR(id, ...)	do { struct dc_path p_;				\
		must(dc_add(g_dc, P(&p_, __VA_ARGS__), (id)), "mkdir"); } while (0)
#define MKFILE(id, ...)	do { struct dc_path p_;				\
		must(dc_add_file(g_dc, P(&p_, __VA_ARGS__), (id)), "create"); } while (0)
#define RM(...)		do { struct dc_path p_;				\
		must(dc_unlink(g_dc, P(&p_, __VA_ARGS__)), "unlink"); } while (0)

static void mv(const char *from, const char *to)
{
	struct dc_path a, b;

	must(dc_rename(g_dc, P(&a, "%s", from), P(&b, "%s", to)), "rename");
}

static char *S(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *S(const char *fmt, ...)
{
	static __thread char buf[4][DC_NAME_MAX * 4];
	static __thread int k;
	char *s = buf[k++ & 3];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(s, sizeof(buf[0]), fmt, ap);
	va_end(ap);
	return s;
}

/* Every callback has run, and what those queued. */
static void drain(void)
{
	int i;

	for (i = 0; i < 3; i++) {
		rcu_quiescent_state();
		synchronize_rcu();
		rcu_barrier();
	}
}

/* ---- one scan ---- */

static uint64_t g_ids[SCAN_CAP];
static long g_n;
static unsigned long g_lingerers;	/* entries this scan went through */

static void scan(uint64_t dir_at, void (*dir_step)(void),
		 uint64_t climb_at, void (*climb_step)(void))
{
	struct dc_scan_stats before, after;

	g_dir_at = dir_at;
	g_dir_step = dir_step;
	g_climb_at = climb_at;
	g_climb_step = climb_step;
	g_dir_fired = g_climb_fired = 0;
	dc_scan_stats(g_dc, &before);
	g_n = dc_scan(g_dc, &g_root, g_ids, SCAN_CAP);
	dc_scan_stats(g_dc, &after);
	g_lingerers = after.lingerers - before.lingerers;
	g_dir_step = g_climb_step = NULL;
}

static int reported(uint64_t id)
{
	long i;

	for (i = 0; i < g_n; i++)
		if (g_ids[i] == id)
			return 1;
	return 0;
}

static int linger_arm(void)
{
	return !strcmp(dc_scan_arm(), "linger");
}

/* ---- the cases ---- */

struct verdict {
	int fail;
	char why[160];
};

static void fail(struct verdict *v, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void fail(struct verdict *v, const char *fmt, ...)
{
	va_list ap;

	if (v->fail++)
		return;
	va_start(ap, fmt);
	vsnprintf(v->why, sizeof(v->why), fmt, ap);
	va_end(ap);
}

static void expect(struct verdict *v, uint64_t id, const char *name)
{
	if (g_n < 0)
		fail(v, "the scan returned %ld", g_n);
	else if (!reported(id))
		fail(v, "%s was below /r throughout and is not reported", name);
}

static void step_move_behind(void)
{
	mv(S("%s/f", LATE), S("%s/f", EARLY));
}

static void case_move_behind(struct verdict *v)
{
	MKFILE(F_ID, "%s/f", LATE);
	drain();
	scan(mid_id, step_move_behind, 0, NULL);
	if (!g_dir_fired)
		fail(v, "the scan never came to MID");
	expect(v, F_ID, "f");
	drain();
	RM("%s/f", EARLY);
}

static void step_exchange_behind(void)
{
	struct dc_path a, b;

	must(dc_rename_exchange(g_dc, P(&a, "%s/a", EARLY), P(&b, "%s/b", LATE)),
	     "exchange");
}

static void case_exchange_behind(struct verdict *v)
{
	MKFILE(A_ID, "%s/a", EARLY);
	MKFILE(B_ID, "%s/b", LATE);
	drain();
	scan(mid_id, step_exchange_behind, 0, NULL);
	if (!g_dir_fired)
		fail(v, "the scan never came to MID");
	expect(v, A_ID, "a");
	expect(v, B_ID, "b");
	drain();
	RM("%s/a", EARLY);		/* the names stay, the objects swap */
	RM("%s/b", LATE);
}

static void step_out_then_rmdir(void)
{
	mv(S("%s/n/f", LATE), S("%s/f", EARLY));
	RM("%s/n", LATE);
}

static void case_out_then_rmdir(struct verdict *v)
{
	MKDIR(N_ID, "%s/n", LATE);
	MKFILE(F_ID, "%s/n/f", LATE);
	drain();
	scan(mid_id, step_out_then_rmdir, 0, NULL);
	if (!g_dir_fired)
		fail(v, "the scan never came to MID");
	expect(v, F_ID, "f");
	drain();
	RM("%s/f", EARLY);
}

static void step_dir_behind(void)
{
	mv(S("%s/g", LATE), S("%s/g", EARLY));
}

static void case_dir_behind(struct verdict *v)
{
	MKDIR(G_ID, "%s/g", LATE);
	MKFILE(H_ID, "%s/g/h", LATE);
	drain();
	scan(mid_id, step_dir_behind, 0, NULL);
	if (!g_dir_fired)
		fail(v, "the scan never came to MID");
	expect(v, G_ID, "g");
	expect(v, H_ID, "h");
	drain();
	RM("%s/g/h", EARLY);
	RM("%s/g", EARLY);
}

static void step_into_fresh(void)
{
	MKDIR(N_ID, "%s/n", EARLY);
	mv(S("%s/f", LATE), S("%s/n/f", EARLY));
}

static void case_into_fresh(struct verdict *v)
{
	MKFILE(F_ID, "%s/f", LATE);
	drain();
	scan(mid_id, step_into_fresh, 0, NULL);
	if (!g_dir_fired)
		fail(v, "the scan never came to MID");
	expect(v, F_ID, "f");
	drain();
	RM("%s/n/f", EARLY);
	RM("%s/n", EARLY);
}

static void step_dead_end(void)
{
	mv(S("%s/n/f", EARLY), S("%s/f", MID));
	RM("%s/n", EARLY);
}

static void case_dead_end_climb(struct verdict *v)
{
	MKFILE(F_ID, "%s/f", LATE);
	drain();
	scan(late_id, step_into_fresh, N_ID, step_dead_end);
	if (!g_dir_fired)
		fail(v, "the scan never came to LATE");
	if (linger_arm() && !g_climb_fired)
		fail(v, "no climb read that f was in n");
	expect(v, F_ID, "f");
	drain();
	if (g_climb_fired) {
		RM("%s/f", MID);
	} else {			/* no climb on this arm: f is still in n */
		RM("%s/n/f", EARLY);
		RM("%s/n", EARLY);
	}
}

static void step_leave(void)
{
	mv(S("%s/z", LATE), "/o/z");
}

static void case_left_before(struct verdict *v)
{
	MKFILE(Z_ID, "%s/z", LATE);
	drain();
	mover_do(step_leave);		/* on line from here to the scan's end */
	scan(0, NULL, 0, NULL);
	if (g_n < 0)
		fail(v, "the scan returned %ld", g_n);
	if (linger_arm() && !g_lingerers)
		fail(v, "what z left in LATE was gone before the scan");
	if (reported(Z_ID))
		fail(v, "z was outside /r throughout and is reported");
	drain();
	RM("/o/z");
}

static void step_loop(void)
{
	mv(S("%s/a/b", EARLY), S("%s/b", LATE));
	mv(S("%s/a", EARLY), S("%s/b/a", LATE));
}

static void case_linger_loop(struct verdict *v)
{
	MKDIR(LA_ID, "%s/a", EARLY);
	MKDIR(LB_ID, "%s/a/b", EARLY);
	MKFILE(LC_ID, "%s/a/b/c", EARLY);
	drain();
	mover_do(step_loop);
	scan(0, NULL, 0, NULL);
	if (linger_arm() && g_lingerers < 2)
		fail(v, "what a and b left was gone before the scan");
	expect(v, LA_ID, "a");
	expect(v, LB_ID, "b");
	expect(v, LC_ID, "c");
	/*
	 * The four directories; then a, b and c by each of two roads, and at
	 * the end of each road the directory that the scan is already in,
	 * which it reports again and does not enter.
	 */
	if (g_n > NDIRS + 4 * 2)
		fail(v, "%ld ids for 7 objects: the scan went round", g_n);
	drain();
	RM("%s/b/c", LATE);
	RM("%s/b/a", LATE);
	RM("%s/b", LATE);
}

static const struct {
	const char *name;
	void (*run)(struct verdict *v);
} cases[] = {
	{ "move-behind", case_move_behind },
	{ "exchange-behind", case_exchange_behind },
	{ "out-then-rmdir", case_out_then_rmdir },
	{ "dir-behind", case_dir_behind },
	{ "into-fresh", case_into_fresh },
	{ "dead-end-climb", case_dead_end_climb },
	{ "left-before", case_left_before },
	{ "linger-loop", case_linger_loop },
};
#define NCASES	((int) (sizeof(cases) / sizeof(cases[0])))

static const char *dir_name(char *buf, uint64_t id)
{
	snprintf(buf, 16, "/r/d%d", (int) (id - DIR_ID_BASE));
	return buf;
}

int main(int argc, char **argv)
{
	const char *must_fail = argc > 1 ? argv[1] : NULL;
	int failed = 0, wanted = 0, known = 0, i;
	struct dc_path p;
	pthread_t t;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (i = 0; must_fail && i < NCASES; i++)
		known |= !strcmp(must_fail, cases[i].name);
	if (must_fail && !known) {
		fprintf(stderr, "no case named %s\n", must_fail);
		return 2;
	}
	rcu_register_thread();
	g_dc = dc_create(1024);
	printf("== repro_scan (engine: %s, scan arm: %s) ==\n",
	       dc_engine_name(), dc_scan_arm());

	must(dc_add(g_dc, P(&g_root, "/r"), ROOT_ID), "mkdir /r");
	must(dc_add(g_dc, P(&p, "/o"), OUT_ID), "mkdir /o");
	for (i = 0; i < NDIRS; i++)
		MKDIR(DIR_ID_BASE + (uint64_t) i, "/r/d%d", i);

	sem_init(&g_go, 0, 0);
	sem_init(&g_done, 0, 0);
	pthread_create(&t, NULL, mover, NULL);
	dc_test_scan_dir_hook = dir_hook;
	dc_test_scan_climb_hook = climb_hook;

	g_probe = 1;			/* the order a scan reads /r in */
	scan(0, NULL, 0, NULL);
	g_probe = 0;
	if (g_norder != NDIRS || g_n != NDIRS) {
		printf("RESULT: FAIL (the scan at rest read %d directories and"
		       " reported %ld ids, expected %d and %d)\n",
		       g_norder, g_n, NDIRS, NDIRS);
		return 1;
	}
	early_id = g_order[0];
	mid_id = g_order[1];
	next_id = g_order[2];
	late_id = g_order[3];
	dir_name(EARLY, early_id);
	dir_name(MID, mid_id);
	dir_name(NEXT, next_id);
	dir_name(LATE, late_id);
	(void) next_id;

	for (i = 0; i < NCASES; i++) {
		struct verdict v = { 0, "" };

		cases[i].run(&v);
		printf("%-16s: %s%s%s\n", cases[i].name, v.fail ? "FAIL" : "ok",
		       v.fail ? " -- " : "", v.why);
		failed += v.fail != 0;
		if (must_fail && !strcmp(must_fail, cases[i].name))
			wanted = v.fail != 0;
	}

	/* At rest again: the four directories, and no lingering entry left. */
	drain();
	scan(0, NULL, 0, NULL);
	if (g_n != NDIRS || g_lingerers) {
		printf("at rest         : FAIL -- %ld ids, %lu lingering entries"
		       " (expected %d, 0)\n", g_n, g_lingerers, NDIRS);
		failed++;
	}

	dc_test_scan_dir_hook = NULL;
	dc_test_scan_climb_hook = NULL;
	g_quit = 1;
	sem_post(&g_go);
	rcu_thread_offline();
	pthread_join(t, NULL);
	rcu_thread_online();
	dc_destroy(g_dc);
	rcu_unregister_thread();

	if (g_errs) {
		printf("RESULT: FAIL (%d operations of the test failed)\n", g_errs);
		return 1;
	}
	if (must_fail) {
		printf("RESULT: %s (%s %s)\n", wanted ? "FAILS AS IT MUST" : "PASS",
		       must_fail, wanted ? "failed" : "passed, and must not");
		return wanted ? 0 : 1;
	}
	printf("RESULT: %s (%d of %d cases failed)\n", failed ? "FAIL" : "PASS",
	       failed, NCASES);
	return failed ? 1 : 0;
}
