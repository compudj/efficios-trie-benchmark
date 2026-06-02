/*
 * ART-OLC engine for the read/write scaling benchmark.
 *
 * ART-OLC (Optimistic Lock Coupling) is the concurrent adaptive radix tree of
 * Leis et al. ("The ART of Practical Synchronization", DaMoN'16;
 * flode/ARTSynchronized, Apache-2.0): readers descend optimistically validating
 * per-node versions and restart on a concurrent change; writers lock-couple
 * down the path; removed nodes reclaim via an epoch (TBB-backed).
 *
 * ART stores no full keys in its nodes — only a TID per leaf — and validates a
 * candidate by calling back loadKey(TID) to materialize the stored key.  We make
 * the TID a pointer to a key COPY in the dense bench_arena, so loadKey (the
 * validation) reads cold, separate memory, and the reader force-reads the TID:
 * the same fairness the other engines get.  Each lookup constructs an ART Key
 * (a 128-byte stack object Key::set copies into) — the API's real per-lookup
 * cost.  Per-thread epoch state is an ART::ThreadInfo; ART_OLC's ops open the
 * epoch guard internally, so no manual reclamation discipline is needed.
 *
 * Self-contained: no bind9/liburcu; built standalone by the top-level Makefile.
 */
#include "Key.h"
#include "Epoche.h"
#include "OptimisticLockCoupling/Tree.h"

#include "bench_scale_common.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

struct artolc_kv {
	uint32_t len;
	uint8_t bytes[];
};

static void load_key(TID tid, Key &key)
{
	artolc_kv *kv = reinterpret_cast<artolc_kv *>(tid);
	key.set(reinterpret_cast<const char *>(kv->bytes), kv->len);
}

static ART_OLC::Tree *g_tree;
static ART::ThreadInfo *g_main_ti;
static struct bench_arena g_arena;	/* lookup-key copies */
static volatile uint64_t g_sink;

/* Pre-allocated churn key copies (reused across insert/remove) + presence. */
static artolc_kv *churn_kv[CHURN_KEYS];
static char churn_present[CHURN_KEYS];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline TID kv_tid(artolc_kv *kv) { return reinterpret_cast<TID>(kv); }

extern "C" void artolc_build(void)
{
	g_tree = new ART_OLC::Tree(load_key);
	g_main_ti = new ART::ThreadInfo(g_tree->getThreadInfo());

	/* Key on len+1 bytes (incl. the NUL terminator) so the keys are
	 * byte-prefix-free, as ART (a byte radix tree) requires. */
	size_t bytes = 0;
	for (unsigned int i = 0; i < N_KEYS; i++)
		bytes += (sizeof(artolc_kv) + str_lens[i] + 1 + 7) & ~(size_t)7;
	bench_arena_init(&g_arena, bytes);

	for (unsigned int i = 0; i < N_KEYS; i++) {
		artolc_kv *kv = (artolc_kv *)bench_arena_alloc(&g_arena,
			sizeof(artolc_kv) + str_lens[i] + 1, 8);
		kv->len = str_lens[i] + 1;
		memcpy(kv->bytes, str_keys[i], str_lens[i] + 1);
		Key key;
		key.set(str_keys[i], str_lens[i] + 1);
		g_tree->insert(key, kv_tid(kv), *g_main_ti);
	}

	for (int c = 0; c < CHURN_KEYS; c++) {
		artolc_kv *kv = (artolc_kv *)malloc(sizeof(artolc_kv) +
			churn_lens[c] + 1);
		kv->len = churn_lens[c] + 1;
		memcpy(kv->bytes, churn_keys[c], churn_lens[c] + 1);
		churn_kv[c] = kv;
	}
}

extern "C" void *artolc_reader_setup(void)
{
	return new ART::ThreadInfo(g_tree->getThreadInfo());
}

extern "C" void artolc_reader_teardown(void *ctx)
{
	delete static_cast<ART::ThreadInfo *>(ctx);
}

extern "C" void artolc_reader_batch(void *ctx, uint64_t *seed)
{
	ART::ThreadInfo *ti = static_cast<ART::ThreadInfo *>(ctx);
	uint64_t acc = 0;
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		Key key;
		key.set(str_keys[idx], str_lens[idx] + 1);
		TID tid = g_tree->lookup(key, *ti);
		if (tid)
			acc += *(const volatile uint8_t *)tid;
	}
	g_sink = acc;
}

extern "C" void *artolc_writer_setup(void)
{
	return new ART::ThreadInfo(g_tree->getThreadInfo());
}

extern "C" void artolc_writer_teardown(void *ctx)
{
	delete static_cast<ART::ThreadInfo *>(ctx);
}

extern "C" void artolc_writer_step(void *ctx, uint64_t *seed,
				   unsigned long writes)
{
	ART::ThreadInfo *ti = static_cast<ART::ThreadInfo *>(ctx);
	(void)writes;
	unsigned int cidx = xorshift64(seed) % CHURN_KEYS;
	Key key;
	key.set(churn_keys[cidx], churn_lens[cidx] + 1);

	pthread_mutex_lock(&g_mutex);
	if (churn_present[cidx]) {
		g_tree->remove(key, kv_tid(churn_kv[cidx]), *ti);
		churn_present[cidx] = 0;
	} else {
		g_tree->insert(key, kv_tid(churn_kv[cidx]), *ti);
		churn_present[cidx] = 1;
	}
	pthread_mutex_unlock(&g_mutex);
}

extern "C" void artolc_run_reset(void)
{
	memset(churn_present, 0, sizeof(churn_present));
}

extern "C" void artolc_cleanup_churn(void)
{
	for (int c = 0; c < CHURN_KEYS; c++) {
		if (churn_present[c]) {
			Key key;
			key.set(churn_keys[c], churn_lens[c] + 1);
			g_tree->remove(key, kv_tid(churn_kv[c]), *g_main_ti);
			churn_present[c] = 0;
		}
	}
}

static const struct bench_engine artolc_engine = {
	"artolc",		/* name */
	"ART-OLC",		/* label */
	artolc_build,		/* build */
	artolc_reader_setup,	/* reader_setup */
	artolc_reader_batch,	/* reader_batch */
	artolc_reader_teardown,	/* reader_teardown */
	artolc_writer_setup,	/* writer_setup */
	artolc_writer_step,	/* writer_step */
	artolc_writer_teardown,	/* writer_teardown */
	artolc_run_reset,	/* run_reset */
	artolc_cleanup_churn,	/* cleanup_churn */
};

int main(int argc, char **argv)
{
	return bench_scale_main(argc, argv, &artolc_engine);
}
