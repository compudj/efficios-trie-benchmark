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
 * writer ran in between (sequence changed, or was odd) it retries.  This is the
 * exact machinery the urcu-txn port replaces with per-slot read-set validation.
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
 * A "raw" begin that does NOT spin on an in-flight writer (returns the odd
 * value); the matching read_seqcount_retry() catches it.  __d_lookup_rcu uses
 * this per-dentry so a reader never blocks on a dentry a writer is touching.
 */
static inline unsigned long raw_read_seqcount(const seqcount_t *s)
{
	unsigned long ret = CMM_LOAD_SHARED(s->sequence);

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

typedef struct {
	seqcount_t seq;
	pthread_spinlock_t lock;
} seqlock_t;

static inline void seqlock_init(seqlock_t *sl)
{
	seqcount_init(&sl->seq);
	pthread_spin_init(&sl->lock, PTHREAD_PROCESS_PRIVATE);
}

static inline void seqlock_destroy(seqlock_t *sl)
{
	pthread_spin_destroy(&sl->lock);
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
	pthread_spin_lock(&sl->lock);
	write_seqcount_begin(&sl->seq);
}

static inline void write_sequnlock(seqlock_t *sl)
{
	write_seqcount_end(&sl->seq);
	pthread_spin_unlock(&sl->lock);
}

#endif /* DCACHE_SEQCOUNT_H */
