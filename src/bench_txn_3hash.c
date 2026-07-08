// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// bench_txn_3hash: a urcu-txn (rcu-mcas) analogue of perfbook's
// datastruct/existence/existence_3hash_uperf.c, for a head-to-head between
// McKenney's "existence structure" and urcu-txn's multi-word CAS.
//
// SAME WORKLOAD, SAME METRIC as the existence uperf:
//   - 3 chained hash tables (ht[0..2]), --nbuckets each.
//   - --nupdaters threads, each owning a DISJOINT key range (firstkey =
//     id*updatespacing), holding K = 3*nobjects keys, nobjects=(updatespacing-16)/3.
//   - A "rotation" moves every one of the thread's K keys from its current
//     table t to table (t+1)%3.  Report rotations and ns/rotation, exactly like
//     existence_3hash_uperf ((elapsed_ns * nupdaters) / total_rotations).
//   - Updaters-only (existence's uperf spawns no reader threads either): this
//     isolates the COMMIT mechanism, not the read-side.
//
// THE ONE DIFFERENCE — the point of the comparison:
//   existence commits a whole batch of moves with ONE store to a shared
//   commit word (existence_flip), and every reader lookup pays a fixed
//   tagged-load "existence check" tax.  urcu-txn commits the same batch with
//   ONE MCAS transaction over the touched bucket slots (del old + insert new,
//   composed via the *_prepare forms), and readers traverse with NO tax.
//
//   So the axis this benchmark exposes is COMMIT WIDTH:
//     * existence_flip is O(1) in the batch size (one word, unbounded batch).
//     * an MCAS commit's cost grows with the number of slots it touches, and
//       its width is bounded by descriptor capacity.
//   --movesper controls how many key-moves share one MCAS commit (0 = attempt
//   the whole rotation in a single transaction, the true existence analogue;
//   a small value trades away batch-atomicity to stay within a cheap MCAS).
//
// Build (after `make urcu-txn`), mirroring the bench_list_scale recipe:
//   cc -O2 -pthread -D_GNU_SOURCE -D_LGPL_SOURCE \
//      -Iurcu-txn-build/include -c src/bench_txn_3hash.c -o src/bench_txn_3hash.o
//   cc -O2 -pthread -o bench_txn_3hash src/bench_txn_3hash.o \
//      -Lurcu-txn-build/src/.libs -Wl,-rpath,urcu-txn-build/src/.libs \
//      -lurcu-qsbr -lurcu-cds -lurcu-common -lpthread
// Run, comparably to existence_3hash_uperf:
//   ./bench_txn_3hash --nupdaters 4 --nbuckets 4096 --duration 1000 --movesper 4

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE
#endif

#include <errno.h>
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
#include <urcu/rcu-txn-hlist.h>
#include <urcu/rcu-txn.h>		/* include AFTER the RCU flavor */

/* ── knobs (names/defaults mirror existence_3hash_uperf) ──────────────── */
static long nbuckets      = 4096;
static int  nupdaters     = 1;
static int  nreaders      = 0;	/* concurrent membership-query threads */
static long updatespacing = 32;	/* key stride between updaters; sets nobjects */
static int  cpustride     = 1;
static long duration_ms   = 1000;
static int  movesper      = 4;	/* moves per MCAS commit; 0 = whole rotation */
static long nobjects;		/* = (updatespacing-16)/3, per existence */
static long keys_per_thread;	/* K = 3*nobjects */

/* ── data structure ───────────────────────────────────────────────────── */
struct hnode {
	struct urcu_txn_hlist_node node;	/* transacted bucket-chain node */
	unsigned long key;
	long val;
	struct rcu_head rh;
};

struct htab {
	struct urcu_txn_hlist_head *bucket;	/* [nbuckets] */
};

static struct htab ht[3];
/* ONE escalation domain shared by all three tables: a cross-table move is a
 * single transaction, so all slots it touches must share one fallback lane. */
static struct urcu_txn_domain g_dom;

static inline struct urcu_txn_hlist_head *bucket_of(int t, unsigned long key)
{
	return &ht[t].bucket[key % (unsigned long)nbuckets];
}

static void free_hnode_cb(struct rcu_head *rh)
{
	free(caa_container_of(rh, struct hnode, rh));
}

/* ── go-flag handshake (as in the existence harness) ──────────────────── */
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
	long long naborts;	/* commit ABORT retries (contention signal) */
};

static void pin_cpu(int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void)sched_setaffinity(0, sizeof(set), &set);
}

static long long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * Commit @cnt key-moves as ONE MCAS transaction: for each i, delete olds[i]
 * from its (current) bucket and insert the freshly-allocated news[i] at the
 * head of dst[i].  Both edges of every move are recorded via the composable
 * *_prepare forms and committed together — the direct analogue of one
 * existence_flip over the same batch.  Returns 0, or -ENOMEM if the batch
 * exceeds MCAS descriptor capacity (the "commit too wide" finding).
 * On success the caller reclaims each olds[i] after a grace period.
 */
static int commit_moves(struct hnode **olds, struct hnode **news,
			struct urcu_txn_hlist_head **dst, int cnt,
			long long *naborts)
{
	struct urcu_mcas_txn txn;
	int i, prep, st;

	urcu_txn_init(&txn, &g_dom);
	for (;;) {
		int retry = 0;

		urcu_txn_begin(&txn);
		for (i = 0; i < cnt; i++) {
			prep = urcu_txn_hlist_del_prepare(&txn, &olds[i]->node);
			if (prep == -EAGAIN) { retry = 1; break; }
			if (prep == -ENOENT) {
				/* disjoint single-owner keys: must not happen */
				fprintf(stderr, "del_prepare -ENOENT on owned key\n");
				abort();
			}
			prep = urcu_txn_hlist_insert_head_prepare(&txn,
					&news[i]->node, dst[i]);
			if (prep == -EAGAIN) { retry = 1; break; }
		}
		if (retry) {
			urcu_txn_conflict(&txn);
			urcu_txn_end(&txn);
			continue;
		}
		st = urcu_txn_commit(&txn);
		urcu_txn_end(&txn);
		if (st == URCU_TXN_STATUS_OK)
			return 0;
		if (st == URCU_TXN_STATUS_ABORT) {	/* contention: retry */
			(*naborts)++;
			continue;
		}
		return -ENOMEM;				/* MEMORY_ERROR: too wide */
	}
}

static void *updater(void *arg)
{
	struct updater_attr *me = arg;
	struct hnode **cur;	/* current node per key slot [K] */
	int *curtab;		/* current table index per key slot [K] */
	struct hnode **olds, **news;
	struct urcu_txn_hlist_head **dst;
	long K = keys_per_thread;
	long chunk = movesper > 0 ? movesper : K;
	long j;
	struct call_rcu_data *crdp;

	pin_cpu(me->cpu);
	rcu_register_thread();
	/* Per-updater RT call_rcu worker pinned to this CPU, exactly as the
	 * existence baseline (existence_3hash_uperf) does — so reclamation
	 * parallelism is identical on both sides, not funnelled through the
	 * single default worker. */
	crdp = create_call_rcu_data(URCU_CALL_RCU_RT, me->cpu);
	set_thread_call_rcu_data(crdp);

	cur    = calloc(K, sizeof(*cur));
	curtab = calloc(K, sizeof(*curtab));
	olds   = calloc(chunk, sizeof(*olds));
	news   = calloc(chunk, sizeof(*news));
	dst    = calloc(chunk, sizeof(*dst));

	/* Populate: key j starts in table j%3. */
	for (j = 0; j < K; j++) {
		struct hnode *e = calloc(1, sizeof(*e));
		e->key = me->firstkey + (unsigned long)j;
		e->val = (long)e->key;
		curtab[j] = (int)(j % 3);
		cur[j] = e;
		if (urcu_txn_hlist_add_rcu(&e->node, bucket_of(curtab[j], e->key),
					   &g_dom))
			abort();	/* -ENOMEM */
	}

	uatomic_inc(&nthreads_running);

	/* Wait for the start signal without stalling grace periods. */
	rcu_thread_offline();
	while (uatomic_read(&goflag) == GOFLAG_INIT)
		(void)poll(NULL, 0, 1);
	rcu_thread_online();

	while (uatomic_read(&goflag) == GOFLAG_RUN) {
		/* One rotation: move every key t -> (t+1)%3, committed in
		 * chunks of `chunk` moves (chunk==K => whole rotation atomic). */
		for (j = 0; j < K; ) {
			long n = 0;
			long base = j;

			for (; n < chunk && j < K; n++, j++) {
				struct hnode *e = cur[j];
				struct hnode *nn = calloc(1, sizeof(*nn));
				int nt = (curtab[j] + 1) % 3;

				nn->key = e->key;
				nn->val = e->val;
				olds[n] = e;
				news[n] = nn;
				dst[n]  = bucket_of(nt, nn->key);
			}
			if (commit_moves(olds, news, dst, (int)n, &me->naborts)) {
				fprintf(stderr,
					"commit too wide at %ld moves; lower --movesper\n",
					n);
				abort();
			}
			me->ncommits++;
			/* Publish the new nodes as current; reclaim the old. */
			for (long k = 0; k < n; k++) {
				long idx = base + k;
				call_rcu(&olds[k]->rh, free_hnode_cb);
				cur[idx] = news[k];
				curtab[idx] = (curtab[idx] + 1) % 3;
			}
		}
		me->nrotations++;
		rcu_quiescent_state();	/* let call_rcu grace periods advance */
	}

	/* Drain this thread's remaining nodes. */
	for (j = 0; j < K; j++)
		(void)urcu_txn_hlist_del_rcu(&cur[j]->node, &g_dom);
	for (j = 0; j < K; j++)
		call_rcu(&cur[j]->rh, free_hnode_cb);

	free(cur); free(curtab); free(olds); free(news); free(dst);
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

/* xorshift64 per-thread RNG (no shared rand() lock). */
static inline unsigned long xrand(unsigned long *s)
{
	unsigned long x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return (*s = x);
}

/*
 * One membership query: is @key present in the 3-table set?  Searches all
 * three bucket chains (a known-present key hits in exactly one) with plain RCU
 * traversal — NO existence-style per-lookup tax.  The read-side counterpart to
 * existence's hash_exists_lookup(), which additionally pays existence_exists()
 * on the hit.
 */
static int query(unsigned long key)
{
	int t, hit = 0;

	for (t = 0; t < 3; t++) {
		struct urcu_txn_hlist_node *n;

		for (n = urcu_txn_hlist_first_rcu(bucket_of(t, key)); n;
		     n = urcu_txn_hlist_next_rcu(n)) {
			if (caa_container_of(n, struct hnode, node)->key == key) {
				hit = 1;
				break;
			}
		}
	}
	return hit;
}

static void *reader(void *arg)
{
	struct reader_attr *me = arg;
	unsigned long seed = 0x9e3779b97f4a7c15UL ^ (unsigned long)(me->id + 1);
	long long nq = 0, nh = 0;

	pin_cpu(me->cpu);
	rcu_register_thread();

	uatomic_inc(&nthreads_running);
	rcu_thread_offline();
	while (uatomic_read(&goflag) == GOFLAG_INIT)
		(void)poll(NULL, 0, 1);
	rcu_thread_online();

	while (uatomic_read(&goflag) == GOFLAG_RUN) {
		/* A key definitely owned by some updater, so the query hits
		 * (and, on the existence side, pays the existence tax). */
		int u = (int)(xrand(&seed) % (unsigned long)nupdaters);
		long off = (long)(xrand(&seed) % (unsigned long)keys_per_thread);
		unsigned long key = (unsigned long)u * (unsigned long)updatespacing
				    + (unsigned long)off;

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
	for (t = 0; t < 3; t++) {
		ht[t].bucket = calloc(nbuckets, sizeof(*ht[t].bucket));
		for (long b = 0; b < nbuckets; b++)
			urcu_txn_hlist_init(&ht[t].bucket[b]);
	}

	for (i = 0; i < nupdaters; i++) {
		uat[i].id = i;
		uat[i].cpu = i * cpustride;
		uat[i].firstkey = (unsigned long)i * (unsigned long)updatespacing;
		pthread_create(&utid[i], NULL, updater, &uat[i]);
	}
	for (i = 0; i < nreaders; i++) {
		rat[i].id = i;
		rat[i].cpu = (nupdaters + i) * cpustride;	/* readers after updaters */
		pthread_create(&rtid[i], NULL, reader, &rat[i]);
	}

	while (uatomic_read(&nthreads_running) < nupdaters + nreaders)
		(void)poll(NULL, 0, 1);
	cmm_smp_mb();

	t0 = now_ns();
	uatomic_set(&goflag, GOFLAG_RUN);
	(void)poll(NULL, 0, (int)duration_ms);
	uatomic_set(&goflag, GOFLAG_STOP);
	t1 = now_ns();

	for (i = 0; i < nupdaters; i++)
		pthread_join(utid[i], NULL);
	for (i = 0; i < nreaders; i++)
		pthread_join(rtid[i], NULL);
	rcu_barrier();		/* flush outstanding call_rcu reclamations */

	for (i = 0; i < nupdaters; i++) {
		total_rot     += uat[i].nrotations;
		total_commits += uat[i].ncommits;
		total_aborts  += uat[i].naborts;
	}
	for (i = 0; i < nreaders; i++) {
		total_q    += rat[i].nqueries;
		total_hits += rat[i].nhits;
	}

	secs = (t1 - t0) / 1e9;
	total_moves = total_rot * keys_per_thread;	/* K key-moves per rotation */
	/* ns/key-move: per-thread time per move, comparable to existence once its
	 * own moves-per-rotation are counted (see design/txn-vs-existence-3hash.md). */
	ns_per_move = total_moves ?
		((double)(t1 - t0) * (double)nupdaters) / (double)total_moves : 0.0;
	mmoves_s = secs > 0 ? (double)total_moves / secs / 1e6 : 0.0;

	printf("engine: urcu-txn (rcu-mcas)  nbuckets: %ld  keys/thread: %ld  "
	       "moves/commit: ", nbuckets, keys_per_thread);
	if (movesper > 0)
		printf("%d\n", movesper);
	else
		printf("whole-rotation (%ld)\n", keys_per_thread);
	printf("duration (s): %g\n", secs);
	printf("UPDATE  updaters: %d  rotations: %lld  key-moves: %lld  "
	       "Mmoves/s: %g  ns/key-move: %g  (commits: %lld  aborts: %lld)\n",
	       nupdaters, total_rot, total_moves, mmoves_s, ns_per_move,
	       total_commits, total_aborts);
	if (nreaders) {
		double mq_s = secs > 0 ? (double)total_q / secs / 1e6 : 0.0;
		double hitpct = total_q ?
			100.0 * (double)total_hits / (double)total_q : 0.0;
		printf("READ    readers: %d  queries: %lld  Mqueries/s: %g  "
		       "hit%%: %.1f\n", nreaders, total_q, mq_s, hitpct);
	}

	for (t = 0; t < 3; t++)
		free(ht[t].bucket);
	free(utid); free(uat); free(rtid); free(rat);
}

static void usage(const char *p)
{
	fprintf(stderr,
	    "usage: %s [--nbuckets N] [--nupdaters N] [--nreaders N]\n"
	    "          [--updatespacing N] [--cpustride N] [--duration MS] [--movesper N]\n"
	    "  --nreaders N => concurrent membership-query threads (read-side)\n"
	    "  --movesper 0 => attempt the whole rotation in one MCAS commit\n"
	    "                  (true existence_flip analogue; may exceed MCAS width)\n",
	    p);
	exit(2);
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--nbuckets"))          nbuckets = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--nupdaters"))    nupdaters = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--nreaders"))     nreaders = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--updatespacing")) updatespacing = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--cpustride"))    cpustride = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--duration"))     duration_ms = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--movesper"))     movesper = (int)strtol(argv[++i], NULL, 0);
		else usage(argv[0]);
	}
	if (nupdaters < 1 || nbuckets < 1 || updatespacing < 20)
		usage(argv[0]);

	nobjects = (updatespacing - 16) / 3;		/* mirror existence */
	keys_per_thread = 3 * nobjects;
	if (keys_per_thread < 1)
		keys_per_thread = 3;

	run();
	return 0;
}
