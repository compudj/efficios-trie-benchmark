/*
 * ART-ROWEX engine for the read/write scaling benchmark.
 *
 * ART-ROWEX (Read-Optimized Write EXclusion) is the second concurrent adaptive
 * radix tree of Leis et al. ("The ART of Practical Synchronization", DaMoN'16;
 * flode/ARTSynchronized, Apache-2.0).  Unlike ART-OLC, ROWEX readers never
 * restart: writers take per-node write locks that *exclude* concurrent readers
 * from the affected node (read-optimized write exclusion), so reads are
 * lock-free of retries.  Unlike HOT's ROWEX, ART-ROWEX *does* support remove,
 * so the writer churns by alternating insert/remove like the other engines.
 * Removed nodes reclaim via an epoch (TBB-backed).
 *
 * Same fairness setup as bench_scale_artolc: ART stores only a TID per leaf and
 * validates a candidate via loadKey(TID); the TID is a pointer to a key COPY in
 * the dense bench_arena, so loadKey reads cold, separate memory and the reader
 * force-reads the TID.  Each lookup builds an ART Key (a 128-byte stack object
 * Key::set copies into).  Per-thread epoch state is an ART::ThreadInfo.
 *
 * Self-contained: no bind9/liburcu; built standalone by the top-level Makefile.
 */
#include "Key.h"
#include "Epoche.h"
#include "ROWEX/Tree.h"

#include "bench_scale_common.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

struct artrowex_kv {
	uint32_t len;
	uint8_t bytes[];
};

static void load_key(TID tid, Key &key)
{
	artrowex_kv *kv = reinterpret_cast<artrowex_kv *>(tid);
	key.set(reinterpret_cast<const char *>(kv->bytes), kv->len);
}

static ART_ROWEX::Tree *g_tree;
static ART::ThreadInfo *g_main_ti;
static struct bench_arena g_arena;	/* lookup-key copies */
static volatile uint64_t g_sink;

/* Pre-allocated churn key copies (reused across insert/remove) + presence. */
static artrowex_kv *churn_kv[CHURN_KEYS];
/* A second value object per churn key (same key bytes, distinct pointer) so
 * the mutator's REPLACE actually changes the leaf's TID — ART has no in-place
 * value update, so replace = remove(old) + insert(new). */
static artrowex_kv *churn_kv_b[CHURN_KEYS];
/* Value currently mapped at each churn key in the mutator sweep (or NULL): so
 * REMOVE/REPLACE pass the right TID to ART's remove and the drain knows what to
 * delete.  Touched only by the single mutator thread. */
static artrowex_kv *churn_cur[CHURN_KEYS];
static char churn_present[CHURN_KEYS];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline TID kv_tid(artrowex_kv *kv) { return reinterpret_cast<TID>(kv); }

extern "C" void artrowex_build(void)
{
	g_tree = new ART_ROWEX::Tree(load_key);
	g_main_ti = new ART::ThreadInfo(g_tree->getThreadInfo());

	/* Key on len+1 bytes (incl. the NUL terminator) so the keys are
	 * byte-prefix-free, as ART (a byte radix tree) requires. */
	size_t bytes = 0;
	for (unsigned int i = 0; i < N_KEYS; i++)
		bytes += (sizeof(artrowex_kv) + str_lens[i] + 1 + 7) & ~(size_t)7;
	bench_arena_init(&g_arena, bytes);

	for (unsigned int i = 0; i < N_KEYS; i++) {
		artrowex_kv *kv = (artrowex_kv *)bench_arena_alloc(&g_arena,
			sizeof(artrowex_kv) + str_lens[i] + 1, 8);
		kv->len = str_lens[i] + 1;
		memcpy(kv->bytes, str_keys[i], str_lens[i] + 1);
		Key key;
		key.set(str_keys[i], str_lens[i] + 1);
		g_tree->insert(key, kv_tid(kv), *g_main_ti);
	}

	for (int c = 0; c < CHURN_KEYS; c++) {
		artrowex_kv *kv = (artrowex_kv *)malloc(sizeof(artrowex_kv) +
			churn_lens[c] + 1);
		kv->len = churn_lens[c] + 1;
		memcpy(kv->bytes, churn_keys[c], churn_lens[c] + 1);
		churn_kv[c] = kv;
		artrowex_kv *kvb = (artrowex_kv *)malloc(sizeof(artrowex_kv) +
			churn_lens[c] + 1);
		kvb->len = churn_lens[c] + 1;
		memcpy(kvb->bytes, churn_keys[c], churn_lens[c] + 1);
		churn_kv_b[c] = kvb;
	}
}

/*
 * Mutator-benchmark op (single mutator thread; the tree handles reader/writer
 * safety, so no churn mutex).  REPLACE swaps to the second value object.
 */
extern "C" void artrowex_writer_op(void *ctx, int op, unsigned int idx)
{
	ART::ThreadInfo *ti = static_cast<ART::ThreadInfo *>(ctx);
	Key key;
	key.set(churn_keys[idx], churn_lens[idx] + 1);
	switch (op) {
	case BENCH_OP_INSERT:
		g_tree->insert(key, kv_tid(churn_kv[idx]), *ti);
		churn_cur[idx] = churn_kv[idx];
		break;
	case BENCH_OP_REPLACE:
		if (churn_cur[idx])
			g_tree->remove(key, kv_tid(churn_cur[idx]), *ti);
		g_tree->insert(key, kv_tid(churn_kv_b[idx]), *ti);
		churn_cur[idx] = churn_kv_b[idx];
		break;
	case BENCH_OP_REMOVE:
		if (churn_cur[idx]) {
			g_tree->remove(key, kv_tid(churn_cur[idx]), *ti);
			churn_cur[idx] = nullptr;
		}
		break;
	}
}

extern "C" void *artrowex_reader_setup(void)
{
	return new ART::ThreadInfo(g_tree->getThreadInfo());
}

extern "C" void artrowex_reader_teardown(void *ctx)
{
	delete static_cast<ART::ThreadInfo *>(ctx);
}

extern "C" void artrowex_reader_batch(void *ctx, uint64_t *seed)
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

extern "C" void *artrowex_writer_setup(void)
{
	return new ART::ThreadInfo(g_tree->getThreadInfo());
}

extern "C" void artrowex_writer_teardown(void *ctx)
{
	delete static_cast<ART::ThreadInfo *>(ctx);
}

extern "C" void artrowex_writer_step(void *ctx, uint64_t *seed,
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

extern "C" void artrowex_run_reset(void)
{
	memset(churn_present, 0, sizeof(churn_present));
}

extern "C" void artrowex_cleanup_churn(void)
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

static const struct bench_engine artrowex_engine = {
	"artrowex",		/* name */
	"ART-ROWEX",		/* label */
	artrowex_build,		/* build */
	artrowex_reader_setup,	/* reader_setup */
	artrowex_reader_batch,	/* reader_batch */
	artrowex_reader_teardown,/* reader_teardown */
	artrowex_writer_setup,	/* writer_setup */
	artrowex_writer_step,	/* writer_step */
	artrowex_writer_teardown,/* writer_teardown */
	artrowex_run_reset,	/* run_reset */
	artrowex_cleanup_churn,	/* cleanup_churn */
	artrowex_writer_op,	/* writer_op */
	0,			/* no_remove */
};

int main(int argc, char **argv)
{
	return bench_scale_main(argc, argv, &artrowex_engine);
}
