// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * probe_local.c -- LD_PRELOAD shim: is a bench's call_rcu() taking the rseq
 * local queue?
 *
 * Interposes urcu_qsbr_call_rcu_data_free() (every bench harness frees its
 * per-writer worker at thread exit) and, before forwarding, prints that crdp's
 * local-queue state through the test hooks a liburcu built
 * --enable-call-rcu-rseq exports.  Aggregated at exit:
 *
 *   probe_local: crdps=N enabled=E local_pushed=L
 *
 * on stderr.  E < N or L == 0 on an arm meant to use the local path = VACUOUS.
 *
 *   cc -O2 -shared -fPIC -o probe_local.so probe_local.c -ldl
 *   LD_PRELOAD=./probe_local.so ./bench ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

struct call_rcu_data;

static unsigned long n_crdp, n_enabled, n_pushed;

void urcu_qsbr_call_rcu_data_free(struct call_rcu_data *crdp)
{
	static void (*real)(struct call_rcu_data *);
	static int (*enabled)(struct call_rcu_data *);
	static unsigned long (*pushed)(struct call_rcu_data *);

	if (!real) {
		real = (void (*)(struct call_rcu_data *))
			dlsym(RTLD_NEXT, "urcu_qsbr_call_rcu_data_free");
		enabled = (int (*)(struct call_rcu_data *))
			dlsym(RTLD_DEFAULT, "urcu_call_rcu_rseq_test_enabled");
		pushed = (unsigned long (*)(struct call_rcu_data *))
			dlsym(RTLD_DEFAULT, "urcu_call_rcu_rseq_test_pushed");
	}
	if (crdp) {
		__atomic_add_fetch(&n_crdp, 1, __ATOMIC_RELAXED);
		if (enabled && enabled(crdp))
			__atomic_add_fetch(&n_enabled, 1, __ATOMIC_RELAXED);
		if (pushed)
			__atomic_add_fetch(&n_pushed, pushed(crdp), __ATOMIC_RELAXED);
	}
	real(crdp);
}

__attribute__((destructor))
static void report(void)
{
	fprintf(stderr, "probe_local: crdps=%lu enabled=%lu local_pushed=%lu\n",
		n_crdp, n_enabled, n_pushed);
}
