/*
 * C++ shim exposing ART-OLC (concurrent ART, Optimistic Lock Coupling;
 * flode/ARTSynchronized, Apache-2.0) to the bind9 load-names benchmark.
 *
 * Compiled separately by g++ with the vendored ART-OLC unity source and linked
 * into load-names with -ltbb -lstdc++.  ART stores no full keys in its nodes,
 * only a TID per leaf; it validates a candidate by calling back loadKey(TID) to
 * materialize the stored key.  The TID load-names passes is a pointer to a copy
 * of the qpkey (an artolc_kv) in a NUMA-interleaved arena, so loadKey (the
 * validation) and the benchmark's force-read hit cold, separate memory.
 *
 * Read-only after build: ART_OLC's ops open the epoch guard internally, so a
 * per-thread ART::ThreadInfo is all that is needed.  Best-effort destroy; run
 * one thread count per process (BENCH_THREADS=N).
 */
#include "Key.h"
#include "Epoche.h"
#include "OptimisticLockCoupling/Tree.h"

#include <cstdint>
#include <cstring>

/* Value layout the TID points at; must match load-names.c's allocation. */
struct artolc_kv {
	uint32_t len;
	uint8_t bytes[];
};

static void load_key(TID tid, Key &key)
{
	artolc_kv *kv = reinterpret_cast<artolc_kv *>(tid);
	key.set(reinterpret_cast<const char *>(kv->bytes), kv->len);
}

extern "C" {

void *artolc_create(void)
{
	return new ART_OLC::Tree(load_key);
}

void *artolc_thread_init(void *tree)
{
	return new ART::ThreadInfo(
		static_cast<ART_OLC::Tree *>(tree)->getThreadInfo());
}

void artolc_insert(void *tree, void *ti, const char *key, int len,
		   uint64_t tid_val)
{
	Key k;
	k.set(key, len);
	static_cast<ART_OLC::Tree *>(tree)->insert(k, (TID)tid_val,
		*static_cast<ART::ThreadInfo *>(ti));
}

uint64_t artolc_lookup(void *tree, void *ti, const char *key, int len)
{
	Key k;
	k.set(key, len);
	return (uint64_t)static_cast<ART_OLC::Tree *>(tree)->lookup(k,
		*static_cast<ART::ThreadInfo *>(ti));
}

void artolc_destroy(void *tree)
{
	delete static_cast<ART_OLC::Tree *>(tree);	/* best effort */
}

} /* extern "C" */
