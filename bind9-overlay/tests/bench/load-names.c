/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MPOL_INTERLEAVE
#define MPOL_INTERLEAVE 3
#endif

#include <isc/barrier.h>
#include <isc/file.h>
#include <isc/hashmap.h>
#include <isc/ht.h>
#include <isc/lib.h>
#include <isc/list.h>
#include <isc/refcount.h>
#include <isc/rwlock.h>
#include <isc/thread.h>
#include <isc/urcu.h>
#include <isc/util.h>

#include <dns/fixedname.h>
#include <dns/lib.h>
#include <dns/qp.h>
#include <dns/types.h>

#include <urcu/fractal-trie.h>

#include "dns/name.h"
#include "qp_p.h"

#include <tests/dns.h>
#include <tests/qp.h>

struct item_s {
	const char *text;
	dns_fixedname_t fixed;
	struct cds_lfht_node ht_node;
	struct cds_ft_node ft_node;
	size_t ft_key_len;
	uint8_t ft_key[sizeof(dns_qpkey_t)];
} item[1024 * 1024];

/*
 * Compact NUMA-interleaved arena for FT external nodes.  Holds only the
 * fields the FT lookup hot path needs (cds_ft_node + key bytes + key
 * length), allocated via mmap + mbind(MPOL_INTERLEAVE) so pages are
 * round-robin'd across all NUMA nodes.  This decouples external-node
 * placement from the main-thread first-touch of item[] (which lives on
 * a single node) and removes item[] reads from the FT query hot path.
 */
struct ft_arena_slot {
	struct cds_ft_node node;
	size_t key_len;
	uint8_t key[sizeof(dns_qpkey_t)];
};

#define FT_ARENA_SLOT_BYTES \
	((sizeof(struct ft_arena_slot) + 63) & ~(size_t)63)

static struct ft_arena_slot *ft_arena = NULL;
static size_t ft_arena_n_slots = 0;
static size_t ft_arena_total_bytes = 0;

/*
 * QUERY_LOOPS env var: each per-thread query phase is repeated this
 * many times so the query phase dominates wall time when running
 * under perf stat.  Default 1 (existing behavior).
 */
static size_t g_query_loops = 1;

/*
 * Index table for the cds_ft_alloc_external engine.  Maps insertion
 * count to the FT-allocated slot pointer so the lookup hot path can
 * find the slot without going through item[].  The table itself is
 * NUMA-interleaved so reading ft_alloc_index[count] does not pin
 * cross-socket readers to a single node.
 */
static struct ft_arena_slot **ft_alloc_index = NULL;
static size_t ft_alloc_index_n_slots = 0;
static size_t ft_alloc_index_total_bytes = 0;

/*
 * Separate query-key buffer used as the lookup input.  Mirrors how
 * a DNS server feeds a query key from the wire packet — the input
 * key does NOT come from the trie's external slot.  This lets us
 * measure the eager-lookup engines (ft_eager*) faithfully: the lookup
 * walks on these bytes, never touching the external slot, while the
 * speculative-lookup engines (ft_spec*) still read the external slot
 * during SIMD validation.  NUMA-interleaved.
 */
struct query_key {
	size_t len;
	uint8_t bytes[sizeof(dns_qpkey_t)];
};
#define QUERY_KEY_SLOT_BYTES \
	((sizeof(struct query_key) + 63) & ~(size_t)63)
static struct query_key *query_keys = NULL;
static size_t query_keys_n_slots = 0;
static size_t query_keys_total_bytes = 0;

static struct query_key *
query_key_at(size_t i) {
	return (struct query_key *)((char *)query_keys +
				    i * QUERY_KEY_SLOT_BYTES);
}

/*
 * Per-item user payload region used only by the qp_arena engine to
 * model a realistic post-lookup access pattern: in production, QP's
 * pval points to a separately-allocated user object (e.g. a DNS DB
 * node), not into QP's keying arena.  64-byte slots, NUMA-interleaved.
 *
 * FT engines do not need this region: ft_specv_alloc already returns
 * a leaf body allocated via cds_ft_alloc_external — that allocation
 * IS the FT user payload, and FT validation already touches it on the
 * lookup path.
 */
#define USER_PAYLOAD_SLOT_BYTES 64
static uint8_t *user_payload = NULL;
static size_t user_payload_n_slots = 0;
static size_t user_payload_total_bytes = 0;

static uint8_t *
user_payload_at(size_t i) {
	return user_payload + i * USER_PAYLOAD_SLOT_BYTES;
}

static long
sys_mbind(void *start, unsigned long len, int mode,
	  const unsigned long *nmask, unsigned long maxnode, unsigned flags) {
	return syscall(__NR_mbind, start, len, mode, nmask, maxnode, flags);
}

static struct ft_arena_slot *
ft_arena_at(size_t i) {
	return (struct ft_arena_slot *)((char *)ft_arena +
					i * FT_ARENA_SLOT_BYTES);
}

static void
ft_arena_create(size_t n) {
	ft_arena_n_slots = n;
	ft_arena_total_bytes = FT_ARENA_SLOT_BYTES * n;
	ft_arena = mmap(NULL, ft_arena_total_bytes, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ft_arena == MAP_FAILED) {
		perror("mmap");
		abort();
	}
	/* Interleave across nodes 0..23 (covers both sockets, 12 each). */
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(ft_arena, ft_arena_total_bytes, MPOL_INTERLEAVE,
		      &mask, 25, 0) < 0) {
		perror("mbind(MPOL_INTERLEAVE)");
	}
	/* Touch every page from the calling thread — interleave policy
	 * places each page on a node round-robin, independent of toucher. */
	memset(ft_arena, 0, ft_arena_total_bytes);
}

/*
 * NUMA-interleaved arena for external slots used by the _il engine
 * variants.  Same MPOL_INTERLEAVE layout as ft_arena, bump-allocated.
 */
static struct ft_arena_slot *ft_il_arena = NULL;
static size_t ft_il_arena_total_bytes = 0;

static struct ft_arena_slot *
ft_il_arena_at(size_t i) {
	return (struct ft_arena_slot *)((char *)ft_il_arena +
					i * FT_ARENA_SLOT_BYTES);
}

static void
ft_il_arena_create(size_t n) {
	ft_il_arena_total_bytes = FT_ARENA_SLOT_BYTES * n;
	ft_il_arena = mmap(NULL, ft_il_arena_total_bytes,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ft_il_arena == MAP_FAILED) {
		perror("mmap(ft_il_arena)");
		abort();
	}
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(ft_il_arena, ft_il_arena_total_bytes,
		      MPOL_INTERLEAVE, &mask, 25, 0) < 0) {
		perror("mbind(ft_il_arena)");
	}
	memset(ft_il_arena, 0, ft_il_arena_total_bytes);
}

/*
 * Default-policy arena for external slots used by the _local engine
 * variants.  mmap without mbind → pages get default first-touch
 * placement (whichever NUMA node the first writer ran on).
 */
static struct ft_arena_slot *ft_local_arena = NULL;
static size_t ft_local_arena_total_bytes = 0;

static struct ft_arena_slot *
ft_local_arena_at(size_t i) {
	return (struct ft_arena_slot *)((char *)ft_local_arena +
					i * FT_ARENA_SLOT_BYTES);
}

static void
ft_local_arena_create(size_t n) {
	ft_local_arena_total_bytes = FT_ARENA_SLOT_BYTES * n;
	ft_local_arena = mmap(NULL, ft_local_arena_total_bytes,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ft_local_arena == MAP_FAILED) {
		perror("mmap(ft_local_arena)");
		abort();
	}
	/*
	 * Deliberately NO memset here: faulting the arena from the main
	 * thread would place every leaf page on the main thread's NUMA
	 * node.  MAP_ANONYMOUS pages are zero on first fault, and the
	 * per-iteration MADV_DONTNEED in the sweep loop keeps them that
	 * way; the worker threads do the first touch so each thread's
	 * leaf slice lands on its own local node.
	 */
}

static void
ft_alloc_index_create(size_t n) {
	ft_alloc_index_n_slots = n;
	ft_alloc_index_total_bytes = n * sizeof(struct ft_arena_slot *);
	ft_alloc_index = mmap(NULL, ft_alloc_index_total_bytes,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ft_alloc_index == MAP_FAILED) {
		perror("mmap(ft_alloc_index)");
		abort();
	}
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(ft_alloc_index, ft_alloc_index_total_bytes,
		      MPOL_INTERLEAVE, &mask, 25, 0) < 0) {
		perror("mbind(ft_alloc_index)");
	}
	memset(ft_alloc_index, 0, ft_alloc_index_total_bytes);
}

static void
user_payload_create(size_t n) {
	user_payload_n_slots = n;
	user_payload_total_bytes = USER_PAYLOAD_SLOT_BYTES * n;
	user_payload = mmap(NULL, user_payload_total_bytes,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (user_payload == MAP_FAILED) {
		perror("mmap(user_payload)");
		abort();
	}
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(user_payload, user_payload_total_bytes,
		      MPOL_INTERLEAVE, &mask, 25, 0) < 0) {
		perror("mbind(user_payload)");
	}
	memset(user_payload, 0, user_payload_total_bytes);
}

static void
query_keys_create(size_t n) {
	query_keys_n_slots = n;
	query_keys_total_bytes = QUERY_KEY_SLOT_BYTES * n;
	query_keys = mmap(NULL, query_keys_total_bytes,
			  PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (query_keys == MAP_FAILED) {
		perror("mmap(query_keys)");
		abort();
	}
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(query_keys, query_keys_total_bytes,
		      MPOL_INTERLEAVE, &mask, 25, 0) < 0) {
		perror("mbind(query_keys)");
	}
	memset(query_keys, 0, query_keys_total_bytes);
}

static void
dump_arena_numa_distribution(const char *label) {
	if (!ft_arena || ft_arena_n_slots == 0)
		return;
	long pagesize = sysconf(_SC_PAGESIZE);
	size_t n_pages = (ft_arena_total_bytes + pagesize - 1) / pagesize;
	void **pages = malloc(n_pages * sizeof(void *));
	int *status = malloc(n_pages * sizeof(int));
	char *base = (char *)ft_arena;
	for (size_t i = 0; i < n_pages; i++)
		pages[i] = base + i * pagesize;
	long ret = syscall(__NR_move_pages, 0, n_pages, pages, NULL, status, 0);
	if (ret < 0) {
		perror("move_pages(arena)");
		free(pages);
		free(status);
		return;
	}
	int counts[64] = { 0 };
	int unknown = 0;
	for (size_t i = 0; i < n_pages; i++) {
		if (status[i] >= 0 && status[i] < 64)
			counts[status[i]]++;
		else
			unknown++;
	}
	fprintf(stderr,
		"\n%s: ft_arena NUMA placement (%zu pages, %.1f MB):\n",
		label, n_pages, ft_arena_total_bytes / (1024.0 * 1024.0));
	for (int i = 0; i < 64; i++)
		if (counts[i])
			fprintf(stderr, "  node %2d: %6d (%5.1f%%)\n", i,
				counts[i], 100.0 * counts[i] / n_pages);
	if (unknown)
		fprintf(stderr, "  unknown: %d\n", unknown);
	fflush(stderr);
	free(pages);
	free(status);
}

static long
sys_move_pages(int pid, unsigned long count, void **pages,
	       const int *nodes, int *status, int flags) {
	return syscall(__NR_move_pages, pid, count, pages, nodes, status, flags);
}

static void
dump_item_layout(void) {
	fprintf(stderr,
		"struct item_s layout (1 element = %zu bytes, %.1f cachelines):\n"
		"  text       @ %4zu (size %zu)\n"
		"  fixed      @ %4zu (size %zu)\n"
		"  ht_node    @ %4zu (size %zu)\n"
		"  ft_node    @ %4zu (size %zu)\n"
		"  ft_key_len @ %4zu (size %zu)\n"
		"  ft_key     @ %4zu (size %zu)\n"
		"  total item[] = %.1f MB\n",
		sizeof(struct item_s),
		(double)sizeof(struct item_s) / 64,
		offsetof(struct item_s, text), sizeof(((struct item_s *)0)->text),
		offsetof(struct item_s, fixed), sizeof(((struct item_s *)0)->fixed),
		offsetof(struct item_s, ht_node), sizeof(((struct item_s *)0)->ht_node),
		offsetof(struct item_s, ft_node), sizeof(((struct item_s *)0)->ft_node),
		offsetof(struct item_s, ft_key_len), sizeof(((struct item_s *)0)->ft_key_len),
		offsetof(struct item_s, ft_key), sizeof(((struct item_s *)0)->ft_key),
		sizeof(item) / (1024.0 * 1024.0));
	fflush(stderr);
}

static void
dump_item_numa_distribution(const char *label) {
	size_t total_bytes = sizeof(item);
	long pagesize = sysconf(_SC_PAGESIZE);
	size_t n_pages = (total_bytes + pagesize - 1) / pagesize;
	void **pages = malloc(n_pages * sizeof(void *));
	int *status = malloc(n_pages * sizeof(int));
	char *base = (char *)item;
	for (size_t i = 0; i < n_pages; i++)
		pages[i] = base + i * pagesize;
	long ret = sys_move_pages(0, n_pages, pages, NULL, status, 0);
	if (ret < 0) {
		perror("move_pages");
		free(pages);
		free(status);
		return;
	}
	int counts[64] = { 0 };
	int unknown = 0;
	for (size_t i = 0; i < n_pages; i++) {
		if (status[i] >= 0 && status[i] < 64)
			counts[status[i]]++;
		else
			unknown++;
	}
	fprintf(stderr,
		"\n%s: item[] NUMA placement (%zu pages, %.1f MB):\n",
		label, n_pages, total_bytes / (1024.0 * 1024.0));
	for (int i = 0; i < 64; i++)
		if (counts[i])
			fprintf(stderr, "  node %2d: %6d (%5.1f%%)\n", i,
				counts[i], 100.0 * counts[i] / n_pages);
	if (unknown)
		fprintf(stderr, "  unknown: %d\n", unknown);
	fflush(stderr);
	free(pages);
	free(status);
}

static pthread_mutex_t ft_writer_mutex = PTHREAD_MUTEX_INITIALIZER;

isc_barrier_t barrier;
isc_rwlock_t rwl;

struct thread_s {
	isc_thread_t thread;
	struct fun *fun;
	void *map;
	size_t start;
	size_t end;
	unsigned int cpu;	/* Pinned CPU id (thread index). */
	uint64_t d0;
	uint64_t d1;
	struct query_key *local_keys;	/* Thread-local NUMA-local input
					   keys for [start..end), modeling a
					   network packet arriving on the
					   receiving thread's local DRAM. */
} threads[1024];

/*
 * Pin the calling worker thread to a single physical CPU.  Without
 * pinning, the kernel scheduler may co-locate threads on SMT siblings
 * of the same physical core; SMT siblings on Zen 4 share the L1d/L1i,
 * L2, and the 256-bit FPU/vector pipeline — so SIMD-heavy paths
 * (collapsed prefix scan, scan_16, scan_32) see throughput halved on
 * any core that hosts two siblings.  By assigning thread i to CPU i
 * (i in [0, 191] = first SMT thread of each physical core on
 * 2x96-core EPYC 9654), each worker gets a private FPU and L1/L2.
 *
 * TODO: replace this hardcoded "CPU id == thread index" mapping with
 * an hwloc-driven enumeration so the bench picks one PU per physical
 * core regardless of the host topology (different SMT degree, different
 * core/socket counts, different OS CPU id ordering).
 */
static void
pin_thread_to_cpu(unsigned int cpu) {
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
		perror("pthread_setaffinity_np");
		abort();
	}
}

/*
 * Thread-local pointer to the worker thread's local_keys slice.  Each
 * thread sets these on entry; the add/get helpers consult them so the
 * input key is always read from local DRAM rather than from any
 * NUMA-interleaved global buffer.
 */
static __thread struct query_key *tls_local_keys = NULL;
static __thread size_t tls_local_start = 0;

/*
 * Per-thread cache flusher.  Allocates a private scratch region (default
 * 64 MiB per thread, override via BENCH_CACHE_FLUSH_MB env var; set =0 to
 * disable) and reads through it once between the build and lookup phases.
 * Reading 64 MiB private per thread evicts L1/L2 fully on Zen 4 (1 MiB L2)
 * and, with 8 threads per CCX touching 64 MiB each, evicts the 32 MiB L3
 * many times over.  Without this, FT's larger build-phase metadata
 * footprint leaves the trie hot in L3 while QP's smaller footprint leaves
 * the trie even hotter — biasing short-lookup-phase comparisons in QP's
 * favour.
 */
static size_t g_cache_flush_bytes = 64 * 1024 * 1024;
static __thread volatile uint8_t *tls_flush_buf = NULL;
static __thread uint64_t tls_flush_acc = 0;

static void
flush_thread_caches(void) {
	if (g_cache_flush_bytes == 0)
		return;
	if (tls_flush_buf == NULL) {
		void *p = mmap(NULL, g_cache_flush_bytes,
			       PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			perror("mmap(flush)");
			abort();
		}
		/* First-touch on the worker thread → local NUMA. */
		memset(p, 0xa5, g_cache_flush_bytes);
		tls_flush_buf = p;
	}
	uint64_t acc = 0;
	const size_t step = 64; /* one cacheline */
	const volatile uint8_t *p = tls_flush_buf;
	for (size_t i = 0; i < g_cache_flush_bytes; i += step)
		acc += p[i];
	/* Sink to prevent dead-store elimination of acc. */
	tls_flush_acc += acc;
}

/*
 * Release the per-thread scratch mappings before a worker exits.  These
 * are mmap'd fresh by every spawned thread and the C runtime does NOT
 * reclaim them on thread exit, so without an explicit munmap each thread
 * leaks its scratch.  Across the (engine x thread-count) sweep the 64 MiB
 * cache-flush buffer alone leaks sum(thread_sweep) * 64 MiB ~= 35 GiB per
 * engine (~350 GiB for the full sweep) — this was the dominant source of
 * the whole-machine OOM, not the trie.
 */
static void
thread_scratch_release(struct thread_s *arg) {
	if (tls_flush_buf != NULL) {
		munmap((void *) tls_flush_buf, g_cache_flush_bytes);
		tls_flush_buf = NULL;
	}
	if (arg->local_keys != NULL) {
		size_t bytes = (arg->end - arg->start) *
			       sizeof(struct query_key);
		bytes = (bytes + 4095) & ~(size_t) 4095;
		munmap(arg->local_keys, bytes);
		arg->local_keys = NULL;
	}
	tls_local_keys = NULL;
}

static void
item_check(void *ctx, void *pval, uint32_t ival) {
	UNUSED(ctx);
	assert(pval == &item[ival]);
}

static size_t
item_makekey(dns_qpkey_t key, void *ctx, void *pval, uint32_t ival) {
	UNUSED(ctx);
	assert(pval == &item[ival]);
	return dns_qpkey_fromname(key, &item[ival].fixed.name,
				  DNS_DBNAMESPACE_NORMAL);
}

static void
testname(void *ctx, char *buf, size_t size) {
	REQUIRE(ctx == NULL);
	strlcpy(buf, "test", size);
}

const dns_qpmethods_t qpmethods = {
	item_check,
	item_check,
	item_makekey,
	testname,
};

#define CHECKN(count, result)                                       \
	do {                                                        \
		if (result != ISC_R_SUCCESS) {                      \
			dns_name_t *name = &item[count].fixed.name; \
			char buf[DNS_NAME_MAXTEXT] = { 0 };         \
			dns_name_format(name, buf, sizeof(buf));    \
			fprintf(stderr, "%s: %s\n", buf,            \
				isc_result_totext(result));         \
			fflush(stderr);                             \
			const char *_sess = getenv("LTTNG_SNAPSHOT_SESSION"); \
			if (_sess && _sess[0]) {                    \
				char _cmd[256];                     \
				snprintf(_cmd, sizeof(_cmd),        \
					"lttng snapshot record -s %s >&2", \
					_sess);                     \
				int _r = system(_cmd);              \
				(void) _r;                          \
				fflush(stderr);                     \
			}                                           \
			abort();                                    \
		}                                                   \
	} while (0)

struct fun {
	const char *name;
	void *(*new)(isc_mem_t *mem);
	isc_threadfunc_t thread;
	void (*destroy)(void *map);
};

/*
 * cds_lfht
 */

static void *
new_lfht(isc_mem_t *mem ISC_ATTR_UNUSED) {
	struct cds_lfht *lfht = cds_lfht_new(
		1, 1, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	return lfht;
}

static int
lfht_match(struct cds_lfht_node *ht_node, const void *_key) {
	const struct item_s *i = caa_container_of(ht_node, struct item_s,
						  ht_node);
	const dns_name_t *key = _key;

	return dns_name_equal(key, &i->fixed.name);
}

static isc_result_t
add_lfht(void *lfht, size_t count) {
	unsigned long hash = dns_name_hash(&item[count].fixed.name);

	struct cds_lfht_node *ht_node = cds_lfht_add_unique(
		lfht, hash, lfht_match, &item[count].fixed.name,
		&item[count].ht_node);

	if (ht_node != &item[count].ht_node) {
		return ISC_R_EXISTS;
	}

	return ISC_R_SUCCESS;
}

static isc_result_t
get_lfht(void *lfht, size_t count, void **pval) {
	unsigned long hash = dns_name_hash(&item[count].fixed.name);

	struct cds_lfht_iter iter;
	cds_lfht_lookup(lfht, hash, lfht_match, &item[count].fixed.name, &iter);

	struct cds_lfht_node *ht_node = cds_lfht_iter_get_node(&iter);
	if (ht_node == NULL) {
		return ISC_R_NOTFOUND;
	}

	*pval = caa_container_of(ht_node, struct item_s, ht_node);
	return ISC_R_SUCCESS;
}

static void *
thread_lfht(void *arg0) {
	struct thread_s *arg = arg0;

	isc_barrier_wait(&barrier);

	isc_time_t t0 = isc_time_now_hires();
	for (size_t n = arg->start; n < arg->end; n++) {
		isc_result_t result = add_lfht(arg->map, n);
		CHECKN(n, result);
	}
	isc_time_t t1 = isc_time_now_hires();

	isc_barrier_wait(&barrier);
	flush_thread_caches();
	isc_barrier_wait(&barrier);
	isc_time_t t1b = isc_time_now_hires();

	for (size_t n = arg->start; n < arg->end; n++) {
		void *pval = NULL;
		isc_result_t result = get_lfht(arg->map, n, &pval);
		CHECKN(n, result);
		assert(pval == &item[n]);
	}

	isc_time_t t2 = isc_time_now_hires();

	arg->d0 = isc_time_microdiff(&t1, &t0);
	arg->d1 = isc_time_microdiff(&t2, &t1b);

	return NULL;
}

/*
 * hashmap
 */

static void *
new_hashmap(isc_mem_t *mem) {
	isc_hashmap_t *hashmap = NULL;
	isc_hashmap_create(mem, 1, &hashmap);

	return hashmap;
}

static bool
name_match(void *node, const void *key) {
	const struct item_s *i = node;
	return dns_name_equal(&i->fixed.name, key);
}

static isc_result_t
add_hashmap(void *hashmap, size_t count) {
	isc_result_t result = isc_hashmap_add(
		hashmap, dns_name_hash(&item[count].fixed.name), name_match,
		&item[count].fixed.name, &item[count], NULL);
	return result;
}

static isc_result_t
get_hashmap(void *hashmap, size_t count, void **pval) {
	isc_result_t result = isc_hashmap_find(
		hashmap, dns_name_hash(&item[count].fixed.name), name_match,
		&item[count].fixed.name, pval);
	return result;
}

static void *
thread_hashmap(void *arg0) {
	struct thread_s *arg = arg0;

	isc_barrier_wait(&barrier);

	isc_time_t t0 = isc_time_now_hires();
	WRLOCK(&rwl);
	for (size_t n = arg->start; n < arg->end; n++) {
		isc_result_t result = add_hashmap(arg->map, n);
		CHECKN(n, result);
	}
	WRUNLOCK(&rwl);
	isc_time_t t1 = isc_time_now_hires();

	isc_barrier_wait(&barrier);
	flush_thread_caches();
	isc_barrier_wait(&barrier);
	isc_time_t t1b = isc_time_now_hires();

	RDLOCK(&rwl);
	for (size_t n = arg->start; n < arg->end; n++) {
		void *pval = NULL;
		isc_result_t result = get_hashmap(arg->map, n, &pval);
		CHECKN(n, result);
		assert(pval == &item[n]);
	}
	RDUNLOCK(&rwl);
	isc_time_t t2 = isc_time_now_hires();

	arg->d0 = isc_time_microdiff(&t1, &t0);
	arg->d1 = isc_time_microdiff(&t2, &t1b);

	return NULL;
}

/*
 * ht
 */

static void *
new_ht(isc_mem_t *mem) {
	isc_ht_t *ht = NULL;
	isc_ht_init(&ht, mem, 1, 0);
	return ht;
}

static isc_result_t
add_ht(void *ht, size_t count) {
	isc_result_t result = isc_ht_add(ht, item[count].fixed.name.ndata,
					 item[count].fixed.name.length,
					 &item[count]);
	return result;
}

static isc_result_t
get_ht(void *ht, size_t count, void **pval) {
	isc_result_t result = isc_ht_find(ht, item[count].fixed.name.ndata,
					  item[count].fixed.name.length, pval);
	return result;
}

static void *
thread_ht(void *arg0) {
	struct thread_s *arg = arg0;

	isc_barrier_wait(&barrier);

	isc_time_t t0 = isc_time_now_hires();
	WRLOCK(&rwl);
	for (size_t n = arg->start; n < arg->end; n++) {
		isc_result_t result = add_ht(arg->map, n);
		CHECKN(n, result);
	}
	WRUNLOCK(&rwl);
	isc_time_t t1 = isc_time_now_hires();

	isc_barrier_wait(&barrier);
	flush_thread_caches();
	isc_barrier_wait(&barrier);
	isc_time_t t1b = isc_time_now_hires();

	RDLOCK(&rwl);
	for (size_t n = arg->start; n < arg->end; n++) {
		void *pval = NULL;
		isc_result_t result = get_ht(arg->map, n, &pval);
		CHECKN(n, result);
		assert(pval == &item[n]);
	}
	RDUNLOCK(&rwl);
	isc_time_t t2 = isc_time_now_hires();

	arg->d0 = isc_time_microdiff(&t1, &t0);
	arg->d1 = isc_time_microdiff(&t2, &t1b);

	return NULL;
}

/*
 * qp
 */

static void *
new_qp(isc_mem_t *mem) {
	dns_qpmulti_t *qpmulti = NULL;
	dns_qpmulti_create(mem, &qpmethods, NULL, &qpmulti);
	return qpmulti;
}

static isc_result_t
add_qp(void *qp, size_t count) {
	isc_result_t result = dns_qp_insert(qp, &item[count], count);
	return result;
}

static void
sqz_qp(void *qp) {
	dns_qp_compact(qp, DNS_QPGC_MAYBE);
}

static isc_result_t
get_qp(void *qp, size_t count, void **pval) {
	return dns_qp_getname(qp, &item[count].fixed.name,
			      DNS_DBNAMESPACE_NORMAL, pval, NULL);
}

static void *
_thread_qp(void *arg0, bool sqz, bool brr) {
	struct thread_s *arg = arg0;

	isc_barrier_wait(&barrier);

	dns_qp_t *qp = NULL;
	dns_qpmulti_write(arg->map, &qp);

	isc_time_t t0 = isc_time_now_hires();
	for (size_t n = arg->start; n < arg->end; n++) {
		isc_result_t result = add_qp(qp, n);
		CHECKN(n, result);
	}
	if (sqz) {
		sqz_qp(qp);
	}
	dns_qpmulti_commit(arg->map, &qp);
	if (brr) {
		rcu_barrier();
	}

	isc_time_t t1 = isc_time_now_hires();

	isc_barrier_wait(&barrier);
	flush_thread_caches();
	isc_barrier_wait(&barrier);
	isc_time_t t1b = isc_time_now_hires();

	dns_qpread_t qpr;
	dns_qpmulti_query(arg->map, &qpr);

	for (size_t loop = 0; loop < g_query_loops; loop++) {
		for (size_t n = arg->start; n < arg->end; n++) {
			void *pval = NULL;
			isc_result_t result = get_qp(&qpr, n, &pval);
			CHECKN(n, result);
			assert(pval == &item[n]);
		}
	}

	dns_qpread_destroy(arg->map, &qpr);

	isc_time_t t2 = isc_time_now_hires();

	arg->d0 = isc_time_microdiff(&t1, &t0);
	arg->d1 = isc_time_microdiff(&t2, &t1b);

	return NULL;
}

static void *
thread_qp(void *arg0) {
	return _thread_qp(arg0, true, false);
}

static void *
thread_qp_nosqz(void *arg0) {
	return _thread_qp(arg0, false, false);
}

static void *
thread_qp_brr(void *arg0) {
	return _thread_qp(arg0, true, true);
}

/*
 * fractal trie (FT)
 */

enum ft_mode {
	FT_MODE_PRECISE,
	FT_MODE_CAND,
	FT_MODE_SPECV,
	FT_MODE_SPECV_ALLOC,
	FT_MODE_SPECV_CALLOC,
};

/*
 * Set by _new_ft() and consumed by destroy_ft().  Safe as a single
 * slot because the engine sweep runs one map at a time.
 */
static struct cds_ft_group *g_ft_group_for_destroy;

/*
 * External-node arena for the ft_spec_extarena engine: leaves are
 * allocated from the FT library's own external arena
 * (cds_ft_external_arena_*) instead of the bench-owned ft_*_arena.
 * Set by new_ft_spec_extarena(), consumed by add_ft_specv_extarena()
 * and destroy_ft_extarena().  Single slot for the same serial-sweep
 * reason as g_ft_group_for_destroy.  This exercises the library
 * allocator's MADV_HUGEPAGE + 2 MiB-interleave path for external
 * nodes; toggle with CDS_FT_HUGEPAGE / CDS_FT_NUMA_INTERLEAVE.
 */
static struct cds_ft_external_arena *g_ft_ext_arena;

static void *
_new_ft(enum ft_mode mode) {
	struct cds_ft_group_attr *group_attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;
	cds_ft_group_attr_create(&group_attr);
	cds_ft_group_attr_set_max_key_len(group_attr, sizeof(dns_qpkey_t));
	if (mode == FT_MODE_CAND || mode == FT_MODE_SPECV ||
	    mode == FT_MODE_SPECV_ALLOC || mode == FT_MODE_SPECV_CALLOC) {
		/*
		 * Speculative-tuned descent (skip-compressed encoding +
		 * cand-style compressed-byte handling).  In the unified
		 * API, group attr only selects the optimization; the
		 * per-call memcmp parameters move to
		 * cds_ft_speculative_lookup_key call args (key_offset,
		 * key_readable_pad).
		 */
		cds_ft_group_attr_set_lookup_optimization(group_attr,
			CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE);
	} else {
		/*
		 * FT_MODE_PRECISE: optimized eager trie.  Set EAGER
		 * explicitly so PRECISE measures the eager-trie path
		 * regardless of the library's default (which is
		 * SPECULATIVE post-flip).
		 */
		cds_ft_group_attr_set_lookup_optimization(group_attr,
			CDS_FT_LOOKUP_OPTIMIZE_EAGER);
	}
	{
		const char *np = getenv("BENCH_FT_NUMA_POLICY");
		if (np) {
			if (!strcmp(np, "interleave"))
				cds_ft_group_attr_set_numa_policy(group_attr,
					CDS_FT_NUMA_INTERLEAVE);
			else if (!strcmp(np, "local"))
				cds_ft_group_attr_set_numa_policy(group_attr,
					CDS_FT_NUMA_LOCAL);
		}
	}
	{
		const char *o = getenv("BENCH_FT_OPTIMIZE");
		if (o && !strcmp(o, "rss"))
			cds_ft_group_attr_set_optimize(group_attr, CDS_FT_OPTIMIZE_RSS);
		else if (o && !strcmp(o, "throughput"))
			cds_ft_group_attr_set_optimize(group_attr, CDS_FT_OPTIMIZE_THROUGHPUT);
	}
	cds_ft_group_create(group_attr, &group);
	cds_ft_group_attr_destroy(group_attr);
	cds_ft_create(group, NULL, &ft);
	/*
	 * Stash the group so destroy_ft() can tear down the per-group
	 * internal-node arena after the run.  The engine sweep is
	 * serial — exactly one FT map is live at a time and it is
	 * destroyed before the next _new_ft() — so a single file-scope
	 * slot is sufficient.  Without this the group pointer is
	 * unrecoverable from the ft and every (engine x thread-count)
	 * iteration leaks a full ~1M-key internal arena.
	 */
	g_ft_group_for_destroy = group;
	return ft;
}

/*
 * Tear down an FT map created by _new_ft().  cds_ft_destroy() frees the
 * root node and decrements the group's instance count;
 * cds_ft_group_destroy() then munmaps the internal-node arena
 * superblocks.  The external (leaf) nodes live in the bench-owned
 * arenas (item[] / ft_il_arena / ft_local_arena) and are never
 * registered with the library, so they are untouched and remain
 * reusable across iterations.
 */
static void
destroy_ft(void *map) {
	struct cds_ft *ft = map;
	struct cds_ft_group *group = g_ft_group_for_destroy;

	cds_ft_destroy(ft);
	if (group != NULL)
		(void) cds_ft_group_destroy(group);
	g_ft_group_for_destroy = NULL;
}

/*
 * Speculative engine whose external (leaf) nodes are allocated from the
 * FT library external arena rather than the bench-owned ft_*_arena.
 * The library arena applies MADV_HUGEPAGE + 2 MiB NUMA interleave to
 * its ranges, so this engine measures the leaf-side benefit of the FT
 * allocator (vs the *_local / *_il engines whose leaves are plain
 * bench mmaps).  NUMA placement follows the process policy
 * (CDS_FT_NUMA_DEFAULT); run under `numactl --interleave=all` for the
 * interleaved variant, plain for first-touch-local.
 */
static void *
new_ft_spec_extarena(isc_mem_t *mem ISC_ATTR_UNUSED) {
	struct cds_ft *ft = _new_ft(FT_MODE_SPECV_CALLOC);

	{
		struct cds_ft_external_arena_attr *ea = NULL;
		const char *o = getenv("BENCH_FT_OPTIMIZE");
		if (o) {
			cds_ft_external_arena_attr_create(&ea);
			cds_ft_external_arena_attr_set_optimize(ea, !strcmp(o, "rss") ?
				CDS_FT_OPTIMIZE_RSS : CDS_FT_OPTIMIZE_THROUGHPUT);
		}
		g_ft_ext_arena = cds_ft_external_arena_create(ea);
		if (ea)
			cds_ft_external_arena_attr_destroy(ea);
	}
	if (g_ft_ext_arena == NULL)
		abort();
	return ft;
}

static void
destroy_ft_extarena(void *map) {
	/*
	 * Order matters: tear down the trie + group first (no readers
	 * after cds_ft_group_destroy), then free every leaf the external
	 * arena served.  Without the arena destroy each iteration leaks
	 * ~1M leaves.
	 */
	destroy_ft(map);
	cds_ft_external_arena_destroy(g_ft_ext_arena);
	g_ft_ext_arena = NULL;
}

/*
 * Process-wide AnonHugePages (kB) from /proc/self/smaps_rollup — how much
 * of this process's anonymous memory is currently backed by transparent
 * hugepages.  Used to self-report whether MADV_HUGEPAGE on the FT arenas
 * actually formed hugepages this run.
 */
static size_t
proc_anonhugepages_kb(void) {
	FILE *f = fopen("/proc/self/smaps_rollup", "r");
	char line[256];
	size_t kb = 0;
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "AnonHugePages: %zu kB", &kb) == 1)
			break;
	fclose(f);
	return kb;
}

static void *
new_ft_cand(isc_mem_t *mem ISC_ATTR_UNUSED) {
	return _new_ft(FT_MODE_CAND);
}

static void *
new_ft_precise(isc_mem_t *mem ISC_ATTR_UNUSED) {
	return _new_ft(FT_MODE_PRECISE);
}

static void *
new_ft_specv(isc_mem_t *mem ISC_ATTR_UNUSED) {
	return _new_ft(FT_MODE_SPECV);
}

static void *
new_ft_spec_il(isc_mem_t *mem ISC_ATTR_UNUSED) {
	/* SPECULATIVE-optimized trie, NUMA-interleaved leaf arena. */
	return _new_ft(FT_MODE_SPECV_CALLOC);
}

static void *
new_ft_spec_local(isc_mem_t *mem ISC_ATTR_UNUSED) {
	/* SPECULATIVE-optimized trie, local-node leaf arena. */
	return _new_ft(FT_MODE_SPECV_CALLOC);
}

static void *
new_ft_eager_il(isc_mem_t *mem ISC_ATTR_UNUSED) {
	/* EAGER-optimized trie, NUMA-interleaved leaf arena. */
	return _new_ft(FT_MODE_PRECISE);
}

static void *
new_ft_eager_local(isc_mem_t *mem ISC_ATTR_UNUSED) {
	/* EAGER-optimized trie, local-node leaf arena. */
	return _new_ft(FT_MODE_PRECISE);
}

static void *
new_ft_cand_il(isc_mem_t *mem ISC_ATTR_UNUSED) {
	return _new_ft(FT_MODE_CAND);
}

/*
 * Slot bodies from a NUMA-interleaved arena (mmap + MPOL_INTERLEAVE
 * across all nodes).  Bump-allocated, no per-slot bookkeeping — the
 * only difference vs the _calloc variant is the NUMA policy.
 */
static isc_result_t
add_ft_specv_il(void *ft, size_t count) {
	struct ft_arena_slot *slot = ft_il_arena_at(count);
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	memcpy(slot->key, key, len);
	slot->key_len = len;
	memcpy(qk->bytes, key, len);
	qk->len = len;
	ft_alloc_index[count] = slot;
	struct cds_ft_node *result;
	enum cds_ft_status s = cds_ft_insert_unique(
		ft, slot->key, slot->key_len, &slot->node, &result);
	if (s == CDS_FT_STATUS_OK || s == CDS_FT_STATUS_DUPLICATE_FOUND)
		return ISC_R_SUCCESS;
	return ISC_R_FAILURE;
}

/*
 * Slot bodies from a default-policy arena (mmap, no mbind).  Pages
 * land on the first-toucher's NUMA node — the only difference vs the
 * _il variant is the absence of MPOL_INTERLEAVE.
 */
static isc_result_t
add_ft_specv_local(void *ft, size_t count) {
	struct ft_arena_slot *slot = ft_local_arena_at(count);
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	memcpy(slot->key, key, len);
	slot->key_len = len;
	memcpy(qk->bytes, key, len);
	qk->len = len;
	ft_alloc_index[count] = slot;
	struct cds_ft_node *result;
	enum cds_ft_status s = cds_ft_insert_unique(
		ft, slot->key, slot->key_len, &slot->node, &result);
	if (s == CDS_FT_STATUS_OK || s == CDS_FT_STATUS_DUPLICATE_FOUND)
		return ISC_R_SUCCESS;
	return ISC_R_FAILURE;
}

/*
 * Same as add_ft_specv_local, but the leaf slot is allocated from the
 * FT library external arena (hugepage + interleave capable) instead of
 * the bench-owned ft_local_arena.  cds_ft_external_arena_alloc is
 * mutex-synchronised, so concurrent writers may share g_ft_ext_arena.
 */
static isc_result_t
add_ft_specv_extarena(void *ft, size_t count) {
	struct ft_arena_slot *slot = cds_ft_external_arena_alloc(
		g_ft_ext_arena, sizeof(struct ft_arena_slot));
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len;

	if (slot == NULL)
		return ISC_R_FAILURE;
	len = dns_qpkey_fromname(key, &item[count].fixed.name,
				 DNS_DBNAMESPACE_NORMAL);
	memcpy(slot->key, key, len);
	slot->key_len = len;
	memcpy(qk->bytes, key, len);
	qk->len = len;
	ft_alloc_index[count] = slot;
	struct cds_ft_node *result;
	enum cds_ft_status s = cds_ft_insert_unique(
		ft, slot->key, slot->key_len, &slot->node, &result);
	if (s == CDS_FT_STATUS_OK || s == CDS_FT_STATUS_DUPLICATE_FOUND)
		return ISC_R_SUCCESS;
	return ISC_R_FAILURE;
}

static isc_result_t
get_ft_specv_alloc(void *ft, size_t count, void **pval) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	struct cds_ft_node *found;
	cds_ft_speculative_lookup_key(ft, qk->bytes, qk->len, 32,
		offsetof(struct ft_arena_slot, key) -
			offsetof(struct ft_arena_slot, node),
		&found);
	if (!found)
		return ISC_R_NOTFOUND;
	*pval = caa_container_of(found, struct ft_arena_slot, node);
	return ISC_R_SUCCESS;
}

/*
 * Eager lookup: exact in-trie descent (cds_ft_eager_lookup_key), no
 * candidate/speculative step and no caller-side memcmp.  Same arena-slot
 * result as get_ft_specv_alloc; used by the ft_eager / ft_eager_on_spec
 * engines (the EAGER-attr vs SPECULATIVE-attr trie is selected by the
 * new_ft_* builder, independent of this lookup).
 */
static isc_result_t
get_ft_eager_alloc(void *ft, size_t count, void **pval) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	struct cds_ft_node *found;
	cds_ft_eager_lookup_key(ft, qk->bytes, qk->len, 32, &found);
	if (!found)
		return ISC_R_NOTFOUND;
	*pval = caa_container_of(found, struct ft_arena_slot, node);
	return ISC_R_SUCCESS;
}


static isc_result_t
add_ft(void *ft, size_t count) {
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	memcpy(item[count].ft_key, key, len);
	item[count].ft_key_len = len;
	struct cds_ft_node *result;
	enum cds_ft_status s = cds_ft_insert_unique(
		ft, key, len, &item[count].ft_node, &result);
	if (s == CDS_FT_STATUS_OK || s == CDS_FT_STATUS_DUPLICATE_FOUND)
		return ISC_R_SUCCESS;
	return ISC_R_FAILURE;
}

/*
 * Candidate lookup: skip-compressed descent without library-side leaf
 * validation, and the caller also skips key validation.  The returned
 * pointer is trusted; if the lookup key was not actually in the trie
 * the returned candidate is a phantom.  Useful only as a "what's the
 * raw descent cost" lower bound — not safe for real workloads.
 *
 * External slot is calloc'd (no FT super-arena), matching the
 * *_local engines' allocator placement.
 */
static isc_result_t
get_ft_cand(void *ft, size_t count, void **pval) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	struct cds_ft_node *found;
	cds_ft_lookup_candidate_key(ft, qk->bytes, qk->len, 32, &found);
	if (!found)
		return ISC_R_NOTFOUND;
	*pval = caa_container_of(found, struct ft_arena_slot, node);
	return ISC_R_SUCCESS;
}

static isc_result_t
get_ft_precise(void *ft, size_t count, void **pval) {
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	struct cds_ft_node *found;
	cds_ft_eager_lookup_key(ft, key, len, 32, &found);
	if (!found)
		return ISC_R_NOTFOUND;
	*pval = caa_container_of(found, struct item_s, ft_node);
	return ISC_R_SUCCESS;
}

typedef isc_result_t (*ft_get_fn)(void *, size_t, void **);

static void *
_thread_ft(void *arg0, ft_get_fn get) {
	struct thread_s *arg = arg0;

	pin_thread_to_cpu(arg->cpu);
	isc_barrier_wait(&barrier);

	pthread_mutex_lock(&ft_writer_mutex);
	isc_time_t t0 = isc_time_now_hires();
	for (size_t n = arg->start; n < arg->end; n++) {
		isc_result_t result = add_ft(arg->map, n);
		CHECKN(n, result);
	}
	isc_time_t t1 = isc_time_now_hires();
	pthread_mutex_unlock(&ft_writer_mutex);

	isc_barrier_wait(&barrier);
	flush_thread_caches();
	isc_barrier_wait(&barrier);
	isc_time_t t1b = isc_time_now_hires();

	rcu_read_lock();
	for (size_t loop = 0; loop < g_query_loops; loop++) {
		for (size_t n = arg->start; n < arg->end; n++) {
			void *pval = NULL;
			isc_result_t result = get(arg->map, n, &pval);
			CHECKN(n, result);
			assert(pval == &item[n]);
		}
	}
	rcu_read_unlock();
	isc_time_t t2 = isc_time_now_hires();

	arg->d0 = isc_time_microdiff(&t1, &t0);
	arg->d1 = isc_time_microdiff(&t2, &t1b);

	return NULL;
}

static void *
thread_ft_precise(void *arg0) {
	return _thread_ft(arg0, get_ft_precise);
}

static void *
thread_ft_specv(void *arg0) {
	return _thread_ft(arg0, get_ft_precise);
}

/*
 * cds_ft_alloc_external engine: insert allocates a fresh slot from FT's
 * super-arena and indexes it; lookup goes through the index table to
 * read the slot.
 */
typedef isc_result_t (*ft_add_fn)(void *, size_t);

/* Total number of keys (set in main after parsing); used by ft_churn. */
static size_t g_total_keys;

/*
 * Bench-only debug export from the FT lib (no public header decl); present
 * only in DEBUG_COUNTERS lib builds.  Weak-referenced so this bench links
 * against the optimized (non-DEBUG_COUNTERS) lib for the lookup/compact sweeps;
 * the FT_BENCH_DELETE arena report degrades to a notice when it is absent.
 */
extern void cds_ft_debug_arena_resident(const struct cds_ft *ft,
		size_t *live_ranges, size_t *reclaimed_ranges,
		size_t *internal_items, size_t *compressed_items,
		size_t *range_bytes) __attribute__((weak));

/*
 * FT_BENCH_CHURN=<rounds>: fragment the internal-node arena by removing and
 * reinserting a random ~50% of the keys, @rounds times, while holding the key
 * SET (and thus the query workload and its access order) constant.  This
 * isolates layout scatter from the query-order effect: only the in-memory
 * placement of internal nodes changes, not which keys exist or the order they
 * are queried.  Exercises the real remove/insert paths, so the fragmentation
 * is what a churning workload actually produces.  A grace period between the
 * remove batch and the reinsert batch lets the freed internal nodes return to
 * the freelist, so the reinserts allocate from the holes (= scatter) and the
 * removed leaf nodes are safe to reuse.  Operates on the ft_spec_extarena
 * slot table (ft_alloc_index[]); run it only with that engine.
 */
static void
ft_churn(struct cds_ft *ft, unsigned int rounds) {
	size_t n = g_total_keys;
	unsigned char *sel = malloc(n);
	struct cds_ft_iter *iter = NULL;
	uint64_t rng = 0x9e3779b97f4a7c15ULL;

	if (sel == NULL || cds_ft_iter_create(ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (unsigned int r = 0; r < rounds; r++) {
		/* remove pass: drop a random ~50% subset */
		for (size_t i = 0; i < n; i++) {
			struct ft_arena_slot *slot;

			rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
			sel[i] = (unsigned char)(rng & 1);
			if (!sel[i])
				continue;
			slot = ft_alloc_index[i];
			rcu_read_lock();
			cds_ft_iter_set_key(iter, slot->key, slot->key_len);
			if (cds_ft_lookup(ft, iter) == CDS_FT_STATUS_OK)
				(void) cds_ft_remove(ft, iter, &slot->node);
			rcu_read_unlock();
		}
		/* grace period: freed internal nodes return to the freelist */
		rcu_barrier();
		/* reinsert pass: allocate fresh nodes from the holes */
		for (size_t i = 0; i < n; i++) {
			struct ft_arena_slot *slot;
			struct cds_ft_node *result;

			if (!sel[i])
				continue;
			slot = ft_alloc_index[i];
			memset(&slot->node, 0, sizeof(slot->node));
			(void) cds_ft_insert_unique(ft, slot->key,
					slot->key_len, &slot->node, &result);
		}
	}
	cds_ft_iter_destroy(iter);
	free(sel);
	fprintf(stderr, "FT churn done (%u rounds, ~50%% per round)\n", rounds);
}

/* Resident set size (kB) from /proc/self/status. */
static long
get_rss_kb(void) {
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long rss = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL)
		if (strncmp(line, "VmRSS:", 6) == 0) {
			sscanf(line + 6, " %ld", &rss);
			break;
		}
	fclose(f);
	return rss;
}

/*
 * FT_BENCH_DELETE=<pct>: remove a random @pct% of the keys WITHOUT reinserting
 * (net deletion).  Random removal leaves most internal-node ranges holding a
 * few scattered survivors, so they cannot self-reclaim; a following
 * cds_ft_compact consolidates the survivors and lets the now-empty ranges
 * release their pages.  Measures the compactor's RSS-reclaim value, which the
 * remove+reinsert churn (which refills holes) deliberately does not exercise.
 * The deleted keys' leaf slots are app-owned and stay allocated; only FT
 * internal-node arenas shrink.
 */
static void
ft_bulk_delete(struct cds_ft *ft, unsigned int pct) {
	size_t n = g_total_keys, removed = 0;
	struct cds_ft_iter *iter = NULL;
	uint64_t rng = 0x123456789abcdefULL;

	if (cds_ft_iter_create(ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (size_t i = 0; i < n; i++) {
		struct ft_arena_slot *slot;

		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		if ((rng % 100) >= pct)
			continue;
		slot = ft_alloc_index[i];
		rcu_read_lock();
		cds_ft_iter_set_key(iter, slot->key, slot->key_len);
		if (cds_ft_lookup(ft, iter) == CDS_FT_STATUS_OK &&
		    cds_ft_remove(ft, iter, &slot->node) == CDS_FT_STATUS_OK)
			removed++;
		rcu_read_unlock();
	}
	cds_ft_iter_destroy(iter);
	fprintf(stderr, "FT bulk-delete done (%zu of %zu removed, %u%%)\n",
		removed, n, pct);
}

static void *
_thread_ft_alloc(void *arg0, ft_add_fn add, ft_get_fn get, bool force_read) {
	struct thread_s *arg = arg0;

	pin_thread_to_cpu(arg->cpu);
	{
		size_t bytes = (arg->end - arg->start) *
			       sizeof(struct query_key);
		bytes = (bytes + 4095) & ~(size_t)4095;
		arg->local_keys = mmap(NULL, bytes,
				       PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS,
				       -1, 0);
		if (arg->local_keys == MAP_FAILED)
			abort();
		/* First-touch by this worker thread → pages on local node. */
		memset(arg->local_keys, 0, bytes);
	}
	tls_local_keys = arg->local_keys;
	tls_local_start = arg->start;

	isc_barrier_wait(&barrier);

	pthread_mutex_lock(&ft_writer_mutex);
	isc_time_t t0 = isc_time_now_hires();
	for (size_t n = arg->start; n < arg->end; n++) {
		isc_result_t result = add(arg->map, n);
		CHECKN(n, result);
	}
	isc_time_t t1 = isc_time_now_hires();
	pthread_mutex_unlock(&ft_writer_mutex);

	isc_barrier_wait(&barrier);

	/*
	 * Drain the build's deferred (call_rcu) frees before the timed query.
	 * FT insert restructuring (node splits / compression) frees the old
	 * internal nodes via call_rcu; left in flight they would keep peak build
	 * residency mapped through the measurement and, worse, get caught by the
	 * query phase's single long read-side critical section -- under
	 * membarrier an open reader blocks the grace period, so the build frees
	 * could not reclaim until timing ends.  One global drain on thread 0
	 * settles the internal-node arena to its steady state first, mirroring
	 * qp's post-compact reclaim and the scale bench's ft_build, keeping the
	 * FT vs qp comparison fair.  The barrier below makes the other workers
	 * wait for it.
	 */
	if (arg->start == 0)
		rcu_barrier();

	/*
	 * Compaction experiment (BENCH_THREADS=1): thread 0 has built the
	 * whole trie under the writer mutex; quiescent at this barrier.
	 * FT_BENCH_COMPACT runs a full in-place compaction (cds_ft_compact)
	 * before the timed query phase, then verifies.
	 */
	if (arg->start == 0) {
		const char *churn = getenv("FT_BENCH_CHURN");

		if (churn != NULL)
			ft_churn((struct cds_ft *) arg->map,
				(unsigned int) atoi(churn));
	}
	if (arg->start == 0 && getenv("FT_BENCH_DELETE") != NULL) {
		struct cds_ft *m = (struct cds_ft *) arg->map;
		size_t lr, rr, ii, ci, rb;

		/* Measure the FT internal+compressed node arena directly (live vs
		 * reclaimed ranges, split by node kind) -- process RSS is swamped
		 * by the constant 944MB item[] and leaf-slot arena. */
#define ARENA_REPORT(stage) do { \
		if (cds_ft_debug_arena_resident) { \
			cds_ft_debug_arena_resident(m, &lr, &rr, &ii, &ci, &rb); \
			fprintf(stderr, "ARENA %-13s live_ranges=%5zu reclaimed=%5zu " \
				"internal=%8zu compressed=%8zu resident=%5zu MB\n", \
				(stage), lr, rr, ii, ci, lr * rb / 1048576); \
		} else { \
			fprintf(stderr, "ARENA %-13s (no DEBUG_COUNTERS lib; arena introspection unavailable)\n", (stage)); \
		} \
	} while (0)
		rcu_barrier();
		ARENA_REPORT("build");
		ft_bulk_delete(m, (unsigned int) atoi(getenv("FT_BENCH_DELETE")));
		rcu_barrier();			/* drained ranges self-reclaim */
		ARENA_REPORT("after_delete");
		if (getenv("FT_BENCH_DELETE_STATS") != NULL) {
			if (cds_ft_verify(m, stderr) != CDS_FT_STATUS_OK)
				fprintf(stderr, "FT VERIFY FAILED after bulk-delete\n");
			else
				fprintf(stderr, "FT verify OK after bulk-delete\n");
			cds_ft_show_stats(m, stderr);	/* structural node count */
		}
		if (getenv("FT_BENCH_COMPACT") != NULL) {
			cds_ft_compact(m);
			rcu_barrier();		/* consolidated ranges reclaim */
			ARENA_REPORT("after_compact");
		}
#undef ARENA_REPORT
		fflush(stderr);
		if (getenv("FT_BENCH_DELETE_DESTROY") != NULL)
			cds_ft_destroy(m);	/* fires ft_final_checks leak report under DEBUG_COUNTERS */
		_Exit(0);	/* survivors-only query set is invalid; skip queries */
	}
	if (arg->start == 0 && getenv("FT_BENCH_COMPACT") != NULL) {
		cds_ft_compact((struct cds_ft *) arg->map);
		/*
		 * Flush deferred frees so the drained ranges reclaim before the
		 * timed query phase -- mirrors qp, whose dns_qp_compact reclaim
		 * completes before its own timing window (fair comparison).
		 */
		rcu_barrier();
		if (getenv("FT_RECOMPACT_NOVERIFY") == NULL &&
			cds_ft_verify((const struct cds_ft *) arg->map,
				stderr) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "FT VERIFY FAILED after compact\n");
			abort();
		}
		fprintf(stderr, "FT compact done\n");
	}

	flush_thread_caches();
	isc_barrier_wait(&barrier);
	/*
	 * Cache-priming phase: ON BY DEFAULT, set BENCH_NO_PRIME to skip it.
	 * Each querying thread runs one untimed lookup pass over its key range
	 * to warm its caches/TLB to the post-build/post-compact steady state
	 * BEFORE timing, so cold-start (or a preceding compaction's eviction)
	 * does not bias the measurement.  Applied identically to EVERY engine —
	 * FT here and qp in _thread_qp_arena() — so cross-engine comparisons are
	 * fair (otherwise FT would be timed warm and qp cold).
	 */
	if (getenv("BENCH_NO_PRIME") == NULL) {
		rcu_read_lock();
		for (size_t n = arg->start; n < arg->end; n++) {
			void *pval = NULL;
			(void) get(arg->map, n, &pval);
		}
		rcu_read_unlock();
		isc_barrier_wait(&barrier);
	}
	isc_time_t t1b = isc_time_now_hires();

	rcu_read_lock();
	for (size_t loop = 0; loop < g_query_loops; loop++) {
		for (size_t n = arg->start; n < arg->end; n++) {
			void *pval = NULL;
			isc_result_t result = get(arg->map, n, &pval);
			CHECKN(n, result);
			assert(pval == ft_alloc_index[n]);
			/*
			 * Force-read the leaf payload's first byte to
			 * make every engine pay the same post-lookup
			 * CL touch.  Skipped for candidate-mode engines
			 * by passing force_read=false (cand semantics:
			 * caller trusts the candidate, no leaf access).
			 */
			if (force_read) {
				asm volatile("" :: "r"(*(const volatile uint8_t *)pval) : "memory");
			}
		}
	}
	rcu_read_unlock();
	isc_time_t t2 = isc_time_now_hires();

	arg->d0 = isc_time_microdiff(&t1, &t0);
	arg->d1 = isc_time_microdiff(&t2, &t1b);

	thread_scratch_release(arg);
	return NULL;
}

/*
 * Thread wrappers compose an allocator (add_ft_specv_{il,local,extarena} =
 * leaf-slot placement) with a lookup (get_ft_*).  The trie's optimization
 * attr (EAGER vs SPECULATIVE) is selected by the paired new_ft_* builder in
 * fun_list, independent of the lookup — so thread_ft_spec_il backs both
 * ft_spec_il (SPEC attr) and ft_spec_on_eager_il (EAGER attr), and
 * thread_ft_eager_il backs both ft_eager_il and ft_eager_on_spec_il.
 */

/* Speculative lookup (library-side memcmp via key offset). */
static void *
thread_ft_spec_il(void *arg0) {
	return _thread_ft_alloc(arg0, add_ft_specv_il, get_ft_specv_alloc, true);
}

static void *
thread_ft_spec_local(void *arg0) {
	return _thread_ft_alloc(arg0, add_ft_specv_local, get_ft_specv_alloc, true);
}

static void *
thread_ft_spec_extarena(void *arg0) {
	return _thread_ft_alloc(arg0, add_ft_specv_extarena, get_ft_specv_alloc, true);
}

/* Eager lookup (exact in-trie descent). */
static void *
thread_ft_eager_il(void *arg0) {
	return _thread_ft_alloc(arg0, add_ft_specv_il, get_ft_eager_alloc, true);
}

static void *
thread_ft_eager_local(void *arg0) {
	return _thread_ft_alloc(arg0, add_ft_specv_local, get_ft_eager_alloc, true);
}

/* Pure candidate lookup (no caller-side memcmp). */
static void *
thread_ft_cand_il(void *arg0) {
	return _thread_ft_alloc(arg0, add_ft_specv_il, get_ft_cand, false);
}

static void *
thread_ft_cand_local(void *arg0) {
	return _thread_ft_alloc(arg0, add_ft_specv_local, get_ft_cand, false);
}

/*
 * QP engines paired by NUMA policy.  Both:
 *   - read input key from tls_local_keys[count] (per-thread copy,
 *     local NUMA placement),
 *   - have qpmethods.makekey read leaf-key bytes from a per-count
 *     arena slot,
 *   - output pval = user_payload_at(count) (force-read post-lookup).
 * The only difference is the NUMA policy of the leaf-key arena.
 */
static void
qp_attach_noop(void *ctx, void *pval, uint32_t ival) {
	UNUSED(ctx);
	UNUSED(pval);
	UNUSED(ival);
}

static size_t
il_qp_makekey(dns_qpkey_t key, void *ctx, void *pval, uint32_t ival) {
	UNUSED(ctx);
	UNUSED(pval);
	struct ft_arena_slot *slot = ft_il_arena_at(ival);
	memcpy(key, slot->key, slot->key_len);
	return slot->key_len;
}

static size_t
local_qp_makekey(dns_qpkey_t key, void *ctx, void *pval, uint32_t ival) {
	UNUSED(ctx);
	UNUSED(pval);
	struct ft_arena_slot *slot = ft_local_arena_at(ival);
	memcpy(key, slot->key, slot->key_len);
	return slot->key_len;
}

static const dns_qpmethods_t il_qpmethods = {
	qp_attach_noop, qp_attach_noop, il_qp_makekey, testname,
};
static const dns_qpmethods_t local_qpmethods = {
	qp_attach_noop, qp_attach_noop, local_qp_makekey, testname,
};

static void *
new_qp_il(isc_mem_t *mem ISC_ATTR_UNUSED) {
	dns_qpmulti_t *qpm = NULL;
	dns_qpmulti_create(isc_g_mctx, &il_qpmethods, NULL, &qpm);
	return qpm;
}

static void *
new_qp_local(isc_mem_t *mem ISC_ATTR_UNUSED) {
	dns_qpmulti_t *qpm = NULL;
	dns_qpmulti_create(isc_g_mctx, &local_qpmethods, NULL, &qpm);
	return qpm;
}

static void
destroy_qp(void *map) {
	dns_qpmulti_t *qpm = map;

	/* attach/detach are no-ops, so bench-owned item[] is untouched. */
	dns_qpmulti_destroy(&qpm);
}

static isc_result_t
add_qp_il(void *qp, size_t count) {
	struct ft_arena_slot *slot = ft_il_arena_at(count);
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	memcpy(slot->key, key, len);
	slot->key_len = len;
	memcpy(qk->bytes, key, len);
	qk->len = len;
	return dns_qp_insert(qp, user_payload_at(count), count);
}

static isc_result_t
add_qp_local(void *qp, size_t count) {
	struct ft_arena_slot *slot = ft_local_arena_at(count);
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	memcpy(slot->key, key, len);
	slot->key_len = len;
	memcpy(qk->bytes, key, len);
	qk->len = len;
	return dns_qp_insert(qp, user_payload_at(count), count);
}

static isc_result_t
get_qp_arena(void *qpr, size_t count, void **pval) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	uint32_t ival = 0;
	isc_result_t r = dns_qp_getkey(qpr, qk->bytes, qk->len,
				       pval, &ival);
	(void)ival;
	return r;
}

typedef isc_result_t (*qp_add_fn)(void *, size_t);

static void *
_thread_qp_arena(void *arg0, qp_add_fn add) {
	struct thread_s *arg = arg0;

	pin_thread_to_cpu(arg->cpu);
	{
		size_t bytes = (arg->end - arg->start) *
			       sizeof(struct query_key);
		bytes = (bytes + 4095) & ~(size_t)4095;
		arg->local_keys = mmap(NULL, bytes,
				       PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS,
				       -1, 0);
		if (arg->local_keys == MAP_FAILED)
			abort();
		/* First-touch by this worker thread → pages on local node. */
		memset(arg->local_keys, 0, bytes);
	}
	tls_local_keys = arg->local_keys;
	tls_local_start = arg->start;

	isc_barrier_wait(&barrier);

	dns_qp_t *qp = NULL;
	dns_qpmulti_write(arg->map, &qp);

	isc_time_t t0 = isc_time_now_hires();
	for (size_t n = arg->start; n < arg->end; n++) {
		isc_result_t result = add(qp, n);
		CHECKN(n, result);
	}
	dns_qp_compact(qp, DNS_QPGC_MAYBE);
	dns_qpmulti_commit(arg->map, &qp);

	isc_time_t t1 = isc_time_now_hires();

	isc_barrier_wait(&barrier);
	flush_thread_caches();
	isc_barrier_wait(&barrier);
	/*
	 * Cache-priming phase, identical to the FT path (_thread_ft_alloc): on
	 * by default, BENCH_NO_PRIME to skip.  One untimed lookup pass over this
	 * thread's key range, before t1b, so the timed window measures the
	 * warmed steady state — keeping FT vs qp comparisons fair.
	 */
	if (getenv("BENCH_NO_PRIME") == NULL) {
		dns_qpread_t pqpr;
		dns_qpmulti_query(arg->map, &pqpr);
		for (size_t n = arg->start; n < arg->end; n++) {
			void *pval = NULL;
			(void) get_qp_arena(&pqpr, n, &pval);
		}
		dns_qpread_destroy(arg->map, &pqpr);
		isc_barrier_wait(&barrier);
	}
	isc_time_t t1b = isc_time_now_hires();

	dns_qpread_t qpr;
	dns_qpmulti_query(arg->map, &qpr);

	for (size_t loop = 0; loop < g_query_loops; loop++) {
		for (size_t n = arg->start; n < arg->end; n++) {
			void *pval = NULL;
			isc_result_t result = get_qp_arena(&qpr, n, &pval);
			CHECKN(n, result);
			assert(pval == user_payload_at(n));
			asm volatile("" :: "r"(*(const volatile uint8_t *)pval) : "memory");
		}
	}

	dns_qpread_destroy(arg->map, &qpr);

	isc_time_t t2 = isc_time_now_hires();

	arg->d0 = isc_time_microdiff(&t1, &t0);
	arg->d1 = isc_time_microdiff(&t2, &t1b);

	thread_scratch_release(arg);
	return NULL;
}

static void *
thread_qp_il(void *arg0) {
	return _thread_qp_arena(arg0, add_qp_il);
}

static void *
thread_qp_local(void *arg0) {
	return _thread_qp_arena(arg0, add_qp_local);
}

/*
 * All engines below use the same input pattern (tls_local_keys per-
 * thread copy) and the same post-lookup force-read on the leaf
 * (except candidate-mode engines, which by definition trust the
 * candidate without reading it).  The only variable across paired
 * engines is the NUMA placement policy of the leaf-key arena:
 *
 *   _il    — mmap + MPOL_INTERLEAVE across all NUMA nodes.
 *   _local — mmap with default first-touch policy → local NUMA node.
 */
/* ───────────────────────────────────────────────────────────────────
 * HOTRowex engine — HOT's concurrent ROWEX trie, via the extern "C" shim
 * src/load_names_hotrowex.cpp (compiled by g++, linked with -ltbb -lstdc++).
 *
 * Keys on a NUL-terminated copy of the qpkey: every qpkey byte is
 * >= SHIFT_NOBYTE (== 2), so the bytes never contain 0x00 and a trailing 0
 * terminates them cleanly (the query buffer is already 0-terminated by its
 * memset).  The copies live in a NUMA-interleaved bump arena, so HOTRowex's
 * value points at cold, separate memory: its contentEquals validation and the
 * benchmark's force-read pay the same leaf-touch the FT _il engines do, rather
 * than re-reading the (NUMA-local, already-hot) query buffer.
 *
 * Reuses _thread_ft_alloc: HOT inserts run under ft_writer_mutex (serialized,
 * so the bump arena needs no locking), reads are concurrent and self-guarded by
 * HOT's epoch.  add_hotrowex stashes the copy pointer in ft_alloc_index[] so
 * the shared loop's correctness assert (pval == ft_alloc_index[n]) still holds.
 * ─────────────────────────────────────────────────────────────────── */
void *hotrowex_create(void);
void hotrowex_destroy(void *h);
int hotrowex_insert(void *h, const char *key);
const char *hotrowex_lookup(void *h, const char *key);

static char *hot_il_arena = NULL;
static size_t hot_il_arena_off = 0;
static size_t hot_il_arena_cap = 0;

static void
hot_il_arena_create(size_t n) {
	/* One max-size qpkey + NUL per key; lazily faulted, so only the bytes
	 * actually written become resident. */
	hot_il_arena_cap = n * (sizeof(dns_qpkey_t) + 1);
	hot_il_arena = mmap(NULL, hot_il_arena_cap, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (hot_il_arena == MAP_FAILED) {
		perror("mmap(hot_il_arena)");
		abort();
	}
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(hot_il_arena, hot_il_arena_cap,
		      MPOL_INTERLEAVE, &mask, 25, 0) < 0) {
		perror("mbind(hot_il_arena)");
	}
	hot_il_arena_off = 0;
}

static char *
hot_il_arena_alloc(size_t sz) {
	size_t off = (hot_il_arena_off + 7) & ~(size_t)7;
	if (off + sz > hot_il_arena_cap)
		abort();
	hot_il_arena_off = off + sz;
	return hot_il_arena + off;
}

static isc_result_t
add_hotrowex(void *map, size_t count) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	char *copy = hot_il_arena_alloc(len + 1);
	memcpy(copy, key, len);
	copy[len] = '\0';
	memcpy(qk->bytes, key, len);
	qk->len = len;
	ft_alloc_index[count] = (struct ft_arena_slot *)copy;
	(void) hotrowex_insert(map, copy);	/* dup == already present == OK */
	return ISC_R_SUCCESS;
}

static isc_result_t
get_hotrowex(void *map, size_t count, void **pval) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	const char *v = hotrowex_lookup(map, (const char *)qk->bytes);
	if (v == NULL)
		return ISC_R_NOTFOUND;
	*pval = (void *)v;
	return ISC_R_SUCCESS;
}

static void *
new_hotrowex(isc_mem_t *mem ISC_ATTR_UNUSED) {
	return hotrowex_create();
}

static void
destroy_hotrowex(void *map) {
	hotrowex_destroy(map);
}

static void *
thread_hotrowex(void *arg0) {
	return _thread_ft_alloc(arg0, add_hotrowex, get_hotrowex, true);
}

/* ───────────────────────────────────────────────────────────────────
 * Masstree engine — concurrent B+tree-of-tries (kohler/masstree-beta, MIT),
 * via the extern "C" shim src/load_names_masstree.cpp (compiled by g++ with
 * the vendored Masstree sources, linked with -lstdc++).
 *
 * Masstree keys on arbitrary binary lcdf::Str, so it takes the dns_qpkey bytes
 * directly.  Read-only after build, so no concurrent removes / no epoch GC to
 * drive at query time; each worker just makes its own threadinfo lazily (there
 * is no teardown hook in _thread_ft_alloc).  The stored value is a copy of the
 * qpkey in a NUMA-interleaved arena, so the benchmark's force-read hits cold,
 * separate memory (Masstree validates the full key during descent).
 *
 * Run one thread count per process (BENCH_THREADS=N): Masstree's build-time
 * split garbage / per-point table is only best-effort reclaimed, so a single
 * build per process keeps any residual leak bounded.
 * ─────────────────────────────────────────────────────────────────── */
void *masstree_create(void);
void *masstree_thread_init(void);
void masstree_insert(void *table, void *ti, const char *key, int len, void *val);
void *masstree_lookup(void *table, void *ti, const char *key, int len);
void masstree_destroy(void *table);

/* Per-worker threadinfo, created lazily (no teardown hook in _thread_ft_alloc). */
static __thread void *tls_mt_ti = NULL;
static inline void *mt_ti(void) {
	if (tls_mt_ti == NULL)
		tls_mt_ti = masstree_thread_init();
	return tls_mt_ti;
}

static char *mt_il_arena = NULL;
static size_t mt_il_arena_off = 0;
static size_t mt_il_arena_cap = 0;

static void
mt_il_arena_create(size_t n) {
	mt_il_arena_cap = n * sizeof(dns_qpkey_t);
	mt_il_arena = mmap(NULL, mt_il_arena_cap, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mt_il_arena == MAP_FAILED) {
		perror("mmap(mt_il_arena)");
		abort();
	}
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(mt_il_arena, mt_il_arena_cap,
		      MPOL_INTERLEAVE, &mask, 25, 0) < 0) {
		perror("mbind(mt_il_arena)");
	}
	mt_il_arena_off = 0;
}

static char *
mt_il_arena_alloc(size_t sz) {
	size_t off = (mt_il_arena_off + 7) & ~(size_t)7;
	if (off + sz > mt_il_arena_cap)
		abort();
	mt_il_arena_off = off + sz;
	return mt_il_arena + off;
}

static isc_result_t
add_masstree(void *map, size_t count) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	char *copy = mt_il_arena_alloc(len);
	memcpy(copy, key, len);
	memcpy(qk->bytes, key, len);
	qk->len = len;
	ft_alloc_index[count] = (struct ft_arena_slot *)copy;
	masstree_insert(map, mt_ti(), (const char *)key, (int)len, copy);
	return ISC_R_SUCCESS;
}

static isc_result_t
get_masstree(void *map, size_t count, void **pval) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	void *v = masstree_lookup(map, mt_ti(), (const char *)qk->bytes,
				  (int)qk->len);
	if (v == NULL)
		return ISC_R_NOTFOUND;
	*pval = v;
	return ISC_R_SUCCESS;
}

static void *
new_masstree(isc_mem_t *mem ISC_ATTR_UNUSED) {
	return masstree_create();
}

static void
destroy_masstree(void *map) {
	masstree_destroy(map);
}

static void *
thread_masstree(void *arg0) {
	return _thread_ft_alloc(arg0, add_masstree, get_masstree, true);
}

/* ───────────────────────────────────────────────────────────────────
 * ART-OLC engine — concurrent ART, Optimistic Lock Coupling
 * (flode/ARTSynchronized, Apache-2.0), via the extern "C" shim
 * src/load_names_artolc.cpp (g++ + the vendored ART-OLC unity source, linked
 * with -ltbb -lstdc++).
 *
 * ART stores only a TID per leaf and validates via a loadKey(TID) callback, so
 * the value is a pointer to an artolc_kv copy of the qpkey in a NUMA-interleaved
 * arena -- loadKey (validation) and the force-read hit cold memory.  Read-only
 * after build: ops open the epoch guard internally, so each worker just makes
 * its own ART::ThreadInfo lazily.  Run one thread count per process.
 * ─────────────────────────────────────────────────────────────────── */
void *artolc_create(void);
void *artolc_thread_init(void *tree);
void artolc_insert(void *tree, void *ti, const char *key, int len, uint64_t tid_val);
uint64_t artolc_lookup(void *tree, void *ti, const char *key, int len);
void artolc_destroy(void *tree);

struct artolc_kv {		/* must match src/load_names_artolc.cpp */
	uint32_t len;
	uint8_t bytes[];
};

static __thread void *tls_artolc_ti = NULL;
static inline void *artolc_ti(void *map) {
	if (tls_artolc_ti == NULL)
		tls_artolc_ti = artolc_thread_init(map);
	return tls_artolc_ti;
}

static char *artolc_il_arena = NULL;
static size_t artolc_il_arena_off = 0;
static size_t artolc_il_arena_cap = 0;

static void
artolc_il_arena_create(size_t n) {
	artolc_il_arena_cap = n * (sizeof(struct artolc_kv) + sizeof(dns_qpkey_t));
	artolc_il_arena = mmap(NULL, artolc_il_arena_cap, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (artolc_il_arena == MAP_FAILED) {
		perror("mmap(artolc_il_arena)");
		abort();
	}
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(artolc_il_arena, artolc_il_arena_cap,
		      MPOL_INTERLEAVE, &mask, 25, 0) < 0) {
		perror("mbind(artolc_il_arena)");
	}
	artolc_il_arena_off = 0;
}

static char *
artolc_il_arena_alloc(size_t sz) {
	size_t off = (artolc_il_arena_off + 7) & ~(size_t)7;
	if (off + sz > artolc_il_arena_cap)
		abort();
	artolc_il_arena_off = off + sz;
	return artolc_il_arena + off;
}

static isc_result_t
add_artolc(void *map, size_t count) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	/*
	 * ART is a byte radix tree, so keys must be byte-prefix-free -- but a
	 * parent domain's qpkey is a byte-prefix of its child's.  Every qpkey
	 * byte is >= SHIFT_NOBYTE (2), so append a 0 terminator (unique) and key
	 * on len+1.  (len+1 <= sizeof(dns_qpkey_t), so qk->bytes has room.)
	 */
	memcpy(qk->bytes, key, len);
	qk->bytes[len] = 0;
	qk->len = len + 1;
	struct artolc_kv *kv = (struct artolc_kv *)artolc_il_arena_alloc(
		sizeof(struct artolc_kv) + len + 1);
	kv->len = (uint32_t)(len + 1);
	memcpy(kv->bytes, qk->bytes, len + 1);
	ft_alloc_index[count] = (struct ft_arena_slot *)kv;
	artolc_insert(map, artolc_ti(map), (const char *)qk->bytes,
		      (int)(len + 1), (uint64_t)(uintptr_t)kv);
	return ISC_R_SUCCESS;
}

static isc_result_t
get_artolc(void *map, size_t count, void **pval) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	uint64_t tid = artolc_lookup(map, artolc_ti(map),
				     (const char *)qk->bytes, (int)qk->len);
	if (tid == 0)
		return ISC_R_NOTFOUND;
	*pval = (void *)(uintptr_t)tid;
	return ISC_R_SUCCESS;
}

static void *
new_artolc(isc_mem_t *mem ISC_ATTR_UNUSED) {
	return artolc_create();
}

static void
destroy_artolc(void *map) {
	artolc_destroy(map);
}

static void *
thread_artolc(void *arg0) {
	return _thread_ft_alloc(arg0, add_artolc, get_artolc, true);
}

/* ───────────────────────────────────────────────────────────────────
 * ART-ROWEX engine — concurrent ART, Read-Optimized Write EXclusion (same
 * flode/ARTSynchronized repo as ART-OLC, Apache-2.0), via the extern "C" shim
 * src/load_names_artrowex.cpp.  Wiring is identical to ART-OLC -- same
 * TID-is-a-key-copy validation, same NUMA-interleaved arena, same prefix-free
 * len+1 keys; only the lookup-side concurrency discipline differs (ROWEX
 * readers never restart).  Its own arena (the engine sweep can run both ART
 * variants in one process, so they must not share a bump allocator).  The kv
 * layout is identical to artolc_kv, so it is reused.
 * ─────────────────────────────────────────────────────────────────── */
void *artrowex_create(void);
void *artrowex_thread_init(void *tree);
void artrowex_insert(void *tree, void *ti, const char *key, int len, uint64_t tid_val);
uint64_t artrowex_lookup(void *tree, void *ti, const char *key, int len);
void artrowex_destroy(void *tree);

static __thread void *tls_artrowex_ti = NULL;
static inline void *artrowex_ti(void *map) {
	if (tls_artrowex_ti == NULL)
		tls_artrowex_ti = artrowex_thread_init(map);
	return tls_artrowex_ti;
}

static char *artrowex_il_arena = NULL;
static size_t artrowex_il_arena_off = 0;
static size_t artrowex_il_arena_cap = 0;

static void
artrowex_il_arena_create(size_t n) {
	artrowex_il_arena_cap = n * (sizeof(struct artolc_kv) + sizeof(dns_qpkey_t));
	artrowex_il_arena = mmap(NULL, artrowex_il_arena_cap, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (artrowex_il_arena == MAP_FAILED) {
		perror("mmap(artrowex_il_arena)");
		abort();
	}
	unsigned long mask = (1UL << 24) - 1;
	if (sys_mbind(artrowex_il_arena, artrowex_il_arena_cap,
		      MPOL_INTERLEAVE, &mask, 25, 0) < 0) {
		perror("mbind(artrowex_il_arena)");
	}
	artrowex_il_arena_off = 0;
}

static char *
artrowex_il_arena_alloc(size_t sz) {
	size_t off = (artrowex_il_arena_off + 7) & ~(size_t)7;
	if (off + sz > artrowex_il_arena_cap)
		abort();
	artrowex_il_arena_off = off + sz;
	return artrowex_il_arena + off;
}

static isc_result_t
add_artrowex(void *map, size_t count) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	dns_qpkey_t key;
	size_t len = dns_qpkey_fromname(key, &item[count].fixed.name,
					DNS_DBNAMESPACE_NORMAL);
	/* len+1 with a 0 terminator: byte-prefix-free keys for ART (see add_artolc). */
	memcpy(qk->bytes, key, len);
	qk->bytes[len] = 0;
	qk->len = len + 1;
	struct artolc_kv *kv = (struct artolc_kv *)artrowex_il_arena_alloc(
		sizeof(struct artolc_kv) + len + 1);
	kv->len = (uint32_t)(len + 1);
	memcpy(kv->bytes, qk->bytes, len + 1);
	ft_alloc_index[count] = (struct ft_arena_slot *)kv;
	artrowex_insert(map, artrowex_ti(map), (const char *)qk->bytes,
			(int)(len + 1), (uint64_t)(uintptr_t)kv);
	return ISC_R_SUCCESS;
}

static isc_result_t
get_artrowex(void *map, size_t count, void **pval) {
	struct query_key *qk = &tls_local_keys[count - tls_local_start];
	uint64_t tid = artrowex_lookup(map, artrowex_ti(map),
				       (const char *)qk->bytes, (int)qk->len);
	if (tid == 0)
		return ISC_R_NOTFOUND;
	*pval = (void *)(uintptr_t)tid;
	return ISC_R_SUCCESS;
}

static void *
new_artrowex(isc_mem_t *mem ISC_ATTR_UNUSED) {
	return artrowex_create();
}

static void
destroy_artrowex(void *map) {
	artrowex_destroy(map);
}

static void *
thread_artrowex(void *arg0) {
	return _thread_ft_alloc(arg0, add_artrowex, get_artrowex, true);
}

static struct fun fun_list[] = {
	{ "qp_il",                new_qp_il,              thread_qp_il,              destroy_qp },
	{ "qp_local",             new_qp_local,           thread_qp_local,           destroy_qp },
	/*
	 * FT engines: 2x2 of build attr (EAGER/SPEC) x lookup (eager/spec),
	 * named with the matched diagonal as the base and the off-diagonal
	 * suffixed _on_<attr>; plus pure candidate.  Suffix _il/_local/_extarena
	 * is leaf-slot allocator placement.
	 *   builder  = attr:   new_ft_eager_* (EAGER)  / new_ft_spec_* (SPEC)
	 *   thread   = lookup: thread_ft_eager_* (eager) / thread_ft_spec_*
	 *              (speculative) / thread_ft_cand_* (candidate)
	 */
	{ "ft_eager_il",           new_ft_eager_il,      thread_ft_eager_il,    destroy_ft },
	{ "ft_eager_local",        new_ft_eager_local,   thread_ft_eager_local, destroy_ft },
	{ "ft_eager_on_spec_il",   new_ft_spec_il,       thread_ft_eager_il,    destroy_ft },
	{ "ft_eager_on_spec_local", new_ft_spec_local,   thread_ft_eager_local, destroy_ft },
	{ "ft_spec_il",            new_ft_spec_il,       thread_ft_spec_il,     destroy_ft },
	{ "ft_spec_local",         new_ft_spec_local,    thread_ft_spec_local,  destroy_ft },
	{ "ft_spec_extarena",      new_ft_spec_extarena, thread_ft_spec_extarena, destroy_ft_extarena },
	{ "ft_spec_on_eager_il",   new_ft_eager_il,      thread_ft_spec_il,     destroy_ft },
	{ "ft_spec_on_eager_local", new_ft_eager_local,  thread_ft_spec_local,  destroy_ft },
	{ "ft_cand_il",            new_ft_cand,          thread_ft_cand_il,     destroy_ft },
	{ "ft_cand_local",         new_ft_cand,          thread_ft_cand_local,  destroy_ft },
	/* HOT concurrent ROWEX trie (NUMA-interleaved key-copy arena). */
	{ "hotrowex",              new_hotrowex,         thread_hotrowex,       destroy_hotrowex },
	/* Masstree concurrent B+tree-of-tries. */
	{ "masstree",              new_masstree,         thread_masstree,       destroy_masstree },
	/* ART-OLC concurrent adaptive radix tree (Optimistic Lock Coupling). */
	{ "artolc",                new_artolc,           thread_artolc,         destroy_artolc },
	/* ART-ROWEX concurrent adaptive radix tree (Read-Optimized Write EXclusion). */
	{ "artrowex",              new_artrowex,         thread_artrowex,       destroy_artrowex },
	{ NULL, NULL, NULL, NULL },
};

#define FILE_CHECK(check, msg)                                                 \
	do {                                                                   \
		if (!(check)) {                                                \
			fprintf(stderr, "%s:%zu: %s\n", filename, lines, msg); \
			exit(EXIT_FAILURE);                                    \
		}                                                              \
	} while (0)

static void
snapshot_on_abort(int sig __attribute__((unused))) {
	const char *sess = getenv("LTTNG_SNAPSHOT_SESSION");
	if (sess && sess[0]) {
		char cmd[256];
		snprintf(cmd, sizeof(cmd),
			"lttng snapshot record -s %s >&2", sess);
		int r = system(cmd);
		(void) r;
		fflush(stderr);
	}
	/* Re-raise so default disposition (core dump) runs. */
	signal(sig, SIG_DFL);
	raise(sig);
}

int
main(int argc, char *argv[]) {
	isc_result_t result;
	const char *filename = NULL;
	char *filetext = NULL;
	off_t fileoff;
	FILE *fp = NULL;
	size_t filesize, lines = 0, wirebytes = 0, labels = 0;
	char *pos = NULL, *file_end = NULL;

	setlinebuf(stdout);
	dump_item_layout();

	/*
	 * Clear any inherited PR_SET_THP_DISABLE so MADV_HUGEPAGE on the FT
	 * arenas can actually form transparent hugepages.  Some execution
	 * sandboxes set this flag on the process tree, which silently turns
	 * MADV_HUGEPAGE into a no-op (THPeligible=0 on every mapping).
	 * Re-enabling here makes the hugepage measurement deterministic
	 * regardless of launcher.  Set BENCH_KEEP_THP_DISABLE=1 to skip
	 * (e.g. to reproduce a THP-disabled environment).
	 */
	if (!getenv("BENCH_KEEP_THP_DISABLE")) {
		int was = prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0);
		(void) prctl(PR_SET_THP_DISABLE, 0, 0, 0, 0);
		fprintf(stderr, "THP_DISABLE was=%d now=%d (system: see /sys/kernel/mm/transparent_hugepage/enabled)\n",
			was, prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0));
	}

	/* Take an LTTng snapshot before aborting on assert() failure. */
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = snapshot_on_abort;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESETHAND;
		sigaction(SIGABRT, &sa, NULL);
	}
	{
		const char *e = getenv("QUERY_LOOPS");
		if (e && *e) {
			char *endp = NULL;
			long v = strtol(e, &endp, 10);
			if (endp && *endp == '\0' && v > 0)
				g_query_loops = (size_t)v;
		}
		fprintf(stderr, "QUERY_LOOPS=%zu\n", g_query_loops);
	}
	{
		const char *e = getenv("BENCH_CACHE_FLUSH_MB");
		if (e && *e) {
			char *endp = NULL;
			long v = strtol(e, &endp, 10);
			if (endp && *endp == '\0' && v >= 0)
				g_cache_flush_bytes = (size_t)v * 1024 * 1024;
		}
		fprintf(stderr, "CACHE_FLUSH_MB=%zu\n",
			g_cache_flush_bytes / (1024 * 1024));
	}
	ft_il_arena_create(sizeof(item) / sizeof(item[0]));
	ft_local_arena_create(sizeof(item) / sizeof(item[0]));
	ft_alloc_index_create(sizeof(item) / sizeof(item[0]));
	query_keys_create(sizeof(item) / sizeof(item[0]));
	user_payload_create(sizeof(item) / sizeof(item[0]));
	hot_il_arena_create(sizeof(item) / sizeof(item[0]));
	mt_il_arena_create(sizeof(item) / sizeof(item[0]));
	artolc_il_arena_create(sizeof(item) / sizeof(item[0]));
	artrowex_il_arena_create(sizeof(item) / sizeof(item[0]));
	isc_rwlock_init(&rwl);

	if (argc != 2) {
		fprintf(stderr,
			"usage: load-names <filename.csv> <nthreads>\n");
		exit(EXIT_FAILURE);
	}

	filename = argv[1];
	result = isc_file_getsize(filename, &fileoff);
	if (result != ISC_R_SUCCESS) {
		fprintf(stderr, "stat(%s): %s\n", filename,
			isc_result_totext(result));
		exit(EXIT_FAILURE);
	}
	filesize = (size_t)fileoff;

	filetext = isc_mem_get(isc_g_mctx, filesize + 1);
	fp = fopen(filename, "r");
	if (fp == NULL || fread(filetext, 1, filesize, fp) < filesize) {
		fprintf(stderr, "read(%s): %s\n", filename, strerror(errno));
		exit(EXIT_FAILURE);
	}
	fclose(fp);
	filetext[filesize] = '\0';

	pos = filetext;
	file_end = pos + filesize;
	while (pos < file_end) {
		char *domain = NULL, *newline = NULL;
		size_t len;

		FILE_CHECK(lines < ARRAY_SIZE(item), "too many lines");
		pos += strspn(pos, "0123456789");

		FILE_CHECK(*pos++ == ',', "missing comma");

		domain = pos;
		pos += strcspn(pos, "\r\n");
		FILE_CHECK(*pos != '\0', "missing newline");
		newline = pos;
		pos += strspn(pos, "\r\n");
		len = newline - domain;

		item[lines].text = domain;
		domain[len] = '\0';

		dns_name_t *name = dns_fixedname_initname(&item[lines].fixed);
		isc_buffer_t buffer;
		isc_buffer_init(&buffer, domain, len);
		isc_buffer_add(&buffer, len);
		result = dns_name_fromtext(name, &buffer, dns_rootname, 0);
		FILE_CHECK(result == ISC_R_SUCCESS, isc_result_totext(result));

		wirebytes += name->length;
		labels += dns_name_countlabels(name);
		lines++;
	}

	printf("names %g MB labels %g MB\n\n", (double)wirebytes / 1048576.0,
	       (double)labels / 1048576.0);

	printf("%10s | %10s | %10s | %10s | %10s | %10s | %10s |\n",
	       "algorithm", "threads", "load Mops/s", "query Mops/s", "dirty MB",
	       "wall (s)", "final MB");

	static const size_t default_sweep[] = { 1, 2, 4, 8, 16, 32, 64, 96, 128, 192 };
	/*
	 * BENCH_THREADS=N restricts the sweep to a single thread count —
	 * used for clean perf-stat runs (combine with a high QUERY_LOOPS so
	 * the lookup phase dominates the counters).
	 */
	size_t single_sweep[1];
	const size_t *thread_sweep = default_sweep;
	size_t sweep_len = sizeof(default_sweep) / sizeof(default_sweep[0]);
	{
		const char *e = getenv("BENCH_THREADS");
		if (e && *e) {
			single_sweep[0] = (size_t) strtoul(e, NULL, 10);
			thread_sweep = single_sweep;
			sweep_len = 1;
		}
	}
	for (size_t si = 0; si < sweep_len; si++) {
		size_t nthreads = thread_sweep[si];
		printf("---------- | ---------- | ---------- | ---------- | "
		       "---------- | ---------- | ---------- |\n");

		const char *engine_filter = getenv("BENCH_ENGINE");
		for (struct fun *fun = fun_list; fun->name != NULL; fun++) {
			if (engine_filter && strcmp(engine_filter, fun->name) != 0)
				continue;
			void *map = NULL;

			/*
			 * Reset per-item embedded link nodes so they can
			 * be reinserted into a fresh data structure on
			 * each iteration.  Same for the shared FT external
			 * slot arenas — every FT engine reuses the same
			 * ft_il_arena / ft_local_arena slots across sweeps;
			 * cds_ft_insert_unique rejects non-zero prev/next
			 * links from a prior insert with EINVAL.
			 *
			 * ft_local_arena is handled separately below: we must
			 * NOT first-touch it from this (main) thread, or all
			 * its leaf pages land on the main thread's NUMA node,
			 * making every *_local engine pay remote leaf reads
			 * from all workers.  That single-node placement also
			 * destabilises high-thread-count runs (one saturated
			 * memory controller stalls random threads).
			 */
			for (size_t i = 0; i < lines; i++) {
				memset(&item[i].ht_node, 0,
				       sizeof(item[i].ht_node));
				memset(&item[i].ft_node, 0,
				       sizeof(item[i].ft_node));
				memset(&ft_il_arena_at(i)->node, 0,
				       sizeof(ft_il_arena_at(i)->node));
			}
			/*
			 * Drop ft_local_arena's pages so the next access
			 * re-faults them.  The re-fault happens in the worker
			 * threads' add phase (add_ft_specv_local writes each
			 * slot), placing each thread's leaf slice on its OWN
			 * local node — and re-faulted anonymous pages come
			 * back zeroed, satisfying the reinsert requirement
			 * without a main-thread touch.
			 */
			(void) madvise(ft_local_arena, ft_local_arena_total_bytes,
				       MADV_DONTNEED);
			map = fun->new(isc_g_mctx);

			size_t nitems = lines / (nthreads + 1);

			/* Keys actually built this iteration = [0, nitems*nthreads);
			 * ft_churn (worker 0) walks exactly that densely-built range. */
			g_total_keys = nitems * nthreads;

			isc_barrier_init(&barrier, nthreads);

			isc_time_t t0 = isc_time_now_hires();
			size_t m0 = isc_mem_inuse(isc_g_mctx);

			for (size_t i = 0; i < nthreads; i++) {
				threads[i] = (struct thread_s){
					.fun = fun,
					.map = map,
					.start = nitems * i,
					.end = nitems * i + nitems,
					.cpu = (unsigned int) i,
				};
				isc_thread_create(fun->thread, &threads[i],
						  &threads[i].thread);
			}

			uint64_t d0 = 0;
			uint64_t d1 = 0;

			for (size_t i = 0; i < nthreads; i++) {
				isc_thread_join(threads[i].thread, NULL);
				d0 += threads[i].d0;
				d1 += threads[i].d1;
			}

			{
				static bool dumped = false;
				if (!dumped) {
					dump_item_numa_distribution(fun->name);
					dump_arena_numa_distribution(fun->name);
					dumped = true;
				}
			}

			if (getenv("BENCH_DUMP_FT_STATS") &&
			    strncmp(fun->name, "ft_", 3) == 0) {
				fprintf(stderr,
					"\n=== %s: cds_ft_show_stats ===\n",
					fun->name);
				cds_ft_show_stats(map, stderr);
			}

			size_t m1 = isc_mem_inuse(isc_g_mctx);

			/*
			 * Self-report transparent-hugepage backing now that the
			 * trie + arenas are fully resident.  0 MB means no THP
			 * formed (e.g. PR_SET_THP_DISABLE still set, or the
			 * allocation pattern never triggered a huge fault).
			 */
			fprintf(stderr, "%-22s T=%-4zu AnonHugePages=%zu MB\n",
				fun->name, nthreads,
				proc_anonhugepages_kb() / 1024);

			rcu_barrier();

			isc_time_t t1 = isc_time_now_hires();
			uint64_t d3 = isc_time_microdiff(&t1, &t0);
			size_t m2 = isc_mem_inuse(isc_g_mctx);

			/*
			 * Aggregate throughput in Mops/s.  d0/d1 are the
			 * sums of per-thread microsecond counters; the
			 * per-thread average is d0/nthreads, so aggregate
			 * Mops/s = (nitems * nthreads) / (d0/nthreads µs)
			 *        = nitems * nthreads^2 / d0.
			 */
			double load_mops = (d0 == 0) ? 0.0 :
				(double) nitems * (double) nthreads
				* (double) nthreads / (double) d0;
			double query_mops = (d1 == 0) ? 0.0 :
				(double) nitems * (double) nthreads
				* (double) nthreads
				* (double) g_query_loops / (double) d1;

			printf("%10s | %10zu | %10.4f | %10.4f | %10.4f | "
			       "%10.4f | %10.4f |\n",
			       fun->name, nthreads,
			       load_mops, query_mops,
			       (double)(m1 - m0) / (1024.0 * 1024.0),
			       (double)d3 / (1000.0 * 1000.0),
			       (double)(m2 - m0) / (1024.0 * 1024.0));

			/*
			 * Tear down the data structure built this
			 * iteration.  Threads are already joined, so there
			 * are no concurrent accessors.  Without this every
			 * (engine x thread-count) pair leaks a full trie;
			 * across the sweep that accumulates ~100 tries and
			 * exhausts machine memory.
			 */
			if (fun->destroy != NULL)
				fun->destroy(map);
		}
	}

	printf("---------- | ---------- | ---------- | ---------- | "
	       "---------- | ---------- | ---------- |\n");

	/*
	 * Release the CSV text buffer back to the mctx.  item[].text
	 * pointed into it, but the sweep is finished and
	 * dns_name_fromtext() already copied the wire form into each
	 * item[].fixed, so nothing references it anymore.  Without this
	 * the BIND9 library-shutdown INSIST(isc_mem_inuse(ctx) == 0)
	 * aborts on an otherwise-clean exit.
	 */
	isc_mem_put(isc_g_mctx, filetext, filesize + 1);

	return EXIT_SUCCESS;
}
