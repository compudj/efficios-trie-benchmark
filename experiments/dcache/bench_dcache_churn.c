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
 *                           [--evict continuous|bursty|off] [--evict-cap N]
 *                           [--evict-period MS] [--evict-batch N]
 *   --slots S   slots owned per writer (the churn working set)
 * Output mirrors bench_dcache: "Mlookups/s:" for the reader rate and
 * "Mchurn/s:" for the writer op rate, with a "conservation: OK" gate line.
 */

#define _GNU_SOURCE
#define _LGPL_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
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
/*
 * ---- PHASE 3: RECLAIM CADENCE (--evict) -----------------------------------
 *
 * design/dcache-lru-txn.md section 7 says which LRU mechanism wins -- per-shard
 * lock or MCAS -- is decided by CADENCE, not by readers, and section 8 asks for
 * exactly this arm.  So make the cadence an explicit knob rather than an
 * accident of when a shrinker happens to run:
 *
 *   off         no shrinking (the historical behaviour, and the control).
 *   continuous  a BOUNDED cache: every writer, after its own op, evicts down
 *               toward --evict-cap.  The consumer is always on and is spread
 *               across all the producers, which is the adversarial case for the
 *               lock design -- producer and consumer contend on the same shard
 *               lock continuously -- and the case MCAS exists to win.
 *   bursty      a single shrinker thread wakes every --evict-period-ms and
 *               shrinks back to the cap.  This is mainline's shape
 *               (pressure-driven, batch-isolate), where contention is a burst
 *               rather than a steady state and the lock should hold up.
 *
 * The LIVE-SET SIZE is reported alongside throughput because throughput alone
 * cannot distinguish "kept up" from "fell behind": a retention regression --
 * the tombstone hazard of section 6 -- shows up as a population that climbs
 * away from the cap while the op rate still looks fine.
 *
 * ⛔ --evict bursty ON THE MCAS ARM (-DDC_LRU_MCAS) CAN WEDGE THE PROCESS, and
 * the cause is NOT in this harness -- see dcache_lru.h and REVIEW.md.  Short
 * version: escalation parks a thread while it is still RCU-ONLINE, which stalls
 * every grace period, which stalls call_rcu descriptor reclaim, which deepens
 * the contention that caused the escalation.  Self-reinforcing and absorbing:
 * once entered it never recovers, and gdb shows the call_rcu worker still inside
 * synchronize_rcu() six seconds apart.
 *
 * Onset is STOCHASTIC -- the same command line completes on one run and wedges
 * on the next -- so a bursty MCAS sweep needs a watchdog, and a run that does
 * finish is not evidence the configuration is safe.  --evict bursty on the LOCK
 * arm and --evict continuous on either arm are unaffected.
 */
static long evict_cap = 0;		/* 0 = --evict off */
static int evict_continuous = 0;
static long evict_period_ms = 10;
static long evict_batch = 64;		/* bursty: max evictions per wakeup */
static unsigned long long g_evictions;	/* total, summed across evictors */
static unsigned long g_lru_peak;
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
/*
 * Recreate a writer's prefix directories after the shrinker evicted one.
 *
 * Only reachable with --evict on.  A directory is normally safe: every writer
 * op resolves THROUGH it, which is the retain_dentry touch, so it is marked
 * referenced and the CLOCK gives it a second chance instead of evicting it.
 * The leaves are never resolved through and so are never marked -- which is
 * exactly the shape we want, hot directories and cold leaves.  But a directory
 * that goes momentarily empty just as its bit is cleared can still be taken,
 * so the writer has to be able to put it back.
 *
 * Idempotent: -EEXIST means a peer rebuilt it first, which is success.
 */
static int rebuild_prefix(int dir)
{
	struct dc_path p;
	char buf[DC_NAME_MAX];
	int i, ret;

	/*
	 * ID MATTERS: the census classifies by id -- anything below DIR_ID_BASE
	 * is a slot, anything at or above it is a directory and is skipped.  An
	 * earlier draft rebuilt with id 0, which made every rebuilt directory
	 * masquerade as slot 0 and show up as a census DUPLICATE.  The exact
	 * value is irrelevant since directories are never counted; being in the
	 * right RANGE is not.
	 */
	dc_path_reset(&p);
	for (i = 0; i < g_prefix_len; i++) {
		dc_path_push(&p, g_prefix[i]);
		ret = dc_add(g_dc, &p, DIR_ID_BASE);
		if (ret != 0 && ret != -EEXIST)
			return ret;
	}
	snprintf(buf, sizeof(buf), "d%d", dir);
	dc_path_push(&p, buf);
	ret = dc_add(g_dc, &p, DIR_ID_BASE);
	return (ret == 0 || ret == -EEXIST) ? 0 : ret;
}

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
	long long nlost;	/* slots the shrinker took first */
	long long nrebuild;	/* prefix directories the shrinker took */
	long long nlookups, lk_wrong;
	long long ndirents;		/* children enumerated (--readdir mode) */
	unsigned long dsink;		/* --readdir-names: consumes the qstr so
					 * the per-dirent name copy cannot be
					 * optimized away */
	uint8_t *present;		/* owner-private truth for its slots */
	int *dir;			/* which dir each slot lives in */
	/*
	 * PER-SLOT PROVENANCE, so a census anomaly can name itself.  "N extra"
	 * -- the census sees a slot the owner believes gone -- is ambiguous
	 * between a real resurrect-after-delete and the owner having believed
	 * wrongly, and the two have opposite conclusions.  The owner clears
	 * present[] on -ENOENT from unlink, which under eviction can mean THE
	 * PREFIX WENT rather than the leaf being gone; recording the op and its
	 * return separates that from corruption at the point of failure instead
	 * of by argument afterwards.
	 */
	uint8_t *last_op;		/* 0 none, 1 add, 2 unlink, 3 add-retry */
	int *last_ret;
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
	/*
	 * DC_CRDP_CPU_OFFSET shifts the reclaim worker off the writer's own cpu.
	 * URCU_CALL_RCU_RT means the worker does not sleep, so co-pinning it with
	 * a writer that never yields makes the two timeshare one hardware thread
	 * and caps reclaim at roughly half -- which shows up not as latency but
	 * as an unbounded call_rcu backlog, since the writer keeps allocating at
	 * full rate while its own reclaim gets half a cpu.  0 = the historical
	 * co-pinned behaviour.
	 */
	if (cpu >= 0) {
		const char *e = getenv("DC_CRDP_CPU_OFFSET");
		int off = e ? atoi(e) : 0;
		long nconf = sysconf(_SC_NPROCESSORS_CONF);
		int ccpu = off ? (int) (((long) cpu + off) % (nconf > 0 ? nconf : 1))
			       : cpu;

		crdp = create_call_rcu_data(URCU_CALL_RCU_RT, ccpu);
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
			me->last_op[j] = 2; me->last_ret[j] = ret;
			if (ret == 0) { me->present[j] = 0; me->nunlinks++; }
			/*
			 * Under eviction -ENOENT is the EXPECTED answer, not an
			 * error: the shrinker reached this slot first.  Counting
			 * it as an error would make every eviction look like a
			 * bug, and hiding it would lose the signal -- so it gets
			 * its own counter and the writer's belief is corrected.
			 */
			else if (evict_cap && ret == -ENOENT) {
				me->present[j] = 0;
				me->nlost++;
			} else me->errs++;
		} else {
			ret = dc_add(g_dc, &p, (uint64_t) gid);
			me->last_op[j] = 1; me->last_ret[j] = ret;
			if (ret == 0) { me->present[j] = 1; me->nadds++; }
			/*
			 * -ENOENT on an ADD means the PREFIX went: a writer's
			 * directory became empty and unreferenced at the wrong
			 * moment and the shrinker took it.  Rebuild and retry
			 * once, and count it -- silently dropping these would
			 * quietly turn the benchmark into a no-op as the tree
			 * eroded, with the op rate still looking healthy.
			 */
			else if (evict_cap && ret == -ENOENT) {
				me->nrebuild++;
				me->last_op[j] = 3;
				if (rebuild_prefix(me->dir[j]) == 0 &&
				    (me->last_ret[j] =
				     dc_add(g_dc, &p, (uint64_t) gid)) == 0) {
					me->present[j] = 1;
					me->nadds++;
				}
			} else me->errs++;
		}
		if (evict_continuous && (long) dc_lru_count(g_dc) > evict_cap) {
			/* LOCAL: evict where we allocate.  A continuous evictor
			 * calling dc_shrink() would make every writer a consumer
			 * of every other writer's shard -- see dcache.h. */
			long n = dc_shrink_local(g_dc, 1);

			if (n > 0)
				__atomic_fetch_add(&g_evictions,
						   (unsigned long long) n,
						   __ATOMIC_RELAXED);
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

/*
 * The BURSTY evictor: one thread, waking every --evict-period-ms and shrinking
 * back toward the cap in a bounded batch.  Mainline's shape -- reclaim is
 * pressure-driven, and the shrinker batch-isolates under the shard lock and
 * processes outside it -- so contention with the producers is a burst rather
 * than a steady state, which is the regime the lock design is built for.
 *
 * Deliberately ONE thread, not one per writer: the point of the arm is a
 * consumer that is mostly idle, and spreading it would turn it back into the
 * continuous case.
 */
static void *shrinker_fn(void *arg)
{
	(void) arg;
	dc_register_thread();
	wait_go();
	while (__atomic_load_n(&goflag, __ATOMIC_ACQUIRE) == GOFLAG_RUN) {
		struct timespec ts = {
			.tv_sec  = evict_period_ms / 1000,
			.tv_nsec = (evict_period_ms % 1000) * 1000000L,
		};
		unsigned long pop;
		long over;

		rcu_thread_offline();		/* never park while online */
		nanosleep(&ts, NULL);
		rcu_thread_online();

		pop = dc_lru_count(g_dc);
		if (pop > (unsigned long) g_lru_peak)
			__atomic_store_n(&g_lru_peak, pop, __ATOMIC_RELAXED);
		over = (long) pop - evict_cap;
		if (over > 0) {
			long n = dc_shrink(g_dc, over > evict_batch
						 ? evict_batch : over);

			if (n > 0)
				__atomic_fetch_add(&g_evictions,
						   (unsigned long long) n,
						   __ATOMIC_RELAXED);
		}
		dc_quiescent();
	}
	dc_unregister_thread();
	return NULL;
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

/*
 * Dump the transaction counters from a SIGNAL and leave immediately.
 *
 * The configuration this instrumentation exists to explain does not finish, so
 * the end-of-run dump never happens for it.  SIGTERM/SIGALRM gives a snapshot of
 * a run that is CURRENTLY wedged, which is the only way to see the counters in
 * the state that matters.  _exit() rather than exit(): the process is wedged, so
 * anything that waits (atexit, rcu_barrier, joins) would wedge with it.
 */
static void stats_signal(int sig)
{
	(void) sig;
	if (dc_txn_stats_supported) {
		dc_txn_stats_dump(stdout);
		dc_txn_stats_last(stdout);
		dc_lru_validate(stdout);
	}
	fflush(stdout);
	_exit(3);
}

int main(int argc, char **argv)
{
	struct warg *wa, *ra;
	pthread_t *wt, *rt;
	struct census c;
	pthread_t shrinker_th;
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
		else if (!strcmp(argv[i], "--evict-cap"))     evict_cap = atol(argv[++i]);
		else if (!strcmp(argv[i], "--evict-period"))  evict_period_ms = atol(argv[++i]);
		else if (!strcmp(argv[i], "--evict-batch"))   evict_batch = atol(argv[++i]);
		else if (!strcmp(argv[i], "--evict")) {
			const char *m = argv[++i];

			if (!strcmp(m, "continuous"))      evict_continuous = 1;
			else if (!strcmp(m, "bursty"))     evict_continuous = 0;
			else if (!strcmp(m, "off"))      { evict_cap = 0; evict_continuous = 0; }
			else { fprintf(stderr, "--evict continuous|bursty|off\n"); return 2; }
		}
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
		wa[i].last_op = calloc(slots, 1);
		wa[i].last_ret = calloc(slots, sizeof(*wa[i].last_ret));
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
	signal(SIGTERM, stats_signal);
	signal(SIGALRM, stats_signal);
	for (i = 0; i < nreaders; i++)
		pthread_create(&rt[i], NULL, reader_fn, &ra[i]);
	if (evict_cap && !evict_continuous)
		pthread_create(&shrinker_th, NULL, shrinker_fn, NULL);
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

	if (evict_cap && !evict_continuous)
		pthread_join(shrinker_th, NULL);

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

	/*
	 * --- eviction reconciliation, BEFORE the invariants ---
	 *
	 * The shrinker destroys names the writers still believe present, so their
	 * present[] is stale by construction.  Rather than weaken invariant 3 to
	 * "missing is allowed" -- which would blind it to the corruption it exists
	 * to catch -- REFRESH the belief from the cache now that every thread has
	 * stopped and nothing more can be evicted, and then keep the exact match.
	 * A slot the writer thinks is there and the cache says is not was evicted;
	 * anything else is still a hard failure.
	 *
	 * It has to run before invariant 2 as well as 3 -- invariant 2 compares
	 * present[] against the cache by lookup, so a stale belief fails it
	 * first, and reconciling only for the census left "26 present-but-missing"
	 * reported as corruption.
	 */
	if (evict_cap) {
		long refreshed = 0;

		for (i = 0; i < nwriters; i++)
			for (j = 0; j < slots; j++) {
				struct dc_path p;

				if (!wa[i].present[j])
					continue;
				mk_slot_path(&p, wa[i].dir[j], wa[i].base + j);
				if (dc_lookup(g_dc, &p, NULL) != DC_POSITIVE) {
					wa[i].present[j] = 0;
					refreshed++;
				}
			}
		printf("evicted-behind-writer: %ld\n", refreshed);
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
			/*
			 * NAME THE OFFENDERS.  A bare count cannot distinguish
			 * a resurrect-after-delete from an owner that believed
			 * wrongly, and those have opposite conclusions.  Print
			 * the slot, what the two indexes say about it now, and
			 * the last op the owner ran on it with its return --
			 * an "extra" whose last op was unlink -> -ENOENT under
			 * eviction is the owner mis-believing (the prefix went,
			 * not the leaf), whereas one whose last op was a
			 * SUCCESSFUL unlink is real corruption.
			 */
			static const char *opn[] = { "none", "add", "unlink",
						     "add-retry" };
			long shown = 0;

			for (i = 0; i < nwriters && shown < 8; i++)
				for (j = 0; j < slots && shown < 8; j++) {
					int gid = wa[i].base + j;
					struct dc_path p;
					uint64_t id = ~0ULL;
					enum dc_result r;

					if (wa[i].present[j] || !c.seen[gid])
						continue;
					mk_slot_path(&p, wa[i].dir[j], gid);
					r = dc_lookup(g_dc, &p, &id);
					printf("  EXTRA gid=%d dir=%d "
					       "lookup=%d id=%llu last-op=%s "
					       "last-ret=%d\n",
					       gid, wa[i].dir[j], (int) r,
					       (unsigned long long) id,
					       opn[wa[i].last_op[j]],
					       wa[i].last_ret[j]);
					shown++;
				}
			anomaly++;
		}
	}
	anomaly += (errs != 0) + (wrong != 0);

	printf("duration (s): %g\n", secs);
	printf("CHURN   adds: %lld  unlinks: %lld  errors: %lld\n",
	       adds, unl, errs);
	printf("Mchurn/s: %g\n", (double) (adds + unl) / secs / 1e6);
	if (evict_cap) {
		long long lost = 0, rebuilt = 0;
		unsigned long pop = dc_lru_count(g_dc);

		for (i = 0; i < nwriters; i++) {
			lost += wa[i].nlost;
			rebuilt += wa[i].nrebuild;
		}
		if (pop > g_lru_peak)
			g_lru_peak = pop;
		/*
		 * LIVE-SET SIZE alongside throughput, because throughput alone
		 * cannot tell "kept up" from "fell behind".  A retention
		 * regression -- section 6's tombstone hazard, where a node's
		 * call_rcu free waits on the shrinker instead of on the grace
		 * period -- shows up as a population climbing away from the cap
		 * while the op rate still looks perfectly healthy.  Compare
		 * lru-final and lru-peak against the cap, not just Mchurn/s.
		 */
		printf("EVICT   mode: %s  cap: %ld  evictions: %llu  "
		       "Mevict/s: %g\n",
		       evict_continuous ? "continuous" : "bursty", evict_cap,
		       g_evictions, (double) g_evictions / secs / 1e6);
		printf("EVICT   lru-final: %lu  lru-peak: %lu  over-cap: %+ld\n",
		       pop, g_lru_peak, (long) pop - evict_cap);
		printf("EVICT   lost-under-writer: %lld  prefix-rebuilds: %lld\n",
		       lost, rebuilt);
		printf("EVICT   arm: %s/%s\n", dc_lru_arm(),
		       dc_lru_supported ? "on" : "OFF");
	}
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
		if (dc_txn_stats_supported)
		dc_txn_stats_dump(stdout);
	/* ⚠ GATED.  This printed unconditionally, so a failing run reported
	 * "CONSERVATION FAILED" and "conservation: OK" three lines apart. */
	if (!anomaly)
		printf("CHECK   conservation: OK (state, census and ids agree)\n");
	printf("RESULT: %s\n", anomaly ? "FAIL" : "PASS");

	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
