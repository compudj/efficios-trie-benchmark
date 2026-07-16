# dcache-in-userspace: can urcu-txn dissolve `rename_lock`?

Status: **S1 done; S2 functional + walk-causality landed** (2026-07-15). Both
engines pass the single-threaded harness (52/52, ASan clean). The txn rename
mechanism is specified in [`rename-shell-transition.md`](rename-shell-transition.md).
The first concurrency race — a walker misdirected by a mid-walk rename
(`rename_lock`'s job) — is reproduced deterministically (`repro_dcache.c`) and
closed with a global rename generation counter. Remaining S2 concurrency work:
async `call_rcu` fold + splice, transacted `d_parent` loop check, atomic
exchange.

### Files

| File | Role |
|---|---|
| `dcache.h` | engine-agnostic interface + inline qstr/path helpers; the two bolt-on seams |
| `seqcount.h` | userspace seqcount + seqlock over urcu barriers (the `rename_lock`/`d_seq` machinery) |
| `dcache_seqlock.c` | faithful kernel-style baseline (RCU hlist + global `rename_lock` + per-dentry `d_seq`) |
| `dcache_txn.c` | **lock-free** urcu-txn engine (S2, in progress) — see the design note |
| `rename-shell-transition.md` | the lock-free rename design: shell-stacking + fold cascade + ancestor-validate loop check |
| `test_dcache.c` | single-threaded correctness + namespace-conservation harness |
| `repro_dcache.c` | deterministic 1-writer/1-walker repro of the walk-causality race (`make repro`) |
| `Makefile` | `make check` builds+runs; `ENGINE=txn` swaps engine; `make repro` runs the race repro |

Build/run: `cd experiments/dcache && make check` (needs `make urcu-txn` at the
repo root once).

## Thesis

The Linux dentry cache is the hardest RCU user in the kernel, and the thing that
makes it hard is *rename*. A `d_move()` can relocate a live dentry to an
arbitrary point in the namespace tree while lockless path walks are mid-flight
through it. The kernel copes with a **global** consistency scheme:

- a system-wide `seqlock_t rename_lock` that every RCU path walk brackets with
  `read_seqbegin()` / `read_seqretry()` — *any* rename *anywhere* forces the
  walk to retry from the top; and
- a per-dentry `seqcount_spinlock_t d_seq` that `__d_lookup_rcu()` validates so a
  single-component match is coherent (name vs parent vs hash-bucket membership).

Both are **global-or-per-object sequence counters read on the fast path**. This
experiment asks whether an urcu-txn (rcu-mcas) formulation — where a rename is a
single multi-slot commit and a lookup validates only *the slots it actually
consumed* (the engine's "help iff the slot is in the txn's own read/write set"
read policy) — can:

1. **Simplify** — delete `rename_lock`, `d_seq`, and the RCU-walk→ref-walk
   `unlazy_walk()` fallback machinery, replacing them with local slot
   validation; and
2. **Scale** — remove the global rename serialization so both concurrent walks
   *and* concurrent renames stop contending on one counter/lock.

We measure both: an LOC / invariant-surface diff for (1), a rename-fraction ×
core-count sweep for (2), gated by a namespace-conservation invariant so a
corrupted run can't masquerade as a fast one (same discipline as
`bench_txn_3skiplist`'s conservation check).

### What the txn win is — reader *and* writer

An earlier draft of this note argued the txn win was reader-only, because the
kernel *already* serializes cross-directory renames on a per-superblock
`s_vfs_rename_mutex` (to prevent directory loops), so no engine could scale
cross-dir renames. **That turned out to be wrong**: the loop check can be made
lock-free by folding the `is_subdir` ancestor walk into the rename commit's
MCAS validate set (a concurrent reparent of any target-ancestor aborts the
commit and forces a re-check). See `rename-shell-transition.md`. So the txn
engine takes **no rename lock at all** and wins on *both* axes:

- **Reader side.** The seqlock engine bumps the *global* `rename_lock` on *every*
  rename — even a same-directory name change — so every in-flight walk
  *everywhere* retries, and each lookup validates a per-dentry `d_seq`. The txn
  reader does an inline name compare and never touches a sequence counter; a
  rename is atomic to it via the MCAS commit alone.
- **Writer side.** The seqlock engine serializes *all* renames on the
  `rename_lock` seqlock (plus an `s_vfs_rename_mutex`-analog for cross-dir loop
  safety). The txn engine's renames are lock-free — one MCAS to stack a shell,
  compressed by per-node `call_rcu` fold workers — so disjoint renames proceed
  concurrently.

The comparison is therefore **kernel-scheme (serialized renames + `d_seq`) vs
fully-lock-free txn**. Headline axis stays rename fraction: as it rises, the
seqlock engine degrades on *both* the reader path (global-retry storms) and the
writer path (serialized renames), while the txn engine stays local and
lock-free on both.

## What we actually port (the RCU-relevant core)

The dcache is enormous; most of it is orthogonal to the rename/RCU question. We
carve out the part that *is* the question:

| Ported | Kernel counterpart |
|---|---|
| dentry: parent ptr, name (`qstr`), children/sibling links, hash-bucket link | `struct dentry` (`d_parent`, `d_name`, `d_children`/`d_sib`, `d_hash`) |
| the `(parent, name) → dentry` hash table + lockless lookup | `dentry_hashtable` (`hlist_bl`), `__d_lookup_rcu` |
| insert / unlink | `d_add` / `__d_add`, `d_delete` / `__d_drop` |
| **rename, incl. exchange** | `d_move` / `__d_move` (`RENAME_EXCHANGE`) |
| multi-component path walk | `link_path_walk` → `walk_component` → `lookup_fast` |

**Deliberately out of scope** (stubbed or omitted — none change the rename/RCU
story, all add bulk): LRU + shrinker, negative-dentry lifecycle, mounts/
`d_splice_alias`, inode alias/hardlink management, external-name refcounting,
`lockref` cmpxchg refcounting (we use a plain atomic refcount), security/audit
hooks, case-folding.

Why multi-component walk is *in* scope: a single-component lookup can't be
misdirected by a rename — the cross-tree hazard only appears when a walk holds a
dentry from step *k* and dereferences its child at step *k+1* after that dentry
has been moved. That is the exact race `rename_lock` exists to catch, so the
harness must walk paths of depth > 1 or it isn't testing anything.

## Two implementations, one interface

Both satisfy the same `dcache.h` API (`dc_lookup_path`, `dc_add`, `dc_unlink`,
`dc_rename`, `dc_rename_exchange`), so the harness is engine-agnostic:

- **`dcache_seqlock`** — faithful kernel-style port: `hlist_bl`-equivalent
  buckets, global `rename_lock` seqcount, per-dentry `d_seq`, RCU-walk with
  retry + ref-walk fallback. This is the **baseline we are trying to beat and
  simplify** — the honest comparison is "can txn beat the kernel's own scheme,"
  not "can txn beat a coarse mutex."
- **`dcache_txn`** — **lock-free** urcu-txn port (full design:
  `rename-shell-transition.md`). Names stay **inline** on the dentry (kernel
  `d_iname` locality); the reader is a plain RCU walk with an inline name compare
  and no `d_seq`. A rename keeps the dentry's address (children never rehash) by
  **stacking a transient named "shell"** — one MCAS — that forwards to the
  content host; per-node `call_rcu` fold workers compress the chain back to a
  single node, doing the one in-place name write inside a grace-period window
  where no reader can see it. Cross-dir loop safety folds the `is_subdir` walk
  into the commit's validate set (no rename lock).

An optional coarse **`dcache_rwlock`** (one rwlock over the whole cache) can
anchor the low end of the scaling plot — it makes the seqlock port's cleverness
legible, but it isn't the point.

## Benchmark design (`bench_dcache`)

Modeled on `bench_txn_3skiplist` / the `scripts/run_*.sh` + `plot_*.py` +
`figures/` flow:

- Build a synthetic namespace tree (fixed fan-out × depth) and warm every engine
  uniformly (warm-vs-warm — see the "prime all engines" project rule).
- `--nthreads` workers each running a mix: `--rename-frac` of ops are renames
  (disjoint subtree moves + a slice of `RENAME_EXCHANGE`), the rest are
  full-path lookups of depth `--depth`.
- **Headline independent variable: rename fraction.** The seqlock baseline
  should be flat-and-fast at 0% rename and fall off as rename fraction climbs —
  on *both* the reader path (walks retry the global `rename_lock`) and the writer
  path (renames serialize on it); the txn engine should localize both. Secondary
  axis: core count at a fixed rename fraction.
- Metric: lookup throughput **and rename throughput** (Mops/s), plus walk-retry
  rate and rename-serialization for the seqlock engine (the mechanism behind any
  gap). Rename throughput is now a co-headline, not a footnote — the txn engine's
  lock-free renames are half the story.
- **Invariant gate:** every run ends by verifying namespace conservation — the
  set of full paths reachable from the root equals the set implied by the
  recorded rename log. Failure prints `CONSERVATION FAILED` and exits nonzero.

## Correctness bar (what must be *shown*, not asserted)

The txn port only earns the simplification claim if it demonstrably handles the
races `rename_lock`/`d_seq` were built for:

1. **Cross-tree misdirection** — a walk holding dentry `X` at depth *k* whose
   child edge is consumed after `X` was moved elsewhere must not silently walk
   the wrong subtree; its slot validation must abort/retry or observe a
   consistent snapshot.
2. **Rename loop / A-into-B-into-A** — `RENAME_EXCHANGE` and ancestor-descendant
   moves must not livelock a concurrent walk or produce a cycle.
3. **d_seq's job** — name / parent / bucket-membership must be mutually coherent
   at the point of match, from slot validation alone.

Plan: reproduce each deterministically under a single writer + single walker
(as `bench_txn_3skiplist` did for the torn-tower bug), *then* run under the
contended sweep. A race we can't trigger on demand we don't claim to have fixed.

## Staging

- **S0** (this session) — directory + this plan + open-decision sign-off.
- **S1** — `dcache.h` interface; `dcache_seqlock` baseline; single-threaded path
  walk + correctness harness.
- **S2** — `dcache_txn` behind the same interface; the 3 correctness
  reproductions above.
- **S3** ✅ — concurrent `bench_dcache` (homogeneous rename-fraction mix **and** a
  role-split reader-vs-writer mode), 3-arm sweep + conservation gate;
  `scripts/run_dcache.sh` + `plot_dcache.py` → `figures/dcache_s3.png`. **Finding:
  the txn port deletes `d_seq` (real simplification) but the DEFAULT reader still
  brackets one *global* `rename_gen`, so on the reader path it ties the seqlock
  baseline — the scaling win needs the per-node arm.** Landed a third arm,
  `dcache_txn` under `-DDC_PER_NODE_GEN`: a per-content-host generation bumped only
  by the moved entry, validated by a versioned descent+up-pass double-collect
  (sample host gen → confirm the matched top is still indexed via the O(1)
  hlist-delete MARK → revalidate all path hosts on the way up). Same correctness
  bar (103/103, walk-causality repro, ASan/TSAN-clean stress: `make check-pernode`).
  In the homogeneous mix the collapse is *writer*-bound (a rename ≈ 50× a lookup),
  so the reader-gen difference is masked; the role-split isolates it and the
  per-node arm runs materially faster than global on the contended reader path.
  Sweeping readers to 184 (8 writers, filling all 192 cores one-thread-per-core via
  an hwloc-derived pin list), per-node **keeps scaling to ~450 Mops/s** while the
  global bracket saturates ~110–120 and seqlock never scales cleanly — **3.7×
  global, 5.8× seqlock at the full-machine point** (see `rename-shell-transition.md`
  S3 results).
- **S3-readdir** ✅ — directory-listing companion (`bench_dcache --readdir`,
  panels `readdir_scale`/`readdir_w` → `figures/dcache_readdir.png`). The seqlock
  baseline was upgraded to an honest **per-directory rwsem** (the kernel inode-rwsem
  analogue, not one global lock). **Finding: listing is the *easy* case — the txn
  `readdir` reads no generation counter at all, so `txn-global` ≡ `txn-pernode`
  (they overlap), and it dissolves to a bare lock-free RCU child-hlist walk.** It
  **scales to ~355 listings/s at 160 readers (~12× the rwsem, which saturates ~15–29**
  — its read-side is a shared cacheline), and it **leads at every reader count**.
  A **write-once `d_host` skip pointer overlaid on `d_id`** (`host_of_rcu`) resolves
  the content host in O(1) regardless of chain depth, so the walk is no longer
  churn-sensitive: under a saturating rename load it **stays ~2× above** the rwsem
  (48 writers: ~27–35 vs ~14) instead of dipping below. Both engines
  conservation-clean + ASan-clean.
- **S4** — simplification analysis (LOC + invariant surface: which
  counters/fallbacks disappear) + scaling figures + writeup back into `design/`.

## Locked decisions (2026-07-15)

1. **Port scope** — **RCU-core-only** (the table above), with the two bolt-on
   seams designed into the interface from S1: (a) `dc_lookup` returns tri-state
   *positive / negative / absent*, and (b) unlink does honest RCU-deferred
   reclaim (real `call_rcu` free), so phase 2/3 reuse the same reclaim path
   instead of forcing a redesign. Phasing:
   - **Phase 1** (now): RCU-core — tree + hash + rename/exchange + walk.
   - **Phase 2**: negative dentries (payload state + `d_instantiate` transition;
     doesn't touch the rename mechanism).
   - **Phase 3**: LRU + shrinker (makes lifetime/refcount first-class; the
     shrinker is "just another unlink mutator" over the S1 reclaim primitive).
2. **Baseline** — **faithful `rename_lock` seqcount + per-dentry `d_seq` port**.
   The honest "can txn beat the kernel's actual design" comparison. A coarse
   rwlock engine may be added later only to anchor the low end of the plot.
3. **Headline axis** — **rename fraction** at fixed cores (seqlock reader path
   should fall off under global-retry storms; txn stays local), with core-count
   as the secondary axis.

## References

- Design: [`rename-shell-transition.md`](rename-shell-transition.md) — the
  lock-free rename mechanism (shell-stacking, fold cascade, ancestor-validate
  loop check) the `dcache_txn` engine implements.
- Kernel: `fs/dcache.c` (`__d_lookup_rcu`, `__d_move`, `d_move`, `__d_add`,
  `dentry_hashtable`), `fs/namei.c` (`link_path_walk`, `walk_component`,
  `lookup_fast`, `unlazy_walk`), `include/linux/dcache.h`.
- Engine: `urcu-txn-build/include/urcu/rcu-txn.h`, `.../rcu-txn-hlist.h`,
  `.../rcu-mcas.h`; the read-policy rule and RYW/chaining idioms in
  `design/rcu-txn-*.md` and the `rcu-pseudo-transaction` skill.
