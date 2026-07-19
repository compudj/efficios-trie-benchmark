// SPDX-License-Identifier: MIT
/*
 * test_skip_invariant -- white-box check of the DC_IPARENT_SKIP host->top skip.
 *
 * The census assertion in walk_rec() runs at QUIESCENCE, where every fold has
 * settled and host == top, so it can only ever observe "no skip present" -- it
 * cannot tell a working set-path from a dead one.  This test drives the engine
 * single-threadedly and inspects the host MID-FLIGHT, between the rename commit
 * and the fold that collapses the chain, which is the only window in which the
 * skip exists.
 *
 * Single-threaded by design: the invariant is racy to assert under concurrent
 * writers (a competing rename can retarget the skip between the read and the
 * check), so concurrency validation belongs to step 3, where the reader depends
 * on the value and the existing stress harnesses exercise it for real.
 *
 * Includes the engine source directly to reach its statics.
 */
#include "dcache_txn.c"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...) do {						\
	if (!(cond)) {							\
		printf("FAIL %s:%d: ", __func__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
		failures++;						\
	}								\
} while (0)

static void mkpath(struct dc_path *p, const char *const *comp, uint32_t n)
{
	uint32_t i;

	dc_path_reset(p);
	for (i = 0; i < n; i++)
		if (dc_path_push(p, comp[i]))
			abort();
}

/*
 * A rename must leave the content host naming the NEW top, tagged SKIP, and the
 * fold must put plain lineage back once the chain collapses.
 */
static void test_skip_set_and_cleared(void)
{
	static const char *const a[] = { "A" };
	static const char *const ab[] = { "A", "B" };
	static const char *const ac[] = { "A", "C" };
	struct dc_path pa, pab, pac;
	struct dentry *host, *top;
	struct dcache *dc;
	uintptr_t raw;

	dc = dc_create(1024);
	if (!dc)
		abort();
	mkpath(&pa, a, 1);
	mkpath(&pab, ab, 2);
	mkpath(&pac, ac, 2);
	CHECK(!dc_add(dc, &pa, 1), "add /A");
	CHECK(!dc_add(dc, &pab, 2), "add /A/B");

	/* settled: the entry is its own top, so plain lineage, no skip */
	rcu_read_lock();
	top = find_top_raw_rcu(dc, resolve(dc, &pab, 1), &pab.comp[1], &raw);
	CHECK(top != NULL, "found /A/B");
	host = host_of_raw(top, raw);
	CHECK(host == top, "settled entry is its own top");
	CHECK((iparent_raw(host) & DC_TAG_SKIP) == 0,
	      "settled host carries no SKIP (raw=%#lx)",
	      (unsigned long) iparent_raw(host));
	rcu_read_unlock();

	/*
	 * Rename WITHOUT draining: the fold is call_rcu-deferred, so the shell is
	 * still stacked here and the host must name it.
	 */
	CHECK(!dc_rename(dc, &pab, &pac), "rename /A/B -> /A/C");

	rcu_read_lock();
	top = find_top_raw_rcu(dc, resolve(dc, &pac, 1), &pac.comp[1], &raw);
	CHECK(top != NULL, "found /A/C after rename");
	host = host_of_raw(top, raw);
	CHECK(host != top, "mid-flight entry is shelled (host != top)");
	CHECK(iparent_raw(host) == ((uintptr_t) top | DC_TAG_SKIP),
	      "host names the new top with SKIP: raw=%#lx want=%#lx",
	      (unsigned long) iparent_raw(host),
	      (unsigned long) ((uintptr_t) top | DC_TAG_SKIP));
	rcu_read_unlock();

	/* drain: the fold collapses the chain and lineage must come back */
	rcu_quiescent_state();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();

	rcu_read_lock();
	top = find_top_raw_rcu(dc, resolve(dc, &pac, 1), &pac.comp[1], &raw);
	CHECK(top != NULL, "found /A/C after fold");
	host = host_of_raw(top, raw);
	CHECK(host == top, "folded entry is settled again");
	CHECK((iparent_raw(host) & DC_TAG_SKIP) == 0,
	      "settled host carries no SKIP after fold (raw=%#lx)",
	      (unsigned long) iparent_raw(host));
	CHECK(iparent_of(host) == resolve(dc, &pac, 1),
	      "lineage restored to the real parent");
	rcu_read_unlock();

	dc_destroy(dc);
}

/* Two stacked renames with no drain between: the skip must name the NEWEST top. */
static void test_skip_follows_newest_top(void)
{
	static const char *const a[] = { "A" };
	static const char *const ab[] = { "A", "B" };
	static const char *const ac[] = { "A", "C" };
	static const char *const ad[] = { "A", "D" };
	struct dc_path pa, pab, pac, pad;
	struct dentry *host, *top, *host2;
	struct dcache *dc;
	uintptr_t raw;

	dc = dc_create(1024);
	if (!dc)
		abort();
	mkpath(&pa, a, 1);
	mkpath(&pab, ab, 2);
	mkpath(&pac, ac, 2);
	mkpath(&pad, ad, 2);
	CHECK(!dc_add(dc, &pa, 1), "add /A");
	CHECK(!dc_add(dc, &pab, 2), "add /A/B");

	CHECK(!dc_rename(dc, &pab, &pac), "rename B -> C");
	rcu_read_lock();
	top = find_top_raw_rcu(dc, resolve(dc, &pac, 1), &pac.comp[1], &raw);
	host = host_of_raw(top, raw);
	rcu_read_unlock();

	CHECK(!dc_rename(dc, &pac, &pad), "rename C -> D (still unfolded)");
	rcu_read_lock();
	top = find_top_raw_rcu(dc, resolve(dc, &pad, 1), &pad.comp[1], &raw);
	CHECK(top != NULL, "found /A/D");
	host2 = host_of_raw(top, raw);
	CHECK(host2 == host, "content host is address-stable across renames");
	CHECK(iparent_raw(host) == ((uintptr_t) top | DC_TAG_SKIP),
	      "skip names the NEWEST top: raw=%#lx want=%#lx",
	      (unsigned long) iparent_raw(host),
	      (unsigned long) ((uintptr_t) top | DC_TAG_SKIP));
	rcu_read_unlock();

	/*
	 * Drain a two-shell chain (host -> s1 -> s2).  Folding the upper shell
	 * has m = s1 != host, which is the ONLY path that exercises the fold's
	 * explicit skip retarget; folding a single-shell chain has m == host and
	 * the promote store clears the skip implicitly.
	 */
	rcu_quiescent_state();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();

	rcu_read_lock();
	top = find_top_raw_rcu(dc, resolve(dc, &pad, 1), &pad.comp[1], &raw);
	CHECK(top != NULL, "found /A/D after fold");
	host2 = host_of_raw(top, raw);
	CHECK(host2 == host, "still the same content host");
	CHECK(host2 == top, "two-shell chain fully collapsed");
	CHECK((iparent_raw(host) & DC_TAG_SKIP) == 0,
	      "no SKIP left after the cascade (raw=%#lx)",
	      (unsigned long) iparent_raw(host));
	CHECK(iparent_of(host) == resolve(dc, &pad, 1),
	      "lineage restored to the real parent");
	rcu_read_unlock();

	dc_destroy(dc);
}

int main(void)
{
	rcu_register_thread();
	printf("== test_skip_invariant (engine: %s) ==\n", dc_engine_name());

	test_skip_set_and_cleared();
	test_skip_follows_newest_top();

	printf("failures: %d\n", failures);
	printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
	rcu_unregister_thread();
	return failures ? 1 : 0;
}
