// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * bench_list_scale -- read/write scaling benchmark for concurrent doubly-linked
 * lists, comparing the new userspace-rcu *bidirectional* RCU lists against the
 * state-of-the-art lock-based and seqlock alternatives.
 *
 * The new lists (from compudj/userspace-rcu-dev, branch rcu-bidir-list-dev):
 *   bidir_su  <urcu/rcu-bidir-list.h>          RCU readers, SINGLE updater
 *   bidir_lf  <urcu/rcu-bidir-list-lockfree.h> RCU readers, LOCK-FREE updaters
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
#include <numa.h>

#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu-qsbr.h>
#include <urcu-call-rcu.h>
#include <urcu/rcu-bidir-list.h>
#include <urcu/rcu-bidir-list-lockfree.h>
#include <urcu/flip-latch-txn-lockfree.h>
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
};

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

/* ── bidir_su: single-updater coherent bidirectional RCU list ── */
struct su_elem {
	struct cds_bidir_list_head node;
	int key;
#ifdef LIST_RCU_INLINE_RCU_HEAD
	struct rcu_head rh;
#endif
};
static struct cds_bidir_list_head g_su_head =
		CDS_BIDIR_LIST_HEAD_INIT(g_su_head);
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
	cds_bidir_list_init(&g_su_head);
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
		if (cds_bidir_list_add_tail_rcu(&e->node, &g_su_head))
			abort();
		g_su_stable[i] = e;
	}
	g_su_churn = calloc(CHURN, sizeof(*g_su_churn));
}
static unsigned long su_read(long *viol)
{
	struct cds_bidir_list_head *p;
	unsigned long vis = 0;
	int prev, steps;

	rcu_read_lock();
	prev = INT_MIN; steps = 0;
	for (p = cds_bidir_list_next_rcu(&g_su_head); p != &g_su_head;
			p = cds_bidir_list_next_rcu(p)) {
		int k = caa_container_of(p, struct su_elem, node)->key;
		if (k <= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
		prev = k; vis++;
	}
	prev = INT_MAX; steps = 0;
	for (p = cds_bidir_list_prev_rcu(&g_su_head); p != &g_su_head;
			p = cds_bidir_list_prev_rcu(p)) {
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
		if (cds_bidir_list_del_rcu(&e->node))
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
		if (cds_bidir_list_add_after_rcu(&e->node, &g_su_stable[a]->node))
			abort();
		g_su_churn[slot] = e;
		g_present[slot] = 1;
	}
	pthread_mutex_unlock(&g_su_wlock);
}

/* ── bidir_lf: lock-free coherent bidirectional RCU list ── */
struct lf_elem {
	struct cds_bidir_list_lf_node node;
	int key;
#ifdef LIST_RCU_INLINE_RCU_HEAD
	struct rcu_head rh;
#endif
};
static struct cds_bidir_list_lf_head g_lf_head;
static struct lf_elem **g_lf_stable;
static struct lf_elem **g_lf_churn;

#ifdef LIST_RCU_INLINE_RCU_HEAD
static void lf_free(struct rcu_head *h)
{
	free(caa_container_of(h, struct lf_elem, rh));
}
#endif
static void lf_build(void)
{
	int i;
	cds_bidir_list_lf_init(&g_lf_head);
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
		if (cds_bidir_list_lf_add_tail_rcu(&e->node, &g_lf_head))
			abort();
		g_lf_stable[i] = e;
	}
	g_lf_churn = calloc(CHURN, sizeof(*g_lf_churn));
}
static unsigned long lf_read(long *viol)
{
	struct cds_bidir_list_lf_node *p;
	unsigned long vis = 0;
	int prev, steps;

	rcu_read_lock();
	prev = INT_MIN; steps = 0;
	for (p = cds_bidir_list_lf_next_rcu(&g_lf_head.node);
			p != &g_lf_head.node;
			p = cds_bidir_list_lf_next_rcu(p)) {
		int k = caa_container_of(p, struct lf_elem, node)->key;
		if (k <= prev || ++steps > STEP_LIMIT) { (*viol)++; break; }
		prev = k; vis++;
	}
	prev = INT_MAX; steps = 0;
	for (p = cds_bidir_list_lf_prev_rcu(&g_lf_head.node);
			p != &g_lf_head.node;
			p = cds_bidir_list_lf_prev_rcu(p)) {
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
		r = cds_bidir_list_lf_del_rcu(&e->node, &g_lf_head);
		rcu_read_unlock();
		if (r < 0)
			abort();		/* -ENOMEM */
		if (r == 1)
#ifndef LIST_RCU_INLINE_RCU_HEAD
			seg_call_rcu(e);
#else
			call_rcu(&e->rh, lf_free);
#endif
		g_lf_churn[slot] = NULL;
		g_present[slot] = 0;
	} else {
		int a = g_anchor[slot], r;
		struct lf_elem *e = malloc(sizeof(*e));
		e->key = 2 * a + 1;
		rcu_read_lock();
		r = cds_bidir_list_lf_insert_after_rcu(&e->node,
				&g_lf_stable[a]->node, &g_lf_head);
		rcu_read_unlock();
		if (r != 0)			/* anchor is permanent: never -ENOENT */
			abort();
		g_lf_churn[slot] = e;
		g_present[slot] = 1;
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
	{ "bidir_su",  "RCU single-updater, coherent bidir", 1, su_build,  su_read,  su_write  },
	{ "bidir_lf",  "RCU lock-free, coherent bidir",      1, lf_build,  lf_read,  lf_write  },
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

	bench_topology_pin(wa->cpu);
	if (g_eng->uses_rcu)
		rcu_register_thread();

	while (!start_flag)
		caa_cpu_relax();
	base = mono_ns();

	while (!uatomic_load(&stop_flag, CMM_RELAXED)) {
		int slot = first + m * nw;
		if (slot >= CHURN) { m = 0; slot = first; }
		g_eng->write_toggle(slot);
		writes++;
		m++;
		if (g_eng->uses_rcu)
			rcu_quiescent_state();
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

	/* Wait until readers finished priming and are spinning on start_flag. */
	while (__atomic_load_n(&prime_done_count, __ATOMIC_ACQUIRE) < nr_readers)
		usleep(1000);

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

	for (i = 0; i < total; i++)
		pthread_join(th[i], NULL);
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
		 * (which otherwise caps lock-free writer throughput at ~8 Mops/s and is
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
	}

	g_eng->build();
	self_check();

	fprintf(stderr, "%s (%s): LIST_SIZE=%d CHURN=%d, RSS=%ld kB, %ds/point\n",
		g_eng->name, g_eng->label, LIST_SIZE, CHURN, get_rss_kb(), DURATION_SEC);
	printf("engine %s rss_kb %ld list_size %d churn %d\n",
		g_eng->name, get_rss_kb(), LIST_SIZE, CHURN);

	allow_smt = getenv("BENCH_ALLOW_SMT") != NULL;

	if (getenv("BENCH_RW_BALANCED")) {
		/*
		 * Balanced 50/50 sweep: at each total thread count T, run T/2 readers
		 * and T/2 writers concurrently (each on its own physical core), so the
		 * structure takes simultaneous read and write pressure as the machine
		 * fills.  Shows how each engine handles a mixed workload: RCU readers
		 * never block (and bidir_lf's writers also scale), whereas the lock /
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
