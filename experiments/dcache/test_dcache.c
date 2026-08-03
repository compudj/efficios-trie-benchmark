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

static void expect_negative(struct dcache *dc, const char *path)
{
	uint64_t got = ~0ULL;
	enum dc_result r = dc_lookup(dc, P(path), &got);

	CHECK(r == DC_NEGATIVE, "%s: expected NEGATIVE got %d", path, r);
	/* A negative dentry yields NO id: dc_lookup writes *out_id only on
	 * DC_POSITIVE, so the caller's value must be untouched.  Checked because
	 * a negative that leaked a stale id would still pass a state-only test. */
	CHECK(got == ~0ULL, "%s: negative yielded an id (%lu)", path,
	      (unsigned long) got);
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

/*
 * PHASE 2 -- negative dentries and d_instantiate.
 *
 * The interesting case is the LAST one: a negative dentry that is RENAMED before
 * being instantiated.  Inode-ness is authoritative on the content host, and a
 * rename stacks a shell as the new top, so this is what proves the state stayed
 * with the content instead of being left behind on the old name -- the failure
 * the pre-phase-2 code would have had, since its shell was always born positive.
 */
static void test_negative_dentries(void)
{
	struct dcache *dc = dc_create(1024);
	uint64_t got = ~0ULL;

	CHECK(dc_add(dc, P("/n"), 700) == 0, "neg: add dir /n");

	/* a name cached as ABSENT is present-but-negative, not absent */
	CHECK(dc_add_negative(dc, P("/n/miss")) == 0, "neg: add_negative");
	expect_negative(dc, "/n/miss");
	CHECK(dc_lookup(dc, P("/n/miss"), &got) == DC_NEGATIVE, "neg: still neg");

	/* NON-VACUITY: a positive dentry must NOT read as negative, or the
	 * assertion above would pass on an engine that reports everything
	 * negative. */
	CHECK(dc_add_file(dc, P("/n/real"), 701) == 0, "neg: add file");
	expect_positive(dc, "/n/real", 701);

	/* duplicate create fails, and instantiate on a positive is -EEXIST */
	CHECK(dc_add_negative(dc, P("/n/miss")) == -EEXIST, "neg: dup negative");
	CHECK(dc_instantiate(dc, P("/n/real"), 999) == -EEXIST,
	      "neg: instantiate an already-positive dentry");
	CHECK(dc_instantiate(dc, P("/n/nothere"), 1) == -ENOENT,
	      "neg: instantiate an absent name");

	/* d_instantiate: same dentry, now positive, carrying its new id */
	CHECK(dc_instantiate(dc, P("/n/miss"), 702) == 0, "neg: instantiate");
	expect_positive(dc, "/n/miss", 702);

	/* unlink still REMOVES (phase 2 deliberately does not leave a negative
	 * behind -- see dcache.h), so the name goes fully absent */
	CHECK(dc_unlink(dc, P("/n/miss")) == 0, "neg: unlink");
	expect_absent(dc, "/n/miss");

	/* THE HOST-AUTHORITY CASE: rename a negative, THEN instantiate it.  The
	 * rename stacks a shell; the state must travel with the content host, so
	 * the renamed name is still negative and instantiating it still works. */
	CHECK(dc_add_negative(dc, P("/n/old")) == 0, "neg: add_negative /n/old");
	expect_negative(dc, "/n/old");
	CHECK(dc_rename(dc, P("/n/old"), P("/n/new")) == 0, "neg: rename");
	expect_absent(dc, "/n/old");
	expect_negative(dc, "/n/new");		/* pos/neg followed the host */

	/*
	 * DRAIN THE FOLD before re-checking, or this case does not test what it
	 * claims to.  Immediately after the rename the chain is shell(top) ->
	 * host, and the reader takes pos/neg off the HOST -- so it reads
	 * correctly no matter what the fold's TRANSFER does with the bit.  Only
	 * once the chain collapses does the TRANSFER's choice become the state
	 * the reader sees.  Verified by mutation: without this drain, reverting
	 * TRANSFER to adopt-from-top (the pre-phase-2 behaviour) still passes.
	 */
	dc_quiescent();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();
	expect_negative(dc, "/n/new");		/* survived the fold's TRANSFER */

	CHECK(dc_instantiate(dc, P("/n/new"), 703) == 0, "neg: instantiate renamed");
	expect_positive(dc, "/n/new", 703);

	/* and the converse: a POSITIVE dentry renamed stays positive with its id */
	CHECK(dc_rename(dc, P("/n/real"), P("/n/moved")) == 0, "neg: rename pos");
	expect_positive(dc, "/n/moved", 701);

	dc_destroy(dc);
	printf("  ok: negative dentries + d_instantiate\n");
}

/*
 * Phase 2 remainder: d_delete leaves a NEGATIVE behind instead of unhashing.
 * The state change now runs on a live, already-indexed node -- the shape the
 * txn engines' write-once-identity assumption forbids -- in BOTH directions.
 */
static void test_delete_to_negative(void)
{
	struct dcache *dc = dc_create(1024);
	uint64_t got = ~0ULL;

	CHECK(dc_add(dc, P("/d"), 800) == 0, "del: add dir /d");
	CHECK(dc_add_file(dc, P("/d/f"), 801) == 0, "del: add file /d/f");
	expect_positive(dc, "/d/f", 801);

	/* d_delete: the NAME survives, the object does not */
	CHECK(dc_delete(dc, P("/d/f")) == 0, "del: delete /d/f");
	expect_negative(dc, "/d/f");
	CHECK(dc_lookup(dc, P("/d/f"), &got) == DC_NEGATIVE, "del: still negative");

	/* NON-VACUITY: it must be distinguishable from BOTH neighbours -- an
	 * engine that reported everything negative, or that had actually
	 * unhashed the dentry, would pass one of these and fail the other. */
	CHECK(dc_lookup(dc, P("/d"), &got) == DC_POSITIVE && got == 800,
	      "del: sibling dir still positive");
	expect_absent(dc, "/d/neverwas");

	/* idempotence and the error surface */
	CHECK(dc_delete(dc, P("/d/f")) == -ENOENT, "del: delete an already-negative");
	CHECK(dc_delete(dc, P("/d/nothere")) == -ENOENT, "del: delete an absent name");
	CHECK(dc_delete(dc, P("/")) == -EISDIR, "del: delete the root -EISDIR");

	/*
	 * rmdir-to-negative is an ENGINE CAPABILITY, so assert the REAL behaviour
	 * on both sides rather than accepting either -- an "either is fine" test
	 * would pass on an engine that silently did nothing.
	 */
	CHECK(dc_add(dc, P("/d/sub"), 810) == 0, "del: add dir /d/sub");
	if (dc_delete_dir_supported) {
		CHECK(dc_add_file(dc, P("/d/sub/kid"), 811) == 0, "del: add kid");
		CHECK(dc_delete(dc, P("/d/sub")) == -ENOTEMPTY,
		      "del: rmdir a NON-empty directory -ENOTEMPTY");
		CHECK(dc_unlink(dc, P("/d/sub/kid")) == 0, "del: drop the kid");

		CHECK(dc_delete(dc, P("/d/sub")) == 0, "del: rmdir-to-negative");
		expect_negative(dc, "/d/sub");

		/* THE INVARIANT: a negative directory must not gain a child. */
		CHECK(dc_add_file(dc, P("/d/sub/kid"), 812) == -ENOENT,
		      "del: no child under a negative DIRECTORY");
		expect_absent(dc, "/d/sub/kid");

		/* and it comes back, still a directory, when re-instantiated */
		CHECK(dc_instantiate(dc, P("/d/sub"), 813) == 0,
		      "del: re-instantiate the directory");
		expect_positive(dc, "/d/sub", 813);
		CHECK(dc_add_file(dc, P("/d/sub/kid"), 814) == 0,
		      "del: a positive directory takes children again");
		expect_positive(dc, "/d/sub/kid", 814);
		CHECK(dc_unlink(dc, P("/d/sub/kid")) == 0, "del: tidy");
	} else {
		CHECK(dc_delete(dc, P("/d/sub")) == -ENOTSUP,
		      "del: engine declines rmdir-to-negative");
		expect_positive(dc, "/d/sub", 810);	/* and did NOT do it */
	}
	CHECK(dc_unlink(dc, P("/d/sub")) == 0, "del: unlink /d/sub");

	/* THE INVARIANT the files-only restriction buys: a negative cannot gain
	 * a child, so a walk through one finds nothing.  If dc_delete ever
	 * accepts directories this is the assertion that must be revisited. */
	CHECK(dc_add_file(dc, P("/d/f/under"), 802) == -ENOTDIR,
	      "del: no child under a negative");
	expect_absent(dc, "/d/f/under");

	/* round trip: the dentry kept its address and its place, so
	 * instantiating the same name brings it back -- with the NEW id, which
	 * is what proves the id was republished and not merely never lost */
	CHECK(dc_instantiate(dc, P("/d/f"), 803) == 0, "del: re-instantiate");
	expect_positive(dc, "/d/f", 803);
	CHECK(dc_delete(dc, P("/d/f")) == 0, "del: delete again");
	expect_negative(dc, "/d/f");

	/*
	 * DELETE A RENAMED FILE.  The rename stacks a shell, so the top and the
	 * content host are different nodes; dc_delete must land on the HOST.
	 * As in the instantiate case this only tests the TRANSFER once the fold
	 * has collapsed the chain -- before that the reader consults the host
	 * directly and would be right either way.
	 */
	CHECK(dc_add_file(dc, P("/d/g"), 804) == 0, "del: add file /d/g");
	CHECK(dc_rename(dc, P("/d/g"), P("/d/h")) == 0, "del: rename it");
	CHECK(dc_delete(dc, P("/d/h")) == 0, "del: delete the renamed file");
	expect_negative(dc, "/d/h");
	dc_quiescent();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();
	expect_negative(dc, "/d/h");	/* survived the fold's TRANSFER */

	/* and the reverse order: delete first, THEN rename the negative */
	CHECK(dc_add_file(dc, P("/d/i"), 805) == 0, "del: add file /d/i");
	CHECK(dc_delete(dc, P("/d/i")) == 0, "del: delete /d/i");
	CHECK(dc_rename(dc, P("/d/i"), P("/d/j")) == 0, "del: rename the negative");
	expect_absent(dc, "/d/i");
	expect_negative(dc, "/d/j");
	dc_quiescent();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();
	expect_negative(dc, "/d/j");	/* survived the fold's TRANSFER */

	/* a negative still unlinks -- the name goes fully absent */
	CHECK(dc_unlink(dc, P("/d/j")) == 0, "del: unlink a negative");
	expect_absent(dc, "/d/j");

	dc_destroy(dc);
	printf("  ok: d_delete -> negative (phase 2 remainder)\n");
}

int main(void)
{
	struct dcache *dc;
	int r;

	rcu_register_thread();
	dc = dc_create(1024);
	printf("== test_dcache (engine: %s) ==\n", dc_engine_name());

	test_midtransition();		/* mid-transition unlink + exchange edges */
	test_negative_dentries();	/* phase 2: negative -> instantiate */
	test_delete_to_negative();	/* phase 2: positive -> negative in place */

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
