// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * bench_txn_3skiplist -- urcu-txn (rcu-mcas) analogue of perfbook's existence
 * 3-SKIPLIST atomic-move microbench
 * (perfbook/datastruct/existence/existence_3skiplist_uperf.c).  The ORDERED-map
 * sibling of bench_txn_3hash: three ORDERED skiplists instead of three hash
 * tables.
 *
 * SAME WORKLOAD, SAME METRIC as the existence uperf:
 *   - --nupdaters threads, each owning a DISJOINT key range (firstkey =
 *     id * updatespacing), K = 3*nobjects keys, key j starting in skiplist j%3.
 *   - A "rotation" moves every one of the thread's K keys from its current
 *     skiplist to the next (t -> (t+1)%3).  Reported metric is ns per key-move,
 *     (elapsed_ns * nupdaters) / total_key_moves, matching existence's
 *     ns/rotation once its own key-moves-per-rotation are counted (see
 *     design/txn-vs-existence-3hash.md).
 *
 * Why the ORDERED axis is its OWN comparison (not just bench_txn_3hash again):
 * a skiplist move is structurally WIDER than a hash move.  Delete marks and
 * unlinks EVERY tower level and insert links every level (vs the hash's single
 * bucket-head edge), and both must SEARCH the ordered structure (O(log n))
 * rather than hash to a bucket.  So one MCAS commit covers more slots per move
 * and FEWER moves fit per commit than in the hash bench -- that width is the
 * point of the ordered comparison.  The existence side pays the ordered cost as
 * a per-lookup read tax (skiplist_exists); urcu-txn pays it as the write-side
 * commit width -- the read-tax vs write-tax dual, on the ordered structure.
 *
 * --movesper composes N structural moves into one commit (0 = a whole rotation).
 * Any N != 1 relies on read-your-own-writes -- the engine's default.  Why: a
 * chunk holds CONSECUTIVE keys j, j+1; key j rotates into sl[(j+1)%3], which is
 * exactly key j+1's source -- so one chunk always inserts and deletes in the same
 * skiplist, on adjacent keys, which therefore share a predecessor P.  Both edits
 * then name the slot P->next[L].
 *
 * Were the buffered writes invisible (the retired pre-RYW mode) each *_prepare
 * would search the COMMITTED structure, so both would present the same old value;
 * the engine keeps at most one record per slot (rcu-mcas.h urcu_mcas_record_chain),
 * matches on old, and the upgrade would overwrite new_ptr -- destroying the
 * insert's edge.  The stale read does double harm: it misdirects the write AND
 * makes the delete's predecessor guard pass.  The damage is not a lost key but a
 * TORN TOWER: the coalesce only bites at the levels both edits record, so a taller
 * new node keeps its upper-level edges and loses level 0, leaving next[0] aimed at
 * the tombstoned (then reclaimed) victim; every later search descending through it
 * adopts a marked predecessor and insert_prepare returns -EAGAIN forever.  That
 * failure was deterministic and reproduced with a SINGLE updater and no readers --
 * so it was composition, not contention.  Delete-first mirrored it: the victim
 * ended marked but still linked, hence a use-after-free plus a duplicate key.
 *
 * Read-your-own-writes is why this is sound: the descent observes the txn's own
 * pending edits, so the later edit lands on its true post-batch predecessor -- for
 * insert-first that is the freshly inserted node, which is txn-private and needs
 * no record at all, dissolving the collision -- and where the fused slot belongs
 * to a PUBLISHED node the two records chain into one {committed_old -> final_new}.
 * movesper == 1 needs none of this (del from skiplist A and insert into skiplist B
 * touch disjoint slots), so it declares its write set disjoint and takes the
 * age-0 blind append.
 *
 * Note movesper > 1 is a DIFFERENT unit of atomicity, not merely a faster one: it
 * makes several key-moves visible together.  It is not the like-for-like
 * comparison against existence's per-object flip -- movesper 1 is.  Reach for it
 * to measure what wider ordered commits cost, not to win the head-to-head.
 *
 * Every run ends with a conservation check (each updater's keys must be exactly
 * where it left them); a failure prints CONSERVATION FAILED and exits nonzero, so
 * a corrupted run cannot masquerade as a throughput result.  --nbuckets is
 * accepted (CLI parity with the existence sweeps) but IGNORED: no buckets.
 *
 * Example:
 *   ./bench_txn_3skiplist --nupdaters 4 --duration 1000 --movesper 1
 *   ./bench_txn_3skiplist --nupdaters 4 --duration 1000 --movesper 3   (batched)
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu-qsbr.h>			/* generic rcu_* names => QSBR flavor */
#include <urcu-call-rcu.h>
#include <urcu/rcu-txn-skiplist.h>
#include <urcu/rcu-txn.h>		/* include AFTER the RCU flavor */

/* ── knobs (names/defaults mirror existence_3skiplist_uperf) ──────────── */
static long nbuckets      = 4096;	/* accepted for CLI parity; IGNORED */
static int  nupdaters     = 1;
static int  nreaders      = 0;	/* concurrent membership-query threads */
static long updatespacing = 32;	/* key stride between updaters; sets nobjects */
static int  cpustride     = 1;
static long duration_ms   = 1000;
static int  movesper      = 1;	/* structural moves composed into one commit; 0 = whole rotation */
static long nobjects;		/* = (updatespacing-16)/3, per existence */
static long keys_per_thread;	/* K = 3*nobjects */

/* Keys that were not where the updater believed they were, at drain time: the
 * conservation check.  A batched move that drops an edge shows up here. */
static long long g_lost_keys;

/* Element: key/val plus the transacted skiplist tower.  The tower's flexible
 * next[] runs past the struct, so a node is malloc'd sizeof(*e) +
 * (toplevel+1)*sizeof(ptr); the sl member MUST be last. */
struct snode {
	unsigned long key;
	long val;
	struct rcu_head rh;
	struct urcu_txn_skiplist_node sl;	/* LAST: flexible next[] */
};

static struct urcu_txn_skiplist g_sl[3];
/* ONE escalation domain shared by all three skiplists: a cross-skiplist move is
 * a single composed transaction, so all three share the fair lane. */
static struct urcu_txn_domain g_dom;

static int snode_cmp(struct urcu_txn_skiplist_node *n, void *key)
{
	unsigned long a = caa_container_of(n, struct snode, sl)->key;
	unsigned long b = *(unsigned long *) key;

	return (a > b) - (a < b);
}

static struct snode *snode_alloc(unsigned long key, long val, unsigned int toplevel)
{
	struct snode *e = malloc(sizeof(*e)
			+ (toplevel + 1) * sizeof(struct urcu_txn_skiplist_node *));

	if (!e)
		abort();
	e->key = key;
	e->val = val;
	urcu_txn_skiplist_node_init(&e->sl, toplevel);
	return e;
}

static void free_snode_cb(struct rcu_head *rh)
{
	free(caa_container_of(rh, struct snode, rh));
}

#define GOFLAG_INIT 0
#define GOFLAG_RUN  1
#define GOFLAG_STOP 2
static volatile int goflag = GOFLAG_INIT;

static int nthreads_running;

struct updater_attr {
	int id;
	int cpu;
	unsigned long firstkey;
	long long nrotations;
	long long ncommits;
	long long naborts;
#ifdef URCU_TXN_ESCALATION_STATS
	long long esc_commits_true;	/* commits with >=1 real same-slot coincidence */
	long long esc_commits_bloom;	/* commits the 64-bit bloom would escalate */
	long long esc_raw_total;	/* sum of read-after-write hits */
	long long esc_waw_total;	/* sum of write-after-write hits */
	long long esc_bloom_total;	/* sum of bloom-would-escalate accesses */
#endif
};

static void pin_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void) sched_setaffinity(0, sizeof(set), &set);
}

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* xorshift64 per-thread RNG (no shared rand() lock). */
static inline unsigned long xrand(unsigned long *s)
{
	unsigned long x = *s;

	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return (*s = x);
}

/*
 * Commit @cnt key-moves as ONE MCAS transaction: for each i, delete keys[i]
 * from its current skiplist src_sl[i] and insert the freshly-allocated news[i]
 * (a new random tower carrying the same key) into dst_sl[i].  Both edges of
 * every move -- across all tower levels -- are recorded via the composable
 * *_prepare forms and committed together, the direct analogue of one
 * existence flip over the same batch.  del_prepare reports the removed node in
 * rem[i] for reclamation.  Returns 0, or -ENOMEM if the batch exceeds MCAS
 * descriptor capacity (the "commit too wide" finding -- reached sooner than the
 * hash bench because skiplist moves are wider).
 */
static int commit_moves(struct urcu_txn_skiplist **src_sl,
			struct urcu_txn_skiplist **dst_sl,
			unsigned long *keys, struct snode **news,
			struct urcu_txn_skiplist_node **rem, int cnt,
			struct updater_attr *me)
{
	struct urcu_mcas_txn txn;
	int i, prep, st;

	urcu_txn_init(&txn, &g_dom);
	/*
	 * Read-your-own-writes + chained same-slot stores is the engine default,
	 * and it is what composes more than one ordered edit per commit: a later
	 * _prepare then observes the txn's own pending edits instead of searching
	 * the committed structure and colliding on a shared pred->next[L] slot.
	 *
	 * A batched move (movesper > 1) is densely self-aliasing: the rotation maps
	 * consecutive keys onto a shared predecessor slot, so an AGE_ESCALATE build's
	 * age-0 optimistic attempt would read its own pending write and abort every
	 * time -- expect_conflict skips it and runs the sorted, RYW-resolved, blocking
	 * path from the first attempt (a no-op on other builds).  A single move
	 * (movesper == 1) instead touches distinct slots (del in src_sl, insert in
	 * dst_sl -- different skiplists), so its write set is disjoint by construction
	 * and it declares that to take the age-0 blind append with no Bloom.
	 */
	if (movesper != 1)
		urcu_txn_expect_conflict(&txn);
	else
		urcu_txn_declare_disjoint(&txn);
	for (;;) {
		int retry = 0;

		urcu_txn_begin(&txn);
		for (i = 0; i < cnt; i++) {
			prep = urcu_txn_skiplist_del_prepare(&txn, src_sl[i],
					&keys[i], &rem[i]);
			if (prep == -EAGAIN) { retry = 1; break; }
			if (prep == -ENOENT) {
				/* disjoint single-owner keys: must not happen */
				fprintf(stderr, "del_prepare -ENOENT on owned key %lu\n",
					keys[i]);
				abort();
			}
			prep = urcu_txn_skiplist_insert_prepare(&txn, dst_sl[i],
					&news[i]->sl, &keys[i]);
			if (prep == -EAGAIN) { retry = 1; break; }
			if (prep == -EEXIST) {
				/* key is only ever in one skiplist at a time */
				fprintf(stderr, "insert_prepare -EEXIST on owned key %lu\n",
					keys[i]);
				abort();
			}
		}
		if (retry) {
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;
		}
		st = urcu_txn_commit(&txn);
#ifdef URCU_TXN_ESCALATION_STATS
		if (st == URCU_TXN_STATUS_OK) {
			unsigned int raw = urcu_txn_esc_raw(&txn);
			unsigned int waw = urcu_txn_esc_waw(&txn);
			unsigned int bl  = urcu_txn_esc_bloom(&txn);

			me->esc_raw_total   += raw;
			me->esc_waw_total   += waw;
			me->esc_bloom_total += bl;
			if (raw + waw > 0)
				me->esc_commits_true++;
			if (bl > 0)
				me->esc_commits_bloom++;
		}
#endif
		urcu_txn_end(&txn);
		if (st == URCU_TXN_STATUS_OK)
			return 0;
		if (st == URCU_TXN_STATUS_ABORT) {	/* contention: retry */
			me->naborts++;
			continue;
		}
		return -ENOMEM;				/* MEMORY_ERROR: too wide */
	}
}

static void *updater(void *arg)
{
	struct updater_attr *me = arg;
	struct snode **cur;	/* current node per key slot [K] */
	int *curtab;		/* current skiplist index per key slot [K] */
	struct urcu_txn_skiplist **src_sl, **dst_sl;
	unsigned long *keys;
	struct snode **news;
	struct urcu_txn_skiplist_node **rem;
	unsigned long seed = 0x9e3779b97f4a7c15UL ^ ((unsigned long) me->id + 1);
	long K = keys_per_thread;
	long chunk = movesper > 0 ? movesper : K;
	long j;
	struct call_rcu_data *crdp;

	pin_cpu(me->cpu);
	rcu_register_thread();
	/* Per-updater RT call_rcu worker pinned to this CPU, exactly as the
	 * existence baseline (existence_3skiplist_uperf) does -- so reclamation
	 * parallelism is identical on both sides, not funnelled through the single
	 * default worker.  (Verified fine to >=8 updaters at movesper=1.) */
	crdp = create_call_rcu_data(URCU_CALL_RCU_RT, me->cpu);
	set_thread_call_rcu_data(crdp);

	cur    = calloc(K, sizeof(*cur));
	curtab = calloc(K, sizeof(*curtab));
	src_sl = calloc(chunk, sizeof(*src_sl));
	dst_sl = calloc(chunk, sizeof(*dst_sl));
	keys   = calloc(chunk, sizeof(*keys));
	news   = calloc(chunk, sizeof(*news));
	rem    = calloc(chunk, sizeof(*rem));

	/* Populate: key j starts in skiplist j%3, with a random tower height. */
	for (j = 0; j < K; j++) {
		unsigned int lvl = urcu_txn_skiplist_random_level(xrand(&seed));
		struct snode *e = snode_alloc(me->firstkey + (unsigned long) j,
				(long) (me->firstkey + (unsigned long) j), lvl);

		curtab[j] = (int) (j % 3);
		cur[j] = e;
		if (urcu_txn_skiplist_add_rcu(&g_sl[curtab[j]], &e->sl, &e->key,
					&g_dom))
			abort();	/* -ENOMEM / -EEXIST */
	}

	uatomic_inc(&nthreads_running);

	/* Wait for the start signal without stalling grace periods. */
	rcu_thread_offline();
	while (uatomic_read(&goflag) == GOFLAG_INIT)
		(void) poll(NULL, 0, 1);
	rcu_thread_online();

	while (uatomic_read(&goflag) == GOFLAG_RUN) {
		/* One rotation: move every key t -> (t+1)%3, committed in chunks
		 * of `chunk` moves (chunk==K => whole rotation atomic). */
		for (j = 0; j < K; ) {
			long n = 0;
			long base = j;
			long k;

			for (; n < chunk && j < K; n++, j++) {
				struct snode *e = cur[j];
				int nt = (curtab[j] + 1) % 3;
				unsigned int lvl =
					urcu_txn_skiplist_random_level(xrand(&seed));

				src_sl[n] = &g_sl[curtab[j]];
				dst_sl[n] = &g_sl[nt];
				keys[n]   = e->key;
				news[n]   = snode_alloc(e->key, e->val, lvl);
			}
			if (commit_moves(src_sl, dst_sl, keys, news, rem,
						(int) n, me)) {
				fprintf(stderr,
					"commit too wide at %ld moves; lower --movesper\n",
					n);
				abort();
			}
			me->ncommits++;
			/* Publish the new nodes as current; reclaim the old. */
			for (k = 0; k < n; k++) {
				long idx = base + k;

				call_rcu(&caa_container_of(rem[k], struct snode, sl)->rh,
						free_snode_cb);
				cur[idx] = news[k];
				curtab[idx] = (curtab[idx] + 1) % 3;
			}
		}
		me->nrotations++;
		rcu_quiescent_state();	/* let call_rcu grace periods advance */
	}

	/*
	 * Drain this thread's remaining nodes -- and CHECK conservation while doing
	 * it.  Every key this updater owns must still be exactly where it believes
	 * (curtab[j]), so del_rcu must report 1 ("this call removed it").  Anything
	 * else means a commit dropped or duplicated an edge: the failure mode of a
	 * batched ordered move without RYW.  Silently ignoring this return is how a
	 * corrupted run could still print a throughput number.
	 */
	for (j = 0; j < K; j++) {
		struct urcu_txn_skiplist_node *rm = NULL;

		if (urcu_txn_skiplist_del_rcu(&g_sl[curtab[j]], &cur[j]->key,
				&g_dom, &rm) != 1)
			uatomic_inc(&g_lost_keys);
	}
	for (j = 0; j < K; j++)
		call_rcu(&cur[j]->rh, free_snode_cb);

	free(cur); free(curtab); free(src_sl); free(dst_sl);
	free(keys); free(news); free(rem);
	rcu_unregister_thread();
	set_thread_call_rcu_data(NULL);
	call_rcu_data_free(crdp);	/* drains this worker's queue */
	return NULL;
}

/* ── reader engine (the read-side half of the comparison) ─────────────── */
struct reader_attr {
	int id;
	int cpu;
	long long nqueries;
	long long nhits;
};

/*
 * One membership query: is @key present in the 3-skiplist set?  A plain RCU
 * ordered descent per skiplist (a known-present key hits in exactly one) -- NO
 * existence-style per-lookup tax.  The read-side counterpart to existence's
 * skiplist_exists lookup, which additionally pays the existence check on a hit.
 */
static int query(unsigned long key)
{
	int t;

	for (t = 0; t < 3; t++)
		if (urcu_txn_skiplist_lookup_rcu(&g_sl[t], &key))
			return 1;
	return 0;
}

static void *reader(void *arg)
{
	struct reader_attr *me = arg;
	unsigned long seed = 0x9e3779b97f4a7c15UL ^ (unsigned long) (me->id + 1);
	long long nq = 0, nh = 0;

	pin_cpu(me->cpu);
	rcu_register_thread();

	uatomic_inc(&nthreads_running);
	rcu_thread_offline();
	while (uatomic_read(&goflag) == GOFLAG_INIT)
		(void) poll(NULL, 0, 1);
	rcu_thread_online();

	while (uatomic_read(&goflag) == GOFLAG_RUN) {
		/* A key definitely owned by some updater, so the query hits. */
		int u = (int) (xrand(&seed) % (unsigned long) nupdaters);
		long off = (long) (xrand(&seed) % (unsigned long) keys_per_thread);
		unsigned long key = (unsigned long) u * (unsigned long) updatespacing
				    + (unsigned long) off;

		rcu_read_lock();
		nh += query(key);
		rcu_read_unlock();
		nq++;
		rcu_quiescent_state();
	}

	rcu_unregister_thread();
	me->nqueries = nq;
	me->nhits = nh;
	return NULL;
}

static void run(void)
{
	pthread_t *utid = calloc(nupdaters, sizeof(*utid));
	struct updater_attr *uat = calloc(nupdaters, sizeof(*uat));
	pthread_t *rtid = nreaders ? calloc(nreaders, sizeof(*rtid)) : NULL;
	struct reader_attr *rat = nreaders ? calloc(nreaders, sizeof(*rat)) : NULL;
	long long total_rot = 0, total_commits = 0, total_aborts = 0;
	long long total_q = 0, total_hits = 0, total_moves;
	double secs, ns_per_move, mmoves_s;
	long long t0, t1;
	int i, t;

	urcu_txn_domain_init(&g_dom);
	for (t = 0; t < 3; t++)
		if (urcu_txn_skiplist_init(&g_sl[t], snode_cmp))
			abort();		/* -ENOMEM on head alloc */

	for (i = 0; i < nupdaters; i++) {
		uat[i].id = i;
		uat[i].cpu = i * cpustride;
		uat[i].firstkey = (unsigned long) i * (unsigned long) updatespacing;
		pthread_create(&utid[i], NULL, updater, &uat[i]);
	}
	for (i = 0; i < nreaders; i++) {
		rat[i].id = i;
		rat[i].cpu = (nupdaters + i) * cpustride;	/* readers after updaters */
		pthread_create(&rtid[i], NULL, reader, &rat[i]);
	}

	while (uatomic_read(&nthreads_running) < nupdaters + nreaders)
		(void) poll(NULL, 0, 1);
	cmm_smp_mb();

	t0 = now_ns();
	uatomic_set(&goflag, GOFLAG_RUN);
	(void) poll(NULL, 0, (int) duration_ms);
	uatomic_set(&goflag, GOFLAG_STOP);
	t1 = now_ns();

	for (i = 0; i < nupdaters; i++)
		pthread_join(utid[i], NULL);
	for (i = 0; i < nreaders; i++)
		pthread_join(rtid[i], NULL);
	rcu_barrier();		/* flush outstanding call_rcu reclamations */

#ifdef URCU_TXN_ESCALATION_STATS
	long long esc_ct = 0, esc_cb = 0, esc_r = 0, esc_w = 0, esc_b = 0;
#endif
	for (i = 0; i < nupdaters; i++) {
		total_rot     += uat[i].nrotations;
		total_commits += uat[i].ncommits;
		total_aborts  += uat[i].naborts;
#ifdef URCU_TXN_ESCALATION_STATS
		esc_ct += uat[i].esc_commits_true;
		esc_cb += uat[i].esc_commits_bloom;
		esc_r  += uat[i].esc_raw_total;
		esc_w  += uat[i].esc_waw_total;
		esc_b  += uat[i].esc_bloom_total;
#endif
	}
	for (i = 0; i < nreaders; i++) {
		total_q    += rat[i].nqueries;
		total_hits += rat[i].nhits;
	}

	secs = (t1 - t0) / 1e9;
	total_moves = total_rot * keys_per_thread;	/* K key-moves per rotation */
	ns_per_move = total_moves ?
		((double) (t1 - t0) * (double) nupdaters) / (double) total_moves : 0.0;
	mmoves_s = secs > 0 ? (double) total_moves / secs / 1e6 : 0.0;

	printf("engine: urcu-txn (rcu-mcas) 3-skiplist  keys/thread: %ld  "
	       "moves/commit: ", keys_per_thread);
	if (movesper > 0)
		printf("%d", movesper);
	else
		printf("whole-rotation (%ld)", keys_per_thread);
	printf("  mode: %s\n",
	       movesper != 1 ? "batched (chained)" : "single-move (disjoint)");
	printf("duration (s): %g\n", secs);
	printf("UPDATE  updaters: %d  rotations: %lld  key-moves: %lld  "
	       "Mmoves/s: %g  ns/key-move: %g  (commits: %lld  aborts: %lld)\n",
	       nupdaters, total_rot, total_moves, mmoves_s, ns_per_move,
	       total_commits, total_aborts);
#ifdef URCU_TXN_ESCALATION_STATS
	/*
	 * age-0/age-1 study: a commit "escalates" if any read or write inside it
	 * lands on a slot the same txn already recorded.  'true' counts the real
	 * same-slot coincidences (the FP-free lower bound, = the fundamental RYW
	 * rate); 'bloom' is what the 64-bit filter would actually trip, real hits
	 * plus false positives.  Low true-rate => age-0 fast path pays off.
	 */
	printf("ESCAL   commits: %lld  escalate-true: %lld (%.1f%%)  "
	       "escalate-bloom64: %lld (%.1f%%)  |  per-commit hits: raw %.2f  "
	       "waw %.2f  bloom %.2f\n",
	       total_commits,
	       esc_ct, total_commits ? 100.0 * (double) esc_ct / (double) total_commits : 0.0,
	       esc_cb, total_commits ? 100.0 * (double) esc_cb / (double) total_commits : 0.0,
	       total_commits ? (double) esc_r / (double) total_commits : 0.0,
	       total_commits ? (double) esc_w / (double) total_commits : 0.0,
	       total_commits ? (double) esc_b / (double) total_commits : 0.0);
#endif
	if (nreaders) {
		double mq_s = secs > 0 ? (double) total_q / secs / 1e6 : 0.0;
		double hitpct = total_q ?
			100.0 * (double) total_hits / (double) total_q : 0.0;
		printf("READ    readers: %d  queries: %lld  Mqueries/s: %g  "
		       "hit%%: %.1f\n", nreaders, total_q, mq_s, hitpct);
	}

	/*
	 * Conservation: every owned key had to be exactly where its updater left
	 * it.  Report loudly -- a dropped edge otherwise hides behind a perfectly
	 * plausible throughput number.
	 */
	if (g_lost_keys) {
		printf("CONSERVATION FAILED: %lld key(s) not where the updater "
		       "believed -- the run is CORRUPT, ignore the numbers above\n",
		       g_lost_keys);
	} else {
		printf("CHECK   conservation: OK (all %lld owned keys accounted for)\n",
		       (long long) keys_per_thread * nupdaters);
	}

	for (t = 0; t < 3; t++)
		urcu_txn_skiplist_destroy(&g_sl[t]);
	free(utid); free(uat); free(rtid); free(rat);
}

static void usage(const char *p)
{
	fprintf(stderr,
	    "usage: %s [--nbuckets N] [--nupdaters N] [--nreaders N]\n"
	    "          [--updatespacing N] [--cpustride N] [--duration MS] [--movesper N]\n"
	    "  --nbuckets N  => accepted for CLI parity with existence; IGNORED (no buckets)\n"
	    "  --nreaders N  => concurrent membership-query threads (read-side)\n"
	    "  --movesper N  => structural moves composed into one commit (0 = whole\n"
	    "                   rotation).  N != 1 composes several ordered edits under\n"
	    "                   the default read-your-own-writes; see the source header.\n",
	    p);
	exit(2);
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--nbuckets"))           nbuckets = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--nupdaters"))     nupdaters = (int) strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--nreaders"))      nreaders = (int) strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--updatespacing")) updatespacing = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--cpustride"))     cpustride = (int) strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--duration"))      duration_ms = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--movesper"))      movesper = (int) strtol(argv[++i], NULL, 0);
		else usage(argv[0]);
	}
	(void) nbuckets;			/* parsed for parity, not used */
	if (nupdaters < 1 || updatespacing < 20 || movesper < 0)
		usage(argv[0]);

	nobjects = (updatespacing - 16) / 3;		/* mirror existence */
	keys_per_thread = 3 * nobjects;
	if (keys_per_thread < 1)
		keys_per_thread = 3;

	run();
	return g_lost_keys ? 1 : 0;	/* a corrupt run must not exit 0 */
}
