/*
 * Topology-aware worker pinning for the EfficiOS trie benchmarks.
 *
 * bench_topology_init() builds, once, a map from dense worker index -> OS
 * logical CPU that walks ONE PU PER PHYSICAL CORE before touching any SMT
 * sibling: worker 0..ncores-1 land on the first PU of each core, ncores..
 * 2*ncores-1 on the second PU (the siblings), and so on.  It uses hwloc to
 * discover the real topology, so the bench fills every core with no sibling
 * contention on ANY machine, instead of assuming the OS numbers CPUs 0..N-1 as
 * one-per-core (true on a 2x EPYC 9654, false in general).
 *
 * Call bench_topology_init() once from main() BEFORE spawning workers (it is
 * not thread-safe to build concurrently; the resulting map is read-only and
 * safe for all workers to read).  Each worker then calls bench_topology_pin()
 * with its index.  If hwloc is unavailable or fails, both degrade to the prior
 * identity map (pin worker i -> CPU i).
 */
#ifndef BENCH_TOPOLOGY_H
#define BENCH_TOPOLOGY_H

void bench_topology_init(void);
void bench_topology_pin(int worker_index);

/*
 * Number of physical cores discovered by bench_topology_init(): worker indices
 * 0 .. bench_topology_ncores()-1 are guaranteed to map one-per-physical-core
 * (no two SMT siblings of the same core).  Keeping the live worker count at or
 * below this value pins every worker to its own core.  Returns 0 if the
 * topology was not discovered (hwloc unavailable / identity fallback).
 */
int bench_topology_ncores(void);

/*
 * OS logical CPU that worker @worker_index pins to (the same mapping
 * bench_topology_pin() uses: one PU per physical core first, then siblings).
 * Lets a caller learn the workload's exact PU set without pinning.  Returns
 * @worker_index unchanged if the topology was not discovered (identity map).
 */
int bench_topology_cpu(int worker_index);

/*
 * Number of PUs in the map (>= any live worker count).  0 if not discovered.
 */
int bench_topology_pu_count(void);

/*
 * Topology domain of OS logical CPU @oscpu: bench_topology_core_of() returns a
 * compact physical-core id, bench_topology_l3_of() a compact L3-cache id (PUs
 * sharing an L3 share an id).  Used to coarsen per-CPU reclaim/allocation to a
 * core or L3 domain.  Both fall back to @oscpu if the topology was not
 * discovered.
 */
int bench_topology_core_of(int oscpu);
int bench_topology_l3_of(int oscpu);

#endif /* BENCH_TOPOLOGY_H */
