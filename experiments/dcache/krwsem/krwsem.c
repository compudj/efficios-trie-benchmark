// SPDX-License-Identifier: GPL-2.0
/*
 * krwsem.c -- glue between the opaque krwsem.h API and the vendored kernel
 * rw_semaphore.  GPL-2.0.
 */
#include "kcompat.h"
#include "krwsem.h"

_Static_assert(sizeof(struct rw_semaphore) <= KRWSEM_SIZE,
	       "kernel rw_semaphore must fit in the krwsem storage");

/*
 * ThreadSanitizer cannot see the happens-before this hand-rolled lock provides
 * (it intercepts pthread_rwlock, not a futex lock built from raw atomics), so it
 * would falsely flag data protected by the lock.  Model the lock explicitly:
 * an unlock RELEASEs the lock address, a lock ACQUIREs it -- exactly the
 * writer-unlock -> next-lock ordering.  No effect in non-TSAN builds.
 */
#if defined(__SANITIZE_THREAD__)
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
#define KRWSEM_TSAN_ACQ(a)	__tsan_acquire(a)
#define KRWSEM_TSAN_REL(a)	__tsan_release(a)
#else
#define KRWSEM_TSAN_ACQ(a)	((void) 0)
#define KRWSEM_TSAN_REL(a)	((void) 0)
#endif

static inline struct rw_semaphore *S(struct krwsem *l)
{
	return (struct rw_semaphore *)l->buf;
}

void krwsem_init(struct krwsem *l)
{
	struct rw_semaphore *s = S(l);

	atomic_long_set(&s->count, RWSEM_UNLOCKED_VALUE);
	raw_spin_lock_init(&s->wait_lock);
	INIT_LIST_HEAD(&s->wait_list);
	atomic_long_set(&s->owner, 0L);
}

void krwsem_rdlock(struct krwsem *l)   { down_read(S(l));  KRWSEM_TSAN_ACQ(l); }
void krwsem_rdunlock(struct krwsem *l) { KRWSEM_TSAN_REL(l); up_read(S(l)); }
void krwsem_wrlock(struct krwsem *l)   { down_write(S(l)); KRWSEM_TSAN_ACQ(l); }
void krwsem_wrunlock(struct krwsem *l) { KRWSEM_TSAN_REL(l); up_write(S(l)); }
