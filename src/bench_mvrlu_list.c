/* SPDX-License-Identifier: MIT
 *
 * mvrlu_list: MV-RLU (Kim/Mathew/Kashyap/Ramanathan/Min, ASPLOS 2019) driving
 * the SAME doubly-linked churn workload as txn_sw_list / txn_list / rlu_list,
 * so the multi-version scheme meets the pseudo-transaction engine and its own
 * single-version predecessor on identical ground.
 *
 * Guarantee (declared, not emulated): an MV-RLU reader section observes a
 * COHERENT SNAPSHOT -- strictly stronger than txn_*'s per-slot-linearizable
 * (non-snapshot) reads, and the same guarantee class as rlu_list.  Where RLU
 * keeps ONE copy per locked object and must quiesce before writeback, MV-RLU
 * keeps a per-object VERSION CHAIN so readers pick a version <= their local
 * clock and stop blocking the writer's writeback.
 *
 * CLOCK VARIANT -- READ third_party/mvrlu/PROVENANCE.txt BEFORE CHANGING THIS.
 * We build the gclk variant (a global atomic clock) and NOT the ordo variant
 * (RDTSCP + a calibrated per-machine uncertainty boundary).  ORDO's
 * __ORDO_BOUNDARY is a compile-time constant that CORRECTNESS depends on:
 * mvrlu.c:1543 accepts a version only if wrt_clk + boundary < local_clk, so a
 * boundary smaller than the machine's true cross-core clock skew yields silent
 * wrong-version reads.  Upstream obtains it by benchmarking the machine, and
 * its estimator -- max over SAMPLED pairs -- is a LOWER bound on worst-case
 * skew used as an UPPER bound.  gclk needs no such constant, so it is the
 * configuration an honest deployability comparison measures.  It does
 * reintroduce the central-clock contention ORDO exists to remove; say so when
 * reporting, rather than quietly picking the flattering arm.
 *
 * MV-RLU also runs a background QP (quiescence) thread for reclamation, started
 * by mvrlu_init().  That is its design, not an artifact of this harness.
 *
 * See bench_mvrlu_list.h for why this is a separate translation unit.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mvrlu.h"		/* third_party/mvrlu/include */

#include "bench_mvrlu_list.h"

struct mv_lnode { struct mv_lnode *next, *prev; int key; };

static struct mv_lnode  *g_mv_head;	/* mvrlu_alloc'd circular sentinel */
static struct mv_lnode **g_mv_stable;	/* [list_size] permanent nodes */
static struct mv_lnode **g_mv_churn;	/* [churn] master ptr per slot */
static struct mvl_ctx	 g_ctx;

/* Per-OS-thread registration.  Unlike RLU -- which draws uniq_id from a
 * monotonic counter bounded by RLU_MAX_THREADS, forcing the pool/generation
 * rewind in rlu_point_reset() -- MV-RLU has no fixed thread-id registry:
 * mvrlu_thread_alloc() is a plain allocation and mvrlu_thread_init() links the
 * thread onto a global live list.  Log space comes from a bitmap allocator over
 * a reserved region (MVRLU_MAX_THREAD_NUM = 16384 slots) and is RETURNED by
 * mvrlu_thread_finish(), so registering afresh each sweep point is safe and no
 * point_reset hook is needed. */
static __thread mvrlu_thread_struct_t *tls_mv;

static mvrlu_thread_struct_t *mv_self(void)
{
	if (__builtin_expect(tls_mv == NULL, 0)) {
		tls_mv = mvrlu_thread_alloc();
		if (!tls_mv) {
			fprintf(stderr, "mvrlu_list: mvrlu_thread_alloc failed\n");
			exit(1);
		}
		mvrlu_thread_init(tls_mv);
	}
	return tls_mv;
}

void mvl_tl_begin(void) { (void) mv_self(); }

void mvl_tl_end(void)
{
	if (!tls_mv)
		return;
	mvrlu_flush_log(tls_mv);	/* push this thread's deferred versions */
	mvrlu_thread_finish(tls_mv);	/* deregister; returns log space */
	mvrlu_thread_free(tls_mv);
	tls_mv = NULL;
}

/* BENCH_MVRLU_STATS=1 dumps MV-RLU's own counters at exit.  Upstream leaves
 * MVRLU_ENABLE_STATS defined in lib/debug.h, so they are always collected; the
 * increments sit in mvrlu_reader_lock/unlock only (2 per section, none in
 * mvrlu_deref), so leaving them on is not a measurable handicap.  The counters
 * that matter for diagnosing writer stalls are n_high_mark_block (the writer
 * blocked in log_reclaim_force waiting for the QP thread), n_qp_detect and
 * n_qp_nap (QP rounds, and how many of those ate the full
 * MVRLU_QP_INTERVAL_USEC nap). */
static void mvl_dump_stats(void) { mvrlu_print_stats(); }

/*
 * QP-THREAD PLACEMENT -- measured 2026-07-26, worth ~7x, do not remove.
 *
 * mvrlu_init() spawns MV-RLU's background QP (quiescence/reclaim) thread via
 * port_create_thread(), which is a bare pthread_create with NO affinity
 * (third_party/mvrlu/lib/port-user.h:205).  It therefore inherits the caller's
 * mask -- and this harness leaves main unpinned (0-383) while hwloc pins every
 * WORKER to one PU per physical core.  On this 2-socket / 24-NUMA-node / 384-PU
 * box the reclaim thread then wanders: observed on PSR 378 while the writer ran
 * on core 0, touching that writer's 512 KB log across sockets.
 *
 * Measured, single writer, 10000/200, 3 s x 5:
 *   unconfined          1.20 1.73 1.14 1.03 1.99 Mops   (bimodal, ~7x spread)
 *   writer's NUMA node  8.34 8.81 8.85 8.79 8.66 Mops   (tight)
 *   core 0 + SMT sib    7.42 7.72 7.65 7.72 7.57 Mops   (tight)
 *
 * Leaving it unpinned is a handicap NO OTHER ENGINE HERE SUFFERS -- every other
 * engine's threads, including liburcu's per-CPU call_rcu workers, are placed.
 * So we confine it, by default to the NUMA node of CPU 0 (where worker 0, the
 * first writer, is pinned).  BENCH_MVRLU_QP_CPUS overrides: an explicit list
 * ("0-7", "0,192"), or "free" to restore upstream's floating behaviour.
 *
 * NOT papered over: MV-RLU has exactly ONE global QP thread by design, so at
 * high writer counts it is a genuine central bottleneck no placement fixes.
 * That is a property of the algorithm and belongs in any write-scaling result.
 */
static int mvl_parse_cpulist(const char *s, cpu_set_t *set)
{
	int n = 0;

	CPU_ZERO(set);
	while (*s) {
		char *end;
		long a = strtol(s, &end, 10), b;

		if (end == s)
			return -1;
		b = a;
		if (*end == '-') {
			s = end + 1;
			b = strtol(s, &end, 10);
			if (end == s)
				return -1;
		}
		for (; a <= b; a++) {
			CPU_SET((int) a, set);
			n++;
		}
		s = (*end == ',') ? end + 1 : end;
	}
	return n;
}

/* CPU list of the NUMA node containing CPU 0, e.g. "0-7". */
static int mvl_node0_cpulist(char *buf, size_t len)
{
	DIR *d = opendir("/sys/devices/system/cpu/cpu0");
	struct dirent *e;
	char path[sizeof(((struct dirent *) 0)->d_name) + 64];
	FILE *f;
	int ok = 0;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "node", 4) || !isdigit((unsigned char) e->d_name[4]))
			continue;
		snprintf(path, sizeof(path),
			 "/sys/devices/system/node/%s/cpulist", e->d_name);
		if ((f = fopen(path, "r"))) {
			if (fgets(buf, (int) len, f)) {
				buf[strcspn(buf, "\n")] = '\0';
				ok = 1;
			}
			fclose(f);
		}
		break;
	}
	closedir(d);
	return ok ? 0 : -1;
}

/* Confine THIS thread across mvrlu_init() so the QP thread inherits the mask,
 * then restore.  Returns 1 if a mask was applied (caller restores). */
static int mvl_confine_for_qp(cpu_set_t *saved)
{
	const char *env = getenv("BENCH_MVRLU_QP_CPUS");
	char buf[512];
	cpu_set_t set;
	int n;

	if (env && !strcmp(env, "free")) {
		fprintf(stderr, "mvrlu_list: QP thread UNPINNED (upstream default) "
			"-- expect ~7x run-to-run variance on this machine\n");
		return 0;
	}
	if (!env) {
		if (mvl_node0_cpulist(buf, sizeof(buf)) != 0)
			return 0;
		env = buf;
	}
	if (sched_getaffinity(0, sizeof(*saved), saved) != 0)
		return 0;
	if ((n = mvl_parse_cpulist(env, &set)) <= 0) {
		fprintf(stderr, "mvrlu_list: bad BENCH_MVRLU_QP_CPUS '%s'\n", env);
		return 0;
	}
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
		return 0;
	fprintf(stderr, "mvrlu_list: QP thread confined to CPUs %s (%d)\n", env, n);
	return 1;
}

void mvl_build(const struct mvl_ctx *ctx)
{
	struct mv_lnode *prev;
	cpu_set_t saved;
	int restore, i;

	g_ctx = *ctx;

	/* mvrlu_init() spawns the QP thread, which inherits our affinity mask --
	 * see mvl_confine_for_qp() above for why that is worth ~7x. */
	restore = mvl_confine_for_qp(&saved);
	if (mvrlu_init() != 0) {
		fprintf(stderr, "mvrlu_list: mvrlu_init failed\n");
		exit(1);
	}
	if (restore)
		sched_setaffinity(0, sizeof(saved), &saved);
	if (getenv("BENCH_MVRLU_STATS"))
		atexit(mvl_dump_stats);

	/* Single-threaded build: no reader exists yet, so plain field stores
	 * publish the initial ring without the MV-RLU write protocol -- the same
	 * shortcut rlu_build() takes.  Nodes MUST still come from mvrlu_alloc:
	 * it prepends the object header the deref path reads. */
	g_mv_head = mvrlu_alloc(sizeof(*g_mv_head));
	g_mv_head->key = INT_MIN;
	g_mv_stable = calloc((size_t) g_ctx.list_size, sizeof(*g_mv_stable));
	prev = g_mv_head;
	for (i = 0; i < g_ctx.list_size; i++) {
		struct mv_lnode *n = mvrlu_alloc(sizeof(*n));

		n->key = 2 * i;
		n->prev = prev;
		prev->next = n;
		g_mv_stable[i] = n;
		prev = n;
	}
	prev->next = g_mv_head;			/* close the ring */
	g_mv_head->prev = prev;
	g_mv_churn = calloc((size_t) g_ctx.churn, sizeof(*g_mv_churn));
}

unsigned long mvl_read(long *viol)
{
	mvrlu_thread_struct_t *self = mv_self();
	struct mv_lnode *head = g_mv_head, *h, *p;
	unsigned long vis = 0;
	int prev, steps;
	/* Hoisted out of the traversal: see run_deref_cost.sh -- loading these
	 * per iteration charges the engine memory references the harness owns. */
	const int rnd = g_ctx.random_pos, slim = g_ctx.step_limit;

	mvrlu_reader_lock(self);
	h = mvrlu_deref(self, head);
	prev = INT_MIN; steps = 0;
	for (p = mvrlu_deref(self, h->next);
			!mvrlu_cmp_ptrs(p, head);
			p = mvrlu_deref(self, p->next)) {
		int k = p->key;
		if (++steps > slim) { (*viol)++; break; }
		if (!rnd) {
			if (k <= prev) { (*viol)++; break; }
			prev = k;
		}
		vis++;
	}
	prev = INT_MAX; steps = 0;
	for (p = mvrlu_deref(self, h->prev);
			!mvrlu_cmp_ptrs(p, head);
			p = mvrlu_deref(self, p->prev)) {
		int k = p->key;
		if (++steps > slim) { (*viol)++; break; }
		if (!rnd) {
			if (k >= prev) { (*viol)++; break; }
			prev = k;
		}
		vis++;
	}
	mvrlu_reader_unlock(self);
	return vis;
}

void mvl_write(int slot)
{
	mvrlu_thread_struct_t *self = mv_self();

	if (g_ctx.present[slot]) {		/* unlink the churn node */
		struct mv_lnode *cur = g_mv_churn[slot];	/* master identity */
		struct mv_lnode *c, *prev, *succ;
del_restart:
		mvrlu_reader_lock(self);
		c    = mvrlu_deref(self, cur);
		prev = mvrlu_deref(self, c->prev);
		succ = mvrlu_deref(self, c->next);
		if (!mvrlu_try_lock(self, &prev)) { mvrlu_abort(self); goto del_restart; }
		if (!mvrlu_try_lock(self, &cur))  { mvrlu_abort(self); goto del_restart; }
		if (!mvrlu_try_lock(self, &succ)) { mvrlu_abort(self); goto del_restart; }
		mvrlu_assign_ptr(self, &prev->next, succ);
		mvrlu_assign_ptr(self, &succ->prev, prev);
		mvrlu_free(self, cur);
		mvrlu_reader_unlock(self);
		g_mv_churn[slot] = NULL;
		g_ctx.present[slot] = 0;
	} else {				/* splice a fresh node after the anchor */
		int a = g_ctx.anchor[slot];
		struct mv_lnode *pos = g_mv_stable[a];		/* master identity */
		struct mv_lnode *p, *succ, *nw;
ins_restart:
		mvrlu_reader_lock(self);
		p    = mvrlu_deref(self, pos);
		succ = mvrlu_deref(self, p->next);
		if (!mvrlu_try_lock(self, &pos))  { mvrlu_abort(self); goto ins_restart; }
		if (!mvrlu_try_lock(self, &succ)) { mvrlu_abort(self); goto ins_restart; }
		nw = mvrlu_alloc(sizeof(*nw));
		nw->key = 2 * a + 1;
		mvrlu_assign_ptr(self, &nw->next, succ);
		mvrlu_assign_ptr(self, &nw->prev, pos);
		mvrlu_assign_ptr(self, &pos->next, nw);
		mvrlu_assign_ptr(self, &succ->prev, nw);
		mvrlu_reader_unlock(self);
		g_mv_churn[slot] = nw;
		g_ctx.present[slot] = 1;
	}
}
