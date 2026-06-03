/*
 * C++ shim exposing ART-ROWEX (concurrent ART, Read-Optimized Write EXclusion;
 * same flode/ARTSynchronized repo as ART-OLC, Apache-2.0) to the bind9
 * load-names benchmark.
 *
 * Compiled separately by g++ with the vendored ROWEX unity source and linked
 * into load-names with -ltbb -lstdc++.  Like the OLC core, the ROWEX unity
 * compiles its own Epoche.cpp; ART::Epoche's out-of-line symbols are weak/local,
 * so the two ART cores coexist in load-names without colliding.
 *
 * Same validation model as the OLC shim: ART stores only a TID per leaf and
 * validates a candidate via loadKey(TID); the TID load-names passes is a
 * pointer to an artrowex_kv copy of the qpkey in a NUMA-interleaved arena, so
 * loadKey (validation) and the benchmark's force-read hit cold, separate
 * memory.  ROWEX readers never restart (writers exclude them per node).
 *
 * Read-only after build: a per-thread ART::ThreadInfo is all that is needed.
 * Best-effort destroy; run one thread count per process (BENCH_THREADS=N).
 */
#include "Key.h"
#include "Epoche.h"
#include "ROWEX/Tree.h"

#include <cstdint>
#include <cstring>

/* Value layout the TID points at; must match load-names.c's allocation. */
struct artrowex_kv {
	uint32_t len;
	uint8_t bytes[];
};

static void load_key(TID tid, Key &key)
{
	artrowex_kv *kv = reinterpret_cast<artrowex_kv *>(tid);
	key.set(reinterpret_cast<const char *>(kv->bytes), kv->len);
}

extern "C" {

void *artrowex_create(void)
{
	return new ART_ROWEX::Tree(load_key);
}

void *artrowex_thread_init(void *tree)
{
	return new ART::ThreadInfo(
		static_cast<ART_ROWEX::Tree *>(tree)->getThreadInfo());
}

void artrowex_insert(void *tree, void *ti, const char *key, int len,
		     uint64_t tid_val)
{
	Key k;
	k.set(key, len);
	static_cast<ART_ROWEX::Tree *>(tree)->insert(k, (TID)tid_val,
		*static_cast<ART::ThreadInfo *>(ti));
}

uint64_t artrowex_lookup(void *tree, void *ti, const char *key, int len)
{
	Key k;
	k.set(key, len);
	return (uint64_t)static_cast<ART_ROWEX::Tree *>(tree)->lookup(k,
		*static_cast<ART::ThreadInfo *>(ti));
}

void artrowex_destroy(void *tree)
{
	delete static_cast<ART_ROWEX::Tree *>(tree);	/* best effort */
}

} /* extern "C" */
