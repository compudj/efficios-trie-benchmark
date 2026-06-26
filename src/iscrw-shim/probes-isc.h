/* Stub: ISC SystemTap/dtrace probes as no-ops (faithful: probes don't alter lock behavior). */
#ifndef BENCH_PROBES_ISC_STUB_H
#define BENCH_PROBES_ISC_STUB_H
#define LIBISC_RWLOCK_RDLOCK_REQ(...)  ((void)0)
#define LIBISC_RWLOCK_RDLOCK_ACQ(...)  ((void)0)
#define LIBISC_RWLOCK_TRYRDLOCK(...)   ((void)0)
#define LIBISC_RWLOCK_RDUNLOCK(...)    ((void)0)
#define LIBISC_RWLOCK_WRLOCK_REQ(...)  ((void)0)
#define LIBISC_RWLOCK_WRLOCK_ACQ(...)  ((void)0)
#define LIBISC_RWLOCK_WRUNLOCK(...)    ((void)0)
#define LIBISC_RWLOCK_TRYWRLOCK(...)   ((void)0)
#define LIBISC_RWLOCK_TRYUPGRADE(...)  ((void)0)
#define LIBISC_RWLOCK_DOWNGRADE(...)   ((void)0)
#define LIBISC_RWLOCK_INIT(...)        ((void)0)
#define LIBISC_RWLOCK_DESTROY(...)     ((void)0)
#endif
