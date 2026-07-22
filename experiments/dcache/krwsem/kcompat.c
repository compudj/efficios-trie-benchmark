// SPDX-License-Identifier: GPL-2.0
/*
 * kcompat.c -- userspace backing for kcompat.h (see that file's header).
 * Derived from Linux kernel interfaces; GPL-2.0.
 *
 * Implements the pieces that cannot be header-only: the per-thread task, the
 * futex-based park/wake that stand in for the scheduler, the wake_q batch, and
 * a jiffies clock in 1/HZ-second ticks so the rwsem handoff timeout keeps its
 * ~4 ms kernel value.
 */
#define _GNU_SOURCE
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include "kcompat.h"

/*
 * One task_struct per thread, in TLS.  Its address is stable for the thread's
 * lifetime, so it can be stored in sem->owner and in a waiter, compared across
 * threads, and referenced by a wake_q, without any refcounting (get/put are
 * no-ops).  Zero-initialised: wake_flag = 0 (parked), wake_q.next = NULL.
 */
__thread struct task_struct kcompat_self;

static long sys_futex(int *uaddr, int op, int val)
{
	return syscall(SYS_futex, uaddr, op, val, NULL, NULL, 0);
}

/*
 * schedule(): sleep unless a wake landed since set_current_state() sampled
 * park_seq.  futex_wait sleeps only while wake_seq == park_seq, so a wake that
 * raced the sample (wake_seq now differs) returns immediately instead of being
 * lost.  One futex_wait per call; the rwsem loop re-checks the grant condition
 * and absorbs any spurious return.
 */
void kcompat_park(void)
{
	struct task_struct *t = current;

	sys_futex((int *)&t->wake_seq, FUTEX_WAIT_PRIVATE, (int)t->park_seq);
}

void wake_up_process(struct task_struct *t)
{
	__atomic_add_fetch(&t->wake_seq, 1u, __ATOMIC_RELEASE);
	sys_futex((int *)&t->wake_seq, FUTEX_WAKE_PRIVATE, 1);
}

/*
 * wake_q: collect granted tasks into the granter's per-call array, then wake
 * them all in wake_up_q() after wait_lock is dropped.  No task_struct field is
 * shared between granters, so there is no lock-free-node reuse race.  A task
 * appearing twice in one batch just double-wakes (wake_flag is level-triggered,
 * so it is idempotent).
 */
void wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	if (head->n < WAKE_Q_MAX)
		head->tasks[head->n++] = task;
	else
		wake_up_process(task);		/* overflow (unreached): wake now */
}

void wake_q_add_safe(struct wake_q_head *head, struct task_struct *task)
{
	wake_q_add(head, task);
}

void wake_up_q(struct wake_q_head *head)
{
	int i;

	for (i = 0; i < head->n; i++)
		wake_up_process(head->tasks[i]);
	head->n = 0;
}

/* jiffies at HZ=250: monotonic time in 4 ms ticks. */
unsigned long kcompat_jiffies(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)((u64)ts.tv_sec * HZ +
			       (u64)ts.tv_nsec * HZ / 1000000000ULL);
}
