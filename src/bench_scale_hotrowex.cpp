/*
 * HOTRowex engine for the read/write scaling benchmark.
 *
 * HOTRowex is HOT's concurrent variant: it synchronizes with ROWEX
 * (Read-Optimized Write EXclusion) rather than RCU.  Readers are optimistic
 * and lock-free (they restart on a concurrent structural change); a single
 * writer mutates in place under per-node atomics.  Stale nodes are reclaimed
 * with epoch-based reclamation backed by TBB's enumerable_thread_specific.
 *
 * Two consequences shape this engine:
 *   1. Reads SELF-GUARD the epoch.  Every lookup()/insert()/upsert() opens a
 *      RAII MemoryGuard internally, and the per-thread epoch slot is created
 *      lazily on first touch.  So reader/writer threads need no explicit
 *      registration — unlike the FT engine, there is no reader_setup/teardown.
 *   2. ROWEX has NO delete (concurrent deletion is unimplemented upstream;
 *      it exposes lookup/scan/insert/insertGuarded/upsert only).  The other
 *      bench_scale engines churn by toggling keys in/out; HOTRowex instead
 *      churns via upsert() of the churn-key set — point value-updates that
 *      still drive the full ROWEX write path (descend + atomic leaf swap)
 *      under concurrent readers.  This asymmetry is deliberate and documented.
 *
 * Self-contained: links neither bind9 nor liburcu, only TBB + the header-only
 * HOT (ISC).  Built standalone by the top-level Makefile (make bench_scale_hotrowex).
 */
#include <hot/rowex/HOTRowex.hpp>
#include <idx/contenthelpers/IdentityKeyExtractor.hpp>
#include <idx/contenthelpers/OptionalValue.hpp>

#include "bench_scale_common.h"

#include <cstdio>
#include <cstring>

using HotRowex =
	hot::rowex::HOTRowex<const char *, idx::contenthelpers::IdentityKeyExtractor>;

static HotRowex *g_hr;

/* Sink so the optimizer cannot elide the lookup loop. */
static volatile uint64_t g_sink;

/*
 * External nodes: a compact arena of key COPIES that the stored values point
 * at -- NOT pointers into the shared query-key buffer.  Storing input pointers
 * would let contentEquals validate against the very bytes the query already
 * touched (near-free) and unfairly favour HOTRowex; copies make it pay a read
 * to separate memory, the same handicap FT's stored-key memcmp pays.  Compact
 * + NUMA-interleavable for the same reasons as FT's arena.
 */
static struct bench_arena hot_arena;

/* A second pointer per churn key with identical bytes: the value IS the key
 * (IdentityKeyExtractor), so the mutator's REPLACE upserts this distinct
 * pointer to actually swap the stored leaf value at the same trie key. */
static char *churn_copy_b[CHURN_KEYS];

extern "C" void hotrowex_build(void)
{
	g_hr = new HotRowex();

	size_t arena_bytes = 0;
	for (unsigned int i = 0; i < N_KEYS; i++)
		arena_bytes += (str_lens[i] + 1 + 7) & ~(size_t)7;
	bench_arena_init(&hot_arena, arena_bytes);

	for (unsigned int i = 0; i < N_KEYS; i++) {
		char *copy = (char *)bench_arena_alloc(&hot_arena,
			str_lens[i] + 1, 8);
		memcpy(copy, str_keys[i], str_lens[i]);
		copy[str_lens[i]] = '\0';
		g_hr->insert(copy);
	}

	for (int c = 0; c < CHURN_KEYS; c++) {
		char *b = (char *)malloc(churn_lens[c] + 1);
		memcpy(b, churn_keys[c], churn_lens[c]);
		b[churn_lens[c]] = '\0';
		churn_copy_b[c] = b;
	}
}

/*
 * Mutator-benchmark op.  ROWEX has no delete (no_remove=1, REMOVE never
 * called), so INSERT and REPLACE are both upserts: INSERT stores the churn
 * key's own pointer, REPLACE stores the distinct same-bytes copy, so the leaf
 * value genuinely changes.  Both drive the full ROWEX write path.
 */
extern "C" void hotrowex_writer_op(void *ctx, int op, unsigned int idx)
{
	(void)ctx;
	switch (op) {
	case BENCH_OP_INSERT:
		g_hr->upsert(churn_keys[idx]);
		break;
	case BENCH_OP_REPLACE:
		g_hr->upsert(churn_copy_b[idx]);
		break;
	case BENCH_OP_REMOVE:
		break;	/* no_remove */
	}
}

extern "C" void hotrowex_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	uint64_t acc = 0;
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		/* lookup() opens its own MemoryGuard (epoch enter/leave). */
		auto r = g_hr->lookup(str_keys[idx]);
		if (r.mIsValid) {
			/*
			 * Force-read the validated leaf (the stored key copy):
			 * prevents DCE of contentEquals and pays the same
			 * post-lookup cache-line touch as FT's force-read.
			 */
			asm volatile("" :: "r"(*(const volatile uint8_t *)r.mValue)
				: "memory");
			acc++;
		}
	}
	g_sink = acc;
}

extern "C" void hotrowex_writer_step(void *ctx, uint64_t *seed,
				     unsigned long writes)
{
	(void)ctx;
	(void)writes;
	unsigned int cidx = xorshift64(seed) % CHURN_KEYS;
	/*
	 * ROWEX has no delete, so the writer cannot toggle keys in/out the way
	 * the RCU/lock engines do.  upsert() instead: it inserts the churn key
	 * on first touch and rewrites its leaf thereafter, exercising the full
	 * ROWEX write path (descend + atomic leaf swap + epoch) concurrently
	 * with the readers traversing the same nodes.
	 */
	g_hr->upsert(churn_keys[cidx]);
}

/* Ordered-iteration op: HOT's synchronized iterator walks keys in sorted order
 * (it self-guards the epoch like lookup). */
extern "C" unsigned long hotrowex_iterate(void *ctx)
{
	(void)ctx;
	unsigned long n = 0;
	for (auto it = g_hr->begin(); it != g_hr->end(); ++it)
		n++;
	return n;
}

static const struct bench_engine hotrowex_engine = {
	"hotrowex",		/* name */
	"HOTRowex",		/* label */
	hotrowex_build,		/* build */
	nullptr,		/* reader_setup */
	hotrowex_reader_batch,	/* reader_batch */
	nullptr,		/* reader_teardown */
	nullptr,		/* writer_setup */
	hotrowex_writer_step,	/* writer_step */
	nullptr,		/* writer_teardown */
	nullptr,		/* run_reset */
	nullptr,		/* cleanup_churn */
	hotrowex_writer_op,	/* writer_op */
	1,			/* no_remove (ROWEX has no concurrent delete) */
	hotrowex_iterate,	/* iterate */
};

int main(int argc, char **argv)
{
	return bench_scale_main(argc, argv, &hotrowex_engine);
}
