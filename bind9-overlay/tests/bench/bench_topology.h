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

#endif /* BENCH_TOPOLOGY_H */
