// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * bench_list_scale -- read/write scaling benchmark for concurrent doubly-linked
 * lists, comparing the new userspace-rcu *bidirectional* RCU lists against the
 * state-of-the-art lock-based and seqlock alternatives.
 *
 * The new lists (from compudj/userspace-rcu-dev, branch rcu-bidir-list-dev):
 *   txn_sw_list  <urcu/rcu-txn-sw-list.h>          RCU readers, SINGLE updater
 *   txn_list  <urcu/rcu-txn-list.h> RCU readers, concurrent updaters
 * Both publish forward AND backward edges coherently, so a reader may walk the
 * ring in either direction (or reverse mid-walk) and never see next/prev disagree.
 *
 * The competitors wrap a plain doubly-linked list in a synchronization strategy:
 *   mutex     pthread_mutex                       (readers serialize)
 *   fairmutex liburcu cds_fair_mutex (MCS/FIFO)   (readers serialize, FIFO-fair)
 *   rwlock_r  pthread_rwlock, reader-preferring   (readers share, writer excl.)
 *   rwlock_w  pthread_rwlock, writer-preferring   (readers share, writer excl.)
 *   iscrw     bind9 isc_rwlock (C-RW-WP)          (readers share, phase-fair)
 *   seqlock   sequence lock + type-stable nodes   (readers optimistic, retry)
 *   rculist   classic <urcu/rculist.h>            (RCU readers, FORWARD-ONLY ref)
 *
 * Workload.  A sorted ring of LIST_SIZE permanent "stable" nodes (keys 2*i) is
 * always present; CHURN "churn" nodes (key 2*anchor+1) are toggled in/out just
 * after a spread-out, unique stable anchor -- so every insert/delete is O(1) and
 * the ring stays strictly sorted at all times.  Readers walk forward then
 * reverse, counting node visits (and asserting monotonicity -- a free coherence
 * check).  Writers toggle churn nodes.  Because the ring stays sorted at every
 * instant, a forward walk must see strictly increasing keys and a reverse walk
 * strictly decreasing -- any coherence defect shows up as a violation.
 *
 * Reclamation is each strategy's honest cost: the RCU engines defer node frees
 * through call_rcu() (a real RCU expense); the lock/seqlock engines recycle a
 * permanent per-slot node in place (safe under their exclusion / type stability).
 *
 * Sweeps (QSBR flavor, one process per engine for clean RSS / no cross-talk):
 *   default          read scaling: readers 1..191 + 1 writer
 *   BENCH_NO_WRITER  read-only ceiling: readers 1..192, no writer
 *   BENCH_WRITESCALE writer scaling: writers 1..192, readers=BENCH_READERS (def 0)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE
#endif

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <numa.h>

#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu-qsbr.h>
#include <urcu-call-rcu.h>
#include <urcu/rcu-txn-sw-list.h>
#include <urcu/rcu-txn-list.h>
#include <urcu/rcu-txn-hlist.h>
#include <urcu/rcu-txn.h>
#include <urcu/rculist.h>
#include <urcu/rculfhash.h>
#include <urcu/fair-mutex.h>

#include "rlu.h"		/* reference Read-Log-Update engine (third_party/rlu) */

#ifdef BENCH_JEMALLOC
/*
 * `make JEMALLOC=1` links libjemalloc and defines BENCH_JEMALLOC.  jemalloc
 * reads this application-defined `malloc_conf` symbol at startup, so the
 * per-CPU arena config is baked into the binary -- a reproducible pooled build
 * needing no MALLOC_CONF env.  The MALLOC_CONF env var still overrides it (e.g.
 * MALLOC_CONF=percpu_arena:phycpu).  Purpose: move the per-commit MCAS
 * descriptor (and churn-node) allocations off glibc's arena mprotect/mmap_lock,
 * which serialises writers process-wide -- ~2x writer throughput at scale.
 */
const char *malloc_conf = "percpu_arena:percpu";
#endif

#include "bench_topology.h"

/* ── ISC C-RW-WP rwlock, behind the bench_iscrw.c isolation wrapper ── */
extern size_t bench_iscrw_size(void);
extern size_t bench_iscrw_align(void);
extern void bench_iscrw_init(void *);
extern void bench_iscrw_rdlock(void *);
extern void bench_iscrw_rdunlock(void *);
extern void bench_iscrw_wrlock(void *);
extern void bench_iscrw_wrunlock(void *);

/*
 * Flight-recorder instrumentation (diagnostic only, -DBENCH_LTTNG): trace the
 * cell lifecycle (call_rcu -> free) and each writer's quiescent state, then dump
 * the LTTng snapshot from the ASan death callback so the window leading to the
 * use-after-free is captured.  Compiles out entirely without the flag.
 */
#ifdef BENCH_LTTNG
#include "bench_tp.h"
extern void __sanitizer_set_death_callback(void (*)(void));
static __thread int t_id = -1;		/* writer index (-1: not a writer) */
static __thread unsigned long t_epoch;	/* quiescent-state count for this thread */
#define TP_QS()       do { lttng_ust_tracepoint(bench, qs, t_id, t_epoch); t_epoch++; } while (0)
#define TP_CALLRCU(c) lttng_ust_tracepoint(bench, callrcu, (void *)(c), t_id, t_epoch)
#define TP_FREE(c)    lttng_ust_tracepoint(bench, freecell, (void *)(c))
#define TP_READSUCC(s) lttng_ust_tracepoint(bench, readsucc, (void *)(s), t_id, t_epoch)
#define TP_SET_WID(w) do { t_id = (w); } while (0)
static void bench_on_death(void) { (void) system("lttng snapshot record 1>&2"); }
#ifdef URCU_MCAS_STORE_TRACE
/*
 * Engine structural-store hook (diagnostic): the MCAS engine calls this
 * for every per-record install (phase 0), steal (1) and settle (2), so the
 * trace names which record's slot was -- or was not -- written by a commit.
 */
void (*urcu_mcas_store_hook)(const void *, void **, void *, void *,
		int, int, unsigned long);
static void bench_store_hook(const void *mcas, void **slot, void *oldv,
		void *newv, int phase, int ok, unsigned long status)
{
	lttng_ust_tracepoint(bench, store, (void *) mcas, (void *) slot,
			oldv, newv, phase, ok, status, t_id);
}
#endif
#else
#define TP_QS()
#define TP_CALLRCU(c)
#define TP_FREE(c)
#define TP_READSUCC(s)
#define TP_SET_WID(w)
#endif

/* ── Run configuration (env-overridable) ───────────────────────── */
static int LIST_SIZE = 1000;	/* stable nodes always present */
static int CHURN     = 200;	/* churn slots toggled by writers (<= LIST_SIZE) */
static int DURATION_SEC = 3;
static int STEP_LIMIT;		/* runaway-walk guard, set after sizes known */

/* Churn schedule, shared (only one engine builds per process). */
static int    *g_anchor;	/* [CHURN] stable index a churn node sits after */
static int8_t *g_present;	/* [CHURN] is this churn slot currently linked? */
/* BENCH_RANDOM_POS: multi-slot transacted-index writer mode.  Declared here (up
 * front) so the read passes can suppress the sortedness check in this mode --
 * random-mode inserts carry key 0, so the list is intentionally unsorted and a
 * reader only counts visits (measuring read cost under multi-slot writes). */
static int g_random_pos;

/* ── Run state ─────────────────────────────────────────────────── */
static volatile int start_flag;
static volatile int stop_flag;
static volatile int prime_done_count;

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ── Optional per-op write-latency histogram (BENCH_LATENCY=1) ─────────
 * Log-scale buckets: LAT_SUB sub-buckets per power-of-two ns.  Throughput hides
 * how long a *starved* op waits before it finally commits, so this exposes the
 * tail-latency cost of a larger optimistic-retry budget (URCU_TXN_FALLBACK): a
 * bigger budget trades more pre-escalation retries for less serialization. */
static int g_lat;			/* BENCH_LATENCY set */
#define LAT_SUB 4
#define LAT_NBUCKETS (48 * LAT_SUB)
static inline int lat_bucket(uint64_t ns)
{
	int oct, sub, idx;
	if (ns < (uint64_t) LAT_SUB)
		return (int) ns;
	oct = 63 - __builtin_clzll(ns);
	sub = (int) ((ns >> (oct - 2)) & (LAT_SUB - 1));
	idx = oct * LAT_SUB + sub;
	return idx < LAT_NBUCKETS ? idx : LAT_NBUCKETS - 1;
}
static inline uint64_t lat_value(int idx)		/* bucket lower edge, ns */
{
	int oct = idx / LAT_SUB, sub = idx % LAT_SUB;
	if (idx < LAT_SUB)
		return (uint64_t) idx;
	return ((uint64_t) (LAT_SUB + sub)) << (oct - 2);
}
static uint64_t lat_pct(const uint64_t *h, uint64_t total, double p)
{
	uint64_t want = (uint64_t) (p * (double) total), cum = 0;
	int i;
	for (i = 0; i < LAT_NBUCKETS; i++) {
		cum += h[i];
		if (cum >= want)
			return lat_value(i);
	}
	return lat_value(LAT_NBUCKETS - 1);
}

static long get_rss_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long rss = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (strncmp(line, "VmRSS:", 6) == 0) {
			sscanf(line + 6, " %ld", &rss);
			break;
		}
	fclose(f);
	return rss;
}

/* ════════════════════════════════════════════════════════════════
 * Reclaim / allocation domain granularity  (BENCH_RECLAIM_DOMAIN)
 *
 * Both the per-CPU call_rcu reclaim workers and the per-CPU node slab are, by
 * default, per hardware thread (one per PU) -- domain=hwthread.  This knob
 * coarsens BOTH to a shared topology domain: core, l3, or a single global one,
 * to study consolidating reclaim/allocation across HW threads that share a
 * cache.
 *
 * HONEST-ACCOUNTING INVARIANT.  The x-axis is "N writers == N PUs"; any reclaim
 * hardware OUTSIDE the writers' PU set is uncounted work that would inflate
 * throughput.  So each domain's call_rcu worker is pinned to the domain's
 * ANCHOR PU -- the lowest writer-index PU in the domain, which is an occupied
 * writer PU whenever the domain has any writer (writers fill one-per-core in
 * index order).  A worker thus only ever time-shares a real writer PU, never a
 * free sibling or idle core.  BENCH_RECLAIM_UNCONFINED lifts this (pins to the
 * whole domain cpumask) purely to MEASURE the inflation the freedom buys -- it
 * is not an honest number.  The worker affinity is set explicitly here, so the
 * workload's own hwloc pinning can never leak into or constrain the workers.
 * ════════════════════════════════════════════════════════════════ */
enum reclaim_domain_level { RDL_HWTHREAD, RDL_CORE, RDL_L3, RDL_SINGLE };
static enum reclaim_domain_level g_rdl = RDL_HWTHREAD;	/* call_rcu worker domain */
static enum reclaim_domain_level g_sdl = RDL_HWTHREAD;	/* node slab domain */
static int  g_rdl_unconfined;		/* BENCH_RECLAIM_UNCONFINED */
static int  g_rdl_ncpu;			/* sysconf(_SC_NPROCESSORS_CONF) */
static int *g_cpu_domain;		/* [ncpu] worker domain id 0..ndom-1 */
static int  g_ndomains;
static int *g_domain_anchor;		/* [ndom] anchor OS cpu (a writer PU) */
static int *g_slab_domain;		/* [ncpu] slab domain id (independent) */
static int  g_slab_ndomains;

static const char *rdl_name(enum reclaim_domain_level l)
{
	switch (l) {
	case RDL_CORE:		return "core";
	case RDL_L3:		return "l3";
	case RDL_SINGLE:	return "single";
	default:		return "hwthread";
	}
}

static enum reclaim_domain_level
rdl_parse(const char *env, enum reclaim_domain_level dflt)
{
	if (env == NULL)			return dflt;
	if (!strcmp(env, "core"))		return RDL_CORE;
	if (!strcmp(env, "l3"))			return RDL_L3;
	if (!strcmp(env, "single"))		return RDL_SINGLE;
	if (!strcmp(env, "hwthread"))		return RDL_HWTHREAD;
	fprintf(stderr, "domain '%s' unknown; using %s\n", env, rdl_name(dflt));
	return dflt;
}

/* Fill map[0..ncpu-1] with a compact domain id per the level; return #domains. */
static int build_cpu_domain_map(enum reclaim_domain_level lvl, int ncpu, int *map)
{
	int c, ndom = 1;

	for (c = 0; c < ncpu; c++) {
		switch (lvl) {
		case RDL_CORE:	 map[c] = bench_topology_core_of(c); break;
		case RDL_L3:	 map[c] = bench_topology_l3_of(c);   break;
		case RDL_SINGLE: map[c] = 0;			     break;
		default:	 map[c] = c;			     break;
		}
		if (map[c] + 1 > ndom)
			ndom = map[c] + 1;
	}
	return ndom;
}

/* Build the WORKER domain (+ anchors) and, independently, the SLAB domain.
 * The slab defaults to hwthread so BENCH_RECLAIM_DOMAIN isolates the worker
 * effect; BENCH_SLAB_DOMAIN opts the slab into a coarser (shared) arena. */
static void reclaim_domain_build(void)
{
	int ncpu = (int) sysconf(_SC_NPROCESSORS_CONF), c, i, npu;

	if (g_cpu_domain != NULL)
		return;				/* already built */
	if (ncpu < 1)
		ncpu = 1;
	g_rdl_ncpu = ncpu;
	g_rdl_unconfined = getenv("BENCH_RECLAIM_UNCONFINED") != NULL;
	g_rdl = rdl_parse(getenv("BENCH_RECLAIM_DOMAIN"), RDL_HWTHREAD);
	g_sdl = rdl_parse(getenv("BENCH_SLAB_DOMAIN"), RDL_HWTHREAD);

	g_cpu_domain = malloc((size_t) ncpu * sizeof(*g_cpu_domain));
	g_ndomains = build_cpu_domain_map(g_rdl, ncpu, g_cpu_domain);
	g_slab_domain = malloc((size_t) ncpu * sizeof(*g_slab_domain));
	g_slab_ndomains = build_cpu_domain_map(g_sdl, ncpu, g_slab_domain);

	/* Anchor = lowest writer-index PU that lands in each worker domain (a
	 * writer PU whenever the domain is active, so confinement holds at C). */
	g_domain_anchor = malloc((size_t) g_ndomains * sizeof(*g_domain_anchor));
	for (i = 0; i < g_ndomains; i++)
		g_domain_anchor[i] = -1;
	npu = bench_topology_pu_count();
	if (npu <= 0)
		npu = ncpu;
	for (i = 0; i < npu; i++) {
		int cpu = bench_topology_cpu(i), d;

		if (cpu < 0 || cpu >= ncpu)
			continue;
		d = g_cpu_domain[cpu];
		if (d >= 0 && d < g_ndomains && g_domain_anchor[d] < 0)
			g_domain_anchor[d] = cpu;
	}
	for (i = 0; i < g_ndomains; i++)	/* domains no worker index hit */
		if (g_domain_anchor[i] < 0)
			for (c = 0; c < ncpu; c++)
				if (g_cpu_domain[c] == i) {
					g_domain_anchor[i] = c;
					break;
				}

	fprintf(stderr,
		"reclaim worker domain: %s (%d) | slab domain: %s (%d) / %d CPUs%s\n",
		rdl_name(g_rdl), g_ndomains, rdl_name(g_sdl), g_slab_ndomains, ncpu,
		g_rdl_unconfined ? " [worker UNCONFINED: measures inflation]" : "");
}

/* One call_rcu worker per domain, pinned to the domain's anchor writer-PU
 * (or the whole domain cpumask when UNCONFINED), routing every CPU to it.
 * Replaces create_all_cpu_call_rcu_data().  Returns 0 on success. */
static int reclaim_workers_setup(void)
{
	struct call_rcu_data **worker;
	int d, c;

	worker = calloc((size_t) g_ndomains, sizeof(*worker));
	if (worker == NULL)
		return -1;
	for (d = 0; d < g_ndomains; d++) {
		struct call_rcu_data *crdp;
		cpu_set_t set;

		/* cpu_affinity = -1: liburcu never pins it, so its throttled
		 * re-pin can't fight or re-narrow the mask we set below. */
		crdp = create_call_rcu_data(0, -1);
		if (crdp == NULL) {
			free(worker);
			return -1;
		}
		worker[d] = crdp;
		CPU_ZERO(&set);
		if (g_rdl_unconfined) {
			for (c = 0; c < g_rdl_ncpu; c++)
				if (g_cpu_domain[c] == d)
					CPU_SET(c, &set);
		} else {
			CPU_SET(g_domain_anchor[d] >= 0 ? g_domain_anchor[d] : 0,
				&set);
		}
		/* Explicit + independent of any workload pinning the creating
		 * thread may carry -- this is the invariant that keeps reclaim
		 * inside the writers' PU budget. */
		if (pthread_setaffinity_np(get_call_rcu_thread(crdp),
					   sizeof(set), &set) != 0)
			perror("reclaim worker setaffinity");
	}
	/* Route every CPU to its domain worker (many CPUs -> one worker). */
	for (c = 0; c < g_rdl_ncpu; c++) {
		int d2 = g_cpu_domain[c];

		if (d2 < 0 || d2 >= g_ndomains || worker[d2] == NULL)
			continue;
		(void) set_cpu_call_rcu_data(c, NULL);		/* clear any prior */
		(void) set_cpu_call_rcu_data(c, worker[d2]);
	}
	free(worker);
	return 0;
}

#ifdef LIST_RCU_INLINE_RCU_HEAD
/* ════════════════════════════════════════════════════════════════
 * Per-CPU slab node allocator (prototype) -- models the fractal-trie
 * external arena (urcu/fractal-trie-alloc.c) repurposed for per-CPU
 * arenas.  Each CPU gets its own arena: a LIFO freelist of recycled
 * nodes plus a bump pointer into RANGE-aligned, NUMA-node-local mmap'd
 * superblocks.  free() finds a node's ORIGIN arena from the superblock
 * header (superblocks are RANGE-aligned, so header = ptr & ~(RANGE-1)),
 * so a node allocated on CPU X and freed by the per-CPU call_rcu worker
 * -- on whatever CPU it runs -- returns to arena X.  That is exactly the
 * migration-safe property the FT range->arena back-pointer provides;
 * here it is a one-word superblock header.  Per-arena mutex (contended
 * only by threads currently on that CPU); an rseq fast path would make
 * it lock-free, like tcmalloc's per-CPU caches.  Activated by
 * BENCH_PCPU_ALLOC (inline-rcu_head build only -- the recycled churn
 * node reuses its first word as the freelist link and carries an rh).
 * ════════════════════════════════════════════════════════════════ */
#define PCPU_SLAB_RANGE_SIZE	(1UL << 24)		/* 16 MiB superblocks (== FT_EXT_ARENA) */
#define PCPU_SLAB_RANGE_MASK	(PCPU_SLAB_RANGE_SIZE - 1)

struct pcpu_slab_arena;
struct pcpu_slab_sb {				/* superblock header at the RANGE-aligned base */
	struct pcpu_slab_arena *owner;
	size_t bump;				/* next free byte offset within this sb */
	struct pcpu_slab_sb *next;		/* arena's superblock list */
};
struct pcpu_slab_freenode { struct pcpu_slab_freenode *next; };
struct pcpu_slab_arena {
	pthread_mutex_t lock;
	struct pcpu_slab_freenode *flist;	/* LIFO recycle list (hot reuse) */
	struct pcpu_slab_sb *sb;		/* current bump superblock + list head */
	int numa_node;				/* NUMA node of this CPU (or -1) */
	char _pad[64];				/* keep arenas off each other's cachelines */
};
struct pcpu_slab {
	int ncpu;
	size_t obj;				/* fixed object size (>= node, >= 16, 16-aligned) */
	struct pcpu_slab_arena *arena;		/* [ncpu] */
};

static size_t pcpu_round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

static struct pcpu_slab *pcpu_slab_create(size_t obj_size)
{
	struct pcpu_slab *s = calloc(1, sizeof(*s));
	int i, narena = g_slab_ndomains > 0 ? g_slab_ndomains : 1;
	int have_numa = (numa_available() != -1);

	s->ncpu = narena;			/* one arena per slab domain */
	s->obj = pcpu_round_up(obj_size < 16 ? 16 : obj_size, 16);
	s->arena = calloc((size_t) narena, sizeof(*s->arena));
	for (i = 0; i < narena; i++) {
		int rep = 0, c;			/* a CPU in slab domain i, for NUMA */

		for (c = 0; c < g_rdl_ncpu; c++)
			if (g_slab_domain != NULL && g_slab_domain[c] == i) {
				rep = c;
				break;
			}
		pthread_mutex_init(&s->arena[i].lock, NULL);
		s->arena[i].numa_node = have_numa ? numa_node_of_cpu(rep) : -1;
	}
	return s;
}

/* Map a fresh RANGE-aligned superblock, bound to @a's NUMA node. */
static struct pcpu_slab_sb *pcpu_slab_sb_new(struct pcpu_slab_arena *a)
{
	size_t raw = 2 * PCPU_SLAB_RANGE_SIZE;
	char *p = mmap(NULL, raw, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	char *base;
	struct pcpu_slab_sb *sb;

	if (p == MAP_FAILED) { perror("pcpu_slab mmap"); abort(); }
	base = (char *) (((uintptr_t) p + PCPU_SLAB_RANGE_MASK) & ~(uintptr_t) PCPU_SLAB_RANGE_MASK);
	if (base != p)				/* trim slack so only the aligned RANGE stays mapped */
		munmap(p, base - p);
	if (base + PCPU_SLAB_RANGE_SIZE != p + raw)
		munmap(base + PCPU_SLAB_RANGE_SIZE, (p + raw) - (base + PCPU_SLAB_RANGE_SIZE));
	if (a->numa_node >= 0)
		numa_tonode_memory(base, PCPU_SLAB_RANGE_SIZE, a->numa_node);
	sb = (struct pcpu_slab_sb *) base;
	sb->owner = a;
	sb->bump = pcpu_round_up(sizeof(*sb), 16);	/* objects start past the header */
	sb->next = a->sb;
	return sb;
}

static void *pcpu_slab_alloc(struct pcpu_slab *s)
{
	int cpu = sched_getcpu();
	int d = (g_slab_domain != NULL && cpu >= 0 && cpu < g_rdl_ncpu)
		? g_slab_domain[cpu] : 0;	/* arena per slab domain */
	struct pcpu_slab_arena *a;
	void *p;

	if (d < 0 || d >= s->ncpu) d = 0;
	a = &s->arena[d];
	pthread_mutex_lock(&a->lock);
	if (a->flist) {				/* hot: reuse a just-freed node */
		p = a->flist;
		a->flist = a->flist->next;
	} else {				/* cold: bump from the active superblock */
		if (!a->sb || a->sb->bump + s->obj > PCPU_SLAB_RANGE_SIZE)
			a->sb = pcpu_slab_sb_new(a);
		p = (char *) a->sb + a->sb->bump;
		a->sb->bump += s->obj;
	}
	pthread_mutex_unlock(&a->lock);
	return p;
}

/* Return @ptr to its ORIGIN arena (found from the RANGE-aligned superblock). */
static void pcpu_slab_free(void *ptr)
{
	struct pcpu_slab_sb *sb = (struct pcpu_slab_sb *)
			((uintptr_t) ptr & ~(uintptr_t) PCPU_SLAB_RANGE_MASK);
	struct pcpu_slab_arena *a = sb->owner;
	struct pcpu_slab_freenode *fn = ptr;

	pthread_mutex_lock(&a->lock);
	fn->next = a->flist;
	a->flist = fn;
	pthread_mutex_unlock(&a->lock);
}

static int g_pcpu_alloc;			/* BENCH_PCPU_ALLOC */
static struct pcpu_slab *g_slab;
#endif /* LIST_RCU_INLINE_RCU_HEAD */

/* ── Engine vtable ─────────────────────────────────────────────── */
struct lengine {
	const char *name;
	const char *label;
	int uses_rcu;			/* register thread + quiescent states */
	void (*build)(void);
	/* one read pass: forward + reverse walk; returns nodes visited,
	 * accumulates monotonicity violations into *viol. */
	unsigned long (*read_pass)(long *viol);
	/* toggle churn slot @slot: insert if absent, else delete. */
	void (*write_toggle)(int slot);
	/* BENCH_RANDOM_POS: toggle a randomly chosen TRANSACTED INDEX slot.  Each
	 * slot points at a list cell or is empty; a writer folds the list splice
	 * (or unlink) AND the index-slot update into ONE flip, and reads the slot
	 * back through a txn load -- modeling an external index (e.g. a
	 * fractal-trie leaf) kept coherent with list membership by a single
	 * multi-slot commit.  Collisions on the same slot (P ~ writers/NINDEX) are
	 * serialized by the slot's MCAS.  NULL => engine has no transacted-index
	 * mode (falls back to the partitioned toggle). */
	void (*write_random)(uint64_t *rng);
	/* Clear all transacted-index cells (single-threaded, between sweep
	 * points).  NULL unless the engine provides write_random. */
	void (*reset_random)(void);
	/* Optional per-engine thread-lifecycle hooks, for an engine (RLU) that
	 * carries its OWN thread registration instead of liburcu's.  point_reset
	 * runs on main at the START of each sweep point, before any worker is
	 * spawned (rewind the engine's thread registry); tl_begin/tl_end run once
	 * per worker (and per main churn-reset pass) at entry/exit.  All NULL for
	 * the liburcu / lock engines. */
	void (*point_reset)(void);
	void (*tl_begin)(void);
	void (*tl_end)(void);
};

/* xorshift64: cheap per-writer PRNG for random anchor selection. */
static inline uint64_t xorshift64(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return *s = x;
}

/* ════════════════════════════════════════════════════════════════
 * Plain doubly-linked list, shared by every lock/seqlock engine.
 * Permanent per-slot churn nodes (recycled in place: safe under the
 * engine's mutual exclusion, and type-stable for the seqlock readers).
 * ════════════════════════════════════════════════════════════════ */
struct pnode { struct pnode *next, *prev; int key;
#ifdef LIST_PNODE_PAD
	/* Controlled test: fatten the plain node to the RCU node's size so the
	 * read working set matches, isolating the rcu_head cache-footprint effect. */
	char _pad[LIST_PNODE_PAD];
#endif
};

static struct pnode  g_phead;
static struct pnode **g_pstable;	/* [LIST_SIZE] */
static struct pnode **g_pchurn;		/* [CHURN], permanent, linked on demand */

static void plain_build(void)
{
	int i, j;

	g_phead.next = g_phead.prev = &g_phead;
	g_pstable = calloc(LIST_SIZE, sizeof(*g_pstable));
	for (i = 0; i < LIST_SIZE; i++) {
		struct pnode *n = calloc(1, sizeof(*n));
		n->key = 2 * i;
		n->prev = g_phead.prev;		/* append at tail */
		n->next = &g_phead;
		g_phead.prev->next = n;
		g_phead.prev = n;
		g_pstable[i] = n;
	}
	g_pchurn = calloc(CHURN, sizeof(*g_pchurn));
	for (j = 0; j < CHURN; j++) {
		struct pnode *n = calloc(1, sizeof(*n));
		n->key = 2 * g_anchor[j] + 1;
		g_pchurn[j] = n;		/* not linked yet */
	}
}

static inline void plain_link_after(struct pnode *n, struct pnode *a)
{
	n->prev = a;
	n->next = a->next;
	a->next->prev = n;
	a->next = n;
}

static inline void plain_unlink(struct pnode *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
}

/* Caller holds the engine's write lock (or seqlock writer mutex). */
static void plain_toggle(int slot)
{
	if (g_present[slot]) {
		plain_unlink(g_pchurn[slot]);
		g_present[slot] = 0;
	} else {
		plain_link_after(g_pchurn[slot], g_pstable[g_anchor[slot]]);
		g_present[slot] = 1;
	}
}

/* Forward+reverse walk of a *consistent* snapshot (caller holds a read lock,
 * or seqlock retries until consistent).  Bumps *viol on a non-monotone step. */
static unsigned long plain_traverse(long *viol)
{
	struct pnode *p;
	unsigned long vis = 0;
	int prev, steps;

	prev = INT_MIN;
	steps = 0;
	for (p = g_phead.next; p != &g_phead; p = p->next) {
		if (p->key <= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
		prev = p->key;
		vis++;
	}
	prev = INT_MAX;
	steps = 0;
	for (p = g_phead.prev; p != &g_phead; p = p->prev) {
		if (p->key >= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
		prev = p->key;
		vis++;
	}
	return vis;
}

/* ── mutex ── */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static void mtx_build(void) { plain_build(); }
static unsigned long mtx_read(long *v)
{
	unsigned long r;
	pthread_mutex_lock(&g_mtx);
	r = plain_traverse(v);
	pthread_mutex_unlock(&g_mtx);
	return r;
}
static void mtx_write(int slot)
{
	pthread_mutex_lock(&g_mtx);
	plain_toggle(slot);
	pthread_mutex_unlock(&g_mtx);
}

/* ── fairmutex (liburcu cds_fair_mutex, MCS/FIFO) ── */
static struct cds_fair_mutex g_fm;
static void fm_build(void) { cds_fair_mutex_init(&g_fm); plain_build(); }
static unsigned long fm_read(long *v)
{
	struct cds_fair_mutex_node w;
	unsigned long r;
	cds_fair_mutex_lock(&g_fm, &w);
	r = plain_traverse(v);
	cds_fair_mutex_unlock(&g_fm, &w);
	return r;
}
static void fm_write(int slot)
{
	struct cds_fair_mutex_node w;
	cds_fair_mutex_lock(&g_fm, &w);
	plain_toggle(slot);
	cds_fair_mutex_unlock(&g_fm, &w);
}

/* ── rwlock_r / rwlock_w (pthread, reader- vs writer-preferring) ── */
static pthread_rwlock_t g_rw;
static void rw_build_reader(void)
{
	pthread_rwlock_init(&g_rw, NULL);	/* glibc default: reader-preferring */
	plain_build();
}
static void rw_build_writer(void)
{
	pthread_rwlockattr_t a;
	pthread_rwlockattr_init(&a);
	pthread_rwlockattr_setkind_np(&a,
		PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	pthread_rwlock_init(&g_rw, &a);
	pthread_rwlockattr_destroy(&a);
	plain_build();
}
static unsigned long rw_read(long *v)
{
	unsigned long r;
	pthread_rwlock_rdlock(&g_rw);
	r = plain_traverse(v);
	pthread_rwlock_unlock(&g_rw);
	return r;
}
static void rw_write(int slot)
{
	pthread_rwlock_wrlock(&g_rw);
	plain_toggle(slot);
	pthread_rwlock_unlock(&g_rw);
}

/* ── iscrw (bind9 isc_rwlock, C-RW-WP phase-fair) ── */
static void *g_iscrw;
static void isc_build(void)
{
	if (posix_memalign(&g_iscrw, bench_iscrw_align() < 64 ? 64 : bench_iscrw_align(),
			bench_iscrw_size()) != 0)
		abort();
	bench_iscrw_init(g_iscrw);
	plain_build();
}
static unsigned long isc_read(long *v)
{
	unsigned long r;
	bench_iscrw_rdlock(g_iscrw);
	r = plain_traverse(v);
	bench_iscrw_rdunlock(g_iscrw);
	return r;
}
static void isc_write(int slot)
{
	bench_iscrw_wrlock(g_iscrw);
	plain_toggle(slot);
	bench_iscrw_wrunlock(g_iscrw);
}

/* ── seqlock (optimistic readers retry; writers serialize + bump seq) ── */
static unsigned long g_seq;			/* even = stable, odd = write in progress */
static pthread_mutex_t g_seq_wmtx = PTHREAD_MUTEX_INITIALIZER;
static void seq_build(void) { plain_build(); }
static unsigned long seq_read(long *v)
{
	for (;;) {
		unsigned long s, vis;
		long localviol = 0;

		s = uatomic_load(&g_seq, CMM_ACQUIRE);
		if (s & 1UL) {				/* writer active */
			if (uatomic_load(&stop_flag, CMM_RELAXED))
				return 0;
			caa_cpu_relax();
			continue;
		}
		cmm_smp_rmb();
		vis = plain_traverse(&localviol);	/* may read torn state */
		cmm_smp_rmb();
		if (uatomic_load(&g_seq, CMM_ACQUIRE) == s) {
			*v += localviol;		/* consistent: real violations */
			return vis;
		}
		/* torn read: discard and retry (not a coherence violation) */
		if (uatomic_load(&stop_flag, CMM_RELAXED))
			return 0;
	}
}
static void seq_write(int slot)
{
	unsigned long s;
	pthread_mutex_lock(&g_seq_wmtx);
	s = g_seq;
	uatomic_store(&g_seq, s + 1, CMM_RELAXED);	/* -> odd */
	cmm_smp_wmb();
	plain_toggle(slot);
	cmm_smp_wmb();
	uatomic_store(&g_seq, s + 2, CMM_RELAXED);	/* -> even */
	pthread_mutex_unlock(&g_seq_wmtx);
}

/* ════════════════════════════════════════════════════════════════
 * RCU engines.  Readers are lock-free under rcu_read_lock(); deleted
 * nodes are freed through call_rcu() after a grace period.
 * ════════════════════════════════════════════════════════════════ */

/*
 * Node layout switch.  DEFAULT is the realistic production layout: the
 * struct rcu_head is segregated OUT of the hot node (into a small reclaim
 * record allocated on the cold delete path) and the stable nodes are packed in
 * a dense arena -- so the hot read-set is 24 B/node and packs like the plain
 * list, the way a strided allocator that pairs hot data with cold metadata on
 * separate cachelines behaves.  With -DLIST_RCU_INLINE_RCU_HEAD the rcu_head is
 * embedded inline next to the hot read fields (link + key), bloating every node
 * 24 -> 40 B so the cold reclamation metadata pollutes the traversal's cacheline
 * working set: that is the *artifact* build, kept to demonstrate how much a bad
 * node layout costs the read side.  In every layout `key` stays at the same
 * offset (right after the link), so the reader's container_of is layout-agnostic.
 */
#ifndef LIST_RCU_INLINE_RCU_HEAD
struct seg_reclaim { struct rcu_head rh; void *node; };
static void seg_reclaim_cb(struct rcu_head *h)
{
	struct seg_reclaim *w = caa_container_of(h, struct seg_reclaim, rh);
	TP_FREE(w->node);		/* cell about to be physically freed */
	free(w->node);
	free(w);
}
static void seg_call_rcu(void *node)		/* node: an individually-malloc'd churn node */
{
	struct seg_reclaim *w = malloc(sizeof(*w));
	if (!w) abort();
	w->node = node;
	call_rcu(&w->rh, seg_reclaim_cb);
}
#endif

/* ── txn_sw_list: single-updater coherent bidirectional RCU list ── */
struct su_elem {
	struct urcu_txn_sw_list_node node;
	int key;
#ifdef LIST_RCU_INLINE_RCU_HEAD
	struct rcu_head rh;
#endif
};
static struct urcu_txn_sw_list_head g_su_head =
		URCU_TXN_SW_LIST_HEAD_INIT(g_su_head);
static struct su_elem **g_su_stable;
static struct su_elem **g_su_churn;
static pthread_mutex_t g_su_wlock = PTHREAD_MUTEX_INITIALIZER;
static int g_su_nolock;		/* BENCH_SU_NOLOCK: skip the per-op writer mutex --
				 * the RAW lock-free single-updater cost (models the
				 * quiesced/bulk-mode plain-store path).  CORRECT ONLY
				 * with a single writer; the mutex otherwise exists so
				 * the multi-writer sweep harness can't corrupt the
				 * single-updater list. */

#ifdef LIST_RCU_INLINE_RCU_HEAD
static void su_free(struct rcu_head *h)
{
	free(caa_container_of(h, struct su_elem, rh));
}
/* call_rcu reclaim that returns the churn node to its per-CPU slab arena. */
static void su_slab_free(struct rcu_head *h)
{
	pcpu_slab_free(caa_container_of(h, struct su_elem, rh));
}
#endif
static void su_build(void)
{
	int i;
	urcu_txn_sw_list_init(&g_su_head);
	g_su_stable = calloc(LIST_SIZE, sizeof(*g_su_stable));
#ifndef LIST_RCU_INLINE_RCU_HEAD
	struct su_elem *arena = calloc(LIST_SIZE, sizeof(struct su_elem));
#endif
	for (i = 0; i < LIST_SIZE; i++) {
#ifndef LIST_RCU_INLINE_RCU_HEAD
		struct su_elem *e = &arena[i];	/* dense, packed */
#else
		struct su_elem *e = calloc(1, sizeof(*e));
#endif
		e->key = 2 * i;
		if (urcu_txn_sw_list_add_tail_rcu(&e->node, &g_su_head))
			abort();
		g_su_stable[i] = e;
	}
	g_su_churn = calloc(CHURN, sizeof(*g_su_churn));
}
static unsigned long su_read(long *viol)
{
	struct urcu_txn_sw_list_node *p;
	unsigned long vis = 0;
	int prev, steps;

	rcu_read_lock();
	prev = INT_MIN; steps = 0;
	for (p = urcu_txn_sw_list_next_rcu(&g_su_head.node); p != &g_su_head.node;
			p = urcu_txn_sw_list_next_rcu(p)) {
		int k = caa_container_of(p, struct su_elem, node)->key;
		if (k <= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
		prev = k; vis++;
	}
	prev = INT_MAX; steps = 0;
	for (p = urcu_txn_sw_list_prev_rcu(&g_su_head.node); p != &g_su_head.node;
			p = urcu_txn_sw_list_prev_rcu(p)) {
		int k = caa_container_of(p, struct su_elem, node)->key;
		if (k >= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
		prev = k; vis++;
	}
	rcu_read_unlock();
	return vis;
}
static void su_write(int slot)
{
	if (!g_su_nolock)
		pthread_mutex_lock(&g_su_wlock);
	if (g_present[slot]) {
		struct su_elem *e = g_su_churn[slot];
		if (urcu_txn_sw_list_del_rcu(&e->node))
			abort();
#ifndef LIST_RCU_INLINE_RCU_HEAD
		seg_call_rcu(e);
#else
		call_rcu(&e->rh, g_pcpu_alloc ? su_slab_free : su_free);
#endif
		g_su_churn[slot] = NULL;
		g_present[slot] = 0;
	} else {
		int a = g_anchor[slot];
#ifdef LIST_RCU_INLINE_RCU_HEAD
		struct su_elem *e = g_pcpu_alloc ?
			(struct su_elem *) pcpu_slab_alloc(g_slab) : malloc(sizeof(*e));
#else
		struct su_elem *e = malloc(sizeof(*e));
#endif
		e->key = 2 * a + 1;
		if (urcu_txn_sw_list_add_after_rcu(&e->node, &g_su_stable[a]->node))
			abort();
		g_su_churn[slot] = e;
		g_present[slot] = 1;
	}
	if (!g_su_nolock)
		pthread_mutex_unlock(&g_su_wlock);
}

/* ── txn_list: concurrent coherent bidirectional RCU list ── */
struct lf_elem {
	struct urcu_txn_list_node node;
	int key;
#ifdef LIST_RCU_INLINE_RCU_HEAD
	struct rcu_head rh;
#endif
};
static struct urcu_txn_list_head g_lf_head;
static struct urcu_txn_domain g_lf_dom;	/* one escalation domain for the list */
static struct lf_elem **g_lf_stable;
static struct lf_elem **g_lf_churn;
/*
 * Transacted external index (BENCH_RANDOM_POS): each slot points at a list cell
 * or is NULL.  Writers update a slot atomically WITH the list splice/unlink in
 * one flip and read it back through a txn load -- modeling a fractal-trie
 * leaf kept coherent with the ordered-cell list.
 */
static struct urcu_txn_list_node **g_lf_index;	/* [g_nindex] */
static int g_nindex;

#ifdef LIST_RCU_INLINE_RCU_HEAD
static void lf_free(struct rcu_head *h)
{
	free(caa_container_of(h, struct lf_elem, rh));
}
/* call_rcu reclaim that returns the churn node to its per-CPU slab arena. */
static void lf_slab_free(struct rcu_head *h)
{
	pcpu_slab_free(caa_container_of(h, struct lf_elem, rh));
}
#endif

/* ── cell alloc / free / reclaim (shared by the transacted-index writer) ── */
static inline struct lf_elem *lf_cell_alloc(void)
{
#ifdef LIST_RCU_INLINE_RCU_HEAD
	return g_pcpu_alloc ? (struct lf_elem *) pcpu_slab_alloc(g_slab)
			    : malloc(sizeof(struct lf_elem));
#else
	return malloc(sizeof(struct lf_elem));
#endif
}
/* Free an UNPUBLISHED cell synchronously (never linked, no readers reached it). */
static inline void lf_cell_free(struct lf_elem *e)
{
#ifdef LIST_RCU_INLINE_RCU_HEAD
	if (g_pcpu_alloc)
		pcpu_slab_free(e);
	else
		free(e);
#else
	free(e);
#endif
}
/* Reclaim a PUBLISHED cell after a grace period. */
static inline void lf_cell_reclaim(struct lf_elem *e)
{
	TP_CALLRCU(&e->node);		/* this writer defers the cell's free */
#ifndef LIST_RCU_INLINE_RCU_HEAD
	seg_call_rcu(e);
#else
	call_rcu(&e->rh, g_pcpu_alloc ? lf_slab_free : lf_free);
#endif
}

static void lf_build(void)
{
	int i;
	urcu_txn_list_init(&g_lf_head);
	urcu_txn_domain_init(&g_lf_dom);
	g_lf_stable = calloc(LIST_SIZE, sizeof(*g_lf_stable));
#ifndef LIST_RCU_INLINE_RCU_HEAD
	struct lf_elem *arena = calloc(LIST_SIZE, sizeof(struct lf_elem));
#endif
	for (i = 0; i < LIST_SIZE; i++) {
#ifndef LIST_RCU_INLINE_RCU_HEAD
		struct lf_elem *e = &arena[i];	/* dense, packed */
#else
		struct lf_elem *e = calloc(1, sizeof(*e));
#endif
		e->key = 2 * i;
		if (urcu_txn_list_add_tail_rcu(&e->node, &g_lf_head, &g_lf_dom))
			abort();
		g_lf_stable[i] = e;
	}
	g_lf_churn = calloc(CHURN, sizeof(*g_lf_churn));
	g_nindex = CHURN > 0 ? CHURN : 1;	/* index-slot count = contention knob */
	g_lf_index = calloc((size_t) g_nindex, sizeof(*g_lf_index));
}
static unsigned long lf_read(long *viol)
{
	struct urcu_txn_list_node *p;
	unsigned long vis = 0;
	int prev, steps;

	rcu_read_lock();
	prev = INT_MIN; steps = 0;
	for (p = urcu_txn_list_next_rcu(&g_lf_head.node);
			p != &g_lf_head.node;
			p = urcu_txn_list_next_rcu(p)) {
		int k = caa_container_of(p, struct lf_elem, node)->key;
		if (++steps > STEP_LIMIT) { (*viol)++; break; }	/* runaway guard: always */
		if (!g_random_pos) {			/* sortedness only in the sorted (churn) mode */
			if (k <= prev) { (*viol)++; break; }
			prev = k;
		}
		vis++;
	}
	prev = INT_MAX; steps = 0;
	for (p = urcu_txn_list_prev_rcu(&g_lf_head.node);
			p != &g_lf_head.node;
			p = urcu_txn_list_prev_rcu(p)) {
		int k = caa_container_of(p, struct lf_elem, node)->key;
		if (++steps > STEP_LIMIT) { (*viol)++; break; }
		if (!g_random_pos) {
			if (k >= prev) { (*viol)++; break; }
			prev = k;
		}
		vis++;
	}
	rcu_read_unlock();
	return vis;
}
static void lf_write(int slot)
{
	/* Lock-free: no writer lock.  Hold a read-side section across the
	 * mutator so the node arguments stay alive (the mutator nests its own). */
	if (g_present[slot]) {
		struct lf_elem *e = g_lf_churn[slot];
		int r;
		rcu_read_lock();
		r = urcu_txn_list_del_rcu(&e->node, &g_lf_dom);
		rcu_read_unlock();
		if (r < 0)
			abort();		/* -ENOMEM */
		if (r == 1)
#ifndef LIST_RCU_INLINE_RCU_HEAD
			seg_call_rcu(e);
#else
			call_rcu(&e->rh, g_pcpu_alloc ? lf_slab_free : lf_free);
#endif
		g_lf_churn[slot] = NULL;
		g_present[slot] = 0;
	} else {
		int a = g_anchor[slot], r;
#ifdef LIST_RCU_INLINE_RCU_HEAD
		struct lf_elem *e = g_pcpu_alloc ?
				(struct lf_elem *) pcpu_slab_alloc(g_slab) : malloc(sizeof(*e));
#else
		struct lf_elem *e = malloc(sizeof(*e));
#endif
		e->key = 2 * a + 1;
		rcu_read_lock();
		r = urcu_txn_list_insert_after_rcu(&e->node,
				&g_lf_stable[a]->node, &g_lf_dom);
		rcu_read_unlock();
		if (r != 0)			/* anchor is permanent: never -ENOENT */
			abort();
		g_lf_churn[slot] = e;
		g_present[slot] = 1;
	}
}
/*
 * Diagnostic: with -DBENCH_IDX_XOR the index slot stores the cell pointer XORed
 * with a fixed non-heap, 16-aligned constant, so the stored value never
 * pointer-EQUALS any list node -- the txn is structurally identical (same slots,
 * same sort, same proxy machinery), only the value "names" nothing.  Used to
 * tell "foreign slot in the sorted MCAS" apart from "index value aliases a list
 * node".  Default (unset): the index stores &cell->node directly.
 */
#ifdef BENCH_IDX_XOR
#define IDX_MAGIC	((uintptr_t) 0x5a5a5a5a0000ULL)	/* 16-aligned, non-heap */
#define IDX_ENC(node)	((void *) ((uintptr_t) (node) ^ IDX_MAGIC))
#define IDX_DEC(v)	((struct urcu_txn_list_node *) ((uintptr_t) (v) ^ IDX_MAGIC))
#else
#define IDX_ENC(node)	((void *) (node))
#define IDX_DEC(v)	((struct urcu_txn_list_node *) (v))
#endif

#ifdef BENCH_LTTNG
/*
 * Coherence probe: pos->next == succ must imply succ->prev == pos in a coherent
 * bidir list.  Fire the incoh violation (and snapshot+abort) if not -- catching
 * a non-atomic/incoherent composed commit while succ is still alive, before it
 * is deleted and pos->next is left dangling.  (If succ is already dangling, the
 * succ->prev read trips ASan -> death-callback snapshot instead.)
 */
static void bench_coh_check(struct urcu_txn_list_node *pos)
{
	struct urcu_txn_list_node *succ = urcu_txn_list_next_rcu(pos);
	struct urcu_txn_list_node *sp;

	if (succ == pos)
		return;
	sp = urcu_txn_list_prev_rcu(succ);		/* succ->prev */
	/* Re-read pos->next: only a STABLE incoherence (pos->next still succ after
	 * we observed succ->prev != pos) counts -- rules out a transient where two
	 * non-atomic reads straddle a coherent flip. */
	if (sp != pos && urcu_txn_list_next_rcu(pos) == succ) {
		lttng_ust_tracepoint(bench, incoh, pos, succ, sp, t_id, t_epoch);
		(void) system("lttng snapshot record 1>&2");
		abort();
	}
}
#endif

/*
 * Transacted-index churn (BENCH_RANDOM_POS): pick a random index slot
 * g_lf_index[i] and toggle it, folding the list mutation AND the index-slot
 * update into ONE flip transaction:
 *
 *   empty slot -> alloc a cell, insert it after a random stable anchor, and
 *                 publish g_lf_index[i] = &cell -- all in one commit;
 *   full slot  -> unlink the indexed cell and clear g_lf_index[i] in one commit,
 *                 then reclaim the cell after a grace period.
 *
 * The index slot is read back through a txn load (urcu_txn_load),
 * so a writer sees a coherent value even while a peer is mid-flip on that slot.
 * Concurrent writers that draw the same i collide on the slot's MCAS -- one
 * commits, the others abort and re-decide -- so contention is ~ writers/NINDEX,
 * and the index<->membership invariant is preserved by the single multi-slot
 * commit.  This is the bidir-list-as-fractal-trie-ordered-cells model in
 * miniature.  Each attempt's begin/end brackets its own RCU read-side section
 * (the txn existence guarantee), keeping the loaded cell and the anchor alive
 * across that attempt's commit.
 */
static void lf_write_random(uint64_t *rng)
{
	int i = (int) (xorshift64(rng) % (uint64_t) g_nindex);
	struct urcu_mcas_txn txn;
	struct lf_elem *reclaim = NULL;		/* published cell to free (delete path) */

	urcu_txn_init(&txn, &g_lf_dom);
	/*
	 * Hold ONE read-side section around the whole operation (all retry
	 * attempts), as the engine's existence model requires ("rcu_read_lock
	 * around the whole operation") -- the per-attempt begin/end sections leave
	 * a quiescent gap between attempts in which a node reached from the array
	 * (a stale cur, or a succ neighbour) can be reclaimed under us.
	 */
	rcu_read_lock();
	for (;;) {
		struct urcu_txn_list_node *cur;
		struct lf_elem *fresh = NULL;	/* cell built this attempt (insert path) */
		enum urcu_txn_status st;
		void *raw;			/* slot's stored value (possibly encoded) */
		int prep;
#ifdef BENCH_LTTNG
		struct urcu_txn_list_node *ins_pos = NULL; /* anchor (insert) */
		void *old_succ = NULL;		/* pos->next BEFORE this insert */
		struct urcu_txn_list_node *del_cur = NULL; /* cell being removed */
		struct urcu_txn_list_node *del_prev = NULL; /* its predecessor P */
		struct urcu_txn_list_node *del_succ = NULL; /* its successor S */
#endif

		urcu_txn_begin(&txn);
		/* Flip-latch load of the transacted index slot (resolves any proxy). */
		raw = urcu_txn_load(&txn, (void **) &g_lf_index[i], URCU_MCAS_TAG);
		cur = (raw == NULL) ? NULL : IDX_DEC(raw);
		if (cur == NULL) {		/* empty -> insert + publish index, one flip */
			int a = (int) (xorshift64(rng) % (uint64_t) LIST_SIZE);

			fresh = lf_cell_alloc();
			fresh->key = 0;		/* unsorted: random mode is writer-only */
			/* record the succ (pos->next) the prepare is about to deref */
			TP_READSUCC(urcu_txn_list_next_rcu(&g_lf_stable[a]->node));
#ifdef BENCH_LTTNG
			ins_pos = &g_lf_stable[a]->node;
			old_succ = urcu_txn_list_next_rcu(ins_pos);
			bench_coh_check(&g_lf_stable[a]->node);
#endif
			prep = urcu_txn_list_insert_after_prepare(&txn,
					&fresh->node, &g_lf_stable[a]->node);
			if (prep == 0)
				urcu_txn_store(&txn, (void **) &g_lf_index[i],
						NULL, IDX_ENC(&fresh->node), URCU_MCAS_TAG);
		} else {			/* full -> unlink + clear index, one flip */
#ifdef BENCH_LTTNG
			del_cur = cur;
			del_prev = urcu_txn_list_prev_rcu(cur);
			del_succ = urcu_txn_list_next_rcu(cur);
#endif
			prep = urcu_txn_list_del_prepare(&txn, cur);
			if (prep == 0)
				urcu_txn_store(&txn, (void **) &g_lf_index[i],
						raw, NULL, URCU_MCAS_TAG);
		}
		if (prep != 0) {		/* del: cur was concurrently removed */
			urcu_txn_end(&txn);
			if (fresh != NULL)	/* insert never bails, but never leak */
				lf_cell_free(fresh);
			continue;		/* re-read the index, re-decide */
		}
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (st == URCU_TXN_STATUS_OK) {
			if (cur != NULL)	/* delete path won: reclaim the old cell */
				reclaim = caa_container_of(cur, struct lf_elem, node);
#ifdef BENCH_LTTNG
			/*
			 * SOURCE CHECK (delete path): the composed del(cur) just
			 * committed SUCCEEDED, so the predecessor's forward edge
			 * P->next MUST now bypass cur.  If P->next still names cur
			 * (while the back edge S->prev landed == P), the MCAS
			 * committed a SUBSET: the prev->next store was dropped, the
			 * succ->prev store landed -- leaving P->next dangling to a
			 * cell we are about to reclaim.  This is the corruption the
			 * 18ms-of-readsucc-after-free window showed.  Snapshot now,
			 * microseconds after the commit and long before the GP that
			 * frees cur, so the engine store events are still in-window.
			 */
			if (cur != NULL && del_prev != NULL &&
					del_prev != del_cur) {
				void *raw = (void *) rcu_dereference(del_prev->next);
				void *logv = (((uintptr_t) raw) &
					(URCU_MCAS_TAG | URCU_TXN_LIST_MARK)) ?
					urcu_mcas_resolve(raw) : raw;
				int prev_dead = urcu_txn_list_is_marked(logv);
				struct urcu_txn_list_node *prev_succ =
					urcu_txn_list_unmark(logv);

				/*
				 * Fire ONLY on a LIVE (unmarked) predecessor still
				 * naming the removed cell.  A MARKED predecessor was
				 * itself concurrently deleted: its dangling ->next is
				 * unreachable, RCU keeps it from being reused, and it is
				 * therefore benign -- the false positive of the earlier
				 * mark-blind check.  A live predecessor pointing at a
				 * cell we are about to reclaim is the real corruption.
				 */
				if (!prev_dead && prev_succ == del_cur) {
					void *sp = (del_succ != del_cur) ?
						urcu_txn_list_prev_rcu(del_succ) :
						NULL;

					lttng_ust_tracepoint(bench, subset,
						del_prev, del_cur, prev_succ, sp,
						t_id, t_epoch);
					fprintf(stderr,
						"BENCH-LIVE-DANGLE prev=%p cur=%p "
						"prevnext=%p succ_prev=%p\n",
						(void *) del_prev, (void *) del_cur,
						(void *) prev_succ, sp);
					(void) system("lttng snapshot record 1>&2");
					abort();
				}
			}
#endif
#ifdef BENCH_LTTNG
			/*
			 * SOURCE CHECK (insert path): the composed insert_after(pos)
			 * just committed SUCCEEDED, so pos->next MUST be the fresh
			 * cell.  If instead pos->next is unchanged (== old_succ) while
			 * the back edge landed (old_succ->prev == fresh), the MCAS
			 * committed a SUBSET of its records: the pos->next store was
			 * dropped, the succ->prev store landed.  Name it and snapshot
			 * now -- microseconds after the corrupting commit, so the
			 * engine store events are still in the flight-recorder window.
			 */
			if (ins_pos != NULL && fresh != NULL) {
				void *pn = urcu_txn_list_next_rcu(ins_pos);

				if (pn != &fresh->node && pn == old_succ &&
						old_succ != ins_pos) {
					struct urcu_txn_list_node *os = old_succ;
					void *sp = urcu_txn_list_prev_rcu(os);

					if (sp == &fresh->node) {
						lttng_ust_tracepoint(bench, subset,
							ins_pos, &fresh->node, pn, sp,
							t_id, t_epoch);
						fprintf(stderr,
							"BENCH-SUBSET-COMMIT pos=%p fresh=%p "
							"posnext=%p succ_prev=%p\n",
							(void *) ins_pos, (void *) &fresh->node,
							pn, sp);
						(void) system("lttng snapshot record 1>&2");
						abort();
					}
				}
			}
#endif
			break;
		}
		if (fresh != NULL)		/* insert path lost the race: drop the cell */
			lf_cell_free(fresh);
		if (st == URCU_TXN_STATUS_ABORT)
			continue;		/* contention: re-read, re-decide */
		abort();			/* -ENOMEM: fatal, as elsewhere */
	}
	rcu_read_unlock();
	if (reclaim != NULL)
		lf_cell_reclaim(reclaim);
}

/* Clear every transacted-index cell (single-threaded, between sweep points). */
static void lf_reset_index(void)
{
	int i;

	for (i = 0; i < g_nindex; i++) {
		void *raw = g_lf_index[i];
		struct urcu_txn_list_node *cur;
		struct urcu_mcas_txn txn;
		int removed = 0;

		if (raw == NULL)
			continue;
		cur = IDX_DEC(raw);
		/*
		 * Unlink the cell AND clear its index slot in ONE flip, like the
		 * writer's delete path, so the transacted index slot is only ever
		 * written through the MCAS -- never a plain store.  Single-threaded
		 * here, so there is no contention (commit cannot ABORT and del_prepare
		 * cannot observe a peer's mark): one attempt suffices.
		 */
		urcu_txn_init(&txn, &g_lf_dom);
		urcu_txn_begin(&txn);
		if (urcu_txn_list_del_prepare(&txn, cur) == 0) {
			urcu_txn_store(&txn, (void **) &g_lf_index[i],
					raw, NULL, URCU_MCAS_TAG);
			removed = urcu_txn_commit(&txn) ==
					URCU_TXN_STATUS_OK;
		}
		urcu_txn_end(&txn);
		if (removed)
			lf_cell_reclaim(caa_container_of(cur, struct lf_elem, node));
	}
}

/* ── rculist: classic forward-only RCU list (reference) ── */
struct rl_elem {
	struct cds_list_head list;
	int key;
#ifdef LIST_RCU_INLINE_RCU_HEAD
	struct rcu_head rh;
#endif
};
static struct cds_list_head g_rl_head = CDS_LIST_HEAD_INIT(g_rl_head);
static struct rl_elem **g_rl_stable;
static struct rl_elem **g_rl_churn;
static pthread_mutex_t g_rl_wlock = PTHREAD_MUTEX_INITIALIZER;

#ifdef LIST_RCU_INLINE_RCU_HEAD
static void rl_free(struct rcu_head *h)
{
	free(caa_container_of(h, struct rl_elem, rh));
}
#endif
static void rl_build(void)
{
	int i;
	CDS_INIT_LIST_HEAD(&g_rl_head);
	g_rl_stable = calloc(LIST_SIZE, sizeof(*g_rl_stable));
#ifndef LIST_RCU_INLINE_RCU_HEAD
	struct rl_elem *arena = calloc(LIST_SIZE, sizeof(struct rl_elem));
#endif
	for (i = 0; i < LIST_SIZE; i++) {
#ifndef LIST_RCU_INLINE_RCU_HEAD
		struct rl_elem *e = &arena[i];	/* dense, packed */
#else
		struct rl_elem *e = calloc(1, sizeof(*e));
#endif
		e->key = 2 * i;
		cds_list_add_tail_rcu(&e->list, &g_rl_head);
		g_rl_stable[i] = e;
	}
	g_rl_churn = calloc(CHURN, sizeof(*g_rl_churn));
}
/* Forward-only: two forward passes so the visit count matches the
 * forward+reverse engines (a coherent reverse walk is exactly what this
 * list cannot provide -- the reason bidir lists exist). */
static unsigned long rl_read(long *viol)
{
	struct cds_list_head *p;
	unsigned long vis = 0;
	int pass, prev, steps;

	rcu_read_lock();
	for (pass = 0; pass < 2; pass++) {
		prev = INT_MIN; steps = 0;
		cds_list_for_each_rcu(p, &g_rl_head) {
			int k = caa_container_of(p, struct rl_elem, list)->key;
			if (k <= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
			prev = k; vis++;
		}
	}
	rcu_read_unlock();
	return vis;
}
static void rl_write(int slot)
{
	pthread_mutex_lock(&g_rl_wlock);
	if (g_present[slot]) {
		struct rl_elem *e = g_rl_churn[slot];
		cds_list_del_rcu(&e->list);
#ifndef LIST_RCU_INLINE_RCU_HEAD
		seg_call_rcu(e);
#else
		call_rcu(&e->rh, rl_free);
#endif
		g_rl_churn[slot] = NULL;
		g_present[slot] = 0;
	} else {
		int a = g_anchor[slot];
		struct rl_elem *e = malloc(sizeof(*e));
		e->key = 2 * a + 1;
		cds_list_add_rcu(&e->list, &g_rl_stable[a]->list);  /* after anchor */
		g_rl_churn[slot] = e;
		g_present[slot] = 1;
	}
	pthread_mutex_unlock(&g_rl_wlock);
}

/* ════════════════════════════════════════════════════════════════
 * rlu_list: reference Read-Log-Update bidirectional list
 *
 * The Matveev/Shavit/Felber/Marlier RLU mechanism (third_party/rlu, MIT),
 * driving the SAME doubly-linked churn workload as txn_list, so the two
 * multi-pointer-update schemes meet on identical ground.  RLU is NOT a liburcu
 * flavor: it carries its own SMR (global clock + rlu_synchronize) and its own
 * per-thread registration, so this engine sets uses_rcu = 0 and does its own
 * reader-lock / thread bookkeeping through the tl_* hooks.
 *
 * Guarantee (declared, not emulated): an RLU reader section observes a COHERENT
 * SNAPSHOT of the objects it dereferences -- strictly stronger than txn_list's
 * per-slot-linearizable (non-snapshot) reads.  We measure both as-is and report
 * the difference rather than making either side emulate the other.
 *
 * Deferral: BENCH_RLU_WS sets RLU's max_write_sets -- 1 = synchronous writeback
 * (the floor), 100 = headline defer (how RLU is meant to run at scale).  Both
 * are FINE_GRAINED (per-object locks); COARSE_GRAINED is never used.
 *
 * Thread-model bridge: upstream RLU draws uniq_id from a monotonic counter and
 * assumes long-lived threads, but our harness spawns a fresh worker set per
 * sweep point.  We give every worker a pooled rlu_thread_data_t, flush it on
 * exit (tl_end), and rewind the RLU registry between points (point_reset ->
 * rlu_bench_reset_threads); see the rlu.c local patch.
 * ════════════════════════════════════════════════════════════════ */
struct rlu_lnode { struct rlu_lnode *next, *prev; int key; };

static struct rlu_lnode  *g_rlu_head;		/* RLU_ALLOC'd circular sentinel */
static struct rlu_lnode **g_rlu_stable;		/* [LIST_SIZE] permanent nodes */
static struct rlu_lnode **g_rlu_churn;		/* [CHURN] master ptr per slot */

/*
 * Transacted external index for the multi-slot random workload (BENCH_RANDOM_POS).
 * rcu-txn transacts the raw array word g_lf_index[i] DIRECTLY in the same MCAS as
 * the list splice.  RLU can only log a write to a FIELD OF A LOCKED OBJECT, so
 * each index slot must be a permanent RLU object (rlu_icell) whose ->target field
 * names a list cell or is NULL; the writer locks that cell together with the
 * splice nodes so the index update commits atomically with the list op.  That
 * extra indirection + lock is exactly the modelling cost of "RLU transacts object
 * fields, not arbitrary words" -- a headline difference, not just plumbing.
 */
struct rlu_icell { struct rlu_lnode *target; };
static struct rlu_icell **g_rlu_index;		/* [g_rlu_nindex] permanent RLU cells */
static int g_rlu_nindex;

#ifndef RLU_POOL_MAX
#define RLU_POOL_MAX	RLU_MAX_THREADS
#endif
static rlu_thread_data_t *g_rlu_pool;		/* [RLU_POOL_MAX], reused each point */
static volatile long g_rlu_slot;		/* next free pool slot; reset per point */
static volatile long g_rlu_gen;			/* bumped per point; invalidates TLS self */
static __thread rlu_thread_data_t *tls_rlu;	/* this OS thread's registered self */
static __thread long tls_rlu_gen = -1;		/* the gen tls_rlu was registered for */
static int g_rlu_ws = 100;			/* RLU deferral depth (BENCH_RLU_WS) */

/* Register (once per point per OS thread) and return this thread's RLU self.
 * A generation mismatch means point_reset rewound the registry, so re-register
 * into a fresh pool slot -- reusing pool memory, no per-thread leak. */
static rlu_thread_data_t *rlu_self(void)
{
	if (caa_unlikely(tls_rlu == NULL || tls_rlu_gen != g_rlu_gen)) {
		long s = __atomic_fetch_add(&g_rlu_slot, 1, __ATOMIC_RELAXED);
		if (s >= RLU_POOL_MAX)		/* raise RLU_MAX_THREADS if this fires */
			abort();
		tls_rlu = &g_rlu_pool[s];
		rlu_thread_init(tls_rlu);
		tls_rlu_gen = g_rlu_gen;
	}
	return tls_rlu;
}

static void rlu_tl_begin(void) { (void) rlu_self(); }
static void rlu_tl_end(void)   { if (tls_rlu) rlu_bench_flush(tls_rlu); }

/* main, between points, no worker live: rewind the RLU registry so the next
 * point's uniq_ids restart from 0 (bounded by RLU_POOL_MAX, not the sweep's
 * total thread count). */
static void rlu_point_reset(void)
{
	rlu_bench_reset_threads();
	__atomic_store_n(&g_rlu_slot, 0, __ATOMIC_RELAXED);
	__atomic_add_fetch(&g_rlu_gen, 1, __ATOMIC_RELAXED);
}

static void rlu_build(void)
{
	const char *ws = getenv("BENCH_RLU_WS");
	struct rlu_lnode *prev;
	int i;

	if (ws) { g_rlu_ws = atoi(ws); if (g_rlu_ws < 1) g_rlu_ws = 1; }
	rlu_init(RLU_TYPE_FINE_GRAINED, g_rlu_ws);
	g_rlu_pool = calloc(RLU_POOL_MAX, sizeof(*g_rlu_pool));

	/* Build single-threaded: nodes are unlocked (copy == NULL), so plain
	 * field stores publish the initial ring without the RLU write protocol. */
	g_rlu_head = (struct rlu_lnode *) RLU_ALLOC(sizeof(struct rlu_lnode));
	g_rlu_head->key = INT_MIN;
	g_rlu_stable = calloc(LIST_SIZE, sizeof(*g_rlu_stable));
	prev = g_rlu_head;
	for (i = 0; i < LIST_SIZE; i++) {
		struct rlu_lnode *n =
			(struct rlu_lnode *) RLU_ALLOC(sizeof(struct rlu_lnode));
		n->key = 2 * i;
		n->prev = prev;
		prev->next = n;
		g_rlu_stable[i] = n;
		prev = n;
	}
	prev->next = g_rlu_head;		/* close the ring */
	g_rlu_head->prev = prev;
	g_rlu_churn = calloc(CHURN, sizeof(*g_rlu_churn));

	/* Permanent RLU index cells for the multi-slot random workload (one per
	 * contention slot; count == CHURN, mirroring txn_list's g_nindex). */
	g_rlu_nindex = CHURN > 0 ? CHURN : 1;
	g_rlu_index = calloc((size_t) g_rlu_nindex, sizeof(*g_rlu_index));
	for (i = 0; i < g_rlu_nindex; i++) {
		g_rlu_index[i] = (struct rlu_icell *) RLU_ALLOC(sizeof(struct rlu_icell));
		g_rlu_index[i]->target = NULL;
	}
}

static unsigned long rlu_read(long *viol)
{
	rlu_thread_data_t *self = rlu_self();
	struct rlu_lnode *head = g_rlu_head, *h, *p;
	unsigned long vis = 0;
	int prev, steps;

	RLU_READER_LOCK(self);
	h = (struct rlu_lnode *) RLU_DEREF(self, head);
	prev = INT_MIN; steps = 0;
	for (p = (struct rlu_lnode *) RLU_DEREF(self, h->next);
			!RLU_IS_SAME_PTRS(p, head);
			p = (struct rlu_lnode *) RLU_DEREF(self, p->next)) {
		int k = p->key;
		if (++steps > STEP_LIMIT) { (*viol)++; break; }	/* runaway guard: always */
		if (!g_random_pos) {			/* sortedness only in the sorted (churn) mode */
			if (k <= prev) { (*viol)++; break; }
			prev = k;
		}
		vis++;
	}
	prev = INT_MAX; steps = 0;
	for (p = (struct rlu_lnode *) RLU_DEREF(self, h->prev);
			!RLU_IS_SAME_PTRS(p, head);
			p = (struct rlu_lnode *) RLU_DEREF(self, p->prev)) {
		int k = p->key;
		if (++steps > STEP_LIMIT) { (*viol)++; break; }
		if (!g_random_pos) {
			if (k >= prev) { (*viol)++; break; }
			prev = k;
		}
		vis++;
	}
	RLU_READER_UNLOCK(self);
	return vis;
}

static void rlu_write(int slot)
{
	rlu_thread_data_t *self = rlu_self();

	if (g_present[slot]) {			/* unlink the churn node */
		struct rlu_lnode *cur = g_rlu_churn[slot];	/* master identity */
		struct rlu_lnode *c, *prev, *succ;
del_restart:
		RLU_READER_LOCK(self);
		c    = (struct rlu_lnode *) RLU_DEREF(self, cur);
		prev = (struct rlu_lnode *) RLU_DEREF(self, c->prev);
		succ = (struct rlu_lnode *) RLU_DEREF(self, c->next);
		if (!RLU_TRY_LOCK(self, &prev)) { RLU_ABORT(self); goto del_restart; }
		if (!RLU_TRY_LOCK(self, &cur))  { RLU_ABORT(self); goto del_restart; }
		if (!RLU_TRY_LOCK(self, &succ)) { RLU_ABORT(self); goto del_restart; }
		RLU_ASSIGN_PTR(self, &prev->next, succ);
		RLU_ASSIGN_PTR(self, &succ->prev, prev);
		RLU_FREE(self, cur);
		RLU_READER_UNLOCK(self);
		g_rlu_churn[slot] = NULL;
		g_present[slot] = 0;
	} else {				/* splice a fresh churn node after the anchor */
		int a = g_anchor[slot];
		struct rlu_lnode *pos = g_rlu_stable[a];	/* master identity */
		struct rlu_lnode *p, *succ, *nw;
ins_restart:
		RLU_READER_LOCK(self);
		p    = (struct rlu_lnode *) RLU_DEREF(self, pos);
		succ = (struct rlu_lnode *) RLU_DEREF(self, p->next);
		if (!RLU_TRY_LOCK(self, &pos))  { RLU_ABORT(self); goto ins_restart; }
		if (!RLU_TRY_LOCK(self, &succ)) { RLU_ABORT(self); goto ins_restart; }
		nw = (struct rlu_lnode *) RLU_ALLOC(sizeof(struct rlu_lnode));
		nw->key = 2 * a + 1;
		RLU_ASSIGN_PTR(self, &nw->next, succ);	/* stored as master (FORCE_ACTUAL) */
		RLU_ASSIGN_PTR(self, &nw->prev, pos);
		RLU_ASSIGN_PTR(self, &pos->next, nw);
		RLU_ASSIGN_PTR(self, &succ->prev, nw);
		RLU_READER_UNLOCK(self);
		g_rlu_churn[slot] = nw;
		g_present[slot] = 1;
	}
}

/*
 * Multi-slot random workload (BENCH_RANDOM_POS): pick a random index slot and
 * fold the list splice/unlink AND the index-slot update into ONE RLU commit --
 * the direct analogue of txn_list's lf_write_random, which folds them into one
 * MCAS.  The index cell is LOCKED FIRST, and its ->target is read from the
 * locked copy: that freezes the insert-vs-delete decision (no peer can change
 * it until we commit) and serializes concurrent writers that draw the same slot,
 * exactly the P ~ writers/nindex contention txn_list resolves on the slot's MCAS.
 * Inserted cells carry key 0 (unsorted -- random mode is writer-only), matching
 * lf_write_random.
 */
static void rlu_write_random(uint64_t *rng)
{
	rlu_thread_data_t *self = rlu_self();
	int i = (int) (xorshift64(rng) % (uint64_t) g_rlu_nindex);
	struct rlu_icell *ic0 = g_rlu_index[i], *ic;
	struct rlu_lnode *cur;

retry:
	RLU_READER_LOCK(self);
	ic = ic0;
	if (!RLU_TRY_LOCK(self, &ic)) { RLU_ABORT(self); goto retry; }
	cur = ic->target;			/* authoritative: frozen while we hold ic */
	if (cur == NULL) {			/* empty slot -> splice a fresh cell, publish it */
		int a = (int) (xorshift64(rng) % (uint64_t) LIST_SIZE);
		struct rlu_lnode *pos = g_rlu_stable[a], *succ, *nw;

		/* Lock the anchor BEFORE reading its ->next.  The lock freezes pos->next,
		 * so the successor we splice against is pos's true current next and cannot
		 * be relinked or freed by a concurrent op while it sits in our write-set.
		 * (Reading pos->next first, then locking, is the read-before-lock UAF: a
		 * peer deleting that successor at the same position leaves a dead node in
		 * the write-set that RLU's writeback dereferences.) */
		if (!RLU_TRY_LOCK(self, &pos))  { RLU_ABORT(self); goto retry; }
		succ = (struct rlu_lnode *) RLU_DEREF(self, pos->next);
		if (!RLU_TRY_LOCK(self, &succ)) { RLU_ABORT(self); goto retry; }
		nw = (struct rlu_lnode *) RLU_ALLOC(sizeof(struct rlu_lnode));
		nw->key = 0;
		RLU_ASSIGN_PTR(self, &nw->next, succ);
		RLU_ASSIGN_PTR(self, &nw->prev, pos);
		RLU_ASSIGN_PTR(self, &pos->next, nw);
		RLU_ASSIGN_PTR(self, &succ->prev, nw);
		RLU_ASSIGN_PTR(self, &ic->target, nw);
	} else {				/* full slot -> unlink the named cell, clear it */
		struct rlu_lnode *prev, *succ;

		/* Lock the target cell FIRST.  Holding cur locked freezes cur->prev and
		 * cur->next, so the neighbours we read are its true current neighbours,
		 * and neither can be unlinked (that would require locking cur->prev's or
		 * cur->next's edge into cur -> conflicts with our lock) or freed before we
		 * lock them.  Reading the neighbours first, as in the disjoint-slot churn
		 * path, only stays safe there because no peer touches the same node. */
		if (!RLU_TRY_LOCK(self, &cur))  { RLU_ABORT(self); goto retry; }
		prev = (struct rlu_lnode *) RLU_DEREF(self, cur->prev);
		succ = (struct rlu_lnode *) RLU_DEREF(self, cur->next);
		if (!RLU_TRY_LOCK(self, &prev)) { RLU_ABORT(self); goto retry; }
		if (!RLU_TRY_LOCK(self, &succ)) { RLU_ABORT(self); goto retry; }
		RLU_ASSIGN_PTR(self, &prev->next, succ);
		RLU_ASSIGN_PTR(self, &succ->prev, prev);
		RLU_ASSIGN_PTR(self, &ic->target, NULL);
		RLU_FREE(self, cur);
	}
	RLU_READER_UNLOCK(self);
}

/* Clear every index cell (single-threaded, between sweep points): unlink each
 * named cell AND null its slot in one commit, like the writer's delete path. */
static void rlu_reset_random(void)
{
	rlu_thread_data_t *self = rlu_self();
	int i;

	for (i = 0; i < g_rlu_nindex; i++) {
		struct rlu_icell *ic0 = g_rlu_index[i], *ic;
		struct rlu_lnode *cur, *prev, *succ;
retry:
		RLU_READER_LOCK(self);
		ic = ic0;
		if (!RLU_TRY_LOCK(self, &ic)) { RLU_ABORT(self); goto retry; }
		cur = ic->target;
		if (cur == NULL) { RLU_READER_UNLOCK(self); continue; }
		/* Lock the cell first, then read its neighbours from the locked copy --
		 * same lock-before-read discipline as rlu_write_random (single-threaded
		 * here, but kept consistent). */
		if (!RLU_TRY_LOCK(self, &cur))  { RLU_ABORT(self); goto retry; }
		prev = (struct rlu_lnode *) RLU_DEREF(self, cur->prev);
		succ = (struct rlu_lnode *) RLU_DEREF(self, cur->next);
		if (!RLU_TRY_LOCK(self, &prev)) { RLU_ABORT(self); goto retry; }
		if (!RLU_TRY_LOCK(self, &succ)) { RLU_ABORT(self); goto retry; }
		RLU_ASSIGN_PTR(self, &prev->next, succ);
		RLU_ASSIGN_PTR(self, &succ->prev, prev);
		RLU_ASSIGN_PTR(self, &ic->target, NULL);
		RLU_FREE(self, cur);
		RLU_READER_UNLOCK(self);
	}
}

/* ════════════════════════════════════════════════════════════════
 * Hash-of-lists (Phase 5 re-home): RLU's OWN showcase structure -- a hash
 * table of sorted singly-linked bucket lists -- brought into THIS harness so
 * rcu-txn and RLU meet on RLU's home turf under identical pinning, warm-up,
 * timing and workload.  Singly-linked (not bidir) buckets on purpose: that is
 * RLU's native form; bidir buckets would tax RLU an extra prev edge per op it
 * would never pay in the paper.  Each engine uses its required allocator
 * (RLU_ALLOC for RLU, malloc+call_rcu for rcu-txn) -- an inherent asymmetry.
 *
 * Workload (writer-only-safe knobs, env-overridable): HL_BUCKETS buckets,
 * HL_INIT initial keys drawn from [0,HL_RANGE) (HL_RANGE = 2*INIT => ~50%
 * occupancy, steady under toggle), HL_BATCH lookups per read pass.  A reader
 * pass does HL_BATCH random key lookups (checking per-bucket sortedness into
 * *viol); a writer op toggles a random key (present -> remove, absent -> add),
 * so the population stays ~HL_INIT without an explicit reset.  Both bucket
 * lists carry LONG_MIN head and LONG_MAX tail sentinels so search needs no NULL
 * or empty-bucket special cases.
 * ════════════════════════════════════════════════════════════════ */
static int HL_BUCKETS = 1000;
static int HL_INIT    = 100000;
static int HL_RANGE   = 200000;
static int HL_BATCH   = 16;

static void hl_config(void)
{
	const char *s;
	if ((s = getenv("HL_BUCKETS"))) HL_BUCKETS = atoi(s);
	if ((s = getenv("HL_INIT")))    HL_INIT    = atoi(s);
	if ((s = getenv("HL_RANGE")))   HL_RANGE   = atoi(s);
	if ((s = getenv("HL_BATCH")))   HL_BATCH   = atoi(s);
	if (HL_BUCKETS < 1) HL_BUCKETS = 1;
	if (HL_RANGE   < 2) HL_RANGE   = 2;
	if (HL_BATCH   < 1) HL_BATCH   = 1;
}

static __thread uint64_t g_hl_rng;
static inline uint64_t hl_rng(void)
{
	if (!g_hl_rng)			/* per-thread seed: address-diverse, non-zero */
		g_hl_rng = 0x9e3779b97f4a7c15ULL ^ (uint64_t) (uintptr_t) &g_hl_rng;
	return g_hl_rng;
}
static inline unsigned hl_hash(long key)
{
	return (unsigned) ((unsigned long) key % (unsigned) HL_BUCKETS);
}

/* ── txn_hlist: rcu-txn hash-of-lists on the SINGLE-POINTER-HEAD hlist ──
 *
 * Buckets are sorted urcu_txn_hlist chains (<urcu/rcu-txn-hlist.h>): a
 * kernel-hlist-shaped head that is ONE pointer (8 B) instead of the bidir list's
 * 16 B sentinel node, so the bucket array is half the footprint and twice as
 * dense per cache line -- matching rlu_hlist's 8 B head pointer for a fair
 * head-density comparison.  The escalation domain is NOT per bucket (that would
 * bloat the head): ONE domain serves the whole table (escalation is rare, so a
 * single shared lane costs nothing on the optimistic path and keeps the head at
 * 8 B).  Removal uses the engine's validated urcu_txn_hlist_del_rcu, which needs
 * only the node (pprev names the slot -- no bucket lookup).  A sorted insert PINS
 * the exact validated successor via insert_at_slot_prepare: a concurrent insert
 * of a smaller key (or a delete at the slot) then changes *slot and aborts the
 * commit, so we retry -- which keeps the bucket sorted AND dedups (two adds of
 * the same key contend on the same slot; the loser re-finds it present). */
struct thl_node { struct urcu_txn_hlist_node node; long key; struct rcu_head rh; };
static struct urcu_txn_hlist_head *g_thl_bkt;	/* [HL_BUCKETS], 8 B each */
static struct urcu_txn_domain g_thl_dom;	/* one domain for the whole table */

static void thl_node_free(struct rcu_head *h)
{
	free(caa_container_of(h, struct thl_node, rh));
}
static struct thl_node *thl_alloc(long key)
{
	struct thl_node *n = malloc(sizeof(*n));
	if (!n) abort();
	n->key = key;
	return n;
}
/* Key of a chain node (hlist chains terminate on NULL, not a sentinel). */
static inline long thl_key(struct urcu_txn_hlist_node *p)
{
	return caa_container_of(p, struct thl_node, node)->key;
}
static void thl_build(void)
{
	uint64_t r = 0x1234567ULL;
	long inserted = 0;
	int i;

	hl_config();
	urcu_txn_domain_init(&g_thl_dom);
	g_thl_bkt = calloc((size_t) HL_BUCKETS, sizeof(*g_thl_bkt));
	for (i = 0; i < HL_BUCKETS; i++)
		urcu_txn_hlist_init(&g_thl_bkt[i]);
	while (inserted < HL_INIT) {			/* sorted initial fill, single-threaded */
		long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
		struct urcu_txn_hlist_head *head = &g_thl_bkt[hl_hash(key)];
		struct urcu_txn_hlist_node *pred = NULL, *cur;
		struct thl_node *n;

		rcu_read_lock();
		for (cur = urcu_txn_hlist_first_rcu(head); cur != NULL;
				cur = urcu_txn_hlist_next_rcu(cur)) {
			if (thl_key(cur) >= key) break;
			pred = cur;
		}
		rcu_read_unlock();
		if (cur != NULL && thl_key(cur) == key)
			continue;			/* dup */
		n = thl_alloc(key);
		if (pred == NULL) {			/* smallest key: insert at head */
			if (urcu_txn_hlist_add_rcu(&n->node, head, &g_thl_dom))
				abort();
		} else if (urcu_txn_hlist_insert_after_rcu(&n->node, pred,
				&g_thl_dom)) {
			abort();
		}
		inserted++;
	}
}
static int thl_contains(struct urcu_txn_hlist_head *head, long key, long *viol)
{
	struct urcu_txn_hlist_node *p = urcu_txn_hlist_first_rcu(head);
	long pk = LONG_MIN;
	int steps = 0;

	while (p != NULL) {
		long k = thl_key(p);
		if (k < pk || ++steps > STEP_LIMIT) { (*viol)++; return 0; }
		pk = k;
		if (k >= key) return k == key;
		p = urcu_txn_hlist_next_rcu(p);
	}
	return 0;
}
static unsigned long thl_read(long *viol)
{
	unsigned long found = 0;
	uint64_t r = hl_rng();
	int i;

	rcu_read_lock();
	for (i = 0; i < HL_BATCH; i++) {
		long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
		found += (unsigned long) thl_contains(&g_thl_bkt[hl_hash(key)], key, viol);
	}
	rcu_read_unlock();
	g_hl_rng = r;
	return found;
}
static void thl_write(int slot)
{
	uint64_t r = hl_rng();
	long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
	struct urcu_txn_hlist_head *head = &g_thl_bkt[hl_hash(key)];
	struct urcu_txn_hlist_node *p;
	struct thl_node *victim = NULL, *n;
	struct urcu_mcas_txn txn;
	int mem_err = 0;

	(void) slot;
	g_hl_rng = r;
	rcu_read_lock();
	/* Locate the key. */
	for (p = urcu_txn_hlist_first_rcu(head); p != NULL;
			p = urcu_txn_hlist_next_rcu(p)) {
		long k = thl_key(p);
		if (k >= key) {
			if (k == key)
				victim = caa_container_of(p, struct thl_node, node);
			break;
		}
	}
	if (victim) {					/* present -> remove (del needs no bucket: pprev) */
		if (urcu_txn_hlist_del_rcu(&victim->node, &g_thl_dom) == 1)
			call_rcu(&victim->rh, thl_node_free);
		rcu_read_unlock();
		return;
	}
	/* absent -> sorted insert pinning the EXACT validated successor. */
	n = thl_alloc(key);
	urcu_txn_init(&txn, &g_thl_dom);
	for (;;) {
		/* @slotp names the insertion point; @cur is the first node with
		 * key >= @key (or NULL), pinned by insert_at_slot's *slot old-value. */
		struct urcu_txn_hlist_node **slotp = &head->first;
		struct urcu_txn_hlist_node *pred = NULL, *cur;
		void *raw;
		enum urcu_txn_status st;
		int prep, restart = 0;

		urcu_txn_begin(&txn);
		raw = urcu_txn_load(&txn, (void **) slotp, URCU_MCAS_TAG);
		cur = urcu_txn_hlist_unmark(raw);	/* head->first: never marked */
		while (cur != NULL && thl_key(cur) < key) {
			pred = cur;
			slotp = &pred->next;
			raw = urcu_txn_load(&txn, (void **) slotp, URCU_MCAS_TAG);
			if (urcu_txn_hlist_is_marked(raw)) {	/* pred being deleted */
				restart = 1;
				break;
			}
			cur = (struct urcu_txn_hlist_node *) raw;
		}
		if (restart) {
			urcu_txn_conflict(&txn); urcu_txn_end(&txn); continue;
		}
		if (cur != NULL && thl_key(cur) == key) {	/* someone else added it */
			urcu_txn_end(&txn); free(n); rcu_read_unlock(); return;
		}
		/* Insert between @pred (or the head) and @cur, pinning *slotp == cur. */
		prep = urcu_txn_hlist_insert_at_slot_prepare(&txn, &n->node,
				slotp, cur);
		if (prep) {				/* -EAGAIN: succ mid-delete */
			urcu_txn_conflict(&txn); urcu_txn_end(&txn); continue;
		}
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (st == URCU_TXN_STATUS_OK)
			break;
		if (st == URCU_TXN_STATUS_ABORT)
			continue;
		if (++mem_err < 64)			/* poison (retry) vs real OOM (bounded) */
			continue;
		abort();
	}
	rcu_read_unlock();
}

/* ── rlu_hlist: RLU hash-of-lists (mirrors third_party/rlu hash-list.c) ── */
struct rlu_hnode { struct rlu_hnode *next; long key; };
static struct rlu_hnode **g_rhl_bkt;		/* [HL_BUCKETS] head sentinels */

static struct rlu_hnode *rhl_alloc(long key)
{
	struct rlu_hnode *n = (struct rlu_hnode *) RLU_ALLOC(sizeof(struct rlu_hnode));
	n->key = key; n->next = NULL;
	return n;
}
static void rhl_build(void)
{
	const char *ws = getenv("BENCH_RLU_WS");
	uint64_t r = 0x1234567ULL;
	long inserted = 0;
	int i;

	if (ws) { g_rlu_ws = atoi(ws); if (g_rlu_ws < 1) g_rlu_ws = 1; }
	hl_config();
	rlu_init(RLU_TYPE_FINE_GRAINED, g_rlu_ws);
	g_rlu_pool = calloc(RLU_POOL_MAX, sizeof(*g_rlu_pool));
	g_rhl_bkt = calloc((size_t) HL_BUCKETS, sizeof(*g_rhl_bkt));
	for (i = 0; i < HL_BUCKETS; i++) {
		struct rlu_hnode *head = rhl_alloc(LONG_MIN);
		struct rlu_hnode *tail = rhl_alloc(LONG_MAX);
		head->next = tail; tail->next = tail;
		g_rhl_bkt[i] = head;
	}
	while (inserted < HL_INIT) {
		long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
		struct rlu_hnode *pred = g_rhl_bkt[hl_hash(key)], *cur = pred->next, *n;
		while (cur->key < key) { pred = cur; cur = cur->next; }
		if (cur->key == key) continue;
		n = rhl_alloc(key); n->next = cur; pred->next = n; inserted++;
	}
}
static int rhl_contains(rlu_thread_data_t *self, struct rlu_hnode *head, long key, long *viol)
{
	struct rlu_hnode *pred = (struct rlu_hnode *) RLU_DEREF(self, head);
	struct rlu_hnode *cur  = (struct rlu_hnode *) RLU_DEREF(self, pred->next);
	long pk = LONG_MIN;
	int steps = 0;

	while (cur->key < key) {
		if (cur->key < pk || ++steps > STEP_LIMIT) { (*viol)++; return 0; }
		pk = cur->key; pred = cur;
		cur = (struct rlu_hnode *) RLU_DEREF(self, pred->next);
	}
	return cur->key == key;
}
static unsigned long rhl_read(long *viol)
{
	rlu_thread_data_t *self = rlu_self();
	unsigned long found = 0;
	uint64_t r = hl_rng();
	int i;

	RLU_READER_LOCK(self);
	for (i = 0; i < HL_BATCH; i++) {
		long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
		found += (unsigned long) rhl_contains(self, g_rhl_bkt[hl_hash(key)], key, viol);
	}
	RLU_READER_UNLOCK(self);
	g_hl_rng = r;
	return found;
}
static void rhl_write(int slot)
{
	rlu_thread_data_t *self = rlu_self();
	uint64_t r = hl_rng();
	long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
	struct rlu_hnode *head = g_rhl_bkt[hl_hash(key)], *pred, *cur;

	(void) slot;
	g_hl_rng = r;
restart:
	RLU_READER_LOCK(self);
	pred = (struct rlu_hnode *) RLU_DEREF(self, head);
	cur  = (struct rlu_hnode *) RLU_DEREF(self, pred->next);
	while (cur->key < key) {
		pred = cur;
		cur = (struct rlu_hnode *) RLU_DEREF(self, pred->next);
	}
	if (cur->key == key) {				/* present -> remove cur */
		struct rlu_hnode *n = (struct rlu_hnode *) RLU_DEREF(self, cur->next);
		if (!RLU_TRY_LOCK(self, &pred)) { RLU_ABORT(self); goto restart; }
		if (!RLU_TRY_LOCK(self, &cur))  { RLU_ABORT(self); goto restart; }
		RLU_ASSIGN_PTR(self, &pred->next, n);
		RLU_FREE(self, cur);
	} else {					/* absent -> add between pred and cur */
		struct rlu_hnode *nw;
		if (!RLU_TRY_LOCK(self, &pred)) { RLU_ABORT(self); goto restart; }
		if (!RLU_TRY_LOCK(self, &cur))  { RLU_ABORT(self); goto restart; }
		nw = rhl_alloc(key);
		RLU_ASSIGN_PTR(self, &nw->next, cur);
		RLU_ASSIGN_PTR(self, &pred->next, nw);
	}
	RLU_READER_UNLOCK(self);
}

/* ── rcu_hlist: classic RCU hash-of-sorted-lists (RCU reads + per-bucket lock) ──
 *
 * The article's actual baseline (LWN #667720): RCU-protected sorted singly-
 * linked buckets, readers wait-free under rcu_read_lock (rcu_dereference walk),
 * writers serialized PER BUCKET by a plain mutex (an update touches one bucket,
 * so contention is 1/HL_BUCKETS of a global lock).  Deletes publish the unlink
 * with rcu_assign_pointer and defer the free through call_rcu -- the honest RCU
 * grace-period cost.  Same LONG_MIN/LONG_MAX sentinels and key stream as the txn
 * and RLU hashes, so the three sorted-list schemes meet on identical ground. */
struct chl_node { struct chl_node *next; long key; struct rcu_head rh; };
static struct chl_node **g_chl_bkt;		/* [HL_BUCKETS] head sentinels */
static pthread_mutex_t   *g_chl_lock;		/* [HL_BUCKETS] per-bucket writer locks */

static void chl_node_free(struct rcu_head *h)
{
	free(caa_container_of(h, struct chl_node, rh));
}
static struct chl_node *chl_alloc(long key)
{
	struct chl_node *n = malloc(sizeof(*n));
	if (!n) abort();
	n->key = key; n->next = NULL;
	return n;
}
static void chl_build(void)
{
	uint64_t r = 0x1234567ULL;
	long inserted = 0;
	int i;

	hl_config();
	g_chl_bkt  = calloc((size_t) HL_BUCKETS, sizeof(*g_chl_bkt));
	g_chl_lock = calloc((size_t) HL_BUCKETS, sizeof(*g_chl_lock));
	for (i = 0; i < HL_BUCKETS; i++) {
		struct chl_node *head = chl_alloc(LONG_MIN);
		struct chl_node *tail = chl_alloc(LONG_MAX);
		head->next = tail; tail->next = tail;	/* tail self-loops (wrap guard) */
		g_chl_bkt[i] = head;
		pthread_mutex_init(&g_chl_lock[i], NULL);
	}
	while (inserted < HL_INIT) {			/* sorted single-threaded fill */
		long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
		struct chl_node *pred = g_chl_bkt[hl_hash(key)], *cur = pred->next, *n;
		while (cur->key < key) { pred = cur; cur = cur->next; }
		if (cur->key == key) continue;		/* dup */
		n = chl_alloc(key); n->next = cur; pred->next = n; inserted++;
	}
}
static int chl_contains(struct chl_node *head, long key, long *viol)
{
	struct chl_node *cur = rcu_dereference(head->next);
	long pk = LONG_MIN;
	int steps = 0;

	while (cur->key < key) {
		if (cur->key < pk || ++steps > STEP_LIMIT) { (*viol)++; return 0; }
		pk = cur->key;
		cur = rcu_dereference(cur->next);
	}
	return cur->key == key;
}
static unsigned long chl_read(long *viol)
{
	unsigned long found = 0;
	uint64_t r = hl_rng();
	int i;

	rcu_read_lock();
	for (i = 0; i < HL_BATCH; i++) {
		long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
		found += (unsigned long) chl_contains(g_chl_bkt[hl_hash(key)], key, viol);
	}
	rcu_read_unlock();
	g_hl_rng = r;
	return found;
}
static void chl_write(int slot)
{
	uint64_t r = hl_rng();
	long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
	unsigned b = hl_hash(key);
	struct chl_node *head = g_chl_bkt[b], *pred, *cur;

	(void) slot;
	g_hl_rng = r;
	pthread_mutex_lock(&g_chl_lock[b]);		/* one writer per bucket */
	pred = head; cur = pred->next;			/* writer-exclusive: plain loads */
	while (cur->key < key) { pred = cur; cur = cur->next; }
	if (cur->key == key) {				/* present -> unlink + defer free */
		rcu_assign_pointer(pred->next, cur->next);
		call_rcu(&cur->rh, chl_node_free);
	} else {					/* absent -> sorted insert */
		struct chl_node *n = chl_alloc(key);
		n->next = cur;
		rcu_assign_pointer(pred->next, n);
	}
	pthread_mutex_unlock(&g_chl_lock[b]);
}

/* ── lfht: liburcu cds_lfht, a split-ordered lock-free hash ──
 *
 * cds_lfht is normally run WITH auto-resize so its chains stay ~O(1).  We
 * instead PIN it to the SAME bucket count as the sorted-list schemes (HL_BUCKETS
 * rounded up to a power of two, which cds_lfht requires; no CDS_LFHT_AUTO_RESIZE)
 * so every engine walks the same ~100-node chain and the comparison isolates the
 * SYNCHRONIZATION mechanism -- split-ordered lock-free list vs RCU+per-bucket
 * lock vs RLU vs txn-MCAS -- rather than the bucket count.  This deliberately
 * denies cds_lfht its resize advantage: an apples-to-apples equal-chain match,
 * not cds_lfht at its intended O(1) design point. */
struct lfht_node { struct cds_lfht_node node; long key; struct rcu_head rh; };
static struct cds_lfht *g_lfht;

/* HL_BUCKETS rounded up to a power of two (cds_lfht bucket counts are 2^k). */
static unsigned long lfht_nbuckets(void)
{
	unsigned long b = 1;
	while (b < (unsigned long) HL_BUCKETS)
		b <<= 1;
	return b;
}

static inline unsigned long lfht_hash(long key)
{
	uint64_t h = (uint64_t) key;			/* fmix64 avalanche */
	h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	return (unsigned long) h;
}
static int lfht_match(struct cds_lfht_node *node, const void *key)
{
	return caa_container_of(node, struct lfht_node, node)->key == *(const long *) key;
}
static void lfht_free(struct rcu_head *h)
{
	free(caa_container_of(h, struct lfht_node, rh));
}
static void lfht_build(void)
{
	uint64_t r = 0x1234567ULL;
	long inserted = 0;
	unsigned long nb;

	hl_config();
	nb = lfht_nbuckets();
	/* Fix bucket count to the sorted schemes' (no auto-resize, no resize worker):
	 * min == init == max so cds_lfht never grows past nb buckets. */
	g_lfht = cds_lfht_new(nb, nb, nb, 0, NULL);
	if (!g_lfht) abort();
	while (inserted < HL_INIT) {
		long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
		struct lfht_node *n = malloc(sizeof(*n));
		struct cds_lfht_node *ret;
		if (!n) abort();
		n->key = key; cds_lfht_node_init(&n->node);
		rcu_read_lock();
		ret = cds_lfht_add_unique(g_lfht, lfht_hash(key), lfht_match, &key, &n->node);
		rcu_read_unlock();
		if (ret != &n->node) { free(n); continue; }	/* dup */
		inserted++;
	}
}
static unsigned long lfht_read(long *viol)
{
	unsigned long found = 0;
	uint64_t r = hl_rng();
	int i;

	(void) viol;					/* unsorted: no monotonicity check */
	rcu_read_lock();
	for (i = 0; i < HL_BATCH; i++) {
		long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
		struct cds_lfht_iter iter;
		cds_lfht_lookup(g_lfht, lfht_hash(key), lfht_match, &key, &iter);
		found += (cds_lfht_iter_get_node(&iter) != NULL);
	}
	rcu_read_unlock();
	g_hl_rng = r;
	return found;
}
static void lfht_write(int slot)
{
	uint64_t r = hl_rng();
	long key = (long) (xorshift64(&r) % (uint64_t) HL_RANGE);
	unsigned long h = lfht_hash(key);
	struct cds_lfht_iter iter;
	struct cds_lfht_node *found;

	(void) slot;
	g_hl_rng = r;
	rcu_read_lock();
	cds_lfht_lookup(g_lfht, h, lfht_match, &key, &iter);
	found = cds_lfht_iter_get_node(&iter);
	if (found) {					/* present -> remove + defer free */
		if (!cds_lfht_del(g_lfht, found))
			call_rcu(&caa_container_of(found, struct lfht_node, node)->rh,
				 lfht_free);
	} else {					/* absent -> add (unique) */
		struct lfht_node *n = malloc(sizeof(*n));
		if (!n) abort();
		n->key = key; cds_lfht_node_init(&n->node);
		if (cds_lfht_add_unique(g_lfht, h, lfht_match, &key, &n->node) != &n->node)
			free(n);			/* lost the add race to a dup */
	}
	rcu_read_unlock();
}

/* ── Engine registry ─────────────────────────────────────────── */
static const struct lengine engines[] = {
	{ "txn_sw_list",  "RCU single-updater, coherent bidir", 1, su_build,  su_read,  su_write  },
	{ "txn_list",  "RCU concurrent, coherent bidir",      1, lf_build,  lf_read,  lf_write, lf_write_random, lf_reset_index },
	{ "rlu_list",  "RLU (Read-Log-Update), coherent bidir", 0, rlu_build, rlu_read, rlu_write,
		rlu_write_random, rlu_reset_random, rlu_point_reset, rlu_tl_begin, rlu_tl_end },
	{ "txn_hlist", "rcu-txn hash-of-sorted-lists",       1, thl_build, thl_read, thl_write },
	{ "rlu_hlist", "RLU hash-of-sorted-lists (home turf)", 0, rhl_build, rhl_read, rhl_write,
		NULL, NULL, rlu_point_reset, rlu_tl_begin, rlu_tl_end },
	{ "rcu_hlist", "RCU + per-bucket lock hash-of-sorted-lists", 1, chl_build, chl_read, chl_write },
	{ "lfht",      "liburcu cds_lfht (resizable lock-free hash)", 1, lfht_build, lfht_read, lfht_write },
	{ "rculist",   "RCU classic, forward-only (ref)",    1, rl_build,  rl_read,  rl_write  },
	{ "mutex",     "pthread_mutex",                      0, mtx_build, mtx_read, mtx_write },
	{ "fairmutex", "liburcu cds_fair_mutex (MCS/FIFO)",  0, fm_build,  fm_read,  fm_write  },
	{ "rwlock_r",  "pthread_rwlock, reader-preferring",  0, rw_build_reader, rw_read, rw_write },
	{ "rwlock_w",  "pthread_rwlock, writer-preferring",  0, rw_build_writer, rw_read, rw_write },
	{ "iscrw",     "bind9 isc_rwlock (C-RW-WP)",         0, isc_build, isc_read, isc_write },
	{ "seqlock",   "seqlock + type-stable nodes",        0, seq_build, seq_read, seq_write },
};
#define NR_ENGINES ((int)(sizeof(engines) / sizeof(engines[0])))

static const struct lengine *g_eng;

/* ── Worker threads ───────────────────────────────────────────── */
struct reader_arg { unsigned long visits; long viol; int cpu; };
struct writer_arg { unsigned long writes; int wid; int nwriters; int cpu;
		    uint64_t *lat; uint64_t lat_max; };

static void *reader_thread(void *arg)
{
	struct reader_arg *ra = arg;
	unsigned long visits = 0;
	long viol = 0;

	bench_topology_pin(ra->cpu);
	if (g_eng->uses_rcu)
		rcu_register_thread();
	if (g_eng->tl_begin)
		g_eng->tl_begin();			/* engine-private thread registration (RLU) */

	if (getenv("BENCH_NO_PRIME") == NULL)
		(void) g_eng->read_pass(&viol);		/* one warm pass */

	__atomic_fetch_add(&prime_done_count, 1, __ATOMIC_RELEASE);
	while (!start_flag)
		caa_cpu_relax();

	while (!uatomic_load(&stop_flag, CMM_RELAXED)) {
		visits += g_eng->read_pass(&viol);
		if (g_eng->uses_rcu)
			rcu_quiescent_state();
	}

	if (g_eng->tl_end)
		g_eng->tl_end();			/* flush this thread's deferred RLU write-sets */
	if (g_eng->uses_rcu)
		rcu_unregister_thread();
	ra->visits = visits;
	ra->viol = viol;
	return NULL;
}

static void *writer_thread(void *arg)
{
	struct writer_arg *wa = arg;
	unsigned long writes = 0;
	int nw = wa->nwriters, wid = wa->wid;
	/* This writer owns churn slots { wid, wid+nw, wid+2nw, ... }. */
	int first = wid, m = 0;
	/*
	 * BENCH_WRITE_RATE=N throttles each writer to N toggles/s (absolute-time
	 * pacing), so every engine faces the SAME mutation rate.  This separates
	 * read scaling from the writer's own speed -- a cheap/fast writer otherwise
	 * dirties reader cachelines more and depresses read throughput, which is an
	 * engine property, not a read-path property.  Unset => writer runs flat out.
	 */
	const char *wr = getenv("BENCH_WRITE_RATE");
	long rate = wr ? atol(wr) : 0;
	uint64_t period_ns = rate > 0 ? 1000000000ULL / (uint64_t) rate : 0;
	uint64_t base;
	/* Per-writer PRNG seed for BENCH_RANDOM_POS (distinct, non-zero per wid). */
	uint64_t rng = (0x9e3779b97f4a7c15ULL ^ ((uint64_t) wid * 0x100000001b3ULL)) | 1;
	int random_pos = g_random_pos && g_eng->write_random != NULL;

	bench_topology_pin(wa->cpu);
	TP_SET_WID(wid);
	if (g_eng->uses_rcu)
		rcu_register_thread();
	if (g_eng->tl_begin)
		g_eng->tl_begin();			/* engine-private thread registration (RLU) */
	if (g_lat)
		wa->lat = calloc(LAT_NBUCKETS, sizeof(*wa->lat));	/* thread-local, NUMA-local */

	while (!start_flag)
		caa_cpu_relax();
	base = mono_ns();

	while (!uatomic_load(&stop_flag, CMM_RELAXED)) {
		uint64_t t0 = g_lat ? mono_ns() : 0;
		if (random_pos) {
			g_eng->write_random(&rng);
		} else {
			int slot = first + m * nw;
			if (slot >= CHURN) { m = 0; slot = first; }
			g_eng->write_toggle(slot);
			m++;
		}
		if (g_lat && wa->lat) {
			uint64_t d = mono_ns() - t0;
			wa->lat[lat_bucket(d)]++;
			if (d > wa->lat_max)
				wa->lat_max = d;
		}
		writes++;
		if (g_eng->uses_rcu) {
			rcu_quiescent_state();
			TP_QS();
		}
		if (period_ns) {
			uint64_t target = base + writes * period_ns;
			struct timespec ts = { (time_t)(target / 1000000000ULL),
					(long)(target % 1000000000ULL) };
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		}
	}

	if (g_eng->tl_end)
		g_eng->tl_end();			/* flush this thread's deferred RLU write-sets */
	if (g_eng->uses_rcu)
		rcu_unregister_thread();
	wa->writes = writes;
	return NULL;
}

/* Restore the churn set to empty between sweep points (called single-threaded
 * by main; main is registered/online so RCU frees and barrier work).  For an
 * engine with its own SMR (RLU), main registers here via tl_begin and flushes
 * its deferred write-sets via tl_end, so no churn node is left locked before
 * point_reset rewinds the registry at the next point. */
static void reset_churn(void)
{
	int j;
	if (g_eng->tl_begin)
		g_eng->tl_begin();
	for (j = 0; j < CHURN; j++)
		if (g_present[j])
			g_eng->write_toggle(j);		/* present -> delete */
	if (g_random_pos && g_eng->reset_random)
		g_eng->reset_random();			/* clear transacted-index cells */
	if (g_eng->uses_rcu) {
		rcu_quiescent_state();
		rcu_barrier();				/* drain deferred frees */
	}
	if (g_eng->tl_end)
		g_eng->tl_end();
}

static void run_point(int nr_readers, int nr_writers,
		double *read_mvps, double *write_mops, long *violations)
{
	int total = nr_readers + nr_writers, i;
	pthread_t *th = calloc(total ? total : 1, sizeof(*th));
	struct reader_arg *ra = calloc(nr_readers ? nr_readers : 1, sizeof(*ra));
	struct writer_arg *wa = calloc(nr_writers ? nr_writers : 1, sizeof(*wa));
	unsigned long tot_visits = 0, tot_writes = 0;
	long tot_viol = 0;
	uint64_t t0, t1;
	double elapsed;

	start_flag = 0;
	stop_flag = 0;
	prime_done_count = 0;

	if (g_eng->point_reset)
		g_eng->point_reset();			/* rewind engine thread registry (RLU) */

	for (i = 0; i < nr_readers; i++) {
		ra[i].cpu = i;
		pthread_create(&th[i], NULL, reader_thread, &ra[i]);
	}
	for (i = 0; i < nr_writers; i++) {
		wa[i].wid = i;
		wa[i].nwriters = nr_writers;
		wa[i].cpu = nr_readers + i;	/* writers past the readers */
		pthread_create(&th[nr_readers + i], NULL, writer_thread, &wa[i]);
	}

	/* Wait until readers finished priming and are spinning on start_flag.
	 * main blocks here, so it must be RCU-offline: an online thread that
	 * stops quiescing stalls every grace period (and thus call_rcu reclaim
	 * AND any writer's commit-time synchronize_rcu) for the whole wait. */
	if (g_eng->uses_rcu)
		rcu_thread_offline();
	while (__atomic_load_n(&prime_done_count, __ATOMIC_ACQUIRE) < nr_readers)
		usleep(1000);
	if (g_eng->uses_rcu)
		rcu_thread_online();

	t0 = mono_ns();
	__atomic_store_n(&start_flag, 1, __ATOMIC_RELEASE);

	/* Keep main online but quiescing so call_rcu grace periods advance and
	 * deferred frees stay bounded during the timed window. */
	{
		uint64_t end = t0 + (uint64_t) DURATION_SEC * 1000000000ULL;
		while (mono_ns() < end) {
			usleep(2000);
			if (g_eng->uses_rcu)
				rcu_quiescent_state();
		}
	}
	__atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);

	/* main blocks in join while workers drain their last ops -- some of which
	 * are committing through synchronize_rcu.  Stay RCU-offline across the join
	 * so those grace periods can complete; otherwise main (online, no longer
	 * quiescing) deadlocks the writers waiting on it. */
	if (g_eng->uses_rcu)
		rcu_thread_offline();
	for (i = 0; i < total; i++)
		pthread_join(th[i], NULL);
	if (g_eng->uses_rcu)
		rcu_thread_online();
	t1 = mono_ns();
	elapsed = (double)(t1 - t0) / 1e9;

	for (i = 0; i < nr_readers; i++) {
		tot_visits += ra[i].visits;
		tot_viol += ra[i].viol;
	}
	for (i = 0; i < nr_writers; i++)
		tot_writes += wa[i].writes;

	if (g_lat && nr_writers > 0) {
		uint64_t merged[LAT_NBUCKETS], tot = 0, mx = 0;
		int b;
		memset(merged, 0, sizeof(merged));
		for (i = 0; i < nr_writers; i++) {
			if (!wa[i].lat)
				continue;
			for (b = 0; b < LAT_NBUCKETS; b++) {
				merged[b] += wa[i].lat[b];
				tot += wa[i].lat[b];
			}
			if (wa[i].lat_max > mx)
				mx = wa[i].lat_max;
			free(wa[i].lat);
		}
		if (tot)
			fprintf(stderr, "LAT w=%d n=%llu p50=%llu p90=%llu p99=%llu "
				"p999=%llu p9999=%llu max=%llu ns\n", nr_writers,
				(unsigned long long) tot,
				(unsigned long long) lat_pct(merged, tot, 0.50),
				(unsigned long long) lat_pct(merged, tot, 0.90),
				(unsigned long long) lat_pct(merged, tot, 0.99),
				(unsigned long long) lat_pct(merged, tot, 0.999),
				(unsigned long long) lat_pct(merged, tot, 0.9999),
				(unsigned long long) mx);
	}

	*read_mvps = (double) tot_visits / elapsed / 1e6;
	*write_mops = (double) tot_writes / elapsed / 1e6;
	*violations = tot_viol;

	reset_churn();
	free(th); free(ra); free(wa);
}

/*
 * Mixed per-thread workload (BENCH_UPDATE_PCT=X), the LWN #667720 / RLU-paper
 * shape: every thread runs the SAME loop, doing an update with probability X%
 * (else a lookup) rather than the harness's dedicated reader/writer split.  One
 * op = one lookup or one update (HL_BATCH is forced to 1 in this mode so a read
 * op is a single key lookup), matching the article's per-operation accounting.
 * Reported metric is total ops/s (lookups + updates).  Intended for the hash
 * engines (txn_hlist / rlu_hlist / rcu_hlist / lfht).
 */
static int g_update_pct = -1;
struct mixed_arg { unsigned long reads, writes; long viol; int cpu; int tid; };

static void *mixed_thread(void *arg)
{
	struct mixed_arg *ma = arg;
	unsigned long reads = 0, writes = 0;
	long viol = 0;
	uint64_t rng = (0x9e3779b97f4a7c15ULL ^ ((uint64_t) ma->tid * 0x100000001b3ULL)) | 1;

	bench_topology_pin(ma->cpu);
	if (g_eng->uses_rcu)
		rcu_register_thread();
	if (g_eng->tl_begin)
		g_eng->tl_begin();			/* engine-private thread registration (RLU) */

	if (getenv("BENCH_NO_PRIME") == NULL)
		(void) g_eng->read_pass(&viol);		/* one warm pass */

	__atomic_fetch_add(&prime_done_count, 1, __ATOMIC_RELEASE);
	while (!start_flag)
		caa_cpu_relax();

	while (!uatomic_load(&stop_flag, CMM_RELAXED)) {
		if ((int) (xorshift64(&rng) % 100u) < g_update_pct) {
			g_eng->write_toggle(0);		/* hash update: random key, slot ignored */
			writes++;
		} else {
			(void) g_eng->read_pass(&viol);	/* one lookup (HL_BATCH == 1 here) */
			reads++;
		}
		if (g_eng->uses_rcu)
			rcu_quiescent_state();
	}

	if (g_eng->tl_end)
		g_eng->tl_end();			/* flush this thread's deferred RLU write-sets */
	if (g_eng->uses_rcu)
		rcu_unregister_thread();
	ma->reads = reads; ma->writes = writes; ma->viol = viol;
	return NULL;
}

static void run_point_mixed(int nthreads, double *ops_mps, double *upd_mops,
		long *violations)
{
	pthread_t *th = calloc(nthreads ? nthreads : 1, sizeof(*th));
	struct mixed_arg *ma = calloc(nthreads ? nthreads : 1, sizeof(*ma));
	unsigned long tot_reads = 0, tot_writes = 0;
	long tot_viol = 0;
	uint64_t t0, t1;
	double elapsed;
	int i;

	start_flag = 0;
	stop_flag = 0;
	prime_done_count = 0;

	if (g_eng->point_reset)
		g_eng->point_reset();			/* rewind engine thread registry (RLU) */

	for (i = 0; i < nthreads; i++) {
		ma[i].cpu = i;
		ma[i].tid = i;
		pthread_create(&th[i], NULL, mixed_thread, &ma[i]);
	}

	/* Stay RCU-offline while blocking on prime / join (see run_point rationale). */
	if (g_eng->uses_rcu)
		rcu_thread_offline();
	while (__atomic_load_n(&prime_done_count, __ATOMIC_ACQUIRE) < nthreads)
		usleep(1000);
	if (g_eng->uses_rcu)
		rcu_thread_online();

	t0 = mono_ns();
	__atomic_store_n(&start_flag, 1, __ATOMIC_RELEASE);
	{
		uint64_t end = t0 + (uint64_t) DURATION_SEC * 1000000000ULL;
		while (mono_ns() < end) {
			usleep(2000);
			if (g_eng->uses_rcu)
				rcu_quiescent_state();
		}
	}
	__atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);

	if (g_eng->uses_rcu)
		rcu_thread_offline();
	for (i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	if (g_eng->uses_rcu)
		rcu_thread_online();
	t1 = mono_ns();
	elapsed = (double)(t1 - t0) / 1e9;

	for (i = 0; i < nthreads; i++) {
		tot_reads += ma[i].reads;
		tot_writes += ma[i].writes;
		tot_viol += ma[i].viol;
	}
	*ops_mps = (double) (tot_reads + tot_writes) / 1e6 / elapsed;
	*upd_mops = (double) tot_writes / 1e6 / elapsed;
	*violations = tot_viol;

	reset_churn();
	free(th); free(ma);
}

/* ── Self-check: build is sorted & coherent both directions ──── */
static void self_check(void)
{
	long viol = 0;
	unsigned long vis;
	int j;

	/* Toggle a handful of churn slots on, then walk. */
	for (j = 0; j < CHURN; j += 3)
		g_eng->write_toggle(j);
	vis = g_eng->read_pass(&viol);
	if (viol != 0) {
		fprintf(stderr, "%s: SELF-CHECK FAILED (%ld monotonicity violations)\n",
			g_eng->name, viol);
		exit(2);
	}
	fprintf(stderr, "%s: self-check OK (%lu nodes visited fwd+rev, sorted both ways)\n",
		g_eng->name, vis);
	reset_churn();
}

static void usage(const char *p)
{
	int i;
	fprintf(stderr, "usage: %s <engine> [max_threads]\n  engines:", p);
	for (i = 0; i < NR_ENGINES; i++)
		fprintf(stderr, " %s", engines[i].name);
	fprintf(stderr, "\n  env: LIST_SIZE CHURN DURATION_SEC BENCH_NO_WRITER "
		"BENCH_WRITESCALE BENCH_RW_BALANCED BENCH_READERS\n"
		"       BENCH_FIXED_READERS=N BENCH_WRITE_RATE=N(toggles/s/writer) "
		"BENCH_ALLOW_SMT\n"
		"       BENCH_UPDATE_PCT=X (mixed hash: every thread does X%% updates, "
		"LWN #667720) HL_BUCKETS/HL_INIT/HL_RANGE/HL_BATCH\n"
		"       BENCH_NO_PRIME BENCH_NUMA_INTERLEAVE "
		"BENCH_NO_PERCPU_CALLRCU\n"
		"  build: default = segregated rcu_head (24B hot node);"
		" -DLIST_RCU_INLINE_RCU_HEAD = inline (40B) artifact build\n");
}

int main(int argc, char **argv)
{
	int max_threads = 384, i;
	int max_phys_cores = 0;
	int allow_smt;
	const char *e;

#ifdef BENCH_LTTNG
	__sanitizer_set_death_callback(bench_on_death);	/* dump snapshot on ASan abort */
#ifdef URCU_MCAS_STORE_TRACE
	urcu_mcas_store_hook = bench_store_hook;	/* engine structural-store trace */
#endif
#endif

	if (argc < 2) { usage(argv[0]); return 1; }
	for (i = 0; i < NR_ENGINES; i++)
		if (strcmp(argv[1], engines[i].name) == 0) { g_eng = &engines[i]; break; }
	if (!g_eng) { usage(argv[0]); return 1; }
	if (argc > 2) max_threads = atoi(argv[2]);

	if ((e = getenv("LIST_SIZE")))     LIST_SIZE = atoi(e);
	if ((e = getenv("CHURN")))         CHURN = atoi(e);
	if ((e = getenv("DURATION_SEC")))  DURATION_SEC = atoi(e);
	if (CHURN > LIST_SIZE) CHURN = LIST_SIZE;
	STEP_LIMIT = (LIST_SIZE + CHURN) + 64;

	/* Churn schedule: unique, spread-out stable anchors.  CHURN=0 is allowed
	 * (a pure static list for the read-only cache-footprint experiments). */
	{
		int stride = CHURN > 0 ? LIST_SIZE / CHURN : 1;
		size_t nch = CHURN > 0 ? (size_t) CHURN : 1;
		if (stride < 1) stride = 1;
		g_anchor = malloc(nch * sizeof(*g_anchor));
		g_present = calloc(nch, sizeof(*g_present));
		for (i = 0; i < CHURN; i++)
			g_anchor[i] = (i * stride) % LIST_SIZE;
	}

	bench_topology_init();

	/*
	 * Physical-core count: worker indices 0..ncores-1 are pinned one-per-core
	 * (no SMT sibling sharing).  We keep every sweep at or below ncores total
	 * workers so writers always land on their own cores -- set BENCH_ALLOW_SMT
	 * to deliberately oversubscribe into the SMT siblings.
	 */
	{
		int nc = bench_topology_ncores();
		if (nc <= 0) nc = (int) sysconf(_SC_NPROCESSORS_ONLN);
		max_phys_cores = nc;
	}

	/* NUMA interleave (on by default): spread the shared list across nodes so
	 * no single node's bandwidth bottlenecks high reader counts. */
	{
		const char *ni = getenv("BENCH_NUMA_INTERLEAVE");
		if ((!ni || ni[0] != '0') && numa_available() != -1) {
			numa_set_interleave_mask(numa_all_nodes_ptr);
			fprintf(stderr, "NUMA: interleaving allocations across all nodes\n");
		}
	}

	if (g_eng->uses_rcu) {
		rcu_register_thread();
		/*
		 * Per-CPU call_rcu reclaim workers, ON BY DEFAULT
		 * (BENCH_NO_PERCPU_CALLRCU to disable).  Spawn one reclaim worker per
		 * CPU instead of liburcu's single default worker, so call_rcu() routes
		 * each deferred free to the worker for the caller's CPU: reclamation is
		 * distributed and CPU-local rather than funnelled through one thread
		 * (which otherwise caps concurrent writer throughput at ~8 Mops/s and is
		 * the worst case for any thread-caching allocator's cross-thread free).
		 * Pays off most when the allocator is ALSO per-CPU (tcmalloc rseq caches
		 * or jemalloc MALLOC_CONF=percpu_arena:phycpu), so a node freed on CPU X
		 * returns to the same pool the writer on CPU X allocates from.
		 */
		reclaim_domain_build();		/* also indexes the per-CPU slab below */
		if (getenv("BENCH_NO_PERCPU_CALLRCU") == NULL) {
			if (reclaim_workers_setup())
				fprintf(stderr, "per-domain call_rcu: setup failed (%m); using single default worker\n");
			else
				fprintf(stderr, "per-domain call_rcu workers enabled "
					"(domain=%s%s; BENCH_RECLAIM_DOMAIN=hwthread|core|l3|single, BENCH_NO_PERCPU_CALLRCU)\n",
					rdl_name(g_rdl),
					g_rdl_unconfined ? ",unconfined" : "");
		}
#ifdef LIST_RCU_INLINE_RCU_HEAD
		/*
		 * Per-CPU slab node allocator (prototype) for txn_list's churn nodes.
		 * `make PCPU=1` compiles this in (-DLIST_RCU_INLINE_RCU_HEAD) and also
		 * defines BENCH_PCPU_ALLOC_DEFAULT so the slab is ON out of the box -- a
		 * reproducible pooled build.  BENCH_NO_PCPU_ALLOC forces glibc malloc for
		 * an in-binary A/B; BENCH_PCPU_ALLOC still force-enables it when only
		 * -DLIST_RCU_INLINE_RCU_HEAD was set by hand (no PCPU=1 default).
		 */
#ifdef BENCH_PCPU_ALLOC_DEFAULT
		if (getenv("BENCH_NO_PCPU_ALLOC") == NULL) {
#else
		if (getenv("BENCH_PCPU_ALLOC")) {
#endif
			/* One engine per process, but size to the larger node so
			 * either the concurrent (lf_elem) or single-updater
			 * (su_elem) churn engine can draw from the same slab. */
			size_t nsz = sizeof(struct lf_elem);
			if (sizeof(struct su_elem) > nsz)
				nsz = sizeof(struct su_elem);
			g_pcpu_alloc = 1;
			g_slab = pcpu_slab_create(nsz);
			fprintf(stderr, "per-CPU slab node allocator enabled "
				"(obj=%zu B, %d arenas)\n", g_slab->obj, g_slab->ncpu);
		}
#endif
	}

	/* Mixed-% mode accounts one lookup per read op, so a read pass must be a
	 * single key lookup: force HL_BATCH=1 (unless the user pinned it). */
	if (getenv("BENCH_UPDATE_PCT") && !getenv("HL_BATCH"))
		setenv("HL_BATCH", "1", 1);

	g_eng->build();
	self_check();

	fprintf(stderr, "%s (%s): LIST_SIZE=%d CHURN=%d, RSS=%ld kB, %ds/point\n",
		g_eng->name, g_eng->label, LIST_SIZE, CHURN, get_rss_kb(), DURATION_SEC);
	printf("engine %s rss_kb %ld list_size %d churn %d\n",
		g_eng->name, get_rss_kb(), LIST_SIZE, CHURN);

	allow_smt = getenv("BENCH_ALLOW_SMT") != NULL;
	g_random_pos = getenv("BENCH_RANDOM_POS") != NULL;
	g_su_nolock = getenv("BENCH_SU_NOLOCK") != NULL;
	g_lat = getenv("BENCH_LATENCY") != NULL;
	if (g_su_nolock)
		fprintf(stderr, "txn_sw_list: writer mutex DISABLED (BENCH_SU_NOLOCK) -- "
			"raw single-updater cost; VALID ONLY with a single writer\n");
	if (g_random_pos && g_eng->write_random)
		fprintf(stderr, "%s: random-position writer mode "
			"(collisions ~ writers/LIST_SIZE)\n", g_eng->name);

	if ((e = getenv("BENCH_UPDATE_PCT"))) {
		/*
		 * Article-style mixed workload (LWN #667720): N threads, each doing
		 * (100-X)% lookups + X% updates on the 1000-bucket x 100-node hash.
		 * Sweep thread count 1 -> N; report total ops/s.  The 64-thread point
		 * is the article's box; we run to the full machine.
		 */
		int tc[] = {1,2,4,8,16,32,64,96,128,160,191,192};
		int n = sizeof(tc)/sizeof(tc[0]);
		int cap = allow_smt ? max_threads
			: (max_phys_cores < max_threads ? max_phys_cores : max_threads);
		g_update_pct = atoi(e);
		if (g_update_pct < 0) g_update_pct = 0;
		if (g_update_pct > 100) g_update_pct = 100;
		printf("# threads total_mops update_mops read_mops violations "
			"(update_pct=%d)\n", g_update_pct);
		fflush(stdout);
		const char *ft = getenv("BENCH_FIXED_THREADS");
		if (ft) {
			/* Single thread count (e.g. sweeping HL_BUCKETS at a fixed load
			 * for the write-contention study), not the thread sweep. */
			int nt = atoi(ft);
			double ops, upd; long v;
			if (nt >= 1 && nt <= cap) {
				run_point_mixed(nt, &ops, &upd, &v);
				printf("%d %.2f %.2f %.2f %ld\n", nt, ops, upd, ops - upd, v);
				fflush(stdout);
			}
			goto done;
		}
		for (i = 0; i < n; i++) {
			double ops, upd; long v;
			if (tc[i] > cap)
				break;
			run_point_mixed(tc[i], &ops, &upd, &v);
			printf("%d %.2f %.2f %.2f %ld\n", tc[i], ops, upd, ops - upd, v);
			fflush(stdout);
		}
		goto done;
	}

	if (getenv("BENCH_RW_BALANCED")) {
		/*
		 * Balanced 50/50 sweep: at each total thread count T, run T/2 readers
		 * and T/2 writers concurrently (each on its own physical core), so the
		 * structure takes simultaneous read and write pressure as the machine
		 * fills.  Shows how each engine handles a mixed workload: RCU readers
		 * never block (and txn_list's writers also scale), whereas the lock /
		 * seqlock engines serialize readers against writers.
		 */
		int tc[] = {2,4,8,16,32,64,96,128,160,192};
		int n = sizeof(tc)/sizeof(tc[0]);
		int cap = allow_smt ? max_threads
			: (max_phys_cores < max_threads ? max_phys_cores : max_threads);
		printf("# total readers writers read_mvisits write_mops violations\n");
		fflush(stdout);
		for (i = 0; i < n; i++) {
			int t = tc[i], rr = t / 2, ww = t - rr;
			double r, w; long v;
			if (t > cap)
				break;
			run_point(rr, ww, &r, &w, &v);
			printf("%d %d %d %.1f %.2f %ld\n", t, rr, ww, r, w, v);
			fflush(stdout);
		}
		goto done;
	}

	if (getenv("BENCH_WRITESCALE")) {
		/* Writer-scaling: readers fixed, writers 1 -> N, each on its own core. */
		int readers = getenv("BENCH_READERS") ? atoi(getenv("BENCH_READERS")) : 0;
		int wc[] = {1,2,4,8,16,32,64,96,128,160,191,192};
		int n = sizeof(wc)/sizeof(wc[0]);
		/* Cap total workers at the physical-core count (no SMT) unless asked. */
		int cap = allow_smt ? max_threads
			: (max_phys_cores < max_threads ? max_phys_cores : max_threads);
		printf("# writers read_mvisits write_mops violations "
			"(readers=%d, distinct cores)\n", readers);
		fflush(stdout);
		const char *fw = getenv("BENCH_FIXED_WRITERS");
		if (fw) {
			/* Single writer count (controlled experiments), not the sweep --
			 * lets us hold a collapsed point for CPU sampling / perf / trace. */
			int nw = atoi(fw);
			double r, w; long v;
			if (readers + nw <= cap) {
				run_point(readers, nw, &r, &w, &v);
				printf("%d %.1f %.2f %ld\n", nw, r, w, v);
				fflush(stdout);
			}
			goto done;
		}
		for (i = 0; i < n; i++) {
			double r, w; long v;
			if (readers + wc[i] > cap) break;
			run_point(readers, wc[i], &r, &w, &v);
			printf("%d %.1f %.2f %ld\n", wc[i], r, w, v);
			fflush(stdout);
		}
	} else {
		/* Read-scaling: readers 1 -> N, optional single writer.  When a writer
		 * is present we keep readers <= ncores-1 so the writer always pins to
		 * its own physical core (no SMT sibling shared with a reader). */
		int no_writer = getenv("BENCH_NO_WRITER") != NULL;
		int wslots = no_writer ? 0 : 1;
		int rc[] = {1,2,4,8,16,32,64,96,128,160,191,192,256,383};
		int n = sizeof(rc)/sizeof(rc[0]);
		const char *fr = getenv("BENCH_FIXED_READERS");
		printf("# readers read_mvisits write_mops violations (writers=%d)\n", wslots);
		fflush(stdout);
		if (fr) {
			/* Single reader count (controlled experiments), not the sweep. */
			int nt = atoi(fr);
			double r, w; long v;
			if (nt + wslots <= max_threads) {
				run_point(nt, wslots, &r, &w, &v);
				printf("%d %.1f %.2f %ld\n", nt, r, w, v);
				fflush(stdout);
			}
			goto done;
		}
		for (i = 0; i < n; i++) {
			double r, w; long v;
			/* Writer present: reserve it a core unless SMT explicitly allowed. */
			if (wslots && !allow_smt && rc[i] + wslots > max_phys_cores)
				break;
			if (rc[i] + wslots > max_threads)
				break;
			run_point(rc[i], wslots, &r, &w, &v);
			printf("%d %.1f %.2f %ld\n", rc[i], r, w, v);
			fflush(stdout);
		}
	}

done:
	if (g_eng->uses_rcu) {
		rcu_barrier();
		rcu_unregister_thread();
	}
	return 0;
}
