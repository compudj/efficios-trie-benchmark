// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * repro_mount.c -- the mount count (dcache_mount.h), one case at a time.
 *
 * Each case has a cache of its own.  The first eleven are one thread: they
 * say what every count has to be after a mount, an unmount, a rename or an
 * exchange, that dc_mount_check() agrees, and that a count which is back to
 * zero is gone.  The others park one thread in the middle of an operation, by
 * a hook, and have another thread act there:
 *
 *   ancestor-moves-first  k has a mountpoint below it and moves to /p/q.  When
 *                    the rename has frozen q and not p, p moves to /z.  It has
 *                    no flag yet, so nothing stops it; the rename has to go on
 *                    from where p is now.
 *   ancestor-moves-late   the same, when the rename has frozen p as well and
 *                    holds no lock yet: p moves all the same, with its bucket
 *                    held, and the rename, which finds under the locks that p
 *                    is not where it read it, works its counts out again.
 *   ancestor-move-waits   the same, when the rename holds the buckets of its
 *                    counts and is about to commit: the move of p waits for
 *                    p's bucket, and then takes k's count along to /z.
 *   mount-under-move      m is mounted in /p/q.  When the mount has frozen p,
 *                    p moves to /z: the mount works its counts out again.
 *   reader-before-commit  the same move, about to commit: /a/x, which k is
 *                    still in, counts it.
 *   reader-at-commit      k has a mountpoint below it and moves from /a/x to
 *                    /a/y.  Once the move has committed, and before it lets go
 *                    of its buckets, another thread reads the counts: they are
 *                    the new ones already, and the reader took no lock.
 *   unlink-waits          k moves to the empty directory /a/e.  When the
 *                    rename has frozen e, e is removed: the removal has to
 *                    wait, and then finds e not empty.
 *   exists                k moves to /p/q.  When the rename has frozen q, the
 *                    name it was going to take there is taken: the move
 *                    fails, and what it froze has no count left.
 *   pin-keeps-frozen      k moves to /p/q.  When the rename has frozen p,
 *                    another mountpoint moves into p and out again, which
 *                    takes p's count to one and back to zero: p's count stays
 *                    hashed, since the first rename has it pinned.
 *   freeze-under-move     p, which has no count, is on its way to /z: its
 *                    rename has read that p has no flag and has not committed.
 *                    A mount in /p/q has to wait for it before it freezes p,
 *                    or it reads that p is in the root.
 *   move-holds-bucket     m is mounted in /p/q.  When the mount has frozen p,
 *                    a rename of p, which sees the flag, takes p's bucket and
 *                    is parked before its commit.  The mount has to wait for
 *                    that bucket, and then finds p moved.
 *   find-moved            a search below /a/x has come down to k.  k moves out
 *                    of /a/x, what was mounted below it is unmounted, and a
 *                    mountpoint that was never below /a/x moves into k.  The
 *                    search goes on to that one, and must not hand it back.
 *
 * ./repro_mount [case]: with a case named, runs that case alone and returns 0
 * if it FAILS, for the builds that take a rule away (make check-mount).
 */
#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <urcu-qsbr.h>

#include "dcache.h"

extern void (*dc_test_mnt_frozen_hook)(uint64_t id);
extern void (*dc_test_mnt_moved_hook)(void);
extern void (*dc_test_mnt_locked_hook)(void);
extern void (*dc_test_mnt_found_hook)(uint64_t id);
extern void (*dc_test_scan_move_hook)(int locked);

static struct dcache *g_dc;
static int g_errs;			/* operations that failed */

/*
 * Two arms (dc_mount_arm()): the counts in a table, dcache_mount.h, which
 * looks again under its locks and starts over when something moved; and the
 * counts in the dentries, dcache_mount_inline.h, where whoever would move
 * something that counts waits for the one lock.  The same cases, and where a
 * thread is parked the two differ in who waits for whom.
 */
static int inline_arm(void)
{
	return !strcmp(dc_mount_arm(), "inline");
}

/* ---- the other thread ---- */

static sem_t g_go, g_done, g_parked, g_release;
static void (*g_step)(void);
static int g_quit;
static __thread int t_other;

static void *other(void *arg)
{
	(void) arg;
	t_other = 1;
	dc_register_thread();
	for (;;) {
		rcu_thread_offline();	/* parked with nothing in hand */
		sem_wait(&g_go);
		rcu_thread_online();
		if (g_quit)
			break;
		g_step();
		dc_quiescent();
		sem_post(&g_done);
	}
	dc_unregister_thread();
	return NULL;
}

static void other_start(void (*step)(void))
{
	g_step = step;
	sem_post(&g_go);
}

/* Wait for it on line: the caller is in the middle of an operation. */
static void other_join(void)
{
	sem_wait(&g_done);
}

static void other_do(void (*step)(void))
{
	other_start(step);
	other_join();
}

/* Has it finished within @ms?  If not it is waiting for the caller. */
static int other_done_within(int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long) ms * 1000000L;
	ts.tv_sec += ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;
	return sem_timedwait(&g_done, &ts) == 0;
}

/* ---- helpers ---- */

static struct dc_path *P(struct dc_path *p, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static struct dc_path *P(struct dc_path *p, const char *fmt, ...)
{
	char buf[DC_NAME_MAX * 30];
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

static void must(int ret, const char *what, const char *path)
{
	if (ret) {
		fprintf(stderr, "  %s %s: %d\n", what, path, ret);
		g_errs++;
	}
}

static void mkdir_(uint64_t id, const char *path)
{
	struct dc_path p;

	must(dc_add(g_dc, P(&p, "%s", path), id), "mkdir", path);
}

static void mkfile(uint64_t id, const char *path)
{
	struct dc_path p;

	must(dc_add_file(g_dc, P(&p, "%s", path), id), "create", path);
}

static int mv_ret(const char *from, const char *to)
{
	struct dc_path a, b;

	return dc_rename(g_dc, P(&a, "%s", from), P(&b, "%s", to));
}

static void mv(const char *from, const char *to)
{
	must(mv_ret(from, to), "rename", from);
}

static void xchg(const char *a, const char *b)
{
	struct dc_path pa, pb;

	must(dc_rename_exchange(g_dc, P(&pa, "%s", a), P(&pb, "%s", b)),
	     "exchange", a);
}

static int mount_ret(const char *path)
{
	struct dc_path p;

	return dc_mount(g_dc, P(&p, "%s", path));
}

static void mount(const char *path)
{
	must(mount_ret(path), "mount", path);
}

static int umount_ret(const char *path)
{
	struct dc_path p;

	return dc_umount(g_dc, P(&p, "%s", path));
}

static void umount(const char *path)
{
	must(umount_ret(path), "umount", path);
}

struct verdict {
	int fail;
	char why[200];
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

/* The count at @path is @n. */
static void want(struct verdict *v, const char *path, long n)
{
	struct dc_path p;
	long got = dc_mount_count(g_dc, P(&p, "%s", path));

	if (got != n)
		fail(v, "%s counts %ld, %ld expected", path, got, n);
}

static void want_ret(struct verdict *v, const char *what, long got, long n)
{
	if (got != n)
		fail(v, "%s returned %ld, %ld expected", what, got, n);
}

/* Counted again from the mountpoints, every count is right. */
static void sound(struct verdict *v)
{
	long bad = dc_mount_check(g_dc, NULL);

	if (bad) {
		fail(v, "%ld counts are not what is below them", bad);
		(void) dc_mount_check(g_dc, stderr);
	}
}

/* @n dentries have a count: nothing is left of the others. */
static void want_counts(struct verdict *v, unsigned long n)
{
	struct dc_mount_stats st;

	dc_mount_stats(g_dc, &st);
	if (st.counts != n)
		fail(v, "%lu dentries have a count, %lu expected", st.counts, n);
}

static unsigned long moves_with_count(void)
{
	struct dc_mount_stats st;

	dc_mount_stats(g_dc, &st);
	return st.moves;
}

/* Operations that worked their counts out again; searches that went again. */
static unsigned long again(void)
{
	struct dc_mount_stats st;

	dc_mount_stats(g_dc, &st);
	return st.again;
}

static unsigned long find_again(void)
{
	struct dc_mount_stats st;

	dc_mount_stats(g_dc, &st);
	return st.find_again;
}

/*
 * The tree most cases start from:
 *
 *   /a/x/k/h   /a/y/j   /b   /p/q   /z
 *
 * k, j: directories.  h: a file, the mountpoint.
 */
enum { A = 10, X, Y, B, PP, Q, Z, K = 100, H, J, H2, E, M, G, M2, WW, DEEP = 1000 };

static void tree(void)
{
	g_dc = dc_create(1024);
	mkdir_(A, "/a");
	mkdir_(X, "/a/x");
	mkdir_(Y, "/a/y");
	mkdir_(B, "/b");
	mkdir_(PP, "/p");
	mkdir_(Q, "/p/q");
	mkdir_(Z, "/z");
	mkdir_(K, "/a/x/k");
	mkfile(H, "/a/x/k/h");
	mkdir_(J, "/a/y/j");
}

/* ---- the cases with one thread ---- */

static void case_mount_counts(struct verdict *v)
{
	mkfile(H2, "/a/x/h2");
	mount("/a/x/k/h");
	want(v, "/", 1);
	want(v, "/a", 1);
	want(v, "/a/x", 1);
	want(v, "/a/x/k", 1);
	want(v, "/a/x/k/h", 1);
	want(v, "/a/y", 0);
	want(v, "/b", 0);
	want_counts(v, 5);
	want_ret(v, "a second mount", mount_ret("/a/x/k/h"), -EBUSY);
	want_ret(v, "a mount of nothing", mount_ret("/a/x/none"), -ENOENT);
	mount("/a/x/h2");
	want(v, "/a/x", 2);
	want(v, "/a/x/k", 1);
	want(v, "/", 2);
	sound(v);
	umount("/a/x/h2");
	want(v, "/a/x", 1);
	want_counts(v, 5);
	umount("/a/x/k/h");
	want(v, "/", 0);
	want(v, "/a/x/k", 0);
	want_counts(v, 0);
	want_ret(v, "a second unmount", umount_ret("/a/x/k/h"), -EINVAL);
	sound(v);
}

static void case_move_inside(struct verdict *v)
{
	mount("/a/x/k/h");
	mv("/a/x/k", "/a/y/k");
	want(v, "/a", 1);
	want(v, "/a/x", 0);
	want(v, "/a/y", 1);
	want(v, "/a/y/k", 1);
	want(v, "/a/y/k/h", 1);
	want(v, "/", 1);
	want_counts(v, 5);		/* /, a, y, k, h: x has none left */
	if (moves_with_count() != 1)
		fail(v, "%lu moves took a count along, 1 expected",
		     moves_with_count());
	sound(v);
}

static void case_move_across(struct verdict *v)
{
	mount("/a/x/k/h");
	mv("/a/x/k", "/b/k");
	want(v, "/a", 0);
	want(v, "/a/x", 0);
	want(v, "/b", 1);
	want(v, "/b/k/h", 1);
	want(v, "/", 1);
	want_counts(v, 4);
	sound(v);
	mv("/b/k/h", "/z/h");		/* the mountpoint itself */
	want(v, "/b", 0);
	want(v, "/b/k", 0);
	want(v, "/z", 1);
	want_counts(v, 3);
	sound(v);
}

static void case_move_two(struct verdict *v)
{
	mkfile(H2, "/a/x/k/h2");
	mount("/a/x/k/h");
	mount("/a/x/k/h2");
	mount("/a/x/k");		/* a directory, with two below it */
	want(v, "/a/x/k", 3);
	mv("/a/x/k", "/p/q/k");
	want(v, "/a", 0);
	want(v, "/p", 3);
	want(v, "/p/q", 3);
	want(v, "/p/q/k", 3);
	want(v, "/", 3);
	sound(v);
	umount("/p/q/k");
	want(v, "/p/q/k", 2);		/* no mountpoint, and still a count */
	want(v, "/p", 2);
	sound(v);
}

static void case_move_in_place(struct verdict *v)
{
	mount("/a/x/k/h");
	mv("/a/x/k", "/a/x/k2");
	mv("/a/x/k2/h", "/a/x/k2/h3");
	want(v, "/a/x", 1);
	want(v, "/a/x/k2", 1);
	want(v, "/a/x/k2/h3", 1);
	if (moves_with_count() != 0)
		fail(v, "a rename within a directory took the slow way");
	sound(v);
}

static void case_move_up_down(struct verdict *v)
{
	mount("/a/x/k/h");
	mv("/a/x/k", "/a/k");		/* to an ancestor of where it was */
	want(v, "/a", 1);
	want(v, "/a/x", 0);
	want(v, "/a/k", 1);
	want_counts(v, 4);
	sound(v);
	mv("/a/k", "/a/y/j/k");		/* and down again */
	want(v, "/a", 1);
	want(v, "/a/y", 1);
	want(v, "/a/y/j", 1);
	want_counts(v, 6);
	sound(v);
	mv("/a/y/j/k", "/k");		/* to the root */
	want(v, "/a", 0);
	want(v, "/k", 1);
	want(v, "/", 1);
	want_counts(v, 3);
	sound(v);
}

/* An exchange gives each the other's name and place: k is j afterwards. */
static void case_exchange(struct verdict *v)
{
	mkfile(H2, "/a/y/j/h2");
	mkfile(M, "/a/y/j/m");
	mount("/a/x/k/h");
	xchg("/a/x/k", "/a/y/j");	/* one with a count, one without */
	want(v, "/a/x", 0);
	want(v, "/a/y", 1);
	want(v, "/a/y/j/h", 1);
	want(v, "/a/x/k", 0);
	want(v, "/a", 1);
	sound(v);
	mount("/a/x/k/h2");
	mount("/a/x/k/m");
	want(v, "/a/x", 2);
	xchg("/a/x/k", "/a/y/j");	/* two against one */
	want(v, "/a/x", 1);
	want(v, "/a/y", 2);
	want(v, "/a/x/k/h", 1);
	want(v, "/a", 3);
	sound(v);
	umount("/a/y/j/m");
	xchg("/a/x/k", "/a/y/j");	/* one against one: no count changes */
	want(v, "/a/x", 1);
	want(v, "/a/y", 1);
	want(v, "/a/x/k/h2", 1);
	want(v, "/a/y/j/h", 1);
	sound(v);
	xchg("/a/x/k", "/b");		/* across the tree */
	want(v, "/a", 1);
	want(v, "/a/x", 0);
	want(v, "/b/h2", 1);
	want(v, "/a/x/k", 0);
	want(v, "/", 2);
	sound(v);
}

/* The engine's own check refuses this before a count is looked at. */
static void case_cycle(struct verdict *v)
{
	mkdir_(G, "/a/x/k/g");
	mount("/a/x/k/h");
	want_ret(v, "a move below itself", mv_ret("/a/x/k", "/a/x/k/g/k"),
		 -EINVAL);
	want(v, "/a/x/k", 1);
	want(v, "/a/x/k/g", 0);
	want_counts(v, 5);
	sound(v);
}

static void case_busy(struct verdict *v)
{
	struct dc_path p;

	mkdir_(E, "/a/e");
	mount("/a/x/k/h");
	mount("/a/e");
	want_ret(v, "the removal of a mountpoint",
		 dc_unlink(g_dc, P(&p, "/a/x/k/h")), -EBUSY);
	want_ret(v, "the removal of a mountpoint that is a directory",
		 dc_unlink(g_dc, P(&p, "/a/e")), -EBUSY);
	want_ret(v, "dc_delete() of a mountpoint",
		 dc_delete(g_dc, P(&p, "/a/x/k/h")), -EBUSY);
	sound(v);
	umount("/a/x/k/h");
	umount("/a/e");
	want_ret(v, "the removal once unmounted",
		 dc_unlink(g_dc, P(&p, "/a/x/k/h")), 0);
	want_ret(v, "the removal once unmounted",
		 dc_unlink(g_dc, P(&p, "/a/e")), 0);
	want_counts(v, 0);
}

#define DEPTH	20

static char *deep(const char *top, int n)
{
	static char buf[2][DC_NAME_MAX * 30];
	static int k;
	char *s = buf[k++ & 1];
	int i, len;

	len = sprintf(s, "/%s", top);
	for (i = 0; i < n; i++)
		len += sprintf(s + len, "/d%d", i);
	return s;
}

/* Two chains of twenty directories: more counts than a descriptor starts with. */
static void case_deep(struct verdict *v)
{
	char from[DC_NAME_MAX * 30], to[DC_NAME_MAX * 30], m[DC_NAME_MAX * 30];
	int i;

	mkdir_(DEEP, "/s");
	mkdir_(DEEP + 1, "/t");
	for (i = 1; i <= DEPTH; i++) {
		mkdir_(DEEP + 2 * (uint64_t) i, deep("s", i));
		mkdir_(DEEP + 2 * (uint64_t) i + 1, deep("t", i));
	}
	snprintf(m, sizeof(m), "%s/m", deep("s", DEPTH));
	mkfile(M, m);
	mount(m);
	want(v, "/s", 1);
	want(v, deep("s", DEPTH), 1);
	want_counts(v, DEPTH + 3);		/* /, s, twenty, m */
	sound(v);
	snprintf(from, sizeof(from), "%s", deep("s", 10));	/* ten below it */
	snprintf(to, sizeof(to), "%s/moved", deep("t", DEPTH));
	mv(from, to);
	want(v, "/s", 0);
	want(v, deep("s", 9), 0);
	want(v, "/t", 1);
	want(v, deep("t", DEPTH), 1);
	want(v, to, 1);
	want_counts(v, 1 + 1 + DEPTH + 11 + 1);	/* /, t, twenty, moved+10, m */
	sound(v);
}

static void case_find(struct verdict *v)
{
	struct dc_path p;
	uint64_t id = 0;

	want_ret(v, "the search with nothing mounted",
		 dc_mount_find(g_dc, P(&p, "/a"), &id), 0);
	mount("/a/x/k/h");
	want_ret(v, "the search above a mountpoint",
		 dc_mount_find(g_dc, P(&p, "/a"), &id), 1);
	if (id != H)
		fail(v, "the search found %llu, not the mountpoint",
		     (unsigned long long) id);
	want_ret(v, "the search beside it",
		 dc_mount_find(g_dc, P(&p, "/a/y"), &id), 0);
	mv("/a/x/k", "/a/y/j/k");
	id = 0;
	want_ret(v, "the search after a move",
		 dc_mount_find(g_dc, P(&p, "/a/y"), &id), 1);
	if (id != H)
		fail(v, "after a move the search found %llu",
		     (unsigned long long) id);
	mount("/a/y/j");
	id = 0;
	want_ret(v, "the search above two",
		 dc_mount_find(g_dc, P(&p, "/"), &id), 1);
	if (id != J)			/* the first on the way down */
		fail(v, "above two the search found %llu",
		     (unsigned long long) id);
	want_ret(v, "the search of nothing",
		 dc_mount_find(g_dc, P(&p, "/none"), &id), -ENOENT);
	sound(v);
}

/* ---- the cases with a hook ---- */

static uint64_t g_at;			/* the hook acts at this one */
static void (*g_hook_step)(void);
static int g_hook_async;		/* the step may have to wait for us */
static int g_fired, g_pending, g_step_ret;
static long g_read_x, g_read_y;

static void hook_act(void)
{
	g_fired = 1;
	if (!g_hook_async) {
		other_do(g_hook_step);
		return;
	}
	other_start(g_hook_step);
	g_pending = !other_done_within(200);
}

static void at_hook(uint64_t id)
{
	if (id == g_at && !g_fired)
		hook_act();
}

static void locked_hook(void)
{
	if (!g_fired)
		hook_act();
}

static void arm_step(void (*step)(void), int async)
{
	g_hook_step = step;
	g_hook_async = async;
	g_fired = g_pending = 0;
	g_step_ret = 12345;
}

/* When an operation has frozen @at. */
static void arm(uint64_t at, void (*step)(void), int async)
{
	arm_step(step, async);
	g_at = at;
	dc_test_mnt_frozen_hook = at_hook;
}

/* When a move holds the buckets of its counts. */
static void arm_locked(void (*step)(void), int async)
{
	arm_step(step, async);
	dc_test_mnt_locked_hook = locked_hook;
}

/* When a search has come down to @at. */
static void arm_found(uint64_t at, void (*step)(void))
{
	arm_step(step, 0);
	g_at = at;
	dc_test_mnt_found_hook = at_hook;
}

/* The operation that was parked is over: so is what the hook started. */
static void settle(struct verdict *v, int has_to_wait)
{
	dc_test_mnt_frozen_hook = NULL;
	dc_test_mnt_locked_hook = NULL;
	dc_test_mnt_found_hook = NULL;
	if (!g_fired) {
		fail(v, "the hook did not fire");
		return;
	}
	if (g_pending)
		other_join();
	if (g_hook_async && has_to_wait != g_pending)
		fail(v, has_to_wait ? "the other thread did not wait" :
		     "the other thread waited");
}

static void step_mv_p(void)
{
	g_step_ret = mv_ret("/p", "/z/p");
}

static void want_k_below_z(struct verdict *v)
{
	want_ret(v, "the move of p", g_step_ret, 0);
	want(v, "/z", 1);
	want(v, "/z/p", 1);
	want(v, "/z/p/q", 1);
	want(v, "/z/p/q/k/h", 1);
	want(v, "/a", 0);
	want(v, "/", 1);
	want_counts(v, 6);
}

static void case_ancestor_moves_first(struct verdict *v)
{
	mount("/a/x/k/h");
	arm(Q, step_mv_p, 0);
	mv("/a/x/k", "/p/q/k");
	settle(v, 0);
	want_k_below_z(v);
	sound(v);
}

/*
 * p counts k already when it moves.  In the table that is a count of zero
 * and p moves under its bucket, which is free: the rename finds p elsewhere
 * and starts over.  In the dentries p counts what k has, and its move waits.
 */
static void case_ancestor_moves_late(struct verdict *v)
{
	mount("/a/x/k/h");
	arm(PP, step_mv_p, inline_arm());
	mv("/a/x/k", "/p/q/k");
	settle(v, inline_arm());
	want_k_below_z(v);
	if (!inline_arm() && !again())
		fail(v, "the rename did not work its counts out again");
	sound(v);
}

static void case_ancestor_move_waits(struct verdict *v)
{
	mount("/a/x/k/h");
	arm_locked(step_mv_p, 1);
	mv("/a/x/k", "/p/q/k");
	settle(v, 1);
	want_k_below_z(v);
	if (moves_with_count() != 2)
		fail(v, "%lu moves took a count along, 2 expected",
		     moves_with_count());
	sound(v);
}

static void case_mount_under_move(struct verdict *v)
{
	mkfile(M, "/p/q/m");
	arm(PP, step_mv_p, inline_arm());
	mount("/p/q/m");
	settle(v, inline_arm());
	want_ret(v, "the move of p", g_step_ret, 0);
	want(v, "/z", 1);
	want(v, "/z/p/q/m", 1);
	want(v, "/", 1);
	want_counts(v, 5);
	if (!inline_arm() && !again())
		fail(v, "the mount did not work its counts out again");
	sound(v);
}

static void step_read(void)
{
	struct dc_path p;

	g_read_x = dc_mount_count(g_dc, P(&p, "/a/x"));
	g_read_y = dc_mount_count(g_dc, P(&p, "/a/y"));
}

static void moved_hook(void)
{
	g_fired = 1;
	other_do(step_read);		/* takes no lock, so it does not wait */
}

static void case_reader_at_commit(struct verdict *v)
{
	mount("/a/x/k/h");
	g_fired = 0;
	g_read_x = g_read_y = -1;
	dc_test_mnt_moved_hook = moved_hook;
	mv("/a/x/k", "/a/y/k");
	dc_test_mnt_moved_hook = NULL;
	/*
	 * The new directory counts it.  The old one does not, where the count
	 * went with the move in one commit; where it is taken away after, it
	 * may still.
	 */
	if (!g_fired)
		fail(v, "the hook did not fire");
	else if (g_read_y != 1 || (!inline_arm() && g_read_x != 0))
		fail(v, "once the move had committed a reader found %ld in "
		     "the old directory and %ld in the new", g_read_x, g_read_y);
	want(v, "/a/x", 0);
	want(v, "/a/y", 1);
	sound(v);
}

/* About to commit: k is still in /a/x, which has to count it. */
static void case_reader_before_commit(struct verdict *v)
{
	mount("/a/x/k/h");
	g_read_x = g_read_y = -1;
	arm_locked(step_read, 0);
	mv("/a/x/k", "/a/y/k");
	settle(v, 0);
	if (g_read_x != 1)
		fail(v, "before the move had committed a reader found %ld in "
		     "the directory it was still in", g_read_x);
	sound(v);
}

static void step_mk_dest(void)
{
	mkdir_(G, "/p/q/k");
}

static void case_exists(struct verdict *v)
{
	mount("/a/x/k/h");
	arm(Q, step_mk_dest, 0);
	want_ret(v, "a move onto a name taken meanwhile",
		 mv_ret("/a/x/k", "/p/q/k"), -EEXIST);
	settle(v, 0);
	want(v, "/a/x", 1);
	want(v, "/p", 0);
	want(v, "/p/q", 0);
	want_counts(v, 5);		/* q and p were frozen for it */
	sound(v);
}

static void step_rm_e(void)
{
	struct dc_path p;

	g_step_ret = dc_unlink(g_dc, P(&p, "/a/e"));
}

static void case_unlink_waits(struct verdict *v)
{
	mkdir_(E, "/a/e");
	mount("/a/x/k/h");
	arm(E, step_rm_e, 1);
	mv("/a/x/k", "/a/e/k");
	settle(v, 1);
	want_ret(v, "the removal of the directory it landed in", g_step_ret,
		 -ENOTEMPTY);
	want(v, "/a/e", 1);
	want(v, "/a/e/k/h", 1);
	sound(v);
}

static void step_in_and_out(void)
{
	mv("/b/m2", "/p/m2");
	mv("/p/m2", "/b/m2");
}

static void case_pin_keeps_frozen(struct verdict *v)
{
	mkfile(M2, "/b/m2");
	mount("/b/m2");
	mount("/a/x/k/h");
	arm(PP, step_in_and_out, 0);
	mv("/a/x/k", "/p/q/k");
	settle(v, 0);
	want(v, "/p", 1);
	want(v, "/p/q", 1);
	want(v, "/b", 1);
	want(v, "/", 2);
	sound(v);
}

/* The other thread's rename, parked under its locks, about to commit. */
static void move_hook(int locked)
{
	struct timespec ts;

	if (!t_other || !locked)
		return;
	sem_post(&g_parked);
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 200000000L;
	ts.tv_sec += ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;
	(void) sem_timedwait(&g_release, &ts);	/* or we are what it waits for */
}

/* Have the other thread move p, and come back when it is parked. */
static void park_move_of_p(void)
{
	g_step_ret = 12345;
	dc_test_scan_move_hook = move_hook;
	other_start(step_mv_p);
	sem_wait(&g_parked);
}

static void unpark_move_of_p(void)
{
	sem_post(&g_release);
	other_join();
	dc_test_scan_move_hook = NULL;
	while (sem_trywait(&g_release) == 0)
		;
}

static void want_m_below_z(struct verdict *v)
{
	want_ret(v, "the move of p", g_step_ret, 0);
	want(v, "/z", 1);
	want(v, "/z/p", 1);
	want(v, "/z/p/q/m", 1);
	want(v, "/", 1);
	want_counts(v, 5);
}

/*
 * A mount, not a rename, is what has to wait here.  A rename would be held
 * back by the engine's own check for cycles, which lets none through while an
 * ancestor of its new parent is being moved; a mount has no such check.
 */
static void case_freeze_under_move(struct verdict *v)
{
	mkfile(M, "/p/q/m");
	park_move_of_p();		/* it has read that p has no flag */
	mount("/p/q/m");
	unpark_move_of_p();
	want_m_below_z(v);
	sound(v);
}

static void park_at_p(uint64_t id)
{
	if (id != PP || g_fired)
		return;
	g_fired = 1;
	park_move_of_p();		/* it has p's flag, and now p's bucket */
}

static void case_move_holds_bucket(struct verdict *v)
{
	mkfile(M, "/p/q/m");
	g_fired = 0;
	dc_test_mnt_frozen_hook = park_at_p;
	mount("/p/q/m");
	dc_test_mnt_frozen_hook = NULL;
	if (!g_fired)
		fail(v, "the hook did not fire");
	else
		unpark_move_of_p();
	want_m_below_z(v);
	if (!again())
		fail(v, "the mount did not work its counts out again");
	sound(v);
}

static void step_swap_under_find(void)
{
	mv("/a/x/k", "/b/k");
	umount("/b/k/h");
	mv("/z/w", "/b/k/w");
}

static void case_find_moved(struct verdict *v)
{
	struct dc_path p;
	uint64_t id = 0;
	long ret;

	mkdir_(WW, "/z/w");
	mkfile(M2, "/z/w/m");
	mount("/z/w/m");
	mount("/a/x/k/h");
	arm_found(K, step_swap_under_find);
	ret = dc_mount_find(g_dc, P(&p, "/a/x"), &id);
	settle(v, 0);
	if (ret == 1)
		fail(v, "the search handed back %llu, which was never below "
		     "/a/x", (unsigned long long) id);
	else
		want_ret(v, "the search", ret, 0);
	if (!find_again())
		fail(v, "the search did not go again");
	want(v, "/b/k/w/m", 1);
	sound(v);
}

/* ---- */

static const struct {
	const char *name;
	void (*run)(struct verdict *v);
	int table_only;		/* about pins and buckets, which one arm has */
} cases[] = {
	{ "mount-counts",		case_mount_counts, 0 },
	{ "move-inside",		case_move_inside, 0 },
	{ "move-across",		case_move_across, 0 },
	{ "move-two",			case_move_two, 0 },
	{ "move-in-place",		case_move_in_place, 0 },
	{ "move-up-down",		case_move_up_down, 0 },
	{ "exchange",			case_exchange, 0 },
	{ "cycle",			case_cycle, 0 },
	{ "busy",			case_busy, 0 },
	{ "deep",			case_deep, 0 },
	{ "find",			case_find, 0 },
	{ "ancestor-moves-first",	case_ancestor_moves_first, 0 },
	{ "ancestor-moves-late",	case_ancestor_moves_late, 0 },
	{ "ancestor-move-waits",	case_ancestor_move_waits, 0 },
	{ "mount-under-move",		case_mount_under_move, 0 },
	{ "reader-before-commit",	case_reader_before_commit, 0 },
	{ "reader-at-commit",		case_reader_at_commit, 0 },
	{ "unlink-waits",		case_unlink_waits, 0 },
	{ "exists",			case_exists, 0 },
	{ "pin-keeps-frozen",		case_pin_keeps_frozen, 1 },
	{ "freeze-under-move",		case_freeze_under_move, 0 },
	{ "move-holds-bucket",		case_move_holds_bucket, 1 },
	{ "find-moved",			case_find_moved, 0 },
};
#define NCASES	((int) (sizeof(cases) / sizeof(cases[0])))

int main(int argc, char **argv)
{
	const char *must_fail = argc > 1 ? argv[1] : NULL;
	int failed = 0, wanted = 0, known = 0, ran = 0, i;
	pthread_t t;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (i = 0; must_fail && i < NCASES; i++)
		known |= !strcmp(must_fail, cases[i].name);
	if (must_fail && !known) {
		fprintf(stderr, "no case named %s\n", must_fail);
		return 2;
	}
	rcu_register_thread();
	sem_init(&g_go, 0, 0);
	sem_init(&g_done, 0, 0);
	sem_init(&g_parked, 0, 0);
	sem_init(&g_release, 0, 0);
	pthread_create(&t, NULL, other, NULL);
	printf("== repro_mount (engine: %s, mount arm: %s) ==\n",
	       dc_engine_name(), dc_mount_arm());

	for (i = 0; i < NCASES; i++) {
		struct verdict v = { 0, "" };

		/* a build that takes a rule away may not survive the others */
		if (must_fail && strcmp(must_fail, cases[i].name))
			continue;
		if (cases[i].table_only && inline_arm())
			continue;
		ran++;
		g_errs = 0;
		tree();
		cases[i].run(&v);
		if (g_errs)
			fail(&v, "%d operations failed", g_errs);
		rcu_quiescent_state();
		dc_destroy(g_dc);
		printf("%-22s: %s%s%s\n", cases[i].name, v.fail ? "FAIL" : "ok",
		       v.fail ? " -- " : "", v.why);
		failed += v.fail != 0;
		if (must_fail && !strcmp(must_fail, cases[i].name))
			wanted = v.fail != 0;
	}
	g_quit = 1;
	sem_post(&g_go);
	pthread_join(t, NULL);
	rcu_unregister_thread();
	if (must_fail) {
		printf("RESULT: %s (%s %s)\n", wanted ? "as expected" : "VACUOUS",
		       must_fail, wanted ? "fails" : "passes");
		return wanted ? 0 : 1;
	}
	printf("RESULT: %s (%d of %d cases failed)\n", failed ? "FAIL" : "PASS",
	       failed, ran);
	return failed != 0;
}
