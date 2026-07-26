/* SPDX-License-Identifier: MIT
 *
 * MV-RLU bidirectional-list engine for bench_list_scale -- interface.
 *
 * WHY A SEPARATE TRANSLATION UNIT: third_party/mvrlu/include/mvrlu.h ships an
 * "RLU compatibility wrapper" (mvrlu.h:144-172) that #defines every RLU_* macro
 * -- RLU_DEREF, RLU_TRY_LOCK, RLU_ALLOC, ... -- AND does
 *     typedef mvrlu_thread_struct_t rlu_thread_data_t;
 * so it collides head-on with third_party/rlu/rlu.h, which bench_list_scale.c
 * already includes for the rlu_list engine.  The macros could be #undef'd; the
 * typedef cannot.  Hence MV-RLU lives here, behind a plain C interface that
 * mentions none of its types, and bench_list_scale.c includes only THIS header.
 *
 * The engine mirrors rlu_write()/rlu_read() in bench_list_scale.c operation for
 * operation -- same lock order, same abort-and-retry structure, same traversal
 * and validation -- so the two multi-version schemes meet on identical ground.
 */
#ifndef BENCH_MVRLU_LIST_H
#define BENCH_MVRLU_LIST_H

#include <stdint.h>

/* Harness state the engine needs.  Filled by bench_list_scale.c at build time,
 * after g_anchor/g_present are allocated and the sizes are known.  `present` is
 * the harness's own array and is written by the engine, exactly as the other
 * engines write it. */
struct mvl_ctx {
	int list_size;		/* LIST_SIZE   */
	int churn;		/* CHURN       */
	int random_pos;		/* g_random_pos: no sortedness invariant */
	int step_limit;		/* STEP_LIMIT runaway-walk guard */
	const int *anchor;	/* [churn] g_anchor  */
	int8_t *present;	/* [churn] g_present, shared with the harness */
};

void mvl_build(const struct mvl_ctx *ctx);
unsigned long mvl_read(long *viol);
void mvl_write(int slot);
void mvl_tl_begin(void);
void mvl_tl_end(void);

#endif /* BENCH_MVRLU_LIST_H */
