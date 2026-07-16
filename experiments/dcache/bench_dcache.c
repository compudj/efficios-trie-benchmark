// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * bench_dcache -- concurrent throughput harness for the userspace dentry-cache,
 * engine-agnostic (drives dcache_seqlock or dcache_txn through dcache.h).  The
 * S3 sweep: does the urcu-txn port beat the faithful rename_lock + d_seq baseline
 * on BOTH the reader path (path walks) and the writer path (renames), as the
 * rename fraction climbs?
 *
 * Workload -- homogeneous mixed workers over disjoint-owner leaves
 * ---------------------------------------------------------------
 * A fixed directory tree is built once: an immutable prefix spine of `depth-2`
 * dirs, then `ndirs` sibling directories d0..d(ndirs-1) at depth `depth-1`, then
 * leaves at depth `depth`.  A full leaf path is /p0/../d{k}/L{gid}, so every
 * lookup is a `depth`-component RCU walk from the root -- the exact shape the
 * rename_lock/d_seq machinery exists to make consistent.
 *
 * Each of `nthreads` workers runs the SAME mix (README "locked decision": one
 * homogeneous worker kind, not split reader/writer pools).  Per op it rolls the
 * dice: with probability `rename-frac` it RENAMES one of the leaves it OWNS to a
 * different d{k} (a slice of those are RENAME_EXCHANGEs between two of its own
 * leaves), otherwise it LOOKS UP a random full leaf path.  Every leaf id is owned
 * by exactly one worker, so a leaf is always reachable at exactly the path its
 * owner last recorded -- which is what makes the end-of-run conservation census
 * exact even though renames ran wide open.
 *
 * Headline metric: Mlookups/s and Mrenames/s over the same wall-clock, charted
 * against rename fraction (fixed cores) and against cores (fixed fraction).  The
 * seqlock engine additionally exposes dc_seq_walk_retries (the walk-restart storm
 * that IS the reader-path gap); the txn engine never retries a walk, so the
 * harness prints N/A there.
 *
 * Invariant gate: every run ends by verifying namespace conservation -- a
 * dc_walk census shows each leaf id exactly once, and a dc_lookup of each owner's
 * recorded final path returns POSITIVE with the right id.  A failure prints
 * CONSERVATION FAILED and exits nonzero, so a corrupt run can't masquerade as a
 * fast one (same discipline as bench_txn_3skiplist).
 *
 * Usage: bench_dcache [--nthreads N] [--rename-frac F] [--ndirs N] [--depth N]
 *                     [--leaves N] [--duration MS] [--cpustride N]
 *                     [--cpulist c0,c1,...] [--nbuckets N]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE
#endif

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <urcu/uatomic.h>
#include <urcu-qsbr.h>			/* generic rcu_* names => QSBR flavor */
#include <urcu-call-rcu.h>

#include "dcache.h"

/*
 * Weak ref: defined by the seqlock engine, absent from the txn engine.  Reading
 * through the address (guarded by the null check) keeps the harness blind to
 * which engine it links -- no dcache.h pollution.
 */
extern unsigned long dc_seq_walk_retries __attribute__((weak));

/* ---- knobs (argv-overridable) ------------------------------------------- */
static int    nthreads     = 4;
static double rename_frac  = 0.10;	/* fraction of ops that are renames */
static int    ndirs        = 8;		/* rename-target dirs d0..d(ndirs-1) */
static int    depth        = 2;		/* leaf path depth (>=2) */
static int    leaves       = 16;	/* leaves owned per thread */
static long   duration_ms  = 1000;
static int    cpustride    = 1;
/*
 * Explicit CPU map: thread i pins to cpulist[i], overriding the id*cpustride
 * default.  Populated from `--cpulist c0,c1,...` -- run_dcache.sh fills it from
 * `hwloc-calc core:all.pu:0` so the sweep uses exactly one hardware thread per
 * physical core (no SMT sibling doubled up) regardless of how the kernel numbers
 * the PUs.  NULL => fall back to i*cpustride.
 */
static int   *cpulist      = NULL;
static int    cpulist_len  = 0;
static unsigned int nbuckets = 4096;
/*
 * -1 (default): HOMOGENEOUS -- every thread runs the rename-frac mix.  >=0:
 * ROLE-SPLIT -- the first `nwriters` threads do only renames (of their own
 * leaves), the rest do only lookups.  The split isolates the READER path from
 * the "renames eat my timeslice" confound of the mix, so the reader-side gap
 * between the global and per-node generation shows up cleanly.
 */
static int    nwriters     = -1;

static unsigned int rename_thr;		/* rename_frac scaled into [0,1<<20) */
#define FRAC_ONE (1u << 20)

static struct dcache *g_dc;
/*
 * The namespace is tracked as a PERMUTATION over fixed name-slots (a plain
 * rename moves a name to a new dir; RENAME_EXCHANGE swaps which id carries two
 * names).  Indexed by global name token g = owner*leaves + j: g_final_dir[g] is
 * the dir the name L{g} ends in, g_final_id[g] is the id that ends up carrying
 * it.  Both are a permutation of the seed, so the census stays exact.
 */
static int      *g_final_dir;		/* [nthreads*leaves] final dir per name */
static uint64_t *g_final_id;		/* [nthreads*leaves] final id per name */
static char  g_prefix[DC_PATH_MAX][DC_NAME_MAX];	/* spine component names */
static int   g_prefix_len;		/* = depth - 2 */

#define DIR_ID_BASE 1000000ULL		/* dirs (spine + d{k}) carry ids >= this */

/* ---- timing / start gate ------------------------------------------------ */

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

/* Parse `c0,c1,c2,...` (commas or spaces) into the cpulist[] map. */
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

static inline uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return (*s = x);
}

/* ---- path construction -------------------------------------------------- */

/* Build the full leaf path /p0/../d{dir}/L{gid} into p. */
static void mk_leaf_path(struct dc_path *p, int dir, int gid)
{
	int i;

	dc_path_reset(p);
	for (i = 0; i < g_prefix_len; i++)
		dc_path_push(p, g_prefix[i]);
	{
		char buf[DC_NAME_MAX];

		snprintf(buf, sizeof(buf), "d%d", dir);
		dc_path_push(p, buf);
		snprintf(buf, sizeof(buf), "L%d", gid);
		dc_path_push(p, buf);
	}
}

/* Build the directory path /p0/../d{dir} into p. */
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

/* ---- worker ------------------------------------------------------------- */

struct warg {
	int id;
	int cpu;
	long long nlookups;
	long long nrenames;	/* successful renames + exchanges */
	long long nexch;
	long long lk_wrong;	/* POSITIVE hit whose id left its owner's range */
	long long errs;		/* single-owner rename/exchange failures */
};

static void *worker(void *arg)
{
	struct warg *me = arg;
	int base = me->id * leaves;
	int total = nthreads * leaves;
	/* Per name-token j (the leaf named L{base+j}): dir[j] = the dir it lives
	 * in; who[j] = the id currently carrying that name.  A plain rename moves
	 * dir[j]; a RENAME_EXCHANGE swaps who[] between two tokens (the ids trade
	 * names).  This owner is the sole writer of its tokens, so both stay a
	 * deterministic permutation of the seed for the end-of-run census. */
	int *dir = calloc(leaves, sizeof(*dir));
	uint64_t *who = calloc(leaves, sizeof(*who));
	uint64_t s = 0x9e3779b97f4a7c15ULL ^ ((uint64_t) (me->id + 1) * 0x100000001b3ULL);
	struct call_rcu_data *crdp;
	/* role: <0 homogeneous (per-op frac mix); 1 pure writer; 0 pure reader.
	 * Writers are the LAST nwriters ids, so the reader set always occupies the
	 * same low cpus regardless of writer count -- otherwise the readers' NUMA
	 * placement would shift with W and confound the reader-throughput curve. */
	int role = (nwriters >= 0) ? (me->id >= nthreads - nwriters) : -1;
	long long ops = 0;
	int i;

	pin_cpu(me->cpu);
	dc_register_thread();
	/* Per-worker RT call_rcu worker pinned to this CPU, as bench_txn_3skiplist
	 * does: reclaim (and the txn engine's async fold) drains in parallel rather
	 * than funnelling through the single default worker -- otherwise the fold
	 * backlog, being grace-period-bound, would cap the txn engine artificially. */
	crdp = create_call_rcu_data(URCU_CALL_RCU_RT, me->cpu);
	if (crdp)
		set_thread_call_rcu_data(crdp);

	for (i = 0; i < leaves; i++) {
		dir[i] = (base + i) % ndirs;	/* mirror the seed distribution */
		who[i] = (uint64_t) (base + i);	/* name L{base+i} starts on id base+i */
	}

	uatomic_inc(&nthreads_running);
	rcu_thread_offline();			/* don't stall GPs while parked */
	while (uatomic_read(&goflag) == GOFLAG_INIT)
		(void) poll(NULL, 0, 1);
	rcu_thread_online();

	while (uatomic_read(&goflag) == GOFLAG_RUN) {
		int do_rename = (role < 0)
			? ((int) ((xrand(&s) & (FRAC_ONE - 1)) < rename_thr))
			: role;

		if (do_rename) {
			int j0 = (int) (xrand(&s) % (uint64_t) leaves);
			int j1 = -1;

			/* A slice of moves are RENAME_EXCHANGEs of two of my own
			 * tokens: the two ids trade names in one commit (the
			 * two-shells-in-one-commit path).  Both names always
			 * exist, so a single owner never fails. */
			if (leaves >= 2 && (xrand(&s) & 7) == 0) {
				j1 = (int) (xrand(&s) % (uint64_t) leaves);
				if (j1 == j0)
					j1 = (j1 + 1) % leaves;
			}

			if (j1 >= 0) {
				struct dc_path a, b;

				mk_leaf_path(&a, dir[j0], base + j0);
				mk_leaf_path(&b, dir[j1], base + j1);
				if (dc_rename_exchange(g_dc, &a, &b) == 0) {
					uint64_t t = who[j0];
					who[j0] = who[j1]; who[j1] = t;
					me->nrenames++;
					me->nexch++;
				} else {
					me->errs++;
				}
			} else {
				/* Plain rename: move name L{base+j0} to a new dir
				 * (its target dir is empty of that name, so no
				 * -EEXIST; a single owner never sees -ENOENT). */
				struct dc_path from, to;
				int nd = (int) (xrand(&s) % (uint64_t) ndirs);

				if (nd == dir[j0])
					nd = (nd + 1) % ndirs;
				mk_leaf_path(&from, dir[j0], base + j0);
				mk_leaf_path(&to, nd, base + j0);
				if (dc_rename(g_dc, &from, &to) == 0) {
					dir[j0] = nd;
					me->nrenames++;
				} else {
					me->errs++;
				}
			}
		} else {
			/* LOOKUP a random full leaf path.  The walk cost is paid
			 * hit or miss; a POSITIVE hit must carry an id from the
			 * name's owner (exchange keeps every name's id inside its
			 * owner's leaf range), catching torn / wrong-host reads. */
			int g = (int) (xrand(&s) % (uint64_t) total);
			int dr = (int) (xrand(&s) % (uint64_t) ndirs);
			int owner_base = (g / leaves) * leaves;
			struct dc_path p;
			uint64_t id = ~0ULL;

			mk_leaf_path(&p, dr, g);
			if (dc_lookup(g_dc, &p, &id) == DC_POSITIVE &&
			    (id < (uint64_t) owner_base ||
			     id >= (uint64_t) (owner_base + leaves)))
				me->lk_wrong++;
			me->nlookups++;
		}
		if ((++ops & 15) == 0)
			dc_quiescent();		/* let grace periods advance */
	}

	for (i = 0; i < leaves; i++) {
		g_final_dir[base + i] = dir[i];
		g_final_id[base + i] = who[i];
	}
	free(dir); free(who);
	dc_unregister_thread();
	if (crdp) {
		set_thread_call_rcu_data(NULL);
		call_rcu_data_free(crdp);	/* drains this worker's queue */
	}
	return NULL;
}

/* ---- conservation census ------------------------------------------------ */

struct census {
	uint8_t *seen;
	int total;
	long stray;
};

static void census_cb(uint64_t id, const struct dc_path *p, void *arg)
{
	struct census *c = arg;

	(void) p;
	if (id >= DIR_ID_BASE)
		return;			/* a directory, not a leaf */
	if (id >= (uint64_t) c->total) {
		c->stray++;
		return;
	}
	if (c->seen[id]++)
		c->stray++;		/* duplicate reachability */
}

/* ---- setup -------------------------------------------------------------- */

/* Create the immutable spine + the ndirs rename-target dirs, then seed leaves. */
static void build_tree(void)
{
	uint64_t dir_id = DIR_ID_BASE;
	struct dc_path p;
	int i, k;

	/* Prefix spine: p0/p1/.. (depth-2 static dirs). */
	dc_path_reset(&p);
	for (i = 0; i < g_prefix_len; i++) {
		snprintf(g_prefix[i], DC_NAME_MAX, "p%d", i);
		dc_path_push(&p, g_prefix[i]);
		if (dc_add(g_dc, &p, dir_id++)) {
			fprintf(stderr, "mkdir spine /%s failed\n", g_prefix[i]);
			exit(2);
		}
	}
	/* The ndirs rename-target directories under the spine terminal. */
	for (k = 0; k < ndirs; k++) {
		mk_dir_path(&p, k);
		if (dc_add(g_dc, &p, dir_id++)) {
			fprintf(stderr, "mkdir d%d failed\n", k);
			exit(2);
		}
	}
	/* Seed every leaf into its owner's starting dir. */
	for (i = 0; i < nthreads * leaves; i++) {
		mk_leaf_path(&p, i % ndirs, i);
		if (dc_add(g_dc, &p, (uint64_t) i)) {
			fprintf(stderr, "seed leaf %d failed\n", i);
			exit(2);
		}
	}
}

/* Uniform warm-up: touch every leaf once so both engines enter timing warm. */
static void warm(void)
{
	int total = nthreads * leaves;
	int i;

	for (i = 0; i < total; i++) {
		struct dc_path p;
		uint64_t id = 0;

		mk_leaf_path(&p, i % ndirs, i);
		(void) dc_lookup(g_dc, &p, &id);
	}
}

/* ---- main --------------------------------------------------------------- */

static void usage(const char *p)
{
	fprintf(stderr,
	    "usage: %s [--nthreads N] [--rename-frac F] [--writers K] [--ndirs N]\n"
	    "          [--depth N] [--leaves N] [--duration MS] [--cpustride N]\n"
	    "          [--cpulist c0,c1,...] [--nbuckets N]\n"
	    "  --cpulist ...   => explicit CPU map (thread i -> ci), overriding\n"
	    "                     --cpustride.  Feed it `hwloc-calc core:all.pu:0`\n"
	    "                     to pin one hardware thread per physical core.\n"
	    "  --rename-frac F => (homogeneous) fraction (0..1) of ops that are\n"
	    "                     renames; the rest are full-path leaf lookups.\n"
	    "  --writers K     => role-SPLIT: the first K threads do only renames,\n"
	    "                     the rest only lookups.  Isolates the reader path\n"
	    "                     (overrides --rename-frac).  Reader Mlookups/s is\n"
	    "                     then the clean global-vs-per-node headline.\n"
	    "  --depth N       => leaf path depth (>=2): a spine of N-2 static dirs,\n"
	    "                     then d{k}, then the leaf.  Widens the walk window.\n",
	    p);
	exit(2);
}

int main(int argc, char **argv)
{
	pthread_t *tid;
	struct warg *wa;
	struct census c;
	long long t0, t1, total_lk = 0, total_rn = 0, total_ex = 0;
	long long total_wrong = 0, total_err = 0;
	unsigned long retries0 = 0, retries1 = 0;
	double secs, mlk_s, mrn_s;
	int total, i, anomaly = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--nthreads"))         nthreads = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rename-frac")) rename_frac = strtod(argv[++i], NULL);
		else if (!strcmp(argv[i], "--ndirs"))       ndirs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--depth"))       depth = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--leaves"))      leaves = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--duration"))    duration_ms = atol(argv[++i]);
		else if (!strcmp(argv[i], "--cpustride"))   cpustride = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cpulist"))     parse_cpulist(argv[++i]);
		else if (!strcmp(argv[i], "--nbuckets"))    nbuckets = (unsigned) atoi(argv[++i]);
		else if (!strcmp(argv[i], "--writers"))     nwriters = atoi(argv[++i]);
		else usage(argv[0]);
	}
	if (nthreads < 1 || ndirs < 2 || depth < 2 || leaves < 1 ||
	    rename_frac < 0.0 || rename_frac > 1.0 || nwriters > nthreads)
		usage(argv[0]);
	if (cpulist && nthreads > cpulist_len) {
		fprintf(stderr, "--cpulist has %d cpus but --nthreads=%d "
			"(would double up on a core)\n", cpulist_len, nthreads);
		exit(2);
	}
	if (depth - 2 > DC_PATH_MAX - 2)
		depth = DC_PATH_MAX;		/* leave room for d{k} + leaf */
	g_prefix_len = depth - 2;
	rename_thr = (unsigned int) (rename_frac * (double) FRAC_ONE + 0.5);
	total = nthreads * leaves;

	rcu_register_thread();
	g_dc = dc_create(nbuckets);
	g_final_dir = calloc(total, sizeof(*g_final_dir));
	g_final_id = calloc(total, sizeof(*g_final_id));

	printf("== bench_dcache (engine: %s) ==\n", dc_engine_name());
	if (nwriters >= 0)
		printf("threads=%d SPLIT(writers=%d readers=%d) ndirs=%d depth=%d "
		       "leaves/thr=%d duration_ms=%ld total_leaves=%d\n",
		       nthreads, nwriters, nthreads - nwriters, ndirs, depth,
		       leaves, duration_ms, total);
	else
		printf("threads=%d rename_frac=%.4f ndirs=%d depth=%d leaves/thr=%d "
		       "duration_ms=%ld total_leaves=%d\n",
		       nthreads, rename_frac, ndirs, depth, leaves, duration_ms, total);

	build_tree();
	warm();

	tid = calloc(nthreads, sizeof(*tid));
	wa = calloc(nthreads, sizeof(*wa));
	for (i = 0; i < nthreads; i++) {
		wa[i].id = i;
		wa[i].cpu = cpulist ? cpulist[i] : i * cpustride;
		pthread_create(&tid[i], NULL, worker, &wa[i]);
	}
	while (uatomic_read(&nthreads_running) < nthreads)
		(void) poll(NULL, 0, 1);
	cmm_smp_mb();

	/*
	 * Go RCU-offline for the whole timed window AND the join: main is a
	 * registered QSBR thread but only sleeps in poll()/blocks in join here,
	 * reporting no quiescent state.  Left online it would stall EVERY grace
	 * period for the entire measurement -- and the txn engine's async fold is
	 * grace-period-bound, so its chains would grow unbounded (O(n^2)) and the
	 * throughput number would be a liveness artifact, not the engine's speed.
	 */
	if (&dc_seq_walk_retries)
		retries0 = uatomic_read(&dc_seq_walk_retries);
	rcu_thread_offline();
	t0 = now_ns();
	uatomic_set(&goflag, GOFLAG_RUN);
	(void) poll(NULL, 0, (int) duration_ms);
	uatomic_set(&goflag, GOFLAG_STOP);
	t1 = now_ns();

	for (i = 0; i < nthreads; i++) {
		pthread_join(tid[i], NULL);
		total_lk    += wa[i].nlookups;
		total_rn    += wa[i].nrenames;
		total_ex    += wa[i].nexch;
		total_wrong += wa[i].lk_wrong;
		total_err   += wa[i].errs;
	}
	rcu_thread_online();
	if (&dc_seq_walk_retries)
		retries1 = uatomic_read(&dc_seq_walk_retries);

	/* Drain any in-flight reclamation/folds before the census. */
	rcu_quiescent_state();
	synchronize_rcu();
	rcu_barrier();

	secs = (t1 - t0) / 1e9;
	mlk_s = secs > 0 ? (double) total_lk / secs / 1e6 : 0.0;
	mrn_s = secs > 0 ? (double) total_rn / secs / 1e6 : 0.0;

	/* Census: every leaf id reachable exactly once (dc_walk), and each name
	 * L{g} resolves at its recorded final dir to the recorded final id (the
	 * permutation the owner left behind). */
	memset(&c, 0, sizeof(c));
	c.total = total;
	c.seen = calloc(total, 1);
	dc_walk(g_dc, census_cb, &c);
	for (i = 0; i < total; i++) {
		struct dc_path p;
		uint64_t id = ~0ULL;

		if (c.seen[i] != 1)
			anomaly++;		/* id i missing or duplicated */
		mk_leaf_path(&p, g_final_dir[i], i);
		if (dc_lookup(g_dc, &p, &id) != DC_POSITIVE ||
		    id != g_final_id[i])
			anomaly++;
	}

	printf("duration (s): %g\n", secs);
	printf("LOOKUP  lookups: %lld  Mlookups/s: %g  wrong-id: %lld\n",
	       total_lk, mlk_s, total_wrong);
	printf("RENAME  renames: %lld  exchanges: %lld  Mrenames/s: %g  errors: %lld\n",
	       total_rn, total_ex, mrn_s, total_err);
	printf("OPS     Mops/s: %g\n",
	       secs > 0 ? (double) (total_lk + total_rn) / secs / 1e6 : 0.0);
	if (&dc_seq_walk_retries) {
		unsigned long dr = retries1 - retries0;

		printf("MECH    walk-retries: %lu  (%.4f per lookup)\n",
		       dr, total_lk ? (double) dr / (double) total_lk : 0.0);
	} else {
		printf("MECH    walk-retries: N/A (engine never retries a walk)\n");
	}

	anomaly += (total_wrong != 0) + (total_err != 0) + (c.stray != 0);
	if (anomaly)
		printf("CONSERVATION FAILED: %d anomalies (stray/dup %ld, wrong-id %lld, "
		       "rename-err %lld) -- run is CORRUPT, ignore the numbers above\n",
		       anomaly, c.stray, total_wrong, total_err);
	else
		printf("CHECK   conservation: OK (all %d leaves accounted for)\n", total);

	free(c.seen);
	free(tid); free(wa);
	dc_destroy(g_dc);
	free(g_final_dir); free(g_final_id);
	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
