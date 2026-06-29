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
#include <urcu/rcu-txn.h>
#include <urcu/rculist.h>
#include <urcu/fair-mutex.h>

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
	int i, ncpu = (int) sysconf(_SC_NPROCESSORS_CONF);
	int have_numa = (numa_available() != -1);

	if (ncpu < 1) ncpu = 1;
	s->ncpu = ncpu;
	s->obj = pcpu_round_up(obj_size < 16 ? 16 : obj_size, 16);
	s->arena = calloc((size_t) ncpu, sizeof(*s->arena));
	for (i = 0; i < ncpu; i++) {
		pthread_mutex_init(&s->arena[i].lock, NULL);
		s->arena[i].numa_node = have_numa ? numa_node_of_cpu(i) : -1;
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
	struct pcpu_slab_arena *a;
	void *p;

	if (cpu < 0 || cpu >= s->ncpu) cpu = 0;
	a = &s->arena[cpu];
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

#ifdef LIST_RCU_INLINE_RCU_HEAD
static void su_free(struct rcu_head *h)
{
	free(caa_container_of(h, struct su_elem, rh));
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
	pthread_mutex_lock(&g_su_wlock);
	if (g_present[slot]) {
		struct su_elem *e = g_su_churn[slot];
		if (urcu_txn_sw_list_del_rcu(&e->node))
			abort();
#ifndef LIST_RCU_INLINE_RCU_HEAD
		seg_call_rcu(e);
#else
		call_rcu(&e->rh, su_free);
#endif
		g_su_churn[slot] = NULL;
		g_present[slot] = 0;
	} else {
		int a = g_anchor[slot];
		struct su_elem *e = malloc(sizeof(*e));
		e->key = 2 * a + 1;
		if (urcu_txn_sw_list_add_after_rcu(&e->node, &g_su_stable[a]->node))
			abort();
		g_su_churn[slot] = e;
		g_present[slot] = 1;
	}
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
		if (urcu_txn_list_add_tail_rcu(&e->node, &g_lf_head))
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
		if (k <= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
		prev = k; vis++;
	}
	prev = INT_MAX; steps = 0;
	for (p = urcu_txn_list_prev_rcu(&g_lf_head.node);
			p != &g_lf_head.node;
			p = urcu_txn_list_prev_rcu(p)) {
		int k = caa_container_of(p, struct lf_elem, node)->key;
		if (k >= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
		prev = k; vis++;
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
		r = urcu_txn_list_del_rcu(&e->node, &g_lf_head);
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
				&g_lf_stable[a]->node, &g_lf_head);
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

	urcu_txn_init(&txn, &g_lf_head.domain);
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
		raw = urcu_txn_load(&txn, (void **) &g_lf_index[i]);
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
						NULL, IDX_ENC(&fresh->node));
		} else {			/* full -> unlink + clear index, one flip */
#ifdef BENCH_LTTNG
			del_cur = cur;
			del_prev = urcu_txn_list_prev_rcu(cur);
			del_succ = urcu_txn_list_next_rcu(cur);
#endif
			prep = urcu_txn_list_del_prepare(&txn, cur);
			if (prep == 0)
				urcu_txn_store(&txn, (void **) &g_lf_index[i],
						raw, NULL);
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
		urcu_txn_init(&txn, &g_lf_head.domain);
		urcu_txn_begin(&txn);
		if (urcu_txn_list_del_prepare(&txn, cur) == 0) {
			urcu_txn_store(&txn, (void **) &g_lf_index[i],
					raw, NULL);
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

/* ── Engine registry ─────────────────────────────────────────── */
static const struct lengine engines[] = {
	{ "txn_sw_list",  "RCU single-updater, coherent bidir", 1, su_build,  su_read,  su_write  },
	{ "txn_list",  "RCU concurrent, coherent bidir",      1, lf_build,  lf_read,  lf_write, lf_write_random, lf_reset_index },
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
static int g_random_pos;		/* BENCH_RANDOM_POS: random-anchor writer mode */

/* ── Worker threads ───────────────────────────────────────────── */
struct reader_arg { unsigned long visits; long viol; int cpu; };
struct writer_arg { unsigned long writes; int wid; int nwriters; int cpu; };

static void *reader_thread(void *arg)
{
	struct reader_arg *ra = arg;
	unsigned long visits = 0;
	long viol = 0;

	bench_topology_pin(ra->cpu);
	if (g_eng->uses_rcu)
		rcu_register_thread();

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

	while (!start_flag)
		caa_cpu_relax();
	base = mono_ns();

	while (!uatomic_load(&stop_flag, CMM_RELAXED)) {
		if (random_pos) {
			g_eng->write_random(&rng);
		} else {
			int slot = first + m * nw;
			if (slot >= CHURN) { m = 0; slot = first; }
			g_eng->write_toggle(slot);
			m++;
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

	if (g_eng->uses_rcu)
		rcu_unregister_thread();
	wa->writes = writes;
	return NULL;
}

/* Restore the churn set to empty between sweep points (called single-threaded
 * by main; main is registered/online so RCU frees and barrier work). */
static void reset_churn(void)
{
	int j;
	for (j = 0; j < CHURN; j++)
		if (g_present[j])
			g_eng->write_toggle(j);		/* present -> delete */
	if (g_random_pos && g_eng->reset_random)
		g_eng->reset_random();			/* clear transacted-index cells */
	if (g_eng->uses_rcu) {
		rcu_quiescent_state();
		rcu_barrier();				/* drain deferred frees */
	}
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

	*read_mvps = (double) tot_visits / elapsed / 1e6;
	*write_mops = (double) tot_writes / elapsed / 1e6;
	*violations = tot_viol;

	reset_churn();
	free(th); free(ra); free(wa);
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
		if (getenv("BENCH_NO_PERCPU_CALLRCU") == NULL) {
			if (create_all_cpu_call_rcu_data(0))
				fprintf(stderr, "per-cpu call_rcu: setup failed (%m); using single default worker\n");
			else
				fprintf(stderr, "per-cpu call_rcu workers enabled (default; BENCH_NO_PERCPU_CALLRCU to disable)\n");
		}
#ifdef LIST_RCU_INLINE_RCU_HEAD
		/* Per-CPU slab node allocator (prototype) for txn_list's churn nodes. */
		if (getenv("BENCH_PCPU_ALLOC")) {
			g_pcpu_alloc = 1;
			g_slab = pcpu_slab_create(sizeof(struct lf_elem));
			fprintf(stderr, "per-CPU slab node allocator enabled "
				"(obj=%zu B, %d arenas)\n", g_slab->obj, g_slab->ncpu);
		}
#endif
	}

	g_eng->build();
	self_check();

	fprintf(stderr, "%s (%s): LIST_SIZE=%d CHURN=%d, RSS=%ld kB, %ds/point\n",
		g_eng->name, g_eng->label, LIST_SIZE, CHURN, get_rss_kb(), DURATION_SEC);
	printf("engine %s rss_kb %ld list_size %d churn %d\n",
		g_eng->name, get_rss_kb(), LIST_SIZE, CHURN);

	allow_smt = getenv("BENCH_ALLOW_SMT") != NULL;
	g_random_pos = getenv("BENCH_RANDOM_POS") != NULL;
	if (g_random_pos && g_eng->write_random)
		fprintf(stderr, "%s: random-position writer mode "
			"(collisions ~ writers/LIST_SIZE)\n", g_eng->name);

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
