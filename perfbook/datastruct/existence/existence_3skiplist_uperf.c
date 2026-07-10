/*
 * existence_3skiplist_uperf.c: Test existence data structures for a set
 *	of three skiplists.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * Copyright (c) 2016-2019 Paul E. McKenney, IBM Corporation.
 * Copyright (c) 2019 Paul E. McKenney, Facebook.
 */

#include "stdarg.h"
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/time.h>
#include "../../api.h"
#define _GNU_SOURCE
#define _LGPL_SOURCE
#define RCU_SIGNAL
#include <urcu.h>
#include "../skiplist/skiplist.c"

#include "procon.h"
#include "existence.h"
#include "keyvalue.h"
#include "skiplist_exists.h"

/* Parameters for performance test. */
int nbuckets = 4096;
int nobjects;
int nreaders = 0;
int nupdaters = 1;
int updatewait = -1;
long updatespacing = 32;
int cpustride = 1;
long duration = 10; /* in milliseconds. */
long dump_procon_stats = 0;

atomic_t nthreads_running;
atomic_t nthreads_done;

#define GOFLAG_INIT 0
#define GOFLAG_RUN  1
#define GOFLAG_STOP 2

int goflag __attribute__((__aligned__(CACHE_LINE_SIZE))) = GOFLAG_INIT;

struct skiplist sl_array[3];

/* Per-test-thread attribute/statistics structure. */
struct perftest_attr {
	int myid;
	long long nlookups;
	long long nlookupfails;
	long long nrotations;
	long long nmoves;	/* LOCAL PATCH: exact key-moves, for ns/key-move */
	long long nadds;
	long long ndels;
	int mycpu;
	long firstkey;
	struct procon_stats kv_ps;
	struct procon_stats se_ps;
	struct procon_stats eg_ps;
};

/*
 * LOCAL PATCH (efficios-trie-benchmark): reader threads + a per-key-move metric
 * for the urcu-txn comparison, mirroring the same patch in
 * existence_3hash_uperf.c.  Upstream parses --nreaders but never spawns
 * readers, so --nreaders>0 hangs on the nthreads_running gate; this adds the
 * missing reader engine and fixes that.
 */
struct reader_attr {
	int myid;
	int mycpu;
	long long nqueries;
	long long nhits;
};

static inline unsigned long xrand(unsigned long *s)
{
	unsigned long x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return (*s = x);
}

/*
 * Rotate values through the three skiplists, shifting in the key
 * specified by nextkey.
 */
/* LOCAL PATCH: returns the number of key-moves committed by this flip, so the
 * caller can report ns/key-move (the work-unit-normalized metric). */
long skiplist_rotate(struct skiplist slp[], struct skiplist_exists *sei[],
		     struct skiplist_exists *seo[])
{
	struct existence_group *egp;
	long nmoves = 0;
	int i;

	egp = existence_group__procon_alloc();
	BUG_ON(!egp);
	existence_group_init(egp);
	rcu_read_lock();
	for (i = 0; i < nobjects; i += 3) {
		seo[i + 0] = skiplist_exists_alloc(egp, &slp[0],
						   sei[i + 2]->se_kv, ~0, ~0);
		seo[i + 1] = skiplist_exists_alloc(egp, &slp[1],
						   sei[i + 0]->se_kv, ~0, ~0);
		seo[i + 2] = skiplist_exists_alloc(egp, &slp[2],
						   sei[i + 1]->se_kv, ~0, ~0);
		BUG_ON(existence_head_set_outgoing(&sei[i + 0]->se_eh, egp));
		BUG_ON(existence_head_set_outgoing(&sei[i + 1]->se_eh, egp));
		BUG_ON(existence_head_set_outgoing(&sei[i + 2]->se_eh, egp));
		nmoves += 3;
	}
	rcu_read_unlock();
	existence_flip(egp);
	call_rcu(&egp->eg_rh, existence_group_rcu_cb);
#if 0
	if (atomic_read(&seo[0]->se_kv->refcnt) > 10000)
		poll(NULL, 0, 1);
#endif
	return nmoves;
}

/*
 * LOCAL PATCH: reader engine — membership query for a known-present key across
 * the three skiplists via skiplist_exists_lookup() (which pays
 * existence_exists() on the hit).  Read-side counterpart to
 * bench_txn_3skiplist's query().
 */
void *perftest_reader(void *arg)
{
	struct reader_attr *rp = arg;
	unsigned long seed = 0x9e3779b97f4a7c15UL ^ (unsigned long)(rp->myid + 1);
	long long nq = 0LL, nh = 0LL;

	rcu_register_thread();
	run_on(rp->mycpu);
	atomic_inc(&nthreads_running);
	while (ACCESS_ONCE(goflag) == GOFLAG_INIT)
		poll(NULL, 0, 1);
	while (ACCESS_ONCE(goflag) == GOFLAG_RUN) {
		int u = (int)(xrand(&seed) % (unsigned long)nupdaters);
		long off = (long)(xrand(&seed) % (unsigned long)(3 * nobjects));
		unsigned long key = (unsigned long)(u * updatespacing) +
				    (unsigned long)off;
		int t, hit = 0;

		rcu_read_lock();
		for (t = 0; t < 3; t++)
			if (skiplist_exists_lookup(&sl_array[t], key))
				hit = 1;
		rcu_read_unlock();
		nq++;
		nh += hit;
	}
	rcu_unregister_thread();
	rp->nqueries = nq;
	rp->nhits = nh;
	atomic_inc(&nthreads_done);
	return NULL;
}

void *perftest_child(void *arg)
{
	struct perftest_attr *childp = arg;
	struct call_rcu_data *crdp;
	struct existence_group *egp;
	struct skiplist_exists **sei;
	struct skiplist_exists **seo;
	int i;
	long long nrotations = 0LL;
	long long nmoves = 0LL;		/* LOCAL PATCH */

	rcu_register_thread();
	run_on(childp->mycpu);
	/* LOCAL PATCH: seed this thread's Park-Miller state.  randseed is a
	 * __thread variable that defaults to 0, and Schrage's algorithm maps
	 * 0 -> 2^31-1, which is a FIXED POINT.  Unseeded, random() therefore
	 * returns 2^31-1 forever, random_level() sees 31 trailing 1-bits and
	 * always returns SL_MAX_LEVELS-1, every node gets a full-height tower,
	 * every level becomes the complete list, and the skiplist degenerates
	 * to a sorted linked list with O(n) search.  Seed must be in [1, 2^31-2]. */
	setrandom((unsigned int)(childp->mycpu * 2654435761U + 12345U) % 0x7ffffffeU + 1U);
	crdp = create_call_rcu_data(URCU_CALL_RCU_RT, childp->mycpu);
	set_thread_call_rcu_data(crdp);
	keyvalue__procon_init();
	skiplist_exists__procon_init();
	existence_group__procon_init();
	atomic_inc(&nthreads_running);
	egp = existence_group__procon_alloc();
	BUG_ON(!egp);
	existence_group_init(egp);
	sei = calloc(sizeof(*sei), 3 * nobjects);
	seo = calloc(sizeof(*seo), 3 * nobjects);
	rcu_read_lock();
	for (i = 0; i < 3 * nobjects; i++)
		sei[i] = skiplist_exists_alloc(egp, &sl_array[i % 3], NULL,
					   childp->firstkey + i,
					   childp->firstkey + i);
	rcu_read_unlock();
	existence_flip(egp);
	call_rcu(&egp->eg_rh, existence_group_rcu_cb);
	while (ACCESS_ONCE(goflag) == GOFLAG_INIT)
		poll(NULL, 0, 1);
	while (ACCESS_ONCE(goflag) == GOFLAG_RUN) {
		nmoves += skiplist_rotate(sl_array, sei, seo);	/* LOCAL PATCH */
		for (i = 0; i < 3 * nobjects; i++)
			sei[i] = seo[i];
		nrotations++;
	}
	free(sei);
	free(seo);
	rcu_unregister_thread();
	childp->nrotations = nrotations;
	childp->nmoves = nmoves;		/* LOCAL PATCH */
	rcu_barrier();
	keyvalue__procon_stats(&childp->kv_ps);
	skiplist_exists__procon_stats(&childp->se_ps);
	existence_group__procon_stats(&childp->eg_ps);
	atomic_inc(&nthreads_done);
	set_thread_call_rcu_data(NULL);
	call_rcu_data_free(crdp);
	return NULL;
}


void perftest(void)
{
	struct perftest_attr *childp = calloc(sizeof(*childp), nupdaters);
	struct reader_attr *readp = nreaders ?			/* LOCAL PATCH */
		calloc(sizeof(*readp), nreaders) : NULL;
	int i;
	long long nrotations = 0LL;
	long long nmoves = 0LL;		/* LOCAL PATCH */
	long long nqueries = 0LL, nhits = 0LL;	/* LOCAL PATCH */
	long long starttime;
	long long endtime;
	struct procon_stats kv_pst = { 0 };
	struct procon_stats se_pst = { 0 };
	struct procon_stats eg_pst = { 0 };

	rcu_register_thread();
	keyvalue__procon_init();
	skiplist_exists__procon_init();
	existence_group__procon_init();

	rcu_read_lock();
	for (i = 0; i < 3; i++)
		skiplist_init(&sl_array[i], skiplist_exists_cmp);
	rcu_read_unlock();

	atomic_set(&nthreads_running, 0);
	goflag = GOFLAG_INIT;

	for (i = 0; i < nupdaters; i++) {
		childp[i].myid = i;
		childp[i].nlookups = 0LL;
		childp[i].nlookupfails = 0LL;
		childp[i].nrotations = 0LL;
		childp[i].nadds = 0LL;
		childp[i].ndels = 0LL;
		childp[i].mycpu = i * cpustride;
		childp[i].firstkey = i * updatespacing;
		create_thread(perftest_child, &childp[i]);
	}
	for (i = 0; i < nreaders; i++) {		/* LOCAL PATCH */
		readp[i].myid = i;
		readp[i].mycpu = (nupdaters + i) * cpustride;
		create_thread(perftest_reader, &readp[i]);
	}
	rcu_unregister_thread();

	/* Wait for all threads to initialize. */
	while (atomic_read(&nthreads_running) < nreaders + nupdaters)
		poll(NULL, 0, 1);
	smp_mb();

	/* Run the test. */
	starttime = get_microseconds();
	ACCESS_ONCE(goflag) = GOFLAG_RUN;
	do {
		poll(NULL, 0, duration);
		endtime = get_microseconds();
	} while (endtime - starttime < duration * 1000);
	starttime = endtime - starttime;
	ACCESS_ONCE(goflag) = GOFLAG_STOP;
	wait_all_threads();

	rcu_register_thread();
	for (i = 0; i < nupdaters; i++) {
		nrotations += childp[i].nrotations;
		nmoves += childp[i].nmoves;		/* LOCAL PATCH */
		procon_stats_accumulate(&kv_pst, &childp[i].kv_ps);
		procon_stats_accumulate(&se_pst, &childp[i].se_ps);
		procon_stats_accumulate(&eg_pst, &childp[i].eg_ps);
	}
	for (i = 0; i < nreaders; i++) {		/* LOCAL PATCH */
		nqueries += readp[i].nqueries;
		nhits += readp[i].nhits;
	}
	printf("duration (s): %g  rotations: %lld  ns/rotation: %g  obj/sl/thread: %d\n",
	       starttime / 1000. / 1000., nrotations,
	       (starttime * 1000. * (double)nupdaters) / (double)nrotations,
	       nobjects);
	/* LOCAL PATCH: work-unit-normalized update metric + reader throughput. */
	printf("UPDATE  updaters: %d  key-moves: %lld  Mmoves/s: %g  ns/key-move: %g\n",
	       nupdaters, nmoves,
	       (double)nmoves / (starttime / 1000. / 1000.) / 1e6,
	       nmoves ? (starttime * 1000. * (double)nupdaters) / (double)nmoves : 0.0);
	if (nreaders)
		printf("READ    readers: %d  queries: %lld  Mqueries/s: %g  hit%%: %.1f\n",
		       nreaders, nqueries,
		       (double)nqueries / (starttime / 1000. / 1000.) / 1e6,
		       nqueries ? 100.0 * (double)nhits / (double)nqueries : 0.0);
	if (dump_procon_stats) {
		printf("Key-value producer-consumer statistics:\n");
		procon_stats_print(&kv_pst);
		printf("Hash-exists producer-consumer statistics:\n");
		procon_stats_print(&se_pst);
		printf("Existence-group producer-consumer statistics:\n");
		procon_stats_print(&eg_pst);
	}
	free(childp);
	free(readp);		/* LOCAL PATCH (NULL-safe) */
	rcu_unregister_thread();
	rcu_barrier();
}

void usage(char *progname, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fprintf(stderr, "\t--nbuckets\n");
	fprintf(stderr, "\t\tNumber of buckets, defaults to 4096.\n");
	fprintf(stderr, "\t--nupdaters\n");
	fprintf(stderr, "\t\tNumber of updaters, defaults to 1.  Must be 1\n");
	fprintf(stderr, "\t\tor greater, or skiplist will be empty.\n");
	fprintf(stderr, "\t--nreaders\n");		/* LOCAL PATCH */
	fprintf(stderr, "\t\tNumber of membership-query readers, defaults\n");
	fprintf(stderr, "\t\tto 0.\n");
	fprintf(stderr, "\t--updatewait\n");
	fprintf(stderr, "\t\tNumber of spin-loop passes per update,\n");
	fprintf(stderr, "\t\tdefaults to -1.  If 0, the updater will not.\n");
	fprintf(stderr, "\t\tdo any updates, except for initialization.\n");
	fprintf(stderr, "\t\tIf negative, the updater waits for the\n");
	fprintf(stderr, "\t\tcorresponding number of milliseconds\n");
	fprintf(stderr, "\t\tbetween updates.\n");
	fprintf(stderr, "\t--updatespacing\n");
	fprintf(stderr, "\t\tKey values between successive updaters,\n");
	fprintf(stderr, "\t\tdefaults to 32.  Must be greater than 19.\n");
	fprintf(stderr, "\t--cpustride\n");
	fprintf(stderr, "\t\tStride when spreading threads across CPUs,\n");
	fprintf(stderr, "\t\tdefaults to 1.\n");
	fprintf(stderr, "\t--duration\n");
	fprintf(stderr, "\t\tDuration of test, in milliseconds.\n");
	fprintf(stderr, "\t--dump-procon-stats\n");
	fprintf(stderr, "\t\tDump procon memory-piping statistics.\n");
	exit(-1);
}

/*
 * Mainprogram.
 */
int main(int argc, char *argv[])
{
	int i = 1;
	void (*test_to_do)(void) = perftest;

	smp_init();

	while (i < argc) {
		if (strcmp(argv[i], "--nbuckets") == 0) {
			nbuckets = strtol(argv[++i], NULL, 0);
			if (nbuckets < 0)
				usage(argv[0],
				      "%s must be >= 0\n", argv[i - 1]);
		} else if (strcmp(argv[i], "--nreaders") == 0) {
			nreaders = strtol(argv[++i], NULL, 0);
			if (nreaders < 0)
				usage(argv[0],
				      "%s must be >= 0\n", argv[i - 1]);
		} else if (strcmp(argv[i], "--nupdaters") == 0) {
			nupdaters = strtol(argv[++i], NULL, 0);
			if (nupdaters < 1)
				usage(argv[0],
				      "%s must be >= 1\n", argv[i - 1]);
		} else if (strcmp(argv[i], "--updatewait") == 0) {
			updatewait = strtol(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--updatespacing") == 0) {
			updatespacing = strtol(argv[++i], NULL, 0);
			if (updatespacing < 20)
				usage(argv[0],
				      "%s must be >= 32\n", argv[i - 1]);
		} else if (strcmp(argv[i], "--cpustride") == 0) {
			cpustride = strtol(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--duration") == 0) {
			duration = strtol(argv[++i], NULL, 0);
			if (duration < 0)
				usage(argv[0],
				      "%s must be >= 0\n", argv[i - 1]);
		} else if (strcmp(argv[i], "--dump-procon-stats") == 0) {
			dump_procon_stats = 1;
		} else {
			usage(argv[0], "Unrecognized argument: %s\n",
			      argv[i]);
		}
		i++;
	}
	nobjects = (updatespacing - 16) / 3;
	test_to_do();
	return 0;
}
