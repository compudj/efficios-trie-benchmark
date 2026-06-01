/*
 * Read/write scalability benchmark: FT (RCU) vs Judy/QP/ART (rwlock)
 * vs BIND9 QP-trie (dns_qpmulti_t with RCU).
 *
 * Pre-populates with 1M DNS keys, then runs N reader threads with
 * 1 writer thread doing continuous insert/remove churn.
 */
#define _GNU_SOURCE
#define _LGPL_SOURCE
/* Use membarrier flavor for all engines — matches BIND9's default. */
#define RCU_MEMBARRIER
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <urcu.h>
#include <urcu/fractal-trie.h>
#include <Judy.h>
#include "Tbl.h"
#include "art.h"

/* BIND9 headers — config.h must come first for constexpr compat */
#include "config.h"
#include <isc/mem.h>
#include <isc/lib.h>
#include <isc/tid.h>
#include <isc/urcu.h>
#include <isc/result.h>
#include <dns/qp.h>
#include <dns/lib.h>

#define N_KEYS		1000000
#define DURATION_SEC	3
#define BATCH_SIZE	1000

static char *str_keys[N_KEYS];
static size_t str_lens[N_KEYS];

static uint64_t xorshift64(uint64_t *s) {
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
#define N_DOMAINS 24

struct ft_entry {
	struct rcu_head rcu_head;
	struct cds_ft_node ft_node;
	size_t key_len;
	char key[];
};

static volatile int start_flag;
static volatile int stop_flag;

/* Engine-specific globals */
static struct cds_ft *g_ft;
static Pvoid_t g_judy;
static Tbl *g_qp;
static art_tree g_art;
static pthread_rwlock_t g_rwlock;

static void init_rwlock(void)
{
	pthread_rwlockattr_t attr;
	pthread_rwlockattr_init(&attr);
	pthread_rwlockattr_setkind_np(&attr,
		PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	pthread_rwlock_init(&g_rwlock, &attr);
	pthread_rwlockattr_destroy(&attr);
}
static pthread_mutex_t g_ft_mutex = PTHREAD_MUTEX_INITIALIZER;

enum engine { ENG_FT_CAND, ENG_JUDY, ENG_QP, ENG_ART, ENG_BIND9_QP };

static const char *eng_name[] = {
	[ENG_FT_CAND]   = "FT cand+v",
	[ENG_JUDY]       = "Judy+rwl",
	[ENG_QP]         = "QP+rwl",
	[ENG_ART]        = "ART+rwl",
	[ENG_BIND9_QP]   = "BIND9 QP",
};

struct thread_arg {
	enum engine eng;
	unsigned long count;
	unsigned int seed;
	int tid;		/* for BIND9 QP isc_tid */
};

/* ── BIND9 QP-trie infrastructure ─────────────────────────── */

/*
 * BIND9 QP-trie key encoding constants (from qp_p.h).
 * These are stable ABI values.
 */
#define QP_SHIFT_NOBYTE  2
#define QP_SHIFT_OFFSET  49

/*
 * Extern reference to BIND9's byte-to-bit lookup table.
 * Initialized by dns_lib_initialize() (constructor).
 */
extern uint16_t dns_qp_bits_for_byte[];

/*
 * We store the BIND9 QP-trie leaf data in a struct that holds the
 * original key bytes so makekey can reconstruct the qpkey.
 */
struct bind9_leaf {
	size_t raw_len;
	char raw_key[];
};

/*
 * Pre-computed QP keys for all lookup keys and churn keys.
 * These are used by the makekey callback to reconstruct keys from leaves.
 */
static dns_qpkey_t  qp_lookup_keys[N_KEYS];
static size_t       qp_lookup_lens[N_KEYS];

#define CHURN_KEYS	10000
static dns_qpkey_t  qp_churn_keys[CHURN_KEYS];
static size_t       qp_churn_lens[CHURN_KEYS];

/*
 * Convert raw bytes to a BIND9 QP key.
 * Each raw byte is mapped through dns_qp_bits_for_byte[].
 * Common hostname chars produce 1 qpkey byte; others produce 2 (escaped).
 * We add SHIFT_NOBYTE as a terminator.
 *
 * Returns key length.
 */
static size_t
raw_to_qpkey(dns_qpkey_t key, const char *raw, size_t raw_len)
{
	size_t len = 0;

	/* Namespace prefix byte - use '0' (namespace 0) */
	key[len++] = dns_qp_bits_for_byte[(unsigned char)'0'];

	for (size_t i = 0; i < raw_len; i++) {
		uint16_t bits = dns_qp_bits_for_byte[(unsigned char)raw[i]];
		key[len++] = bits & 0xFF;       /* bit_one */
		if ((bits >> 8) != 0) {         /* escape? */
			key[len++] = bits >> 8; /* bit_two */
		}
	}
	/* End marker */
	key[len++] = QP_SHIFT_NOBYTE;
	return len;
}

/*
 * BIND9 QP methods for our benchmark leaves.
 * pval points to a bind9_leaf, ival is the index.
 */

/* Leaves stored per-index for lookup and churn sets. */
static struct bind9_leaf *bind9_lookup_leaves[N_KEYS];
static struct bind9_leaf *bind9_churn_leaves[CHURN_KEYS];
static bool bind9_churn_present[CHURN_KEYS];

static void
bench_qp_attach(void *uctx, void *pval, uint32_t ival) {
	(void)uctx;
	(void)pval;
	(void)ival;
	/* No-op: we manage lifetime ourselves */
}

static void
bench_qp_detach(void *uctx, void *pval, uint32_t ival) {
	(void)uctx;
	(void)pval;
	(void)ival;
	/* No-op: we manage lifetime ourselves */
}

/*
 * Reconstruct the QP key from the stored leaf.
 * We use ival to distinguish lookup keys (ival < N_KEYS) from
 * churn keys (ival >= N_KEYS).
 */
static size_t
bench_qp_makekey(dns_qpkey_t key, void *uctx, void *pval, uint32_t ival) {
	(void)uctx;
	(void)pval;
	if (ival < N_KEYS) {
		memcpy(key, qp_lookup_keys[ival], qp_lookup_lens[ival]);
		return qp_lookup_lens[ival];
	} else {
		uint32_t cidx = ival - N_KEYS;
		memcpy(key, qp_churn_keys[cidx], qp_churn_lens[cidx]);
		return qp_churn_lens[cidx];
	}
}

static void
bench_qp_triename(void *uctx, char *buf, size_t size) {
	(void)uctx;
	snprintf(buf, size, "bench_rw");
}

static const dns_qpmethods_t bench_qp_methods = {
	.attach   = bench_qp_attach,
	.detach   = bench_qp_detach,
	.makekey  = bench_qp_makekey,
	.triename = bench_qp_triename,
};

static dns_qpmulti_t *g_bind9_qp;
static isc_mem_t *g_bind9_mctx;

/*
 * Thread-local TID counter for BIND9.
 * Each reader/writer thread that uses dns_qpmulti_query needs a unique tid.
 */
static _Atomic int g_bind9_tid_counter;

/* ── Reader threads ──────────────────────────────────────── */

static void *reader_thread(void *arg)
{
	struct thread_arg *ta = arg;
	unsigned long count = 0;
	uint64_t seed = ta->seed;

	if (ta->eng == ENG_FT_CAND || ta->eng == ENG_BIND9_QP)
		rcu_register_thread();

	if (ta->eng == ENG_BIND9_QP) {
		int tid = __atomic_fetch_add(&g_bind9_tid_counter, 1,
					     __ATOMIC_RELAXED);
		isc__tid_init(tid);
	}

	while (!start_flag)
		__asm__ __volatile__("pause" ::: "memory");

	while (!stop_flag) {
		unsigned int batch = BATCH_SIZE;

		switch (ta->eng) {
		case ENG_FT_CAND:
			rcu_read_lock();
			for (unsigned int b = 0; b < batch; b++) {
				unsigned int idx = xorshift64(&seed) % N_KEYS;
				struct cds_ft_node *found;
				cds_ft_lookup_candidate_key(g_ft,
					(const uint8_t *)str_keys[idx],
					str_lens[idx], str_lens[idx], &found);
				if (found) {
					struct ft_entry *e = cds_ft_entry(found,
						struct ft_entry, ft_node);
					if (e->key_len != str_lens[idx] ||
					    memcmp(e->key, str_keys[idx], e->key_len) != 0)
						found = NULL;
				}
			}
			rcu_read_unlock();
			rcu_quiescent_state();
			break;

		case ENG_JUDY:
			pthread_rwlock_rdlock(&g_rwlock);
			for (unsigned int b = 0; b < batch; b++) {
				unsigned int idx = xorshift64(&seed) % N_KEYS;
				Word_t *pv;
				JSLG(pv, g_judy, (uint8_t *)str_keys[idx]);
			}
			pthread_rwlock_unlock(&g_rwlock);
			break;

		case ENG_QP:
			pthread_rwlock_rdlock(&g_rwlock);
			for (unsigned int b = 0; b < batch; b++) {
				unsigned int idx = xorshift64(&seed) % N_KEYS;
				volatile void *sink;
				sink = Tgetl(g_qp, str_keys[idx], str_lens[idx]);
				(void)sink;
			}
			pthread_rwlock_unlock(&g_rwlock);
			break;

		case ENG_ART:
			pthread_rwlock_rdlock(&g_rwlock);
			for (unsigned int b = 0; b < batch; b++) {
				unsigned int idx = xorshift64(&seed) % N_KEYS;
				volatile void *sink;
				sink = art_search(&g_art,
					(unsigned char *)str_keys[idx],
					str_lens[idx] + 1);
				(void)sink;
			}
			pthread_rwlock_unlock(&g_rwlock);
			break;

		case ENG_BIND9_QP:
		{
			dns_qpread_t qpr;
			dns_qpmulti_query(g_bind9_qp, &qpr);
			for (unsigned int b = 0; b < batch; b++) {
				unsigned int idx = xorshift64(&seed) % N_KEYS;
				void *pval = NULL;
				uint32_t ival = 0;
				dns_qp_getkey(&qpr,
					qp_lookup_keys[idx],
					qp_lookup_lens[idx],
					&pval, &ival);
			}
			dns_qpread_destroy(g_bind9_qp, &qpr);
			rcu_quiescent_state();
			break;
		}
		}
		count += batch;
	}

	if (ta->eng == ENG_FT_CAND || ta->eng == ENG_BIND9_QP)
		rcu_unregister_thread();

	ta->count = count;
	return NULL;
}

/* ── Writer thread (1 per run) ───────────────────────────── */

/*
 * Churn: remove a random existing key, then re-insert it.
 * This maintains constant population while creating write
 * contention on the data structure.
 */

/* Extra keys for writer churn (not in the lookup set). */
static char *churn_keys[CHURN_KEYS];
static size_t churn_lens[CHURN_KEYS];
static struct ft_entry *churn_entries[CHURN_KEYS];

/* RCU callback for deferred ft_entry free after remove. */
static void free_ft_entry_rcu(struct rcu_head *head)
{
	struct ft_entry *e = caa_container_of(head, struct ft_entry, rcu_head);
	free(e);
}

struct writer_arg {
	enum engine eng;
	unsigned long writes;
};

static void *writer_thread(void *arg)
{
	struct writer_arg *wa = arg;
	unsigned long writes = 0;
	uint64_t seed = 99999;
	int inserted = 0;	/* how many churn keys are currently in */

	struct cds_ft_iter *ft_iter = NULL;

	if (wa->eng == ENG_FT_CAND) {
		rcu_register_thread();
		cds_ft_iter_create(g_ft, &ft_iter);
	}

	if (wa->eng == ENG_BIND9_QP) {
		rcu_register_thread();
		int tid = __atomic_fetch_add(&g_bind9_tid_counter, 1,
					     __ATOMIC_RELAXED);
		isc__tid_init(tid);
	}

	while (!start_flag)
		__asm__ __volatile__("pause" ::: "memory");

	while (!stop_flag) {
		/* Alternate: insert a churn key, then remove one. */
		unsigned int cidx = xorshift64(&seed) % CHURN_KEYS;

		switch (wa->eng) {
		case ENG_FT_CAND:
			pthread_mutex_lock(&g_ft_mutex);
			if (churn_entries[cidx]) {
				/* Remove */
				struct ft_entry *old = churn_entries[cidx];
				enum cds_ft_status ls, rs;
				cds_ft_iter_invalidate_path(ft_iter);
				ls = cds_ft_iter_set_key(ft_iter,
					(const uint8_t *)churn_keys[cidx],
					churn_lens[cidx]);
				if (ls < 0) {
					fprintf(stderr, "iter_set_key error: %d\n", ls);
					abort();
				}
				ls = cds_ft_lookup(g_ft, ft_iter);
				if (ls != CDS_FT_STATUS_OK) {
					fprintf(stderr, "lookup failed: %d cidx=%u key=%.*s\n",
						ls, cidx, (int)churn_lens[cidx], churn_keys[cidx]);
					abort();
				}
				rs = cds_ft_remove(g_ft, ft_iter, &old->ft_node);
				if (rs != CDS_FT_STATUS_OK) {
					fprintf(stderr, "remove failed: %d\n", rs);
					abort();
				}
				churn_entries[cidx] = NULL;
				pthread_mutex_unlock(&g_ft_mutex);
				/* Defer free after grace period. */
				call_rcu(&old->rcu_head, free_ft_entry_rcu);
			} else {
				/* Insert */
				struct ft_entry *e = calloc(1,
					sizeof(*e) + churn_lens[cidx]);
				struct cds_ft_node *result;
				enum cds_ft_status status;
				if (!e) { fprintf(stderr, "OOM\n"); abort(); }
				memcpy(e->key, churn_keys[cidx], churn_lens[cidx]);
				e->key_len = churn_lens[cidx];
				status = cds_ft_insert_unique(g_ft,
					(const uint8_t *)churn_keys[cidx],
					churn_lens[cidx], &e->ft_node, &result);
				if (status == CDS_FT_STATUS_OK) {
					churn_entries[cidx] = e;
				} else if (status == CDS_FT_STATUS_DUPLICATE_FOUND) {
					free(e);
				} else {
					fprintf(stderr, "insert_unique error: %d\n", status);
					abort();
				}
				pthread_mutex_unlock(&g_ft_mutex);
			}
			if (++writes % 1000 == 0)
				rcu_quiescent_state();
			break;

		case ENG_JUDY:
			pthread_rwlock_wrlock(&g_rwlock);
			{
				Word_t *pv;
				int rc;
				JSLG(pv, g_judy, (uint8_t *)churn_keys[cidx]);
				if (pv) {
					JSLD(rc, g_judy, (uint8_t *)churn_keys[cidx]);
				} else {
					JSLI(pv, g_judy, (uint8_t *)churn_keys[cidx]);
					*pv = cidx;
				}
			}
			pthread_rwlock_unlock(&g_rwlock);
			break;

		case ENG_QP:
			pthread_rwlock_wrlock(&g_rwlock);
			{
				void *v = Tgetl(g_qp, churn_keys[cidx], churn_lens[cidx]);
				if (v) {
					g_qp = Tdell(g_qp, churn_keys[cidx], churn_lens[cidx]);
				} else {
					g_qp = Tsetl(g_qp, churn_keys[cidx], churn_lens[cidx],
						(void *)(uintptr_t)((cidx + 1) << 1));
				}
			}
			pthread_rwlock_unlock(&g_rwlock);
			break;

		case ENG_ART:
			pthread_rwlock_wrlock(&g_rwlock);
			{
				void *v = art_search(&g_art,
					(unsigned char *)churn_keys[cidx],
					churn_lens[cidx] + 1);
				if (v) {
					art_delete(&g_art,
						(unsigned char *)churn_keys[cidx],
						churn_lens[cidx] + 1);
				} else {
					art_insert(&g_art,
						(unsigned char *)churn_keys[cidx],
						churn_lens[cidx] + 1,
						(void *)(uintptr_t)((cidx + 1) << 1));
				}
			}
			pthread_rwlock_unlock(&g_rwlock);
			break;

		case ENG_BIND9_QP:
		{
			dns_qp_t *qpt = NULL;
			dns_qpmulti_write(g_bind9_qp, &qpt);
			if (bind9_churn_present[cidx]) {
				/* Delete */
				isc_result_t result;
				result = dns_qp_deletekey(qpt,
					qp_churn_keys[cidx],
					qp_churn_lens[cidx],
					NULL, NULL);
				if (result == ISC_R_SUCCESS) {
					bind9_churn_present[cidx] = false;
				}
			} else {
				/* Insert */
				isc_result_t result;
				uint32_t ival = N_KEYS + cidx;
				result = dns_qp_insert(qpt,
					bind9_churn_leaves[cidx], ival);
				if (result == ISC_R_SUCCESS) {
					bind9_churn_present[cidx] = true;
				}
			}
			/* Compact if fragmented */
			if (dns_qp_memusage(qpt).fragmented) {
				dns_qp_compact(qpt, DNS_QPGC_NOW);
			}
			dns_qpmulti_commit(g_bind9_qp, &qpt);
			break;
		}
		}
		writes++;
	}

	if (wa->eng == ENG_FT_CAND) {
		cds_ft_iter_destroy(ft_iter);
		rcu_unregister_thread();
	}

	if (wa->eng == ENG_BIND9_QP)
		rcu_unregister_thread();

	wa->writes = writes;
	return NULL;
}

static void run_bench(enum engine eng, int nr_readers, double *read_mops, double *write_kops)
{
	pthread_t *threads = calloc(nr_readers + 1, sizeof(pthread_t));
	struct thread_arg *rargs = calloc(nr_readers, sizeof(struct thread_arg));
	struct writer_arg warg = { .eng = eng, .writes = 0 };
	struct timespec ts;
	unsigned long total_reads = 0;

	start_flag = 0;
	stop_flag = 0;

	/* Clear churn state */
	memset(churn_entries, 0, sizeof(churn_entries));
	if (eng == ENG_BIND9_QP)
		memset(bind9_churn_present, 0, sizeof(bind9_churn_present));

	/*
	 * Reset BIND9 TID counter for each run so we don't overflow
	 * ISC_TID_MAX across runs.  Each run's threads get fresh tids.
	 */
	if (eng == ENG_BIND9_QP)
		__atomic_store_n(&g_bind9_tid_counter, 0, __ATOMIC_RELAXED);

	/* Spawn readers */
	for (int i = 0; i < nr_readers; i++) {
		rargs[i].eng = eng;
		rargs[i].count = 0;
		rargs[i].seed = 42 + i * 1000;
		rargs[i].tid = i;
		pthread_create(&threads[i], NULL, reader_thread, &rargs[i]);
	}

	/* Spawn writer */
	pthread_create(&threads[nr_readers], NULL, writer_thread, &warg);

	usleep(10000);

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

	/* Cleanup churn entries still in FT */
	if (eng == ENG_FT_CAND) {
		struct cds_ft_iter *cleanup_iter;
		rcu_register_thread();
		cds_ft_iter_create(g_ft, &cleanup_iter);
		pthread_mutex_lock(&g_ft_mutex);
		for (int i = 0; i < CHURN_KEYS; i++) {
			if (churn_entries[i]) {
				cds_ft_iter_set_key(cleanup_iter,
					(const uint8_t *)churn_keys[i],
					churn_lens[i]);
				cds_ft_lookup(g_ft, cleanup_iter);
				cds_ft_remove(g_ft, cleanup_iter,
					&churn_entries[i]->ft_node);
				rcu_quiescent_state();
				free(churn_entries[i]);
				churn_entries[i] = NULL;
			}
		}
		pthread_mutex_unlock(&g_ft_mutex);
		cds_ft_iter_destroy(cleanup_iter);
		rcu_unregister_thread();
	}

	/* Cleanup churn entries still in BIND9 QP */
	if (eng == ENG_BIND9_QP) {
		rcu_register_thread();
		int tid = __atomic_fetch_add(&g_bind9_tid_counter, 1,
					     __ATOMIC_RELAXED);
		isc__tid_init(tid);

		dns_qp_t *qpt = NULL;
		dns_qpmulti_write(g_bind9_qp, &qpt);
		for (int i = 0; i < CHURN_KEYS; i++) {
			if (bind9_churn_present[i]) {
				dns_qp_deletekey(qpt,
					qp_churn_keys[i],
					qp_churn_lens[i],
					NULL, NULL);
				bind9_churn_present[i] = false;
			}
		}
		dns_qpmulti_commit(g_bind9_qp, &qpt);
		rcu_unregister_thread();
	}

	free(threads);
	free(rargs);
}

int main(int argc, char **argv)
{
	uint64_t seed = 12345;
	int max_threads = 384;

	if (argc > 1)
		max_threads = atoi(argv[1]);

	init_rwlock();

	rcu_register_thread();

	/* Generate lookup keys */
	for (unsigned int i = 0; i < N_KEYS; i++) {
		char buf[128];
		int len = snprintf(buf, sizeof(buf), "%s.host%06u.zone%u",
			dns_domains[xorshift64(&seed) % N_DOMAINS],
			(unsigned)(xorshift64(&seed) % 100000),
			(unsigned)(xorshift64(&seed) % 50));
		str_keys[i] = strdup(buf);
		str_lens[i] = len;
	}

	/* Generate churn keys (different from lookup keys) */
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

	/* Build FT */
	{
		struct cds_ft_group_attr *attr;
		struct cds_ft_group *group;
		cds_ft_group_attr_create(&attr);
		cds_ft_group_attr_set_max_key_len(attr, 256);
		cds_ft_group_attr_set_key_len(attr, CDS_FT_LEN_VARIABLE);
		/* Enable speculative_validated with the entry's key_off/key_len_off */
		cds_ft_group_attr_set_speculative_validated(attr,
			offsetof(struct ft_entry, key),
			offsetof(struct ft_entry, key_len), 0);
		cds_ft_group_create(attr, &group);
		cds_ft_group_attr_destroy(attr);
		cds_ft_create(group, NULL, &g_ft);

		for (unsigned int i = 0; i < N_KEYS; i++) {
			struct cds_ft_node *result;
			struct ft_entry *e = calloc(1, sizeof(*e) + str_lens[i]);
			memcpy(e->key, str_keys[i], str_lens[i]);
			e->key_len = str_lens[i];
			cds_ft_insert_unique(g_ft, (const uint8_t *)str_keys[i],
				str_lens[i], &e->ft_node, &result);
			if ((i & 1023) == 0) rcu_quiescent_state();
		}
		rcu_quiescent_state();
	}

	/* Build Judy */
	for (unsigned int i = 0; i < N_KEYS; i++) {
		Word_t *pv;
		JSLI(pv, g_judy, (uint8_t *)str_keys[i]);
		*pv = i;
	}

	/* Build QP */
	for (unsigned int i = 0; i < N_KEYS; i++)
		g_qp = Tsetl(g_qp, str_keys[i], str_lens[i],
			(void *)(uintptr_t)((i + 1) << 1));

	/* Build ART */
	art_tree_init(&g_art);
	for (unsigned int i = 0; i < N_KEYS; i++)
		art_insert(&g_art, (unsigned char *)str_keys[i],
			str_lens[i] + 1, (void *)(uintptr_t)((i + 1) << 1));

	/*
	 * Build BIND9 QP-trie.
	 *
	 * dns_lib_initialize() and isc_lib_initialize() are __constructor__
	 * attributes, so they run automatically when the shared libraries
	 * are loaded.  This initializes dns_qp_bits_for_byte[] among other
	 * things.
	 */
	{
		/* Create ISC memory context */
		isc_mem_create("bench", &g_bind9_mctx);

		/* Pre-compute QP keys for lookup set */
		for (unsigned int i = 0; i < N_KEYS; i++) {
			qp_lookup_lens[i] = raw_to_qpkey(qp_lookup_keys[i],
				str_keys[i], str_lens[i]);
		}

		/* Pre-compute QP keys for churn set */
		for (int i = 0; i < CHURN_KEYS; i++) {
			qp_churn_lens[i] = raw_to_qpkey(qp_churn_keys[i],
				churn_keys[i], churn_lens[i]);
		}

		/*
		 * Allocate bind9_leaf objects for lookup keys.
		 * These are 4-byte aligned (malloc guarantees this).
		 */
		for (unsigned int i = 0; i < N_KEYS; i++) {
			struct bind9_leaf *leaf = calloc(1,
				sizeof(*leaf) + str_lens[i]);
			if (!leaf) {
				fprintf(stderr, "OOM allocating bind9_leaf\n");
				abort();
			}
			leaf->raw_len = str_lens[i];
			memcpy(leaf->raw_key, str_keys[i], str_lens[i]);
			bind9_lookup_leaves[i] = leaf;
		}

		/* Allocate bind9_leaf objects for churn keys */
		for (int i = 0; i < CHURN_KEYS; i++) {
			struct bind9_leaf *leaf = calloc(1,
				sizeof(*leaf) + churn_lens[i]);
			if (!leaf) {
				fprintf(stderr, "OOM allocating bind9_leaf\n");
				abort();
			}
			leaf->raw_len = churn_lens[i];
			memcpy(leaf->raw_key, churn_keys[i], churn_lens[i]);
			bind9_churn_leaves[i] = leaf;
		}

		/* Initialize thread TID for main thread */
		isc__tid_init(0);
		__atomic_store_n(&g_bind9_tid_counter, 1, __ATOMIC_RELAXED);

		/* Create the multi-threaded QP-trie */
		dns_qpmulti_create(g_bind9_mctx, &bench_qp_methods, NULL,
			&g_bind9_qp);

		/* Bulk-load using an update transaction */
		{
			dns_qp_t *qpt = NULL;
			dns_qpmulti_update(g_bind9_qp, &qpt);
			for (unsigned int i = 0; i < N_KEYS; i++) {
				isc_result_t result;
				result = dns_qp_insert(qpt,
					bind9_lookup_leaves[i], i);
				if (result == ISC_R_EXISTS) {
					/* Duplicate key — skip. */
					continue;
				}
				if (result != ISC_R_SUCCESS) {
					fprintf(stderr,
						"BIND9 QP insert failed: %u "
						"(key=%.*s)\n",
						result, (int)str_lens[i],
						str_keys[i]);
					abort();
				}
			}
			dns_qp_compact(qpt, DNS_QPGC_ALL);
			dns_qpmulti_commit(g_bind9_qp, &qpt);
		}

		fprintf(stderr, "BIND9 QP-trie loaded %u keys\n", N_KEYS);
	}

	rcu_unregister_thread();

	printf("Read/Write scalability (1M DNS keys, 1 writer + N readers, %ds per point)\n",
		DURATION_SEC);
	printf("%-10s %12s %10s %12s %10s %12s %10s %12s %10s %12s %10s\n",
		"Readers",
		"FT reads", "FT wr",
		"Judy reads", "Judy wr",
		"QP reads", "QP wr",
		"ART reads", "ART wr",
		"B9QP reads", "B9QP wr");
	printf("%-10s %12s %10s %12s %10s %12s %10s %12s %10s %12s %10s\n",
		"", "(Mops/s)", "(Kops/s)",
		"(Mops/s)", "(Kops/s)",
		"(Mops/s)", "(Kops/s)",
		"(Mops/s)", "(Kops/s)",
		"(Mops/s)", "(Kops/s)");
	printf("──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────\n");

	int thread_counts[] = {1, 2, 4, 8, 16, 32, 64, 128, 192, 256, 383};
	int nr_counts = sizeof(thread_counts) / sizeof(thread_counts[0]);

	for (int ti = 0; ti < nr_counts; ti++) {
		int nt = thread_counts[ti];
		if (nt + 1 > max_threads)	/* +1 for writer */
			break;

		double ft_r, ft_w, judy_r, judy_w, qp_r, qp_w, art_r, art_w;
		double b9qp_r, b9qp_w;

		run_bench(ENG_FT_CAND, nt, &ft_r, &ft_w);
		run_bench(ENG_JUDY, nt, &judy_r, &judy_w);
		run_bench(ENG_QP, nt, &qp_r, &qp_w);
		run_bench(ENG_ART, nt, &art_r, &art_w);
		run_bench(ENG_BIND9_QP, nt, &b9qp_r, &b9qp_w);

		printf("%-10d %12.1f %10.1f %12.1f %10.1f %12.1f %10.1f %12.1f %10.1f %12.1f %10.1f\n",
			nt, ft_r, ft_w, judy_r, judy_w, qp_r, qp_w,
			art_r, art_w, b9qp_r, b9qp_w);
		fflush(stdout);
	}

	/* Cleanup BIND9 */
	dns_qpmulti_destroy(&g_bind9_qp);
	isc_mem_detach(&g_bind9_mctx);

	return 0;
}
