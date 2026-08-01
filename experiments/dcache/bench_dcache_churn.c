// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * bench_dcache_churn -- insert/remove throughput for the userspace dcache.
 *
 * The other benchmarks hold the namespace FIXED and move names around it
 * (bench_dcache: rename + exchange; bench_dcache_height: exchange at height).
 * Nothing measured dc_add / dc_unlink, which are the create/delete path a real
 * dcache spends much of its life on.  This does, and it is a SEPARATE harness
 * because the conservation invariant has to be built for a namespace that is
 * moving rather than merely permuted -- bench_dcache's census asserts every
 * leaf is present at the end, which is exactly what churn violates.
 *
 * WHAT THE ARMS SHOULD DO, so the numbers can be read as confirmation or not:
 *
 *   dc_add   touches NO walk-causality version in any arm, and commits TWICE
 *            (urcu_txn_hlist_add_rcu for the name hash, then children_add for
 *            the parent's child list).  Expect insert to be arm-INDEPENDENT.
 *            Note the two commits also mean an insert is not atomic across the
 *            two indexes: a dentry is briefly in the name hash and not yet in
 *            its parent's child list.
 *   dc_unlink bumps the version inside its single commit.  So it is where the
 *            arms diverge: the GLOBAL arm bumps dc->rename_gen (a whole-tree
 *            cacheline every reader brackets on), DC_PER_NODE_GEN bumps the
 *            removed entry's host->d_seq, and DC_MARK_GEN bumps nothing at all
 *            because the hlist del already marks.
 *
 * MODEL.  W writers each own a disjoint slot range in a shared set of dirs, so
 * writers never collide with each other and every op must succeed -- an error
 * is a bug, not contention.  A writer repeatedly picks one of its own slots and
 * TOGGLES it: present -> dc_unlink, absent -> dc_add (same path, same id).  It
 * knows its own slots' states exactly, since it is their only mutator.
 *
 * INVARIANT (designed for churn, not retrofitted).  At the end, after joining:
 *   1. every slot a writer believes PRESENT resolves, with its id;
 *   2. every slot a writer believes ABSENT does not resolve;
 *   3. a dc_walk census enumerates EXACTLY the union of the believed-present
 *      sets -- no strays, no duplicates, nothing leaked by a half-done op.
 * (3) is the one that catches a torn insert: dc_add's two commits mean a
 * dentry can exist in one index and not the other, and the census walks the
 * CHILD index while the checks in (1)/(2) go through the name hash, so a
 * discrepancy between them shows up here.
 *
 * Readers do full-path lookups against the churning namespace and must tolerate
 * ABSENT -- a slot legitimately blinks out.  What they must never see is a
 * POSITIVE with an id outside the owning writer's range, or a torn/garbage id.
 * In address-identity builds dc_lookup returns the host address, which a
 * re-added dentry legitimately changes, so the per-lookup id check only runs in
 * -DDC_SPLIT_KEEPID builds; the end-of-run census uses dc_walk, which reads the
 * real cold d_id, and is therefore exact in BOTH builds.
 *
 * Usage: bench_dcache_churn [--readers R] [--writers W] [--ndirs N]
 *                           [--slots S] [--duration MS] [--cpulist c0,c1,...]
 *                           [--prefix-depth D]
 *   --slots S   slots owned per writer (the churn working set)
 * Output mirrors bench_dcache: "Mlookups/s:" for the reader rate and
 * "Mchurn/s:" for the writer op rate, with a "conservation: OK" gate line.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <urcu-qsbr.h>

#include "dcache.h"

extern const int dc_lookup_id_is_address __attribute__((weak));
static inline int id_is_address(void)
{
	return &dc_lookup_id_is_address && dc_lookup_id_is_address;
}

/* ---- knobs --------------------------------------------------------------- */

static int nreaders = 32;
static int readdir_mode = 0;		/* --readdir: readers list a dir vs look up */
/*
 * --readdir-names: pass a real callback to dc_readdir instead of NULL.
 *
 * dc_readdir(fn == NULL) only COUNTS -- it never materializes a name -- so the
 * default readdir mode never exercises the per-dirent qstr handoff.  That makes
 * it useless for any measurement whose subject IS the name: DC_NAME_MAX is paid
 * once per DIRENT here (not once per path component as in a lookup), and with a
 * NULL callback that cost is structurally absent, so a name-width A/B over the
 * default mode is vacuous and will report "no effect" no matter the width.
 *
 * Kept opt-in rather than made the default so the already-published readdir
 * figures stay comparable; note those are therefore COUNT-ONLY and understate a
 * real getdents(), which does copy every name out.
 */
static int readdir_names = 0;
static int nwriters = 8;
static int ndirs = 16;
static int slots = 32;			/* slots owned per writer */
static long duration_ms = 1000;
static int prefix_depth = 2;
static unsigned int nbuckets = 1u << 16;
static int *cpulist, cpulist_len;

#define DIR_ID_BASE 1000000ULL		/* ids >= this are dirs, not slots */

static struct dcache *g_dc;
static char g_prefix[8][DC_NAME_MAX];
static int g_prefix_len;

/* ---- plumbing ------------------------------------------------------------ */

#define GOFLAG_INIT 0
#define GOFLAG_RUN  1
#define GOFLAG_STOP 2
static volatile int goflag = GOFLAG_INIT;
static int nthreads_running;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void pin_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void) sched_setaffinity(0, sizeof(set), &set);
}

static void parse_cpulist(const char *s)
{
	int cap = 16;

	cpulist = malloc(cap * sizeof(*cpulist));
	cpulist_len = 0;
	while (*s) {
		char *end;
		long v = strtol(s, &end, 10);

		if (end == s)
			break;
		if (cpulist_len == cap) {
			cap *= 2;
			cpulist = realloc(cpulist, cap * sizeof(*cpulist));
		}
		cpulist[cpulist_len++] = (int) v;
		s = end;
		while (*s == ',' || *s == ' ')
			s++;
	}
}

static int thread_cpu(int idx)
{
	return cpulist_len ? cpulist[idx % cpulist_len] : -1;
}

static void pin_thread(int idx)
{
	int c = thread_cpu(idx);

	if (c >= 0)
		pin_cpu(c);
}

static uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return *s = x;
}

/* /p0/../d{dir}/S{gid} */
static void mk_slot_path(struct dc_path *p, int dir, int gid)
{
	char buf[DC_NAME_MAX];
	int i;

	dc_path_reset(p);
	for (i = 0; i < g_prefix_len; i++)
		dc_path_push(p, g_prefix[i]);
	snprintf(buf, sizeof(buf), "d%d", dir);
	dc_path_push(p, buf);
	snprintf(buf, sizeof(buf), "S%d", gid);
	dc_path_push(p, buf);
}

/* Path to a churned directory /pfx/d{dir} (its children are the slots writers
 * add/unlink), for a readdir reader. */
static void mk_dir_path(struct dc_path *p, int dir)
{
	char buf[DC_NAME_MAX];
	int i;

	dc_path_reset(p);
	for (i = 0; i < g_prefix_len; i++)
		dc_path_push(p, g_prefix[i]);
	snprintf(buf, sizeof(buf), "d%d", dir);
	dc_path_push(p, buf);
}

/* ---- workers ------------------------------------------------------------- */

struct warg {
	int idx, base;			/* thread index; first slot id owned */
	uint64_t seed;
	long long nadds, nunlinks, errs;
	long long nlookups, lk_wrong;
	long long ndirents;		/* children enumerated (--readdir mode) */
	unsigned long dsink;		/* --readdir-names: consumes the qstr so
					 * the per-dirent name copy cannot be
					 * optimized away */
	uint8_t *present;		/* owner-private truth for its slots */
	int *dir;			/* which dir each slot lives in */
};

static void wait_go(void)
{
	__atomic_fetch_add(&nthreads_running, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&goflag, __ATOMIC_ACQUIRE) == GOFLAG_INIT)
		sched_yield();
}

static void *writer_fn(void *arg)
{
	struct warg *me = arg;
	uint64_t s = me->seed;
	struct call_rcu_data *crdp = NULL;
	int cpu = thread_cpu(me->idx);

	dc_register_thread();
	pin_thread(me->idx);
	/*
	 * Per-writer RT call_rcu worker pinned to this writer's CPU, as
	 * bench_dcache.c does.  Every dc_unlink defers a dentry free, so without
	 * this ALL writers' reclaim funnels through liburcu's single default
	 * worker -- one unpinned thread free to float across both sockets.  That
	 * caps writer scaling at the reclaim thread's throughput rather than the
	 * engine's, and makes the numbers drift between runs as it migrates.
	 */
	if (cpu >= 0) {
		crdp = create_call_rcu_data(URCU_CALL_RCU_RT, cpu);
		if (crdp)
			set_thread_call_rcu_data(crdp);
	}
	wait_go();
	while (__atomic_load_n(&goflag, __ATOMIC_ACQUIRE) == GOFLAG_RUN) {
		int j = (int) (xrand(&s) % (uint64_t) slots);
		int gid = me->base + j;
		struct dc_path p;
		int ret;

		mk_slot_path(&p, me->dir[j], gid);
		if (me->present[j]) {
			ret = dc_unlink(g_dc, &p);
			if (ret == 0) { me->present[j] = 0; me->nunlinks++; }
			else me->errs++;
		} else {
			ret = dc_add(g_dc, &p, (uint64_t) gid);
			if (ret == 0) { me->present[j] = 1; me->nadds++; }
			else me->errs++;
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	if (crdp) {
		set_thread_call_rcu_data(NULL);
		call_rcu_data_free(crdp);	/* drains this worker's queue */
	}
	return NULL;
}

/*
 * --readdir-names sink: what a getdents() would do with each dirent, minus the
 * copy_to_user.  Touches the length, the hash AND two name bytes, so the qstr
 * the engine materializes per child is genuinely read rather than just handed
 * over -- reading only the header would leave the inline name untouched and the
 * width invisible again, which is the same vacuity one level down.
 */
static void dirent_sink(uint64_t id, const struct qstr *name, void *arg)
{
	unsigned long *acc = arg;

	*acc += (unsigned long) id + name->len + name->hash
	      + (unsigned long) name->name[0]
	      + (unsigned long) name->name[name->len ? name->len - 1 : 0];
}

static void *reader_fn(void *arg)
{
	struct warg *me = arg;
	uint64_t s = me->seed;
	int total_slots = nwriters * slots;

	dc_register_thread();
	pin_thread(me->idx);
	wait_go();
	while (__atomic_load_n(&goflag, __ATOMIC_ACQUIRE) == GOFLAG_RUN) {
		int dr = (int) (xrand(&s) % (uint64_t) ndirs);
		struct dc_path p;

		if (readdir_mode) {
			/* READDIR a churned dir /pfx/d{dr}: enumerate its
			 * currently-present slots while writers add/unlink into
			 * the SAME child list.  POSIX-soft (a slot mid-churn may
			 * or may not appear) but must never tear or crash. */
			long n;

			mk_dir_path(&p, dr);
			n = readdir_names
			  ? dc_readdir(g_dc, &p, dirent_sink, &me->dsink)
			  : dc_readdir(g_dc, &p, NULL, NULL);
			if (n < 0)
				me->errs++;
			else {
				me->nlookups++;
				me->ndirents += n;
			}
		} else {
			int gid = (int) (xrand(&s) % (uint64_t) total_slots);
			uint64_t id = ~0ULL;

			mk_slot_path(&p, dr, gid);
			/* ABSENT is legitimate -- the slot may be mid-churn, or in
			 * a different dir than the one we guessed.  Only a POSITIVE
			 * with a bogus id is a bug (logical-id builds only). */
			if (dc_lookup(g_dc, &p, &id) == DC_POSITIVE &&
			    !id_is_address() && id != (uint64_t) gid)
				me->lk_wrong++;
			me->nlookups++;
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
}

/* ---- census: exactly the believed-present set, no more --------------------- */

struct census {
	uint8_t *seen;
	int total;
	long stray, dup;
};

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= DIR_ID_BASE)
		return;			/* a directory */
	if (id >= (uint64_t) c->total) {
		c->stray++;
		return;
	}
	if (c->seen[id]++)
		c->dup++;
}

/* ---- setup --------------------------------------------------------------- */

static void build_tree(void)
{
	struct dc_path p;
	uint64_t dir_id = DIR_ID_BASE;
	int i, d;

	for (i = 0; i < prefix_depth; i++) {
		snprintf(g_prefix[i], sizeof(g_prefix[i]), "p%d", i);
		dc_path_reset(&p);
		for (d = 0; d <= i; d++)
			dc_path_push(&p, g_prefix[d]);
		if (dc_add(g_dc, &p, dir_id++)) {
			fprintf(stderr, "setup: prefix add failed\n");
			exit(2);
		}
	}
	g_prefix_len = prefix_depth;
	for (d = 0; d < ndirs; d++) {
		char buf[DC_NAME_MAX];

		dc_path_reset(&p);
		for (i = 0; i < g_prefix_len; i++)
			dc_path_push(&p, g_prefix[i]);
		snprintf(buf, sizeof(buf), "d%d", d);
		dc_path_push(&p, buf);
		if (dc_add(g_dc, &p, dir_id++)) {
			fprintf(stderr, "setup: dir add failed\n");
			exit(2);
		}
	}
}

static void usage(const char *p)
{
	fprintf(stderr,
	    "usage: %s [--readers R] [--writers W] [--ndirs N] [--slots S]\n"
	    "          [--duration MS] [--prefix-depth D] [--nbuckets N]\n"
	    "          [--cpulist c0,c1,...]\n"
	    "  writers TOGGLE their own slots (present -> unlink, absent -> add);\n"
	    "  readers look up random slots and must tolerate ABSENT.\n", p);
	exit(2);
}

int main(int argc, char **argv)
{
	struct warg *wa, *ra;
	pthread_t *wt, *rt;
	struct census c;
	long long t0, t1;
	long long adds = 0, unl = 0, errs = 0, lk = 0, wrong = 0, dirents = 0;
	unsigned long dsink = 0;
	int i, j, anomaly = 0, total_slots;
	double secs;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--readers"))            nreaders = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--writers"))       nwriters = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ndirs"))         ndirs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--slots"))         slots = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--duration"))      duration_ms = atol(argv[++i]);
		else if (!strcmp(argv[i], "--prefix-depth"))  prefix_depth = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--nbuckets"))      nbuckets = (unsigned) atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cpulist"))       parse_cpulist(argv[++i]);
		else if (!strcmp(argv[i], "--readdir"))       readdir_mode = 1;
		else if (!strcmp(argv[i], "--readdir-names")) { readdir_mode = 1;
							       readdir_names = 1; }
		else usage(argv[0]);
	}
	if (nwriters < 1 || slots < 1 || ndirs < 1 || prefix_depth < 1 ||
	    prefix_depth > 8 || nreaders < 0)
		usage(argv[0]);
	total_slots = nwriters * slots;

	rcu_register_thread();
	g_dc = dc_create(nbuckets);
	if (!g_dc) {
		fprintf(stderr, "dc_create failed\n");
		return 2;
	}
	build_tree();
	printf("== bench_dcache_churn (engine: %s) ==\n", dc_engine_name());
	printf("readers=%d writers=%d ndirs=%d slots/writer=%d total_slots=%d "
	       "duration_ms=%ld\n", nreaders, nwriters, ndirs, slots,
	       total_slots, duration_ms);

	wa = calloc(nwriters, sizeof(*wa));
	wt = calloc(nwriters, sizeof(*wt));
	ra = calloc(nreaders ? nreaders : 1, sizeof(*ra));
	rt = calloc(nreaders ? nreaders : 1, sizeof(*rt));

	/* Seed HALF the slots present, so the toggle starts balanced rather
	 * than measuring a long all-insert or all-delete transient. */
	for (i = 0; i < nwriters; i++) {
		wa[i].idx = i;
		wa[i].base = i * slots;
		wa[i].seed = 0x9e3779b97f4a7c15ULL ^ ((uint64_t) (i + 1) * 0x100000001b3ULL);
		wa[i].present = calloc(slots, 1);
		wa[i].dir = calloc(slots, sizeof(int));
		for (j = 0; j < slots; j++) {
			struct dc_path p;

			wa[i].dir[j] = (wa[i].base + j) % ndirs;
			if ((j & 1) == 0)
				continue;		/* leave absent */
			mk_slot_path(&p, wa[i].dir[j], wa[i].base + j);
			if (dc_add(g_dc, &p, (uint64_t) (wa[i].base + j))) {
				fprintf(stderr, "setup: slot seed failed\n");
				return 2;
			}
			wa[i].present[j] = 1;
		}
	}
	for (i = 0; i < nreaders; i++) {
		ra[i].idx = nwriters + i;
		ra[i].seed = 0xdeadbeefULL ^ ((uint64_t) (i + 1) * 0x2545F4914F6CDD1DULL);
	}

	for (i = 0; i < nwriters; i++)
		pthread_create(&wt[i], NULL, writer_fn, &wa[i]);
	for (i = 0; i < nreaders; i++)
		pthread_create(&rt[i], NULL, reader_fn, &ra[i]);
	while (__atomic_load_n(&nthreads_running, __ATOMIC_SEQ_CST) <
	       nwriters + nreaders)
		sched_yield();

	/*
	 * Main is RCU-registered and never quiesces, so it must be OFFLINE for
	 * the whole time the workers run -- not merely across the join.
	 *
	 * Offlining only around the join (as this did) leaves main online and
	 * asleep in nanosleep() for the entire MEASURED window, and one stuck
	 * online QSBR thread blocks every grace period: no call_rcu callback
	 * fires, so no freed descriptor returns to a slab freelist, so every
	 * allocation carves a fresh block.  The whole run's frees then land at
	 * teardown, when this thread finally goes offline.
	 *
	 * That is not a small distortion.  It made the descriptor slab's reuse
	 * rate read ~11% and its footprint grow LINEARLY with duration (3.8 GiB
	 * at 0.5 s to 48.7 GiB at 8 s) -- an unbounded-growth signature that is
	 * an artifact of this line's placement, not a property of the slab.
	 */
	rcu_thread_offline();

	t0 = now_ns();
	__atomic_store_n(&goflag, GOFLAG_RUN, __ATOMIC_RELEASE);
	{
		struct timespec ts = { duration_ms / 1000,
				       (duration_ms % 1000) * 1000000L };
		nanosleep(&ts, NULL);
	}
	__atomic_store_n(&goflag, GOFLAG_STOP, __ATOMIC_RELEASE);
	t1 = now_ns();

	for (i = 0; i < nwriters; i++)
		pthread_join(wt[i], NULL);
	for (i = 0; i < nreaders; i++)
		pthread_join(rt[i], NULL);
	rcu_thread_online();

	/* Let any deferred frees drain before the census. */
	rcu_quiescent_state();
	synchronize_rcu();
	rcu_barrier();
	synchronize_rcu();
	rcu_barrier();

	secs = (double) (t1 - t0) / 1e9;
	for (i = 0; i < nwriters; i++) {
		adds += wa[i].nadds; unl += wa[i].nunlinks; errs += wa[i].errs;
	}
	for (i = 0; i < nreaders; i++) {
		lk += ra[i].nlookups; wrong += ra[i].lk_wrong; dirents += ra[i].ndirents;
		dsink += ra[i].dsink;
	}

	/* --- invariant 1 & 2: believed state matches reality, by lookup ----- */
	{
		long mismatch_present = 0, mismatch_absent = 0;

		for (i = 0; i < nwriters; i++)
			for (j = 0; j < slots; j++) {
				struct dc_path p;
				uint64_t id = ~0ULL;
				enum dc_result r;

				mk_slot_path(&p, wa[i].dir[j], wa[i].base + j);
				r = dc_lookup(g_dc, &p, &id);
				if (wa[i].present[j]) {
					if (r != DC_POSITIVE ||
					    (!id_is_address() &&
					     id != (uint64_t) (wa[i].base + j)))
						mismatch_present++;
				} else if (r == DC_POSITIVE) {
					mismatch_absent++;
				}
			}
		if (mismatch_present || mismatch_absent) {
			printf("STATE MISMATCH: %ld present-but-missing, "
			       "%ld absent-but-found\n",
			       mismatch_present, mismatch_absent);
			anomaly++;
		}
	}

	/* --- invariant 3: the census sees EXACTLY the believed-present set --- */
	c.total = total_slots;
	c.seen = calloc(total_slots, 1);
	c.stray = c.dup = 0;
	dc_walk(g_dc, census_cb, &c);
	{
		long missing = 0, extra = 0;

		for (i = 0; i < nwriters; i++)
			for (j = 0; j < slots; j++) {
				int gid = wa[i].base + j;

				if (wa[i].present[j] && !c.seen[gid]) missing++;
				if (!wa[i].present[j] && c.seen[gid])  extra++;
			}
		if (missing || extra || c.stray || c.dup) {
			printf("CENSUS MISMATCH: %ld missing, %ld extra, "
			       "%ld stray, %ld dup\n",
			       missing, extra, c.stray, c.dup);
			anomaly++;
		}
	}
	anomaly += (errs != 0) + (wrong != 0);

	printf("duration (s): %g\n", secs);
	printf("CHURN   adds: %lld  unlinks: %lld  errors: %lld\n",
	       adds, unl, errs);
	printf("Mchurn/s: %g\n", (double) (adds + unl) / secs / 1e6);
	printf("LOOKUP  lookups: %lld  wrong-id: %lld\n", lk, wrong);
	printf("Mlookups/s: %g\n", (double) lk / secs / 1e6);
	if (readdir_mode) {
		printf("READDIR dirents: %lld  Mdirents/s: %g  children/readdir~%g\n",
		       dirents, (double) dirents / secs / 1e6,
		       lk ? (double) dirents / (double) lk : 0.0);
		/* names=0 means dc_readdir only COUNTED: no qstr was built, so
		 * this run says nothing about name width.  Printed so a reader
		 * of the log can tell the two modes apart after the fact. */
		printf("READDIR names: %d  name-sink: %lu\n", readdir_names, dsink);
	}
	if (anomaly)
		printf("CONSERVATION FAILED: %d anomalies -- run is CORRUPT, "
		       "ignore the numbers above\n", anomaly);
	else
		printf("CHECK   conservation: OK (state, census and ids agree)\n");
	printf("RESULT: %s\n", anomaly ? "FAIL" : "PASS");

	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
