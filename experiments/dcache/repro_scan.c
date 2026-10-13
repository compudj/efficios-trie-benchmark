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
 * The cases that follow are for the scan without a stack (the walk arm), which
 * comes back from a directory through what that directory is now, and the
 * directory may have moved.  Every arm runs them.  In each, g is a directory
 * that holds h, x is in EARLY behind g and y is in MID; all have to be
 * reported.  "At g" is the scan about to read the list of g.
 *
 *   dir-ahead        g is in EARLY.  At g, it moves to LATE.
 *   dir-twice        g is in EARLY.  At g, it moves to MID and then to LATE.
 *                    The walk arm cannot tell where it was: it falls back.
 *   dir-renamed      g is in EARLY.  At g, it is renamed where it is.
 *   dir-shelled      g moves from MID to EARLY before the scan, as z does in
 *                    left-before: a shell names it when the scan comes to it.
 *   dir-shelled-ahead  the same, and at g it moves on to LATE.
 *   dir-shelled-twice  the same, and at g it moves to NEXT and then to LATE:
 *                    the walk arm falls back.
 *   exchange-ahead   g is in EARLY and the file q in LATE.  At g, the two are
 *                    exchanged.
 *   deep-shelled     five directories, one in the other, each moved into the
 *                    one above before the scan, and the first into EARLY: a
 *                    shell names each.  z is in the last.
 *   deep-frames      ten of them: more than the walk arm has frames for.  It
 *                    falls back.
 *   fold-under       g moves from MID to EARLY before the scan, and its fold
 *                    is parked where it has put g itself in the lists and has
 *                    not let go of the shell yet.  The scan runs then.  (On
 *                    the linger and walk arms only.)
 *
 * The walk arm also has to fall back in those three cases and in no other.
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
 * and of the walk arm:
 *   -DDC_TEST_WALK_MOVED_UNSEEN       dir-ahead: back to where the directory
 *                                     is now
 *   -DDC_TEST_WALK_TWICE_AS_ONCE      dir-twice: two moves taken for one
 *   -DDC_TEST_WALK_NO_SHELL_FRAME     dir-shelled: no frame for a directory
 *                                     that a shell names
 *   -DDC_TEST_WALK_FROM_ELSEWHERE     dir-shelled-twice: a last move that
 *                                     left another directory than the frame's
 *   -DDC_TEST_WALK_NO_FRAME_LEFT      deep-frames: in without a frame when
 *                                     none is left
 *   -DDC_TEST_WALK_NO_WALK_IN         linger-loop: round the circle until the
 *                                     frames run out
 *   -DDC_TEST_WALK_FOLD_UNSEEN        fold-under: a shell that the lists no
 *                                     longer hold taken for a move
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
extern void (*dc_test_folded_hook)(void);

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
#define X_ID		600ULL
#define Y_ID		601ULL
#define Q_ID		602ULL
#define DEEP_ID_BASE	700ULL
#define DEEP_Z_ID	799ULL

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
static unsigned long g_frames;		/* frames it noted (the walk arm) */
static unsigned long g_fallbacks;	/* 1 if it fell back */

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
	g_frames = after.frames - before.frames;
	g_fallbacks = after.fallbacks - before.fallbacks;
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

/* An arm on which entries linger: scanned with a stack, or without one. */
static int linger_arm(void)
{
	return !strcmp(dc_scan_arm(), "linger") || !strcmp(dc_scan_arm(), "walk");
}

/* The scan without a stack, which has frames and may fall back. */
static int walk_arm(void)
{
	return !strcmp(dc_scan_arm(), "walk");
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

/* The walk arm fell back, or did not, as the case has it. */
static void expect_fallback(struct verdict *v, unsigned long want)
{
	if (!walk_arm())
		return;
	if (g_fallbacks != want)
		fail(v, want ? "the walk did not fall back" :
			       "the walk fell back");
}

/* g with h in it in @dir, x behind g in EARLY, y in MID. */
static void setup_g(const char *dir)
{
	MKFILE(X_ID, "%s/x", EARLY);
	MKDIR(G_ID, "%s/g", dir);
	MKFILE(H_ID, "%s/g/h", dir);
	MKFILE(Y_ID, "%s/y", MID);
	drain();
}

static void expect_g(struct verdict *v)
{
	if (!g_dir_fired && g_dir_at)
		fail(v, "the scan never came to g");
	expect(v, G_ID, "g");
	expect(v, H_ID, "h");
	expect(v, X_ID, "x");
	expect(v, Y_ID, "y");
}

/* g is at @where by now, under the name @name. */
static void cleanup_g(const char *where, const char *name)
{
	drain();
	RM("%s/%s/h", where, name);
	RM("%s/%s", where, name);
	RM("%s/x", EARLY);
	RM("%s/y", MID);
}

static void step_g_early_late(void)
{
	mv(S("%s/g", EARLY), S("%s/g", LATE));
}

static void case_dir_ahead(struct verdict *v)
{
	setup_g(EARLY);
	scan(G_ID, step_g_early_late, 0, NULL);
	expect_g(v);
	cleanup_g(LATE, "g");
}

static void step_g_early_mid_late(void)
{
	mv(S("%s/g", EARLY), S("%s/g", MID));
	mv(S("%s/g", MID), S("%s/g", LATE));
}

static void case_dir_twice(struct verdict *v)
{
	setup_g(EARLY);
	scan(G_ID, step_g_early_mid_late, 0, NULL);
	expect_g(v);
	cleanup_g(LATE, "g");
}

static void step_g_renamed(void)
{
	mv(S("%s/g", EARLY), S("%s/g2", EARLY));
}

static void case_dir_renamed(struct verdict *v)
{
	setup_g(EARLY);
	scan(G_ID, step_g_renamed, 0, NULL);
	expect_g(v);
	cleanup_g(EARLY, "g2");
}

static void step_g_mid_early(void)
{
	mv(S("%s/g", MID), S("%s/g", EARLY));
}

static void case_dir_shelled(struct verdict *v)
{
	setup_g(MID);
	mover_do(step_g_mid_early);	/* on line from here to the scan's end */
	scan(0, NULL, 0, NULL);
	expect_g(v);
	if (walk_arm() && !g_frames)
		fail(v, "no shell named g when the scan came to it");
	cleanup_g(EARLY, "g");
}

static void case_dir_shelled_ahead(struct verdict *v)
{
	setup_g(MID);
	mover_do(step_g_mid_early);
	scan(G_ID, step_g_early_late, 0, NULL);
	expect_g(v);
	cleanup_g(LATE, "g");
}

static void step_g_early_next_late(void)
{
	mv(S("%s/g", EARLY), S("%s/g", NEXT));
	mv(S("%s/g", NEXT), S("%s/g", LATE));
}

static void case_dir_shelled_twice(struct verdict *v)
{
	setup_g(MID);
	mover_do(step_g_mid_early);
	scan(G_ID, step_g_early_next_late, 0, NULL);
	expect_g(v);
	cleanup_g(LATE, "g");
}

static void step_g_exchanged(void)
{
	struct dc_path a, b;

	must(dc_rename_exchange(g_dc, P(&a, "%s/g", EARLY), P(&b, "%s/q", LATE)),
	     "exchange");
}

static void case_exchange_ahead(struct verdict *v)
{
	MKFILE(Q_ID, "%s/q", LATE);
	setup_g(EARLY);
	scan(G_ID, step_g_exchanged, 0, NULL);
	expect_g(v);
	expect(v, Q_ID, "q");
	drain();
	RM("%s/g", EARLY);		/* the file q, under the name g */
	cleanup_g(LATE, "q");		/* the directory g, under the name q */
}

/*
 * @g_deep directories c0, c1, ... made side by side in NEXT, then each moved
 * into the one before it, from the last, and c0 into EARLY: a shell names
 * each of them where it is, and what each left lingers in NEXT.
 */
static int g_deep;

static char *deep_path(int depth)
{
	static char buf[DC_NAME_MAX * 4];
	int i, off = snprintf(buf, sizeof(buf), "%s", EARLY);

	for (i = 0; i <= depth; i++)
		off += snprintf(buf + off, sizeof(buf) - (size_t) off, "/c%d", i);
	return buf;
}

static void step_deep(void)
{
	int i;

	for (i = g_deep - 1; i > 0; i--)
		mv(S("%s/c%d", NEXT, i), S("%s/c%d/c%d", NEXT, i - 1, i));
	mv(S("%s/c0", NEXT), S("%s/c0", EARLY));
}

static void deep(struct verdict *v, int n)
{
	int i;

	g_deep = n;
	MKFILE(X_ID, "%s/x", EARLY);
	for (i = 0; i < n; i++)
		MKDIR(DEEP_ID_BASE + (uint64_t) i, "%s/c%d", NEXT, i);
	MKFILE(DEEP_Z_ID, "%s/c%d/z", NEXT, n - 1);
	drain();
	mover_do(step_deep);
	scan(0, NULL, 0, NULL);
	expect(v, X_ID, "x");
	expect(v, DEEP_Z_ID, "z");
	for (i = 0; i < n; i++)
		expect(v, DEEP_ID_BASE + (uint64_t) i, "a directory of the chain");
	if (walk_arm() && g_frames < (unsigned long) (n < 8 ? n : 8))
		fail(v, "%lu frames for %d directories that shells name",
		     g_frames, n);
	drain();
	RM("%s/z", deep_path(n - 1));
	for (i = n - 1; i >= 0; i--)
		RM("%s", deep_path(i));
	RM("%s/x", EARLY);
}

static void case_deep_shelled(struct verdict *v)
{
	deep(v, 5);
}

static void case_deep_frames(struct verdict *v)
{
	deep(v, 10);
}

/* The fold of the case below, parked between its commit and the rest. */
static sem_t g_folded, g_fold_go;
static int g_fold_parked;

static void folded_hook(void)
{
	if (g_fold_parked)
		return;
	g_fold_parked = 1;
	sem_post(&g_folded);
	sem_wait(&g_fold_go);		/* on line: no grace period meanwhile */
}

static void case_fold_under(struct verdict *v)
{
	/*
	 * Where entries linger only.  The parked fold holds the lock of
	 * EARLY's list, which a scan of the mark arm waits for.
	 */
	if (!linger_arm())
		return;
	setup_g(MID);
	g_fold_parked = 0;
	dc_test_folded_hook = folded_hook;
	mover_do(step_g_mid_early);
	sem_wait_quiescent(&g_folded);	/* a grace period, then the fold */
	scan(0, NULL, 0, NULL);
	sem_post(&g_fold_go);
	dc_test_folded_hook = NULL;
	expect_g(v);
	cleanup_g(EARLY, "g");
}

static const struct {
	const char *name;
	void (*run)(struct verdict *v);
	unsigned long fallbacks;	/* of the walk arm, in the scan of the case */
} cases[] = {
	{ "move-behind", case_move_behind, 0 },
	{ "exchange-behind", case_exchange_behind, 0 },
	{ "out-then-rmdir", case_out_then_rmdir, 0 },
	{ "dir-behind", case_dir_behind, 0 },
	{ "into-fresh", case_into_fresh, 0 },
	{ "dead-end-climb", case_dead_end_climb, 0 },
	{ "left-before", case_left_before, 0 },
	{ "linger-loop", case_linger_loop, 0 },
	{ "dir-ahead", case_dir_ahead, 0 },
	{ "dir-twice", case_dir_twice, 1 },
	{ "dir-renamed", case_dir_renamed, 0 },
	{ "dir-shelled", case_dir_shelled, 0 },
	{ "dir-shelled-ahead", case_dir_shelled_ahead, 0 },
	{ "dir-shelled-twice", case_dir_shelled_twice, 1 },
	{ "exchange-ahead", case_exchange_ahead, 0 },
	{ "deep-shelled", case_deep_shelled, 0 },
	{ "deep-frames", case_deep_frames, 1 },
	{ "fold-under", case_fold_under, 0 },
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
	sem_init(&g_folded, 0, 0);
	sem_init(&g_fold_go, 0, 0);
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
		expect_fallback(&v, cases[i].fallbacks);
		printf("%-18s: %s%s%s\n", cases[i].name, v.fail ? "FAIL" : "ok",
		       v.fail ? " -- " : "", v.why);
		failed += v.fail != 0;
		if (must_fail && !strcmp(must_fail, cases[i].name))
			wanted = v.fail != 0;
	}

	/* At rest again: the four directories, and no lingering entry left. */
	drain();
	scan(0, NULL, 0, NULL);
	if (g_n != NDIRS || g_lingerers) {
		printf("at rest            : FAIL -- %ld ids, %lu lingering entries"
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
