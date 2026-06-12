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

#include <math.h>
#include <numa.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <isc/async.h>
#include <isc/barrier.h>
#include <isc/file.h>
#include <isc/lib.h>
#include <isc/list.h>
#include <isc/log.h>
#include <isc/loop.h>
#include <isc/magic.h>
#include <isc/mem.h>
#include <isc/mutex.h>
#include <isc/os.h>
#include <isc/random.h>
#include <isc/refcount.h>
#include <isc/rwlock.h>
#include <isc/thread.h>
#include <isc/tid.h>
#include <isc/time.h>
#include <isc/timer.h>
#include <isc/urcu.h>
#include <isc/util.h>
#include <isc/uv.h>

#include <urcu/fractal-trie.h>

#include <dns/fixedname.h>
#include <dns/lib.h>
#include <dns/qp.h>
#include <dns/types.h>

#include "dns/name.h"
#include "qp_p.h"

#include <tests/qp.h>

#include "bench_topology.h"

#define ITEM_COUNT	 ((size_t)1000000)
#define RUNTIME		 (0.25 * NS_PER_SEC)
#define MAX_OPS_PER_LOOP (1 << 10)

#define VERBOSE 0
#define ZIPF	0

#if VERBOSE
#define TRACE(fmt, ...)                                                  \
	isc_log_write(DNS_LOGCATEGORY_DATABASE, DNS_LOGMODULE_QP,        \
		      ISC_LOG_DEBUG(7), "%s:%d:%s():t%" PRItid ": " fmt, \
		      __FILE__, __LINE__, __func__, isc_tid(), ##__VA_ARGS__)
#else
#define TRACE(...)
#endif

#if ZIPF
/*
 * Zipf rejection sampling derived from code by Jason Crease
 * https://jasoncrease.medium.com/rejection-sampling-the-zipf-distribution-6b359792cffa
 */
static uint32_t
rand_zipf(uint32_t max, double skew) {
	double s = skew;
	double t = (pow(max, 1 - s) - s) / (1 - s);
	for (;;) {
		double p = t * (double)isc_random32() / UINT32_MAX;
		double invB = p <= 1 ? p : pow(p * (1 - s) + s, 1 / (1 - s));
		uint32_t sample = (uint32_t)(invB + 1);
		double ratio = sample <= 1 ? pow(sample, -s)
					   : pow(sample, -s) / pow(invB, -s);
		if (ratio > (double)isc_random32() / UINT32_MAX) {
			return sample - 1;
		}
	}
}
#endif

static struct {
	size_t len;
	bool present;
	dns_qpkey_t key;
} *item;

static void
item_refcount(void *ctx, void *pval, uint32_t ival) {
	UNUSED(ctx);
	UNUSED(pval);
	UNUSED(ival);
}

static size_t
item_makekey(dns_qpkey_t key, void *ctx, void *pval, uint32_t ival) {
	UNUSED(ctx);
	UNUSED(pval);
	memmove(key, item[ival].key, item[ival].len);
	return item[ival].len;
}

static void
benchname(void *ctx, char *buf, size_t size) {
	UNUSED(ctx);
	strlcpy(buf, "bench", size);
}

const dns_qpmethods_t item_methods = {
	item_refcount,
	item_refcount,
	item_makekey,
	benchname,
};

static struct cds_ft *g_ft;
static pthread_mutex_t g_ft_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_ft_skip_mode = true;
static uint32_t g_mut_pace_us = 0;
static uint32_t g_read_hot_size = 0;
static uint32_t g_read_hot_pct = 100;

static inline uint32_t
read_pick_idx(uint32_t max_item) {
	if (g_read_hot_size > 0 &&
	    (g_read_hot_pct >= 100 ||
	     isc_random_uniform(100) < g_read_hot_pct)) {
		uint32_t cap = g_read_hot_size < max_item ? g_read_hot_size
							  : max_item;
		return isc_random_uniform(cap);
	}
	return isc_random_uniform(max_item);
}

struct ft_entry {
	struct cds_ft_node ft_node;
	struct rcu_head rcu;	/* defers node-memory reuse past a grace period */
	uint32_t idx;		/* index into item[] */
	bool reclaiming;	/* removed; memory not reusable until the GP ends */
};

static struct ft_entry *ft_entries;  /* array of ITEM_COUNT entries */

/*
 * After cds_ft_remove() unchains a node, concurrent rcu readers may still
 * dereference it until a grace period elapses, so its caller-owned memory must
 * not be reused (zeroed + reinserted) before then.  The mutate path call_rcu's
 * this callback on remove; it zeroes the node and clears @reclaiming once the
 * grace period has passed, and refuses to reinsert an index while reclaiming is
 * set -- so a removed node is never written under a live reader.
 */
static void
ft_entry_reclaim(struct rcu_head *head) {
	struct ft_entry *e = caa_container_of(head, struct ft_entry, rcu);

	memset(&e->ft_node, 0, sizeof(e->ft_node));
	__atomic_store_n(&e->reclaiming, false, __ATOMIC_RELEASE);
}

static void
init_items(isc_mem_t *mctx) {
	const char *filename = getenv("DNS_NAMES_FILE");
	isc_result_t result;
	off_t fileoff;
	FILE *fp = NULL;
	size_t filesize, count = 0;
	char *filetext = NULL, *pos = NULL, *file_end = NULL;
	uint64_t start;

	if (filename == NULL) {
		filename = "tests/bench/names.csv";
	}

	start = isc_time_monotonic();

	result = isc_file_getsize(filename, &fileoff);
	if (result != ISC_R_SUCCESS) {
		fprintf(stderr, "stat(%s): %s\n", filename,
			isc_result_totext(result));
		exit(EXIT_FAILURE);
	}
	filesize = (size_t)fileoff;

	filetext = isc_mem_get(mctx, filesize + 1);
	fp = fopen(filename, "r");
	if (fp == NULL || fread(filetext, 1, filesize, fp) < filesize) {
		fprintf(stderr, "read(%s): %s\n", filename, strerror(errno));
		exit(EXIT_FAILURE);
	}
	fclose(fp);
	filetext[filesize] = '\0';

	item = isc_mem_callocate(mctx, ITEM_COUNT, sizeof(*item));

	pos = filetext;
	file_end = pos + filesize;
	while (pos < file_end && count < ITEM_COUNT) {
		char *domain = NULL, *newline = NULL;
		size_t len;
		dns_fixedname_t fixed;
		dns_name_t *name;
		isc_buffer_t buffer;

		/* skip "<idx>," */
		pos += strspn(pos, "0123456789");
		if (*pos != ',') {
			break;
		}
		pos++;

		domain = pos;
		pos += strcspn(pos, "\r\n");
		if (*pos == '\0') {
			break;
		}
		newline = pos;
		pos += strspn(pos, "\r\n");
		len = newline - domain;
		domain[len] = '\0';

		name = dns_fixedname_initname(&fixed);
		isc_buffer_init(&buffer, domain, len);
		isc_buffer_add(&buffer, len);
		result = dns_name_fromtext(name, &buffer, dns_rootname, 0);
		if (result != ISC_R_SUCCESS) {
			continue;
		}

		item[count].len = dns_qpkey_fromname(item[count].key, name,
						     DNS_DBNAMESPACE_NORMAL);
		count++;
	}

	isc_mem_put(mctx, filetext, filesize + 1);

	if (count != ITEM_COUNT) {
		fprintf(stderr,
			"%s: loaded %zu names, expected %zu\n",
			filename, count, (size_t)ITEM_COUNT);
		exit(EXIT_FAILURE);
	}

	double time = (double)(isc_time_monotonic() - start) / NS_PER_SEC;
	printf("%f sec to load %zu DNS names from %s, %f/sec\n", time,
	       count, filename, count / time);
}

static void
init_ft(void) {
	struct cds_ft_group_attr *group_attr;
	struct cds_ft_group *group;
	uint64_t start;
	size_t count = 0;

	start = isc_time_monotonic();

	ft_entries = calloc(ITEM_COUNT, sizeof(*ft_entries));
	INSIST(ft_entries != NULL);

	cds_ft_group_attr_create(&group_attr);
	cds_ft_group_attr_set_max_key_len(group_attr, sizeof(dns_qpkey_t));
	{
		const char *no_skip = getenv("FT_NO_SKIP_COMPRESSED");
		g_ft_skip_mode = !(no_skip && *no_skip == '1');
		/*
		 * Skip-compressed (the old CDS_FT_FLAG_SKIP_COMPRESSED) is now
		 * the SPECULATIVE lookup optimization, which is the default;
		 * precise is EAGER.  The read path pairs each with its matching
		 * lookup function below (candidate vs eager).
		 */
		cds_ft_group_attr_set_lookup_optimization(group_attr,
			g_ft_skip_mode ? CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE
				       : CDS_FT_LOOKUP_OPTIMIZE_EAGER);
		printf("FT mode: %s\n", g_ft_skip_mode ? "speculative (ft_cand)"
						        : "eager (precise)");
	}
	cds_ft_group_create(group_attr, &group);
	cds_ft_group_attr_destroy(group_attr);
	/*
	 * The per-instance collapse/compress tuning knobs
	 * (cds_ft_attr_set_collapse_threshold / _collapse_scan_mul /
	 * _compress_scan_mul + CDS_FT_COLLAPSE_THRESHOLD_DISABLED) were removed
	 * from the FT API -- those heuristics are internal now -- so create the
	 * trie with default per-instance attributes.
	 */
	cds_ft_create(group, NULL, &g_ft);

	for (size_t i = 0; i < ITEM_COUNT; i++) {
		if (!item[i].present) {
			continue;
		}
		ft_entries[i].idx = i;
		struct cds_ft_node *result;
		cds_ft_insert_unique(g_ft, item[i].key, item[i].len,
				     &ft_entries[i].ft_node, &result);
		count++;
	}

	/*
	 * Drain the build's deferred (call_rcu) frees before the FT read
	 * window so it times a settled internal-node arena rather than peak
	 * build residency -- the same drain-before-timing discipline as
	 * load-names and the read/write scale bench.
	 */
	rcu_barrier();

	double time = (double)(isc_time_monotonic() - start) / NS_PER_SEC;
	printf("FT: %f sec to load %zu items, %f/sec\n", time, count,
	       count / time);
}

static void
init_logging(void) {
#if VERBOSE
	isc_log_setdebuglevel(7);
#endif
	isc_logconfig_t *logconfig = isc_logconfig_get();
	isc_log_createandusechannel(
		logconfig, "default_stderr", ISC_LOG_TOFILEDESC,
		ISC_LOG_DYNAMIC, ISC_LOGDESTINATION_STDERR,
		ISC_LOG_PRINTPREFIX | ISC_LOG_PRINTTIME | ISC_LOG_ISO8601,
		ISC_LOGCATEGORY_DEFAULT, ISC_LOGMODULE_DEFAULT);
}

static void
collect(void *);

struct thread_args {
	struct bench_state *bctx; /* (in) */
	isc_barrier_t *barrier;	  /* (in) */
	isc_job_t job;		  /* (in) */
	isc_job_cb cb;		  /* (in) */
	dns_qpmulti_t *multi;	  /* (in) */
	double zipf_skew;	  /* (in) */
	uint32_t max_item;	  /* (in) */
	uint32_t ops_per_tx;	  /* (in) */
	uint32_t tx_per_loop;	  /* (in) */
	uint32_t absent;	  /* items not found or inserted (out) */
	uint32_t present;	  /* items found or deleted (out) */
	uint32_t compactions;	  /* (out) */
	uint64_t transactions;	  /* (out) */
	isc_nanosecs_t worked;	  /* (out) */
	isc_nanosecs_t start;	  /* (out) */
	isc_nanosecs_t stop;	  /* (out) */
};

static void
first_loop(void *varg) {
	struct thread_args *args = varg;
	isc_loop_t *loop = isc_loop();

	isc_job_run(loop, &args->job, args->cb, args);

	isc_barrier_wait(args->barrier);
	args->start = isc_time_monotonic();
}

static void
next_loop(struct thread_args *args, isc_nanosecs_t start) {
	isc_nanosecs_t stop = isc_time_monotonic();

	args->worked += stop - start;
	args->stop = stop;
	if (args->stop - args->start < RUNTIME) {
		isc_job_run(isc_loop(), &args->job, args->cb, args);
		return;
	}
	isc_async_run(isc_loop_main(), collect, args);
}

#if ZIPF
static void
read_zipf(void *varg) {
	struct thread_args *args = varg;
	isc_nanosecs_t start;

	/* outside time because it is v slow */
	uint32_t r[args->tx_per_loop][args->ops_per_tx];
	for (uint32_t tx = 0; tx < args->tx_per_loop; tx++) {
		for (uint32_t op = 0; op < args->ops_per_tx; op++) {
			r[tx][op] = rand_zipf(args->max_item, args->zipf_skew);
		}
	}

	start = isc_time_monotonic();
	for (uint32_t tx = 0; tx < args->tx_per_loop; tx++) {
		args->transactions++;
		dns_qpread_t qp;
		dns_qpmulti_query(args->multi, &qp);
		for (uint32_t op = 0; op < args->ops_per_tx; op++) {
			uint32_t i = r[tx][op];
			isc_result_t result = dns_qp_getkey(
				&qp, item[i].key, item[i].len, NULL, NULL);
			if (result == ISC_R_SUCCESS) {
				args->present++;
			} else {
				args->absent++;
			}
		}
		dns_qpread_destroy(args->multi, &qp);
	}
	next_loop(args, start);
}
#else
#define read_zipf read_transactions
#endif

static void
read_transactions(void *varg) {
	struct thread_args *args = varg;
	isc_nanosecs_t start = isc_time_monotonic();

	for (uint32_t tx = 0; tx < args->tx_per_loop; tx++) {
		args->transactions++;
		dns_qpread_t qp;
		dns_qpmulti_query(args->multi, &qp);
		for (uint32_t op = 0; op < args->ops_per_tx; op++) {
			uint32_t i = read_pick_idx(args->max_item);
			isc_result_t result = dns_qp_getkey(
				&qp, item[i].key, item[i].len, NULL, NULL);
			if (result == ISC_R_SUCCESS) {
				args->present++;
			} else {
				args->absent++;
			}
		}
		dns_qpread_destroy(args->multi, &qp);
	}
	next_loop(args, start);
}

static void
mutate_transactions(void *varg) {
	struct thread_args *args = varg;
	isc_nanosecs_t start = isc_time_monotonic();

	for (uint32_t tx = 0; tx < args->tx_per_loop; tx++) {
		dns_qp_t *qp = NULL;
		dns_qpmulti_write(args->multi, &qp);
		for (uint32_t op = 0; op < args->ops_per_tx; op++) {
			uint32_t i = isc_random_uniform(args->max_item);
			if (item[i].present) {
				isc_result_t result = dns_qp_deletekey(
					qp, item[i].key, item[i].len, NULL,
					NULL);
				INSIST(result == ISC_R_SUCCESS);
				item[i].present = false;
				args->present++;
			} else {
				isc_result_t result =
					dns_qp_insert(qp, &item[i], i);
				INSIST(result == ISC_R_SUCCESS);
				item[i].present = true;
				args->absent++;
			}
		}
		/*
		 * We would normally use DNS_QPGC_MAYBE, but here we do the
		 * fragmented check ourself so we can count compactions
		 */
		if (dns_qp_memusage(qp).fragmented) {
			dns_qp_compact(qp, DNS_QPGC_NOW);
			args->compactions++;
		}
		dns_qpmulti_commit(args->multi, &qp);
		args->transactions++;
		if (g_mut_pace_us) {
			usleep(g_mut_pace_us);
		}
	}
	next_loop(args, start);
}

static void
ft_read_transactions(void *varg) {
	struct thread_args *args = varg;
	isc_nanosecs_t start = isc_time_monotonic();

	for (uint32_t tx = 0; tx < args->tx_per_loop; tx++) {
		args->transactions++;
		rcu_read_lock();
		for (uint32_t op = 0; op < args->ops_per_tx; op++) {
			uint32_t i = read_pick_idx(args->max_item);
			struct cds_ft_node *found;
			if (g_ft_skip_mode) {
				cds_ft_lookup_candidate_key(g_ft, item[i].key,
							    item[i].len, item[i].len, &found);
			} else {
				cds_ft_eager_lookup_key(g_ft, item[i].key,
							item[i].len, item[i].len, &found);
			}
			if (found) {
				args->present++;
			} else {
				args->absent++;
			}
		}
		rcu_read_unlock();
	}
	next_loop(args, start);
}

static void
ft_mutate_transactions(void *varg) {
	struct thread_args *args = varg;
	isc_nanosecs_t start = isc_time_monotonic();

	for (uint32_t tx = 0; tx < args->tx_per_loop; tx++) {
		for (uint32_t op = 0; op < args->ops_per_tx; op++) {
			uint32_t i = isc_random_uniform(args->max_item);
			pthread_mutex_lock(&g_ft_mutex);
			if (__atomic_load_n(&ft_entries[i].reclaiming,
					    __ATOMIC_ACQUIRE)) {
				/*
				 * Node still within its post-remove grace
				 * period: its memory is not yet reusable, so
				 * skip this op rather than touch a node a
				 * reader may still hold.
				 */
				pthread_mutex_unlock(&g_ft_mutex);
				continue;
			}
			if (item[i].present) {
				struct cds_ft_iter *iter;
				cds_ft_iter_create(g_ft, &iter);
				cds_ft_iter_set_key(iter, item[i].key,
						    item[i].len);
				if (cds_ft_lookup(g_ft, iter) ==
				    CDS_FT_STATUS_OK) {
					cds_ft_remove(g_ft, iter,
						      &ft_entries[i].ft_node);
					item[i].present = false;
					args->present++;
					/*
					 * Defer the node-memory reuse past a
					 * grace period: rcu readers may still
					 * hold the just-removed node, so the
					 * zero + any reinsert must wait.
					 * ft_entry_reclaim() zeroes it and
					 * clears reclaiming once the GP ends.
					 */
					__atomic_store_n(
						&ft_entries[i].reclaiming, true,
						__ATOMIC_RELEASE);
					call_rcu(&ft_entries[i].rcu,
						 ft_entry_reclaim);
				}
				cds_ft_iter_destroy(iter);
			} else {
				struct cds_ft_node *result;
				if (cds_ft_insert_unique(
					    g_ft, item[i].key, item[i].len,
					    &ft_entries[i].ft_node,
					    &result) == CDS_FT_STATUS_OK)
				{
					item[i].present = true;
					args->absent++;
				}
			}
			pthread_mutex_unlock(&g_ft_mutex);
		}
		args->transactions++;
		if (g_mut_pace_us) {
			usleep(g_mut_pace_us);
		}
	}
	next_loop(args, start);
}

enum benchmode {
	init,
	vary_max_items_rw,
	vary_max_items_ro,
	vary_mut_read,
	vary_read_only,
	vary_mut_ops_per_tx,
	vary_mut_tx_per_loop,
	vary_read_ops_per_tx_rw,
	vary_read_ops_per_tx_ro,
	vary_read_tx_per_loop_rw,
	vary_read_tx_per_loop_ro,
	vary_zipf_skew,
	vary_ft_mut_read,
	vary_ft_read_only,
};

struct bench_state {
	isc_mem_t *mctx;
	isc_barrier_t barrier;
	dns_qpmulti_t *multi;
	enum benchmode mode;
	size_t bytes;
	size_t qp_bytes;
	size_t qp_items;
	isc_nanosecs_t load_time;
	uint32_t nloops;
	uint32_t waiting;
	uint32_t max_item;
	uint32_t mutate;
	uint32_t mut_ops_per_tx;
	uint32_t mut_tx_per_loop;
	uint32_t readers;
	uint32_t read_ops_per_tx;
	uint32_t read_tx_per_loop;
	double zipf_skew;
	struct thread_args thread[];
};

static void
load_multi(struct bench_state *bctx) {
	dns_qp_t *qp = NULL;
	size_t count = 0;
	uint64_t start;

	dns_qpmulti_create(bctx->mctx, &item_methods, NULL, &bctx->multi);

	/* initial contents of the trie */
	start = isc_time_monotonic();
	dns_qpmulti_update(bctx->multi, &qp);
	for (size_t i = 0; i < bctx->max_item; i++) {
		if (isc_random_uniform(2) == 0) {
			item[i].present = false;
			continue;
		}
		INSIST(dns_qp_insert(qp, &item[i], i) == ISC_R_SUCCESS);
		item[i].present = true;
		count++;
	}
	dns_qp_compact(qp, DNS_QPGC_ALL);
	dns_qpmulti_commit(bctx->multi, &qp);

	bctx->load_time = isc_time_monotonic() - start;
	bctx->qp_bytes = dns_qpmulti_memusage(bctx->multi).bytes;
	bctx->qp_items = count;
}

static void
tsv_header(void) {
	printf("runtime\t");
	printf("elapsed\t");
	printf(" load s\t");
	printf(" B/item\t");
	printf("  items\t");

	printf("    mut\t");
	printf("tx/loop\t");
	printf(" ops/tx\t");
	printf("     gc\t");
	printf("   txns\t");
	printf("    ops\t");
	printf(" work s\t");
	printf("txns/us\t");
	printf(" ops/us\t");

	printf("   read\t");
	printf("tx/loop\t");
	printf(" ops/tx\t");
	printf("  Ktxns\t");
	printf("   Kops\t");
	printf(" work s\t");
	printf("txns/us\t");
	printf(" ops/us\t");
	printf("    raw\t");
	printf("   loop\n");
}

/*
 * This function sets up the parameters for each benchmark run and
 * dispatches the work to the event loops. Each run is part of a
 * series, where most of the parameters are fixed and one parameter is
 * varied. The layout here is somewhat eccentric, in order to keep
 * each series together.
 *
 * A series starts with an `init` block, which sets up the constant
 * parameters and the variable parameter for the first run. Following
 * the `init` block is a `case` label which adjusts the variable
 * parameter for each subsequent run in the series, and checks when
 * the series is finished. At the end of the series, we `goto` the
 * `init` label for the next series.
 */
static void
dispatch(struct bench_state *bctx) {
	switch (bctx->mode) {
	case init:
		goto init_max_items_rw;

	fini:
		dns_qpmulti_destroy(&bctx->multi);
		isc_mem_putanddetach(&bctx->mctx, bctx, bctx->bytes);
		isc_loopmgr_shutdown();
		return;

	init_max_items_rw:
		bctx->mode = vary_max_items_rw;
		printf("\n");
		printf("vary size of trie\n");
		tsv_header();
		bctx->mutate = 1;
		bctx->readers = bctx->nloops - 1;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		bctx->max_item = 10;
		load_multi(bctx);
		break;

	case vary_max_items_rw:
		if (bctx->max_item == ITEM_COUNT) {
			goto init_max_items_ro;
		} else {
			dns_qpmulti_destroy(&bctx->multi);
			bctx->max_item *= 10;
			load_multi(bctx);
		}
		break;

	init_max_items_ro:
		bctx->mode = vary_max_items_ro;
		printf("\n");
		printf("vary size of trie (readonly)\n");
		tsv_header();
		bctx->mutate = 0;
		bctx->readers = bctx->nloops;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		dns_qpmulti_destroy(&bctx->multi);
		bctx->max_item = 10;
		load_multi(bctx);
		break;
	case vary_max_items_ro:
		if (bctx->max_item == ITEM_COUNT) {
			goto init_zipf_skew;
		} else {
			dns_qpmulti_destroy(&bctx->multi);
			bctx->max_item *= 10;
			load_multi(bctx);
		}
		break;

	init_zipf_skew:
		bctx->mode = vary_zipf_skew;
		printf("\n");
		printf("vary zipf skew (readonly) "
		       " [ cache friendliness? ]\n");
		tsv_header();
		bctx->mutate = 0;
		bctx->readers = 0;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		bctx->zipf_skew = 0.01;
		/* dumb hack */
		bctx->load_time = bctx->zipf_skew * NS_PER_SEC;
		break;
	case vary_zipf_skew:
		bctx->zipf_skew += 0.1;
		bctx->load_time = bctx->zipf_skew * NS_PER_SEC;
		if (bctx->zipf_skew >= 1.0) {
			bctx->zipf_skew = 0.0;
			bctx->load_time = 0;
			goto init_mut_read;
		}
		break;

	init_mut_read:
		bctx->mode = vary_mut_read;
		printf("\n");
		printf("vary mutate / read threads "
		       "[ read perf per thread should be flat ]\n");
		tsv_header();
		bctx->mutate = bctx->nloops - 1;
		bctx->readers = 1;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		break;
	case vary_mut_read:
		if (bctx->readers >= bctx->nloops - 1) {
			goto init_read_only;
		} else {
			uint32_t next_readers = bctx->readers * 2;
			if (next_readers > bctx->nloops - 1)
				next_readers = bctx->nloops - 1;
			bctx->mutate = bctx->nloops - next_readers;
			bctx->readers = next_readers;
		}
		break;

	init_read_only:
		bctx->mode = vary_read_only;
		printf("\n");
		printf("vary read threads "
		       "[ read perf per thread should be flat ]\n");
		tsv_header();
		bctx->mutate = 0;
		bctx->readers = 1;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		break;
	case vary_read_only:
		if (bctx->readers >= bctx->nloops) {
			goto init_ft_mut_read;
		} else {
			uint32_t next = bctx->readers * 2;
			if (next > bctx->nloops)
				next = bctx->nloops;
			bctx->readers = next;
		}
		break;

	init_mut_ops_per_tx:
		bctx->mode = vary_mut_ops_per_tx;
		printf("\n");
		printf("vary mutate operations per transaction "
		       "[ mutate activity affects read perf? ]\n");
		tsv_header();
		bctx->mutate = 1;
		bctx->readers = bctx->nloops - 1;
		bctx->mut_ops_per_tx = 1;
		bctx->mut_tx_per_loop = 1;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		break;
	case vary_mut_ops_per_tx:
		if (bctx->mut_ops_per_tx * bctx->mut_tx_per_loop ==
		    MAX_OPS_PER_LOOP)
		{
			goto init_mut_tx_per_loop;
		} else {
			bctx->mut_ops_per_tx *= 2;
		}
		break;

	init_mut_tx_per_loop:
		bctx->mode = vary_mut_tx_per_loop;
		printf("\n");
		printf("vary mutate transactions per loop "
		       "[ mutate activity affects read perf? ]\n");
		tsv_header();
		bctx->mutate = 1;
		bctx->readers = bctx->nloops - 1;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 1;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		break;
	case vary_mut_tx_per_loop:
		if (bctx->mut_ops_per_tx * bctx->mut_tx_per_loop ==
		    MAX_OPS_PER_LOOP)
		{
			goto init_read_tx_per_loop_rw;
		} else {
			bctx->mut_tx_per_loop *= 2;
		}
		break;

	init_read_tx_per_loop_rw:
		bctx->mode = vary_read_tx_per_loop_rw;
		printf("\n");
		printf("vary read transactions per loop "
		       "[ loop overhead? ]\n");
		tsv_header();
		bctx->mutate = 1;
		bctx->readers = bctx->nloops - 1;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 4;
		bctx->read_tx_per_loop = 1;
		break;
	case vary_read_tx_per_loop_rw:
		if (bctx->read_ops_per_tx * bctx->read_tx_per_loop ==
		    MAX_OPS_PER_LOOP)
		{
			goto init_read_tx_per_loop_ro;
		} else {
			bctx->read_tx_per_loop *= 2;
		}
		break;

	init_read_tx_per_loop_ro:
		bctx->mode = vary_read_tx_per_loop_ro;
		printf("\n");
		printf("vary read transactions per loop (readonly) "
		       "[ loop overhead? ]\n");
		tsv_header();
		bctx->mutate = 0;
		bctx->readers = bctx->nloops;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 4;
		bctx->read_tx_per_loop = 1;
		break;
	case vary_read_tx_per_loop_ro:
		if (bctx->read_ops_per_tx * bctx->read_tx_per_loop ==
		    MAX_OPS_PER_LOOP)
		{
			goto init_read_ops_per_tx_rw;
		} else {
			bctx->read_tx_per_loop *= 2;
		}
		break;

	init_read_ops_per_tx_rw:
		bctx->mode = vary_read_ops_per_tx_rw;
		printf("\n");
		printf("vary read operations per transaction "
		       " [ transaction overhead should be small ]\n");
		tsv_header();
		bctx->mutate = 1;
		bctx->readers = bctx->nloops - 1;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 1;
		bctx->read_tx_per_loop = MAX_OPS_PER_LOOP;
		break;
	case vary_read_ops_per_tx_rw:
		if (bctx->read_ops_per_tx == MAX_OPS_PER_LOOP) {
			goto init_read_ops_per_tx_ro;
		} else {
			bctx->read_ops_per_tx *= 2;
			bctx->read_tx_per_loop /= 2;
		}
		break;

	init_read_ops_per_tx_ro:
		bctx->mode = vary_read_ops_per_tx_ro;
		printf("\n");
		printf("vary read operations per transaction (readonly) "
		       " [ transaction overhead should be small ]\n");
		tsv_header();
		bctx->mutate = 0;
		bctx->readers = bctx->nloops;
		bctx->mut_ops_per_tx = 0;
		bctx->mut_tx_per_loop = 0;
		bctx->read_ops_per_tx = 1;
		bctx->read_tx_per_loop = MAX_OPS_PER_LOOP;
		break;
	case vary_read_ops_per_tx_ro:
		if (bctx->read_ops_per_tx == MAX_OPS_PER_LOOP) {
			goto init_ft_mut_read;
		} else {
			bctx->read_ops_per_tx *= 2;
			bctx->read_tx_per_loop /= 2;
		}
		break;

	init_ft_mut_read:
		bctx->mode = vary_ft_mut_read;
		printf("\n");
		printf("vary mutate / read threads (FT) "
		       "[ read perf per thread should be flat ]\n");
		tsv_header();
		init_ft();
		bctx->mutate = bctx->nloops - 1;
		bctx->readers = 1;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		break;
	case vary_ft_mut_read:
		if (bctx->readers >= bctx->nloops - 1) {
			goto init_ft_read_only;
		} else {
			uint32_t next_readers = bctx->readers * 2;
			if (next_readers > bctx->nloops - 1)
				next_readers = bctx->nloops - 1;
			bctx->mutate = bctx->nloops - next_readers;
			bctx->readers = next_readers;
		}
		break;

	init_ft_read_only:
		bctx->mode = vary_ft_read_only;
		printf("\n");
		printf("vary read threads (FT) "
		       "[ read perf per thread should be flat ]\n");
		tsv_header();
		bctx->mutate = 0;
		bctx->readers = 1;
		bctx->mut_ops_per_tx = 4;
		bctx->mut_tx_per_loop = 4;
		bctx->read_ops_per_tx = 32;
		bctx->read_tx_per_loop = 32;
		break;
	case vary_ft_read_only:
		if (bctx->readers >= bctx->nloops) {
			goto fini;
		} else {
			uint32_t next = bctx->readers * 2;
			if (next > bctx->nloops)
				next = bctx->nloops;
			bctx->readers = next;
		}
		break;
	}

	/* dispatch a benchmark run */

	bool zipf = bctx->mutate == 0 && bctx->readers == 0;
	bool ft_mode = (bctx->mode == vary_ft_mut_read ||
			bctx->mode == vary_ft_read_only);
	bctx->waiting = zipf ? bctx->nloops : bctx->readers + bctx->mutate;
	isc_barrier_init(&bctx->barrier, bctx->waiting);
	for (uint32_t t = 0; t < bctx->waiting; t++) {
		bool mut = t < bctx->mutate;
		isc_job_cb cb;
		if (ft_mode) {
			cb = mut ? ft_mutate_transactions
				 : ft_read_transactions;
		} else if (zipf) {
			cb = read_zipf;
		} else if (mut) {
			cb = mutate_transactions;
		} else {
			cb = read_transactions;
		}
		bctx->thread[t] = (struct thread_args){
			.bctx = bctx,
			.barrier = &bctx->barrier,
			.multi = bctx->multi,
			.max_item = bctx->max_item,
			.zipf_skew = bctx->zipf_skew,
			.cb = cb,
			.job = ISC_JOB_INITIALIZER,
			.ops_per_tx = mut ? bctx->mut_ops_per_tx
					  : bctx->read_ops_per_tx,
			.tx_per_loop = mut ? bctx->mut_tx_per_loop
					   : bctx->read_tx_per_loop,
		};
		isc_async_run(isc_loop_get(t), first_loop, &bctx->thread[t]);
	}
}

static void
collect(void *varg) {
	struct thread_args *args = varg;
	struct bench_state *bctx = args->bctx;
	struct thread_args *thread = bctx->thread;
	struct {
		uint64_t worked, txns, ops, compactions;
	} stats[2] = {};
	double load_time = bctx->load_time;
	double elapsed = 0, mut_work, readers, read_work, elapsed_ms;
	uint32_t nloops;
	bool zipf;

	TRACE("collect");

	bctx->waiting--;
	if (bctx->waiting > 0) {
		return;
	}
	isc_barrier_destroy(&bctx->barrier);

	load_time = load_time > 0 ? load_time / (double)NS_PER_SEC : NAN;

	zipf = bctx->mutate == 0 && bctx->readers == 0;
	nloops = zipf ? bctx->nloops : bctx->readers + bctx->mutate;
	for (uint32_t t = 0; t < nloops; t++) {
		struct thread_args *tp = &thread[t];
		elapsed = ISC_MAX(elapsed, tp->stop - tp->start);
		bool mut = t < bctx->mutate;

		stats[mut].worked += tp->worked;
		stats[mut].txns += tp->transactions;
		stats[mut].ops += tp->transactions * tp->ops_per_tx;
		stats[mut].compactions += tp->compactions;
	}

	INSIST(elapsed >= RUNTIME);

	printf("%7.3f\t", RUNTIME / (double)NS_PER_SEC);
	printf("%7.3f\t", elapsed / (double)NS_PER_SEC);
	printf("%7.3f\t", load_time);
	printf("%7.2f\t", (double)bctx->qp_bytes / bctx->qp_items);
	printf("%7u\t", bctx->max_item);

	mut_work = stats[1].worked / (double)US_PER_MS;
	printf("%7u\t", bctx->mutate);
	printf("%7u\t", bctx->mut_tx_per_loop);
	printf("%7u\t", bctx->mut_ops_per_tx);
	printf("%7llu\t", (unsigned long long)stats[1].compactions);
	printf("%7llu\t", (unsigned long long)stats[1].txns);
	printf("%7llu\t", (unsigned long long)stats[1].ops);
	printf("%7.2f\t", stats[1].worked / (double)NS_PER_SEC);
	printf("%7.2f\t", stats[1].txns / mut_work);
	printf("%7.2f\t", stats[1].ops / mut_work);

	readers = zipf ? bctx->nloops - bctx->mutate : bctx->readers;
	read_work = stats[0].worked / (double)US_PER_MS;
	elapsed_ms = elapsed / (double)US_PER_MS;
	printf("%7u\t", bctx->readers);
	printf("%7u\t", bctx->read_tx_per_loop);
	printf("%7u\t", bctx->read_ops_per_tx);
	printf("%7llu\t", (unsigned long long)stats[0].txns / 1000);
	printf("%7llu\t", (unsigned long long)stats[0].ops / 1000);
	printf("%7.2f\t", stats[0].worked / (double)NS_PER_SEC);
	printf("%7.2f\t", stats[0].txns / read_work);
	printf("%7.2f\t", stats[0].ops / read_work);
	printf("%7.2f\t", stats[0].ops * readers / read_work);
	printf("%7.2f\n", stats[0].ops / elapsed_ms);

	dispatch(bctx);
}

static void
startup(void *arg ISC_ATTR_UNUSED) {
	isc_loop_t *loop = isc_loop();
	isc_mem_t *mctx = isc_loop_getmctx(loop);
	uint32_t nloops = isc_loopmgr_nloops();
	size_t bytes = sizeof(struct bench_state) +
		       sizeof(struct thread_args) * nloops;
	struct bench_state *bctx = isc_mem_cget(mctx, 1, bytes);

	*bctx = (struct bench_state){
		.bytes = bytes,
		.nloops = nloops,
	};
	isc_mem_attach(mctx, &bctx->mctx);

	dispatch(bctx);
}

struct ticker {
	isc_mem_t *mctx;
	isc_timer_t *timer;
};

static void
tick(void *varg) {
	/* just make the loop cycle */
	UNUSED(varg);
}

static void
start_ticker(void *varg) {
	struct ticker *ticker = varg;
	isc_loop_t *loop = isc_loop();

	/*
	 * isc_loopmgr does not bind its loops to CPUs, so pin each loop's
	 * thread to one PU per physical core here (this setup runs on the
	 * loop's own thread).  Without it the scheduler floats loops across
	 * SMT siblings, halving SIMD throughput on shared cores and adding
	 * run-to-run variance.  hwloc-driven via the shared bench_topology map.
	 */
	bench_topology_pin(isc_tid());

	isc_timer_create(loop, tick, NULL, &ticker->timer);
	isc_timer_start(ticker->timer, isc_timertype_ticker,
			&(isc_interval_t){
				.seconds = 0,
				.nanoseconds = 1 * NS_PER_MS,
			});
}

static void
stop_ticker(void *varg) {
	struct ticker *ticker = varg;

	isc_timer_stop(ticker->timer);
	isc_timer_destroy(&ticker->timer);
	isc_mem_putanddetach(&ticker->mctx, ticker, sizeof(*ticker));
}

static void
setup_tickers(isc_mem_t *mctx) {
	uint32_t nloops = isc_loopmgr_nloops();
	for (uint32_t i = 0; i < nloops; i++) {
		isc_loop_t *loop = isc_loop_get(i);
		struct ticker *ticker = isc_mem_get(mctx, sizeof(*ticker));
		*ticker = (struct ticker){
			.mctx = isc_mem_ref(mctx),
		};
		isc_loop_setup(loop, start_ticker, ticker);
		isc_loop_teardown(loop, stop_ticker, ticker);
	}
}

int
main(void) {
	setlinebuf(stdout);

	/*
	 * NUMA interleaving, ON BY DEFAULT (BENCH_NUMA_INTERLEAVE=0 to disable).
	 * Spread every allocation the process first-touches -- item[], the FT,
	 * and the qp -- across all NUMA nodes, set BEFORE any allocation, so at
	 * high loop counts spanning sockets no single node's bandwidth
	 * bottlenecks the shared read set and auto-NUMA-balancing does not
	 * thrash a first-touched-on-one-node structure.  Applied uniformly to
	 * qp and FT so the comparison stays fair.  Matches bench_scale_common.
	 */
	{
		const char *ni = getenv("BENCH_NUMA_INTERLEAVE");

		if ((!ni || ni[0] != '0') && numa_available() != -1) {
			numa_set_interleave_mask(numa_all_nodes_ptr);
			fprintf(stderr,
				"NUMA: interleaving allocations across all nodes "
				"(BENCH_NUMA_INTERLEAVE=0 to disable)\n");
		}
	}

	/* Build the loop -> physical-core pin map before the loops spawn. */
	bench_topology_init();

	uint32_t nloops;
	const char *env_workers = getenv("ISC_TASK_WORKERS");

	if (env_workers != NULL) {
		nloops = atoi(env_workers);
	} else {
		nloops = isc_os_ncpus();
	}
	INSIST(nloops > 1);

	{
		const char *pace = getenv("MUT_PACE_US");
		const char *hot_size = getenv("READ_HOT_SIZE");
		const char *hot_pct = getenv("READ_HOT_PCT");
		if (pace) {
			g_mut_pace_us = (uint32_t)atoi(pace);
		}
		if (hot_size) {
			g_read_hot_size = (uint32_t)atoi(hot_size);
		}
		if (hot_pct) {
			g_read_hot_pct = (uint32_t)atoi(hot_pct);
		}
		printf("MUT_PACE_US: %u (0 = unpaced)\n", g_mut_pace_us);
		printf("READ_HOT_SIZE: %u  READ_HOT_PCT: %u "
		       "(0 size = uniform across all)\n",
		       g_read_hot_size, g_read_hot_pct);
	}

	isc_mem_setdestroycheck(isc_g_mctx, true);
	init_logging();
	init_items(isc_g_mctx);

	isc_loopmgr_create(isc_g_mctx, nloops);
	setup_tickers(isc_g_mctx);
	isc_loop_setup(isc_loop_main(), startup, NULL);
	isc_loopmgr_run();
	isc_loopmgr_destroy();

	isc_mem_free(isc_g_mctx, item);
	isc_mem_checkdestroyed(stdout);

	return 0;
}
