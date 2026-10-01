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
 * dice: with probability `rename-frac` it MUTATES one of the leaves it OWNS,
 * otherwise it LOOKS UP a random full leaf path.  Every leaf id is owned by
 * exactly one worker, so a leaf is always reachable at exactly the path its
 * owner last recorded -- which is what makes the end-of-run conservation census
 * exact even though renames ran wide open.
 *
 * Which mutation -- the op taxonomy (--op-mix)
 * -------------------------------------------
 * The four mutating ops are a 2x2 of {file|directory} x {same-dir|cross-dir}:
 * *rename* (same-dir file), *file move* (cross-dir file), *directory rename*
 * (same-dir dir), *directory move* (cross-dir dir).  The two axes ARE the two
 * code branches: same-vs-cross gates the O(depth) ancestry cycle-check, the
 * d_moving lock and the reparent store; file-vs-dir sets how many reader walks
 * the relocation invalidates.  This bench owns the LEAF axis -- its leaf type is
 * a build switch (-DDC_BENCH_FILE_LEAVES) and its leaves have no children, so it
 * covers `rename` and `file move`; the subtree cells belong to
 * bench_dcache_height, which moves nodes that dominate B^H leaves.
 *
 * --op-mix rename=A,move=B,exchange=C weights the leaf ops (default 0/7/1, the
 * historical hardcoded mix, whose RNG stream it reproduces exactly):
 *   move      cross-dir: name kept, moved to a different d{k}.  Runs the whole
 *             cross_parent branch.
 *   rename    SAME-dir: the token flips between its two reserved names L{g} and
 *             M{g} inside its current d{k}.  Skips the cross_parent branch
 *             entirely -- and it is the most common op in a real filesystem,
 *             which is why leaving it unmeasured was a coverage gap.
 *   exchange  RENAME_EXCHANGE of two of the owner's tokens (two shells, one
 *             commit).
 * Two names per token is what keeps the census exact under same-dir renames: a
 * name-slot is a permutation only if the op moves a name between FIXED slots,
 * so each token gets a private pair and a rename is a flip between them.  The
 * alternate qstr table is allocated whenever --op-mix is passed (even at
 * rename=0) so every point of an op-mix sweep carries the same harness
 * footprint, and the legacy default allocates nothing new.
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
 * Reverse walk (--dpath): the readers ask the kernel's dentry_path_raw question
 * instead -- "where is this object NOW?" -- of a handle pinned at seed time
 * (dc_lookup_dentry), while the writers rename.  That is the one operation the
 * kernel itself serves with a whole-path SNAPSHOT (a rename_lock bracket), so it
 * is the like-for-like reader for engines that give every walk a snapshot.
 * Whatever the reader op, every run ends by checking each handle's reported path
 * against the name its owner last gave it, exactly.
 *
 * Usage: bench_dcache [--nthreads N] [--rename-frac F] [--writers K] [--readdir]
 *                     [--dpath]
 *                     [--op-mix rename=A,move=B,exchange=C]
 *                     [--ndirs N] [--depth N] [--leaves N] [--duration MS]
 *                     [--cpustride N] [--cpulist c0,c1,...] [--nbuckets N]
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
#include "dcache_bench_rand.h"
#include "dcache_bench_pace.h"
#include "dcache_bench_setup.h"

/*
 * Weak ref: defined by the seqlock engine, absent from the txn engine.  Reading
 * through the address (guarded by the null check) keeps the harness blind to
 * which engine it links -- no dcache.h pollution.
 */
extern unsigned long dc_seq_walk_retries __attribute__((weak));

/*
 * Weak ref: 1 in a txn engine built -DDC_SPLIT_ELIDE_ID (dc_lookup returns the
 * host ADDRESS, not the logical d_id), else 0/absent.  When set, the id-VALUE
 * checks below are meaningless (the returned id is a pointer), so they are
 * skipped -- the dc_walk census (which reads the real stored d_id) remains the
 * conservation gate, and the DC_POSITIVE/absent presence checks still run.
 */
extern const int dc_lookup_id_is_address __attribute__((weak));
static inline int id_is_address(void)
{
	return &dc_lookup_id_is_address && dc_lookup_id_is_address;
}

/* ---- knobs (argv-overridable) ------------------------------------------- */
static int    nthreads     = 4;
static double rename_frac  = 0.10;	/* fraction of ops that are renames */
static int    ndirs        = 8;		/* rename-target dirs d0..d(ndirs-1) */
static int    depth        = 2;		/* leaf path depth (>=2) */
static int    leaves       = 16;	/* leaves owned per thread (per writer
					 * under writers_own) */
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
 * Co-tenant model (--pollute N [--pollute-kb K]): between lookups each thread
 * streams N cachelines of its own K-KB "application" buffer.  This evicts the
 * dentry's cold lines exactly as a real caller would -- the dcache never owns
 * the cache alone in production -- so a smaller per-dentry footprint (the
 * DC_HOT1CL_SPLIT 1-CL layout) is rewarded, which the isolated walk hides.
 */
static unsigned int pollute = 0;
static unsigned int pollute_kb = 4096;
static volatile uint64_t g_pollute_sink;
/*
 * Precompute the path component qstrs once and assemble each lookup's dc_path
 * from them, so the timed loop pays NO per-lookup snprintf + FNV hash.  A real
 * VFS walks pre-parsed components; that per-op string building is a harness
 * artifact, equal for every engine, and -- because it runs concurrently with the
 * descent's memory stalls -- it HIDES the per-lookup cache-footprint difference
 * the layout A/B is trying to measure (see simplification-s4.md §5).  So it is
 * the DEFAULT; --no-precomp restores the legacy per-lookup snprintf.  The
 * leaf-qstr table is itself a co-footprint -- identical for every binary EXCEPT
 * the mark arm, whose wider DC_NAME_MAX widens `struct qstr` and so both this
 * table and every per-component path copy.  That is the matched-width control's
 * whole subject; see dcache.h's DC_NAME_PAD.
 */
static int precomp = 1;
static struct qstr *g_prefix_q, *g_dir_q, *g_leaf_q;
/*
 * Alternate leaf-name table ("M{g}" against g_leaf_q's "L{g}"), the second half
 * of each token's private name pair -- see the op-taxonomy note in the header.
 * Allocated iff --op-mix was passed, so the legacy default's harness footprint
 * (and the reader's qstr working set, which is NOT free -- see dcache.h's
 * DC_NAME_PAD note) is byte-for-byte what it always was.
 */
static struct qstr *g_leaf_q_alt;
static int    g_two_names  = 0;		/* 1 => tokens have an L/M name pair */
static uint64_t g_alt_mask = 0;		/* 1 when g_two_names, else 0 (branchless) */
/*
 * Ops between QSBR quiescent-state announcements (dc_quiescent()).  Tunable via
 * --quiesce (power of two).  Default 16: the per-op cost of the quiescent smp_mb
 * is real but tiny, and MEASUREMENT shows a coarser cadence is a net *loss* for
 * the txn engine -- its async fold is grace-period-bound, so stretching the GP
 * (16 -> 256 ops) starves the fold drain and the reader's chain_host walk pays
 * for the longer shell chains (readdir -43%, lookup -10% at 256; the fold-less
 * seqlock arm is flat).  Frequent quiescing is not overhead here; it keeps the
 * fold healthy.  The knob exists to demonstrate exactly that sensitivity.
 */
static unsigned int quiesce_mask = 15;
/*
 * -1 (default): HOMOGENEOUS -- every thread runs the rename-frac mix.  >=0:
 * ROLE-SPLIT -- the first `nwriters` threads do only renames (of their own
 * leaves), the rest do only lookups.  The split isolates the READER path from
 * the "renames eat my timeslice" confound of the mix, so the reader-side gap
 * between the global and per-node generation shows up cleanly.
 */
static int    nwriters     = -1;
/*
 * --rename-rate R (role-split only): hold the writers to R renames/s in
 * aggregate, R/nwriters each (dcache_bench_pace.h).  Unpaced (0, the default)
 * each engine's readers face the rate its OWN writers reach, which differs by
 * up to two orders of magnitude between engines -- and fewer renames means
 * fewer invalidations of the lines the readers share, so the engine with the
 * slower writer gets the quieter reader measurement.  Paced, every engine's
 * readers face one offered load; the RENAME line reports the achieved rate
 * against the target, and an engine that cannot keep up shows it there.
 */
static double rename_rate  = 0.0;
/*
 * --readdir (split mode only): the reader threads enumerate a random target dir
 * (dc_readdir of /pfx/d{k}) instead of a full-path leaf lookup.  To keep the
 * directory size -- and thus the per-readdir cost -- INDEPENDENT of the reader
 * count, the namespace is owned only by the writers (writers_own below).
 */
static int    readdir_mode = 0;
/*
 * --dpath: the reader threads call dc_dentry_path() on a random leaf's HANDLE
 * (pinned once at seed time) instead of looking its path up -- the reverse walk
 * behind getcwd / readlink(/proc/PID/fd/N).  Works in split or homogeneous mode.
 */
static int    dpath_mode   = 0;
/*
 * WRITER-OWNED NAMESPACE (role-split --readdir, --dpath and --hit-current, with
 * at least one writer): the readers own no leaves, and the writers own the whole
 * namespace, g_nnames = nwriters*leaves, writer w owning [w*leaves ..).  So the
 * namespace does not grow with the reader count, and every leaf a dpath or hit
 * reader targets is one the writers are moving.
 *
 * ⛔ Until 2026-09-30 only readdir did this.  dpath and hit readers each owned
 * `leaves` names that NOTHING ever moves and drew their target from all of
 * them, so only nwriters/nthreads of the targets moved: 80% at 2 readers + 8
 * writers, 20% at 32, 4% at 184.  The reader axis of those panels confounded
 * the reader count with the share of targets in motion, and the growing set of
 * static leaves grew each reader's working set too (L1 misses per hit 3.5 ->
 * 5.5 from 2 to 32 readers, every engine).
 */
static int    writers_own  = 0;
/*
 * Negative dentries (default ON for lookup readers; --no-negatives restores the
 * legacy workload).  A reader asks for a leaf name in a RANDOM dir, and a leaf
 * lives in exactly one, so almost every lookup names something that does not
 * exist.  A kernel answers the first such lookup through lookup_slow, which
 * caches a NEGATIVE dentry, and every later one as a negative HIT on the RCU
 * fast path.  Without negatives this bench measured the MISS path instead --
 * ~99% of lookups, on every engine.  So: a PRIMING phase before the timed
 * window looks up every (dir, name) a reader can ask for and caches a negative
 * on each miss, and a reader that misses during the window (only at a name a
 * rename just vacated) caches one the same way.  Renames onto a negative
 * replace it, as d_move does.  readdir / dpath readers look nothing up, so a
 * kernel would hold no such negatives for them: no priming there.
 */
static int    negatives    = 1;
/*
 * --hit-current: the POSITIVE-hit reader.  Each owner publishes every token's
 * CURRENT (dir, name) in g_cur[] after each successful move or same-dir
 * rename (an exchange trades the objects behind two names, not the names), and
 * a reader looks up a random token's current path.  Every token is a writer's
 * (writers_own), so nearly every lookup is a positive hit on an object the
 * writers are moving right now, at any reader count -- the dense
 * reader/rename interaction the probing workload (1-2% of lookups on a moving
 * leaf) only grazes: d_seq retries on seqlock, shell resolution on txn.  The
 * read of g_cur[] is racy by design; a stale one names a vacated path, misses,
 * and caches a negative there as lookup_slow would.  No priming: these readers
 * do not probe.
 */
static int    hit_mode     = 0;
static uint32_t *g_cur;			/* [total] token -> (dir << 1) | alt */
static int    g_nnames     = 0;		/* effective leaf namespace size */
/*
 * Names [0, g_static_names) are owned by readers, which never rename, so after
 * priming a lookup of one can only miss if priming left a hole or something
 * dropped the negative: the miss counter for them should read 0.  Zero under
 * writers_own (the readers own nothing).
 */
static int    g_static_names = 0;

static unsigned int rename_thr;		/* rename_frac scaled into [0,1<<20) */
#define FRAC_BITS 20
#define FRAC_ONE (1u << FRAC_BITS)

/*
 * Leaf op mix, as cumulative thresholds over one FRAC_ONE die: [0,exch_thr) =
 * RENAME_EXCHANGE, [exch_thr,samedir_thr) = same-dir rename, the rest = cross-dir
 * move.  The defaults are the historical hardcoded mix: 1/8 exchange, no same-dir
 * rename.
 *
 * The draw order below (j0 first, then the op die) is the historical one.  The
 * generator itself changed (dcache_bench_rand.h: plain xorshift64 correlated the
 * low bits of consecutive draws), so no run reproduces an older op sequence;
 * what the order preserves is that an unmixed run makes the same draws as a
 * mixed one, so adding a weight shifts no later decision.
 */
static unsigned int exch_thr    = FRAC_ONE / 8;
static unsigned int samedir_thr = FRAC_ONE / 8;	/* == exch_thr => zero rename */

/* Parse `rename=A,move=B,exchange=C` (any subset; unnamed keys are an error). */
static void parse_op_mix(const char *s)
{
	double w[3] = { 0.0, 0.0, 0.0 };		/* rename, move, exchange */
	double sum;

	while (*s) {
		const char *eq = strchr(s, '=');
		int which;
		char *end;

		if (!eq)
			goto bad;
		if (!strncmp(s, "rename", (size_t) (eq - s)) && eq - s == 6)
			which = 0;
		else if (!strncmp(s, "move", (size_t) (eq - s)) && eq - s == 4)
			which = 1;
		else if (!strncmp(s, "exchange", (size_t) (eq - s)) && eq - s == 8)
			which = 2;
		else
			goto bad;
		w[which] = strtod(eq + 1, &end);
		if (end == eq + 1 || w[which] < 0.0)
			goto bad;
		s = end;
		while (*s == ',' || *s == ' ')
			s++;
	}
	sum = w[0] + w[1] + w[2];
	if (sum <= 0.0)
		goto bad;
	exch_thr = (unsigned int) (w[2] / sum * (double) FRAC_ONE + 0.5);
	samedir_thr = exch_thr +
		(unsigned int) (w[0] / sum * (double) FRAC_ONE + 0.5);
	if (samedir_thr > FRAC_ONE)
		samedir_thr = FRAC_ONE;
	g_two_names = 1;
	g_alt_mask = 1;
	return;
bad:
	fprintf(stderr, "--op-mix wants rename=A,move=B,exchange=C "
		"(non-negative weights, at least one positive)\n");
	exit(2);
}

static struct dcache *g_dc;
/*
 * The namespace is tracked as a PERMUTATION over fixed name-slots (a plain
 * rename moves a name to a new dir; RENAME_EXCHANGE swaps which id carries two
 * names).  Indexed by global name token g = owner*leaves + j: g_final_dir[g] is
 * the dir the name L{g} ends in, g_final_id[g] is the id that ends up carrying
 * it.  Both are a permutation of the seed, so the census stays exact.
 */
static int      *g_final_dir;		/* [nthreads*leaves] final dir per name */
static int      *g_final_alt;		/* [nthreads*leaves] final L/M pick per token */
static uint64_t *g_final_id;		/* [nthreads*leaves] final id per name */
/*
 * Address-identity builds (dc_lookup_id_is_address): each leaf's content-host
 * ADDRESS, captured once at seed time.  A host is address-stable across renames
 * (they stack shells; the tail host never moves), so this is the rename-invariant
 * ground truth the torn-read / conservation checks compare against instead of a
 * logical id.  NULL on logical-id builds.
 */
static uintptr_t *g_leaf_addr;		/* [nthreads*leaves] seed-time host addr */
/*
 * Each leaf id's HANDLE for the reverse walk, pinned at seed time (before any
 * rename) with dc_lookup_dentry().  Valid for the whole run: this bench renames
 * and exchanges but never unlinks, which is the reference an open file holds.
 */
static struct dentry **g_leaf_dentry;
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

/* ---- path construction -------------------------------------------------- */

/*
 * Build the full leaf path /p0/../d{dir}/{L|M}{gid} into p.  @alt picks which of
 * the token's two reserved names to use (0 = L, the seeded one; 1 = M, its
 * same-dir rename partner).  Both spellings are the same length, so the two
 * names cost the walk exactly the same compare.
 */
static void mk_leaf_path_alt(struct dc_path *p, int dir, int gid, int alt)
{
	int i;

	dc_path_reset(p);
	if (precomp && g_leaf_q) {	/* assemble from prebuilt component qstrs
					 * (NULL during build_tree seeding: setup
					 * falls through to the snprintf path) */
		for (i = 0; i < g_prefix_len; i++)
			p->comp[p->ndepth++] = g_prefix_q[i];
		p->comp[p->ndepth++] = g_dir_q[dir];
		p->comp[p->ndepth++] = alt ? g_leaf_q_alt[gid] : g_leaf_q[gid];
		return;
	}
	for (i = 0; i < g_prefix_len; i++)
		dc_path_push(p, g_prefix[i]);
	{
		char buf[DC_NAME_MAX];

		snprintf(buf, sizeof(buf), "d%d", dir);
		dc_path_push(p, buf);
		snprintf(buf, sizeof(buf), "%c%d", alt ? 'M' : 'L', gid);
		dc_path_push(p, buf);
	}
}

/* Build the full leaf path at the token's seeded name L{gid}. */
static void mk_leaf_path(struct dc_path *p, int dir, int gid)
{
	mk_leaf_path_alt(p, dir, gid, 0);
}

/* Prebuild the path component qstrs for --precomp (once, after build_tree). */
static void build_qstr_tables(void)
{
	char buf[DC_NAME_MAX];
	int i;

	g_prefix_q = calloc((size_t) (g_prefix_len > 0 ? g_prefix_len : 1),
			    sizeof(*g_prefix_q));
	g_dir_q = calloc((size_t) ndirs, sizeof(*g_dir_q));
	g_leaf_q = calloc((size_t) g_nnames, sizeof(*g_leaf_q));
	for (i = 0; i < g_prefix_len; i++)
		dc_qstr_init(&g_prefix_q[i], g_prefix[i]);
	for (i = 0; i < ndirs; i++) {
		snprintf(buf, sizeof(buf), "d%d", i);
		dc_qstr_init(&g_dir_q[i], buf);
	}
	for (i = 0; i < g_nnames; i++) {
		snprintf(buf, sizeof(buf), "L%d", i);
		dc_qstr_init(&g_leaf_q[i], buf);
	}
	if (g_two_names) {
		g_leaf_q_alt = calloc((size_t) g_nnames, sizeof(*g_leaf_q_alt));
		for (i = 0; i < g_nnames; i++) {
			snprintf(buf, sizeof(buf), "M%d", i);
			dc_qstr_init(&g_leaf_q_alt[i], buf);
		}
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
	long long nlookups;	/* lookups, or readdir calls in --readdir mode */
	long long ndirents;	/* children enumerated (--readdir mode) */
	long long nrenames;	/* successful mutations of every kind */
	long long nexch;
	long long nsamedir;	/* of those, same-dir renames (--op-mix rename=) */
	long long lk_wrong;	/* POSITIVE hit whose id left its owner's range */
	long long npos, nneg, nabs;	/* lookup outcomes */
	long long nnegadd;	/* negatives cached by readers on a miss */
	long long nabs_static;	/* of nabs, misses on a name no writer moves */
	long long nprimed, nprime_fail;	/* priming: negatives cached / refused */
	long long errs;		/* single-owner rename/exchange failures */
};

/*
 * Report the FIRST failed mutation of the run, with its return code: a single
 * owner's rename cannot legitimately fail, so the count alone (which fails the
 * gate) says something broke but not what.  Error path only.
 */
static int first_err_reported;

static void report_err(const char *op, int rc, const struct dc_path *a,
		       const struct dc_path *b)
{
	if (uatomic_xchg(&first_err_reported, 1))
		return;
	fprintf(stderr, "first mutation error: %s rc=%d (%s)  %s/%s -> %s/%s\n",
		op, rc, strerror(-rc),
		a->ndepth > 1 ? a->comp[a->ndepth - 2].name : "",
		a->ndepth ? a->comp[a->ndepth - 1].name : "",
		b->ndepth > 1 ? b->comp[b->ndepth - 2].name : "",
		b->ndepth ? b->comp[b->ndepth - 1].name : "");
}

static void *worker(void *arg)
{
	struct warg *me = arg;
	int total = g_nnames;
	/* Per name-token j (the leaf seeded as L{base+j}): dir[j] = the dir it
	 * lives in; alt[j] = which of its two reserved names it currently wears
	 * (0 = L, 1 = M); who[j] = the id currently carrying that name.  A file
	 * move changes dir[j]; a same-dir rename flips alt[j]; a RENAME_EXCHANGE
	 * swaps who[] between two tokens (the ids trade names, the names stay).
	 * This owner is the sole writer of its tokens, so all three stay a
	 * deterministic permutation of the seed for the end-of-run census. */
	int *dir = calloc(leaves, sizeof(*dir));
	int *alt = calloc(leaves, sizeof(*alt));
	uint64_t *who = calloc(leaves, sizeof(*who));
	size_t pbytes = (size_t) pollute_kb * 1024;
	volatile unsigned char *pbuf = pollute ? malloc(pbytes) : NULL;
	size_t pcur = 0;
	uint64_t psum = 0;
	if (pbuf) memset((void *) pbuf, 1, pbytes);	/* fault in the app buffer */
	uint64_t s = 0x9e3779b97f4a7c15ULL ^ ((uint64_t) (me->id + 1) * 0x100000001b3ULL);
	struct call_rcu_data *crdp;
	/* role: <0 homogeneous (per-op frac mix); 1 pure writer; 0 pure reader.
	 * Writers are the LAST nwriters ids, so the reader set always occupies the
	 * same low cpus regardless of writer count -- otherwise the readers' NUMA
	 * placement would shift with W and confound the reader-throughput curve. */
	int role = (nwriters >= 0) ? (me->id >= nthreads - nwriters) : -1;
	/*
	 * Leaf ownership.  Normally every thread owns leaves [id*leaves ..).
	 * Under writers_own the readers own nothing; the writers own the whole
	 * fixed namespace, writer w (the w'th of the last nwriters ids) owning
	 * [w*leaves ..).  So the namespace -- and dir sizes -- do not grow with
	 * the reader count.
	 */
	int owns = !(writers_own && role == 0);
	int base = writers_own ? (me->id - (nthreads - nwriters)) * leaves
			       : me->id * leaves;
	long long ops = 0;
	struct pace pace;
	int i;

	pace_init(&pace, role == 1 ? rename_rate : 0.0, nwriters);
	pin_cpu(me->cpu);
	dc_register_thread();
	/* Per-worker RT call_rcu worker pinned to this CPU, as bench_txn_3skiplist
	 * does: reclaim (and the txn engine's async fold) drains in parallel rather
	 * than funnelling through the single default worker -- otherwise the fold
	 * backlog, being grace-period-bound, would cap the txn engine artificially. */
	crdp = create_call_rcu_data(URCU_CALL_RCU_RT, me->cpu);
	if (crdp)
		set_thread_call_rcu_data(crdp);

	if (owns)
		for (i = 0; i < leaves; i++) {
			dir[i] = (base + i) % ndirs;	/* mirror the seed distribution */
			who[i] = (uint64_t) (base + i);	/* name L{base+i} starts on base+i */
		}

	/*
	 * PRIMING (lookup readers only; see `negatives`): this worker's share of
	 * the directories, every name a reader can ask for there, looked up once
	 * and a negative cached on each miss -- the lookup_slow a kernel would
	 * have run on first use.  Split by directory so workers take disjoint dir
	 * locks, and done by EVERY engine the same way before the timed window.
	 */
	if (negatives && !readdir_mode && !dpath_mode && !hit_mode) {
		long long n = 0;
		int k, g, a;

		for (k = me->id; k < ndirs; k += nthreads)
			for (g = 0; g < total; g++)
				for (a = 0; a <= (int) g_alt_mask; a++) {
					struct dc_path p;

					mk_leaf_path_alt(&p, k, g, a);
					if (dc_lookup(g_dc, &p, NULL) == DC_ABSENT) {
						if (dc_add_negative(g_dc, &p) == 0)
							me->nprimed++;
						else
							me->nprime_fail++;
					}
					if ((++n & quiesce_mask) == 0)
						dc_quiescent();
				}
	}

	uatomic_inc(&nthreads_running);
	rcu_thread_offline();			/* don't stall GPs while parked */
	while (uatomic_read(&goflag) == GOFLAG_INIT)
		(void) poll(NULL, 0, 1);
	rcu_thread_online();

	while (uatomic_read(&goflag) == GOFLAG_RUN) {
		int do_rename = (role < 0)
			? ((int) (xtop(&s, FRAC_BITS) < rename_thr))
			: role;

		if (do_rename) {
			if (!pace_wait(&pace, &goflag, GOFLAG_RUN))
				break;			/* window closed while paced */
			int j0 = (int) xrange(&s, (uint32_t) leaves);
			unsigned int die = xtop(&s, FRAC_BITS);
			int j1 = -1;

			/* [0,exch_thr): RENAME_EXCHANGE of two of my own tokens
			 * -- the two ids trade names in one commit (the
			 * two-shells-in-one-commit path).  Both names always
			 * exist, so a single owner never fails. */
			if (leaves >= 2 && die < exch_thr) {
				j1 = (int) xrange(&s, (uint32_t) leaves);
				if (j1 == j0)
					j1 = (j1 + 1) % leaves;
			}

			if (j1 >= 0) {
				struct dc_path a, b;

				mk_leaf_path_alt(&a, dir[j0], base + j0, alt[j0]);
				mk_leaf_path_alt(&b, dir[j1], base + j1, alt[j1]);
				int rc = dc_rename_exchange(g_dc, &a, &b);

				if (rc == 0) {
					uint64_t t = who[j0];
					who[j0] = who[j1]; who[j1] = t;
					/* the NAMES stay put; the ids trade, so
					 * each token keeps its own dir/alt. */
					me->nrenames++;
					me->nexch++;
				} else {
					report_err("exchange", rc, &a, &b);
					me->errs++;
				}
			} else if (die < samedir_thr) {
				/* RENAME (same-dir file): flip this token between
				 * its two private names inside its CURRENT dir.
				 * Same parent on both sides, so the engine takes
				 * none of the cross_parent branch -- no ancestry
				 * cycle check, no d_moving lock, no reparent, one
				 * child head instead of two.  The partner name is
				 * reserved for this token and this thread owns it,
				 * so -EEXIST/-ENOENT cannot happen. */
				struct dc_path from, to;

				mk_leaf_path_alt(&from, dir[j0], base + j0, alt[j0]);
				mk_leaf_path_alt(&to, dir[j0], base + j0, !alt[j0]);
				int rc = dc_rename(g_dc, &from, &to);

				if (rc == 0) {
					alt[j0] = !alt[j0];
					if (hit_mode)
						uatomic_store(&g_cur[base + j0],
							(uint32_t) ((dir[j0] << 1) | alt[j0]),
							CMM_RELAXED);
					me->nrenames++;
					me->nsamedir++;
				} else {
					report_err("rename", rc, &from, &to);
					me->errs++;
				}
			} else {
				/* FILE MOVE (cross-dir): move this token's current
				 * name to a new dir (its target dir is empty of
				 * that name, so no -EEXIST; a single owner never
				 * sees -ENOENT). */
				struct dc_path from, to;
				int nd = (int) xrange(&s, (uint32_t) ndirs);

				if (nd == dir[j0])
					nd = (nd + 1) % ndirs;
				mk_leaf_path_alt(&from, dir[j0], base + j0, alt[j0]);
				mk_leaf_path_alt(&to, nd, base + j0, alt[j0]);
				int rc = dc_rename(g_dc, &from, &to);

				if (rc == 0) {
					dir[j0] = nd;
					if (hit_mode)
						uatomic_store(&g_cur[base + j0],
							(uint32_t) ((nd << 1) | alt[j0]),
							CMM_RELAXED);
					me->nrenames++;
				} else {
					report_err("move", rc, &from, &to);
					me->errs++;
				}
			}
		} else if (readdir_mode) {
			/* READDIR a random target dir /pfx/d{k}: enumerate its
			 * current children.  Soft POSIX semantics -- a child mid-
			 * rename may or may not appear -- but it must never tear or
			 * crash.  dir size is fixed (writers own the namespace), so
			 * readdir cost is independent of the reader count. */
			int k = (int) xrange(&s, (uint32_t) ndirs);
			struct dc_path p;
			long n;

			mk_dir_path(&p, k);
			n = dc_readdir(g_dc, &p, NULL, NULL);
			if (n < 0)
				me->errs++;
			else {
				me->nlookups++;
				me->ndirents += n;
			}
		} else if (dpath_mode) {
			/* REVERSE WALK: where is leaf i NOW?  Every leaf sits at
			 * exactly `depth` below the root whatever its renames did, so
			 * any other depth is wrong.  That is ALL this per-op check can
			 * see: an answer mixing two instants (old name + new dir) has
			 * the right depth and passes.  The exact path is checked for
			 * every handle after the run, i.e. only for permanent damage. */
			int li = (int) xrange(&s, (uint32_t) total);
			struct dc_path out;

			if (dc_dentry_path(g_dc, g_leaf_dentry[li], &out) != 0 ||
			    out.ndepth != (uint32_t) depth)
				me->lk_wrong++;
			me->nlookups++;
		} else {
			/* LOOKUP a random full leaf path.  A POSITIVE hit for name
			 * L{g} must carry an id in g's owner range -- a worker only
			 * renames/exchanges its OWN leaves, so L{g}'s carrier stays
			 * within [owner_base, owner_base+leaves).  This concurrent
			 * torn-read check needs the logical id, so it runs on KEEPID
			 * builds only; the address build cannot cheaply range-check a
			 * pointer per op (an exchange moves L{g} to another host's
			 * address), so it relies on the census + the exact post-run
			 * g_leaf_addr[g_final_id] check below. */
			int g = (int) xrange(&s, (uint32_t) total);
			uint64_t r = xrand(&s);
			int dr = (int) (((r >> 32) * (uint64_t) ndirs) >> 32);
			/* Which of the token's two names to ask for.  Taken from
			 * a bit of the dir draw BELOW the 32 the dir used, rather
			 * than a fresh draw: an extra xrand on the reader's hot
			 * loop is exactly the harness ALU that hid the 1-CL
			 * layout win once already.  g_alt_mask is 0 unless
			 * --op-mix, so a legacy run always asks for L{g}.  Asking
			 * for both names keeps the reader's hit rate independent
			 * of the rename weight (a renamed token wears M). */
			int ra = (int) ((r >> 20) & g_alt_mask);

			if (hit_mode) {		/* the token's CURRENT path (racy) */
				uint32_t v = uatomic_load(&g_cur[g], CMM_RELAXED);

				dr = (int) (v >> 1);
				ra = (int) (v & 1);
			}
			int owner_base = (g / leaves) * leaves;
			struct dc_path p;
			uint64_t id = ~0ULL;

			mk_leaf_path_alt(&p, dr, g, ra);
			switch (dc_lookup(g_dc, &p, &id)) {
			case DC_POSITIVE:
				me->npos++;
				if (!id_is_address() &&
				    (id < (uint64_t) owner_base ||
				     id >= (uint64_t) (owner_base + leaves)))
					me->lk_wrong++;
				break;
			case DC_NEGATIVE:
				me->nneg++;
				break;
			default:
				/* lookup_slow: cache the absence (-EEXIST if a
				 * racing rename or reader got there first). */
				me->nabs++;
				if (g < g_static_names)
					me->nabs_static++;
				if (negatives && dc_add_negative(g_dc, &p) == 0)
					me->nnegadd++;
				break;
			}
			me->nlookups++;
		}
		if (pollute) {			/* co-tenant: touch app cachelines */
			unsigned int t;
			for (t = 0; t < pollute; t++) {
				psum += pbuf[pcur];
				pcur += 64;
				if (pcur >= pbytes)
					pcur = 0;
			}
		}
		if ((++ops & quiesce_mask) == 0)
			dc_quiescent();		/* let grace periods advance */
	}
	g_pollute_sink = psum;			/* keep the pollution reads live */
	free((void *) pbuf);

	if (owns)
		for (i = 0; i < leaves; i++) {
			g_final_dir[base + i] = dir[i];
			g_final_alt[base + i] = alt[i];
			g_final_id[base + i] = who[i];
		}
	free(dir); free(alt); free(who);
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
	/* Seed every leaf.  -DDC_BENCH_FILE_LEAVES makes the leaves FILES, so a
	 * leaf rename/move is a FILE operation and the txn engine skips its
	 * walk-causality bump (the global-arm rescue); the default (directories)
	 * makes it a directory operation that bumps. */
	for (i = 0; i < g_nnames; i++) {
		mk_leaf_path(&p, i % ndirs, i);
#ifdef DC_BENCH_FILE_LEAVES
		if (dc_add_file(g_dc, &p, (uint64_t) i)) {
#else
		if (dc_add(g_dc, &p, (uint64_t) i)) {
#endif
			fprintf(stderr, "seed leaf %d failed\n", i);
			exit(2);
		}
	}
}

/* Uniform warm-up: touch every leaf once so both engines enter timing warm. */
static void warm(void)
{
	int total = g_nnames;
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
	    "  --readdir       => (split mode) readers enumerate a random dir instead\n"
	    "                     of a leaf lookup; only writers own the namespace so\n"
	    "                     dir size is fixed as readers scale.\n"
	    "  --rename-rate R => (split mode) pace the writers to R renames/s in\n"
	    "                     aggregate (\"250k\", \"1M\"); default unpaced.\n"
	    "  --hit-current   => readers look up each leaf's CURRENT path (published\n"
	    "                     by its owner): positive hits on moving objects.\n"
	    "                     In split mode only writers own the namespace.\n"
	    "  --no-negatives  => (lookup readers) skip the negative-dentry priming\n"
	    "                     and the cache-on-miss: the legacy workload, whose\n"
	    "                     lookups are ~99%% misses.\n"
	    "  --dpath         => readers report a random leaf's CURRENT path from a\n"
	    "                     handle pinned at seed time (dc_dentry_path: the\n"
	    "                     kernel's dentry_path_raw) instead of a lookup.\n"
	    "                     In split mode only writers own the namespace.\n"
	    "  --op-mix rename=A,move=B,exchange=C\n"
	    "                  => weight the leaf ops of the taxonomy.  `rename` is\n"
	    "                     SAME-dir (the token flips between its two reserved\n"
	    "                     names), so it skips the cross_parent branch --\n"
	    "                     ancestry cycle check, d_moving lock, reparent -- that\n"
	    "                     `move` runs.  Default 0/7/1 == the historical mix.\n"
	    "  --quiesce N     => announce a QSBR quiescent state every N ops (power\n"
	    "                     of two, default 16); coarser trades a little smp_mb\n"
	    "                     tax for longer GPs -- a net LOSS for the GP-bound\n"
	    "                     txn fold (measure it).\n"
	    "  --depth N       => leaf path depth (>=2): a spine of N-2 static dirs,\n"
	    "                     then d{k}, then the leaf.  Widens the walk window.\n"
	    "  --no-precomp    => pay the per-lookup snprintf+hash in the timed loop\n"
	    "                     (legacy).  Default precomputes the component qstrs so\n"
	    "                     the timed loop does not, exposing the layout A/B.\n",
	    p);
	exit(2);
}

int main(int argc, char **argv)
{
	pthread_t *tid;
	struct warg *wa;
	struct census c;
	long long t0, t1, total_lk = 0, total_rn = 0, total_ex = 0, total_sd = 0;
	long long total_wrong = 0, total_err = 0, total_dirents = 0;
	long long total_pos = 0, total_neg = 0, total_abs = 0, total_negadd = 0;
	long long total_abs_static = 0, total_primed = 0, total_prime_fail = 0;
	unsigned long retries0 = 0, retries1 = 0;
	long dpath_bad = 0;
	double secs, mlk_s, mrn_s, mdir_s;
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
		else if (!strcmp(argv[i], "--pollute"))     pollute = (unsigned) atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pollute-kb"))  pollute_kb = (unsigned) atoi(argv[++i]);
		else if (!strcmp(argv[i], "--precomp"))     precomp = 1;
		else if (!strcmp(argv[i], "--no-precomp"))  precomp = 0;
		else if (!strcmp(argv[i], "--writers"))     nwriters = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--readdir"))     readdir_mode = 1;
		else if (!strcmp(argv[i], "--dpath"))       dpath_mode = 1;
		else if (!strcmp(argv[i], "--no-negatives")) negatives = 0;
		else if (!strcmp(argv[i], "--hit-current")) hit_mode = 1;
		else if (!strcmp(argv[i], "--rename-rate")) {
			rename_rate = pace_parse_rate(argv[++i]);
			if (rename_rate < 0.0) {
				fprintf(stderr, "--rename-rate: bad rate '%s'\n",
					argv[i]);
				exit(2);
			}
		}
		else if (!strcmp(argv[i], "--op-mix"))      parse_op_mix(argv[++i]);
		else if (!strcmp(argv[i], "--quiesce")) {
			int q = atoi(argv[++i]);
			if (q < 1 || (q & (q - 1)) != 0) {
				fprintf(stderr, "--quiesce N must be a power of two\n");
				exit(2);
			}
			quiesce_mask = (unsigned) (q - 1);
		}
		else usage(argv[0]);
	}
	if (nthreads < 1 || ndirs < 2 || depth < 2 || leaves < 1 ||
	    rename_frac < 0.0 || rename_frac > 1.0 || nwriters > nthreads)
		usage(argv[0]);
	if (hit_mode && (readdir_mode || dpath_mode)) {
		fprintf(stderr, "--hit-current selects the lookup reader; it "
			"excludes --readdir and --dpath\n");
		exit(2);
	}
	if (readdir_mode && dpath_mode) {
		fprintf(stderr, "--readdir and --dpath are exclusive reader ops\n");
		exit(2);
	}
	if (rename_rate > 0.0 && nwriters < 1) {
		fprintf(stderr, "--rename-rate paces the dedicated writers: it "
			"requires role-split with --writers >= 1\n");
		exit(2);
	}
	if (readdir_mode && nwriters < 1) {
		fprintf(stderr, "--readdir requires role-split with --writers >= 1 "
			"(the writers own the fixed namespace the readers list)\n");
		exit(2);
	}
	if (cpulist && nthreads > cpulist_len) {
		fprintf(stderr, "--cpulist has %d cpus but --nthreads=%d "
			"(would double up on a core)\n", cpulist_len, nthreads);
		exit(2);
	}
	if (depth - 2 > DC_PATH_MAX - 2)
		depth = DC_PATH_MAX;		/* leave room for d{k} + leaf */
	g_prefix_len = depth - 2;
	rename_thr = (unsigned int) (rename_frac * (double) FRAC_ONE + 0.5);
	/* Readers that target the writers' objects own no leaves (writers_own).
	 * With zero writers nothing moves and the readers keep their own names. */
	writers_own = nwriters >= 1 && (readdir_mode || dpath_mode || hit_mode);
	g_nnames = writers_own ? nwriters * leaves : nthreads * leaves;
	g_static_names = (nwriters >= 0 && !writers_own)
		? (nthreads - nwriters) * leaves : 0;
	total = g_nnames;

	/* Setup memory lives on the first worker's node, from exec on (see
	 * dcache_bench_setup.h: why not a CPU pin, and why not only here). */
	dc_bench_setup_on_cpu(cpulist ? cpulist[0] : 0, argv);
	rcu_register_thread();
	g_dc = dc_create(nbuckets);
	g_final_dir = calloc(total, sizeof(*g_final_dir));
	g_final_alt = calloc(total, sizeof(*g_final_alt));
	g_final_id = calloc(total, sizeof(*g_final_id));
	g_cur = calloc(total, sizeof(*g_cur));
	for (i = 0; i < total; i++)		/* seeded at L{i} in dir i % ndirs */
		g_cur[i] = (uint32_t) ((i % ndirs) << 1);

	printf("== bench_dcache (engine: %s) ==\n", dc_engine_name());
	if (g_two_names)
		printf("op-mix: exchange=%.4f same-dir-rename=%.4f cross-dir-move=%.4f "
		       "(leaf type: %s)\n",
		       (double) exch_thr / FRAC_ONE,
		       (double) (samedir_thr - exch_thr) / FRAC_ONE,
		       (double) (FRAC_ONE - samedir_thr) / FRAC_ONE,
#ifdef DC_BENCH_FILE_LEAVES
		       "file");
#else
		       "directory");
#endif
	if (nwriters >= 0)
		printf("threads=%d SPLIT(writers=%d readers=%d) reader-op=%s ndirs=%d "
		       "depth=%d leaves/thr=%d duration_ms=%ld total_leaves=%d "
		       "children/dir~%d\n",
		       nthreads, nwriters, nthreads - nwriters,
		       readdir_mode ? "readdir" : dpath_mode ? "dpath" :
		       hit_mode ? "lookup-current" : "lookup",
		       ndirs, depth,
		       leaves, duration_ms, total, ndirs ? total / ndirs : 0);
	else
		printf("threads=%d rename_frac=%.4f reader-op=%s ndirs=%d depth=%d "
		       "leaves/thr=%d duration_ms=%ld total_leaves=%d\n",
		       nthreads, rename_frac, dpath_mode ? "dpath" :
		       hit_mode ? "lookup-current" : "lookup",
		       ndirs, depth, leaves, duration_ms, total);

	build_tree();
	if (precomp)
		build_qstr_tables();
	warm();

	/* Address-identity: capture each leaf's host address at its seed dir
	 * (i % ndirs) before any rename runs; it is invariant thereafter. */
	if (id_is_address()) {
		g_leaf_addr = calloc(total, sizeof(*g_leaf_addr));
		for (i = 0; i < total; i++) {
			struct dc_path p;
			uint64_t a = 0;

			mk_leaf_path(&p, i % ndirs, i);
			if (dc_lookup(g_dc, &p, &a) != DC_POSITIVE) {
				fprintf(stderr, "seed addr capture: leaf %d missing\n", i);
				exit(2);
			}
			g_leaf_addr[i] = (uintptr_t) a;
		}
	}

	/* Pin every leaf's handle for the reverse walk, also before any rename. */
	g_leaf_dentry = calloc(total, sizeof(*g_leaf_dentry));
	for (i = 0; i < total; i++) {
		struct dc_path p;

		mk_leaf_path(&p, i % ndirs, i);
		g_leaf_dentry[i] = dc_lookup_dentry(g_dc, &p);
		if (!g_leaf_dentry[i]) {
			fprintf(stderr, "seed handle capture: leaf %d missing\n", i);
			exit(2);
		}
	}

	tid = calloc(nthreads, sizeof(*tid));
	wa = calloc(nthreads, sizeof(*wa));
	dc_bench_setup_done();		/* workers allocate node-locally, as before */
	for (i = 0; i < nthreads; i++) {
		wa[i].id = i;
		wa[i].cpu = cpulist ? cpulist[i] : i * cpustride;
		pthread_create(&tid[i], NULL, worker, &wa[i]);
	}
	rcu_thread_offline();	/* workers prime for a while: do not stall GPs */
	while (uatomic_read(&nthreads_running) < nthreads)
		(void) poll(NULL, 0, 1);
	rcu_thread_online();
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
		total_sd    += wa[i].nsamedir;
		total_wrong += wa[i].lk_wrong;
		total_pos   += wa[i].npos;
		total_neg   += wa[i].nneg;
		total_abs   += wa[i].nabs;
		total_negadd += wa[i].nnegadd;
		total_abs_static += wa[i].nabs_static;
		total_primed += wa[i].nprimed;
		total_prime_fail += wa[i].nprime_fail;
		total_err   += wa[i].errs;
		total_dirents += wa[i].ndirents;
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
	mdir_s = secs > 0 ? (double) total_dirents / secs / 1e6 : 0.0;

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
		mk_leaf_path_alt(&p, g_final_dir[i], i, g_final_alt[i]);
		/* presence always; then the exact identity: name L{i} is finally
		 * carried by id g_final_id[i], whose host address is the seed-time
		 * g_leaf_addr[g_final_id[i]] (a host keeps its id, address-stable).
		 * The logical build checks the id directly. */
		if (dc_lookup(g_dc, &p, &id) != DC_POSITIVE ||
		    (id_is_address()
			 ? (id != (uint64_t) g_leaf_addr[g_final_id[i]])
			 : (id != g_final_id[i])))
			anomaly++;
		/* Reverse: the object now carrying name L{i} (id g_final_id[i])
		 * must report exactly that path from its seed-time handle. */
		{
			struct dc_path out;
			uint32_t k;
			int bad;

			bad = dc_dentry_path(g_dc, g_leaf_dentry[g_final_id[i]],
					     &out) != 0 || out.ndepth != p.ndepth;
			for (k = 0; !bad && k < p.ndepth; k++)
				bad = !dc_qstr_eq(&out.comp[k], &p.comp[k]);
			if (bad) {
				dpath_bad++;
				anomaly++;
			}
		}
	}

	printf("duration (s): %g\n", secs);
	/* In --readdir / --dpath mode Mlookups/s is the readdir / reverse-walk
	 * CALL rate (kept under the same field name so the sweep harness parses one
	 * column); the READDIR line adds the enumerated-children rate and the
	 * average dir size actually seen, the DPATH line the torn-answer count. */
	printf("LOOKUP  lookups: %lld  Mlookups/s: %g  wrong-id: %lld\n",
	       total_lk, mlk_s, total_wrong);
	if (!readdir_mode && !dpath_mode) {
		long long n = total_pos + total_neg + total_abs;

		/* What the reader panel actually measured: the terminal
		 * outcome mix (a hit is POSITIVE or NEGATIVE; ABSENT is the
		 * miss path, where the reader also caches a negative). */
		printf("LOOKUP  mix: positive %.2f%%  negative %.2f%%  absent %.2f%%"
		       "  (negatives cached by readers: %lld%s)\n",
		       n ? 100.0 * total_pos / n : 0.0,
		       n ? 100.0 * total_neg / n : 0.0,
		       n ? 100.0 * total_abs / n : 0.0, total_negadd,
		       negatives ? "" : "; --no-negatives");
		printf("LOOKUP  primed: %lld negatives (%lld refused)  "
		       "misses on never-moved names: %lld\n",
		       total_primed, total_prime_fail, total_abs_static);
	}
	if (readdir_mode)
		printf("READDIR dirents: %lld  Mdirents/s: %g  children/readdir~%g\n",
		       total_dirents, mdir_s,
		       total_lk ? (double) total_dirents / (double) total_lk : 0.0);
	if (dpath_mode)
		printf("DPATH   reverse walks: %lld  Mdpaths/s: %g  depth-mismatch: %lld\n",
		       total_lk, mlk_s, total_wrong);
	/* renames = every mutation; the breakdown names the taxonomy cells --
	 * same-dir = `rename`, exchange = the two-shell commit, and the balance
	 * (renames - same-dir - exchanges) = cross-dir `file move`. */
	printf("RENAME  renames: %lld  exchanges: %lld  same-dir: %lld  "
	       "cross-dir: %lld  Mrenames/s: %g  errors: %lld\n",
	       total_rn, total_ex, total_sd, total_rn - total_ex - total_sd,
	       mrn_s, total_err);
	if (rename_rate > 0.0)
		printf("PACE    target Mrenames/s: %g  achieved: %.1f%%\n",
		       rename_rate / 1e6, 100.0 * mrn_s * 1e6 / rename_rate);
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
		       "rename-err %lld, dpath-mismatch %ld) -- run is CORRUPT, ignore "
		       "the numbers above\n",
		       anomaly, c.stray, total_wrong, total_err, dpath_bad);
	else
		printf("CHECK   conservation: OK (all %d leaves accounted for)\n", total);

	free(c.seen);
	free(tid); free(wa);
	dc_destroy(g_dc);
	free(g_final_dir); free(g_final_alt); free(g_final_id); free(g_leaf_addr);
	free(g_leaf_dentry);
	free(g_cur);
	rcu_unregister_thread();
	return anomaly ? 1 : 0;
}
