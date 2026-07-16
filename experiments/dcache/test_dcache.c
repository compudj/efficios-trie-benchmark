// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * test_dcache.c -- single-threaded correctness harness for the dcache engines
 * (S1).  Engine-agnostic: it links whichever dcache_*.o is built and drives it
 * through the public dcache.h interface.  Covers every operation (lookup / add /
 * unlink / rename same+cross parent / exchange / loop guard) plus a
 * namespace-CONSERVATION check: after a scripted sequence, dc_walk() must report
 * EXACTLY the set of paths we expect -- nothing lost, nothing stray.
 *
 * Concurrency is out of scope here (that is S3's bench_dcache); this pins down
 * single-threaded semantics so the racy story is debugged against a known-good
 * sequential oracle.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <urcu-qsbr.h>

#include "dcache.h"

static int g_checks, g_fails;

#define CHECK(cond, ...) do {						\
	g_checks++;							\
	if (!(cond)) {							\
		g_fails++;						\
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);	\
		fprintf(stderr, __VA_ARGS__);				\
		fprintf(stderr, "\n");					\
	}								\
} while (0)

/* Rotating path buffers so several P("...") fit in one expression. */
static struct dc_path *P(const char *s)
{
	static struct dc_path buf[4];
	static int r;
	struct dc_path *p = &buf[r++ & 3];

	if (dc_path_parse(p, s) != 0) {
		fprintf(stderr, "bad path: %s\n", s);
		exit(2);
	}
	return p;
}

/* ---- lookup assertions -------------------------------------------------- */

static void expect_positive(struct dcache *dc, const char *path, uint64_t id)
{
	uint64_t got = ~0ULL;
	enum dc_result r = dc_lookup(dc, P(path), &got);

	CHECK(r == DC_POSITIVE, "%s: expected POSITIVE got %d", path, r);
	CHECK(got == id, "%s: expected id %lu got %lu", path,
	      (unsigned long) id, (unsigned long) got);
}

static void expect_absent(struct dcache *dc, const char *path)
{
	enum dc_result r = dc_lookup(dc, P(path), NULL);

	CHECK(r == DC_ABSENT, "%s: expected ABSENT got %d", path, r);
}

/* ---- conservation check ------------------------------------------------- */

struct entry {
	uint64_t id;
	char path[256];
};

struct collector {
	struct entry *e;
	int n, cap;
};

static void path_to_str(const struct dc_path *p, char *out, size_t outsz)
{
	size_t off = 0;
	uint32_t i;

	if (p->ndepth == 0) {
		snprintf(out, outsz, "/");
		return;
	}
	for (i = 0; i < p->ndepth; i++)
		off += snprintf(out + off, outsz - off, "/%s", p->comp[i].name);
}

static void collect_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct collector *c = arg;

	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 64;
		c->e = realloc(c->e, c->cap * sizeof(*c->e));
	}
	c->e[c->n].id = id;
	path_to_str(p, c->e[c->n].path, sizeof(c->e[c->n].path));
	c->n++;
}

static int entry_cmp(const void *a, const void *b)
{
	return strcmp(((const struct entry *) a)->path,
		      ((const struct entry *) b)->path);
}

/*
 * Assert the cache holds EXACTLY the given "path id" lines (order-independent).
 * expected is a NULL-terminated array of "path\0id" pairs encoded as two arrays.
 */
static void expect_namespace(struct dcache *dc, const char *const *paths,
			     const uint64_t *ids, int nexp)
{
	struct collector c = { 0 };
	struct entry *exp;
	int i;

	dc_walk(dc, collect_cb, &c);

	CHECK(c.n == nexp, "conservation: %d entries, expected %d", c.n, nexp);

	exp = calloc(nexp, sizeof(*exp));
	for (i = 0; i < nexp; i++) {
		exp[i].id = ids[i];
		snprintf(exp[i].path, sizeof(exp[i].path), "%s", paths[i]);
	}
	qsort(exp, nexp, sizeof(*exp), entry_cmp);
	qsort(c.e, c.n, sizeof(*c.e), entry_cmp);

	for (i = 0; i < nexp && i < c.n; i++) {
		CHECK(strcmp(exp[i].path, c.e[i].path) == 0,
		      "conservation: path[%d] got '%s' expected '%s'",
		      i, c.e[i].path, exp[i].path);
		CHECK(exp[i].id == c.e[i].id,
		      "conservation: '%s' id got %lu expected %lu",
		      exp[i].path, (unsigned long) c.e[i].id,
		      (unsigned long) exp[i].id);
	}
	free(exp);
	free(c.e);
}

/* ---- readdir check ------------------------------------------------------ */

struct dirent_entry {
	uint64_t id;
	char name[DC_NAME_MAX];
};

struct dcollector {
	struct dirent_entry e[64];
	int n;
};

static void readdir_cb(uint64_t id, const struct qstr *name, void *arg)
{
	struct dcollector *c = arg;

	if (c->n < 64) {
		c->e[c->n].id = id;
		snprintf(c->e[c->n].name, sizeof(c->e[c->n].name), "%s", name->name);
		c->n++;
	}
}

static int dirent_cmp(const void *a, const void *b)
{
	return strcmp(((const struct dirent_entry *) a)->name,
		      ((const struct dirent_entry *) b)->name);
}

/* Assert dc_readdir(dirpath) reports EXACTLY the given (name, id) children. */
static void expect_readdir(struct dcache *dc, const char *dirpath,
			   const char *const *names, const uint64_t *ids,
			   int nexp)
{
	struct dcollector c = { .n = 0 };
	struct dirent_entry exp[64];
	long got = dc_readdir(dc, P(dirpath), readdir_cb, &c);
	int i;

	CHECK(got == nexp, "readdir %s: %ld entries, expected %d",
	      dirpath, got, nexp);

	for (i = 0; i < nexp; i++) {
		exp[i].id = ids[i];
		snprintf(exp[i].name, sizeof(exp[i].name), "%s", names[i]);
	}
	qsort(exp, nexp, sizeof(exp[0]), dirent_cmp);
	qsort(c.e, c.n, sizeof(c.e[0]), dirent_cmp);

	for (i = 0; i < nexp && i < c.n; i++) {
		CHECK(strcmp(exp[i].name, c.e[i].name) == 0,
		      "readdir %s: name[%d] got '%s' expected '%s'",
		      dirpath, i, c.e[i].name, exp[i].name);
		CHECK(exp[i].id == c.e[i].id,
		      "readdir %s: '%s' id got %lu expected %lu", dirpath,
		      exp[i].name, (unsigned long) c.e[i].id,
		      (unsigned long) exp[i].id);
	}
}

/*
 * Mid-transition unlink + exchange edge cases, in a private cache with NO
 * quiescence, so on the txn engine every rename leaves an unfolded shell and the
 * unlinks land ON those shells (top != host) -- exercising fold()'s RECLAIM
 * cascade rather than the settled path.  The reclaim drains at dc_destroy's
 * rcu_barriers; under ASan a leak or double free there fails the run.  Behaviour
 * is engine-agnostic: on the seqlock engine these are plain settled unlinks.
 */
static void test_midtransition(void)
{
	static const char *const paths[] = { "/p", "/p/f", "/p/g", "/p/h", "/p/h/i" };
	static const uint64_t     ids[]  = { 100,  104,     103,    105,    106 };
	struct dcache *dc = dc_create(1024);

	CHECK(dc_add(dc, P("/p"), 100) == 0, "mt: add /p");

	/* 1-shell mid-transition unlink: rename leaves a shell, unlink hits it. */
	CHECK(dc_add(dc, P("/p/a"), 101) == 0, "mt: add /p/a");
	CHECK(dc_rename(dc, P("/p/a"), P("/p/b")) == 0, "mt: rename a->b (shell)");
	CHECK(dc_unlink(dc, P("/p/b")) == 0, "mt: unlink shell /p/b");
	expect_absent(dc, "/p/b");
	expect_absent(dc, "/p/a");

	/* 2-shell mid-transition unlink: stack two renames, then unlink the top. */
	CHECK(dc_add(dc, P("/p/c"), 102) == 0, "mt: add /p/c");
	CHECK(dc_rename(dc, P("/p/c"), P("/p/d")) == 0, "mt: rename c->d (shell)");
	CHECK(dc_rename(dc, P("/p/d"), P("/p/e")) == 0, "mt: rename d->e (2 shells)");
	CHECK(dc_unlink(dc, P("/p/e")) == 0, "mt: unlink 2-shell /p/e");
	expect_absent(dc, "/p/e");
	expect_absent(dc, "/p/c");

	/* Same-parent exchange (cross == 0): pure name swap, no reparent. */
	CHECK(dc_add(dc, P("/p/f"), 103) == 0, "mt: add /p/f");
	CHECK(dc_add(dc, P("/p/g"), 104) == 0, "mt: add /p/g");
	CHECK(dc_rename_exchange(dc, P("/p/f"), P("/p/g")) == 0, "mt: exchange f<->g");
	expect_positive(dc, "/p/f", 104);
	expect_positive(dc, "/p/g", 103);

	/* Exchange that would nest a dir under its own descendant: -EINVAL, no-op. */
	CHECK(dc_add(dc, P("/p/h"), 105) == 0, "mt: add /p/h");
	CHECK(dc_add(dc, P("/p/h/i"), 106) == 0, "mt: add /p/h/i");
	CHECK(dc_rename_exchange(dc, P("/p/h"), P("/p/h/i")) == -EINVAL,
	      "mt: cyclic exchange -EINVAL");
	expect_positive(dc, "/p/h", 105);
	expect_positive(dc, "/p/h/i", 106);

	/* Conservation: exactly the survivors (a,b,c,d,e all gone). */
	expect_namespace(dc, paths, ids, 5);

	dc_destroy(dc);			/* rcu_barriers drain the reclaim cascade */
}

/* ---- the scripted test -------------------------------------------------- */

int main(void)
{
	struct dcache *dc;
	int r;

	rcu_register_thread();
	dc = dc_create(1024);
	printf("== test_dcache (engine: %s) ==\n", dc_engine_name());

	test_midtransition();		/* mid-transition unlink + exchange edges */

	/* Build a small tree.
	 *   /a(1) /a/b(2) /a/b/c(3) /a/x(4) /d(5) /d/e(6)
	 */
	CHECK(dc_add(dc, P("/a"), 1) == 0, "add /a");
	CHECK(dc_add(dc, P("/a/b"), 2) == 0, "add /a/b");
	CHECK(dc_add(dc, P("/a/b/c"), 3) == 0, "add /a/b/c");
	CHECK(dc_add(dc, P("/a/x"), 4) == 0, "add /a/x");
	CHECK(dc_add(dc, P("/d"), 5) == 0, "add /d");
	CHECK(dc_add(dc, P("/d/e"), 6) == 0, "add /d/e");

	/* Lookups. */
	expect_positive(dc, "/a", 1);
	expect_positive(dc, "/a/b/c", 3);
	expect_positive(dc, "/d/e", 6);
	expect_absent(dc, "/a/b/nope");
	expect_absent(dc, "/nope/b");
	expect_absent(dc, "/a/b/c/d");	/* c is a leaf */

	/* Error paths. */
	CHECK(dc_add(dc, P("/a/b"), 99) == -EEXIST, "dup add -EEXIST");
	CHECK(dc_add(dc, P("/no/b"), 99) == -ENOENT, "add missing parent -ENOENT");
	CHECK(dc_unlink(dc, P("/a/b")) == -ENOTEMPTY, "unlink non-empty -ENOTEMPTY");
	CHECK(dc_unlink(dc, P("/a/z")) == -ENOENT, "unlink missing -ENOENT");

	/* Unlink a leaf, then confirm it's gone. */
	CHECK(dc_unlink(dc, P("/a/x")) == 0, "unlink /a/x");
	expect_absent(dc, "/a/x");
	rcu_quiescent_state();		/* let the deferred free advance */

	/* Same-parent rename: /a/b/c -> /a/b/z (id preserved). */
	CHECK(dc_rename(dc, P("/a/b/c"), P("/a/b/z")) == 0, "rename c->z");
	expect_absent(dc, "/a/b/c");
	expect_positive(dc, "/a/b/z", 3);

	/* Cross-parent rename: /a/b -> /d/b; its subtree (z) must come along. */
	CHECK(dc_rename(dc, P("/a/b"), P("/d/b")) == 0, "rename a/b->d/b");
	expect_absent(dc, "/a/b");
	expect_positive(dc, "/d/b", 2);
	expect_positive(dc, "/d/b/z", 3);	/* subtree followed the move */

	/* Rename target must not exist (phase 1: no replace). */
	CHECK(dc_add(dc, P("/a/y"), 7) == 0, "add /a/y");
	CHECK(dc_rename(dc, P("/a/y"), P("/d/e")) == -EEXIST, "rename onto existing");

	/* Loop guard: cannot move /d under its own descendant /d/b. */
	CHECK(dc_rename(dc, P("/d"), P("/d/b/d")) == -EINVAL, "loop rename -EINVAL");

	/* Exchange: swap /d/e(6) and /a/y(7). */
	CHECK(dc_rename_exchange(dc, P("/d/e"), P("/a/y")) == 0, "exchange e<->y");
	expect_positive(dc, "/d/e", 7);
	expect_positive(dc, "/a/y", 6);

	/*
	 * readdir: list directories and check their child sets, against the same
	 *   final namespace: root{a,d} /a{y} /d{e,b} /d/b{z}.
	 */
	{
		static const char *const root_names[] = { "a", "d" };
		static const uint64_t root_ids[] = { 1, 5 };
		static const char *const d_names[] = { "e", "b" };
		static const uint64_t d_ids[] = { 7, 2 };
		static const char *const a_names[] = { "y" };
		static const uint64_t a_ids[] = { 6 };

		expect_readdir(dc, "/", root_names, root_ids, 2);
		expect_readdir(dc, "/d", d_names, d_ids, 2);
		expect_readdir(dc, "/a", a_names, a_ids, 1);
		CHECK(dc_readdir(dc, P("/nope"), NULL, NULL) == -ENOENT,
		      "readdir missing dir -ENOENT");
	}

	/*
	 * Conservation: the whole namespace, exactly.
	 *   /a(1) /a/y(6) /d(5) /d/e(7) /d/b(2) /d/b/z(3)
	 */
	{
		static const char *const paths[] = {
			"/a", "/a/y", "/d", "/d/e", "/d/b", "/d/b/z",
		};
		static const uint64_t ids[] = { 1, 6, 5, 7, 2, 3 };

		expect_namespace(dc, paths, ids,
				 (int) (sizeof(ids) / sizeof(ids[0])));
	}

	dc_destroy(dc);
	rcu_unregister_thread();

	printf("checks: %d, failures: %d\n", g_checks, g_fails);
	if (g_fails) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	(void) r;
	return 0;
}
