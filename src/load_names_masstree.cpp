/*
 * C++ shim exposing Masstree (kohler/masstree-beta, MIT) to the bind9
 * load-names lookup-scaling benchmark via a small extern "C" API.
 *
 * Compiled separately by g++ together with the vendored Masstree core sources
 * (scripts/build-bind9.sh) and linked into load-names with -lstdc++.  Masstree
 * keys on lcdf::Str(bytes, len) -- arbitrary binary, so it takes the dns_qpkey
 * bytes directly (no NUL-termination needed, unlike the HOTRowex shim).
 *
 * load-names is read-only after build, so there are no concurrent removes and
 * thus no epoch-GC discipline to drive during the timed phase: each worker just
 * needs its own threadinfo (rcu_start once).  Build-time split garbage and the
 * tree itself are reclaimed best-effort in masstree_destroy; run one thread
 * count per process (BENCH_THREADS=N) so any residual leak is bounded to a
 * single build rather than accumulating across the default sweep.
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

/* Globals every Masstree embedder must define. */
relaxed_atomic<mrcu_epoch_type> globalepoch{1};
relaxed_atomic<mrcu_epoch_type> active_epoch{1};

class mt_params : public Masstree::nodeparams<15, 15> {
public:
	typedef void *value_type;
	typedef threadinfo threadinfo_type;
};
typedef Masstree::basic_table<mt_params> table_type;

static int g_thread_idx;

extern "C" {

/* Create the table (initialized with a main threadinfo). */
void *masstree_create(void)
{
	threadinfo *main_ti = threadinfo::make(threadinfo::TI_MAIN, -1);
	main_ti->rcu_start();
	table_type *t = new table_type();
	t->initialize(*main_ti);
	return t;
}

/* Make this worker thread's threadinfo (returned as an opaque handle). */
void *masstree_thread_init(void)
{
	int idx = __atomic_fetch_add(&g_thread_idx, 1, __ATOMIC_RELAXED);
	threadinfo *ti = threadinfo::make(threadinfo::TI_PROCESS, idx);
	ti->rcu_start();
	return ti;
}

void masstree_insert(void *table, void *ti, const char *key, int len,
		     void *val)
{
	table_type *t = static_cast<table_type *>(table);
	Masstree::tcursor<mt_params> lp(*t, key, len);
	lp.find_insert(*static_cast<threadinfo *>(ti));
	lp.value() = val;
	lp.finish(1, *static_cast<threadinfo *>(ti));
}

/* Validated point lookup; returns the stored value or NULL. */
void *masstree_lookup(void *table, void *ti, const char *key, int len)
{
	table_type *t = static_cast<table_type *>(table);
	void *v = nullptr;
	if (t->get(lcdf::Str(key, len), v, *static_cast<threadinfo *>(ti)))
		return v;
	return nullptr;
}

void masstree_destroy(void *table)
{
	table_type *t = static_cast<table_type *>(table);
	threadinfo *ti = threadinfo::make(threadinfo::TI_MAIN, -1);
	ti->rcu_start();
	t->destroy(*ti);		/* free the tree (best effort) */
	globalepoch.store(globalepoch.load() + 2);
	active_epoch.store(globalepoch.load());
	ti->rcu_quiesce();
	delete t;
}

} /* extern "C" */
