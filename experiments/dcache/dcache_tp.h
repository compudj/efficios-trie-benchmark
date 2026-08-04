/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * dcache_tp.h -- LTTng-UST tracepoints for the MCAS LRU live-lock.
 *
 * SCAFFOLDING, gated behind -DDC_ENABLE_TRACING.  The wedge leaves a node
 * HALF-LINKED -- its successor's prev still names it while its predecessor's
 * next does not -- and the state at the wedge cannot say how it got there.
 * These events reconstruct one node's history across threads.
 *
 * The hypothesis being discriminated: can the shard word read OFF while the
 * node is still linked?  If it can, lru_add_at() claims a LINKED node and
 * insert_before_prepare()'s PLAIN prepare-time stores (newp->next, newp->prev)
 * clobber live edges with no CAS behind them -- which is precisely a
 * half-linked node.  So the event set is the node's lifecycle: every shard-word
 * transition, every list commit, and the violation itself.  Everything else
 * (per-retry, per-record) is noise for THIS question.
 */

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER dc

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./dcache_tp.h"

#if !defined(_DCACHE_TP_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _DCACHE_TP_H

#include <lttng/tracepoint.h>
#include <stdint.h>

#define DC_TP_PTR(name, val) \
	lttng_ust_field_integer_hex(uintptr_t, name, (uintptr_t) (val))

/*
 * Every transition of d_lru.shard, whether it succeeded or not.  @old is what
 * the caller expected, @seen what the cmpxchg found, @want what it tried to
 * install.  A claim that succeeds from OFF on a node the list still contains is
 * the bug this is looking for.
 */
LTTNG_UST_TRACEPOINT_EVENT(dc, claim,
	LTTNG_UST_TP_ARGS(void *, n, unsigned int, old, unsigned int, seen,
			  unsigned int, want, int, site),
	LTTNG_UST_TP_FIELDS(
		DC_TP_PTR(n, n)
		lttng_ust_field_integer(unsigned int, old, old)
		lttng_ust_field_integer(unsigned int, seen, seen)
		lttng_ust_field_integer(unsigned int, want, want)
		lttng_ust_field_integer(int, site, site)
	)
)

/* A list commit that reported OK, with the edges it believed it was rewriting. */
LTTNG_UST_TRACEPOINT_EVENT(dc, commit,
	LTTNG_UST_TP_ARGS(void *, n, void *, a, void *, b, int, op, int, st),
	LTTNG_UST_TP_FIELDS(
		DC_TP_PTR(n, n)
		DC_TP_PTR(a, a)
		DC_TP_PTR(b, b)
		lttng_ust_field_integer(int, op, op)
		lttng_ust_field_integer(int, st, st)
	)
)

/*
 * The violation.  Self-diagnosing: names_n on each side is the round-trip that
 * classifies the damage without reading the window at all -- successor names it
 * but predecessor does not means a half-applied link, not a stale pointer and
 * not recycled memory.
 */
LTTNG_UST_TRACEPOINT_EVENT(dc, wedge,
	LTTNG_UST_TP_ARGS(void *, n, void *, nnext, void *, nprev,
			  void *, pv, void *, pvnext, void *, nx, void *, nxprev,
			  unsigned int, shard),
	LTTNG_UST_TP_FIELDS(
		DC_TP_PTR(n, n)
		DC_TP_PTR(nnext, nnext)
		DC_TP_PTR(nprev, nprev)
		DC_TP_PTR(pv, pv)
		DC_TP_PTR(pvnext, pvnext)
		DC_TP_PTR(nx, nx)
		DC_TP_PTR(nxprev, nxprev)
		lttng_ust_field_integer(unsigned int, shard, shard)
	)
)

#endif /* _DCACHE_TP_H */

#include <lttng/tracepoint-event.h>
