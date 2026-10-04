/*
 * Fractal Trie (FT) engine for the read/write scaling benchmark.
 *
 * Speculative reference lookup mode (FT's recommended "ft_spec" path):
 * cds_ft_speculative_lookup_key() does a speculative descent and validates the
 * candidate with a library-side memcmp at a fixed key offset
 * (CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE).  This matches the engine the
 * load-names benchmark reports as ft_spec, so the two benchmarks compare the
 * same FT lookup path.
 *
 * Links liburcu only (no bind9), so nothing registers the main thread with
 * RCU on our behalf — main() does it once.  Reader/writer threads are raw
 * pthreads and register themselves.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _LGPL_SOURCE
/*
 * RCU flavor is the ONLY thing -DBENCH_FT_QSBR changes: it picks liburcu-qsbr
 * (the lowest-overhead read side — rcu_read_lock/unlock compile to nothing,
 * readers instead report a quiescent state periodically) over the membarrier
 * liburcu.  Everything else below is identical: the quiescent_state() and
 * thread_online()/thread_offline() calls are real, valid operations under both
 * flavors, so they stay unconditional.  They matter most under QSBR — there an
 * online thread that neither reports a quiescent state nor goes offline stalls
 * every grace period — but are harmless under membarrier (whose grace periods
 * wait on read-side sections, not quiescent states).
 */
#ifdef BENCH_FT_QSBR
#define URCU_API_MAP		/* map generic rcu_* names onto urcu_qsbr_* */
#include <urcu/urcu-qsbr.h>
#else
#define RCU_MEMBARRIER
#include <urcu.h>
#endif
#include <urcu/fractal-trie.h>

#include "bench_scale_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ft_entry {
	struct rcu_head rcu_head;
	struct cds_ft_node ft_node;
	size_t key_len;
	char key[];
};

/*
 * Byte offset from the (struct cds_ft_node *) stored in the trie to the user
 * key bytes, for cds_ft_speculative_lookup_key()'s library-side validation.
 */
#define FT_KEY_OFFSET \
	(offsetof(struct ft_entry, key) - offsetof(struct ft_entry, ft_node))

/*
 * Byte offset from the (struct cds_ft_node *) to the leaf's stored key length,
 * for the library-owned ordered list (Option E): a variable-length cds_ft_next
 * materializes the result-key length from the leaf without descending.
 */
#define FT_KEYLEN_OFFSET \
	(offsetof(struct ft_entry, key_len) - offsetof(struct ft_entry, ft_node))

static struct cds_ft *g_ft;
static pthread_mutex_t g_ft_mutex = PTHREAD_MUTEX_INITIALIZER;
/*
 * g_ft_mutex is the APPLICATION's writer exclusion: the single-writer churn
 * has always taken it (uncontended).
 *
 * Built with -DBENCH_FT_WRITER_STRATEGY, against a liburcu that has the
 * writer-strategy API (cds_ft_group_attr_set_writer_strategy):
 * BENCH_FT_WRITER=fine|coarse|external-sync picks the trie's strategy
 * (default: the library's, fine), and BENCH_WRITERS > 1 is supported -- the
 * mutex is then taken only for external-sync, whose contract asks the
 * application for it; fine and coarse exclude their own writers.  Without
 * it the engine is exactly the single-writer one it always was.
 */
#ifdef BENCH_FT_WRITER_STRATEGY
static enum cds_ft_writer_strategy g_ft_writer_strategy =
	CDS_FT_WRITER_LOCK_FINE;
#endif
static int g_ft_writer_mutex = 1;

/*
 * Compact arena holding the N_KEYS lookup-set external nodes (the ft_entry
 * that embeds each cds_ft_node + a copy of its key).  The reader's speculative
 * memcmp validates against the key copy here, so packing them contiguously
 * keeps that validating read TLB/cache-friendly (see struct bench_arena).
 * The churn keys are NOT placed here — they are inserted/removed dynamically,
 * so they keep their own malloc'd, RCU-freed ft_entry.
 */
static struct bench_arena ft_str_arena;

/* 16-byte alignment keeps each ft_entry's embedded cds_ft_node aligned. */
#define FT_ENTRY_ALIGN 16

/* Which churn keys are currently inserted, and the live entry for each. */
static struct ft_entry *churn_entries[CHURN_KEYS];

/* Ordered-iteration batch cap (FT_BATCH env): 0 = per-element cds_ft_next. */
static int g_ft_batch;
/*
 * Reader lookup-mode toggles (NUMA leaf-sensitivity A/B):
 *   BENCH_FT_CAND  -> cds_ft_lookup_candidate_key (no validating memcmp)
 *   BENCH_FT_EAGER -> build the EAGER encoding (descent compares bytes inline)
 */
static int g_ft_cand;
static int g_ft_eager;
/*
 * BENCH_FT_NO_READER_QS: skip the reader's per-batch rcu_quiescent_state().
 * ONLY valid in readers-only (BENCH_NO_WRITER) mode -- with no writer there is
 * no grace period to advance, so the reader need not quiesce.  Diagnostic for
 * the QSBR-vs-memb reader gap (isolates the quiescent-state path).
 */
static int g_no_reader_qs;

/* RCU callback: free an ft_entry after a grace period. */
static void free_ft_entry_rcu(struct rcu_head *head)
{
	struct ft_entry *e = caa_container_of(head, struct ft_entry, rcu_head);
	free(e);
}

static void ft_build(void)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;

	/*
	 * The whole construction (group create, inserts, restructuring frees) has
	 * exclusive access -- no readers/writer exist yet -- so run it ALL with the
	 * builder OFFLINE.  An online QSBR builder blocks the grace periods that
	 * reclaim deferred node frees, so the node allocator bump-allocates fresh
	 * slots instead of recycling, scattering the layout.  Online again only for
	 * the read-side self-check + optional compaction below.  No-op under
	 * membarrier.
	 */
	rcu_thread_offline();

	/*
	 * Per-CPU call_rcu workers, before anything defers a free.  The default
	 * is ONE global worker: every thread's call_rcu -- this engine's churn
	 * frees and the trie's own node / txn-descriptor retires -- would share
	 * its queue, and any multi-threaded measurement would measure that
	 * worker.  Fatal if it cannot be set up: a run without it is not
	 * comparable.
	 */
	if (create_all_cpu_call_rcu_data(0)) {
		perror("create_all_cpu_call_rcu_data");
		abort();
	}

	cds_ft_group_attr_create(&attr);
#ifdef BENCH_FT_WRITER_STRATEGY
	{
		const char *ws = getenv("BENCH_FT_WRITER");

		if (ws) {
			if (!strcmp(ws, "fine"))
				g_ft_writer_strategy = CDS_FT_WRITER_LOCK_FINE;
			else if (!strcmp(ws, "coarse"))
				g_ft_writer_strategy = CDS_FT_WRITER_LOCK_COARSE;
			else if (!strcmp(ws, "external-sync"))
				g_ft_writer_strategy = CDS_FT_WRITER_EXTERNAL_SYNC;
			else {
				fprintf(stderr, "BENCH_FT_WRITER=%s: unknown\n", ws);
				abort();
			}
			if (cds_ft_group_attr_set_writer_strategy(attr,
					g_ft_writer_strategy) != CDS_FT_STATUS_OK)
				abort();
		}
	}
#endif
	cds_ft_group_attr_set_max_key_len(attr, 256);
	cds_ft_group_attr_set_key_len(attr, CDS_FT_LEN_VARIABLE);
	/*
	 * Lookup optimization: SPECULATIVE by default (blind descent + one
	 * end-of-walk memcmp at FT_KEY_OFFSET; see ft_reader_batch).  BENCH_FT_EAGER
	 * builds the EAGER encoding instead (compressed bytes compared inline during
	 * descent) -- a descent-heavier, less leaf-read-dominated profile for the
	 * NUMA leaf-sensitivity A/B.
	 */
	g_ft_eager = (getenv("BENCH_FT_EAGER") != NULL);
	g_ft_cand = (getenv("BENCH_FT_CAND") != NULL);
	g_no_reader_qs = (getenv("BENCH_FT_NO_READER_QS") != NULL);
	cds_ft_group_attr_set_lookup_optimization(attr,
		g_ft_eager ? CDS_FT_LOOKUP_OPTIMIZE_EAGER
			   : CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE);
	if (g_ft_eager)
		fprintf(stderr, "[ft_build] EAGER lookup optimization (BENCH_FT_EAGER)\n");
	if (g_ft_cand)
		fprintf(stderr, "[ft_build] CANDIDATE lookup, no validation (BENCH_FT_CAND)\n");
	/*
	 * Leaf-key capture offset: makes the ordered-iteration path
	 * (cds_ft_for_each_rcu -> cds_ft_next -> limit-none inequality) run the
	 * speculative use_keycopy=true path -- recovering the result key from the
	 * matched leaf instead of rebuilding it from the descent.  Without this the
	 * "ft_spec" engine's iterate silently falls back to the eager ordinal_key
	 * rebuild, so the speculative iterate path goes unmeasured.
	 */
	cds_ft_group_attr_set_speculative_key_offset(attr, FT_KEY_OFFSET);
	/*
	 * FT_ORD: enable the library-owned ordered cell list (Option E,
	 * FEATURE_FT_ORD_CELL).  cds_ft_for_each_rcu / cds_ft_next then walk the
	 * key-ordered cell list (O(1)/step, lazy-ref result key) instead of the
	 * O(depth) limit-none inequality descent.  key_len_offset lets the
	 * variable-length walk recover the result-key length from the leaf.
	 */
	if (getenv("FT_ORD")) {
		cds_ft_group_attr_set_key_len_offset(attr, FT_KEYLEN_OFFSET);
		cds_ft_group_attr_set_ordered_list(attr, true);
		fprintf(stderr, "[ft_build] ordered cell list ENABLED (FT_ORD)\n");
	}
	/*
	 * FT_NO_ORD: turn the ordered cell list OFF.  It is on by default in
	 * the library and every insert / remove maintains it, so a mutator
	 * figure taken without this knob includes a feature the other engines
	 * do not have.  With it off the trie mutates faster and uses 32 B per
	 * key less, and gives up ordered iteration -- hence no BENCH_ITERATE.
	 */
	if (getenv("FT_NO_ORD")) {
		if (getenv("FT_ORD") || getenv("BENCH_ITERATE")) {
			fprintf(stderr, "FT_NO_ORD excludes FT_ORD and BENCH_ITERATE\n");
			abort();
		}
		cds_ft_group_attr_set_ordered_list(attr, false);
		fprintf(stderr, "[ft_build] ordered cell list DISABLED (FT_NO_ORD)\n");
	}
	if (getenv("FT_BATCH")) {
		g_ft_batch = atoi(getenv("FT_BATCH"));
		if (g_ft_batch < 1) g_ft_batch = 1;
		if (g_ft_batch > 1024) g_ft_batch = 1024;
		fprintf(stderr, "[ft_build] batched iterate cap=%d (FT_BATCH)\n", g_ft_batch);
	}
	cds_ft_group_create(attr, &group);
	cds_ft_group_attr_destroy(attr);
	cds_ft_create(group, NULL, &g_ft);

	/* Size the arena exactly: one aligned ft_entry (+ key) per lookup key. */
	size_t arena_bytes = 0;
	for (unsigned int i = 0; i < N_KEYS; i++) {
		size_t slot = sizeof(struct ft_entry) + str_lens[i];
		arena_bytes += (slot + (FT_ENTRY_ALIGN - 1)) & ~(size_t)(FT_ENTRY_ALIGN - 1);
	}
	bench_arena_init(&ft_str_arena, arena_bytes);

	/* Builder is already offline (see top of ft_build) -> prompt reclaim. */
	for (unsigned int i = 0; i < N_KEYS; i++) {
		struct cds_ft_node *result;
		struct ft_entry *e = bench_arena_alloc(&ft_str_arena,
			sizeof(*e) + str_lens[i], FT_ENTRY_ALIGN);
		memcpy(e->key, str_keys[i], str_lens[i]);
		e->key_len = str_lens[i];
		cds_ft_insert_unique(g_ft, (const uint8_t *)str_keys[i],
			str_lens[i], &e->ft_node, &result);
	}
	/* Back online for the read-side self-check + (optional) compaction below. */
	rcu_thread_online();

	/*
	 * Self-check: a present key must resolve via the speculative path, i.e.
	 * FT_KEY_OFFSET must correctly locate the stored key for the library-side
	 * memcmp.  A wrong offset would silently turn every lookup into a miss
	 * (no crash, plausible throughput), so guard it here once.
	 */
	{
		struct cds_ft_node *probe = NULL;
		rcu_read_lock();
		cds_ft_speculative_lookup_key(g_ft, (const uint8_t *)str_keys[0],
			str_lens[0], str_lens[0], FT_KEY_OFFSET, &probe);
		rcu_read_unlock();
		if (probe == NULL) {
			fprintf(stderr, "FT spec self-check FAILED: present key "
				"str_keys[0] not found (bad FT_KEY_OFFSET?)\n");
			abort();
		}
	}

	/*
	 * FT_BENCH_COMPACT: compact the freshly-built trie HERE, while the main
	 * thread is still online (cds_ft_compact needs it for its per-batch
	 * read-lock) and before the offline below.  Doing it here -- rather than
	 * via ft_cleanup_churn, whose extra rcu_thread_online/offline + rcu_barrier
	 * perturb the iterate sweep's RCU thread state and collapse MT scaling --
	 * lets the sweep measure a compacted shape with main left in exactly the
	 * same (offline) state as a non-compacting build.  The deferred cell/node
	 * frees drain lazily; no rcu_barrier needed.
	 */
	int do_compact = (getenv("FT_BENCH_COMPACT") != NULL);
	if (do_compact)
		cds_ft_compact(g_ft);

	/*
	 * Drop the main thread offline for the remainder of the sweep.  It does
	 * RCU work only here (build) and in ft_cleanup_churn (which briefly
	 * brings it back online); in between it sleeps through each timed window,
	 * where (under QSBR) an online idle thread would block the writer's grace
	 * periods.
	 */
	rcu_thread_offline();
	/*
	 * Flush ALL deferred call_rcu frees BEFORE the timed sweep: both the plain
	 * insert build's node-restructuring frees and (if compacted) the compaction's
	 * cell/node frees.  Without this, the ~nproc per-CPU call_rcu worker threads
	 * churn through the pending frees DURING the timed window -- stealing cores
	 * from the reader/iterate threads (capping MT throughput, adding variance) and
	 * leaving freed ranges mapped (inflated RSS).  UNCONDITIONAL: the non-
	 * compacting build defers frees too, so it needs the drain just as much (the
	 * barrier was previously gated on FT_BENCH_COMPACT, leaving the common build
	 * to drain mid-measurement).  Done after going offline so an online idle main
	 * can't stall the grace period.
	 */
	(void) do_compact;
	rcu_barrier();
}

static void *ft_reader_setup(void)
{
	rcu_register_thread();
	return NULL;
}

static void ft_reader_teardown(void *ctx)
{
	(void)ctx;
	rcu_unregister_thread();
}

/*
 * Sink so the optimizer cannot drop the validating memcmp (result unused).
 * Per-thread: every reader stores to it once per batch.  As one shared global
 * it sat on the cache line of g_ft / g_ft_cand, which every lookup loads, so
 * the readers kept invalidating each other's copy of that line -- 435 instead
 * of 545 Mops/s at 192 readers, and which builds were hit depended on link
 * layout (the membarrier build was, the QSBR build was not).
 */
static __thread volatile unsigned long ft_reader_sink;

static void ft_reader_batch(void *ctx, uint64_t *seed)
{
	(void)ctx;
	unsigned long acc = 0;
	rcu_read_lock();
	for (unsigned int b = 0; b < BATCH_SIZE; b++) {
		unsigned int idx = xorshift64(seed) % N_KEYS;
		struct cds_ft_node *found = NULL;
		/*
		 * Speculative descent + library-side memcmp validation (ft_spec).
		 * key_readable_pad = str_lens[idx] matches the prior candidate
		 * call's descent behavior; FT_KEY_OFFSET locates the stored key.
		 */
		if (g_ft_cand)
			cds_ft_lookup_candidate_key(g_ft,
				(const uint8_t *)str_keys[idx],
				str_lens[idx], str_lens[idx], &found);
		else
			cds_ft_speculative_lookup_key(g_ft,
				(const uint8_t *)str_keys[idx],
				str_lens[idx], str_lens[idx], FT_KEY_OFFSET, &found);
		if (found) {
			/*
			 * Force-read the validated leaf's first byte: prevents
			 * DCE of the speculative memcmp and pays the same
			 * post-lookup cache-line touch as load-names' force_read
			 * and HOTRowex's contentEquals.
			 */
			asm volatile("" :: "r"(*(const volatile uint8_t *)found)
				: "memory");
			acc++;
		}
	}
	rcu_read_unlock();
	ft_reader_sink = acc;
	if (!g_no_reader_qs)
		rcu_quiescent_state();
}

static void *ft_writer_setup(void)
{
	struct cds_ft_iter *ft_iter;

#ifdef BENCH_FT_WRITER_STRATEGY
	/* BENCH_WRITERS is known by now (the trie is built before it is read). */
	g_ft_writer_mutex = bench_nr_writers == 1 ||
		g_ft_writer_strategy == CDS_FT_WRITER_EXTERNAL_SYNC;
#endif
	rcu_register_thread();
	/*
	 * The writer mutates under g_ft_mutex and never reads under RCU (its
	 * cds_ft_lookup walks live nodes it has not removed yet), so it has no
	 * reason to be an online QSBR reader.  Stay OFFLINE for the whole churn:
	 * an online writer that only quiesces occasionally blocks every QSBR
	 * grace period, postponing call_rcu reclaim -- freed entries pile up and
	 * each insert calloc's fresh memory instead of recycling, scattering the
	 * working set and hurting both writer and reader locality.  Offline, the
	 * per-CPU call_rcu helpers run grace periods without waiting on us, so
	 * reclaim is prompt -- matching membarrier (where the writer never blocks
	 * GPs).  No-op under membarrier.  call_rcu from an offline thread is fine.
	 */
	rcu_thread_offline();
	cds_ft_iter_create(g_ft, &ft_iter);
	return ft_iter;
}

static void ft_writer_teardown(void *ctx)
{
	cds_ft_iter_destroy((struct cds_ft_iter *)ctx);
	rcu_thread_online();		/* online before unregister */
	rcu_unregister_thread();
}

/*
 * One writer never needs a read-side section: nothing else frees the nodes
 * its lookup walks.  With BENCH_WRITERS > 1 a PEER retires nodes this
 * writer's lookup and update descents cross, so each op runs inside one.
 */
static inline void ft_writer_lock(void)
{
	if (g_ft_writer_mutex)
		pthread_mutex_lock(&g_ft_mutex);
	if (bench_nr_writers > 1)
		rcu_read_lock();
}

static inline void ft_writer_unlock(void)
{
	if (bench_nr_writers > 1)
		rcu_read_unlock();
	if (g_ft_writer_mutex)
		pthread_mutex_unlock(&g_ft_mutex);
}

static void ft_writer_step(void *ctx, uint64_t *seed, unsigned long writes)
{
	struct cds_ft_iter *ft_iter = ctx;
	unsigned int cidx = bench_churn_pick(seed);

	ft_writer_lock();
	if (churn_entries[cidx]) {
		/* Remove the currently-present churn key. */
		struct ft_entry *old = churn_entries[cidx];
		enum cds_ft_status ls, rs;
		/* Repositioning with a fresh search key: cds_ft_iter_set_key()
		 * clears any cached position, so no explicit invalidate/bind. */
		ls = cds_ft_iter_set_key(ft_iter,
			(const uint8_t *)churn_keys[cidx], churn_lens[cidx]);
		if (ls < 0) {
			fprintf(stderr, "iter_set_key error: %d\n", ls);
			abort();
		}
		ls = cds_ft_lookup(g_ft, ft_iter);
		if (ls != CDS_FT_STATUS_OK) {
			fprintf(stderr, "lookup failed: %d cidx=%u key=%.*s\n",
				ls, cidx, (int)churn_lens[cidx], churn_keys[cidx]);
			abort();
		}
		rs = cds_ft_remove(g_ft, ft_iter, &old->ft_node);
		if (rs != CDS_FT_STATUS_OK) {
			fprintf(stderr, "remove failed: %d\n", rs);
			abort();
		}
		churn_entries[cidx] = NULL;
		ft_writer_unlock();
		/* Defer the free until after a grace period. */
		call_rcu(&old->rcu_head, free_ft_entry_rcu);
	} else {
		/* Insert the currently-absent churn key. */
		struct ft_entry *e = calloc(1, sizeof(*e) + churn_lens[cidx]);
		struct cds_ft_node *result;
		enum cds_ft_status status;
		if (!e) {
			fprintf(stderr, "OOM\n");
			abort();
		}
		memcpy(e->key, churn_keys[cidx], churn_lens[cidx]);
		e->key_len = churn_lens[cidx];
		status = cds_ft_insert_unique(g_ft,
			(const uint8_t *)churn_keys[cidx],
			churn_lens[cidx], &e->ft_node, &result);
		if (status == CDS_FT_STATUS_OK) {
			churn_entries[cidx] = e;
		} else if (status == CDS_FT_STATUS_DUPLICATE_FOUND) {
			free(e);
		} else {
			fprintf(stderr, "insert_unique error: %d\n", status);
			abort();
		}
		ft_writer_unlock();
	}

	(void) writes;	/* writer is rcu_thread_offline(); no quiescent state to report */
}

/* Remove the entry currently mapped at churn key @idx (must be present). */
static void ft_churn_remove(struct cds_ft_iter *it, unsigned int idx)
{
	struct ft_entry *old = churn_entries[idx];
	enum cds_ft_status ls, rs;

	/* Fresh-search reposition: set_key clears the cached position. */
	ls = cds_ft_iter_set_key(it, (const uint8_t *)churn_keys[idx],
		churn_lens[idx]);
	if (ls < 0) {
		fprintf(stderr, "iter_set_key error: %d\n", ls);
		abort();
	}
	ls = cds_ft_lookup(g_ft, it);
	if (ls != CDS_FT_STATUS_OK) {
		fprintf(stderr, "lookup failed: %d\n", ls);
		abort();
	}
	rs = cds_ft_remove(g_ft, it, &old->ft_node);
	if (rs != CDS_FT_STATUS_OK) {
		fprintf(stderr, "remove failed: %d\n", rs);
		abort();
	}
	churn_entries[idx] = NULL;
	call_rcu(&old->rcu_head, free_ft_entry_rcu);
}

/* Insert a fresh node (the mutator's value) for churn key @idx (must be absent). */
static void ft_churn_insert(unsigned int idx)
{
	struct ft_entry *e = calloc(1, sizeof(*e) + churn_lens[idx]);
	struct cds_ft_node *result;
	enum cds_ft_status status;

	if (!e) {
		fprintf(stderr, "OOM\n");
		abort();
	}
	memcpy(e->key, churn_keys[idx], churn_lens[idx]);
	e->key_len = churn_lens[idx];
	status = cds_ft_insert_unique(g_ft, (const uint8_t *)churn_keys[idx],
		churn_lens[idx], &e->ft_node, &result);
	if (status != CDS_FT_STATUS_OK) {
		fprintf(stderr, "insert_unique error/dup: %d\n", status);
		abort();
	}
	churn_entries[idx] = e;
}

/*
 * Replace the node at churn key @idx (must be present) by a fresh one, with
 * cds_ft_replace(): the trie's own value update.  It swaps the leaf under the
 * same key and leaves the node structure alone; the old leaf is RCU-freed.
 */
static void ft_churn_replace(struct cds_ft_iter *it, unsigned int idx)
{
	struct ft_entry *old = churn_entries[idx];
	struct ft_entry *e = calloc(1, sizeof(*e) + churn_lens[idx]);
	enum cds_ft_status ls, rs;

	if (!e) {
		fprintf(stderr, "OOM\n");
		abort();
	}
	memcpy(e->key, churn_keys[idx], churn_lens[idx]);
	e->key_len = churn_lens[idx];
	cds_ft_node_init(&e->ft_node);

	/* Fresh-search reposition: set_key clears the cached position. */
	ls = cds_ft_iter_set_key(it, (const uint8_t *)churn_keys[idx],
		churn_lens[idx]);
	if (ls < 0) {
		fprintf(stderr, "iter_set_key error: %d\n", ls);
		abort();
	}
	ls = cds_ft_lookup(g_ft, it);
	if (ls != CDS_FT_STATUS_OK) {
		fprintf(stderr, "lookup failed: %d\n", ls);
		abort();
	}
	rs = cds_ft_replace(g_ft, it, &old->ft_node, &e->ft_node);
	if (rs != CDS_FT_STATUS_OK) {
		fprintf(stderr, "replace failed: %d\n", rs);
		abort();
	}
	churn_entries[idx] = e;
	call_rcu(&old->rcu_head, free_ft_entry_rcu);
}

/*
 * Mutator-benchmark op.  REPLACE is cds_ft_replace(), FT's value update: a
 * fresh leaf takes the old one's place under the same key.  (It used to be a
 * remove followed by an insert of the same key, which is two structural
 * updates and not what the trie offers for this.)  A REPLACE of an absent key
 * inserts it, as an upsert would.  churn_entries[] tracks the live node so the
 * driver's drain can empty the churn set between reader-count points.  Single
 * mutator, so the (uncontended) g_ft_mutex only mirrors the read-sweep writer;
 * FT readers stay lock-free throughout.
 */
static void ft_writer_op(void *ctx, int op, unsigned int idx)
{
	struct cds_ft_iter *it = ctx;

	pthread_mutex_lock(&g_ft_mutex);
	switch (op) {
	case BENCH_OP_INSERT:
		ft_churn_insert(idx);
		break;
	case BENCH_OP_REPLACE:
		if (churn_entries[idx])
			ft_churn_replace(it, idx);
		else
			ft_churn_insert(idx);
		break;
	case BENCH_OP_REMOVE:
		if (churn_entries[idx])
			ft_churn_remove(it, idx);
		break;
	}
	pthread_mutex_unlock(&g_ft_mutex);
	/* writer is rcu_thread_offline(); no quiescent state to report */
}

static void ft_run_reset(void)
{
	memset(churn_entries, 0, sizeof(churn_entries));
}

static void ft_cleanup_churn(void)
{
	/*
	 * Runs on the main thread, already RCU-registered by main(); do NOT
	 * register again here.
	 */
	struct cds_ft_iter *cleanup_iter;
	/* Back online: ft_build / the previous cleanup left main offline. */
	rcu_thread_online();
	cds_ft_iter_create(g_ft, &cleanup_iter);
	pthread_mutex_lock(&g_ft_mutex);
	for (int i = 0; i < CHURN_KEYS; i++) {
		if (churn_entries[i]) {
			cds_ft_iter_set_key(cleanup_iter,
				(const uint8_t *)churn_keys[i], churn_lens[i]);
			cds_ft_lookup(g_ft, cleanup_iter);
			cds_ft_remove(g_ft, cleanup_iter,
				&churn_entries[i]->ft_node);
			rcu_quiescent_state();
			free(churn_entries[i]);
			churn_entries[i] = NULL;
		}
	}
	pthread_mutex_unlock(&g_ft_mutex);
	cds_ft_iter_destroy(cleanup_iter);

	/*
	 * Opt-in (FT_BENCH_COMPACT): restore the trie's descent locality after
	 * this run's insert/remove churn.  cds_ft_compact() is a copying GC-style
	 * recompact; it permits concurrent RCU readers but excludes writers, and
	 * here the run's threads have already joined, so the trie is quiescent.
	 * Leaves the next thread-count point measuring a freshly-compacted shape
	 * (closer to how BIND9-QP stays compact via dns_qp_compact during churn).
	 * rcu_barrier() flushes the deferred node frees so the drained ranges are
	 * reclaimed before the next run.  Note: the reported RSS is sampled once
	 * after build (pre-sweep), so this affects throughput/shape, not that RSS.
	 */
	int do_compact = (getenv("FT_BENCH_COMPACT") != NULL);
	if (do_compact)
		cds_ft_compact(g_ft);		/* an online RCU writer op */
	/*
	 * Return main to its sweep-long offline state — and crucially do so
	 * BEFORE the blocking rcu_barrier() below: under QSBR a thread that
	 * blocks while online would stall the very grace period it waits on.
	 */
	rcu_thread_offline();
	/*
	 * Flush the run's deferred call_rcu frees (this churn's insert/remove
	 * toggling, plus any compaction frees) BEFORE the next thread-count point,
	 * so the per-CPU call_rcu workers are quiescent for its timed window.
	 * UNCONDITIONAL for the same reason as the post-build barrier: the churn
	 * defers frees whether or not we recompact.
	 */
	(void) do_compact;
	rcu_barrier();
}

/* Ordered-iteration op: one full in-order traversal under the RCU read lock.
 * FT_BATCH=<n> uses the batched for-each (amortizes the per-step call boundary
 * over n nodes via the iterator-free cds_ft_node_next_batch / node cursor);
 * unset = per-step cds_ft_for_each_rcu via a stateful iterator. */
static unsigned long ft_iterate(void *ctx)
{
	unsigned long n = 0;

	(void)ctx;
	rcu_read_lock();
	if (g_ft_batch) {
		const struct cds_ft_cell *cell, *batch[1024];
		size_t cap = (size_t) g_ft_batch;

		if (getenv("FT_REVERSE")) {
			cds_ft_for_each_reverse_batched_rcu(g_ft, cell, batch, cap) {
				(void) cell;
				n++;
			}
		} else {
			cds_ft_for_each_batched_rcu(g_ft, cell, batch, cap) {
				(void) cell;
				n++;
			}
		}
	} else {
		struct cds_ft_iter *iter;

		cds_ft_iter_create(g_ft, &iter);
		cds_ft_for_each_rcu(g_ft, iter)
			n++;
		cds_ft_iter_destroy(iter);
	}
	rcu_read_unlock();
	rcu_quiescent_state();
	return n;
}

static const struct bench_engine ft_engine = {
#ifdef BENCH_FT_QSBR
	.name		= "ft_qsbr",
	.label		= "FT spec (QSBR)",
#else
	.name		= "ft",
	.label		= "FT spec",
#endif
	.build		= ft_build,
	.reader_setup	= ft_reader_setup,
	.reader_batch	= ft_reader_batch,
	.reader_teardown = ft_reader_teardown,
	.writer_setup	= ft_writer_setup,
	.writer_step	= ft_writer_step,
	.writer_teardown = ft_writer_teardown,
	.run_reset	= ft_run_reset,
	.cleanup_churn	= ft_cleanup_churn,
	.writer_op	= ft_writer_op,
	.iterate	= ft_iterate,
#if defined(BENCH_FT_WRITER_STRATEGY) && !defined(BENCH_FT_QSBR)
	/* QSBR keeps its writer offline: single-writer only. */
	.multi_writer	= 1,
#endif
};

int main(int argc, char **argv)
{
	/*
	 * Register the main thread with RCU exactly once.  This executable does
	 * NOT link libisc, so no ELF constructor does it for us.  (The BIND9-QP
	 * executable must NOT do this — libisc's isc__lib_initialize() already
	 * registers main, and a second registration corrupts the RCU registry.)
	 */
	rcu_register_thread();
	int ret = bench_scale_main(argc, argv, &ft_engine);
	/* ft_build / ft_cleanup_churn leave main offline; balance before unregister. */
	rcu_thread_online();
	rcu_unregister_thread();
	return ret;
}
