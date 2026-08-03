/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * dcache.h -- engine-agnostic userspace dentry-cache interface.
 *
 * Both the faithful kernel-style baseline (dcache_seqlock) and the urcu-txn port
 * (dcache_txn, S2) satisfy exactly this interface, so the benchmark harness is
 * blind to which one it drives.  The interface is deliberately PATH-oriented:
 * the RCU/rename race only manifests across a multi-component walk (a walk holds
 * dentry k and dereferences its child k+1 after k was moved), so lookups take a
 * whole path and resolve it from the root inside one RCU read-side section --
 * exactly the kernel's LOOKUP_RCU fast path.
 *
 * Two bolt-on seams are baked in from phase 1 (see README "Locked decisions"):
 *   (a) dc_lookup() returns TRI-STATE (positive / negative / absent) so phase-2
 *       negative dentries slot in without changing the signature; phase 1 never
 *       returns DC_NEGATIVE.
 *   (b) unlink is specified as honest RCU-deferred reclaim, so the phase-3
 *       shrinker is just another caller of the same reclaim path.
 */

#ifndef DCACHE_H
#define DCACHE_H

#include <stdint.h>
#include <string.h>

/*
 * Max component name length (incl. NUL).  Sized to fill the 1-CL hot line
 * exactly, so it depends on what else that line has to carry:
 *
 *   default / seqlock: d_iparent(8) + d_iname(40) + d_seq(8) + d_hash.next(8)
 *                      => 32, which also matches the kernel's DNAME_INLINE_LEN.
 *   DC_MARK_GEN:       walk causality rides the d_hash.next deletion mark, so
 *                      d_seq is gone and its 8 bytes go to the name:
 *                      d_iparent(8) + d_iname(48) + d_hash.next(8) => 40.
 *
 * The larger budget is a straight density win rather than a fidelity break: the
 * kernel's 32 is a SPILL threshold (longer names go to an external buffer),
 * while ours is a hard limit (we reject them), so 40 inline represents strictly
 * more real names -- and the line spends those bytes on the compare the reader
 * actually does instead of on a version word.
 *
 * It also closes a real asymmetry.  Needing no PER-NODE version word was a cited
 * advantage of the GLOBAL arm -- it brackets on dc->rename_gen and carries d_seq
 * only "for a uniform offset" (see dcache_txn.c) -- so the wider name was
 * structurally available to global and not to per-node.  Localized walk
 * causality therefore cost 8 name bytes.  With the skip it does not: the skip
 * arm gets per-node localization AND the density, and that advantage of the
 * global arm is gone.
 *
 * Keep it conditional: the seqlock reference genuinely needs its d_seq (it IS
 * the seqlock), and moving its layout would break the mechanism-vs-mechanism
 * A/B.  Note the name budget is a CONSEQUENCE of the mechanism, not an
 * independent knob -- a mechanism that needs a per-node version word costs you
 * those bytes, and that cost is properly attributed to it.
 *
 * -DDC_NAME_MAX=N overrides it, for ONE purpose: the MATCHED-WIDTH CONTROL.
 * Because the mark arm's natural width differs from every other arm's, a
 * mark-vs-{seqlock,global,per-node} table confounds two changes -- the causality
 * mechanism AND the name field the mechanism paid for.  Building the mark arm at
 * the baseline's 32 separates them: mark@32 vs per-node@32 isolates the
 * mechanism, mark@40 vs mark@32 prices the density the mechanism bought.  The
 * DEFAULT stays the natural width: the control is a decomposition, not the
 * shipped configuration, and reporting only mark@32 would hide a real win.
 * Note the two also differ in node SIZE (mark@32 spends 8 fewer bytes than
 * mark@40), so the control moves the allocation footprint too -- which is the
 * point: it is the "is it the mechanism or the struct" question.
 */
#ifndef DC_NAME_MAX
#ifdef DC_MARK_GEN
#define DC_NAME_MAX 40
#else
#define DC_NAME_MAX 32
#endif
#endif

/*
 * DC_NAME_PAD=N: dead padding after d_iname, so a narrowed DC_NAME_MAX leaves
 * the dentry BYTE-IDENTICAL (same sizeof, same d_hash offset) to the arm it is
 * controlling for.  Same idea as DC_SWMW_PAD.
 *
 * It exists because DC_NAME_MAX is not only a dentry knob: `struct qstr` is
 * shared between the dentry's inline name and `struct dc_path`'s component
 * array, so the width also sets the HARNESS path object -- measured here:
 *
 *   arm                      sizeof(dentry)  d_hash@  sizeof(dc_path)
 *   txn global / per-node         168          56          964
 *   txn mark (DC_NAME_MAX 40)     168          56         1156   <- +20% path
 *   txn mark, NAME_MAX=32         160          48          964
 *   txn mark, NAME_MAX=32 PAD=8   168          56          964   <- the control
 *
 * So the dentry is NOT where the mark arm differs (the Makefile's "sizeof still
 * 168, d_hash still @56" holds) -- `struct dc_path` is: a 20% bigger per-lookup
 * stack object, a 48-byte instead of 40-byte struct copy per path component,
 * and a 20% bigger precomputed leaf-qstr table.  All three sit on the reader's
 * hot path and none of them are the mechanism under test, which is what makes a
 * mark-vs-{global,per-node,seqlock} reader table not apples-to-apples.
 *
 * Hence two controls, not one:
 *   NAME_MAX=32 PAD=8  -- dentry identical to the shipped mark arm, harness
 *                         path matched to the baselines.  Isolates the harness
 *                         co-footprint; this is the arm to publish alongside.
 *   NAME_MAX=32        -- both narrowed; prices what the freed 8 bytes buy.
 */
#ifdef DC_NAME_PAD
#define DC_DENTRY_NAME_PAD	char __d_name_pad[DC_NAME_PAD];
#else
#define DC_DENTRY_NAME_PAD
#endif

/*
 * -DDC_DEBUG_NAME_GUARD: make the fold TRANSFER's plain d_iname copy checkable.
 *
 * The fold's TRANSFER hands a node's identity one hop down the transition chain
 * with a plain 40/48-byte struct copy into the successor.  It is safe for
 * exactly one reason: the successor is the UNINDEXED content host at that
 * moment, and no reader reads a non-top node's name -- match and pos/neg come
 * off the write-once top.  That reason is an invariant enforced by a comment,
 * and it is the last thing standing between the fold and a data race; it has
 * already had to be re-argued twice.  A new reader path that reaches a node by
 * any route other than an index scan -- d_host, d_fwd, a child pointer -- and
 * reads its name breaks it silently.
 *
 * So make the invariant say so out loud.  The TRANSFER brackets its copy in a
 * per-node odd/even counter and every reader-side name access validates it, in
 * the shape of a seqlock used as a detector rather than as a retry loop: an
 * overlap ABORTS with the offending node.  It cannot false-positive -- the
 * counter is odd only while a TRANSFER owns that node's name, which by the
 * invariant no reader may be looking at -- so a fire is a real violation.
 *
 * Why not just TSAN: TSAN sees this race only if its shadow cells still hold
 * the write when the read lands, and the review's own rule 4.6 records that a
 * TSAN-clean run is non-exhaustive (this engine's d_iparent race survived one).
 * The guard is deterministic given an overlap, and it runs in the ASan and
 * plain stress builds, which execute orders of magnitude more folds per second
 * than a TSAN build does.
 *
 * -DDC_DEBUG_NAME_GUARD_MUTATE additionally points dc_readdir at the HOST's
 * name instead of the top's -- a deliberate violation, since hosts are exactly
 * the nodes a TRANSFER writes.  The guard MUST fire under churn; if a run of
 * the mutated build passes, the guard is vacuous and proves nothing.
 */
#ifdef DC_DEBUG_NAME_GUARD
#define DC_DENTRY_NAME_GUARD	unsigned long __d_name_xfer;
#else
#define DC_DENTRY_NAME_GUARD
#endif
/* The guard's accessors live below dc_qstr_eq, which they wrap. */
#define DC_PATH_MAX 24		/* max components below the root */

/*
 * A path component name.  Mirrors the kernel's struct qstr {hash, len, name}:
 * the hash is precomputed once so the hot lookup path never rehashes.  The name
 * is stored inline (the kernel likewise inlines DNAME_INLINE_LEN and spills
 * longer names to an external buffer; we reject them -- no external names here).
 */
struct qstr {
	uint32_t hash;
	uint32_t len;			/* strlen(name), excludes NUL */
	char name[DC_NAME_MAX];
};

/*
 * A path expressed as its components below the root, e.g. "/a/b/c" -> ndepth 3,
 * comp = {a, b, c}.  ndepth 0 is the root itself.
 */
struct dc_path {
	uint32_t ndepth;
	struct qstr comp[DC_PATH_MAX];
};

/* Tri-state lookup outcome -- seam (a).  Phase 1 yields only ABSENT/POSITIVE. */
enum dc_result {
	DC_ABSENT = 0,			/* no dentry for this path */
	DC_POSITIVE,			/* dentry present, has an inode */
	DC_NEGATIVE,			/* dentry present, no inode (phase 2) */
};

struct dcache;				/* opaque; defined per engine */

/* ---- lifecycle ---------------------------------------------------------- */

struct dcache *dc_create(unsigned int nbuckets);
void dc_destroy(struct dcache *dc);

/* Name of the linked engine ("seqlock", "txn", ...) -- for harness banners. */
const char *dc_engine_name(void);

/* ---- RCU thread registration (engine wraps the liburcu flavor) ---------- */

void dc_register_thread(void);
void dc_unregister_thread(void);
void dc_quiescent(void);		/* QSBR: announce a quiescent state */

/* ---- operations --------------------------------------------------------- */

/*
 * Resolve p from the root and report the terminal state.  On DC_POSITIVE and a
 * non-NULL out_id, *out_id receives the leaf's stable id.  Runs inside its own
 * RCU read-side section; takes no long-lived reference (phase 1).
 */
enum dc_result dc_lookup(struct dcache *dc, const struct dc_path *p,
			 uint64_t *out_id);

/*
 * Create a positive dentry for path->comp[ndepth-1] under the directory named by
 * path->comp[0..ndepth-2] (which must already exist).  id is the caller's stable
 * identity for later verification.  0 on success, -EEXIST, -ENOENT (parent
 * missing), -ENAMETOOLONG.
 *
 * dc_add creates a DIRECTORY (a node that may have children); dc_add_file
 * creates a FILE (a leaf that never can).  A file is never an interior path
 * waypoint, so the txn engine skips its rename walk-causality bump -- which is
 * what lets the global arm stay competitive on file rename/move.  Adding a child
 * under a file returns -ENOTDIR, which enforces the invariant that skip relies
 * on.  The seqlock (kernel-faithful) engine tracks the type but bumps regardless,
 * as the kernel does.  Both return -ENOTDIR on a child-under-file.
 */
int dc_add(struct dcache *dc, const struct dc_path *path, uint64_t id);
int dc_add_file(struct dcache *dc, const struct dc_path *path, uint64_t id);

/*
 * PHASE 2 -- negative dentries.
 *
 * dc_add_negative creates a dentry that caches the ABSENCE of a name: hashed
 * under its parent, carrying a name, but with no inode.  dc_lookup reports it
 * DC_NEGATIVE and yields no id.  This is what a failed filesystem lookup
 * installs, so a second miss on the same name is a hash lookup rather than a
 * trip to the filesystem.
 *
 * dc_instantiate turns one positive: the kernel's d_instantiate, called when the
 * name is created.  The dentry keeps its address, its place in the bucket and
 * its children; only its state changes.  -ENOENT if absent, -EEXIST if already
 * positive.
 *
 * WHY THIS IS NOT A TRIVIAL ADDITION, and where the two engine families differ.
 * The seqlock baseline gets it nearly free: it has a per-dentry d_seq whose job
 * is exactly to make multi-field dentry state coherent to a lockless reader, and
 * instantiate is one more update inside that bracket -- which is what d_seq is
 * FOR.  The txn engines DELETED d_seq (the mark arm spends its 8 bytes on the
 * inline name), and paid for that by declaring pos/neg a write-once-per-identity
 * property so the reader could test it off the already-loaded d_iparent.
 * Instantiate is a node changing kind while live and reachable, which is the one
 * shape that assumption forbids.
 *
 * WHAT REPLACES d_seq IS AN ATOMIC RMW, NOT A TRANSACTION.  Publishing the flip
 * as a single-slot commit was tried first and is WRONG: urcu_txn_store_sw()
 * "parks it with a plain store that never fails", an SW-only commit never
 * contention-aborts, and SW asserts exclusion across EVERY writer of the slot --
 * which the fold's TRANSFER, another writer of that same word, breaks.  The two
 * plain read-modify-writes lose an update (see repro_delete_fold.c).  Both
 * writers use a cmpxchg on the word instead; the reader's load stays plain.
 *
 * The state is authoritative on the content HOST, not on the named top: an inode
 * is content, a rename replaces the name and must not disturb it, and keeping it
 * on the host is what makes rename correct for free rather than by copying the
 * bit into every shell.  Costs the reader one host-line load at the TERMINAL
 * component (never per hop).
 *
 * dc_delete is the inverse: the kernel's d_delete when the dentry still has
 * users, which makes it NEGATIVE in place instead of unhashing it.  This is the
 * larger source of negatives in a real dcache -- bigger than failed lookups --
 * and unlike dc_add_negative it puts a live, reachable, already-indexed node
 * through a state change on a COMMON operation.  -ENOENT if absent or already
 * negative, -EISDIR on a directory.
 *
 * dc_delete is FILES ONLY, and that is load-bearing rather than a simplifying
 * omission.  A negative must have no children and must not be able to GAIN any,
 * or a reader walking through it would find something beneath a name that does
 * not exist.  A children_empty check here would not establish that in the txn
 * engines: a concurrent dc_add commits to a different slot, so the two do not
 * conflict and the child can land after the check.  Enforcing it would cost
 * dc_add a read-set entry on the parent's state word -- a hot path taxed to
 * protect a rare call.  Files get it free and race-free: the type is write-once
 * at allocation and dc_add already rejects a child under a file with -ENOTDIR,
 * so EVERY negative is a file by construction.  It is also the faithful scope,
 * unlink(2) being the non-directory call; rmdir-to-negative would need the
 * atomic check and is deliberately out.
 *
 * WHAT dc_delete COST THE no-bump PROOF -- the reason it was not a rider on
 * phase 2.  dc_unlink owes no walk-causality bump because "unlink REMOVES, and
 * the removed node is EMPTY, hence a TERMINAL".  Neither clause survives here:
 * the node lives on, hashed, and a reader can hold it as an interior waypoint.
 * The proof still holds, but only once restated on the property the two
 * operations actually share -- THE NODE'S LOCATION DOES NOT CHANGE.  A bump is
 * owed when a reader's stale prefix can be combined with a node's NEW location
 * to name a path that never existed, which needs a RELOCATION.  A late reader
 * that finds something under a node still sitting where it always sat is
 * reporting a real path at a real time.  So the general rule is relocation, and
 * dc_unlink's terminal argument is a corollary of it (remove being relocation
 * to "nowhere"), not the other way round.
 *
 * The census (dc_walk) skips negatives: it counts OBJECTS, and a negative holds
 * a name without one.  Its id is stale by construction -- dc_delete cannot clear
 * it without racing a reader in one direction or the other -- so a conservation
 * gate must not read a cached absence as a surviving object.
 */
int dc_add_negative(struct dcache *dc, const struct dc_path *path);
int dc_instantiate(struct dcache *dc, const struct dc_path *path, uint64_t id);
int dc_delete(struct dcache *dc, const struct dc_path *path);

/*
 * Can dc_delete turn an empty DIRECTORY negative (rmdir-to-negative), or only a
 * file?  1 on the engines that can; 0 means dc_delete answers -ENOTSUP for a
 * directory.  This is a genuine engine difference, not a stub, and it is the
 * cleanest measurement rmdir-to-negative produced:
 *
 * A negative must not be able to GAIN a child.  For a FILE that is free on every
 * engine -- d_isdir is write-once and dc_add already answers -ENOTDIR.  A
 * DIRECTORY can legitimately take one, so children_empty has to still hold at
 * the instant the state flips, which means excluding a concurrent dc_add.
 *
 *   dcache_seqlock     FREE.  dc_add takes its parent's dir lock, so dc_delete
 *                      takes the VICTIM's -- which is that same lock.  Exactly
 *                      why the kernel's rmdir holds the victim's i_rwsem.
 *   dcache_bucketlock  FREE.  dc_add takes its parent's d_child_head bit-lock,
 *                      so dc_delete takes the victim's -- the same head, and one
 *                      it can pair with the bucket it already holds.
 *   dcache_txn         1 on the arms that already transact d_iparent (the MARK
 *                      arm), 0 otherwise.  No lock: a GUARD PAIR, which is the
 *                      lock-free answer to the same question --
 *
 *                          dc_add    WRITES parent->d_child_head
 *                                    GUARDS parent->d_iparent
 *                          d_delete  GUARDS host->d_child_head
 *                                    WRITES host->d_iparent
 *
 *                      Each side's write set hits the other's read set, so the
 *                      two cannot both commit, in EITHER order.  BOTH guards are
 *                      required: one alone is one-directional (dc_add's alone
 *                      lets a child land after emptiness was checked; d_delete's
 *                      alone lets a child land after the parent was read
 *                      positive).  repro_negdir.c drives both directions and is
 *                      mutation-verified -- removing either guard fails exactly
 *                      its own direction and leaves the other passing.
 *
 *                      Costs the READER nothing on that arm: iparent_raw()
 *                      already resolves the slot (DC_IPARENT_TXN), a resolve
 *                      that until now handled a proxy nobody installed.  It does
 *                      cost the WRITER one read-set entry, with one consequence
 *                      to measure: the guarded word also carries the parent
 *                      POINTER and the shell tag, so an add under a directory
 *                      now aborts when that directory is itself renamed or its
 *                      fold runs -- not only when it goes negative.  Sealing the
 *                      child-list head instead would avoid that (the seal exists
 *                      only while the directory IS negative) at the price of a
 *                      tag every child-list traversal must resolve.
 *
 * So the feature is free where a lock already covers the child list -- the
 * kernel's own arrangement, and one more thing the hybrid winner inherits by
 * ending on the bucket lock -- and costs a read-set entry where nothing does.
 */
extern const int dc_delete_dir_supported;

/*
 * PHASE 3 -- the LRU and its shrinker.
 *
 * A real dcache is BOUNDED: entries are evicted under memory pressure, and the
 * kernel picks victims from a per-(NUMA node x memcg) `struct list_lru` with a
 * CLOCK / second-chance policy.  Three properties of that design are load-bearing
 * and are reproduced here (see design/dcache-lru-txn.md for the derivation):
 *
 *   1. THE ORDER IS FUZZY, ON PURPOSE.  dentry_lru_isolate is a clock: in-use ->
 *      remove, REFERENCED -> clear the bit and rotate to the tail (second
 *      chance), otherwise evict.  A precise LRU is doubly disqualified -- it
 *      would write a shared list head on every access AND need reader-atomicity
 *      on the list.
 *   2. A LOOKUP TOUCHES THE LRU ZERO TIMES.  The kernel's RCU walk takes no
 *      reference, so it never dputs and never reaches the list; recency is a
 *      per-object bit set on the ref-taking paths.  This port is pure RCU walk,
 *      so the reader stays exactly as it was -- no new load, no new store, and
 *      the LRU fields sit off the 1-CL hot line.
 *   3. THE LRU WANTS NO SW TRANSACTION.  The SW txn's one product is a
 *      reader-atomic multi-word flip, and the LRU has no lockless reader to
 *      consume it -- only the shrinker walks the links.  So the links are plain
 *      stores under the shard lock.  (That is an argument against SW here, NOT
 *      against MW: an arbitrary mid-list splice is a genuine multi-writer
 *      problem, which is the second arm this phase is built to compare.)
 *
 * dc_shrink evicts up to @nr entries and returns how many it actually freed.
 * dc_lru_count reports the current population.  Eviction is a real unlink --
 * the same RCU-deferred reclaim path dc_unlink uses (interface seam (b)) -- so
 * an evicted name is ABSENT afterwards, not negative.
 *
 * ⚠ A NON-EMPTY DIRECTORY IS NEVER EVICTED.  In the kernel a child pins its
 * parent (the parent's refcount is non-zero while children are cached), so a
 * populated directory is never a candidate; here the shrinker skips it
 * explicitly.  Without that, eviction would orphan a live subtree -- and the
 * conservation census would be right to call it corruption.
 *
 * ⚠ dc_shrink DESTROYS NAMES BY DESIGN, so any harness with a conservation gate
 * must account for what it evicted.  It is never called implicitly: nothing
 * shrinks unless the caller asks.
 */
extern const int dc_lru_supported;
long dc_shrink(struct dcache *dc, long nr);
/*
 * Shrink only the CALLER'S OWN shard.  What an evict-on-insert bounded cache
 * wants: a continuous evictor calling dc_shrink() instead makes every producer
 * a consumer of every other producer's shard, destroying the isolation that
 * sharding exists for.  On the MCAS arm that is not a slowdown but a collapse.
 */
long dc_shrink_local(struct dcache *dc, long nr);
unsigned long dc_lru_count(struct dcache *dc);

/*
 * Which axis the LRU shards on -- "pernode" (default, what the kernel does),
 * "percpu", or "mm_cid" (rseq's dense per-process concurrency id).  A build arm,
 * because the trade is real and measurable: finer sharding trivially wins the
 * enqueue microbenchmark, and what it costs is eviction QUALITY -- N independent
 * clocks make the global order only as good as the shard balance -- plus a shard
 * array sized by the machine rather than by the workload.  Per-node keeps one
 * clock per node, which is why the kernel can still call the result an LRU.
 */
const char *dc_lru_arm(void);

/*
 * -DDC_TXN_STATS: per-call-site transaction counters (attempts / contention
 * aborts / fallback-lane entries / aging depth).  Built to answer WHICH commit
 * site starves first, because escalation is domain-wide -- once any site
 * escalates, every site's begin() pays for it, so only per-site attribution
 * separates the initiator from the victims.  Off by default; see
 * dcache_txn_stats.h.  dc_txn_stats_supported is 0 in a normal build.
 */
extern const int dc_txn_stats_supported;
void dc_txn_stats_dump(void *stream);
void dc_txn_stats_last(void *stream);
/* Walk the LRU and report any MARKED-but-LINKED node (diagnostic). */
void dc_lru_validate(void *stream);

/*
 * Unlink the leaf at path, RCU-deferring the free past a grace period (seam
 * (b)).  0 on success, -ENOENT, -ENOTEMPTY (still has children).
 */
int dc_unlink(struct dcache *dc, const struct dc_path *path);

/*
 * Move the node at `from` to `to`: its new parent is to's parent (must exist),
 * its new name is to's last component (must not already exist).  Same-parent
 * moves are a pure rename; cross-parent moves take the cache rename mutex and
 * reject moving a directory into its own descendant (the s_vfs_rename_mutex /
 * loop-prevention analog).  0 on success, -ENOENT, -EEXIST, -EINVAL (loop).
 */
int dc_rename(struct dcache *dc, const struct dc_path *from,
	      const struct dc_path *to);

/* Atomically swap the two nodes' positions (RENAME_EXCHANGE); both must exist. */
int dc_rename_exchange(struct dcache *dc, const struct dc_path *a,
		       const struct dc_path *b);

/*
 * List the directory at `path`: invoke `fn(id, name, arg)` once per child, in
 * unspecified order.  A reader fast path (served from the child index, not a
 * backing store), so it runs concurrently with mutators; consistency is POSIX-
 * soft -- a child added or removed by a concurrent rename may or may not appear,
 * but every reported child is a real one and the call never tears.  Returns the
 * number of children reported, or -ENOENT if `path` does not resolve, -ENOTDIR
 * reserved for phase 2.  fn may be NULL to just count.
 */
typedef void (*dc_dirent_fn)(uint64_t id, const struct qstr *name, void *arg);
long dc_readdir(struct dcache *dc, const struct dc_path *path,
		dc_dirent_fn fn, void *arg);

/* ---- verification ------------------------------------------------------- */

/*
 * Visit every reachable (id, full path) exactly once, in unspecified order.
 * Single-threaded / quiescent use only (namespace-conservation checks).
 */
typedef void (*dc_visit_fn)(uint64_t id, const struct dc_path *path, void *arg);
void dc_walk(struct dcache *dc, dc_visit_fn fn, void *arg);

/* ---- engine-independent path/qstr helpers (pure, inline) ---------------- */

/* FNV-1a over the name; the engine mixes this with the parent ptr for a bucket. */
static inline void dc_qstr_init(struct qstr *q, const char *name)
{
	uint32_t h = 2166136261u;
	size_t i = 0;

	for (; name[i] != '\0' && i < DC_NAME_MAX - 1; i++) {
		h ^= (unsigned char) name[i];
		h *= 16777619u;
	}
	q->hash = h;
	q->len = (uint32_t) i;
	memcpy(q->name, name, i);
	q->name[i] = '\0';
}

static inline int dc_qstr_eq(const struct qstr *a, const struct qstr *b)
{
	return a->hash == b->hash && a->len == b->len &&
	       memcmp(a->name, b->name, a->len) == 0;
}

/* ---- fold TRANSFER name guard (see DC_DEBUG_NAME_GUARD above) ----------- */
#ifdef DC_DEBUG_NAME_GUARD
#include <stdio.h>
#include <stdlib.h>

/* TRANSFER brackets: __d_name_xfer is odd exactly while the copy is in flight. */
#define DC_NAME_XFER_BEGIN(d)	do {					\
	__atomic_add_fetch(&(d)->__d_name_xfer, 1, __ATOMIC_RELEASE);	\
} while (0)
#define DC_NAME_XFER_END(d)	do {					\
	__atomic_add_fetch(&(d)->__d_name_xfer, 1, __ATOMIC_RELEASE);	\
} while (0)

static inline void dc_name_guard_fail(const void *d, unsigned long s0,
				      unsigned long s1, const char *what)
{
	fprintf(stderr,
		"NAME GUARD: %s read dentry %p's name across a fold TRANSFER "
		"(xfer %lu -> %lu).\nA reader reached a node that was NOT the "
		"indexed top and read its name -- the invariant the TRANSFER's "
		"plain copy rests on is broken.\n", what, d, s0, s1);
	abort();
}

/* Bracket a name MATCH.  @xfer is the same node's counter. */
static inline int dc_name_guard_eq(const struct qstr *iname,
				   const unsigned long *xfer,
				   const struct qstr *name, const void *d)
{
	unsigned long s0 = __atomic_load_n(xfer, __ATOMIC_ACQUIRE);
	int r = dc_qstr_eq(iname, name);
	unsigned long s1 = __atomic_load_n(xfer, __ATOMIC_ACQUIRE);

	if ((s0 & 1UL) || s0 != s1)
		dc_name_guard_fail(d, s0, s1, "match");
	return r;
}

/* Bracket a name COPY-OUT (readdir / walk hand the name to a caller). */
static inline void dc_name_guard_copy(struct qstr *dst, const struct qstr *iname,
				      const unsigned long *xfer, const void *d)
{
	unsigned long s0 = __atomic_load_n(xfer, __ATOMIC_ACQUIRE);
	unsigned long s1;

	*dst = *iname;
	s1 = __atomic_load_n(xfer, __ATOMIC_ACQUIRE);
	if ((s0 & 1UL) || s0 != s1)
		dc_name_guard_fail(d, s0, s1, "copy-out");
}

#define DC_INAME_EQ(d, name)						\
	dc_name_guard_eq(&(d)->d_iname, &(d)->__d_name_xfer, (name), (d))
#define DC_INAME_COPY(dst, d)						\
	dc_name_guard_copy((dst), &(d)->d_iname, &(d)->__d_name_xfer, (d))

#else  /* the shipped build carries none of this */
#define DC_NAME_XFER_BEGIN(d)	do { } while (0)
#define DC_NAME_XFER_END(d)	do { } while (0)
#define DC_INAME_EQ(d, name)	dc_qstr_eq(&(d)->d_iname, (name))
#define DC_INAME_COPY(dst, d)	do { *(dst) = (d)->d_iname; } while (0)
#endif

/*
 * Non-vacuity hooks.  MUTATE points name reads at the HOST instead of the top,
 * and hosts are exactly the nodes a fold TRANSFER writes, so the guard must fire
 * under churn.  A mutated build that SURVIVES a stress run is the bug: it means
 * the guard cannot see the thing it exists to see.
 *
 * The mutation has to sit on a path the harnesses actually run.  The first
 * attempt mutated only dc_readdir's callback copy -- which bench_dcache calls
 * with fn == NULL, so the mutated line was DEAD CODE and a passing run would
 * have "proved" a guard that had never been exercised: rule 4.3's trap, caught
 * by mutation-testing the mutation.  So the bucket-scan MATCH carries it too --
 * that one runs on every component of every lookup in every harness, and
 * "resolve the host, then compare its name" is the realistic shape of the
 * mistake this guard exists to catch.
 *
 * Defined for every build (not just guarded ones) so the shipped sources
 * compile: outside a MUTATE build both are the identity.
 */
#ifdef DC_DEBUG_NAME_GUARD_MUTATE
#define DC_READDIR_NAME_SRC(top, host)	(host)
#define DC_MATCH_NAME_SRC(d)		host_of_rcu(d)
#else
#define DC_READDIR_NAME_SRC(top, host)	(top)
#define DC_MATCH_NAME_SRC(d)		(d)
#endif

static inline void dc_path_reset(struct dc_path *p)
{
	p->ndepth = 0;
}

/* Append a component; returns 0 or -1 if the path is full. */
static inline int dc_path_push(struct dc_path *p, const char *name)
{
	if (p->ndepth >= DC_PATH_MAX)
		return -1;
	dc_qstr_init(&p->comp[p->ndepth++], name);
	return 0;
}

/* Parse "/a/b/c" (leading slash optional, no "." / "..") into components. */
static inline int dc_path_parse(struct dc_path *p, const char *path)
{
	char comp[DC_NAME_MAX];
	size_t n = 0;

	dc_path_reset(p);
	for (;; path++) {
		char c = *path;

		if (c == '/' || c == '\0') {
			if (n > 0) {
				comp[n] = '\0';
				if (dc_path_push(p, comp) != 0)
					return -1;
				n = 0;
			}
			if (c == '\0')
				return 0;
			continue;
		}
		if (n >= DC_NAME_MAX - 1)
			return -1;
		comp[n++] = c;
	}
}

#endif /* DCACHE_H */
