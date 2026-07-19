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
 *   DC_IPARENT_SKIP:   walk causality rides the host's d_iparent skip, so d_seq
 *                      is gone and its 8 bytes go to the name:
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
 */
#ifdef DC_IPARENT_SKIP
#define DC_NAME_MAX 40
#else
#define DC_NAME_MAX 32
#endif
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
 */
int dc_add(struct dcache *dc, const struct dc_path *path, uint64_t id);

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
