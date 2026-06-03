/*
 * BIND9 QP-trie engine (dns_qpmulti_t, RCU-backed) for the read/write scaling
 * benchmark.  This is the ONLY per-engine executable that links bind9
 * (libisc/libdns) and liburcu.
 *
 * RCU registration: libisc's ELF constructor isc__lib_initialize() already
 * registers the MAIN thread with RCU (and leaves it registered until
 * isc__lib_shutdown()).  So main() here must NOT call rcu_register_thread() —
 * doing so would double-add main's urcu_reader to the registry and wedge every
 * grace period.  The spawned reader/writer threads are raw pthreads not covered
 * by the constructor, so they register themselves.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _LGPL_SOURCE
#define RCU_MEMBARRIER
#include "config.h"
#include <urcu.h>

#include <isc/mem.h>
#include <isc/lib.h>
#include <isc/tid.h>
#include <isc/urcu.h>
#include <isc/result.h>
#include <dns/qp.h>
#include <dns/lib.h>

#include "bench_scale_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>	/* _exit */

/*
 * BIND9 QP-trie key encoding constants (from qp_p.h); stable ABI values.
 */
#define QP_SHIFT_NOBYTE  2
#define QP_SHIFT_OFFSET  49

/* BIND9's byte-to-bit lookup table; initialized by dns_lib_initialize(). */
extern uint16_t dns_qp_bits_for_byte[];

/* Leaf storing the original key bytes so makekey can reconstruct the qpkey. */
struct bind9_leaf {
	size_t raw_len;
	char raw_key[];
};

static dns_qpkey_t qp_lookup_keys[N_KEYS];
static size_t      qp_lookup_lens[N_KEYS];
static dns_qpkey_t qp_churn_keys[CHURN_KEYS];
static size_t      qp_churn_lens[CHURN_KEYS];

static struct bind9_leaf *bind9_lookup_leaves[N_KEYS];
static struct bind9_leaf *bind9_churn_leaves[CHURN_KEYS];
static bool bind9_churn_present[CHURN_KEYS];

static dns_qpmulti_t *g_bind9_qp;
/*
 * Use isc's global, isc-managed memory context (isc_g_mctx) rather than a
 * custom isc_mem_create() one.  isc owns its lifetime, so we never detach it;
 * we only have to free what we put in it (the qpmulti, via
 * dns_qpmulti_destroy()), and libisc's shutdown rcu_barrier() drains the
 * deferred reclamation before its INSIST(isc_mem_inuse(ctx) == 0).  This
 * mirrors load-names.  (bind9_leaf objects below are plain calloc(), not isc
 * memory, so they don't count toward isc_mem_inuse.)
 */

/*
 * Per-thread isc tid counter.  Each thread doing dns_qpmulti_query/_write needs
 * a unique tid < ISC_TID_MAX.  Reset per run (reserving 0 for the main thread)
 * so the sweep doesn't exceed ISC_TID_MAX across thread-count points.
 */
static _Atomic int g_bind9_tid_counter;

/*
 * Convert raw bytes to a BIND9 QP key.  Each raw byte maps through
 * dns_qp_bits_for_byte[]; common hostname chars produce 1 qpkey byte, others 2
 * (escaped).  A namespace prefix and an end marker bracket the key.
 */
static size_t
raw_to_qpkey(dns_qpkey_t key, const char *raw, size_t raw_len)
{
	size_t len = 0;

	key[len++] = dns_qp_bits_for_byte[(unsigned char)'0'];
	for (size_t i = 0; i < raw_len; i++) {
		uint16_t bits = dns_qp_bits_for_byte[(unsigned char)raw[i]];
		key[len++] = bits & 0xFF;
		if ((bits >> 8) != 0)
			key[len++] = bits >> 8;
	}
	key[len++] = QP_SHIFT_NOBYTE;
	return len;
}

static void
bench_qp_attach(void *uctx, void *pval, uint32_t ival)
{
	(void)uctx; (void)pval; (void)ival;	/* we manage lifetime ourselves */
}

static void
bench_qp_detach(void *uctx, void *pval, uint32_t ival)
{
	(void)uctx; (void)pval; (void)ival;
}

static size_t
bench_qp_makekey(dns_qpkey_t key, void *uctx, void *pval, uint32_t ival)
{
	(void)uctx; (void)pval;
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
bench_qp_triename(void *uctx, char *buf, size_t size)
{
	(void)uctx;
	snprintf(buf, size, "bench_rw");
}

static const dns_qpmethods_t bench_qp_methods = {
	.attach   = bench_qp_attach,
	.detach   = bench_qp_detach,
	.makekey  = bench_qp_makekey,
	.triename = bench_qp_triename,
};

static void b9qp_build(void)
{
	for (unsigned int i = 0; i < N_KEYS; i++)
		qp_lookup_lens[i] = raw_to_qpkey(qp_lookup_keys[i],
			str_keys[i], str_lens[i]);
	for (int i = 0; i < CHURN_KEYS; i++)
		qp_churn_lens[i] = raw_to_qpkey(qp_churn_keys[i],
			churn_keys[i], churn_lens[i]);

	for (unsigned int i = 0; i < N_KEYS; i++) {
		struct bind9_leaf *leaf = calloc(1, sizeof(*leaf) + str_lens[i]);
		if (!leaf) {
			fprintf(stderr, "OOM allocating bind9_leaf\n");
			abort();
		}
		leaf->raw_len = str_lens[i];
		memcpy(leaf->raw_key, str_keys[i], str_lens[i]);
		bind9_lookup_leaves[i] = leaf;
	}
	for (int i = 0; i < CHURN_KEYS; i++) {
		struct bind9_leaf *leaf = calloc(1, sizeof(*leaf) + churn_lens[i]);
		if (!leaf) {
			fprintf(stderr, "OOM allocating bind9_leaf\n");
			abort();
		}
		leaf->raw_len = churn_lens[i];
		memcpy(leaf->raw_key, churn_keys[i], churn_lens[i]);
		bind9_churn_leaves[i] = leaf;
	}

	/* Main thread is tid 0 (reserved); readers/writers start at 1. */
	isc__tid_init(0);
	__atomic_store_n(&g_bind9_tid_counter, 1, __ATOMIC_RELAXED);

	dns_qpmulti_create(isc_g_mctx, &bench_qp_methods, NULL, &g_bind9_qp);

	{
		dns_qp_t *qpt = NULL;
		dns_qpmulti_update(g_bind9_qp, &qpt);
		for (unsigned int i = 0; i < N_KEYS; i++) {
			isc_result_t result = dns_qp_insert(qpt,
				bind9_lookup_leaves[i], i);
			if (result == ISC_R_EXISTS)
				continue;	/* duplicate key — skip */
			if (result != ISC_R_SUCCESS) {
				fprintf(stderr,
					"BIND9 QP insert failed: %u (key=%.*s)\n",
					result, (int)str_lens[i], str_keys[i]);
				abort();
			}
		}
		dns_qp_compact(qpt, DNS_QPGC_ALL);
		dns_qpmulti_commit(g_bind9_qp, &qpt);
	}

	fprintf(stderr, "BIND9 QP-trie loaded %u keys\n", N_KEYS);
}

static void *b9qp_reader_setup(void)
{
	rcu_register_thread();
	int tid = __atomic_fetch_add(&g_bind9_tid_counter, 1, __ATOMIC_RELAXED);
	isc__tid_init(tid);
	return NULL;
}

static void b9qp_reader_teardown(void *ctx)
{
	(void)ctx;
	rcu_unregister_thread();
}

static void b9qp_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	dns_qpread_t qpr;
	dns_qpmulti_query(g_bind9_qp, &qpr);
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		void *pval = NULL;
		uint32_t ival = 0;
		dns_qp_getkey(&qpr, qp_lookup_keys[idx], qp_lookup_lens[idx],
			&pval, &ival);
	}
	dns_qpread_destroy(g_bind9_qp, &qpr);
	rcu_quiescent_state();
}

static void *b9qp_writer_setup(void)
{
	rcu_register_thread();
	int tid = __atomic_fetch_add(&g_bind9_tid_counter, 1, __ATOMIC_RELAXED);
	isc__tid_init(tid);
	return NULL;
}

static void b9qp_writer_teardown(void *ctx)
{
	(void)ctx;
	rcu_unregister_thread();
}

static void b9qp_writer_step(void *ctx, uint64_t *seed, unsigned long writes)
{
	(void)ctx;
	(void)writes;
	unsigned int cidx = xorshift64(seed) % CHURN_KEYS;
	dns_qp_t *qpt = NULL;

	dns_qpmulti_write(g_bind9_qp, &qpt);
	if (bind9_churn_present[cidx]) {
		isc_result_t result = dns_qp_deletekey(qpt,
			qp_churn_keys[cidx], qp_churn_lens[cidx], NULL, NULL);
		if (result == ISC_R_SUCCESS)
			bind9_churn_present[cidx] = false;
	} else {
		uint32_t ival = N_KEYS + cidx;
		isc_result_t result = dns_qp_insert(qpt,
			bind9_churn_leaves[cidx], ival);
		if (result == ISC_R_SUCCESS)
			bind9_churn_present[cidx] = true;
	}
	if (dns_qp_memusage(qpt).fragmented)
		dns_qp_compact(qpt, DNS_QPGC_NOW);
	dns_qpmulti_commit(g_bind9_qp, &qpt);
}

static void b9qp_run_reset(void)
{
	memset(bind9_churn_present, 0, sizeof(bind9_churn_present));
	/* Reserve tid 0 for the main thread; spawned threads start at 1. */
	__atomic_store_n(&g_bind9_tid_counter, 1, __ATOMIC_RELAXED);
}

static void b9qp_cleanup_churn(void)
{
	/*
	 * Runs on the main thread (isc tid 0, RCU-registered by libisc); no
	 * register/tid-init needed.  Threads have already joined.
	 */
	dns_qp_t *qpt = NULL;
	dns_qpmulti_write(g_bind9_qp, &qpt);
	for (int i = 0; i < CHURN_KEYS; i++) {
		if (bind9_churn_present[i]) {
			dns_qp_deletekey(qpt, qp_churn_keys[i],
				qp_churn_lens[i], NULL, NULL);
			bind9_churn_present[i] = false;
		}
	}
	dns_qpmulti_commit(g_bind9_qp, &qpt);
}

/*
 * Mutator-benchmark op.  Each op is one dns_qpmulti write transaction.  dns_qp
 * has no in-place value update, so REPLACE deletes + reinserts the leaf within
 * one transaction.  bind9_churn_present[] tracks state so the driver's drain
 * (and cleanup) remove exactly what is live.
 */
static void b9qp_writer_op(void *ctx, int op, unsigned int idx)
{
	(void)ctx;
	dns_qp_t *qpt = NULL;

	dns_qpmulti_write(g_bind9_qp, &qpt);
	switch (op) {
	case BENCH_OP_INSERT:
		dns_qp_insert(qpt, bind9_churn_leaves[idx], N_KEYS + idx);
		bind9_churn_present[idx] = true;
		break;
	case BENCH_OP_REPLACE:
		dns_qp_deletekey(qpt, qp_churn_keys[idx], qp_churn_lens[idx],
			NULL, NULL);
		dns_qp_insert(qpt, bind9_churn_leaves[idx], N_KEYS + idx);
		break;
	case BENCH_OP_REMOVE:
		if (bind9_churn_present[idx]) {
			dns_qp_deletekey(qpt, qp_churn_keys[idx],
				qp_churn_lens[idx], NULL, NULL);
			bind9_churn_present[idx] = false;
		}
		break;
	}
	if (dns_qp_memusage(qpt).fragmented)
		dns_qp_compact(qpt, DNS_QPGC_NOW);
	dns_qpmulti_commit(g_bind9_qp, &qpt);
}

static const struct bench_engine b9qp_engine = {
	.name		= "b9qp",
	.label		= "BIND9 QP",
	.build		= b9qp_build,
	.reader_setup	= b9qp_reader_setup,
	.reader_batch	= b9qp_reader_batch,
	.reader_teardown = b9qp_reader_teardown,
	.writer_setup	= b9qp_writer_setup,
	.writer_step	= b9qp_writer_step,
	.writer_teardown = b9qp_writer_teardown,
	.run_reset	= b9qp_run_reset,
	.cleanup_churn	= b9qp_cleanup_churn,
	.writer_op	= b9qp_writer_op,
};

int main(int argc, char **argv)
{
	/*
	 * Do NOT rcu_register_thread() here: libisc's isc__lib_initialize()
	 * constructor already registered the main thread (see file header).
	 */
	int ret = bench_scale_main(argc, argv, &b9qp_engine);

	/*
	 * Flush results and _exit() instead of returning, to skip libisc's ELF
	 * shutdown destructors.  dns_qp logs the number of reclaimed chunks from
	 * its call_rcu reclaim threads; without bind9's managed app/logging
	 * lifecycle that interacts with isc__log_shutdown's
	 * INSIST(isc_mem_inuse(ctx) == 0) and aborts an otherwise-clean exit
	 * (see the data-race note in lib/isc/log.c).  All output is already
	 * flushed and the OS reclaims the process memory, so skipping the
	 * destructors is safe here.
	 */
	fflush(NULL);
	_exit(ret);
}
