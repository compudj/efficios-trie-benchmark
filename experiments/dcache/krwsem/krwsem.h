/* SPDX-License-Identifier: GPL-2.0 */
/*
 * krwsem.h -- opaque public wrapper around the vendored Linux kernel
 * rw_semaphore (rwsem.c + kcompat.h).  Consumers include ONLY this header, so
 * none of the kernel-compat macros (current, READ_ONCE, smp_*, ...) leak into
 * their translation unit.
 *
 * The storage is a fixed 56-byte buffer -- the same size as pthread_rwlock_t --
 * so a struct that embeds a krwsem in place of a pthread_rwlock keeps the exact
 * same footprint, which matters for the dcache dentry A/B.  GPL-2.0 (it fronts
 * GPL kernel code).
 */
#ifndef KRWSEM_H
#define KRWSEM_H

#define KRWSEM_SIZE 56
struct krwsem { unsigned char buf[KRWSEM_SIZE] __attribute__((aligned(8))); };

void krwsem_init(struct krwsem *l);
void krwsem_rdlock(struct krwsem *l);
void krwsem_rdunlock(struct krwsem *l);
void krwsem_wrlock(struct krwsem *l);
void krwsem_wrunlock(struct krwsem *l);

#endif /* KRWSEM_H */
