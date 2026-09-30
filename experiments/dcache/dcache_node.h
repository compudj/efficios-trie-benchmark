// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_node.h -- the NUMA node of the CPU this thread runs on: the LRU shard
 * axis (the kernel's `lru->node[nid]`).
 *
 * The kernel takes nid from the dentry's own memory (page_to_nid in
 * list_lru_add_obj).  Here the caller's node stands in for it: each thread
 * allocates from its own allocator arena, first-touches the pages it uses, and
 * its reclaim runs on the same CPU, so the two coincide.  Querying the page's
 * node instead (get_mempolicy) was tried and bought nothing measurable.
 *
 * Read from the rseq ABI page when it exposes node ids -- a plain load, no
 * syscall.  Otherwise getcpu(), which glibc serves from the vDSO and which
 * returns the node directly.  NEVER default to node 0: on this 24-node machine
 * (one NUMA node per 8-core CCD) that puts every thread's enqueue on ONE shard
 * lock contended across CCDs.  That is exactly what happened on the rseq slab
 * routes: their librseq reports node ids unavailable to threads it did not
 * register itself, the seqlock engine's threads never are, and its churn
 * collapsed 20-60x (2026-09-29) -- a build artifact that read as an engine
 * property.
 */
#ifndef DCACHE_NODE_H
#define DCACHE_NODE_H

#include <sched.h>
#include <rseq/rseq.h>

static inline unsigned int dc_current_node(void)
{
	unsigned int cpu, node;

	if (rseq_node_id_available())
		return rseq_current_node_id();
	if (getcpu(&cpu, &node) == 0)
		return node;
	return 0;
}

#endif /* DCACHE_NODE_H */
