// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_bench_setup.h -- place a bench harness's SETUP memory on the node of
 * its first worker's CPU.
 *
 * The setup thread first-touches every seeded dentry and the bucket table, so
 * the node it runs on is where they live.  Left to the scheduler it ran on
 * either socket from one session to the next, and a home on the far socket for
 * the lines readers RMW (a directory's rwsem, rename_lock's sequence) cost the
 * seqlock baseline 30-40% of its readdir and reverse-walk rate at 16 readers,
 * while the txn readers, which only read shared lines, moved 2-5%: sweeps of the
 * same code disagreed by 1.4x (2026-09-30).  Where lookups miss to DRAM (the
 * probing panels) the far home cost EVERY engine up to 2x.
 *
 * So the setup allocates under a preferred-node memory policy, and it has to be
 * in force from exec on: the allocator's first chunks are faulted in before
 * main() runs, on transparent huge pages, and later dentries are carved from
 * them (a harness launched on the far socket that only pinned itself inside
 * main() still kept 4.3 MB there and still ran at the far-socket rate).  A
 * memory policy survives execve(), so: set it, re-exec once, and reset it to
 * the default before the workers are spawned, so their own allocations stay
 * local to them exactly as before.
 *
 * ⛔ NOT by pinning the CPU before the exec.  That was the first version, and
 * jemalloc sizes its arenas from the affinity mask it finds at initialization:
 * started on one CPU it made ONE arena (opt.narenas 1, against 1536 with the
 * full mask) for 200 threads, and every allocation-heavy measurement serialized
 * on it -- bucket-lock renames ran at half speed.  The CPU mask is left alone.
 */
#ifndef DCACHE_BENCH_SETUP_H
#define DCACHE_BENCH_SETUP_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define DC_MPOL_DEFAULT		0	/* <linux/mempolicy.h> MPOL_DEFAULT */
#define DC_MPOL_PREFERRED	1	/* <linux/mempolicy.h> MPOL_PREFERRED */
#define DC_MPOL_MAXNODE		1024

/* The NUMA node of @cpu, from sysfs (cpuN/nodeM), or -1. */
static inline int dc_cpu_node(int cpu)
{
	char path[64];
	struct dirent *de;
	DIR *d;
	int node = -1;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);
	d = opendir(path);
	if (!d)
		return -1;
	while ((de = readdir(d)) != NULL)
		if (!strncmp(de->d_name, "node", 4) &&
		    sscanf(de->d_name + 4, "%d", &node) == 1)
			break;
	closedir(d);
	return node;
}

static inline long dc_set_mempolicy(int mode, int node)
{
	unsigned long mask[DC_MPOL_MAXNODE / (8 * sizeof(unsigned long))];

	memset(mask, 0, sizeof(mask));
	if (node >= 0)
		mask[node / (8 * sizeof(unsigned long))] |=
			1UL << (node % (8 * sizeof(unsigned long)));
	return syscall(SYS_set_mempolicy, mode, node >= 0 ? mask : NULL,
		       node >= 0 ? DC_MPOL_MAXNODE + 1 : 0);
}

/* Call once, after argv parsing and before any setup allocation. */
static inline void dc_bench_setup_on_cpu(int cpu, char **argv)
{
	static const char env[] = "DC_BENCH_SETUP_NODE";
	const char *done = getenv(env);
	int node = dc_cpu_node(cpu);
	char buf[16];

	if (node < 0)
		return;			/* no NUMA topology: nothing to place */
	if (done && atoi(done) == node)
		return;			/* this image started under the policy */
	if (dc_set_mempolicy(DC_MPOL_PREFERRED, node) != 0)
		return;			/* cannot set it: placement as before */
	snprintf(buf, sizeof(buf), "%d", node);
	setenv(env, buf, 1);
	execv("/proc/self/exe", argv);
	perror("dc_bench_setup_on_cpu: execv (policy set, not re-executed)");
}

/* Call once the setup is built, BEFORE spawning any thread: threads inherit
 * the policy of the thread that creates them. */
static inline void dc_bench_setup_done(void)
{
	(void) dc_set_mempolicy(DC_MPOL_DEFAULT, -1);
}

#endif /* DCACHE_BENCH_SETUP_H */
