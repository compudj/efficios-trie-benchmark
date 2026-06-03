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
 * The datasets/harness (WARMUP/RUNS, RSS sampling, output format) are a
 * deliberate copy of the relevant bits of src/bench_one_st.c, with the same
 * seeds/formats, so the numbers are directly comparable to bench_one_st's
 * per-dataset results.  Integer keys are encoded big-endian, exactly like the
 * byte-keyed engines there.  Uses Wormhole's thread-UNSAFE API (whunsafe_*) for
 * the best single-thread speed, matching the single-threaded comparison.
 *
 *   ./bench_wormhole_gpl [dataset]      (default: dns)
 *   dataset: u32d u32s u64d u64s dns dict paths
 *   output:  <ns/op> <RSS_kB>
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
static uint64_t int_keys[N_KEYS];
static int key_len_bytes;	/* 4/8 for integer datasets, 0 for strings */

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

/* Synthetic key sets, identical to bench_one_st.c's datasets (same seeds). */
static const char *dns_domains[] = {
	"com.google", "com.amazon", "com.microsoft", "com.apple",
	"com.meta", "com.netflix", "com.cloudflare", "com.github",
	"org.wikipedia", "org.mozilla", "org.apache", "org.kernel",
	"net.cloudflare", "net.akamai", "net.fastly", "net.edgecast",
	"io.github", "io.gitlab", "io.docker", "io.kubernetes",
	"dev.chromium", "dev.flutter", "dev.dart", "dev.go",
};
#define N_DOMAINS (sizeof(dns_domains) / sizeof(dns_domains[0]))
static const char *dict_words[] = {
	"algorithm", "benchmark", "compiler", "database", "encryption",
	"firewall", "gateway", "hardware", "interface", "javascript",
	"kernel", "library", "memory", "network", "operating",
	"protocol", "query", "router", "software", "terminal",
	"utility", "virtual", "wireless", "xerox", "yield", "zeppelin",
	"abstraction", "bandwidth", "cache", "debugger", "endpoint",
	"framework", "garbage", "hashmap", "iterator", "json",
};
#define N_DICT_WORDS (sizeof(dict_words) / sizeof(dict_words[0]))

static void store_str(unsigned int i, const char *buf, int len)
{
	str_keys[i] = malloc(len + 1);
	memcpy(str_keys[i], buf, len + 1);
	str_lens[i] = len;
}

static void gen_keys(const char *dataset)
{
	uint64_t seed;
	unsigned int i;

	if (strcmp(dataset, "u32d") == 0) {
		key_len_bytes = 4;
		for (i = 0; i < N_KEYS; i++) int_keys[i] = i;
	} else if (strcmp(dataset, "u32s") == 0) {
		key_len_bytes = 4; seed = 42;
		for (i = 0; i < N_KEYS; i++) int_keys[i] = (uint32_t)xorshift64(&seed);
	} else if (strcmp(dataset, "u64d") == 0) {
		key_len_bytes = 8;
		for (i = 0; i < N_KEYS; i++) int_keys[i] = i;
	} else if (strcmp(dataset, "u64s") == 0) {
		key_len_bytes = 8; seed = 42;
		for (i = 0; i < N_KEYS; i++) int_keys[i] = xorshift64(&seed);
	} else if (strcmp(dataset, "dns") == 0) {
		key_len_bytes = 0; seed = 12345;
		for (i = 0; i < N_KEYS; i++) {
			char buf[128];
			int len = snprintf(buf, sizeof(buf), "%s.host%06u.zone%u",
				dns_domains[xorshift64(&seed) % N_DOMAINS],
				(unsigned)(xorshift64(&seed) % 100000),
				(unsigned)(xorshift64(&seed) % 50));
			store_str(i, buf, len);
		}
	} else if (strcmp(dataset, "dict") == 0) {
		key_len_bytes = 0; seed = 67890;
		for (i = 0; i < N_KEYS; i++) {
			char buf[128];
			int len = snprintf(buf, sizeof(buf), "%s_%s_%06u",
				dict_words[xorshift64(&seed) % N_DICT_WORDS],
				dict_words[xorshift64(&seed) % N_DICT_WORDS],
				(unsigned)(xorshift64(&seed) % 100000));
			store_str(i, buf, len);
		}
	} else if (strcmp(dataset, "paths") == 0) {
		key_len_bytes = 0; seed = 11111;
		static const char *dirs[] = {"/usr","/var","/etc","/home","/opt","/tmp","/lib","/bin","/sbin","/srv","/mnt","/proc"};
		static const char *subdirs[] = {"lib","share","local","cache","log","config","data","run","lock","spool","mail","www"};
		for (i = 0; i < N_KEYS; i++) {
			char buf[256];
			int len = snprintf(buf, sizeof(buf), "%s/%s/%s/file%06u.dat",
				dirs[xorshift64(&seed) % 12], subdirs[xorshift64(&seed) % 12],
				subdirs[xorshift64(&seed) % 12], (unsigned)(xorshift64(&seed) % 100000));
			store_str(i, buf, len);
		}
	} else {
		fprintf(stderr, "Unknown dataset: %s\n", dataset);
		exit(1);
	}
}

/* Byte key + length for index i (integer encoded big-endian into @ibuf). */
static inline const void *key_at(unsigned int i, uint8_t *ibuf, size_t *klen)
{
	if (key_len_bytes) {
		uint64_t v = int_keys[i];
		for (int b = 0; b < key_len_bytes; b++)
			ibuf[b] = (uint8_t)(v >> (8 * (key_len_bytes - 1 - b)));
		*klen = key_len_bytes;
		return ibuf;
	}
	*klen = str_lens[i];
	return str_keys[i];
}

int main(int argc, char **argv)
{
	const char *dataset = (argc > 1) ? argv[1] : "dns";
	uint8_t ibuf[8];

	gen_keys(dataset);

	/* Build: thread-unsafe wormhole, dup keys in (whunsafe owns its copy). */
	struct wormhole *wh = whunsafe_create(&kvmap_mm_dup);
	if (!wh) {
		fprintf(stderr, "whunsafe_create failed\n");
		return 1;
	}
	for (unsigned int i = 0; i < N_KEYS; i++) {
		size_t kl;
		const void *kp = key_at(i, ibuf, &kl);
		uint64_t val = i;
		struct kv *kv = kv_create(kp, (u32)kl, &val, sizeof(val));
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
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			struct kref kref;
			kref_ref_hash32(&kref, (const u8 *)kp, (u32)kl);
			volatile bool hit = whunsafe_probe(wh, &kref);
			(void)hit;
		}
	}
	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < N_KEYS; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			struct kref kref;
			kref_ref_hash32(&kref, (const u8 *)kp, (u32)kl);
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
