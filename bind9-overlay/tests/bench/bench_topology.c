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
#include <unistd.h>

#include <hwloc.h>

/*
 * Worker index -> OS logical CPU.  Entry i is the (i / ncores)-th PU of core
 * (i % ncores), i.e. one PU per core across the first @ncores entries, then the
 * siblings.  Built once by bench_topology_init(); read-only afterward.  NULL
 * (hwloc failed / not yet built) means "use the identity map".
 */
static int *g_pu_map;
static int g_pu_count;
static int g_ncores;	/* physical cores; worker idx < g_ncores => own core */
static int *g_core_of;	/* [g_topo_ncpu] OS cpu -> compact physical-core id */
static int *g_l3_of;	/* [g_topo_ncpu] OS cpu -> compact L3-cache id */
static int g_topo_ncpu;

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
	g_ncores = ncores;

	/* OS-cpu -> core / L3 domain maps (for coarser reclaim/alloc domains). */
	{
		int ncpu = (int) sysconf(_SC_NPROCESSORS_CONF), c;

		if (ncpu < 1)
			ncpu = 1;
		g_topo_ncpu = ncpu;
		g_core_of = malloc((size_t) ncpu * sizeof(*g_core_of));
		g_l3_of = malloc((size_t) ncpu * sizeof(*g_l3_of));
		if (g_core_of != NULL && g_l3_of != NULL) {
			for (c = 0; c < ncpu; c++) {
				hwloc_obj_t pu =
					hwloc_get_pu_obj_by_os_index(topo, c), o;

				g_core_of[c] = c;	/* fallback */
				g_l3_of[c] = c;
				if (pu == NULL)
					continue;
				o = hwloc_get_ancestor_obj_by_type(topo,
					HWLOC_OBJ_CORE, pu);
				if (o != NULL)
					g_core_of[c] = o->logical_index;
				o = hwloc_get_ancestor_obj_by_type(topo,
					HWLOC_OBJ_L3CACHE, pu);
				g_l3_of[c] = (o != NULL) ? o->logical_index
							 : g_core_of[c];
			}
		}
	}

	fprintf(stderr,
		"bench: hwloc pinning -- %d cores, %d PUs, one PU per core first\n",
		ncores, g_pu_count);
	hwloc_topology_destroy(topo);
}

int
bench_topology_ncores(void)
{
	return g_ncores;
}

int
bench_topology_cpu(int worker_index)
{
	if (g_pu_map != NULL && g_pu_count > 0)
		return g_pu_map[worker_index % g_pu_count];
	return worker_index;		/* identity fallback */
}

int
bench_topology_pu_count(void)
{
	return g_pu_count;
}

int
bench_topology_core_of(int oscpu)
{
	if (g_core_of != NULL && oscpu >= 0 && oscpu < g_topo_ncpu)
		return g_core_of[oscpu];
	return oscpu;			/* identity fallback */
}

int
bench_topology_l3_of(int oscpu)
{
	if (g_l3_of != NULL && oscpu >= 0 && oscpu < g_topo_ncpu)
		return g_l3_of[oscpu];
	return oscpu;			/* identity fallback */
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
