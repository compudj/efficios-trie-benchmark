// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * dcache_qspinlock.h -- the LRU shard lock: a userspace port of the kernel's
 * queued spinlock (kernel/locking/qspinlock.c, native arm, no paravirt), which
 * is what list_lru_one.lock -- a spinlock_t -- is.  Every engine's shard lock
 * is this one, so no engine is measured on a different lock.
 *
 * One 32-bit word, laid out as the kernel's with _Q_PENDING_BITS == 8:
 *
 *	 0- 7  locked byte
 *	 8-15  pending byte (bit 8)
 *	16-31  tail: index + 1 of the last queued waiter's node, 0 = none
 *
 * Uncontended, it is one cmpxchg to take and one byte store to release.  The
 * FIRST waiter does not queue: it sets pending and spins on the locked byte.
 * Further waiters queue MCS-style, each spinning on its OWN node; only the
 * queue head spins on the lock word, so a release is seen by one waiter, not
 * by every waiter at once.
 *
 * Why not test-and-test-and-set, which this was until 2026-10-02: on a TTAS
 * lock every release sets off a race among all spinners, each issuing a
 * cmpxchg that fails but still pulls the line, so the handoff gets slower the
 * more threads wait.  Measured on allocating churn (one shard per CCX, 8-core
 * CCXs): one shard with 4 writers ran 15.2 Mchurn/s, with 8 writers 7.1 --
 * MORE contenders, HALF the throughput.  The kernel's lock does not collapse
 * that way, so neither may the analogue.  The herd also amplified small
 * per-engine timing differences, in either direction, into the ratios.
 *
 * What this lock does instead, same workload, one shard: at par with TTAS or
 * a little above for 2-3 writers, then FLAT at ~11.5 Mchurn/s from 4 to 8
 * writers.  TTAS peaks at 15.2 with exactly 4 (a race among three spinners is
 * cheaper than a queued handoff) and falls to 7-8 by 6.  That plateau is the
 * kernel's behaviour too, so the 4-writer point reads lower than it did.
 *
 * Nodes are per THREAD where the kernel's are per CPU, and one per thread
 * suffices: a node is in use only while its thread WAITS (the head hands queue
 * headship on before its critical section runs), a thread waits for one lock
 * at a time, and there is no interrupt context to nest.  Holding several of
 * these locks at once is fine.  The tail encodes a node by its index in
 * dc_qnode_tab[], assigned on a thread's first queueing; at most 65535 threads
 * may ever queue in one process.
 *
 * ⚠ PREEMPTION.  The kernel disables preemption across the wait, so a queued
 * waiter is never descheduled while its successors wait on it.  A userspace
 * thread can be, and a descheduled waiter stalls every waiter queued behind it
 * until it runs again.  That bites where the harness co-pins each writer's
 * call_rcu worker on the writer's CPU AND the worker takes this lock: the txn
 * and bucket-lock engines fold shells in call_rcu callbacks, which delist
 * them, so a worker can preempt its own writer while the writer is queued and
 * then spin behind it.  The kernel cannot get there -- a lock also taken from
 * an RCU callback is taken with bottom halves disabled in process context --
 * and it delists a dentry before its call_rcu in any case.
 *
 * Measured against TTAS (2026-10-02).  48 threads renaming across 6 nodes:
 * renames at 0.86-0.93 of TTAS's rate at rename fraction 0.1, 0.66-0.75 at
 * 0.5.  At 0.2, with the workers moved to the writers' SMT siblings, the gap
 * closes from 0.75 to 0.97 for txn-pernode and from 0.73 to 0.82 for the
 * bucket lock, whose remainder is FIFO handoffs crossing nodes (TTAS keeps
 * the lock on one node more often).  With 8 writers on one node every
 * rename-heavy panel is within 5% of TTAS, and allocating churn's workers
 * never take this lock -- which is why an earlier test there, moving the
 * workers to idle cores, saw nothing.
 */
#ifndef DCACHE_QSPINLOCK_H
#define DCACHE_QSPINLOCK_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <urcu/arch.h>
#include <urcu/compiler.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "dcache_qspinlock.h lays the lock word out little-endian"
#endif

struct dc_qspinlock {
	union {
		uint32_t val;
		struct {
			uint8_t locked;
			uint8_t pending;
		};
		struct {
			uint16_t locked_pending;
			uint16_t tail;
		};
	};
};

#define DC_Q_LOCKED_VAL			1U
#define DC_Q_PENDING_VAL		(1U << 8)
#define DC_Q_LOCKED_MASK		0x000000ffU
#define DC_Q_PENDING_MASK		0x0000ff00U
#define DC_Q_LOCKED_PENDING_MASK	0x0000ffffU
#define DC_Q_TAIL_MASK			0xffff0000U
#define DC_Q_TAIL_OFFSET		16
#define DC_Q_MAX_NODES			65535U
#define DC_Q_PENDING_LOOPS		(1 << 9) /* x86's _Q_PENDING_LOOPS */

struct dc_qnode {
	struct dc_qnode *next;
	int locked;
} __attribute__((aligned(64)));

static struct dc_qnode *dc_qnode_tab[DC_Q_MAX_NODES];
static unsigned int dc_qnode_count;
static __thread struct dc_qnode dc_qnode_me;
static __thread uint32_t dc_qnode_tail;	/* encoded; 0 = not registered */

static __attribute__((unused, noinline)) uint32_t dc_qnode_register(void)
{
	unsigned int idx = __atomic_fetch_add(&dc_qnode_count, 1,
					      __ATOMIC_RELAXED);

	if (idx >= DC_Q_MAX_NODES) {
		fprintf(stderr, "dcache_qspinlock: more than %u queueing threads\n",
			DC_Q_MAX_NODES);
		abort();
	}
	__atomic_store_n(&dc_qnode_tab[idx], &dc_qnode_me, __ATOMIC_RELEASE);
	dc_qnode_tail = (idx + 1) << DC_Q_TAIL_OFFSET;
	return dc_qnode_tail;
}

static inline int dc_qspin_trylock(struct dc_qspinlock *lock)
{
	uint32_t val = __atomic_load_n(&lock->val, __ATOMIC_RELAXED);

	if (val)
		return 0;
	return __atomic_compare_exchange_n(&lock->val, &val, DC_Q_LOCKED_VAL, 0,
					   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* queued_spin_lock_slowpath(), minus the paravirt hooks and node nesting. */
static __attribute__((unused, noinline)) void
dc_qspin_lock_slowpath(struct dc_qspinlock *lock, uint32_t val)
{
	struct dc_qnode *node, *prev, *next;
	uint32_t old, tail;

	/* Wait for an in-progress pending -> locked hand-over, briefly. */
	if (val == DC_Q_PENDING_VAL) {
		int cnt = DC_Q_PENDING_LOOPS;

		do {
			val = __atomic_load_n(&lock->val, __ATOMIC_RELAXED);
		} while (val == DC_Q_PENDING_VAL && cnt--);
	}

	/* Any contention beyond an owner: queue. */
	if (val & ~DC_Q_LOCKED_MASK)
		goto queue;

	/* Trylock or become pending: 0,0,* -> 0,1,* */
	val = __atomic_fetch_or(&lock->val, DC_Q_PENDING_VAL, __ATOMIC_ACQUIRE);
	if (caa_unlikely(val & ~DC_Q_LOCKED_MASK)) {
		/* A concurrent locker got there first: undo our pending (only if
		 * it was ours to set) and queue. */
		if (!(val & DC_Q_PENDING_MASK))
			__atomic_store_n(&lock->pending, 0, __ATOMIC_RELAXED);
		goto queue;
	}

	/* Pending: wait for the owner to go.  0,1,1 -> *,1,0 */
	if (val & DC_Q_LOCKED_MASK)
		while (__atomic_load_n(&lock->locked, __ATOMIC_ACQUIRE))
			caa_cpu_relax();

	/* Take it and clear pending in one store.  *,1,0 -> *,0,1 */
	__atomic_store_n(&lock->locked_pending, DC_Q_LOCKED_VAL,
			 __ATOMIC_RELAXED);
	return;

queue:
	node = &dc_qnode_me;
	tail = dc_qnode_tail ? dc_qnode_tail : dc_qnode_register();
	__atomic_store_n(&node->locked, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&node->next, NULL, __ATOMIC_RELAXED);

	/* The node may have been cold: the lock may have freed meanwhile. */
	if (dc_qspin_trylock(lock))
		return;

	/* Publish the tail; the node's initialization must be visible first. */
	cmm_smp_wmb();
	old = (uint32_t) __atomic_exchange_n(&lock->tail,
			(uint16_t) (tail >> DC_Q_TAIL_OFFSET), __ATOMIC_RELAXED)
		<< DC_Q_TAIL_OFFSET;
	next = NULL;

	/* A predecessor: link behind it, wait to become the queue head. */
	if (old & DC_Q_TAIL_MASK) {
		prev = __atomic_load_n(
			&dc_qnode_tab[(old >> DC_Q_TAIL_OFFSET) - 1],
			__ATOMIC_ACQUIRE);
		__atomic_store_n(&prev->next, node, __ATOMIC_RELAXED);
		while (!__atomic_load_n(&node->locked, __ATOMIC_ACQUIRE))
			caa_cpu_relax();
		next = __atomic_load_n(&node->next, __ATOMIC_RELAXED);
		if (next)
			__builtin_prefetch(next, 1);
	}

	/* Queue head: wait for the owner and any pending waiter to go. */
	while ((val = __atomic_load_n(&lock->val, __ATOMIC_ACQUIRE)) &
	       DC_Q_LOCKED_PENDING_MASK)
		caa_cpu_relax();

	/* Claim it.  Last in the queue: clear the tail too (n,0,0 -> 0,0,1). */
	if ((val & DC_Q_TAIL_MASK) == tail &&
	    __atomic_compare_exchange_n(&lock->val, &val, DC_Q_LOCKED_VAL, 0,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED))
		return;

	/* Someone queued behind us (or a racer's pending is in flight): take
	 * the locked byte alone, then hand headship to the successor. */
	__atomic_store_n(&lock->locked, DC_Q_LOCKED_VAL, __ATOMIC_RELAXED);
	if (!next)
		while (!(next = __atomic_load_n(&node->next, __ATOMIC_RELAXED)))
			caa_cpu_relax();
	__atomic_store_n(&next->locked, 1, __ATOMIC_RELEASE);
}

static inline void dc_qspin_lock(struct dc_qspinlock *lock)
{
	uint32_t val = 0;

	if (caa_likely(__atomic_compare_exchange_n(&lock->val, &val,
						   DC_Q_LOCKED_VAL, 0,
						   __ATOMIC_ACQUIRE,
						   __ATOMIC_RELAXED)))
		return;
	dc_qspin_lock_slowpath(lock, val);
}

static inline void dc_qspin_unlock(struct dc_qspinlock *lock)
{
	__atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

#endif /* DCACHE_QSPINLOCK_H */
