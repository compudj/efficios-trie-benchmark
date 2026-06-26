/*
 * Isolation wrapper for the ISC C-RW-WP reader/writer lock
 * (bind9 lib/isc/rwlock.c, the "Modified C-RW-WP" phase-fair NUMA-aware lock).
 *
 * The isc/ headers define short macros (REQUIRE, UNUSED, ...) that we do not
 * want leaking into the benchmark translation unit, so the benchmark only sees
 * the small opaque API below; the real isc_rwlock_t lives here.  The actual
 * lock object is allocated by the caller (cacheline-padded) via the size/align
 * accessors.  We link the unmodified lib/isc/rwlock.c against this TU, with a
 * no-op probes-isc.h shim and the assertion backend below (so we avoid pulling
 * in all of libisc -- whose constructor would register main with RCU).
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <isc/rwlock.h>

size_t bench_iscrw_size(void)  { return sizeof(isc_rwlock_t); }
size_t bench_iscrw_align(void) { return _Alignof(isc_rwlock_t); }

void bench_iscrw_init(void *p)     { isc_rwlock_init((isc_rwlock_t *) p); }
void bench_iscrw_rdlock(void *p)   { isc_rwlock_rdlock((isc_rwlock_t *) p); }
void bench_iscrw_rdunlock(void *p) { isc_rwlock_rdunlock((isc_rwlock_t *) p); }
void bench_iscrw_wrlock(void *p)   { isc_rwlock_wrlock((isc_rwlock_t *) p); }
void bench_iscrw_wrunlock(void *p) { isc_rwlock_wrunlock((isc_rwlock_t *) p); }

/*
 * REQUIRE()/INSIST() backend used by the linked-in rwlock.c.  We provide just
 * this one symbol rather than libisc's assertions.c, so no isc constructor runs.
 */
__attribute__((noreturn))
void isc_assertion_failed(const char *file, int line,
		isc_assertiontype_t type, const char *cond)
{
	fprintf(stderr, "ISC assertion failed: %s:%d (type %d): %s\n",
		file, line, (int) type, cond);
	abort();
}
