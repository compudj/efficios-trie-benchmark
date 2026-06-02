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
};

struct writer_arg {
	unsigned long writes;
};

static void *reader_thread(void *arg)
{
	struct thread_arg *ta = arg;
	unsigned long count = 0;
	uint64_t seed = ta->seed;
	void *ctx = g_eng->reader_setup ? g_eng->reader_setup() : NULL;

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
	void *ctx = g_eng->writer_setup ? g_eng->writer_setup() : NULL;

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

	start_flag = 0;
	stop_flag = 0;
	prime_done_count = 0;

	if (g_eng->run_reset)
		g_eng->run_reset();

	for (int i = 0; i < nr_readers; i++) {
		rargs[i].count = 0;
		rargs[i].seed = 42 + (uint64_t)i * 1000;
		pthread_create(&threads[i], NULL, reader_thread, &rargs[i]);
	}
	pthread_create(&threads[nr_readers], NULL, writer_thread, &warg);

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

	for (int i = 0; i <= nr_readers; i++)
		pthread_join(threads[i], NULL);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t t1 = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	double elapsed = (double)(t1 - t0) / 1e9;

	for (int i = 0; i < nr_readers; i++)
		total_reads += rargs[i].count;

	*read_mops = (double)total_reads / elapsed / 1e6;
	*write_kops = (double)warg.writes / elapsed / 1e3;

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

int bench_scale_main(int argc, char **argv, const struct bench_engine *eng)
{
	int max_threads = 384;

	if (argc > 1)
		max_threads = atoi(argv[1]);

	g_eng = eng;

	/*
	 * Optional NUMA interleaving (BENCH_NUMA_INTERLEAVE): spread every
	 * allocation this main thread faults in -- the query-key set AND the
	 * structure built below -- across all NUMA nodes, so at high reader
	 * counts spanning multiple sockets no single node's memory bandwidth
	 * bottlenecks the shared read set.  Applied uniformly to whatever engine
	 * is under test.  (load-names' ft_spec_il achieves the same with an
	 * mbind'd arena; this is the cross-engine equivalent.)  Set before
	 * bench_gen_keys() so the query keys are interleaved too.
	 */
	if (getenv("BENCH_NUMA_INTERLEAVE") != NULL) {
		if (numa_available() != -1) {
			numa_set_interleave_mask(numa_all_nodes_ptr);
			fprintf(stderr,
				"NUMA: interleaving allocations across all nodes\n");
		} else {
			fprintf(stderr,
				"NUMA: BENCH_NUMA_INTERLEAVE set but libnuma unavailable\n");
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
	printf("# readers read_mops write_kops\n");
	fflush(stdout);

	int thread_counts[] = {1, 2, 4, 8, 16, 32, 64, 128, 192, 256, 383};
	int nr_counts = sizeof(thread_counts) / sizeof(thread_counts[0]);

	for (int ti = 0; ti < nr_counts; ti++) {
		int nt = thread_counts[ti];
		if (nt + 1 > max_threads)	/* +1 for the writer */
			break;

		double r, w;
		run_bench(nt, &r, &w);
		printf("%d %.1f %.1f\n", nt, r, w);
		fflush(stdout);
	}

	return 0;
}
