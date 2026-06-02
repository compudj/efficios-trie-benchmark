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

using HotRowex =
	hot::rowex::HOTRowex<const char *, idx::contenthelpers::IdentityKeyExtractor>;

static HotRowex *g_hr;

/* Sink so the optimizer cannot elide the lookup loop. */
static volatile uint64_t g_sink;

extern "C" void hotrowex_build(void)
{
	g_hr = new HotRowex();
	for (unsigned int i = 0; i < N_KEYS; i++)
		g_hr->insert(str_keys[i]);
}

extern "C" void hotrowex_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	uint64_t acc = 0;
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		/* lookup() opens its own MemoryGuard (epoch enter/leave). */
		acc += g_hr->lookup(str_keys[idx]).mIsValid;
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
};

int main(int argc, char **argv)
{
	return bench_scale_main(argc, argv, &hotrowex_engine);
}
