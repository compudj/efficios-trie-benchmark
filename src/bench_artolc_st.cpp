/*
 * Single-threaded ART-OLC shim for bench_one_st (flode/ARTSynchronized,
 * Apache-2.0).  The concurrent (Optimistic Lock Coupling) ART run with one
 * ThreadInfo -- it carries the version/epoch machinery, no contention.  ART
 * stores only a TID per leaf and validates via loadKey(TID); the TID is a
 * pointer to a caller-owned record (an artolc_st_kv copy of the key), so
 * loadKey and the harness force-read hit cold memory.  ART needs byte-prefix-
 * free keys: integer keys are fixed length (prefix-free); the caller appends a
 * NUL to string keys.
 */
#include "Key.h"
#include "Epoche.h"
#include "OptimisticLockCoupling/Tree.h"

#include <cstdint>
#include <cstring>

struct artolc_st_kv {		/* must match bench_one_st.c's allocation */
	uint32_t len;
	uint8_t bytes[];
};

static void load_key(TID tid, Key &key)
{
	artolc_st_kv *kv = reinterpret_cast<artolc_st_kv *>(tid);
	key.set(reinterpret_cast<const char *>(kv->bytes), kv->len);
}

static ART_OLC::Tree *g_tree;
static ART::ThreadInfo *g_ti;

extern "C" {

void *artolc_st_create(void)
{
	g_tree = new ART_OLC::Tree(load_key);
	g_ti = new ART::ThreadInfo(g_tree->getThreadInfo());
	return g_tree;
}

/* @key/@len is the lookup key (prefix-free); @tid_val points at the kv record. */
void artolc_st_insert(const void *key, int len, uint64_t tid_val)
{
	Key k;
	k.set(static_cast<const char *>(key), len);
	g_tree->insert(k, (TID)tid_val, *g_ti);
}

uint64_t artolc_st_lookup(const void *key, int len)
{
	Key k;
	k.set(static_cast<const char *>(key), len);
	return (uint64_t)g_tree->lookup(k, *g_ti);
}

} /* extern "C" */
