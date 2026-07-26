/* SPDX-License-Identifier: GPL-2.0
 *
 * Existence-structure bidirectional-list engine for bench_list_scale -- iface.
 *
 * NOTE ON LICENCE: this engine includes perfbook's existence.h/procon.h
 * (Paul E. McKenney, GPL-2.0), so this TU is GPL-2.0, unlike the MIT/Apache
 * engines beside it.  It is confined to its own translation unit partly for
 * that reason and partly because perfbook's headers want an api.h-style
 * environment (BUG_ON, spin_lock, smp_load_acquire, ...) that would collide
 * with bench_list_scale.c's own namespace.
 *
 * WHAT THIS MEASURES.  Existence structures are the ONE design in P1's cost
 * table that is in the SAME guarantee class as the pseudo-transaction: both
 * publish a multi-pointer update with a single store (eg_state vs the
 * selector), both charge the reader a test on every dereference, and both
 * defer a group object through call_rcu.  The difference the table predicts is
 * where the test's operand comes from -- existence loads a per-element field
 * (eh_egi), the pseudo-transaction reads a register it already held -- plus
 * the permanent space cost of that field in every element.  This engine exists
 * to measure exactly that, on the same list, same workload, same harness.
 */
#ifndef BENCH_EXISTENCE_LIST_H
#define BENCH_EXISTENCE_LIST_H

#include <stdint.h>

struct exl_ctx {
	int list_size;
	int churn;
	int random_pos;
	int step_limit;
	const int *anchor;	/* [churn] g_anchor */
	int8_t *present;	/* [churn] g_present, shared with the harness */
	int forward_only;	/* second read pass walks forward (see su_read) */
};

void exl_build(const struct exl_ctx *ctx);
unsigned long exl_read(long *viol);
void exl_write(int slot);
/* Between sweep points, on main, no worker live: rearm the single-writer
 * assertion (the harness respawns workers at every point). */
void exl_point_reset(void);

#endif /* BENCH_EXISTENCE_LIST_H */
