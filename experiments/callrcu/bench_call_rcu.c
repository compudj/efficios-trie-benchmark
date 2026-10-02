// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * bench_call_rcu.c -- what one call_rcu() costs the caller.
 *
 * P producers, pinned one per CPU of --cpulist, each with its own call_rcu
 * worker pinned on the same CPU: the configuration every bench harness here
 * uses (create_call_rcu_data(URCU_CALL_RCU_RT, cpu) + set_thread_call_rcu_data,
 * see bench_dcache.c).  Each producer loops: malloc 64 nodes (untimed), then,
 * timed with the TSC, for each node do --dirty stores and one call_rcu().  The
 * callback frees the node.
 *
 * Which enqueue path call_rcu() takes is the LIBRARY's decision: against a
 * liburcu built --enable-call-rcu-rseq a producer on its worker's cpu pushes
 * on the rseq local queue (two rseq sections, no locked op), anything else
 * takes the wfcq (an xchg on the tail, a locked add on qlen).  Run the same
 * binary with URCU_CALL_RCU_RSEQ=0 for the wfcq arm.  The local-push count is
 * read back through the library's test hook, so a run that meant to measure the
 * local path and did not is reported VACUOUS.
 *
 * --dirty N: before each call_rcu(), N plain stores to lines drawn at random
 * from a pool every producer writes (64 lines per producer).  With P >= 2 most
 * of those lines were last written by another core, so each store is an RFO
 * still outstanding when call_rcu() runs.  A locked instruction waits for the
 * store buffer to drain; a plain-store rseq commit does not.  That is the
 * hypothesis (design/call-rcu-rseq-percpu.md, s.1); --op none times the stores
 * alone, the baseline to subtract.
 *
 * --nonrt: workers without URCU_CALL_RCU_RT, so every enqueue also pays
 * call_rcu_wake_up()'s full fence (kept on the local path by design, s.6).
 *
 * Output: one line, `key: value` pairs.  ns/op is the median over every timed
 * block of every producer (robust to the worker's timeslices landing inside a
 * block); mean_ns/op includes them.
 *
 * Usage: bench_call_rcu --producers P --cpulist LIST [--duration MS]
 *                       [--dirty N] [--op call_rcu|none] [--nonrt]
 */
#define _GNU_SOURCE
#define _LGPL_SOURCE
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

#include <urcu-qsbr.h>
#include <urcu/uatomic.h>

/* Exported by a liburcu built --enable-call-rcu-rseq; absent otherwise. */
extern unsigned long urcu_call_rcu_rseq_test_pushed(struct call_rcu_data *crdp)
	__attribute__((weak));
extern int urcu_call_rcu_rseq_test_enabled(struct call_rcu_data *crdp)
	__attribute__((weak));

#define BLOCK		64
#define MAX_SAMPLES	(1UL << 20)
#define MAX_CPUS	1024

struct node {
	struct rcu_head head;
	unsigned long pad;
};

struct line {
	unsigned long v;
} __attribute__((aligned(64)));

struct producer {
	pthread_t tid;
	int cpu;
	unsigned long ops, local_pushed, nsamples;
	int local_enabled;
	uint32_t *samples;		/* TSC ticks per block */
} __attribute__((aligned(64)));

static int nprod = 1, duration_ms = 1000, dirty, op_none, nonrt;
static int cpus[MAX_CPUS], ncpus;
static struct line *pool;
static unsigned long pool_mask;
static int go, stop;

static void free_node(struct rcu_head *h)
{
	free(caa_container_of(h, struct node, head));
}

static void pin(int cpu)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s)) {
		perror("pthread_setaffinity_np");
		exit(2);
	}
}

static inline uint64_t xorshift(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return *s = x;
}

static void *producer(void *arg)
{
	struct producer *p = arg;
	struct call_rcu_data *crdp;
	struct node *batch[BLOCK];
	uint64_t s = 0x9e3779b97f4a7c15ULL * (uint64_t) (p->cpu + 1);
	int i, j;

	pin(p->cpu);
	rcu_register_thread();
	crdp = create_call_rcu_data(nonrt ? 0 : URCU_CALL_RCU_RT, p->cpu);
	if (!crdp) {
		fprintf(stderr, "create_call_rcu_data failed\n");
		exit(2);
	}
	set_thread_call_rcu_data(crdp);
	if (urcu_call_rcu_rseq_test_enabled)
		p->local_enabled = urcu_call_rcu_rseq_test_enabled(crdp);
	p->samples = malloc(MAX_SAMPLES * sizeof(*p->samples));

	while (!uatomic_load(&go))
		caa_cpu_relax();
	while (!uatomic_load(&stop)) {
		uint64_t t0, t1;
		unsigned int aux;

		for (i = 0; i < BLOCK; i++)
			batch[i] = malloc(sizeof(struct node));
		_mm_lfence();
		t0 = __rdtsc();
		_mm_lfence();
		for (i = 0; i < BLOCK; i++) {
			for (j = 0; j < dirty; j++)
				pool[xorshift(&s) & pool_mask].v = (unsigned long) i;
			if (!op_none)
				call_rcu(&batch[i]->head, free_node);
		}
		t1 = __rdtscp(&aux);
		if (op_none)
			for (i = 0; i < BLOCK; i++)
				free(batch[i]);
		if (p->nsamples < MAX_SAMPLES)
			p->samples[p->nsamples++] = (uint32_t) (t1 - t0 < UINT32_MAX ?
								t1 - t0 : UINT32_MAX);
		p->ops += BLOCK;
		rcu_quiescent_state();
	}
	if (urcu_call_rcu_rseq_test_pushed)
		p->local_pushed = urcu_call_rcu_rseq_test_pushed(crdp);
	set_thread_call_rcu_data(NULL);
	rcu_thread_offline();
	call_rcu_data_free(crdp);	/* drains this worker's queues */
	rcu_unregister_thread();
	return NULL;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;

	return x < y ? -1 : x > y;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void parse_cpulist(const char *s)
{
	char *dup = strdup(s), *tok, *save;

	for (tok = strtok_r(dup, ",", &save); tok && ncpus < MAX_CPUS;
	     tok = strtok_r(NULL, ",", &save)) {
		int a, b;

		if (sscanf(tok, "%d-%d", &a, &b) == 2) {
			for (; a <= b && ncpus < MAX_CPUS; a++)
				cpus[ncpus++] = a;
		} else {
			cpus[ncpus++] = atoi(tok);
		}
	}
	free(dup);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "producers", required_argument, NULL, 'p' },
		{ "cpulist", required_argument, NULL, 'c' },
		{ "duration", required_argument, NULL, 'd' },
		{ "dirty", required_argument, NULL, 'D' },
		{ "op", required_argument, NULL, 'o' },
		{ "nonrt", no_argument, NULL, 'n' },
		{ NULL, 0, NULL, 0 },
	};
	struct producer *prod;
	unsigned long ops = 0, local = 0, nsamp = 0, k, lines;
	uint64_t tsc0, tsc1, sum = 0;
	uint32_t *all;
	double t0, t1, hz;
	int c, i, enabled = 0;

	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case 'p': nprod = atoi(optarg); break;
		case 'c': parse_cpulist(optarg); break;
		case 'd': duration_ms = atoi(optarg); break;
		case 'D': dirty = atoi(optarg); break;
		case 'o': op_none = !strcmp(optarg, "none"); break;
		case 'n': nonrt = 1; break;
		default:
			fprintf(stderr, "usage: %s --producers P --cpulist LIST "
				"[--duration MS] [--dirty N] [--op call_rcu|none] "
				"[--nonrt]\n", argv[0]);
			return 2;
		}
	}
	if (!ncpus)
		for (ncpus = 0; ncpus < nprod; ncpus++)
			cpus[ncpus] = ncpus;
	if (nprod < 1 || nprod > ncpus) {
		fprintf(stderr, "need 1 <= producers <= cpulist length (%d)\n", ncpus);
		return 2;
	}
	for (lines = 1; lines < 64UL * (unsigned long) (nprod < 2 ? 2 : nprod); lines <<= 1)
		;
	pool_mask = lines - 1;
	if (posix_memalign((void **) &pool, 64, lines * sizeof(*pool)))
		return 2;
	memset(pool, 0, lines * sizeof(*pool));

	prod = calloc(nprod, sizeof(*prod));
	for (i = 0; i < nprod; i++) {
		prod[i].cpu = cpus[i];
		pthread_create(&prod[i].tid, NULL, producer, &prod[i]);
	}
	usleep(100 * 1000);	/* let the workers start and pin */
	t0 = now_s();
	tsc0 = __rdtsc();
	uatomic_store(&go, 1);
	usleep(duration_ms * 1000UL);
	uatomic_store(&stop, 1);
	t1 = now_s();
	tsc1 = __rdtsc();
	for (i = 0; i < nprod; i++)
		pthread_join(prod[i].tid, NULL);
	hz = (double) (tsc1 - tsc0) / (t1 - t0);

	for (i = 0; i < nprod; i++) {
		ops += prod[i].ops;
		local += prod[i].local_pushed;
		nsamp += prod[i].nsamples;
		enabled += prod[i].local_enabled;
	}
	all = malloc(nsamp * sizeof(*all));
	for (i = 0, k = 0; i < nprod; i++) {
		memcpy(all + k, prod[i].samples, prod[i].nsamples * sizeof(*all));
		k += prod[i].nsamples;
		free(prod[i].samples);
	}
	for (k = 0; k < nsamp; k++)
		sum += all[k];
	qsort(all, nsamp, sizeof(*all), cmp_u32);
	printf("producers: %d dirty: %d op: %s workers: %s ops: %lu "
	       "Mops/s: %.3f ns/op: %.2f mean_ns/op: %.2f p90_ns/op: %.2f "
	       "local_frac: %.4f local_enabled: %d/%d\n",
	       nprod, dirty, op_none ? "none" : "call_rcu", nonrt ? "nonrt" : "rt",
	       ops, ops / (t1 - t0) / 1e6,
	       nsamp ? all[nsamp / 2] / hz * 1e9 / BLOCK : 0.0,
	       nsamp ? (double) sum / nsamp / hz * 1e9 / BLOCK : 0.0,
	       nsamp ? all[nsamp * 9 / 10] / hz * 1e9 / BLOCK : 0.0,
	       ops ? (double) local / ops : 0.0, enabled, nprod);
	free(all);
	free(prod);
	free(pool);
	return 0;
}
