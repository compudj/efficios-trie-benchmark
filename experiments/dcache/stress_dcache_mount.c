// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * stress_dcache_mount.c -- the mount count (dcache_mount.h) under moves.
 *
 * repro_mount.c says what each rule is for, one interleaving at a time.  This
 * runs everything at once and holds the counts to what is true whatever the
 * interleaving.
 *
 * THE TREE.  Three sides, /in, /out and /quiet, each with D fixed directories
 * that never move.  What moves:
 *
 *   big racks  b<m>   a directory that goes from one fixed directory to another
 *   racks      r<j>   a directory, holding the directory t, that goes from a
 *                     fixed directory or a big rack to another
 *   carriers   k<c>   a directory holding the file h<c>, WHICH IS A MOUNTPOINT
 *                     for the whole run, and beside it a directory g<c> that
 *                     holds nothing mounted.  Each goes from a fixed directory
 *                     or the t of a rack to another, and the two are exchanged.
 *
 * So a carrier lands in a directory whose ancestors, the rack and the big
 * rack, are being moved by two other threads: the race that mnt_freeze() is
 * for.  A rack that holds a carrier moves with a count, an empty one without,
 * and which of the two it is changes under its mover.
 *
 * Each of those has a KIND that says where it may go: "in" stays below /in,
 * "free" goes anywhere below /in and /out, "quiet" stays below /quiet and
 * never holds anything mounted.  A mounter mounts and unmounts files that do
 * not move, some in fixed directories of /in and /out, some in the t of a
 * rack, whose way up moves.
 *
 * WHAT HOLDS, and what the readers hold it to with dc_mount_count() while
 * everything runs:
 *
 *   /in      never counts fewer than the carriers of kind "in": each of those
 *            mountpoints is below /in for the whole run, whatever moves.  This
 *            is the question d_invalidate() asks, and a walk gets it wrong.
 *   /quiet   counts nothing, ever.
 *   /        counts every mountpoint: the carriers, and no more than the
 *            files the mounters have beside.  Exactly the carriers when there
 *            is no mounter.  "No more" is for the counts kept in a table,
 *            which go with a move in one commit; kept in the dentries they
 *            are added before a move and taken away after, and a reader may
 *            find one too high meanwhile, which is counted and printed.
 *   dc_mount_find(/in) finds a mountpoint, and what it hands back is one.
 *   A mountpoint that a lookup finds below a directory is in the count of
 *            that directory read next, unless its carrier has begun another
 *            move meanwhile: the count goes with the move, in one commit.
 *
 * A thread of its own runs dc_mount_check() throughout: every count is what
 * is below its dentry.  With the counts in the dentries it always can.  With
 * the counts in a table it needs -DDC_TEST_MNT_CHECK_ONLINE, a build that
 * shuts the operations that change counts out while it counts again and is
 * not the one to time.  At the end everything is unmounted and no dentry has
 * a count left.
 *
 * A move may fail with -ENOENT, and only that: a thread knows where its own
 * object is, and reads where the racks are from what their movers publish
 * after the fact.
 *
 * ./stress_dcache_mount [carriers [shufflers [mounters [readers [moves
 *                        [find-every]]]]]]
 *
 * find-every: a reader calls dc_mount_find() once in so many rounds (1024; 0
 * for never).
 *
 * Without -DDC_MOUNT_COUNT it builds too, mounts nothing and checks nothing:
 * the same moves, for what they cost without the count.
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
#include <time.h>
#include <unistd.h>

#include <urcu-qsbr.h>
#include <urcu/uatomic.h>

#include "dcache.h"

#ifndef D
#define D		4		/* fixed directories a side */
#endif
#define NFIXED		(3 * D)
#define MAXT		64
#define FILES_PER_MOUNTER 4

enum kind { K_IN, K_FREE, K_QUIET };
static const char *const side_dir[] = { "/in/d", "/out/e", "/quiet/q" };

static struct dcache *g_dc;
static int g_ncar, g_nshuf, g_nmnt, g_nread;
static long g_moves;
static int g_nin;			/* carriers of kind "in" */
static unsigned long g_find_every = 1024; /* reads between two dc_mount_find() */
static int g_stop;
static unsigned long g_errs;

/* Where the racks are, as their movers last said. */
static int g_big_loc[MAXT];		/* a fixed directory */
static int g_rack_loc[2 * MAXT];	/* a fixed directory, or NFIXED + a big rack */

static unsigned long g_bad_in, g_bad_quiet, g_bad_root, g_bad_find, g_bad_check;
static unsigned long g_high_in, g_high_root;	/* above the most there can be */
static unsigned long g_bad_under, g_samples;
static long g_seen_in_min = 1L << 30;

#define ID_FIXED	100ULL
#define ID_BIG		1000ULL
#define ID_RACK		2000ULL
#define ID_RACK_T	3000ULL
#define ID_K		10000ULL
#define ID_G		20000ULL
#define ID_H		30000ULL
#define ID_F		40000ULL	/* a mounter's files */

static struct dc_path *P(struct dc_path *p, const char *s)
{
	if (dc_path_parse(p, s) != 0) {
		fprintf(stderr, "bad path %s\n", s);
		exit(2);
	}
	return p;
}

static void fixed_path(char *buf, int i)
{
	sprintf(buf, "%s%d", side_dir[i / D], i % D);
}

static void big_path(char *buf, int m)
{
	fixed_path(buf, uatomic_load(&g_big_loc[m], CMM_RELAXED));
	sprintf(buf + strlen(buf), "/b%d", m);
}

/* The directory that the rack @j is in. */
static void rack_parent(char *buf, int loc)
{
	if (loc < NFIXED)
		fixed_path(buf, loc);
	else
		big_path(buf, loc - NFIXED);
}

static void rack_path(char *buf, int j)
{
	rack_parent(buf, uatomic_load(&g_rack_loc[j], CMM_RELAXED));
	sprintf(buf + strlen(buf), "/r%d", j);
}

/* A place for a carrier: a fixed directory, or NFIXED + a rack (its t). */
static void place_path(char *buf, int loc)
{
	if (loc < NFIXED) {
		fixed_path(buf, loc);
	} else {
		rack_path(buf, loc - NFIXED);
		strcat(buf, "/t");
	}
}

static inline int kind_of_shuffler(int s)
{
	return s % 3;
}

struct rng {
	uint64_t s;
};

static inline uint64_t rnd(struct rng *r)
{
	r->s ^= r->s << 13;
	r->s ^= r->s >> 7;
	r->s ^= r->s << 17;
	return r->s;
}

/* A fixed directory that something of @kind may be in. */
static int pick_fixed(struct rng *r, int kind)
{
	switch (kind) {
	case K_IN:
		return (int) (rnd(r) % D);
	case K_QUIET:
		return 2 * D + (int) (rnd(r) % D);
	default:
		return (int) (rnd(r) % (2 * D));
	}
}

/* May something of kind @what be inside something of kind @holder? */
static inline int may_hold(int holder, int what)
{
	if (what == K_QUIET || holder == K_QUIET)
		return what == holder;
	return what == K_FREE || holder == K_IN;
}

static void must(int ret, const char *what, const char *path)
{
	if (ret) {
		fprintf(stderr, "  %s %s: %d\n", what, path, ret);
		uatomic_inc(&g_errs);
	}
}

static int mv(const char *from, const char *to)
{
	struct dc_path a, b;
	int ret = dc_rename(g_dc, P(&a, from), P(&b, to));

	if (ret && ret != -ENOENT) {
		fprintf(stderr, "  rename %s -> %s: %d\n", from, to, ret);
		uatomic_inc(&g_errs);
	}
	return ret;
}

#ifdef DC_MOUNT_COUNT
static long count_at(const char *path)
{
	struct dc_path p;

	return dc_mount_count(g_dc, P(&p, path));
}
#endif

struct tstat {
	unsigned long ok, stale;
	unsigned long reads, samples;
} __attribute__((aligned(64)));
static struct tstat g_stat[4 * MAXT];

/* ---- the carriers ---- */

struct carrier {
	int c, kind;
	int loc[2];		/* where k<c> and g<c> are */
	int alt[2];		/* each renamed in place an odd number of times */
	int mounted;		/* which of the two names the mountpoint is under */
	/*
	 * For the readers.  @dst: where the mountpoint is once the operation
	 * that was last begun has committed, as DST().  @started: how many
	 * were begun.  Written in that order.
	 */
	unsigned long dst __attribute__((aligned(64)));
	unsigned long started;
	unsigned long cur;	/* where the mountpoint is, as DST(): after the fact */
};
#define DST(loc, slot, alt)	(((unsigned long) (loc) << 2) | \
				 ((unsigned long) (slot) << 1) | (unsigned long) (alt))
static struct carrier g_car[MAXT];

static void slot_path(char *buf, const struct carrier *c, int slot, int loc,
		      int alt)
{
	place_path(buf, loc);
	sprintf(buf + strlen(buf), "/%c%d", "kgKG"[slot + 2 * alt], c->c);
}

/* A place that a carrier of @kind may be in. */
static int pick_place(struct rng *r, int kind)
{
	int tries;

	if (!g_nshuf || rnd(r) % 3 == 0)
		return pick_fixed(r, kind);
	for (tries = 0; tries < 8; tries++) {
		int j = (int) (rnd(r) % (uint64_t) (2 * g_nshuf));

		if (may_hold(kind_of_shuffler(j / 2), kind))
			return NFIXED + j;
	}
	return pick_fixed(r, kind);
}

static void *carrier_thread(void *arg)
{
	struct carrier *c = arg;
	struct tstat *st = &g_stat[c->c];
	struct rng r = { 0x9e3779b97f4a7c15ULL * (uint64_t) (c->c + 1) };
	char from[256], to[256];
	long n;

	dc_register_thread();
	for (n = 0; n < g_moves; n++) {
		unsigned int what = (unsigned int) (rnd(&r) % 10);
		int slot = (int) (rnd(&r) & 1), ret, m = c->mounted;
		int loc = what < 3 ? c->loc[slot] : pick_place(&r, c->kind);
		unsigned long dst = DST(c->loc[m], m, c->alt[m]);

		if (what < 2)
			dst = DST(c->loc[!m], !m, c->alt[!m]);
		else if (slot == m)
			dst = DST(loc, m, what < 3 ? !c->alt[m] : c->alt[m]);
		uatomic_store(&c->dst, dst, CMM_RELAXED);
		cmm_smp_mb();
		uatomic_inc(&c->started);

		if (what < 2) {			/* the two change places */
			struct dc_path a, b;

			slot_path(from, c, 0, c->loc[0], c->alt[0]);
			slot_path(to, c, 1, c->loc[1], c->alt[1]);
			ret = dc_rename_exchange(g_dc, P(&a, from), P(&b, to));
			if (!ret)
				c->mounted ^= 1;
			else if (ret != -ENOENT)
				must(ret, "exchange", from);
		} else if (what < 3) {		/* a new name where it is */
			slot_path(from, c, slot, c->loc[slot], c->alt[slot]);
			slot_path(to, c, slot, c->loc[slot], !c->alt[slot]);
			ret = mv(from, to);
			if (!ret)
				c->alt[slot] ^= 1;
		} else {			/* to another directory */
			slot_path(from, c, slot, c->loc[slot], c->alt[slot]);
			slot_path(to, c, slot, loc, c->alt[slot]);
			ret = loc == c->loc[slot] ? 0 : mv(from, to);
			if (!ret)
				c->loc[slot] = loc;
		}
		if (ret)
			st->stale++;
		else
			st->ok++;
		uatomic_store(&c->cur, DST(c->loc[c->mounted], c->mounted,
					   c->alt[c->mounted]), CMM_RELAXED);
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

/* ---- the shufflers: one big rack and two racks each ---- */

static void *shuffler_thread(void *arg)
{
	int s = (int) (intptr_t) arg, kind = kind_of_shuffler(s);
	struct tstat *st = &g_stat[MAXT + s];
	struct rng r = { 0xff51afd7ed558ccdULL * (uint64_t) (s + 1) };
	char from[256], to[256];

	dc_register_thread();
	while (!uatomic_load(&g_stop, CMM_RELAXED)) {
		int which = (int) (rnd(&r) % 3), ret;

		if (which == 2) {		/* the big rack */
			int loc = pick_fixed(&r, kind);

			if (loc == g_big_loc[s])
				goto next;
			big_path(from, s);
			fixed_path(to, loc);
			sprintf(to + strlen(to), "/b%d", s);
			ret = mv(from, to);
			if (!ret)
				uatomic_store(&g_big_loc[s], loc, CMM_RELAXED);
		} else {			/* a rack */
			int j = 2 * s + which, loc;

			if (rnd(&r) & 1) {
				int m = (int) (rnd(&r) % (uint64_t) g_nshuf);

				if (!may_hold(kind_of_shuffler(m), kind))
					goto next;
				loc = NFIXED + m;
			} else {
				loc = pick_fixed(&r, kind);
			}
			if (loc == g_rack_loc[j])
				goto next;
			rack_path(from, j);
			rack_parent(to, loc);
			sprintf(to + strlen(to), "/r%d", j);
			ret = mv(from, to);
			if (!ret)
				uatomic_store(&g_rack_loc[j], loc, CMM_RELAXED);
		}
		if (ret)
			st->stale++;
		else
			st->ok++;
next:
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

#ifdef DC_MOUNT_COUNT
/* ---- the mounters ---- */

struct mounter {
	int m;
	int place[FILES_PER_MOUNTER];	/* where each file is: it does not move */
	int mounted[FILES_PER_MOUNTER];
};
static struct mounter g_mnt[MAXT];

static void file_path(char *buf, const struct mounter *m, int f)
{
	place_path(buf, m->place[f]);
	sprintf(buf + strlen(buf), "/f%d_%d", m->m, f);
}

static void *mounter_thread(void *arg)
{
	struct mounter *m = arg;
	struct tstat *st = &g_stat[2 * MAXT + m->m];
	struct rng r = { 0xc4ceb9fe1a85ec53ULL * (uint64_t) (m->m + 1) };
	char path[256];
	struct dc_path p;

	dc_register_thread();
	while (!uatomic_load(&g_stop, CMM_RELAXED)) {
		int f = (int) (rnd(&r) % FILES_PER_MOUNTER), ret;

		file_path(path, m, f);
		ret = m->mounted[f] ? dc_umount(g_dc, P(&p, path)) :
				      dc_mount(g_dc, P(&p, path));
		if (!ret) {
			m->mounted[f] ^= 1;
			st->ok++;
		} else if (ret == -ENOENT) {
			st->stale++;
		} else {
			must(ret, m->mounted[f] ? "umount" : "mount", path);
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

/* ---- the readers ---- */

static int is_mount_id(uint64_t id)
{
	return (id >= ID_H && id < ID_H + (uint64_t) g_ncar) ||
	       (id >= ID_F && id < ID_F + (uint64_t) g_nmnt * FILES_PER_MOUNTER);
}

static void *reader_thread(void *arg)
{
	int rd = (int) (intptr_t) arg;
	struct tstat *st = &g_stat[3 * MAXT + rd];
	long most = g_ncar + (long) g_nmnt * FILES_PER_MOUNTER;
	long seen_min = 1L << 30;
	unsigned long n = 0;

	dc_register_thread();
	while (!uatomic_load(&g_stop, CMM_RELAXED)) {
		long in = count_at("/in"), quiet = count_at("/quiet");
		long root = count_at("/");

		if (in < g_nin)
			uatomic_inc(&g_bad_in);
		else if (in > most)
			uatomic_inc(&g_high_in);
		if (in < seen_min)
			seen_min = in;
		if (quiet != 0)
			uatomic_inc(&g_bad_quiet);
		if (root < g_ncar)
			uatomic_inc(&g_bad_root);
		else if (root > most)
			uatomic_inc(&g_high_root);
		st->reads += 3;
		/*
		 * A lookup finds a mountpoint below a directory, and a count
		 * of that directory read after it says none: what a move that
		 * did not take its count along in one commit looks like.  The
		 * place looked at is where the carrier's last operation takes
		 * the mountpoint, so finding it there says that the operation
		 * has committed; and if the carrier has begun no other since,
		 * it is there still.
		 */
		{
			struct carrier *c = &g_car[++n % (unsigned long) g_ncar];
			unsigned long s1 = uatomic_load(&c->started, CMM_RELAXED);
			unsigned long d;
			char dir[256], file[300];
			struct dc_path p;
			long cnt;

			cmm_smp_mb();
			d = uatomic_load(&c->dst, CMM_RELAXED);
			place_path(dir, (int) (d >> 2));
			sprintf(file, "%s/%c%d/h%d", dir,
				"kgKG"[((d >> 1) & 1) + 2 * (d & 1)], c->c, c->c);
			if (dc_lookup_dentry(g_dc, P(&p, file))) {
				cnt = count_at(dir);
				cmm_smp_mb();
				if (cnt != -ENOENT &&
				    uatomic_load(&c->started, CMM_RELAXED) == s1) {
					st->samples++;
					if (cnt < 1)
						uatomic_inc(&g_bad_under);
				}
			}
			st->reads += 2;
			/*
			 * And where it is before the operation commits: found
			 * there, the directory's count read, found there
			 * still.  It was there when the count was read, and a
			 * count taken away before the move would not say so.
			 */
			d = uatomic_load(&c->cur, CMM_RELAXED);
			place_path(dir, (int) (d >> 2));
			sprintf(file, "%s/%c%d/h%d", dir,
				"kgKG"[((d >> 1) & 1) + 2 * (d & 1)], c->c, c->c);
			if (dc_lookup_dentry(g_dc, P(&p, file))) {
				cnt = count_at(dir);
				if (cnt != -ENOENT &&
				    dc_lookup_dentry(g_dc, P(&p, file))) {
					st->samples++;
					if (cnt < 1)
						uatomic_inc(&g_bad_under);
				}
			}
			st->reads += 3;
		}
		if (g_nin && g_find_every && n % g_find_every == 0) {
			struct dc_path p;
			uint64_t id = 0;

			if (dc_mount_find(g_dc, P(&p, "/in"), &id) != 1 ||
			    !is_mount_id(id))
				uatomic_inc(&g_bad_find);
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	for (;;) {
		long cur = uatomic_load(&g_seen_in_min, CMM_RELAXED);

		if (seen_min >= cur ||
		    uatomic_cmpxchg(&g_seen_in_min, cur, seen_min) == cur)
			break;
	}
	return NULL;
}

/* dc_mount_check() can run with everything else: see dcache.h. */
#if defined(DC_TEST_MNT_CHECK_ONLINE) || defined(DC_MOUNT_INLINE)
# define CHECK_ONLINE 1
#endif

#ifdef CHECK_ONLINE
static unsigned long g_checks;

static void *checker_thread(void *arg)
{
	(void) arg;
	dc_register_thread();
	while (!uatomic_load(&g_stop, CMM_RELAXED)) {
		long bad = dc_mount_check(g_dc, NULL);

		if (bad) {
			if (!uatomic_xchg(&g_bad_check, 1UL))
				(void) dc_mount_check(g_dc, stderr);
			uatomic_add(&g_bad_check, (unsigned long) bad);
		}
		g_checks++;
		dc_quiescent();
		rcu_thread_offline();
		usleep(200);
		rcu_thread_online();
	}
	dc_unregister_thread();
	return NULL;
}
#endif /* CHECK_ONLINE */
#endif /* DC_MOUNT_COUNT */

/* ---- */

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

static void mkdir_(uint64_t id, const char *path)
{
	struct dc_path p;

	must(dc_add(g_dc, P(&p, path), id), "mkdir", path);
}

int main(int argc, char **argv)
{
	pthread_t tcar[MAXT], tshuf[MAXT];
#ifdef DC_MOUNT_COUNT
	pthread_t tmnt[MAXT], tread[MAXT];
#ifdef CHECK_ONLINE
	pthread_t tcheck;
#endif
	struct dc_mount_stats ms;
	unsigned long reads = 0, mok = 0, mstale = 0;
	long left;
#endif
	unsigned long cok = 0, cstale = 0, sok = 0, sstale = 0, qok = 0;
	struct rng r = { 0x2545f4914f6cdd1dULL };
	char path[256];
	struct dc_path p;
	double t0, dt;
	int i, f, bad;

	g_ncar = argc > 1 ? atoi(argv[1]) : 4;
	g_nshuf = argc > 2 ? atoi(argv[2]) : 3;
	g_nmnt = argc > 3 ? atoi(argv[3]) : 1;
	g_nread = argc > 4 ? atoi(argv[4]) : 2;
	g_moves = argc > 5 ? atol(argv[5]) : 30000;
	if (argc > 6)
		g_find_every = strtoul(argv[6], NULL, 0);
	if (g_ncar < 1 || g_ncar > MAXT || g_nshuf < 0 || g_nshuf > MAXT ||
	    g_nmnt < 0 || g_nmnt > MAXT || g_nread < 0 || g_nread > MAXT) {
		fprintf(stderr, "usage: %s [carriers [shufflers [mounters "
			"[readers [moves [find-every]]]]]]\n", argv[0]);
		return 2;
	}
#ifndef DC_MOUNT_COUNT
	g_nmnt = g_nread = 0;
#endif
	setvbuf(stdout, NULL, _IOLBF, 0);
	rcu_register_thread();
	g_dc = dc_create(4096);

	mkdir_(1, "/in");
	mkdir_(2, "/out");
	mkdir_(3, "/quiet");
	for (i = 0; i < NFIXED; i++) {
		fixed_path(path, i);
		mkdir_(ID_FIXED + (uint64_t) i, path);
	}
	for (i = 0; i < g_nshuf; i++) {
		g_big_loc[i] = pick_fixed(&r, kind_of_shuffler(i));
		big_path(path, i);
		mkdir_(ID_BIG + (uint64_t) i, path);
	}
	for (i = 0; i < 2 * g_nshuf; i++) {
		g_rack_loc[i] = pick_fixed(&r, kind_of_shuffler(i / 2));
		rack_path(path, i);
		mkdir_(ID_RACK + (uint64_t) i, path);
		strcat(path, "/t");
		mkdir_(ID_RACK_T + (uint64_t) i, path);
	}
	for (i = 0; i < g_ncar; i++) {
		struct carrier *c = &g_car[i];

		c->c = i;
		c->kind = (i & 1) ? K_FREE : K_IN;	/* the first is "in" */
		g_nin += c->kind == K_IN;
		c->loc[0] = pick_fixed(&r, c->kind);
		c->loc[1] = pick_fixed(&r, c->kind);
		c->dst = DST(c->loc[0], 0, 0);
		c->cur = c->dst;
		slot_path(path, c, 0, c->loc[0], 0);
		mkdir_(ID_K + (uint64_t) i, path);
		sprintf(path + strlen(path), "/h%d", i);
		must(dc_add_file(g_dc, P(&p, path), ID_H + (uint64_t) i),
		     "create", path);
#ifdef DC_MOUNT_COUNT
		must(dc_mount(g_dc, P(&p, path)), "mount", path);
#endif
		slot_path(path, c, 1, c->loc[1], 0);
		mkdir_(ID_G + (uint64_t) i, path);
	}
#ifdef DC_MOUNT_COUNT
	for (i = 0; i < g_nmnt; i++) {
		struct mounter *m = &g_mnt[i];

		m->m = i;
		for (f = 0; f < FILES_PER_MOUNTER; f++) {
			int j = -1, tries;

			/* half of them in the t of a rack that is not quiet */
			for (tries = 0; (f & 1) && g_nshuf && tries < 16; tries++) {
				j = (int) (rnd(&r) % (uint64_t) (2 * g_nshuf));
				if (kind_of_shuffler(j / 2) != K_QUIET)
					break;
				j = -1;
			}
			m->place[f] = j >= 0 ? NFIXED + j : pick_fixed(&r, K_FREE);
			file_path(path, m, f);
			must(dc_add_file(g_dc, P(&p, path),
					 ID_F + (uint64_t) (i * FILES_PER_MOUNTER + f)),
			     "create", path);
		}
	}
	printf("== stress_dcache_mount (engine: %s, mount arm: %s) ==\n",
	       dc_engine_name(), dc_mount_arm());
#else
	printf("== stress_dcache_mount (engine: %s, mount arm: none) ==\n",
	       dc_engine_name());
#endif
	printf("%d carriers (%d stay below /in), %d shufflers, %d mounters, "
	       "%d readers, %ld moves a carrier\n", g_ncar, g_nin, g_nshuf,
	       g_nmnt, g_nread, g_moves);

	t0 = now();
	for (i = 0; i < g_nshuf; i++)
		pthread_create(&tshuf[i], NULL, shuffler_thread,
			       (void *) (intptr_t) i);
#ifdef DC_MOUNT_COUNT
	for (i = 0; i < g_nmnt; i++)
		pthread_create(&tmnt[i], NULL, mounter_thread, &g_mnt[i]);
	for (i = 0; i < g_nread; i++)
		pthread_create(&tread[i], NULL, reader_thread,
			       (void *) (intptr_t) i);
#ifdef CHECK_ONLINE
	pthread_create(&tcheck, NULL, checker_thread, NULL);
#endif
#endif
	for (i = 0; i < g_ncar; i++)
		pthread_create(&tcar[i], NULL, carrier_thread, &g_car[i]);
	rcu_thread_offline();
	for (i = 0; i < g_ncar; i++)
		pthread_join(tcar[i], NULL);
	dt = now() - t0;
	uatomic_store(&g_stop, 1, CMM_RELAXED);
	for (i = 0; i < g_nshuf; i++)
		pthread_join(tshuf[i], NULL);
#ifdef DC_MOUNT_COUNT
	for (i = 0; i < g_nmnt; i++)
		pthread_join(tmnt[i], NULL);
	for (i = 0; i < g_nread; i++)
		pthread_join(tread[i], NULL);
#ifdef CHECK_ONLINE
	pthread_join(tcheck, NULL);
#endif
#endif
	rcu_thread_online();

	for (i = 0; i < g_ncar; i++) {
		cok += g_stat[i].ok;
		cstale += g_stat[i].stale;
	}
	for (i = 0; i < g_nshuf; i++) {
		if (kind_of_shuffler(i) == K_QUIET)
			qok += g_stat[MAXT + i].ok;
		else
			sok += g_stat[MAXT + i].ok;
		sstale += g_stat[MAXT + i].stale;
	}
	printf("carriers: %lu moves (%lu stale), %.0f k/s; racks: %lu moves "
	       "(%lu stale), %.0f k/s; quiet racks: %lu moves, %.0f k/s\n",
	       cok, cstale, (double) cok / dt / 1e3, sok, sstale,
	       (double) sok / dt / 1e3, qok, (double) qok / dt / 1e3);
	bad = g_errs != 0;
#ifdef DC_MOUNT_COUNT
	for (i = 0; i < g_nmnt; i++) {
		mok += g_stat[2 * MAXT + i].ok;
		mstale += g_stat[2 * MAXT + i].stale;
	}
	for (i = 0; i < g_nread; i++) {
		reads += g_stat[3 * MAXT + i].reads;
		g_samples += g_stat[3 * MAXT + i].samples;
	}
	dc_mount_stats(g_dc, &ms);
	printf("mounters: %lu mounts and unmounts (%lu stale); readers: %lu "
	       "reads, %.0f k/s\n", mok, mstale, reads,
	       (double) reads / dt / 1e3);
#ifdef CHECK_ONLINE
	printf("counted again from the mountpoints %lu times while it ran\n",
	       g_checks);
#endif
	printf("counts: %lu moves took one along, %lu dentries frozen for "
	       "them, %lu sets worked out again, %lu searches went again, "
	       "%lu dentries have one now\n", ms.moves, ms.frozen, ms.again,
	       ms.find_again, ms.counts);
	if (g_nread)
		printf("/in: at least %d at all times, the least read was %ld\n",
		       g_nin, g_seen_in_min);
	if (g_nread)
		printf("found below a directory, then that directory's count "
		       "read: %lu times\n", g_samples);
	/*
	 * A count above the most there can be is wrong where the count goes
	 * with the move in one commit.  Where it is added before the move and
	 * taken away after, it is what a reader may find for the length of one
	 * operation, and is said, not held against it.
	 */
	if (!strcmp(dc_mount_arm(), "inline")) {
		printf("too high, as it may be while a rename is under way: "
		       "/in %lu, / %lu of the reads\n", g_high_in, g_high_root);
	} else {
		g_bad_in += g_high_in;
		g_bad_root += g_high_root;
	}
	printf("WRONG: /in %lu, /quiet %lu, / %lu, find %lu, check %lu, "
	       "found and not counted %lu\n", g_bad_in, g_bad_quiet,
	       g_bad_root, g_bad_find, g_bad_check, g_bad_under);
	bad |= g_bad_in || g_bad_quiet || g_bad_root || g_bad_find ||
	       g_bad_check || g_bad_under;
	if (g_nread && !g_samples) {
		printf("VACUOUS: no reader found a mountpoint to count\n");
		bad = 1;
	}

	/* At rest: right, and nothing left once all is unmounted. */
	if (dc_mount_check(g_dc, stderr)) {
		printf("AT REST: the counts are wrong\n");
		bad = 1;
	}
	left = (long) g_ncar;
	for (i = 0; i < g_nmnt; i++)
		for (f = 0; f < FILES_PER_MOUNTER; f++)
			left += g_mnt[i].mounted[f];
	if (count_at("/") != left) {
		printf("AT REST: / counts %ld, %ld are mounted\n",
		       count_at("/"), left);
		bad = 1;
	}
	for (i = 0; i < g_ncar; i++) {
		struct carrier *c = &g_car[i];

		slot_path(path, c, c->mounted, c->loc[c->mounted],
			  c->alt[c->mounted]);
		sprintf(path + strlen(path), "/h%d", i);
		must(dc_umount(g_dc, P(&p, path)), "umount", path);
	}
	for (i = 0; i < g_nmnt; i++) {
		for (f = 0; f < FILES_PER_MOUNTER; f++) {
			if (!g_mnt[i].mounted[f])
				continue;
			file_path(path, &g_mnt[i], f);
			must(dc_umount(g_dc, P(&p, path)), "umount", path);
		}
	}
	dc_mount_stats(g_dc, &ms);
	if (ms.counts || count_at("/") != 0 || g_errs) {
		printf("AT REST: %lu dentries have a count with nothing "
		       "mounted, / counts %ld\n", ms.counts, count_at("/"));
		bad = 1;
	}
#ifndef DC_MNT_NO_STATS
	if (ms.moves == 0 || (g_nshuf && ms.frozen == 0)) {
		printf("VACUOUS: no move took a count along\n");
		bad = 1;
	}
#endif
#else
	(void) f;
#endif
	rcu_quiescent_state();
	dc_destroy(g_dc);
	rcu_unregister_thread();
	printf("RESULT: %s\n", bad ? "FAIL" : "PASS");
	return bad;
}
