// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * probe_maxrss.c -- LD_PRELOAD shim: print the process's peak RSS at exit,
 *
 *   probe_maxrss: <KiB>
 *
 * on stderr.  A destructor and nothing else: no interposition, no shared
 * counter on any hot path (a counting shim's shared atomic capped the dcache
 * churn bench at ~19M calls/s and invented a 3.4x route difference).
 *
 *   cc -O2 -shared -fPIC -o probe_maxrss.so probe_maxrss.c
 */
#include <stdio.h>
#include <sys/resource.h>

__attribute__((destructor))
static void report(void)
{
	struct rusage ru;

	if (!getrusage(RUSAGE_SELF, &ru))
		fprintf(stderr, "probe_maxrss: %ld\n", ru.ru_maxrss);
}
