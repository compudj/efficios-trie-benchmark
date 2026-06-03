/*
 * Single-threaded Masstree shim for bench_one_st (kohler/masstree-beta, MIT).
 * Masstree run single-threaded (one threadinfo) -- it carries its concurrency
 * machinery, but there is no contention.  Keys on arbitrary binary
 * lcdf::Str(bytes, len), so it takes the integer-encoded bytes or the string
 * bytes directly.  The value is a caller-owned cold record (the kv_entry the
 * harness force-reads), not the key.
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

relaxed_atomic<mrcu_epoch_type> globalepoch{1};
relaxed_atomic<mrcu_epoch_type> active_epoch{1};

class mt_params : public Masstree::nodeparams<15, 15> {
public:
	typedef void *value_type;
	typedef threadinfo threadinfo_type;
};
typedef Masstree::basic_table<mt_params> table_type;

static table_type *g_table;
static threadinfo *g_ti;

extern "C" {

void *masstree_st_create(void)
{
	g_ti = threadinfo::make(threadinfo::TI_MAIN, -1);
	g_ti->rcu_start();
	g_table = new table_type();
	g_table->initialize(*g_ti);
	return g_table;
}

void masstree_st_insert(const void *key, int len, void *val)
{
	Masstree::tcursor<mt_params> lp(*g_table,
		static_cast<const char *>(key), len);
	lp.find_insert(*g_ti);
	lp.value() = val;
	lp.finish(1, *g_ti);
}

void *masstree_st_lookup(const void *key, int len)
{
	void *v = nullptr;
	g_table->get(lcdf::Str(static_cast<const char *>(key), len), v, *g_ti);
	return v;
}

} /* extern "C" */
