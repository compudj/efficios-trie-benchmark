/*
 * Masstree engine for the read/write scaling benchmark.
 *
 * Masstree (Mao/Kohler/Morris, EuroSys'12; kohler/masstree-beta, MIT) is a
 * concurrent B+tree of tries: optimistic version-validated readers, fine-
 * grained-locked writers, and epoch-based reclamation of removed nodes.  This
 * engine drives it through the low-level cursor API and matches the fairness
 * the other engines get: each lookup key is stored as a COPY in the dense
 * bench_arena (Masstree's value points at it, not into the shared query
 * buffer), and the reader force-reads the returned value so the post-lookup
 * leaf touch is paid (Masstree itself validates the full key during descent).
 *
 * Concurrency discipline (Masstree's RCU-like epochs): every worker thread gets
 * its own threadinfo, rcu_start() at setup / rcu_stop() at teardown (so a dead
 * thread's gc_epoch_==0 stops holding back min_active_epoch), and rcu_quiesce()
 * periodically.  The writer advances globalepoch and republishes
 * active_epoch = min_active_epoch() so removed nodes actually reclaim.
 *
 * Self-contained: no bind9/liburcu; built standalone by the top-level Makefile.
 */
#include "config.h"
#include "compiler.hh"
#include "kvthread.hh"
#include "str.hh"
#include "masstree.hh"
#include "masstree_tcursor.hh"
#include "masstree_insert.hh"
#include "masstree_get.hh"
#include "masstree_remove.hh"

#include "bench_scale_common.h"

#include <cstdio>
#include <cstring>

/* Globals every Masstree embedder must define. */
relaxed_atomic<mrcu_epoch_type> globalepoch{1};
relaxed_atomic<mrcu_epoch_type> active_epoch{1};

class mt_params : public Masstree::nodeparams<15, 15> {
public:
	typedef void *value_type;
	typedef threadinfo threadinfo_type;
};
typedef Masstree::basic_table<mt_params> table_type;

static table_type g_table;
static threadinfo *g_main_ti;
static struct bench_arena g_mt_arena;	/* lookup-key copies */
static volatile uint64_t g_sink;

/* Distinct threadinfo index per worker. */
static int g_thread_idx;

/* Which churn keys are currently inserted (value pointer, or NULL). */
static char *churn_present[CHURN_KEYS];
static pthread_mutex_t g_mt_mutex = PTHREAD_MUTEX_INITIALIZER;

static threadinfo *make_worker_ti(void)
{
	int idx = __atomic_fetch_add(&g_thread_idx, 1, __ATOMIC_RELAXED);
	threadinfo *ti = threadinfo::make(threadinfo::TI_PROCESS, idx);
	ti->rcu_start();
	return ti;
}

extern "C" void masstree_build(void)
{
	g_main_ti = threadinfo::make(threadinfo::TI_MAIN, -1);
	g_main_ti->rcu_start();
	g_table.initialize(*g_main_ti);

	size_t bytes = 0;
	for (unsigned int i = 0; i < N_KEYS; i++)
		bytes += (str_lens[i] + 1 + 7) & ~(size_t)7;
	bench_arena_init(&g_mt_arena, bytes);

	for (unsigned int i = 0; i < N_KEYS; i++) {
		char *copy = (char *)bench_arena_alloc(&g_mt_arena,
			str_lens[i] + 1, 8);
		memcpy(copy, str_keys[i], str_lens[i]);
		copy[str_lens[i]] = '\0';
		Masstree::tcursor<mt_params> lp(g_table, str_keys[i],
			(int)str_lens[i]);
		lp.find_insert(*g_main_ti);
		lp.value() = copy;
		lp.finish(1, *g_main_ti);
	}
	g_main_ti->rcu_stop();		/* main builds, then goes inactive */
}

extern "C" void *masstree_reader_setup(void)
{
	return make_worker_ti();
}

extern "C" void masstree_reader_teardown(void *ctx)
{
	((threadinfo *)ctx)->rcu_stop();
}

extern "C" void masstree_reader_batch(void *ctx, uint64_t *seed)
{
	threadinfo *ti = (threadinfo *)ctx;
	uint64_t acc = 0;
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		void *v = nullptr;
		if (g_table.get(lcdf::Str(str_keys[idx], (int)str_lens[idx]),
				v, *ti) && v != nullptr) {
			/* Force-read the value (cold arena key copy). */
			acc += *(const volatile uint8_t *)v;
		}
	}
	g_sink = acc;
	ti->rcu_quiesce();
}

extern "C" void *masstree_writer_setup(void)
{
	return make_worker_ti();
}

extern "C" void masstree_writer_teardown(void *ctx)
{
	((threadinfo *)ctx)->rcu_stop();
}

extern "C" void masstree_writer_step(void *ctx, uint64_t *seed,
				     unsigned long writes)
{
	threadinfo *ti = (threadinfo *)ctx;
	unsigned int cidx = xorshift64(seed) % CHURN_KEYS;

	pthread_mutex_lock(&g_mt_mutex);
	if (churn_present[cidx]) {
		Masstree::tcursor<mt_params> lp(g_table, churn_keys[cidx],
			(int)churn_lens[cidx]);
		bool found = lp.find_locked(*ti);
		lp.finish(found ? -1 : 0, *ti);
		churn_present[cidx] = nullptr;
	} else {
		/* The churn value is not read by readers; the input pointer
		 * doubles as the presence flag. */
		Masstree::tcursor<mt_params> lp(g_table, churn_keys[cidx],
			(int)churn_lens[cidx]);
		lp.find_insert(*ti);
		lp.value() = churn_keys[cidx];
		lp.finish(1, *ti);
		churn_present[cidx] = churn_keys[cidx];
	}
	pthread_mutex_unlock(&g_mt_mutex);

	if (writes % 1000 == 0) {
		/* Advance the epoch so removed nodes can reclaim. */
		globalepoch.store(globalepoch.load() + 1);
		active_epoch.store(threadinfo::min_active_epoch());
	}
	ti->rcu_quiesce();
}

extern "C" void masstree_run_reset(void)
{
	memset(churn_present, 0, sizeof(churn_present));
}

static const struct bench_engine masstree_engine = {
	"masstree",		/* name */
	"Masstree",		/* label */
	masstree_build,		/* build */
	masstree_reader_setup,	/* reader_setup */
	masstree_reader_batch,	/* reader_batch */
	masstree_reader_teardown, /* reader_teardown */
	masstree_writer_setup,	/* writer_setup */
	masstree_writer_step,	/* writer_step */
	masstree_writer_teardown, /* writer_teardown */
	masstree_run_reset,	/* run_reset */
	nullptr,		/* cleanup_churn */
};

int main(int argc, char **argv)
{
	return bench_scale_main(argc, argv, &masstree_engine);
}
