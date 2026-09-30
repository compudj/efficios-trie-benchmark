/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * seqcount.h -- minimal userspace seqcount + seqlock for the dcache_seqlock
 * baseline, built on liburcu's memory-model primitives (cmm_smp_rmb/wmb,
 * CMM_LOAD/STORE_SHARED, caa_cpu_relax) so its ordering matches the rest of the
 * benchmark's engines.  Semantics mirror include/linux/seqlock.h:
 *
 *   - the global rename_lock is a seqlock_t (a seqcount plus a writer spinlock);
 *     write_seqlock() serializes writers AND makes the update visible as a single
 *     even->odd->even transition to lockless readers;
 *   - a per-dentry d_seq is a bare seqcount_t, published under the dentry's own
 *     d_lock (the writer already holds it), read by __d_lookup_rcu.
 *
 * A reader brackets its critical loads with read_seqbegin()/read_seqretry(): if a
 * writer ran in between (sequence changed, or was odd) it retries.  The per-dentry
 * sample on the RCU-walk fast path uses raw_seqcount_begin() instead (no spin,
 * odd bit masked), and the snapshot readers (d_path family) use the bounded
 * read_seqbegin_or_lock() form.  This is the machinery the urcu-txn port
 * replaces with per-slot read-set validation.
 */

#ifndef DCACHE_SEQCOUNT_H
#define DCACHE_SEQCOUNT_H

#include <pthread.h>

#include <urcu/compiler.h>
#include <urcu/arch.h>			/* caa_cpu_relax() */
#include <urcu/system.h>		/* CMM_LOAD_SHARED / CMM_STORE_SHARED */

typedef struct {
	unsigned long sequence;
} seqcount_t;

static inline void seqcount_init(seqcount_t *s)
{
	s->sequence = 0;
}

/*
 * Begin a read section: spin while a writer holds it (odd), then order the
 * subsequent protected loads after this read of the sequence.
 */
static inline unsigned long read_seqcount_begin(const seqcount_t *s)
{
	unsigned long ret;

	for (;;) {
		ret = CMM_LOAD_SHARED(s->sequence);
		if (!(ret & 1UL))
			break;
		caa_cpu_relax();
	}
	cmm_smp_rmb();
	return ret;
}

/* End a read section: true => a writer intervened; the reader must retry. */
static inline int read_seqcount_retry(const seqcount_t *s, unsigned long start)
{
	cmm_smp_rmb();
	return CMM_LOAD_SHARED(s->sequence) != start;
}

/*
 * The kernel's raw_seqcount_begin(): a begin that does NOT spin on an in-flight
 * writer.  An odd (writer-active) value is returned with bit 0 CLEARED, so the
 * matching read_seqcount_retry() fails whether or not the writer has finished
 * by then.  Returning the odd value itself would be wrong: a reader that
 * samples mid-write and re-checks before the writer's end-bump sees the same
 * odd value and validates a torn read.  __d_lookup_rcu uses this per dentry so
 * a reader never blocks on a dentry a writer is touching.
 */
static inline unsigned long raw_seqcount_begin(const seqcount_t *s)
{
	unsigned long ret = CMM_LOAD_SHARED(s->sequence) & ~1UL;

	cmm_smp_rmb();
	return ret;
}

/* Writer side of a bare seqcount (caller already holds the relevant lock). */
static inline void write_seqcount_begin(seqcount_t *s)
{
	CMM_STORE_SHARED(s->sequence, s->sequence + 1);
	cmm_smp_wmb();
}

static inline void write_seqcount_end(seqcount_t *s)
{
	cmm_smp_wmb();
	CMM_STORE_SHARED(s->sequence, s->sequence + 1);
}

/* ---- seqlock: seqcount + writer spinlock (the rename_lock analog) -------- */

/*
 * The seqlock's writer lock is a FIFO TICKET lock.  The kernel's rename_lock is
 * a seqlock_t whose lock is a spinlock_t -- a fair, queued (MCS) qspinlock -- so
 * waiters are served in arrival order and no class of waiter starves another.
 * This used to be a pthread spinlock, which is test-and-set: under contention
 * whoever sees the line free first wins, and once readers took this lock too
 * (read_seqlock_excl, the snapshot passes) the renamers starved outright -- a
 * bias the kernel does not have.  A ticket lock restores the arrival order.  It
 * is still not the kernel's MCS queue: every waiter spins on the one owner word,
 * so with many waiters each handoff invalidates all of them, a scalability cost
 * the qspinlock's per-waiter spinning avoids.
 */
typedef struct {
	unsigned int next;		/* ticket dispenser */
	unsigned int owner;		/* now serving */
} dc_ticketlock_t;

static inline void ticket_lock_init(dc_ticketlock_t *l)
{
	l->next = 0;
	l->owner = 0;
}

static inline void ticket_lock(dc_ticketlock_t *l)
{
	unsigned int me = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);

	while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != me)
		caa_cpu_relax();
}

static inline void ticket_unlock(dc_ticketlock_t *l)
{
	/* only the holder writes owner: the relaxed read is its own value */
	__atomic_store_n(&l->owner,
			 __atomic_load_n(&l->owner, __ATOMIC_RELAXED) + 1,
			 __ATOMIC_RELEASE);
}

typedef struct {
	seqcount_t seq;
	dc_ticketlock_t lock;
} seqlock_t;

static inline void seqlock_init(seqlock_t *sl)
{
	seqcount_init(&sl->seq);
	ticket_lock_init(&sl->lock);
}

static inline void seqlock_destroy(seqlock_t *sl)
{
	(void) sl;
}

static inline unsigned long read_seqbegin(const seqlock_t *sl)
{
	return read_seqcount_begin(&sl->seq);
}

static inline int read_seqretry(const seqlock_t *sl, unsigned long start)
{
	return read_seqcount_retry(&sl->seq, start);
}

static inline void write_seqlock(seqlock_t *sl)
{
	ticket_lock(&sl->lock);
	write_seqcount_begin(&sl->seq);
}

static inline void write_sequnlock(seqlock_t *sl)
{
	write_seqcount_end(&sl->seq);
	ticket_unlock(&sl->lock);
}

/*
 * Locking reader: take the writer spinlock WITHOUT bumping the sequence, which
 * excludes writers and other locking readers but leaves lockless readers
 * undisturbed (the kernel's read_seqlock_excl).
 */
static inline void read_seqlock_excl(seqlock_t *sl)
{
	ticket_lock(&sl->lock);
}

static inline void read_sequnlock_excl(seqlock_t *sl)
{
	ticket_unlock(&sl->lock);
}

/*
 * The kernel's "lockless first, locked retry" reader (read_seqbegin_or_lock /
 * need_seqretry / done_seqretry), which the d_path family and d_walk use to
 * get a rename-consistent multi-dentry snapshot with a BOUNDED retry: the
 * caller starts with *seq = 0 (even => lockless pass); if need_seqretry()
 * reports a writer intervened, it sets *seq = 1 (odd => locking pass) and
 * goes again, now holding the writer lock so nothing can intervene.
 */
static inline void read_seqbegin_or_lock(seqlock_t *sl, unsigned long *seq)
{
	if (!(*seq & 1UL))
		*seq = read_seqbegin(sl);
	else
		read_seqlock_excl(sl);
}

static inline int need_seqretry(seqlock_t *sl, unsigned long seq)
{
	return !(seq & 1UL) && read_seqretry(sl, seq);
}

static inline void done_seqretry(seqlock_t *sl, unsigned long seq)
{
	if (seq & 1UL)
		read_sequnlock_excl(sl);
}

#endif /* DCACHE_SEQCOUNT_H */
