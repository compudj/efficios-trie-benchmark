/*
 * Topology-aware worker pinning -- see bench_topology.h.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE	/* sched_setaffinity, CPU_SET, cpu_set_t */
#endif
#include "bench_topology.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include <hwloc.h>

/*
 * Worker index -> OS logical CPU.  Entry i is the (i / ncores)-th PU of core
 * (i % ncores), i.e. one PU per core across the first @ncores entries, then the
 * siblings.  Built once by bench_topology_init(); read-only afterward.  NULL
 * (hwloc failed / not yet built) means "use the identity map".
 */
static int *g_pu_map;
static int g_pu_count;

void
bench_topology_init(void)
{
	hwloc_topology_t topo;
	int ncores, npus, maxsmt = 0, idx = 0;

	if (g_pu_map != NULL)
		return;			/* already built */

	if (hwloc_topology_init(&topo) != 0 ||
	    hwloc_topology_load(topo) != 0) {
		fprintf(stderr,
			"bench: hwloc topology load failed; identity CPU pinning\n");
		return;
	}

	ncores = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE);
	npus = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_PU);
	if (ncores <= 0 || npus <= 0) {
		fprintf(stderr,
			"bench: hwloc found %d cores / %d PUs; identity CPU pinning\n",
			ncores, npus);
		hwloc_topology_destroy(topo);
		return;
	}

	g_pu_map = malloc((size_t) npus * sizeof(*g_pu_map));
	if (g_pu_map == NULL) {
		hwloc_topology_destroy(topo);
		return;
	}

	/* Widest SMT degree across cores (cores may be asymmetric). */
	for (int i = 0; i < ncores; i++) {
		hwloc_obj_t core = hwloc_get_obj_by_type(topo, HWLOC_OBJ_CORE, i);
		int w = hwloc_bitmap_weight(core->cpuset);

		if (w > maxsmt)
			maxsmt = w;
	}

	/*
	 * rank 0: the first PU of every core, in hwloc core order; rank 1: each
	 * core's second PU (its SMT sibling); ...  So the first @ncores workers
	 * spread one-per-core, and only past that do siblings get used.
	 */
	for (int rank = 0; rank < maxsmt && idx < npus; rank++) {
		for (int i = 0; i < ncores && idx < npus; i++) {
			hwloc_obj_t core =
				hwloc_get_obj_by_type(topo, HWLOC_OBJ_CORE, i);
			int pu = hwloc_bitmap_first(core->cpuset);

			for (int r = 0; r < rank && pu != -1; r++)
				pu = hwloc_bitmap_next(core->cpuset, pu);
			if (pu != -1)
				g_pu_map[idx++] = pu;
		}
	}
	g_pu_count = idx;

	fprintf(stderr,
		"bench: hwloc pinning -- %d cores, %d PUs, one PU per core first\n",
		ncores, g_pu_count);
	hwloc_topology_destroy(topo);
}

void
bench_topology_pin(int worker_index)
{
	cpu_set_t set;
	int cpu;

	if (g_pu_map != NULL && g_pu_count > 0)
		cpu = g_pu_map[worker_index % g_pu_count];
	else
		cpu = worker_index;	/* fallback: prior identity pinning */

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) != 0)
		perror("bench: sched_setaffinity");
}
