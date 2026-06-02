/*
 * Single-threaded lookup benchmark for Wormhole (wuxb45/wormhole), kept in a
 * SEPARATE executable from bench_one_st because Wormhole is GPL-3.0.
 *
 * ***  THIS BINARY IS GPL-3.0  ***
 * It links third_party/wormhole (GPL-3.0), so the resulting executable is a
 * derivative work under GPL-3.0.  bench_one_st and the other benchmarks stay
 * permissively licensed precisely because they do NOT link Wormhole.  Do not
 * merge this engine into bench_one_st.
 *
 * The dataset/harness (DNS keys, WARMUP/RUNS, RSS sampling, output format) is
 * a deliberate copy of the relevant bits of src/bench_one_st.c so the numbers
 * are directly comparable to the `dns` engine results there:
 *   output: <ns/op> <RSS_kB>
 * Uses Wormhole's thread-UNSAFE API (whunsafe_*) for the best single-thread
 * speed (no concurrency control), matching the single-threaded comparison.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "lib.h"
#include "kv.h"
#include "wh.h"

#define N_KEYS	1000000
#define WARMUP	3
#define RUNS	10

static char  *str_keys[N_KEYS];
static size_t str_lens[N_KEYS];

static uint64_t xorshift64(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return *s = x;
}

static inline uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
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

/* Identical DNS key set to bench_one_st.c's `dns` dataset (same seed/format). */
static const char *dns_domains[] = {
	"com.google", "com.amazon", "com.microsoft", "com.apple",
	"com.meta", "com.netflix", "com.cloudflare", "com.github",
	"org.wikipedia", "org.mozilla", "org.apache", "org.kernel",
	"net.cloudflare", "net.akamai", "net.fastly", "net.edgecast",
	"io.github", "io.gitlab", "io.docker", "io.kubernetes",
	"dev.chromium", "dev.flutter", "dev.dart", "dev.go",
};
#define N_DOMAINS (sizeof(dns_domains) / sizeof(dns_domains[0]))

static void gen_keys(void)
{
	uint64_t seed = 12345;
	for (unsigned int i = 0; i < N_KEYS; i++) {
		char buf[128];
		int len = snprintf(buf, sizeof(buf), "%s.host%06u.zone%u",
			dns_domains[xorshift64(&seed) % N_DOMAINS],
			(unsigned)(xorshift64(&seed) % 100000),
			(unsigned)(xorshift64(&seed) % 50));
		str_keys[i] = malloc(len + 1);
		memcpy(str_keys[i], buf, len + 1);
		str_lens[i] = len;
	}
}

int main(void)
{
	gen_keys();

	/* Build: thread-unsafe wormhole, dup keys in (whunsafe owns its copy). */
	struct wormhole *wh = whunsafe_create(&kvmap_mm_dup);
	if (!wh) {
		fprintf(stderr, "whunsafe_create failed\n");
		return 1;
	}
	for (unsigned int i = 0; i < N_KEYS; i++) {
		uint64_t val = i;
		struct kv *kv = kv_create(str_keys[i], (u32)str_lens[i],
					  &val, sizeof(val));
		if (!kv) {
			fprintf(stderr, "kv_create OOM at %u\n", i);
			return 1;
		}
		whunsafe_put(wh, kv);	/* mm_dup: wormhole copies it */
		free(kv);
	}

	long rss = get_rss_kb();

	/* Warmup, then timed lookup passes (best-of-RUNS), like bench_one_st. */
	double best = 1e18;
	for (int w = 0; w < WARMUP; w++) {
		for (unsigned int i = 0; i < N_KEYS; i++) {
			struct kref kref;
			kref_ref_hash32(&kref, (const u8 *)str_keys[i],
					(u32)str_lens[i]);
			volatile bool hit = whunsafe_probe(wh, &kref);
			(void)hit;
		}
	}
	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < N_KEYS; i++) {
			struct kref kref;
			kref_ref_hash32(&kref, (const u8 *)str_keys[i],
					(u32)str_lens[i]);
			volatile bool hit = whunsafe_probe(wh, &kref);
			(void)hit;
		}
		uint64_t t1 = now_ns();
		double ns = (double)(t1 - t0) / N_KEYS;
		if (ns < best)
			best = ns;
	}

	/* Same output format as bench_one_st: <ns/op> <RSS_kB>. */
	printf("%.1f %ld\n", best, rss);
	return 0;
}
