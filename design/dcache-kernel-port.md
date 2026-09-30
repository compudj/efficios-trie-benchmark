# Porting the rcu-txn dentry cache to Linux — design note

Status: **draft for review, 2026-09-29; revised the same day to "accessors
first" (§3) on review.  DECIDED: the fold takes the parent's `i_rwsem` by
trylock from a workqueue (§3); pinning rejected.**  Kernel base v7.3-rc5, branch
`rcu-txn-dentry-cache` in `~/git/linux`.  No `fs/dcache.c` code yet — this
note decides what to write there.  Every kernel claim below cites v7.3-rc5
source; claims marked *(verify)* were not checked line by line.

Done so far on the branch (uncommitted): `include/linux/rcu_txn_sw.h` and
`include/linux/rcu_txn_sw_hlist.h` (port of the userspace single-writer
engine and its hlist), KUnit suite `lib/tests/rcu_txn_sw_kunit.c`,
`CONFIG_RCU_TXN_SW_KUNIT_TEST`, `CONFIG_DEBUG_RCU_TXN_SW`.  Builds clean at
W=1; the KUnit suite has not been run yet.

**Evidence caveat.**  The userspace numbers were re-swept on 2026-09-30 on a
corrected methodology (`experiments/dcache/README.md` "Results"): the kernel's
RCU walk as the baseline's lookup, writers paced to one offered rate,
negative dentries, the kernel `rw_semaphore` as the directory lock.  They say
readers are at parity at realistic rename rates and gain as renames climb,
and that writers gain several-fold -- partly by dissolving the per-directory
rwsem and cross-directory rename mutex, so what a kernel port gains depends
on what the VFS keeps taking around the dcache.  Userspace ratios are still
not kernel evidence: this note argues from mechanism and ends with the kernel
measurements that would have to justify each step.

---

## 1. The userspace design, as a package

The userspace engines (`experiments/dcache/dcache_txn.c`,
`dcache_bucketlock.c`) change five things at once, and they depend on each
other:

1. **Shell-stacked renames.**  A rename allocates a *shell* dentry carrying
   the new (parent, name) and a skip pointer to the moving object — the
   *host*, which never moves in memory.  One commit publishes the shell in
   the new hash bucket and child list and demotes the old top (deletion mark
   + `d_back`).  A `call_rcu` *fold* later copies the identity back into the
   host, re-indexes it and frees the shell.
2. **Write-once index identity.**  Because a node's (parent, name) never
   changes while it is in an index, a lookup compares names with no `d_seq`.
3. **Walk causality without `rename_lock`.**  Three arms: one global
   generation (whole-tree line, does not scale), a per-host generation, and —
   the default of the winning engine — the hlist *deletion mark* as the
   version (double collect: confirm unmarked on the way down, re-test on the
   way back).
4. **Writers without a global lock.**  Bucket bit locks + a single-writer
   commit (`dcache_bucketlock.c`) or MCAS (`dcache_txn.c`); the cross-dir loop
   check is a per-host `d_moving` Dekker flag instead of `s_vfs_rename_mutex`.
5. **Lock-free readdir.**  The child list is an RCU hlist whose entries are
   shells too, so a listing never jumps directories mid-rename.

In the kernel, (1) and (2) survive only behind a tree-wide conversion of name
readers to accessors (§3), (3) and (4) survive in a localized form, and (5) is
not worth it.

## 2. The kernel today (v7.3-rc5)

| Mechanism | Written by | Read by |
|---|---|---|
| `rename_lock` (global seqlock, `__cacheline_aligned_in_smp`, fs/dcache.c:85) | `d_move`, `d_exchange`, `d_splice_alias_ops`→`__d_unalias` (write side: a global spinlock + two seq bumps per rename) | **every path walk**: `path_init` samples it with `__read_seqcount_begin` (spins while a `d_move` is in flight), fs/namei.c:2697; the value is used ONLY by `handle_dots` for `LOOKUP_IS_SCOPED` `..` (namei.c:2250-2260).  **Misses**: `d_lookup` (dcache.c:2561), `d_alloc_parallel` (:2787).  **Reverse / tree walks** with `read_seqbegin_or_lock`: `prepend_path`, `__dentry_path` (d_path.c), `d_walk` (:1449), `is_subdir` (:3354); `d_set_mounted` takes it exclusively (:1591). |
| `d_seq` (per dentry) | `__d_move` (both dentries), `__d_drop` (invalidate), `__d_instantiate`, `__d_add`, `dentry_unlink_inode` | RCU walk, hand-over-hand: child sampled in `__d_lookup_rcu`, parent re-checked in `lookup_fast`, child in `step_into`, terminal in `legitimize_path`; also `dget_parent`, `follow_dotdot_rcu`. |
| hash chains (`hlist_bl`, bit 0 = lock, list_bl.h:22) | `__d_rehash`, `___d_drop` | `__d_lookup_rcu`, `__d_lookup` |
| `d_children` / `d_sib` | under the parent's `d_lock` | `d_walk`, libfs `scan_positives` (under `d_lock`), fsnotify, shrinkers |

Three properties of `__d_move` (dcache.c:3059-3142) drive the design:

- **It always renames ONTO a target dentry** — usually the negative dentry
  the caller looked up for the new name — copies that name in (`copy_name`,
  external names are refcounted) and unhashes the target.  (The userspace
  model now does the same: `dc_rename` replaces a negative target.)
- **It re-links the moving dentry's own `d_hash` node into the new bucket
  immediately** (`___d_drop` + `__d_rehash`).  A lockless reader standing on
  that node follows its new `next` into the wrong chain: the *chain-hop false
  negative*.  That is the whole reason `d_lookup` and `d_alloc_parallel`
  re-check `rename_lock` on a miss (`__d_lookup_rcu`'s comment, :2498-2502).
- **Its `d_lock` order is decided with `d_ancestor()`**, a `d_parent` walk that
  is only stable because the caller holds `rename_lock`.  Removing
  `rename_lock`'s write side therefore needs a new deadlock-avoidance order,
  not just new readers.

## 3. The identity problem, and accessors first

In userspace the host's name is stale until its fold runs, which is safe
because only index lookups read names, and they read the top's.  In the
kernel the host is the `struct dentry` the whole VFS holds, and its
`d_name` / `d_parent` are read in place everywhere:

- **Under the parent's `i_rwsem`**: every filesystem's `->lookup`, `->create`,
  `->rename`, … — they expect the new name the moment `d_move` returns.
- **With only a reference**: fsnotify and friends via
  `take_dentry_name_snapshot` (13 call sites in fs/ outside dcache.c), audit and LSMs via `d_path`.
- **Locklessly**: `%pd` (`dentry_name`, lib/vsprintf.c:919-927, `READ_ONCE`
  under RCU), `dget_parent` (`d_seq`), `follow_dotdot_rcu` (`d_seq`), the
  `d_path` family (`rename_lock`).
- **`d_unhashed()` as "deleted"**: 49 call sites in fs/, security/, kernel/,
  mm/, ipc/, drivers/ and net/ outside fs/dcache.c.

Two ways to live with that:

- **(a) Update the host in place** at `d_move` time, as today, and defer only
  the hash-index re-link.  Nothing outside fs/dcache.c changes, but the host's
  name is rewritten under readers, so every name reader keeps needing today's
  protection and write-once identity is lost.  (This note's first draft.)
- **(b) Accessors first** (chosen): route every read of a dentry's NAME
  through a helper that returns the CURRENT name -- the top shell's while a
  rename is folding, the host's otherwise -- so the host's own name is never
  rewritten under a reader and index identity stays write-once, as in
  userspace.  Precedent: David Howells' 2015 `d_inode()` /
  `d_backing_inode()` series ("VFS: ... d_inode() annotations", then "VFS:
  Handle lower layer dentry/inode in pathwalk"), which converted `->d_inode`
  readers tree-wide to accessors BEFORE changing what they return, so
  layered filesystems could interpose.

Only the NAME needs resolving.  `d_parent` stays a host field updated at
rename time -- the userspace engine's logical `d_parent` already is (updated
inside the rename commit), and it carries the child-pins-parent reference the
kernel depends on.  `d_unhashed()` is already a helper; under (b) it consults
the top, so a host demoted from the index while a shell names it is not
"deleted".

Size of (b), kernel code only (tools/ excluded): `->d_name` ~830 sites in 157
files (`.name` 304, `.len` 239, `&x->d_name` 257, a few non-dentry structs
included), heaviest in ocfs2, afs, nfs, ceph, fat, overlayfs, jffs2, ext4.  A
coccinelle series, subsystem by subsystem, as the 2015 one was.

**The crux of (b) is NAME LIFETIME, not the conversion.**  Today a name read
under the parent's `i_rwsem` stays valid for the whole hold -- a rename needs
that lock -- and filesystems SLEEP while using it (`->lookup` does I/O with
`dentry->d_name`).  Under (b) the current name may live in a shell, which the
fold retires (identity copied into the host, shell freed a grace period later)
without that lock: a sleeping reader would keep a pointer into a freed shell.
So the helper comes in three classes, each with the lifetime its callers
already assume:

| Accessor | Caller holds | Pointer valid until | What it takes |
|---|---|---|---|
| `d_name_rcu()` | `rcu_read_lock()`, or any spinlock (an RCU reader) | `rcu_read_unlock()` | nothing: the shell is freed a GP after the fold |
| `d_name_locked()` (lockdep-asserted) | the parent's `i_rwsem`, either mode | the lock is released | **the fold runs from a workqueue after the GP and takes the parent's `i_rwsem` with a trylock, requeueing on contention**; a rename needs that lock too, so name content AND storage are frozen for the hold, exactly as today |
| `take_dentry_name_snapshot()` (exists) | a reference only | `release_dentry_name_snapshot()` | a copy (inline) or a ref (external name), of the TOP's name |

`%pd` and the `d_path` family become `d_name_rcu()` users internally.  The
reference-only class is where today's code is already racy against renames;
those sites get the snapshot, which several already use.

**The snapshot under shells.**  Today (dcache.c:361-387) it brackets the
dentry's OWN `d_name` with that dentry's `d_seq`, copying inline bytes or
taking a ref on the external name, and retries if `d_seq` moved.  Left as is,
it would be *wrong*, not racy: a rename no longer writes the host's name, so it
would return the pre-rename name, cleanly, until the fold -- and `d_seq` could
not catch it, since nothing wrote that name.  Converted to climb to the top
(host -> `d_back` -> the node with `d_back == NULL`, under RCU) and copy or ref
THAT name, it is not racy:

- no torn copy: a shell's name is written before the shell is published, and
  the host's is rewritten only by the fold, a grace period after the rename
  that demoted it -- after any reader that saw the host as top has finished;
- no stale storage: a shell is freed a grace period after its fold, and
  external names are already `atomic_inc_not_zero()` + `kfree_rcu`; a failed
  increment (the fold dropped the shell's last ref) re-climbs from the host;
- linearizable: the result is the name current when the top was observed.

It also gets simpler: no `d_seq` retry for names, only the re-climb on a
failed external-name ref.  Its contract is unchanged (the snapshot owns its
copy or ref), so its 13 callers outside dcache.c need nothing.

**Why the fold must take the parent's `i_rwsem`, and how that differs from
userspace.**  In userspace every name reader is an RCU reader: a lookup
compares inside `rcu_read_lock()`, readdir hands out `&top->d_iname` inside
it, `dc_dentry_path` copies inside it.  So "fold a grace period after the
rename, free the shell a grace period after the fold" retires a shell safely,
and a plain `call_rcu` callback is enough.  The kernel adds a reader class
userspace does not have: SLEEPING name holders protected by a lock, not by
RCU.  `lookup_slow` takes `inode_lock_shared(dir)` (namei.c:1935) and calls
`->lookup`, which uses `dentry->d_name` across disk or network I/O; create,
unlink, rename run under the exclusive lock.  A grace period does not wait for
them.  With a `call_rcu` fold, such a reader can obtain the top shell's name
through `d_name_locked()`, the fold promotes the host and `call_rcu`s the
shell, the next grace period frees it -- and the reader, still asleep in
`->lookup`, dereferences freed memory.  (Pointers into the HOST's own name are
safe: the fold rewrites it only while the host is demoted, and demoting it
takes a rename, which takes the lock.)

Three ways out: copy the name at every locked access (changes the lifetime of
a pointer at every call site; some pass `&d_name` deep into helpers), pin the
shell with a count the fold waits out (a paired release at every call site),
or **exclude by the lock those readers already hold**.  **Decided: the
last** -- pinning never defers a fold, but touches every locked call site and
invents a new invariant where the lock already provides one.  The last keeps the
kernel's invariant -- "a name is frozen while its parent's `i_rwsem` is held"
-- and changes no call site.  `i_rwsem` sleeps, so the fold cannot run from
an RCU callback: it runs from `queue_rcu_work()` (process context, after a
grace period).  It takes the lock with `inode_trylock()` and requeues on
failure rather than blocking, so a fold never ties up a worker behind a long
directory operation and holds nothing when it tries (no new lock-order edge:
`i_rwsem` then `d_lock` then bucket locks, the order `lock_rename` + `d_move`
already use).  A directory whose lock is never free (continuous shared
holders) would starve it; after N failed tries the fold escalates to a
blocking `down_write` in a dedicated worker -- the kernel rwsem is
writer-fair, so it gets through.

Only the fold that retires the CURRENT top needs the lock (the TRANSFER, and
its variant for an entry unlinked while shelled).  A middle relay stopped
being the top when a later rename demoted it; that rename held the relay's
directory lock exclusively, so every locked reader of its name had released
it, and no new one can obtain it -- the accessor now returns the newer top.

| | Userspace fold | Kernel fold |
|---|---|---|
| Trigger | `call_rcu`, a grace period after the rename | `queue_rcu_work`, a grace period after the rename |
| Context | RCU callback, cannot sleep | workqueue, may sleep |
| Excludes readers by | RCU only | RCU, plus the parent's `i_rwsem` (trylock) for the TRANSFER |
| Excludes a racing re-rename by | the engine: MCAS aborts / per-host fold lock, re-deciding TRANSFER vs SPLICE each attempt | the same, but a re-rename of the entry being TRANSFERred cannot run at all: it needs the lock the fold holds |
| Can be deferred | never | yes, on a contended directory: shells live longer (memory and one skip-pointer hop, not correctness) |
| Name copy into the host | plain store, safe because no reader reads an unindexed host's name | under the host's `d_lock` and the parent's `i_rwsem` |
| Teardown | `rcu_barrier()` in `dc_destroy` | umount must drain it: `rcu_barrier()` + flush the workqueue before the dentries are killed |

Two rules survive from the first draft:

- **K2.** A shell is an **index-only** object: in a hash chain, never on
  `d_children`, never returned, never refcounted, never on an LRU, invisible
  to `d_unhashed()` (which resolves to the top's state).
- **K3.** `d_seq` stays.  It guards inode/type transitions too
  (`__d_instantiate`, `__d_add`, `dentry_unlink_inode`), not only renames.
  The userspace "`d_seq` dissolves" result also needed its phase-2 pos/neg
  word, and porting that would touch every `d_inode`/`d_flags` reader.  With
  (b), renames stop rewriting names under readers, so a rename only has to
  invalidate the demoted top's `d_seq`; readers keep their per-hop check for
  inode transitions.

## 4. A staged plan

Each increment is independently mergeable, measurable, and reverts cleanly.
Increments A, 0 and 1 need no rcu_txn_sw; the engine enters at 2.

### Increment A — name accessors, no behavior change

Introduce `d_name_rcu()` and `d_name_locked()` (returning `&dentry->d_name`
today; the latter lockdep-asserting the parent's `i_rwsem` or the dentry's
`d_lock`), steer reference-only readers to `take_dentry_name_snapshot()`,
and convert the ~830 `->d_name` readers with coccinelle, subsystem by
subsystem.  Then restrict direct `d_name` access to fs/dcache.c, as the
existing `__d_name` union member's comment already intends.

- **Standalone value:** it documents, per call site, which protection a name
  read relies on -- and the classification will surface today's
  reference-only sites that race renames.
- **Risk:** none at runtime; review load is the cost.

### Increment 0 — stop sampling `rename_lock` on non-scoped walks

`path_init` samples `rename_lock` for every walk (namei.c:2697), but `nd->r_seq`
has exactly three references: the declaration (:734), this sample, and
`handle_dots` under `LOOKUP_IS_SCOPED` (:2258).  For every other walk the
sample is dead work — yet it reads the one global line every rename writes
twice, and spins while a `d_move` is in flight.  Take it only when
`flags & LOOKUP_IS_SCOPED`.

- **Why it is the userspace lesson:** "a global counter on the read path
  re-creates the contention being removed" (REVIEW.md rule 4).  This is the
  kernel's last global line on the lookup fast path.
- **Risk:** low, provided no other consumer of `r_seq` exists *(verify at each
  rebase; a `WARN_ON` on use-without-sample is cheap)*.  Needs no CONFIG.
- **Measure:** stat()-heavy readers + a rename storm on tmpfs; `perf c2c` on
  the `rename_lock` line; reader throughput vs rename rate.

### Increment 1 — per-dentry reverse walks

Make `__dentry_path`, `prepend_path` and `is_subdir` validate each visited
dentry's `d_seq` (sample on the way up, re-read all at the end) instead of
bracketing on `rename_lock`, keeping `mount_lock` as is.  This is the
userspace per-node reverse walk (`dc_dentry_path`, per-node arm), and it is
simpler in the kernel: `__d_move` bumps both dentries' `d_seq`
unconditionally, so no leaf special case is needed.

- **Guarantee kept:** the reported path existed at one instant (every sampled
  `d_seq` unchanged across the climb ⇒ each component held still throughout).
- **Boundedness:** after N failed passes, fall back to today's
  `read_seqlock_excl(&rename_lock)` pass (the kernel's existing guarantee).
- **Win:** getcwd, readlink of /proc/PID/fd, audit and LSM `d_path` stop
  serializing against every rename on the machine.
- **Gate:** `CONFIG_DCACHE_LOCAL_REVERSE_WALK` (default n) for A/B.

### Increment 2 — shell-stacked renames (rcu_txn_sw), behind the accessors

With increment A in place, `__d_move` stops rewriting the host.  Instead of
`copy_name` + `___d_drop` + `__d_rehash` of the moving dentry:

1. publish an index shell carrying the new (parent, name) in the new bucket
   -- the accessors now return ITS name for the host -- and
2. demote the host's own node from the old bucket with a *marked* delete (its
   `next` stays valid, so a reader standing on it escapes forward in its OWN
   chain) and invalidate its `d_seq`,

as **one rcu_txn_sw commit** with both bucket locks held; `d_parent`,
`d_sib`/`d_children` and the parent references move at `d_move` time as
today.  A lockless reader sees old-name→host or new-name→shell→host, never
neither and never both.  The fold -- a workqueue item queued a grace period
later, taking the new parent's `i_rwsem` by trylock -- copies the name into
the host under its `d_lock`, replaces the shell by the host in the index, and
frees the shell after another grace period.  The host's node is only
re-linked a grace period after it left, so no reader can hop on it, and its
name is only rewritten while no `i_rwsem` holder and no RCU reader can hold
it.

- **What it buys:** index identity is write-once again -- no name is ever
  rewritten under a lockless reader, so the compare in `__d_lookup_rcu` is
  exact without relying on a `d_seq` retry for renames; no chain-hop false
  negatives, so `d_lookup` and `d_alloc_parallel` stop reading `rename_lock`
  on a miss.  With increments 0 and 1, the lookup and reverse paths read no
  global line at all.
- **What it does NOT buy:** removing `d_seq` from the fast path (K3), or fewer
  `d_lock` acquisitions in `d_move`.  With negative dentries a miss is rare in
  steady state, so the direct reader gain is small; the larger value is
  removing a class of `rename_lock` readers on the way to increment 3.
- **Kernel-specific shortcut to evaluate:** `d_move` already has a target
  dentry sitting in the new bucket under the new name.  Turning *that* into
  the forwarding node (a flag + a host pointer) would avoid the shell
  allocation and the new-bucket insert entirely: the commit becomes {target:
  negative → forwards-to-host, old bucket: marked delete of the host}.  Cost:
  the target must outlive its callers' `dput` until the fold, and the
  overwrite case (positive target) and `d_exchange` need separate handling.
- **Bucket word:** `hlist_bl` spends bit 0 on the lock; dentries are only
  8-byte aligned (`KMEM_CACHE_USERCOPY`, dcache.c:3483), so bits 1-2 are
  free.  The userspace winner uses exactly this budget (bit 0 proxy tag,
  bit 1 deletion mark, bit 2 lock); the kernel version keeps bit 0 as the lock
  and needs a dcache-private bucket type with proxy tag + mark in bits 1-2.
  `rcu_txn_sw_hlist.h` as ported assumes the head holds no lock bit, so the
  settle store would clobber it: the dcache needs an `hlist_bl`-aware variant
  (the userspace bucketlock `bl_*` helpers are the model).
- **A name that survives the commit keeps its chain position.**  "No
  chain-hop false negatives" holds only if no commit moves a LIVE name's node:
  deleting it from mid-chain and inserting its successor at the bucket head,
  even in one atomic commit, lets a reader that read the head before the
  commit and reaches the predecessor after it miss the name altogether.  A
  rename is safe (its old name really disappears, its new name really
  appears; a negative target is a name that did not exist either way), but
  `d_exchange` keeps BOTH names and the fold keeps one: each must be an
  in-place replace (the successor takes the node's exact slot, the node's
  `next` is marked).  The userspace MW engine got this wrong for the exchange
  -- two delete + insert-at-head moves -- and its localized readers, which no
  longer retry a miss under a global sequence, reported existing paths
  ABSENT (thousands per second under `bench_dcache_height`; the global
  `rename_gen` bracket masked it except for file exchanges, which owe no
  bump).  Fixed there by four in-place replaces, as the bucket-lock engine
  already did; `make check-xchg-absent` is the gate.  `hlist_bl` has no
  replace primitive today; the dcache-private bucket type needs one.
- **Every hash-chain reader must learn shells:** `__d_lookup_rcu`,
  `__d_lookup`, `d_hash_and_lookup`, and anything that walks
  `dentry_hashtable` *(enumerate)*.
- **Allocation:** `d_move` runs from `vfs_rename` with sleeping allowed until
  it takes spinlocks, so `rcu_txn_sw_reserve(GFP_KERNEL)` (and the shell
  allocation) go before `write_seqlock`/`d_lock`; the commit itself then
  cannot fail.
- **Gate:** `CONFIG_DCACHE_RCU_TXN` (default n).  With it off, the accessors
  return `&dentry->d_name` and `__d_move` is today's.

### Increment 3 — drop `rename_lock`'s write side from `d_move`

Only once no reader needs it: increments 0-2, plus `d_walk`, `d_set_mounted`
and the scoped `..` check converted to per-dentry validation.  Then `d_move`
needs a deadlock-avoidance order that does not rely on `d_ancestor()` over a
frozen tree — the userspace engine's answer is address-ordered locks plus a
per-dentry `d_moving` flag for the loop check.

- **Win:** renames stop serializing on one global spinlock.  Cross-directory
  renames still take `s_vfs_rename_mutex` in `lock_rename` — that is VFS and
  filesystem policy, which the dcache cannot remove — so the gain is for
  same-directory renames and the `d_move` section itself.
- **Risk:** highest of the four; lock ordering in `__d_move` is subtle and
  fstests/LTP exercise it heavily.  Only worth attempting if increments 0-2
  show `rename_lock` in the profile of a real workload.

### Not proposed: lock-free readdir

`d_children` is walked under `d_lock` by `d_walk`, fsnotify, shrinkers and
libfs readdir.  An RCU child list needs the same deferred re-link as
increment 2, applied to `d_sib`, and shells on `d_children` would violate K2
for every one of those walkers.  The userspace readdir result depended on
exactly that.  Revisit only if increment 2's machinery makes it cheap.

## 5. Where rcu_txn_sw is actually needed

Only increment 2 (and `d_exchange`, four in-place replaces) needs a multi-slot
reader-atomic publish; 0, 1 and 3 do not.  The engine fits the kernel
context well: writers are already mutually excluded by bucket bit locks, so
the single-writer engine's contract holds by construction; `reserve()` moves
the only allocation before the spinlocks; the commit is a release store per
edge plus one `call_rcu`, safe under bit spinlocks on PREEMPT_RT.

## 6. Testing and measurement in the kernel

- **Correctness:** the rcu_txn_sw KUnit suite; lockdep and KCSAN builds;
  fstests (`generic/` rename and dcache tests) on tmpfs and ext4; LTP
  rename*, getcwd*, readlink*; a dcache torture module modelled on the
  userspace stress harnesses (rename storms vs lookups vs `d_path`, with a
  census).
- **Performance:** a tmpfs microbenchmark with N reader threads doing
  stat()/openat() (positive and negative hits) or getcwd/readlink, and M
  threads renaming (same-dir, cross-dir, directory moves); `perf c2c` for the
  `rename_lock` and `d_seq` lines; each increment on vs off in the same
  kernel.  Real renames spend most of their time in the filesystem, so a
  rename microbenchmark overstates `rename_lock`'s share — report it next to
  an fs-bound workload (fsmark, a git checkout).

## 7. Open questions for review

0. ~~Increment A's lifetime rule~~ -- **decided**: the fold takes the
   parent's `i_rwsem` by trylock from `queue_rcu_work()`, escalating to a
   blocking `down_write` in a dedicated worker after N failures.  Still open:
   does any `->d_name` reader hold neither RCU nor the parent's `i_rwsem` nor
   a snapshot yet assume stability?  Increment A's classification will tell;
   such a site moves to the snapshot.  And the value of N.
1. **Increment 0:** is there any consumer of `nd->r_seq` or of
   `path_init`'s wait-while-odd that this note missed (e.g. an out-of-tree
   expectation, or ordering someone relies on)?
2. **Increment 1:** acceptable to make the reverse walks lockless first with
   a bounded fallback to `rename_lock`?  What bound?
3. **Increment 2:** separate shell object, or the `d_move` target turned into
   the forwarding node?  Long names: share the host's refcounted external
   name, or copy?
4. **Scope:** stop after increments 0-1 and measure before committing to 2-3,
   given that `s_vfs_rename_mutex` remains for cross-directory renames?
5. **Upstreaming shape:** `rcu_txn_sw.h` as a standalone library (with its
   KUnit) first, or only together with its first in-tree user?
