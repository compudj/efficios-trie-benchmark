/*
 * Single-dataset, single-engine benchmark.
 * Run in its own process for accurate RSS.
 *
 * Usage: bench_one <dataset> <engine>
 *   dataset: u32d u32s u64d u64s dns dict paths
 *   engine:  ft_eager ft_eager_on_spec ft_cand ft_spec judy judyl judysl judyhs qp art hot cuckoo masstree artolc
 *
 * Output: <ns/op> <RSS_kB>
 */
#define _GNU_SOURCE
#define _LGPL_SOURCE
#define RCU_QSBR
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <pthread.h>
#include <sys/prctl.h>

#include <urcu.h>
#include <urcu/fractal-trie.h>
#include <Judy.h>
#include "Tbl.h"
#include "art.h"

#ifndef N_KEYS
#define N_KEYS 1000000
#endif
#ifndef WARMUP
#define WARMUP	3
#endif
#ifndef RUNS
#define RUNS	10
#endif

/*
 * Force a 1-byte volatile load of the leaf the lookup returned, so
 * every engine pays the same post-lookup cache-line touch.  Matches
 * the convention used in tests/bench/load-names.c (BIND9 bench).
 *
 * The asm constraint "r" forces @v to be materialized in a register
 * and the "memory" clobber prevents the compiler from re-ordering
 * loads/stores around this point.  The volatile cast prevents
 * elision of the byte read itself.
 *
 * Skipped for cand-mode FT engines: candidate semantics by design
 * trust the returned pointer without touching the leaf — the
 * memcmp-based candidate verification already touches the leaf
 * key bytes when enabled, providing equivalent CL access.
 */
#define FORCE_READ_LEAF(p) do {						\
	if (p) {							\
		volatile uint8_t _b = *(const volatile uint8_t *)(p);	\
		__asm__ volatile("" : : "r"(_b) : "memory");		\
	}								\
} while (0)

static inline uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/*
 * FT_BENCH_COMPACT: run a full in-place compaction (cds_ft_compact) on the
 * freshly-built trie, between build and the timed query phase.  The trie is
 * quiescent here, so the one-shot wrapper is fine.  Reports compaction time
 * and verifies (unless FT_RECOMPACT_NOVERIFY); RSS sampled by the caller AFTER
 * this returns reflects the post-compaction (drained-range-reclaimed) state.
 * Warmup runs afterward, so both arms reach the same cache steady state.
 */
static void maybe_compact(struct cds_ft *ft)
{
	uint64_t c0, c1;

	if (getenv("FT_BENCH_COMPACT") == NULL)
		return;
	if (getenv("FT_BENCH_VERIFY_PRE") != NULL)
		fprintf(stderr, "FT verify BEFORE compact: %s\n",
			cds_ft_verify(ft, stderr) == CDS_FT_STATUS_OK ? "OK" : "FAILED");
	c0 = now_ns();
	cds_ft_compact(ft);
	c1 = now_ns();
	/*
	 * Flush deferred frees so the drained old ranges are actually
	 * reclaimed (MADV_DONTNEED) before RSS is sampled and before the timed
	 * query phase -- otherwise RSS double-counts old+new and the reclaim's
	 * TLB shootdowns would land inside the measurement window.
	 */
	rcu_barrier();
	if (getenv("FT_RECOMPACT_NOVERIFY") == NULL &&
	    cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "FT VERIFY FAILED after compact\n");
		abort();
	}
	fprintf(stderr, "TIMING: compact_ns=%lu\n", (unsigned long)(c1 - c0));
}

static uint64_t xorshift64(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return *s = x;
}

static long get_rss_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long rss = 0;
	if (!f) return 0;
	while (fgets(line, sizeof(line), f))
		if (strncmp(line, "VmRSS:", 6) == 0) {
			sscanf(line + 6, " %ld", &rss);
			break;
		}
	fclose(f);
	return rss;
}

/* Per-lookup latency histogram (single-thread). Enabled by FT_LATENCY_HIST=1. */
static int g_latency_hist;
static const char *g_dataset_label = "?";

static inline uint64_t rdtscp_serialized(void)
{
	uint32_t lo, hi;
	__asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi) :: "%rcx");
	return ((uint64_t)hi << 32) | lo;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

static void print_latency_hist(uint64_t *s, size_t n, const char *label)
{
	qsort(s, n, sizeof(*s), cmp_u64);
	double mean = 0;
	for (size_t i = 0; i < n; i++) mean += (double)s[i];
	mean /= n;
	fprintf(stderr,
		"LATENCY %s n=%zu  min=%lu  p50=%lu  p90=%lu  p99=%lu  p999=%lu  p9999=%lu  max=%lu  mean=%.1f cycles\n",
		label, n, s[0],
		s[(size_t)(n * 0.50)],
		s[(size_t)(n * 0.90)],
		s[(size_t)(n * 0.99)],
		s[(size_t)(n * 0.999)],
		s[(size_t)(n * 0.9999)],
		s[n - 1], mean);
}

/* ── key generation ────────────────────────────────────────── */

static uint64_t int_keys[N_KEYS];
static char *str_keys[N_KEYS];
static size_t str_lens[N_KEYS];
static unsigned int n_keys;
static int key_len_bytes; /* 4 or 8 for integer, 0 for string */

/*
 * Backing arena for the str_keys[] pointers.  cds_ft_external_arena
 * reserves a trailing guard page per backing range, so every key
 * allocation is safe to over-read for up to a page past its last
 * byte.  Combined with leaf_readable_len = 32 on the FT group attr,
 * that's enough for the library to use unmasked AVX2 leaf compares
 * (or AVX-512 BW+VL predicated load when available) on the input
 * side of spec_validate without per-string padding.
 */
static struct cds_ft_external_arena *str_keys_arena;

static void str_keys_store(unsigned int i, const char *buf, size_t len)
{
	if (!str_keys_arena)
		str_keys_arena = cds_ft_external_arena_create(NULL);
	/*
	 * Request len + 1 so the arena's power-of-2 size class always
	 * leaves at least one zeroed byte past the key (NUL terminator
	 * for Judy / art string lookups).
	 */
	str_keys[i] = cds_ft_external_arena_alloc(str_keys_arena, len + 1);
	memcpy(str_keys[i], buf, len);
	str_lens[i] = len;
}

/*
 * Over-read horizon past the end of an arena-allocated key.  The
 * arena's backing ranges add a trailing guard area beyond every
 * allocation, so 32 bytes past key_len (and far more — up to a
 * full page) are always safely loadable.
 */
#define STR_KEYS_READABLE_PAD	32U

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

static void gen_keys(const char *dataset)
{
	uint64_t seed;
	unsigned int i;

	n_keys = N_KEYS;

	if (strcmp(dataset, "u32d") == 0) {
		key_len_bytes = 4;
		for (i = 0; i < n_keys; i++) int_keys[i] = i;
	} else if (strcmp(dataset, "u32s") == 0) {
		key_len_bytes = 4; seed = 42;
		for (i = 0; i < n_keys; i++) int_keys[i] = (uint32_t)xorshift64(&seed);
	} else if (strcmp(dataset, "u64d") == 0) {
		key_len_bytes = 8;
		for (i = 0; i < n_keys; i++) int_keys[i] = i;
	} else if (strcmp(dataset, "u64s") == 0) {
		key_len_bytes = 8; seed = 42;
		for (i = 0; i < n_keys; i++) int_keys[i] = xorshift64(&seed);
	} else if (strcmp(dataset, "dns") == 0) {
		key_len_bytes = 0; seed = 12345;
		for (i = 0; i < n_keys; i++) {
			char buf[128];
			int len = snprintf(buf, sizeof(buf), "%s.host%06u.zone%u",
				dns_domains[xorshift64(&seed) % N_DOMAINS],
				(unsigned)(xorshift64(&seed) % 100000),
				(unsigned)(xorshift64(&seed) % 50));
			str_keys_store(i, buf, len);
		}
	} else if (strcmp(dataset, "dict") == 0) {
		key_len_bytes = 0; seed = 67890;
		for (i = 0; i < n_keys; i++) {
			char buf[128];
			int len = snprintf(buf, sizeof(buf), "%s_%s_%06u",
				dict_words[xorshift64(&seed) % N_DICT_WORDS],
				dict_words[xorshift64(&seed) % N_DICT_WORDS],
				(unsigned)(xorshift64(&seed) % 100000));
			str_keys_store(i, buf, len);
		}
	} else if (strcmp(dataset, "paths") == 0) {
		key_len_bytes = 0; seed = 11111;
		static const char *dirs[] = {"/usr","/var","/etc","/home","/opt","/tmp","/lib","/bin","/sbin","/srv","/mnt","/proc"};
		static const char *subdirs[] = {"lib","share","local","cache","log","config","data","run","lock","spool","mail","www"};
		for (i = 0; i < n_keys; i++) {
			char buf[256];
			int len = snprintf(buf, sizeof(buf), "%s/%s/%s/file%06u.dat",
				dirs[xorshift64(&seed) % 12], subdirs[xorshift64(&seed) % 12],
				subdirs[xorshift64(&seed) % 12], (unsigned)(xorshift64(&seed) % 100000));
			str_keys_store(i, buf, len);
		}
	} else {
		fprintf(stderr, "Unknown dataset: %s\n", dataset);
		exit(1);
	}
}

/* ── miss-query injection ─────────────────────────────────── */

/*
 * FT_BENCH_MISS_PCT=<0..100> replaces that share of the FT engines'
 * lookup queries (pseudorandomly interleaved, fixed seed) with absent
 * keys; the build phase always inserts the original keys.  Shape:
 *   default ("near"): integer datasets query int_keys[i] + n_keys
 *     (guaranteed absent on dense sets, absent w.h.p. on sparse);
 *     string datasets replace the LAST byte with '~' (never generated)
 *     so the walk diverges at the deepest level.
 *   FT_BENCH_MISS_EARLY=1: integer datasets force the top key byte to
 *     0xFF; string datasets replace byte 0 with '~' — divergence at
 *     the root.  The two shapes bracket divergence-depth sensitivity.
 * Only the FT lookup loops read the q_* arrays (hit slots alias the
 * original keys, so a 0% run is bit-identical to the old behavior).
 */
static uint64_t q_int_keys[N_KEYS];
static const char *q_str_keys[N_KEYS];
static size_t q_str_lens[N_KEYS];
static unsigned int g_miss_pct, g_miss_planned;
static uint64_t g_null_found;

static void setup_miss_queries(void)
{
	const char *e = getenv("FT_BENCH_MISS_PCT");
	int early = getenv("FT_BENCH_MISS_EARLY") != NULL;
	uint64_t dseed = 0xDEADBEEFCAFEF00DULL;
	unsigned int i;

	g_miss_pct = e ? (unsigned int) atoi(e) : 0U;
	for (i = 0; i < n_keys; i++) {
		int miss = g_miss_pct &&
			(xorshift64(&dseed) % 100) < g_miss_pct;

		g_miss_planned += miss;
		if (key_len_bytes) {
			uint64_t q = int_keys[i];

			if (miss)
				q = early ?
					(q | (0xFFULL << (8 * (key_len_bytes - 1)))) :
					(q + n_keys);
			q_int_keys[i] = q;
		} else {
			q_str_keys[i] = str_keys[i];
			q_str_lens[i] = str_lens[i];
			if (miss) {
				char *m = cds_ft_external_arena_alloc(
					str_keys_arena, str_lens[i] + 1);

				memcpy(m, str_keys[i], str_lens[i]);
				m[early ? 0 : str_lens[i] - 1] = '~';
				q_str_keys[i] = m;
			}
		}
	}
	if (g_miss_pct)
		fprintf(stderr, "MISS: pct=%u planned=%u/%u early=%d\n",
			g_miss_pct, g_miss_planned, n_keys, early);
}

/* ── entry struct for candidate verify ────────────────────── */

struct ft_entry {
	struct cds_ft_node ft_node;
	size_t key_len;
	char key[];	/* flexible array, allocated to exact key_len */
};

/*
 * Fixed-length integer entry: stores the encoded key bytes alongside
 * the node so candidate / speculative-validated lookups can verify
 * that the prefilter didn't return a false positive.  Without this
 * the cand-mode integer path silently accepted unvalidated hits.
 */
struct ft_entry_int {
	struct cds_ft_node ft_node;
	uint8_t key[8];	/* 4 bytes used for u32, 8 for u64 */
};

/*
 * Equivalent payload struct for qp / art, which do NOT embed any
 * library plumbing in the user's leaf (no analogue to cds_ft_node).
 * Same key_len + flexible-key layout as ft_entry so the per-leaf
 * cache footprint comparison stays apples-to-apples — except for
 * FT's 16-byte ft_node header, which is the genuine overhead FT
 * pays for embedded duplicate-chain plumbing.
 */
struct kv_entry {
	size_t key_len;
	char key[];
};

/*
 * Encode an integer key as key_len_bytes big-endian bytes, for the byte-keyed
 * engines (qp / ART / Cuckoo / Masstree / ART-OLC) on the integer datasets.
 * Big-endian keeps radix order == integer order; fixed length means the keys
 * are prefix-free, so no terminator is needed (unlike the string datasets).
 */
static inline void encode_int_key(uint64_t v, uint8_t *out)
{
	for (int b = 0; b < key_len_bytes; b++)
		out[b] = (uint8_t)(v >> (8 * (key_len_bytes - 1 - b)));
}

/*
 * Byte key + length for lookup index @i: the encoded integer (into @intbuf)
 * on an integer dataset, or str_keys[i] on a string dataset.  Callers that
 * need a prefix-free string key (ART) read one extra byte (the str_keys NUL).
 */
static inline const void *key_at(unsigned int i, uint8_t *intbuf, size_t *klen)
{
	if (key_len_bytes) {
		encode_int_key(int_keys[i], intbuf);
		*klen = key_len_bytes;
		return intbuf;
	}
	*klen = str_lens[i];
	return str_keys[i];
}

/* ── engines ─────────────────────────────────────────────── */

static void run_ft(int skip, int candidate, int spec_validated)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;
	long rss;
	double best = 1e18;

	cds_ft_group_attr_create(&attr);
	if (key_len_bytes)
		cds_ft_group_attr_set_key_len(attr, key_len_bytes);
	else
		cds_ft_group_attr_set_max_key_len(attr, 256);
	/*
	 * Engine modes (explicit optimization, post-default-flip).
	 * Two independent knobs: the group's lookup-optimization attr
	 * (EAGER vs SPECULATIVE) and the lookup query function.
	 *
	 *   ft_eager (no flags):       EAGER attr + cds_ft_eager_lookup_key
	 *                              ("optimized eager trie": no skip-
	 *                              compressed encoding, per-step exact
	 *                              compare on compressed bytes).
	 *   ft_eager_on_spec (skip=1): SPECULATIVE attr + cds_ft_eager_
	 *                              lookup_key — eager lookup run on the
	 *                              speculative/skip-encoded trie (the cost
	 *                              a caller using the new default attr but
	 *                              the old eager lookup API pays).
	 *   ft_cand   (cand=1):        SPECULATIVE attr + cds_ft_lookup_
	 *                              candidate_key returned as-is, with NO
	 *                              caller-side validation — a pure
	 *                              candidate fetch.  (Adding a memcmp here
	 *                              would just reproduce ft_spec.)
	 *   ft_spec   (specv=1):       SPECULATIVE attr + cds_ft_speculative_
	 *                              lookup_key (lib-side memcmp via
	 *                              @key_offset).
	 */
	cds_ft_group_attr_set_lookup_optimization(attr,
		(skip || spec_validated || candidate)
			? CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE
			: CDS_FT_LOOKUP_OPTIMIZE_EAGER);
	cds_ft_group_create(attr, &group);
	cds_ft_group_attr_destroy(attr);
	cds_ft_create(group, NULL, &ft);

	uint64_t t_build_start = now_ns();
	uint64_t t_lookup_start = 0, t_lookup_end = 0;
	uint64_t lookups_total = 0;

	if (key_len_bytes) {
		struct ft_entry_int *entries = calloc(n_keys, sizeof(struct ft_entry_int));
		/*
		 * ft_spec validates caller-side, so its key length must be the
		 * resolved byte count -- and we pass it as a compile-time constant
		 * (4/8) at each call site below, so cds_ft_speculative_lookup_key's
		 * memcmp folds into an inlined fixed-width integer compare instead
		 * of a memcmp() call with a runtime length.  Passing
		 * CDS_FT_LEN_DEFAULT (== SIZE_MAX) here would make that memcmp
		 * over-read and spuriously miss every key.
		 */
		for (unsigned int i = 0; i < n_keys; i++) {
			if (key_len_bytes == 4)
				cds_ft_u32_to_key(ft, (uint32_t)int_keys[i], entries[i].key, CDS_FT_LEN_DEFAULT);
			else
				cds_ft_u64_to_key(ft, int_keys[i], entries[i].key, CDS_FT_LEN_DEFAULT);
			cds_ft_insert(ft, entries[i].key, CDS_FT_LEN_DEFAULT, &entries[i].ft_node);
			if ((i & 1023) == 0) rcu_quiescent_state();
		}
		rcu_quiescent_state();
		rcu_barrier();
		maybe_compact(ft);
		rss = get_rss_kb();
		if (getenv("FT_DUMP_STATS"))
			cds_ft_show_stats(ft, stderr);
		t_lookup_start = now_ns();

		/* Warmup */
		for (int w = 0; w < WARMUP; w++) {
			rcu_read_lock();
			for (unsigned int i = 0; i < n_keys; i++) {
				uint8_t k[8];
				struct cds_ft_node *found = NULL;
				if (key_len_bytes == 4)
					cds_ft_u32_to_key(ft, (uint32_t)q_int_keys[i], k, CDS_FT_LEN_DEFAULT);
				else
					cds_ft_u64_to_key(ft, q_int_keys[i], k, CDS_FT_LEN_DEFAULT);
				if (candidate) {
					/* Pure candidate lookup: no caller-side memcmp. */
					cds_ft_lookup_candidate_key(ft, k, CDS_FT_LEN_DEFAULT, CDS_FT_LEN_DEFAULT, &found);
				} else if (spec_validated) {
					if (key_len_bytes == 4)
						cds_ft_speculative_lookup_key(ft, k, 4, CDS_FT_LEN_DEFAULT,
							offsetof(struct ft_entry_int, key), &found);
					else
						cds_ft_speculative_lookup_key(ft, k, 8, CDS_FT_LEN_DEFAULT,
							offsetof(struct ft_entry_int, key), &found);
				} else {
					cds_ft_eager_lookup_key(ft, k, CDS_FT_LEN_DEFAULT, CDS_FT_LEN_DEFAULT, &found);
				}
				FORCE_READ_LEAF(found);
			}
			rcu_read_unlock();
			rcu_quiescent_state();
		}

		for (int r = 0; r < RUNS; r++) {
			uint64_t t0, t1;
			rcu_read_lock();
			t0 = now_ns();
			for (unsigned int i = 0; i < n_keys; i++) {
				uint8_t k[8];
				struct cds_ft_node *found = NULL;
				if (key_len_bytes == 4)
					cds_ft_u32_to_key(ft, (uint32_t)q_int_keys[i], k, CDS_FT_LEN_DEFAULT);
				else
					cds_ft_u64_to_key(ft, q_int_keys[i], k, CDS_FT_LEN_DEFAULT);
				if (candidate) {
					/* Pure candidate lookup: no caller-side memcmp. */
					cds_ft_lookup_candidate_key(ft, k, CDS_FT_LEN_DEFAULT, CDS_FT_LEN_DEFAULT, &found);
				} else if (spec_validated) {
					if (key_len_bytes == 4)
						cds_ft_speculative_lookup_key(ft, k, 4, CDS_FT_LEN_DEFAULT,
							offsetof(struct ft_entry_int, key), &found);
					else
						cds_ft_speculative_lookup_key(ft, k, 8, CDS_FT_LEN_DEFAULT,
							offsetof(struct ft_entry_int, key), &found);
				} else {
					cds_ft_eager_lookup_key(ft, k, CDS_FT_LEN_DEFAULT, CDS_FT_LEN_DEFAULT, &found);
				}
				FORCE_READ_LEAF(found);
				g_null_found += !found;
			}
			t1 = now_ns();
			rcu_read_unlock();
			rcu_quiescent_state();
			double ns = (double)(t1 - t0) / n_keys;
			if (ns < best) best = ns;
		}
		t_lookup_end = now_ns();
		lookups_total = (uint64_t)(WARMUP + RUNS) * n_keys;

		if (g_latency_hist) {
			uint64_t *samples = malloc((size_t)n_keys * sizeof(uint64_t));
			rcu_read_lock();
			for (unsigned int i = 0; i < n_keys; i++) {
				uint8_t k[8];
				struct cds_ft_node *found = NULL;
				if (key_len_bytes == 4)
					cds_ft_u32_to_key(ft, (uint32_t)q_int_keys[i], k, CDS_FT_LEN_DEFAULT);
				else
					cds_ft_u64_to_key(ft, q_int_keys[i], k, CDS_FT_LEN_DEFAULT);
				uint64_t s = rdtscp_serialized();
				if (candidate) {
					cds_ft_lookup_candidate_key(ft, k, CDS_FT_LEN_DEFAULT, CDS_FT_LEN_DEFAULT, &found);
				} else if (spec_validated) {
					if (key_len_bytes == 4)
						cds_ft_speculative_lookup_key(ft, k, 4, CDS_FT_LEN_DEFAULT,
							offsetof(struct ft_entry_int, key), &found);
					else
						cds_ft_speculative_lookup_key(ft, k, 8, CDS_FT_LEN_DEFAULT,
							offsetof(struct ft_entry_int, key), &found);
				} else {
					cds_ft_eager_lookup_key(ft, k, CDS_FT_LEN_DEFAULT, CDS_FT_LEN_DEFAULT, &found);
				}
				FORCE_READ_LEAF(found);
				uint64_t e = rdtscp_serialized();
				samples[i] = e - s;
			}
			rcu_read_unlock();
			rcu_quiescent_state();
			print_latency_hist(samples, n_keys, g_dataset_label);
			free(samples);
		}

		free(entries);
	} else {
		struct ft_entry **entries = calloc(n_keys, sizeof(struct ft_entry *));
		/*
		 * Allocate every ft_entry from a cds_ft_external_arena.
		 * Each allocation lands in a power-of-2 size class slot;
		 * the backing ranges reserve a trailing guard page so the
		 * library's 32-byte SIMD leaf compare can safely over-read
		 * past any entry's stored key without per-entry padding.
		 */
		struct cds_ft_external_arena *ft_entry_arena =
			cds_ft_external_arena_create(NULL);
		for (unsigned int i = 0; i < n_keys; i++) {
			struct cds_ft_node *result;
			entries[i] = cds_ft_external_arena_alloc(ft_entry_arena,
				sizeof(struct ft_entry) + str_lens[i]);
			memcpy(entries[i]->key, str_keys[i], str_lens[i]);
			entries[i]->key_len = str_lens[i];
			cds_ft_insert_unique(ft, (const uint8_t *)str_keys[i],
				str_lens[i], &entries[i]->ft_node, &result);
			if ((i & 1023) == 0) rcu_quiescent_state();
		}
		rcu_quiescent_state();
		rcu_barrier();
		maybe_compact(ft);
		rss = get_rss_kb();
		if (getenv("FT_DUMP_STATS"))
			cds_ft_show_stats(ft, stderr);
		t_lookup_start = now_ns();

		/* Warmup */
		for (int w = 0; w < WARMUP; w++) {
			rcu_read_lock();
			for (unsigned int i = 0; i < n_keys; i++) {
				struct cds_ft_node *found = NULL;
				if (candidate) {
					cds_ft_lookup_candidate_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD, &found);
				} else if (spec_validated) {
					cds_ft_speculative_lookup_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD,
						offsetof(struct ft_entry, key), &found);
				} else {
					cds_ft_eager_lookup_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD, &found);
				}
				FORCE_READ_LEAF(found);
			}
			rcu_read_unlock();
			rcu_quiescent_state();
		}

		for (int r = 0; r < RUNS; r++) {
			uint64_t t0, t1;
			rcu_read_lock();
			t0 = now_ns();
			for (unsigned int i = 0; i < n_keys; i++) {
				struct cds_ft_node *found = NULL;
				if (candidate) {
					/* Pure candidate lookup: no caller-side memcmp. */
					cds_ft_lookup_candidate_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD, &found);
				} else if (spec_validated) {
					cds_ft_speculative_lookup_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD,
						offsetof(struct ft_entry, key), &found);
				} else {
					cds_ft_eager_lookup_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD, &found);
				}
				FORCE_READ_LEAF(found);
				g_null_found += !found;
			}
			t1 = now_ns();
			rcu_read_unlock();
			rcu_quiescent_state();
			double ns = (double)(t1 - t0) / n_keys;
			if (ns < best) best = ns;
		}
		t_lookup_end = now_ns();
		lookups_total = (uint64_t)(WARMUP + RUNS) * n_keys;

		if (g_latency_hist) {
			uint64_t *samples = malloc((size_t)n_keys * sizeof(uint64_t));
			rcu_read_lock();
			for (unsigned int i = 0; i < n_keys; i++) {
				struct cds_ft_node *found = NULL;
				uint64_t s = rdtscp_serialized();
				if (candidate) {
					cds_ft_lookup_candidate_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD, &found);
				} else if (spec_validated) {
					cds_ft_speculative_lookup_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD,
						offsetof(struct ft_entry, key), &found);
				} else {
					cds_ft_eager_lookup_key(ft,
						(const uint8_t *)q_str_keys[i],
						q_str_lens[i], STR_KEYS_READABLE_PAD, &found);
				}
				FORCE_READ_LEAF(found);
				uint64_t e = rdtscp_serialized();
				samples[i] = e - s;
			}
			rcu_read_unlock();
			rcu_quiescent_state();
			print_latency_hist(samples, n_keys, g_dataset_label);
			free(samples);
		}

		cds_ft_external_arena_destroy(ft_entry_arena);
		free(entries);
	}
	printf("%.1f %ld\n", best, rss);
	fprintf(stderr, "NULLFOUND: %lu / %lu timed lookups\n",
		g_null_found, (uint64_t)RUNS * n_keys);
	fprintf(stderr, "TIMING: build_ns=%lu lookup_ns=%lu lookups=%lu\n",
		t_lookup_start - t_build_start,
		t_lookup_end - t_lookup_start,
		lookups_total);
}

static void run_judy(void)
{
	Pvoid_t judy = NULL;
	long rss;
	double best = 1e18;

	if (key_len_bytes) {
		for (unsigned int i = 0; i < n_keys; i++) {
			Word_t *pv;
			JLI(pv, judy, (Word_t)int_keys[i]);
			*pv = i;
		}
	} else {
		for (unsigned int i = 0; i < n_keys; i++) {
			Word_t *pv;
			JSLI(pv, judy, (uint8_t *)str_keys[i]);
			*pv = i;
		}
	}
	rss = get_rss_kb();

	/* Warmup */
	for (int w = 0; w < WARMUP; w++) {
		if (key_len_bytes) {
			for (unsigned int i = 0; i < n_keys; i++) {
				Word_t *pv;
				JLG(pv, judy, (Word_t)int_keys[i]);
				FORCE_READ_LEAF(pv);
			}
		} else {
			for (unsigned int i = 0; i < n_keys; i++) {
				Word_t *pv;
				JSLG(pv, judy, (uint8_t *)str_keys[i]);
				FORCE_READ_LEAF(pv);
			}
		}
	}

	for (int r = 0; r < RUNS; r++) {
		uint64_t t0, t1;
		t0 = now_ns();
		if (key_len_bytes) {
			for (unsigned int i = 0; i < n_keys; i++) {
				Word_t *pv;
				JLG(pv, judy, (Word_t)int_keys[i]);
				FORCE_READ_LEAF(pv);
			}
		} else {
			for (unsigned int i = 0; i < n_keys; i++) {
				Word_t *pv;
				JSLG(pv, judy, (uint8_t *)str_keys[i]);
				FORCE_READ_LEAF(pv);
			}
		}
		t1 = now_ns();
		double ns = (double)(t1 - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
}

/*
 * JudySL — Judy's digital/radix tree keyed on NUL-terminated byte strings
 * (JSLI/JSLG).  String datasets only: integer byte encodings contain NUL bytes,
 * which JudySL would treat as a string terminator.  (The combined "judy" engine
 * also uses JudySL on string datasets; this is the explicit, integer-skipping
 * variant for the per-dataset tables, alongside judyl and judyhs.)
 */
static void run_judysl(void)
{
	if (key_len_bytes) { printf("- 0\n"); return; }
	Pvoid_t judy = NULL;
	long rss;
	double best = 1e18;

	for (unsigned int i = 0; i < n_keys; i++) {
		Word_t *pv;
		JSLI(pv, judy, (uint8_t *)str_keys[i]);
		*pv = i;
	}
	rss = get_rss_kb();

	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			Word_t *pv;
			JSLG(pv, judy, (uint8_t *)str_keys[i]);
			FORCE_READ_LEAF(pv);
		}

	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			Word_t *pv;
			JSLG(pv, judy, (uint8_t *)str_keys[i]);
			FORCE_READ_LEAF(pv);
		}
		double ns = (double)(now_ns() - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
}

/*
 * JudyL — Judy's integer-keyed array (JLI/JLG).  Integer datasets only; this is
 * the explicit variant of what the combined "judy" engine does on u32/u64.
 */
static void run_judyl(void)
{
	if (!key_len_bytes) { printf("- 0\n"); return; }
	Pvoid_t judy = NULL;
	long rss;
	double best = 1e18;

	for (unsigned int i = 0; i < n_keys; i++) {
		Word_t *pv;
		JLI(pv, judy, (Word_t)int_keys[i]);
		*pv = i;
	}
	rss = get_rss_kb();

	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			Word_t *pv;
			JLG(pv, judy, (Word_t)int_keys[i]);
			FORCE_READ_LEAF(pv);
		}

	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			Word_t *pv;
			JLG(pv, judy, (Word_t)int_keys[i]);
			FORCE_READ_LEAF(pv);
		}
		double ns = (double)(now_ns() - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
}

/*
 * JudyHS — Judy's hash-based array, keyed on arbitrary byte strings (JHSI/JHSG)
 * rather than JudySL's digital/radix tree.  Runs on every dataset via key_at
 * (JudyHS-vs-JudySL on strings, JudyHS-vs-JudyL on integers).
 */
static void run_judyhs(void)
{
	Pvoid_t judy = NULL;
	long rss;
	double best = 1e18;
	uint8_t ibuf[8];

	for (unsigned int i = 0; i < n_keys; i++) {
		size_t kl;
		const void *kp = key_at(i, ibuf, &kl);
		Word_t *pv;
		JHSI(pv, judy, (void *)kp, (Word_t)kl);
		*pv = i;
	}
	rss = get_rss_kb();

	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			Word_t *pv;
			JHSG(pv, judy, (void *)kp, (Word_t)kl);
			FORCE_READ_LEAF(pv);
		}

	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			Word_t *pv;
			JHSG(pv, judy, (void *)kp, (Word_t)kl);
			FORCE_READ_LEAF(pv);
		}
		double ns = (double)(now_ns() - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
}

static void run_qp(void)
{
	Tbl *qp = NULL;
	long rss;
	double best = 1e18;
	uint8_t ibuf[8];
	struct kv_entry **entries = calloc(n_keys, sizeof(struct kv_entry *));
	/*
	 * Allocate kv_entry leaves from a cds_ft_external_arena so the
	 * leaf-layout fairness matches the FT engines (dense bump
	 * packing, no per-entry malloc header, same NUMA pattern).
	 * QP requires 4-byte alignment on the value pointer; arena
	 * slots are at least 16-byte aligned (MIN_ORDER = 4).
	 */
	struct cds_ft_external_arena *leaf_arena = cds_ft_external_arena_create(NULL);

	for (unsigned int i = 0; i < n_keys; i++) {
		size_t kl;
		const void *kp = key_at(i, ibuf, &kl);
		entries[i] = cds_ft_external_arena_alloc(leaf_arena,
			sizeof(struct kv_entry) + kl);
		entries[i]->key_len = kl;
		memcpy(entries[i]->key, kp, kl);
		Tbl *new_qp = Tsetl(qp, kp, kl, (void *)entries[i]);
		if (!new_qp) {
			fprintf(stderr, "Tsetl failed at i=%u\n", i);
			exit(1);
		}
		qp = new_qp;
	}
	rss = get_rss_kb();

	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			void *sink = Tgetl(qp, kp, kl);
			FORCE_READ_LEAF(sink);
		}

	for (int r = 0; r < RUNS; r++) {
		uint64_t t0, t1;
		t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			void *sink = Tgetl(qp, kp, kl);
			FORCE_READ_LEAF(sink);
		}
		t1 = now_ns();
		double ns = (double)(t1 - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
	cds_ft_external_arena_destroy(leaf_arena);
	free(entries);
}

static void run_art(void)
{
	art_tree art;
	long rss;
	double best = 1e18;
	uint8_t ibuf[8];
	struct kv_entry **entries = calloc(n_keys, sizeof(struct kv_entry *));
	struct cds_ft_external_arena *leaf_arena = cds_ft_external_arena_create(NULL);

	/* ART needs prefix-free keys: strings include the NUL (str_lens[i]+1),
	 * integers are fixed length (key_len_bytes) and already prefix-free. */
	art_tree_init(&art);
	for (unsigned int i = 0; i < n_keys; i++) {
		size_t kl;
		const void *kp = key_at(i, ibuf, &kl);
		int alen = key_len_bytes ? (int)kl : (int)kl + 1;
		entries[i] = cds_ft_external_arena_alloc(leaf_arena,
			sizeof(struct kv_entry) + kl);
		entries[i]->key_len = kl;
		memcpy(entries[i]->key, kp, kl);
		art_insert(&art, (unsigned char *)kp, alen, (void *)entries[i]);
	}
	rss = get_rss_kb();

	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			int alen = key_len_bytes ? (int)kl : (int)kl + 1;
			void *sink = art_search(&art, (unsigned char *)kp, alen);
			FORCE_READ_LEAF(sink);
		}

	for (int r = 0; r < RUNS; r++) {
		uint64_t t0, t1;
		t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			int alen = key_len_bytes ? (int)kl : (int)kl + 1;
			void *sink = art_search(&art, (unsigned char *)kp, alen);
			FORCE_READ_LEAF(sink);
		}
		t1 = now_ns();
		double ns = (double)(t1 - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
	cds_ft_external_arena_destroy(leaf_arena);
	free(entries);
}

/*
 * HOT (Height Optimized Trie) engine — implemented in src/bench_hot.cpp, a
 * C++ shim over the ISC-licensed header-only HOT library (third_party/hot).
 */
void *hot_create(void);
void hot_destroy(void *h);
int hot_insert(void *h, const char *key);
const char *hot_lookup(void *h, const char *key);
void *hot_u64_create(void);
void hot_u64_destroy(void *h);
int hot_u64_insert(void *h, uint64_t *rec);
const void *hot_u64_lookup(void *h, uint64_t key);

static void run_hot(void)
{
	if (key_len_bytes) {
		/* Integer HOT (map-mode): value = pointer to the key record, so
		 * the lookup returns a cold pointer to force-read, like the other
		 * integer engines (not HOT's cheaper set-mode where value==key). */
		long rss;
		double best = 1e18;
		void *hot = hot_u64_create();
		uint64_t *recs = malloc(n_keys * sizeof(uint64_t));
		for (unsigned int i = 0; i < n_keys; i++) {
			recs[i] = int_keys[i];
			hot_u64_insert(hot, &recs[i]);
		}
		rss = get_rss_kb();
		for (int w = 0; w < WARMUP; w++)
			for (unsigned int i = 0; i < n_keys; i++) {
				const void *s = hot_u64_lookup(hot, int_keys[i]);
				FORCE_READ_LEAF(s);
			}
		for (int r = 0; r < RUNS; r++) {
			uint64_t t0 = now_ns();
			for (unsigned int i = 0; i < n_keys; i++) {
				const void *s = hot_u64_lookup(hot, int_keys[i]);
				FORCE_READ_LEAF(s);
			}
			double ns = (double)(now_ns() - t0) / n_keys;
			if (ns < best) best = ns;
		}
		printf("%.1f %ld\n", best, rss);
		hot_u64_destroy(hot);
		free(recs);
		return;
	}
	long rss;
	double best = 1e18;
	void *hot = hot_create();

	/*
	 * Store key COPIES from a dense cds_ft_external_arena and hand HOT a
	 * pointer into that arena (not into str_keys[]).  HOT's value IS its key,
	 * so without this its contentEquals + the FORCE_READ_LEAF below would hit
	 * the already-hot query buffer (near-free) -- unfair vs FT/qp/ART, which
	 * validate/touch a cold key copy.  Same arena the FT/qp/ART engines use,
	 * so HOT now pays the identical leaf-layout cost.
	 */
	struct cds_ft_external_arena *key_arena =
		cds_ft_external_arena_create(NULL);
	for (unsigned int i = 0; i < n_keys; i++) {
		char *copy = cds_ft_external_arena_alloc(key_arena,
			str_lens[i] + 1);
		memcpy(copy, str_keys[i], str_lens[i]);
		copy[str_lens[i]] = '\0';
		hot_insert(hot, copy);
	}
	rss = get_rss_kb();

	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			const char *sink = hot_lookup(hot, str_keys[i]);
			FORCE_READ_LEAF(sink);
		}

	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			const char *sink = hot_lookup(hot, str_keys[i]);
			FORCE_READ_LEAF(sink);
		}
		uint64_t t1 = now_ns();
		double ns = (double)(t1 - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
	hot_destroy(hot);
	cds_ft_external_arena_destroy(key_arena);
}

/*
 * Cuckoo Trie engine — implemented in src/bench_cuckoo.c, a C shim over the
 * Unlicense (public domain) Cuckoo Trie (third_party/cuckoo-trie).
 */
void *cuckoo_create(unsigned long num_keys);
int cuckoo_insert(void *t, const void *key, unsigned int len);
const void *cuckoo_lookup(void *t, const void *key, unsigned int len);

static void run_cuckoo(void)
{
	long rss;
	double best = 1e18;
	uint8_t ibuf[8];
	void *ct = cuckoo_create(n_keys);
	if (!ct) { fprintf(stderr, "cuckoo alloc failed\n"); printf("- 0\n"); return; }

	for (unsigned int i = 0; i < n_keys; i++) {
		size_t kl;
		const void *kp = key_at(i, ibuf, &kl);
		int r = cuckoo_insert(ct, kp, kl);
		if (r != 0) {
			fprintf(stderr, "cuckoo_insert status %d at i=%u "
				"(2=overflow,3=keytoolong)\n", r, i);
			printf("- 0\n");
			return;
		}
	}
	rss = get_rss_kb();

	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			const void *sink = cuckoo_lookup(ct, kp, kl);
			FORCE_READ_LEAF(sink);
		}

	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			const void *sink = cuckoo_lookup(ct, kp, kl);
			FORCE_READ_LEAF(sink);
		}
		uint64_t t1 = now_ns();
		double ns = (double)(t1 - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
}

/*
 * Masstree (kohler/masstree-beta, MIT) — single-threaded, via
 * src/bench_masstree_st.cpp.  Keys on arbitrary bytes; value = a cold kv_entry.
 */
void *masstree_st_create(void);
void masstree_st_insert(const void *key, int len, void *val);
void *masstree_st_lookup(const void *key, int len);

static void run_masstree(void)
{
	long rss;
	double best = 1e18;
	uint8_t ibuf[8];
	masstree_st_create();
	struct kv_entry **entries = calloc(n_keys, sizeof(struct kv_entry *));
	struct cds_ft_external_arena *arena = cds_ft_external_arena_create(NULL);

	for (unsigned int i = 0; i < n_keys; i++) {
		size_t kl;
		const void *kp = key_at(i, ibuf, &kl);
		entries[i] = cds_ft_external_arena_alloc(arena,
			sizeof(struct kv_entry) + kl);
		entries[i]->key_len = kl;
		memcpy(entries[i]->key, kp, kl);
		masstree_st_insert(kp, (int)kl, (void *)entries[i]);
	}
	rss = get_rss_kb();
	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			void *sink = masstree_st_lookup(kp, (int)kl);
			FORCE_READ_LEAF(sink);
		}
	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			void *sink = masstree_st_lookup(kp, (int)kl);
			FORCE_READ_LEAF(sink);
		}
		double ns = (double)(now_ns() - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
	cds_ft_external_arena_destroy(arena);
	free(entries);
}

/*
 * ART-OLC (flode/ARTSynchronized, Apache-2.0) — single-threaded, via
 * src/bench_artolc_st.cpp.  Byte radix => prefix-free keys: integers are fixed
 * length; strings include the NUL.  Value = a cold artolc_st_kv the loadKey
 * callback reconstructs and the harness force-reads.
 */
void *artolc_st_create(void);
void artolc_st_insert(const void *key, int len, uint64_t tid_val);
uint64_t artolc_st_lookup(const void *key, int len);

struct artolc_st_kv { uint32_t len; uint8_t bytes[]; };	/* matches the shim */

static void run_artolc(void)
{
	long rss;
	double best = 1e18;
	uint8_t ibuf[8];
	artolc_st_create();
	struct artolc_st_kv **kvs = calloc(n_keys, sizeof(struct artolc_st_kv *));
	struct cds_ft_external_arena *arena = cds_ft_external_arena_create(NULL);

	for (unsigned int i = 0; i < n_keys; i++) {
		size_t kl;
		const void *kp = key_at(i, ibuf, &kl);
		int alen = key_len_bytes ? (int)kl : (int)kl + 1;
		kvs[i] = cds_ft_external_arena_alloc(arena,
			sizeof(struct artolc_st_kv) + alen);
		kvs[i]->len = (uint32_t)alen;
		memcpy(kvs[i]->bytes, kp, alen);
		artolc_st_insert(kp, alen, (uint64_t)(uintptr_t)kvs[i]);
	}
	rss = get_rss_kb();
	for (int w = 0; w < WARMUP; w++)
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			int alen = key_len_bytes ? (int)kl : (int)kl + 1;
			uint64_t tid = artolc_st_lookup(kp, alen);
			FORCE_READ_LEAF((const void *)(uintptr_t)tid);
		}
	for (int r = 0; r < RUNS; r++) {
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < n_keys; i++) {
			size_t kl;
			const void *kp = key_at(i, ibuf, &kl);
			int alen = key_len_bytes ? (int)kl : (int)kl + 1;
			uint64_t tid = artolc_st_lookup(kp, alen);
			FORCE_READ_LEAF((const void *)(uintptr_t)tid);
		}
		double ns = (double)(now_ns() - t0) / n_keys;
		if (ns < best) best = ns;
	}
	printf("%.1f %ld\n", best, rss);
	cds_ft_external_arena_destroy(arena);
	free(kvs);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <dataset> <engine>\n"
			"  dataset: u32d u32s u64d u64s dns dict paths\n"
			"  engine:  ft_eager ft_eager_on_spec ft_cand ft_spec judy judyl judysl judyhs qp art hot cuckoo masstree artolc\n", argv[0]);
		return 1;
	}

	rcu_register_thread();
	/*
	 * Clear any inherited PR_SET_THP_DISABLE so the FT internal-node arena's
	 * MADV_HUGEPAGE is honored (the agent sandbox inherits THP disabled).
	 * Set BENCH_KEEP_THP_DISABLE=1 to skip.  Mirrors load-names.
	 */
	if (!getenv("BENCH_KEEP_THP_DISABLE")) {
		(void) prctl(PR_SET_THP_DISABLE, 0, 0, 0, 0);
	}
	{
		const char *e = getenv("FT_LATENCY_HIST");
		g_latency_hist = (e && *e && *e != '0');
	}
	g_dataset_label = argv[1];
	gen_keys(argv[1]);
	setup_miss_queries();

	if (strcmp(argv[2], "ft_eager") == 0)          run_ft(0, 0, 0);
	else if (strcmp(argv[2], "ft_eager_on_spec") == 0) run_ft(1, 0, 0);
	else if (strcmp(argv[2], "ft_cand") == 0)      run_ft(1, 1, 0);
	else if (strcmp(argv[2], "ft_spec") == 0)      run_ft(0, 0, 1);
	else if (strcmp(argv[2], "judy") == 0)    run_judy();
	else if (strcmp(argv[2], "judyl") == 0)   run_judyl();
	else if (strcmp(argv[2], "judysl") == 0)  run_judysl();
	else if (strcmp(argv[2], "judyhs") == 0)  run_judyhs();
	else if (strcmp(argv[2], "qp") == 0)      run_qp();
	else if (strcmp(argv[2], "art") == 0)     run_art();
	else if (strcmp(argv[2], "hot") == 0)     run_hot();
	else if (strcmp(argv[2], "cuckoo") == 0)  run_cuckoo();
	else if (strcmp(argv[2], "masstree") == 0) run_masstree();
	else if (strcmp(argv[2], "artolc") == 0)  run_artolc();
	else { fprintf(stderr, "Unknown engine: %s\n", argv[2]); return 1; }

	rcu_unregister_thread();
	return 0;
}
