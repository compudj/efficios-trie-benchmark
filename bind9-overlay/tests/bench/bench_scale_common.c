/*
 * Common driver for the per-engine read/write scaling benchmarks.
 * See bench_scale_common.h for the engine contract and the rationale for
 * running each engine in its own process.
 *
 * This translation unit is deliberately free of any trie/engine and of
 * liburcu/bind9 dependencies: all engine- and RCU-specific work happens
 * through the bench_engine callbacks, so engines that don't need liburcu or
 * libisc (Judy/qp/ART) link neither.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "bench_scale_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <numa.h>
#include <sys/mman.h>
#include <fcntl.h>

char  *str_keys[N_KEYS];
size_t str_lens[N_KEYS];
char  *churn_keys[CHURN_KEYS];
size_t churn_lens[CHURN_KEYS];

uint64_t xorshift64(uint64_t *s)
{
	uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x;
}

static const char *dns_domains[] = {
	"com.google","com.amazon","com.microsoft","com.apple",
	"com.meta","com.netflix","com.cloudflare","com.github",
	"org.wikipedia","org.mozilla","org.apache","org.kernel",
	"net.cloudflare","net.akamai","net.fastly","net.edgecast",
	"io.github","io.gitlab","io.docker","io.kubernetes",
	"dev.chromium","dev.flutter","dev.dart","dev.go",
};

long get_rss_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long rss = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "VmRSS:", 6) == 0) {
			sscanf(line + 6, " %ld", &rss);
			break;
		}
	}
	fclose(f);
	return rss;
}

void bench_gen_keys(void)
{
	uint64_t seed = 12345;

	/* Lookup keys (present in every structure at build time). */
	for (unsigned int i = 0; i < N_KEYS; i++) {
		char buf[128];
		int len = snprintf(buf, sizeof(buf), "%s.host%06u.zone%u",
			dns_domains[xorshift64(&seed) % N_DOMAINS],
			(unsigned)(xorshift64(&seed) % 100000),
			(unsigned)(xorshift64(&seed) % 50));
		str_keys[i] = strdup(buf);
		str_lens[i] = len;
	}

	/* Churn keys (disjoint from the lookup set; toggled by the writer). */
	seed = 77777;
	for (int i = 0; i < CHURN_KEYS; i++) {
		char buf[128];
		int len = snprintf(buf, sizeof(buf), "%s.churn%06u.zone%u",
			dns_domains[xorshift64(&seed) % N_DOMAINS],
			(unsigned)(xorshift64(&seed) % 100000),
			(unsigned)(xorshift64(&seed) % 50));
		churn_keys[i] = strdup(buf);
		churn_lens[i] = len;
	}
}

pthread_rwlock_t g_rwlock;

void bench_init_rwlock(void)
{
	pthread_rwlockattr_t attr;
	pthread_rwlockattr_init(&attr);
	pthread_rwlockattr_setkind_np(&attr,
		PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	pthread_rwlock_init(&g_rwlock, &attr);
	pthread_rwlockattr_destroy(&attr);
}

/* ── Run state ───────────────────────────────────────────── */

static volatile int start_flag;
static volatile int stop_flag;
/*
 * Number of reader threads that have finished any cache-priming warm-up and
 * are now spinning on start_flag.  run_bench() starts the timed window only
 * once this reaches nr_readers, so thread startup and priming never overlap
 * the measurement.  Reset per run.
 */
static volatile int prime_done_count;

/* The engine under test, set once by bench_scale_main(). */
static const struct bench_engine *g_eng;

struct thread_arg {
	unsigned long count;
	uint64_t seed;
	int cpu;
};

struct writer_arg {
	unsigned long writes;
	int cpu;
};

/*
 * Pin the calling thread to one logical CPU.  Workers are pinned worker i ->
 * CPU i; the sweep is capped at 192 threads and CPUs 0-191 are the first HW
 * thread of each physical core (siblings live at 192-383), so this gives one
 * thread per physical core with no SMT-sibling contention, matching load-names.
 * Pin BEFORE engine setup / priming so first-touch allocations land on the
 * pinned core's NUMA node.
 */
static void bench_pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
		perror("bench: sched_setaffinity");
}

/*
 * perf-stat gating.  When BENCH_PERF_CTL names the ctl FIFO of an external
 *   perf stat -D -1 --control fifo:$BENCH_PERF_CTL,$BENCH_PERF_ACK -- ...
 * write "enable"/"disable" around the timed window so perf counts ONLY the
 * measured 3s, not build / prime / teardown.  Reads one ack line (from
 * BENCH_PERF_ACK) to be sure perf applied the command before we proceed.
 * No-op when BENCH_PERF_CTL is unset.
 */
static void bench_perf_ctl(const char *cmd)
{
	static int ctl_fd = -2, ack_fd = -2;
	char buf[32];
	int n;

	if (ctl_fd == -2) {
		const char *cp = getenv("BENCH_PERF_CTL");
		const char *ap = getenv("BENCH_PERF_ACK");

		ctl_fd = cp ? open(cp, O_WRONLY) : -1;
		ack_fd = ap ? open(ap, O_RDONLY) : -1;
	}
	if (ctl_fd < 0)
		return;
	n = snprintf(buf, sizeof(buf), "%s\n", cmd);
	if (write(ctl_fd, buf, n) != n)
		return;
	if (ack_fd >= 0) {
		ssize_t r = read(ack_fd, buf, sizeof(buf));	/* sync on ack */
		(void) r;
	}
}

static void *reader_thread(void *arg)
{
	struct thread_arg *ta = arg;
	unsigned long count = 0;
	uint64_t seed = ta->seed;
	void *ctx;

	bench_pin_to_cpu(ta->cpu);
	ctx = g_eng->reader_setup ? g_eng->reader_setup() : NULL;

	/*
	 * Cache-priming phase: on by default, set BENCH_NO_PRIME to skip it.
	 * One untimed warm pass of ~N_KEYS lookups, using the same engine and
	 * random access pattern as the timed loop, so the measured window
	 * reflects steady-state cache / TLB behavior rather than post-build
	 * cold-start misses.  Identical pass for every engine (same count and
	 * access pattern via reader_batch), so priming is fair across tries.
	 */
	if (getenv("BENCH_NO_PRIME") == NULL) {
		uint64_t pseed = ta->seed;
		for (unsigned int done = 0; done < N_KEYS; done += BATCH_SIZE)
			g_eng->reader_batch(ctx, &pseed);
	}

	__atomic_fetch_add(&prime_done_count, 1, __ATOMIC_RELEASE);
	while (!start_flag)
		__asm__ __volatile__("pause" ::: "memory");

	while (!stop_flag) {
		g_eng->reader_batch(ctx, &seed);
		count += BATCH_SIZE;
	}

	if (g_eng->reader_teardown)
		g_eng->reader_teardown(ctx);

	ta->count = count;
	return NULL;
}

static void *writer_thread(void *arg)
{
	struct writer_arg *wa = arg;
	unsigned long writes = 0;
	uint64_t seed = 99999;
	void *ctx;

	bench_pin_to_cpu(wa->cpu);
	ctx = g_eng->writer_setup ? g_eng->writer_setup() : NULL;

	while (!start_flag)
		__asm__ __volatile__("pause" ::: "memory");

	while (!stop_flag) {
		g_eng->writer_step(ctx, &seed, writes);
		writes++;
	}

	if (g_eng->writer_teardown)
		g_eng->writer_teardown(ctx);

	wa->writes = writes;
	return NULL;
}

static void run_bench(int nr_readers, double *read_mops, double *write_kops)
{
	pthread_t *threads = calloc(nr_readers + 1, sizeof(pthread_t));
	struct thread_arg *rargs = calloc(nr_readers, sizeof(struct thread_arg));
	struct writer_arg warg = { .writes = 0 };
	struct timespec ts;
	unsigned long total_reads = 0;
	/*
	 * BENCH_NO_WRITER: drop the writer thread and run a pure read-only
	 * lookup-scaling sweep (the load-names workload character) instead of the
	 * read/write churn — no concurrent mutation, no grace periods, so this
	 * isolates lookup throughput with the engines on the same fair footing
	 * (validated lookups, key copies in the dense arena).
	 */
	int no_writer = (getenv("BENCH_NO_WRITER") != NULL);

	start_flag = 0;
	stop_flag = 0;
	prime_done_count = 0;

	if (g_eng->run_reset)
		g_eng->run_reset();

	for (int i = 0; i < nr_readers; i++) {
		rargs[i].count = 0;
		rargs[i].seed = 42 + (uint64_t)i * 1000;
		rargs[i].cpu = i;
		pthread_create(&threads[i], NULL, reader_thread, &rargs[i]);
	}
	if (!no_writer) {
		warg.cpu = nr_readers;	/* writer on the core past the readers */
		pthread_create(&threads[nr_readers], NULL, writer_thread, &warg);
	}

	/*
	 * Wait until every reader has finished any cache-priming warm-up pass
	 * and is spinning on start_flag, so the timed window excludes thread
	 * startup and priming.
	 */
	while (__atomic_load_n(&prime_done_count, __ATOMIC_ACQUIRE) < nr_readers)
		usleep(1000);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t t0 = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	__atomic_store_n(&start_flag, 1, __ATOMIC_RELEASE);

	usleep(DURATION_SEC * 1000000);
	__atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);

	for (int i = 0; i < nr_readers + (no_writer ? 0 : 1); i++)
		pthread_join(threads[i], NULL);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t t1 = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	double elapsed = (double)(t1 - t0) / 1e9;

	for (int i = 0; i < nr_readers; i++)
		total_reads += rargs[i].count;

	*read_mops = (double)total_reads / elapsed / 1e6;
	*write_kops = no_writer ? 0.0 : (double)warg.writes / elapsed / 1e3;

	if (g_eng->cleanup_churn)
		g_eng->cleanup_churn();

	free(threads);
	free(rargs);
}

void bench_arena_init(struct bench_arena *a, size_t cap_bytes)
{
	long pg = sysconf(_SC_PAGESIZE);
	if (pg <= 0)
		pg = 4096;
	cap_bytes = (cap_bytes + (size_t)pg - 1) & ~((size_t)pg - 1);
	void *p = mmap(NULL, cap_bytes, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap(bench_arena)");
		exit(1);
	}
	a->base = p;
	a->off = 0;
	a->cap = cap_bytes;
}

void *bench_arena_alloc(struct bench_arena *a, size_t sz, size_t align)
{
	size_t off = (a->off + (align - 1)) & ~(align - 1);
	if (off + sz > a->cap) {
		fprintf(stderr, "bench_arena_alloc: out of space "
			"(cap=%zu off=%zu sz=%zu)\n", a->cap, off, sz);
		abort();
	}
	a->off = off + sz;
	return a->base + off;
}

/* ── Mutator benchmark (BENCH_MUTATOR=1) ─────────────────────
 * One mutator thread runs timed insert-all / replace-all / remove-all phases
 * over the churn set while nr_readers readers do lookups, isolating each op's
 * throughput.  Sweeping readers 0 -> N shows how reader concurrency affects a
 * single mutator: RCU writers never block readers; rwlock writers serialize
 * with them; the concurrent tries restart (OLC) or node-exclude (ROWEX).
 */
struct mut_arg {
	uint64_t ns[3];
	uint64_t cnt[3];
	int cpu;
};

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void *mutator_thread(void *arg)
{
	struct mut_arg *ma = arg;
	void *ctx;
	int nops = g_eng->no_remove ? 2 : 3;	/* skip REMOVE phase if unsupported */

	bench_pin_to_cpu(ma->cpu);
	ctx = g_eng->writer_setup ? g_eng->writer_setup() : NULL;
	static const int order[3] = {
		BENCH_OP_INSERT, BENCH_OP_REPLACE, BENCH_OP_REMOVE
	};

	for (int i = 0; i < 3; i++) {
		ma->ns[i] = 0;
		ma->cnt[i] = 0;
	}

	while (!start_flag)
		__asm__ __volatile__("pause" ::: "memory");

	/*
	 * Chunked phases: each outer pass runs insert -> replace -> remove over a
	 * CHUNK-key sub-batch, advancing through the churn set.  Chunking (rather
	 * than one whole-set phase per op) ensures all three ops get sampled even
	 * when the mutator is slow — e.g. an rwlock writer starved under reader
	 * load would otherwise spend the whole window in the insert phase and
	 * report replace/remove = 0.  Each chunk fully cycles
	 * absent->present->present->absent within one pass, so at every chunk
	 * boundary (and at stop) at most the in-flight chunk is present; the drain
	 * below empties it.  CHUNK divides CHURN_KEYS evenly.
	 */
	const unsigned int CHUNK = 500;
	unsigned int base = 0;
	while (!stop_flag) {
		for (int oi = 0; oi < nops; oi++) {
			int op = order[oi];
			uint64_t t0 = mono_ns();
			for (unsigned int i = 0; i < CHUNK; i++)
				g_eng->writer_op(ctx, op, (base + i) % CHURN_KEYS);
			ma->ns[op] += mono_ns() - t0;
			ma->cnt[op] += CHUNK;
			if (stop_flag)
				break;
		}
		base += CHUNK;
		if (base >= CHURN_KEYS)
			base = 0;
	}

	/*
	 * Drain (untimed, not counted): leave every churn key absent so the next
	 * reader-count point starts from a known-empty churn set.  Without this,
	 * a point that stops after an insert/replace phase leaves keys present,
	 * and the next point's insert phase corrupts state (duplicate inserts /
	 * removes that miss).  Skipped for no_remove engines (their INSERT/REPLACE
	 * are idempotent upserts, so leftover-present is harmless).
	 */
	if (!g_eng->no_remove)
		for (unsigned int idx = 0; idx < CHURN_KEYS; idx++)
			g_eng->writer_op(ctx, BENCH_OP_REMOVE, idx);

	if (g_eng->writer_teardown)
		g_eng->writer_teardown(ctx);
	return NULL;
}

/* Per-op throughput in kops/s (insert, replace, remove) into out[3]. */
static void mutator_run_bench(int nr_readers, double out[3])
{
	pthread_t *threads = calloc(nr_readers + 1, sizeof(pthread_t));
	struct thread_arg *rargs =
		calloc(nr_readers ? nr_readers : 1, sizeof(struct thread_arg));
	struct mut_arg ma;

	start_flag = 0;
	stop_flag = 0;
	prime_done_count = 0;

	if (g_eng->run_reset)
		g_eng->run_reset();

	for (int i = 0; i < nr_readers; i++) {
		rargs[i].count = 0;
		rargs[i].seed = 42 + (uint64_t)i * 1000;
		rargs[i].cpu = i;
		pthread_create(&threads[i], NULL, reader_thread, &rargs[i]);
	}
	ma.cpu = nr_readers;	/* mutator on the core past the readers */
	pthread_create(&threads[nr_readers], NULL, mutator_thread, &ma);

	while (__atomic_load_n(&prime_done_count, __ATOMIC_ACQUIRE) < nr_readers)
		usleep(1000);

	bench_perf_ctl("enable");	/* count only the timed window */
	__atomic_store_n(&start_flag, 1, __ATOMIC_RELEASE);
	usleep(DURATION_SEC * 1000000);
	__atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);
	bench_perf_ctl("disable");

	for (int i = 0; i < nr_readers + 1; i++)
		pthread_join(threads[i], NULL);

	for (int op = 0; op < 3; op++)
		out[op] = ma.cnt[op]
			? (double)ma.cnt[op] / ((double)ma.ns[op] / 1e9) / 1e3
			: 0.0;

	if (g_eng->cleanup_churn)
		g_eng->cleanup_churn();

	free(threads);
	free(rargs);
}

/* ── Ordered-iteration benchmark (BENCH_ITERATE=1) ───────────
 * Read-only: each of nr_readers threads repeatedly traverses every key in
 * sorted order (engine->iterate), and we report aggregate "next" (key-visit)
 * throughput as readers scale 1 -> 192.  Full traversals stream the whole
 * structure, so this is largely a memory-bandwidth / read-scaling test of the
 * ordered cursor, distinct from the random point-lookup sweep.
 */
static void *iterate_reader_thread(void *arg)
{
	struct thread_arg *ta = arg;
	unsigned long count = 0;
	void *ctx;

	bench_pin_to_cpu(ta->cpu);
	ctx = g_eng->reader_setup ? g_eng->reader_setup() : NULL;

	if (getenv("BENCH_NO_PRIME") == NULL) {
		unsigned long visited = g_eng->iterate(ctx);	/* one warm traversal */
		if (getenv("BENCH_ITER_DEBUG"))
			fprintf(stderr, "[iter] %s one traversal visited %lu keys "
				"(expect %d)\n", g_eng->name, visited, N_KEYS);
	}

	__atomic_fetch_add(&prime_done_count, 1, __ATOMIC_RELEASE);
	while (!start_flag)
		__asm__ __volatile__("pause" ::: "memory");

	while (!stop_flag)
		count += g_eng->iterate(ctx);

	if (g_eng->reader_teardown)
		g_eng->reader_teardown(ctx);
	ta->count = count;
	return NULL;
}

/* Aggregate next-op throughput (Mops/s) for nr_readers iterating threads. */
static double iterate_run_bench(int nr_readers)
{
	pthread_t *threads = calloc(nr_readers, sizeof(pthread_t));
	struct thread_arg *rargs = calloc(nr_readers, sizeof(struct thread_arg));
	unsigned long total = 0;

	start_flag = 0;
	stop_flag = 0;
	prime_done_count = 0;

	for (int i = 0; i < nr_readers; i++) {
		rargs[i].count = 0;
		rargs[i].seed = 42 + (uint64_t)i * 1000;
		rargs[i].cpu = i;
		pthread_create(&threads[i], NULL, iterate_reader_thread, &rargs[i]);
	}

	while (__atomic_load_n(&prime_done_count, __ATOMIC_ACQUIRE) < nr_readers)
		usleep(1000);

	uint64_t t0 = mono_ns();
	bench_perf_ctl("enable");	/* count only the timed window */
	__atomic_store_n(&start_flag, 1, __ATOMIC_RELEASE);
	usleep(DURATION_SEC * 1000000);
	__atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);
	bench_perf_ctl("disable");

	for (int i = 0; i < nr_readers; i++)
		pthread_join(threads[i], NULL);
	uint64_t t1 = mono_ns();

	for (int i = 0; i < nr_readers; i++)
		total += rargs[i].count;

	free(threads);
	free(rargs);
	return (double)total / ((double)(t1 - t0) / 1e9) / 1e6;
}

int bench_scale_main(int argc, char **argv, const struct bench_engine *eng)
{
	int max_threads = 384;

	if (argc > 1)
		max_threads = atoi(argv[1]);

	g_eng = eng;

	/*
	 * NUMA interleaving, ON BY DEFAULT.  Spread every allocation this main
	 * thread faults in -- the query-key set AND the structure built below --
	 * across all NUMA nodes, so at high reader counts spanning multiple
	 * sockets no single node's memory bandwidth bottlenecks the shared read
	 * set.  Applied UNIFORMLY to whatever engine is under test (a process-wide
	 * mempolicy, set before bench_gen_keys() / eng->build()), so the
	 * cross-engine reference is fair: it interleaves qp/ART/etc. exactly like
	 * the Fractal Trie.  Without it, first-touch piles the whole shared
	 * structure onto this node and Linux auto-NUMA-balancing thrashes the
	 * unbound pages -- a ~10-20x MT cliff and high run-to-run variance on a
	 * many-node box.  No-op on a single-node machine.
	 *
	 * Opt out with BENCH_NUMA_INTERLEAVE=0 to measure the naive first-touch
	 * case (matches the FT library's CDS_FT_NUMA_INTERLEAVE=0 convention).
	 */
	{
		const char *ni = getenv("BENCH_NUMA_INTERLEAVE");

		if (!ni || ni[0] != '0') {
			if (numa_available() != -1) {
				numa_set_interleave_mask(numa_all_nodes_ptr);
				fprintf(stderr,
					"NUMA: interleaving allocations across all nodes (BENCH_NUMA_INTERLEAVE=0 to disable)\n");
			} else {
				fprintf(stderr,
					"NUMA: interleave requested but libnuma unavailable\n");
			}
		}
	}

	bench_gen_keys();
	eng->build();

	/* RSS of just this engine's structure, sampled after build. */
	long rss = get_rss_kb();
	fprintf(stderr,
		"%s (%s): %d keys built, RSS=%ld kB; 1 writer + N readers, %ds/point\n",
		eng->name, eng->label, N_KEYS, rss, DURATION_SEC);

	/* Machine-readable output consumed by scripts/run_scale_rw.sh. */
	printf("engine %s rss_kb %ld\n", eng->name, rss);

	/*
	 * Mutator-throughput sweep (BENCH_MUTATOR=1): one mutator thread doing
	 * insert/replace/remove, readers scaling 0 -> 191, per-op kops.  Requires
	 * the engine to provide writer_op; otherwise fall through to the default
	 * read-scaling sweep.
	 */
	if (getenv("BENCH_MUTATOR") != NULL && eng->writer_op != NULL) {
		int reader_counts[] = { 0, 1, 2, 4, 8, 16, 32, 64, 96, 128, 191 };
		size_t nrc = sizeof(reader_counts) / sizeof(reader_counts[0]);
		printf("# readers insert_kops replace_kops remove_kops\n");
		fflush(stdout);
		for (size_t i = 0; i < nrc; i++) {
			int nr = reader_counts[i];
			if (nr + 1 > max_threads)	/* +1 for the mutator */
				break;
			double out[3];
			mutator_run_bench(nr, out);
			printf("%d %.1f %.1f %.1f\n", nr, out[0], out[1], out[2]);
			fflush(stdout);
		}
		return 0;
	}

	/*
	 * Ordered-iteration sweep (BENCH_ITERATE=1): readers 1 -> 192, each looping
	 * a full in-order traversal; aggregate next-op throughput.  Read-only.
	 */
	if (getenv("BENCH_ITERATE") != NULL && eng->iterate != NULL) {
		int counts[] = { 1, 2, 4, 8, 16, 32, 64, 96, 128, 192 };
		size_t nrc = sizeof(counts) / sizeof(counts[0]);
		printf("# readers next_mops\n");
		fflush(stdout);
		for (size_t i = 0; i < nrc; i++) {
			int nr = counts[i];
			if (nr > max_threads)
				break;
			printf("%d %.1f\n", nr, iterate_run_bench(nr));
			fflush(stdout);
		}
		return 0;
	}

	printf("# readers read_mops write_kops\n");
	fflush(stdout);

	/*
	 * Reserve a core for the writer thread only when there is one
	 * (BENCH_NO_WRITER drops it).  This lets each sweep fill the 192
	 * physical cores exactly, with no SMT oversubscription:
	 *   - read/write (1 active writer): readers stop at 191 (191 + 1 = 192)
	 *   - read-only  (BENCH_NO_WRITER):  readers reach 192
	 * The shared list carries both 191 and 192; the guard picks the right
	 * top per mode.
	 */
	int no_writer = (getenv("BENCH_NO_WRITER") != NULL);
	int writer_slots = no_writer ? 0 : 1;
	int thread_counts[] = {1, 2, 4, 8, 16, 32, 64, 96, 128, 191, 192, 256, 383};
	int nr_counts = sizeof(thread_counts) / sizeof(thread_counts[0]);

	/*
	 * BENCH_THREADS=N: run a single reader count instead of the sweep --
	 * one clean timed window for perf-stat / profiling (mirrors load-names).
	 */
	const char *bt = getenv("BENCH_THREADS");
	if (bt) {
		int nt = atoi(bt);
		double r, w;

		if (nt > 0 && nt + writer_slots <= max_threads) {
			run_bench(nt, &r, &w);
			printf("%d %.1f %.1f\n", nt, r, w);
			fflush(stdout);
		}
		return 0;
	}

	for (int ti = 0; ti < nr_counts; ti++) {
		int nt = thread_counts[ti];
		if (nt + writer_slots > max_threads)	/* + writer thread, if any */
			break;

		double r, w;
		run_bench(nt, &r, &w);
		printf("%d %.1f %.1f\n", nt, r, w);
		fflush(stdout);
	}

	return 0;
}
