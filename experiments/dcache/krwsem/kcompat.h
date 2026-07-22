/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kcompat.h -- a thin userspace shim that lets the UNMODIFIED Linux kernel
 * rw_semaphore slow-path (kernel/locking/rwsem.c) compile and run in a
 * pthreads benchmark.  This file is DERIVED FROM the Linux kernel's in-kernel
 * interfaces (atomics, list, wake_q, task/scheduler hooks) and is therefore,
 * like the vendored rwsem.c it serves, licensed GPL-2.0.
 *
 * It maps kernel primitives to userspace equivalents:
 *   - atomic_long_t / smp_* / READ_ONCE  -> C11 __atomic builtins
 *   - raw_spinlock_t                     -> a test-and-set spinlock
 *   - task_struct / current / wake_q     -> a per-thread TLS task + futex park
 *   - schedule()/set_current_state()     -> futex wait on the task's wake word
 *   - jiffies / HZ / handoff timeout     -> CLOCK_MONOTONIC in 1/250 s ticks
 *   - lockdep / lockevents / tracing     -> no-ops
 *
 * The kernel is built here with CONFIG_RWSEM_SPIN_ON_OWNER=n (so the optimistic
 * owner-spinning path uses the kernel's OWN fallback stubs), CONFIG_PREEMPT_RT=n,
 * CONFIG_DEBUG_RWSEMS=n.  The FAIRNESS slow-path -- the FIFO wait list, the
 * handoff bit, reader-steal gating, reader batching, "a queued writer blocks new
 * readers" -- is compiled VERBATIM, which is the whole point: this is Linux's
 * rwsem bias behaviour, not an approximation of it.
 */
#ifndef KCOMPAT_H
#define KCOMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

typedef uint64_t u64;
typedef int64_t  s64;
typedef uint32_t u32;
typedef int32_t  s32;

#define BITS_PER_LONG	(__SIZEOF_LONG__ * 8)

/* ---- attributes / no-op kernel annotations ------------------------------- */
#define __sched
#define notrace
#define __visible
#define __ro_after_init
#define __read_mostly
#ifndef __always_inline
#define __always_inline	inline __attribute__((always_inline))
#endif
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)

/* sparse annotations -> nothing */
#define __releases(x)
#define __acquires(x)
#define __must_hold(x)
#define __cond_acquires(x)

/* WARN / BUG family: evaluate the condition (kernel WARN_ON returns it), no print */
#define WARN_ON_ONCE(c)		({ int __w = !!(c); __w; })
#define WARN_ON(c)		({ int __w = !!(c); __w; })
#define WARN_ONCE(c, ...)	({ int __w = !!(c); __w; })
#define WARN(c, ...)		({ int __w = !!(c); __w; })
#define BUG_ON(c)		do { if (unlikely(c)) __builtin_trap(); } while (0)
#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

#ifndef offsetof
#define offsetof(t, m)	__builtin_offsetof(t, m)
#endif
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/* ---- barriers / one-shot accessors --------------------------------------- */
#define READ_ONCE(x)		(*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, val)	(*(volatile __typeof__(x) *)&(x) = (val))
#define smp_load_acquire(p)	__atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, v)	__atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define smp_acquire__after_ctrl_dep()	__atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_mb()		__atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb()		__atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb()		__atomic_thread_fence(__ATOMIC_RELEASE)
#define cpu_relax()		__builtin_ia32_pause()

/* ---- atomic_long_t -------------------------------------------------------- */
typedef struct { long counter; } atomic_long_t;
#define ATOMIC_LONG_INIT(i)	{ (i) }

static __always_inline long atomic_long_read(const atomic_long_t *v)
{ return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); }
static __always_inline void atomic_long_set(atomic_long_t *v, long i)
{ __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); }
static __always_inline void atomic_long_add(long i, atomic_long_t *v)
{ (void) __atomic_fetch_add(&v->counter, i, __ATOMIC_RELAXED); }
static __always_inline long atomic_long_add_return(long i, atomic_long_t *v)
{ return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static __always_inline long atomic_long_add_return_acquire(long i, atomic_long_t *v)
{ return __atomic_add_fetch(&v->counter, i, __ATOMIC_ACQUIRE); }
static __always_inline long atomic_long_add_return_release(long i, atomic_long_t *v)
{ return __atomic_add_fetch(&v->counter, i, __ATOMIC_RELEASE); }
static __always_inline long atomic_long_fetch_add(long i, atomic_long_t *v)
{ return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static __always_inline long atomic_long_fetch_add_release(long i, atomic_long_t *v)
{ return __atomic_fetch_add(&v->counter, i, __ATOMIC_RELEASE); }
static __always_inline void atomic_long_or(long i, atomic_long_t *v)
{ (void) __atomic_fetch_or(&v->counter, i, __ATOMIC_SEQ_CST); }
static __always_inline void atomic_long_andnot(long i, atomic_long_t *v)
{ (void) __atomic_fetch_and(&v->counter, ~i, __ATOMIC_SEQ_CST); }
static __always_inline long atomic_long_cmpxchg(atomic_long_t *v, long old, long neu)
{ __atomic_compare_exchange_n(&v->counter, &old, neu, 0,
	__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return old; }
static __always_inline bool atomic_long_try_cmpxchg(atomic_long_t *v, long *old, long neu)
{ return __atomic_compare_exchange_n(&v->counter, old, neu, 0,
	__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
static __always_inline bool atomic_long_try_cmpxchg_acquire(atomic_long_t *v, long *old, long neu)
{ return __atomic_compare_exchange_n(&v->counter, old, neu, 0,
	__ATOMIC_ACQUIRE, __ATOMIC_RELAXED); }

/* ---- doubly linked list (minimal subset) --------------------------------- */
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name)	{ &(name), &(name) }
static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l; l->prev = l; }
static inline int list_empty(const struct list_head *h) { return h->next == h; }
static inline int list_is_singular(const struct list_head *h)
{ return !list_empty(h) && (h->next == h->prev); }
static inline void __list_add(struct list_head *n, struct list_head *p, struct list_head *x)
{ x->prev = n; n->next = x; n->prev = p; p->next = n; }
static inline void list_add_tail(struct list_head *n, struct list_head *h)
{ __list_add(n, h->prev, h); }
static inline void list_del(struct list_head *e)
{ e->next->prev = e->prev; e->prev->next = e->next; e->next = e->prev = NULL; }
static inline void list_move_tail(struct list_head *e, struct list_head *h)
{ e->next->prev = e->prev; e->prev->next = e->next; list_add_tail(e, h); }
#define list_entry(ptr, type, member)	container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)
#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_entry((head)->next, __typeof__(*pos), member),	\
	     n = list_entry(pos->member.next, __typeof__(*pos), member);	\
	     &pos->member != (head);					\
	     pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

/* ---- raw_spinlock_t (test-and-set) --------------------------------------- */
typedef struct { volatile int locked; } raw_spinlock_t;
#define raw_spin_lock_init(l)	do { (l)->locked = 0; } while (0)
static inline void raw_spin_lock(raw_spinlock_t *l)
{
	while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
		while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
			cpu_relax();
}
static inline void raw_spin_unlock(raw_spinlock_t *l)
{ __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE); }
#define raw_spin_lock_irq(l)			raw_spin_lock(l)
#define raw_spin_unlock_irq(l)			raw_spin_unlock(l)
#define raw_spin_lock_irqsave(l, f)		do { (void)(f); raw_spin_lock(l); } while (0)
#define raw_spin_unlock_irqrestore(l, f)	do { (void)(f); raw_spin_unlock(l); } while (0)

/* ---- task / current / wake_q / scheduler --------------------------------- */
/*
 * wake_q: the kernel batches "wake these tasks after dropping wait_lock" via a
 * lock-free list threaded through each task_struct.  That reuse is safe only
 * under the scheduler's guarantee that a task is not runnable until wake_up_q()
 * has unlinked its node -- which a futex cannot give (a woken task can return and
 * re-block on ANOTHER lock while its node is still linked, racing the shared
 * node; TSAN flags exactly this).  We keep the same wake_q_add/wake_up_q API the
 * rwsem calls but back it with a per-call fixed array on the GRANTER's stack, so
 * no task_struct field is shared between granters.  Bounded by MAX_READERS_WAKEUP
 * (256); on the (unreached) overflow we wake in place.
 */
#define WAKE_Q_MAX	320
struct wake_q_head { int n; struct task_struct *tasks[WAKE_Q_MAX]; };
#define DEFINE_WAKE_Q(name)	struct wake_q_head name = { .n = 0 }
static inline void wake_q_init(struct wake_q_head *h) { h->n = 0; }
static inline bool wake_q_empty(struct wake_q_head *h) { return h->n == 0; }

/*
 * Park/wake via a monotonic wake SEQUENCE (lossless futex idiom).  A resettable
 * "woken" bit loses wakes: a waiter stays on the wait-list, so a granter can set
 * the bit concurrently with the waiter's set_current_state reset, and the reset
 * clobbers the wake -> sleep forever.  Instead wake_up_process INCREMENTS wake_seq
 * (never resets); set_current_state SAMPLES it into park_seq; schedule() does
 * futex_wait(&wake_seq, park_seq), which sleeps only if no wake landed since the
 * sample.  A wake racing the sample bumps wake_seq != park_seq, so futex_wait
 * returns at once and the rwsem loop re-checks -- no lost wake.
 */
struct task_struct {
	long __state;
	unsigned wake_seq;		/* bumped by every wake (the futex word) */
	unsigned park_seq;		/* wake_seq sampled at set_current_state */
};

extern __thread struct task_struct kcompat_self;
#define current			(&kcompat_self)
#define get_task_struct(t)	do { (void)(t); } while (0)
#define put_task_struct(t)	do { (void)(t); } while (0)

extern void kcompat_park(void);			/* schedule(): futex wait  */
extern void wake_up_process(struct task_struct *t);
extern void wake_q_add(struct wake_q_head *h, struct task_struct *t);
extern void wake_q_add_safe(struct wake_q_head *h, struct task_struct *t);
extern void wake_up_q(struct wake_q_head *h);
extern unsigned long kcompat_jiffies(void);

#define TASK_RUNNING		0x0000
#define TASK_INTERRUPTIBLE	0x0001
#define TASK_UNINTERRUPTIBLE	0x0002
#define TASK_KILLABLE		TASK_UNINTERRUPTIBLE
#define TASK_NORMAL		(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
#define MAX_SCHEDULE_TIMEOUT	((long)(~0UL >> 1))

#define __set_current_state(s)	do { current->__state = (s); } while (0)
#define set_current_state(s)						\
	do {								\
		current->__state = (s);					\
		if ((s) != TASK_RUNNING)				\
			current->park_seq =				\
				__atomic_load_n(&current->wake_seq,	\
						__ATOMIC_ACQUIRE);	\
	} while (0)
#define schedule()			kcompat_park()
#define schedule_preempt_disabled()	kcompat_park()
#define signal_pending_state(state, task)	(0)
#define might_sleep()			do { } while (0)
#define need_resched()			(0)
#define preempt_disable()		do { } while (0)
#define preempt_enable()		do { } while (0)
#define rt_or_dl_task(t)		(0)

/* hung-task detector: no-op */
#define hung_task_set_blocker(a, b)	do { } while (0)
#define hung_task_clear_blocker()	do { } while (0)
#define BLOCKER_TYPE_RWSEM_READER	0
#define BLOCKER_TYPE_RWSEM_WRITER	0

/* ---- time / jiffies (HZ=250 -> 1 tick = 4 ms, faithful handoff timeout) --- */
#define HZ			250
#define jiffies			(kcompat_jiffies())
#define time_after(a, b)	((long)((b) - (a)) < 0)
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))

/* ---- lockdep / lockevents / tracing: no-ops ------------------------------ */
#define lockevent_inc(x)		do { } while (0)
#define lockevent_cond_inc(x, c)	do { } while (0)
#define trace_contention_begin(a, b)	do { } while (0)
#define trace_contention_end(a, b)	do { } while (0)
#define LCB_F_READ			0
#define LCB_F_WRITE			0
#define lockdep_assert_held(x)		do { } while (0)
#define lockdep_assert_preemption_disabled()	do { } while (0)
struct lock_class_key { int __unused; };
struct lockdep_map { int __unused; };

/* lockdep instrumentation in the public API: off (as CONFIG_DEBUG_LOCK_ALLOC=n) */
#define _RET_IP_	0UL
#define _THIS_IP_	0UL
#define rwsem_acquire(m, s, t, i)		do { } while (0)
#define rwsem_acquire_nest(m, s, t, n, i)	do { } while (0)
#define rwsem_acquire_read(m, s, t, i)		do { } while (0)
#define rwsem_release(m, i)			do { } while (0)
#define lock_downgrade(m, i)			do { } while (0)
#define LOCK_CONTENDED(_lock, try, lock)		lock(_lock)
#define LOCK_CONTENDED_RETURN(_lock, try, lock)		lock(_lock)

/* ---- ERR_PTR family ------------------------------------------------------ */
#define MAX_ERRNO	4095
#define IS_ERR_VALUE(x)	unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long e) { return (void *)e; }
static inline long PTR_ERR(const void *p) { return (long)p; }
static inline bool IS_ERR(const void *p) { return IS_ERR_VALUE((unsigned long)p); }

/* ---- the rw_semaphore itself (CONFIG_RWSEM_SPIN_ON_OWNER=n, no debug) ----- */
#define RWSEM_UNLOCKED_VALUE	0UL
struct rw_semaphore {
	atomic_long_t count;
	atomic_long_t owner;
	raw_spinlock_t wait_lock;
	struct list_head wait_list;
};

/* public API implemented by rwsem.c */
extern void down_read(struct rw_semaphore *sem);
extern void up_read(struct rw_semaphore *sem);
extern void down_write(struct rw_semaphore *sem);
extern void up_write(struct rw_semaphore *sem);
extern int down_read_trylock(struct rw_semaphore *sem);
extern int down_write_trylock(struct rw_semaphore *sem);

#endif /* KCOMPAT_H */
