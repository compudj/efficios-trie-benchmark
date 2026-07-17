# Lock-free dcache rename: shell-stacking + fold cascade

Design note for `dcache_txn` (S2). **Supersedes** two earlier sketches: the
`dc_key` separate-allocation identity (lost the kernel's inline-name locality)
and the `mutator_lock` + coalesce version of this note (coalesce needs a
serialization point; stacking does not). The engine here takes **no lock and no
seqlock** — renames are lock-free through the urcu-txn (rcu-mcas) engine, and the
reader is a plain RCU walk with an inline name compare.

## Goals, all four at once

1. **inline name** — name bytes live in the dentry, so the RCU walk compares them
   off a cache line it already loaded (kernel `d_iname` parity);
2. **no seqcount** — the reader's identity check is a single inline compare with
   no per-object sequence validation;
3. **stable address identity** — a directory keeps its address across renames, so
   its children (keyed on the parent *pointer*, `hash(&parent, name)`) never
   rehash;
4. **lock-free writers** — concurrent renames compose through MCAS with
   retry-on-abort; no global rename lock, no per-superblock rename mutex.

An inline name mutated *in place* can tear under a lockless compare, which is why
the kernel needs `d_seq` for (1)+(2); and object COW breaks (3). The way out is
to spend an RCU **grace period**: carve a window in which no reader is looking at
a node's name, and do the one in-place name write then — keeping the node's
address put. `call_rcu` *is* that grace-period wait, so scheduling the fixup on
`call_rcu` is all the timing we need (no explicit epoch counter).

## Nodes

One node type plays two roles:

- **content host `C`** — the address-stable dentry. `&C` is its children's parent
  key; `C` is never freed or relocated by a rename. In steady state `C` is also
  its own **named top** (directly in its parent's bucket).
- **shell `S`** — a transient node allocated per rename, carrying a *new* name.
  It becomes the entry's named top and **forwards** down to the content host.

During a rename the entry is a **chain**, named top at the head (in a bucket),
content host at the tail:

```
P's bucket ─→ S_top ─fwd→ S_k ─fwd→ … ─fwd→ C ─→ {children keyed on &C}
              (named)     (relays, forward-only)   (content host, fwd=NULL)
```

### Fields

```
struct dentry {
  /* reader hot line */
  struct urcu_txn_hlist_node d_hash; /* transacted: bucket linkage (only while named top) */
  struct dentry *d_iparent;          /* inline identity: parent addr  (role-1 match) */
  struct qstr    d_iname;            /* inline identity: name bytes    (role-1 match) */

  /* transition chain (transacted; forward read by readers, back only by folds) */
  struct dentry *d_fwd;              /* down toward content host; NULL AT the content host */
  struct dentry *d_back;             /* up toward named top;     NULL AT the named top */

  /* writer-only (accessed only inside rename txns; never on the read path) */
  struct dentry *d_parent;           /* LOGICAL parent (content host); transacted; for is_subdir */
  struct dentry *d_children, *d_sib; /* child list, keyed on &this content host */
  uint64_t d_id; int d_inode;
  struct rcu_head d_rcu;
};
```

"Tombstoned" ≡ **not the named top** ≡ `d_back != NULL`; such a node is never in
any bucket (it left when it was demoted), so a reader scanning a bucket never
encounters one and needs no tombstone check.

## Reader rule (one line over the base RCU walk)

Scan `hash(&cur, comp)` for the named top matching `(d_iparent==&cur,
d_iname==comp)`; then **resolve the content host** before using the result as the
next `cur` or as the terminal (id/inode come from the content host).
Conceptually this is "follow `d_fwd` to the tail," but it is realized in **O(1)**
by a write-once `d_host` skip pointer (`host_of_rcu`, § *readdir*): a reader never
walks the chain, so chain depth is never a reader-side time cost. Downward walks
never read `d_parent` or `d_back`. Forward chain depth is transient (an entry
being renamed right now) and, since nothing traverses it, costs only *memory*,
not time — the property that (once it was actually enforced) closed the
liveness collapse in § *Implementation status*.

## Rename = stack a shell (one MCAS, lock-free)

`rename(entry at /D/old → /D'/new)`, where `T` = the current named top of the
entry (found by resolving `/D/old`), `C` = its content host:

1. Build a fresh shell `S` invisibly: `d_iparent=&D'`, `d_iname=new`,
   `d_fwd=T`, `d_back=NULL`.
2. One MCAS commit:
   - `del T` from `hash(&D, old)` ∘ `insert S` into `hash(&D', new)`  (S is the new named top);
   - `T.d_back: NULL → S`  (T demoted, now a relay);
   - `C.d_parent: &D → &D'`  (reparent the content host — for `is_subdir`);
   - **loop-check validates** (cross-dir only; see below).
3. `call_rcu(fold, S)` — schedule S's fixup.

Concurrent renames of the same entry contend on `del T` (the bucket slot naming
the top): the loser fails its old-value check, retries, and re-stacks on the new
top. Nothing is mutated in place; every existing node is only *linked to*. That
is why stacking is lock-free where coalesce (which had to read-and-swap the live
shell atomically with a state check) was not.

## The fold cascade (per-node `call_rcu`, retry-on-abort)

Each shell's `call_rcu(fold, S)` fires **≥ 1 grace period after S was stacked** —
exactly when S's demotion of its child was published, so a GP later no reader is
still matching that child by name. `fold(N)` re-reads N's *current* neighbours
each attempt (robust to other folds having run) and branches:

- **N is still the named top** (`N.d_back == NULL`, N in a bucket) → **transfer +
  promote**. Let `C = N.d_fwd`. Copy `N.{d_iparent,d_iname}` **into C in place**
  (safe: C is `d_back != NULL`, so out of every bucket → unread by name; the GP
  drained readers that saw C named). Then one MCAS: `del N` from its bucket ∘
  `insert C` into that bucket; `C.d_back: N → NULL`. If `C.d_fwd == NULL`, C is
  the content host and the entry is now a single settled node under the new name
  — **this is where the content host's tombstone clears**. `call_rcu(free, N)`.
- **N is now a middle relay** (`N.d_back != NULL`; a newer rename stacked above
  it) → **splice**. One MCAS unlink from the doubly-linked chain:
  `(N.d_back).d_fwd: N → N.d_fwd` and `(N.d_fwd).d_back: N → N.d_back`. The child
  keeps its tombstone. `call_rcu(free, N)`.

Two rules make the cascade safe under concurrency:

- **retry on abort** — a fold's MCAS can lose to an adjacent fold or a new stack
  on the shared `d_fwd`/`d_back`/bucket slots (the same adjacency serialization
  the hlist uses). On `URCU_TXN_STATUS_ABORT` the worker re-reads N's neighbours
  and re-decides top-vs-relay, so a top that was superseded mid-fold simply
  becomes a splice.
- **self-free only** — a node is freed **only by its own** `fold` worker, never
  by a neighbour's splice. A splice routes *around* N and leaves N (still a valid
  forward target for in-flight readers) to N's own `call_rcu(free, N)` a GP
  later. This is why the chain is **doubly linked**: a splice must rewire N's
  parent's `d_fwd`, which needs `d_back` to find that parent.

Worked example, chain `P → Y → X → C` (two renames), whichever order the workers
fire:
- `fold(X)` sees X a middle relay → splice → `P → Y → C` (C keeps tombstone).
  `fold(Y)` sees Y the top, child now C → transfer `Y`'s name into C, promote C
  into P's bucket, clear tombstone → `P → C(new)`.
- or `fold(Y)` first: Y top, child X → transfer into X, promote X → `P → X → C`.
  `fold(X)`: X now top, child C → transfer into C, promote, clear tombstone.
Both converge to a single content host under the newest name.

### Why the in-place name write is legal (the load-bearing invariant)

> While a content host (or relay) `C` is tombstoned (`d_back != NULL`), **no
> reader reads `C.d_iname`/`C.d_iparent`.**

`C` left every name-bucket when it was demoted and does not re-enter one until its
promotion; reachable only via `d_fwd`, and a forwarder uses `C` as a parent
address / terminal, never comparing its name. The only readers of `C`'s name saw
it while it was the named top, before demotion — and the fold's `call_rcu` GP
drains exactly those. The transfer writes the name, then MCAS-publishes `C` into
the bucket; a reader that finds `C` named afterwards sees the new bytes (release
via the commit), one that finds the shell never reads `C`'s name.

## Cross-directory loop check, lock-free

The hazard `s_vfs_rename_mutex` guards: `root→A`, `root→B`; R1 moves A under B,
R2 moves B under A; each `is_subdir` passes independently, both commit → a
detached `A↔B` cycle. We make the check atomic with the move **by folding the
ancestry walk into the commit's validate set**, since "is D an ancestor of T" is
a pure function of T's parent chain:

`rename(D → child of T)`:
1. Walk `T.d_parent` to the root. If `D` appears → reject `-EINVAL`.
2. Otherwise fold **every `d_parent` edge on that T→root path** into the txn via
   `urcu_txn_load_validate`; the move's write is `D.d_parent: oldP → T`.
3. Commit (composed with the shell-stacking MCAS above).

A concurrent reparent of any T-ancestor mutates a validated edge → our commit
aborts → we re-walk and re-check. Trace the race: R1 writes `A.d_parent`,
validates `{B.d_parent}`; R2 writes `B.d_parent`, validates `{A.d_parent}` —
mutual conflict. R1 commits first; R2's validate of `A.d_parent` fails, R2
re-walks A's chain (now `A→B→root`), finds B, rejects. N-way cycles die the same
way: forming an N-cycle needs each rename to validate an edge another writes, a
conflict cycle in which not all can commit; the last to try aborts, re-walks,
finds the now-formed partial chain, rejects. Livelock-free — every abort
re-checks against strictly-more-committed state.

Requirements / cost:
- `d_parent` is **transacted** but **writer-only** (read via the txn in
  `is_subdir`, written on cross-dir rename). Downward reader walks never touch
  it, so **zero** read-path cost.
- Validate set is O(depth of T); stable ancestors (root, …) never change so never
  *abort* us — abort rate tracks real reparent activity on the shared path, not
  depth. Cross-dir renames are rare. (Later: trim to the LCA, or to D's depth,
  since a deeper D can't be T's ancestor.)
- Fold T's **liveness** edge into the same validate set so a rename into a
  concurrently-unlinked T aborts to `-ENOENT` rather than inserting under a dead
  parent.

## Walk causality: the rename generation counter

The shell mechanism kills **`d_seq`'s** job — per-*component* coherence — but not
**`rename_lock`'s** job: whole-*walk* causality. Those are two separate jobs the
kernel does with two separate mechanisms, and the shell only addressed one. A
plain reader that walks `root → P1 → … → Pn` observes each edge atomically, but
the *composition* is not atomic; and because parents are address-stable, a reader
keeps happily using a `cur` that has silently relocated. The anomaly (resolving
`/A/B/K`):

1. Reader steps onto `X_B` as `/A/B` — legitimately, it is there right now.
2. Writer moves `X_B` `/A/B → /Z/B` (its `d_iparent` becomes `&X_Z`).
3. Writer inserts a new node `M` as `X_B`'s child `"K"` — i.e. at `/Z/B/K`.
4. Reader reads `hash(&X_B,"K")`, finds `M`, returns it **for `/A/B/K`**.

`M` was only ever at `/Z/B/K`; `/A/B/K` was never `M` at any instant — no
linearization point. It needs neither a cross-dir move (a same-directory
`B→B2` rename of an interior component triggers it too — the prefix `/A/B` stops
naming `X_B`, but the reader still holds `X_B`) nor even the insert to be a
*wrong* answer in every variant, but the insert-after-move is the cleanest
demonstration. This is exactly why the kernel bumps `rename_lock` on **every**
`d_move`.

### The mechanism: one global generation, folded into the rename commit

We reintroduce a **single global** `rename_gen` (this is `rename_lock`'s job, not
`d_seq`'s — we do *not* bring back a per-dentry counter for the fast path). Two
properties make it a *txn-mitigated* counter rather than a naked seqlock:

- **Folded into the commit.** `rename_gen` is a transacted slot bumped as part of
  each shell-stacking / fold MCAS commit, so the generation and the structural
  edge change flip at one linearization point. A classic odd/even seqlock write
  bracket (`gen++` before the mutation, `gen++` after) is **not usable here**:
  odd/even assumes writers are mutually exclusive, and two concurrent lock-free
  renames would corrupt it. Folding gives the atomicity *and* the serialization
  for free — concurrent renames conflict on that slot and retry via the engine.
  All renames alias it, so the commit runs under `urcu_txn_expect_conflict()`.
- **Read plain, no odd/even.** Because the bump is atomic-in-commit there is no
  in-progress "odd" state to skip, so the reader needs only change detection. It
  brackets the *whole* walk:

  ```
  g0 = urcu_mcas_read(&dc->rename_gen, TAG);   /* resolves an in-flight commit
                                                  descriptor to its logical value */
  cmm_smp_rmb();
      ... resolve the whole path ...
  cmm_smp_rmb();
  g1 = urcu_mcas_read(&dc->rename_gen, TAG);
  if (g1 != g0) retry;                          /* a rename touched the tree mid-walk */
  ```

  `urcu_mcas_read` is a plain RCU-side load (one acquire load + a branch on the
  common path where the slot is a plain value); it is **not** a transaction — the
  reader never runs `urcu_txn_*`. Cost: two such reads per lookup, versus the
  kernel's `rename_lock` bracket *plus* an O(depth) per-component `d_seq`. We
  still delete `d_seq` (the shell absorbs its in-place-tear job), so even with the
  global counter the reader is strictly cheaper than the kernel's read path.

Cost accepted: folding the counter into every rename serializes renames on that
slot, and a reader retries on *any* rename anywhere (global-retry, like
`rename_lock`). Both are fine under the read-mostly framing — renames are the
slow path — but they mean the txn engine's reader curve falls off with rename
fraction the same shape as the seqlock baseline, and renames no longer scale
lock-free. The headline narrows to *"delete `d_seq`, cheaper reads than the
kernel, rename-serialization at parity."*

### A/B arm: per-node generation — LANDED (`DC_PER_NODE_GEN`)

To keep the reader flat as rename load rises, put the generation **per content
host** (`d_seq`), bumped only by that node's own move — never its ancestors
(bumping to the root just rebuilds the global counter on the hottest line). It
lives on the address-stable host, so it is durable across renames *and* folds (a
fold frees shells from the *top* down; the tail host never moves), and it is
stepped inside the *same* MCAS as the structural edge change (`txn_bump_gen`), so
a reader sees `(gen, index-membership)` as one atom.

The reader (`dc_lookup` under `DC_PER_NODE_GEN`) samples each path host's `d_seq`
descending, stores `(host, gen)` per hop, and re-reads *all* of them at walk-end;
unchanged everywhere means the whole path was simultaneously live at the leaf
**turnaround** instant (each hop's version brackets `[sample, up-read]`, and the
turnaround lies inside every one of those windows). By the edge lemma — a move of
`Pᵢ` changes only `Pᵢ`'s own incoming edge, because children key on the parent
*address* — that is sufficient for causality, and a retry fires only when a node
the walk actually passed through moved. A rename down a disjoint subtree bumps a
`d_seq` this walk never reads: no shared cacheline, which is the whole point over
the global counter.

**The observe-then-read window, and why it closes cheaply here.** The down-sample
has an ordering hazard: the host counter is reached only *after* the name match +
chain resolve, so a bare "find the node, then read its version" samples the
version *after* the edge read it is meant to guard, and a move in that gap is
masked. A kernel `d_seq` walk closes this by sampling the version *before* the
identity read, co-located on one address-stable object. The shell scheme cannot:
identity migrates onto the transient top shell while the durable counter must sit
on the host. **But the scheme also makes identity write-once per node** (a rename
never rewrites a name in place — it stacks a fresh shell), so the version is *not*
guarding a torn identity read at all — it is a pure **freshness** signal. That
turns the fix into a cheap re-verify: sample `host->d_seq`, `rmb`, then confirm the
matched top is *still the current indexed top* — an O(1) test of the deletion MARK
on `top->d_hash.next`, which a rename (demote) and an unlink both set atomically in
the very commit that bumped the gen. Marked ⇒ re-walk. No second identity read, no
bucket re-scan; the fast path (settled entry, `top == host`) reads the gen on the
same node it matched.

### The versioned double-collect is the sound form (the version-less one is not)

Recording the path descending and re-checking it on the way up *is* a
double-collect — the landed per-node reader is exactly that, made sound by the
per-host **version** compared across the two passes. The tempting *version-less*
variant (walk `d_parent` back up and check the two passes agree) stays ruled out:
it is defeated by move-away-move-back ABA (validate `X_B` while `X_A` is displaced,
validate `X_A` after it returns but `X_B` has since left; both collects agree yet
no instant held the whole path), and `d_parent` alone misses same-directory
renames (parent unchanged, only the name moves). So `d_parent` stays strictly the
writer-side loop-check field; the reader's soundness rests on the `d_seq` versions,
not on re-deriving the path.

`dcache_txn` carries the global counter as the **default** and the per-node host
counter behind `-DDC_PER_NODE_GEN`, so the S3 sweep prices **one bracket per walk
(global, retries on any rename anywhere)** against **one versioned double-collect
per walk (per-node, retries only when a node on *this* path moved)** — see the S3
results below.

### S3 results (`figures/dcache_s3.png`, 2×96-core EPYC, best-of-5, conserved)

`bench_dcache` runs the three arms — `seqlock`, `txn` (global `rename_gen`), `txn`
(`-DDC_PER_NODE_GEN`) — in two modes, every one of 420 runs gated on namespace
conservation (**0 failures**). Threads are pinned one-per-physical-core via an
hwloc-derived CPU list (`hwloc-calc core:all.pu:0` → `--cpulist`), so the SMT
siblings stay idle and the reader sweep fills all 192 cores cleanly.

- **Homogeneous mix** (48 threads, each doing `rename-frac` of its ops as renames):
  lookup throughput collapses from ~420 Mops/s at frac 0 to <1 Mops/s by frac 0.5
  on *all three* arms. The collapse is **writer-bound** — a rename is ≈50× the cost
  of a lookup, so even a 1% rename fraction spends most of the thread's time
  renaming — which *masks* the reader-generation difference (per-node edges only
  ~25% ahead of global at the high-frac end, tied at the low end). This is why the
  mix is the wrong instrument for the reader-path question.
- **Role-split** (dedicated readers + writers) isolates the reader path, and the
  per-node localization appears:
  - *32 readers, sweep writers*: per-node reader throughput leads global by
    **1.3–2.1×** across 1–24 writers (e.g. 205 vs 141 Mops/s at 1 writer, 101 vs 50
    at 4), converging only when writers ≈ readers (the rename load itself dominates).
  - *8 writers, sweep readers to 184 (filling all 192 cores)*: the clean scaling
    result — per-node reader throughput **keeps scaling** (8 → **451 Mops/s** from
    2 → 160 readers, easing to 422 at 184 where the writers share the last socket)
    while the global bracket **saturates** (~110–120 Mops/s past ~128 readers) and
    seqlock never scales cleanly at all (a noisy 40–93 Mops/s — reader-retry storms
    under the fixed rename load). At the full-machine point (184 readers) per-node
    is **3.7× global and 5.8× seqlock**; at its 160-reader peak, 3.7× global. The
    global `rename_gen` cacheline, read by every walk and written by every rename,
    is the ceiling; the per-node host counter — read only by walks through the
    moved entry — has none.

**Reading of the experiment:** the txn port's *simplification* win (`d_seq`
deleted, one reader rule) is real and independent of the counter choice; the
*scaling* win on the reader path is real too **but only with per-node
generations** — the global `rename_gen` reintroduces exactly the whole-tree
contention `rename_lock` had. So "dissolve `rename_lock` + `d_seq`" splits into two
claims that must be made separately: `d_seq` dissolves outright; the global-retry
role of `rename_lock` dissolves *only* under the per-node arm.

Correctness parity for the per-node reader: **103/103** suite, the deterministic
walk-causality repro (no misdirection), and concurrent conservation under **ASan
and TSAN** (`make check-pernode`; TSAN on the compiler-atomic-builtins liburcu).

## Directory listing (readdir) and the child index

`readdir` is a reader fast path, not just verification scaffolding: an in-memory
dcache serves listing straight from the child index (the kernel's `dcache_readdir`
walks `d_children` for tmpfs/ramfs/sysfs). So the child list stays — but it
becomes a **first-class concurrent structure**: a per-directory
`rcu-txn-hlist` (the parent holds a child-hlist head; each node's `d_sib` is the
hlist_node), mutated by MCAS in the add / unlink / rename commits and traversed by
`readdir` under `rcu_read_lock`. This is the second `rcu-txn-hlist` in the engine
(the first is the global name-hash), and it replaces the bare `d_children`/`d_sib`
pointer list, whose non-atomic mutation races under concurrent writers (lost
updates / double-links — see `repro_fold`).

`readdir` is a **softer** reader than the path walk: POSIX leaves the effect of a
concurrent rename on an in-progress `readdir` unspecified, so the child-hlist needs
only RCU-safe traversal — **no `rename_gen` bracket**. It does not reopen the
walk-causality problem. The `-ENOTEMPTY` unlink check reads the same head
(`first == NULL`); a concurrent-add-vs-unlink race is validated in the unlink
commit (later).

Moving a node between two directories' child-hlists is a `del ∘ insert` on the
*same* `d_sib` link — the same self-conflict the shell solves for the name-hash —
but `readdir`'s soft consistency permits splitting it into two commits (a brief
in-neither-list transient) or threading the two edges across the stack/fold
commits; picked at implementation time.

### readdir results (`figures/dcache_readdir.png`, 2×96-core EPYC, best-of-5, conserved)

To make the baseline honest, the seqlock arm's `readdir` was upgraded from the
global `mutator_lock` to a **per-directory rwsem** — the faithful kernel analogue
(`iterate_dir` under the inode rwsem): readers of a dir share a read-lock, a rename
write-locks only its affected parent(s); different dirs and concurrent readers of
one dir do not serialize. The txn arm is the lock-free RCU child-hlist walk. The
readers enumerate a random dir while writers rename; the namespace is owned only by
the writers, so dir size is fixed as readers scale. Two axes:

- **Reader scaling** (8 writers, sweep readers to 184, ~32 children/dir): the txn
  walk **scales to ~355 listings/s at 160 readers** (310 at 184, writers sharing
  the last cores) while the per-dir rwsem **saturates at ~15–29** — its read-side
  is a shared cacheline that bounces among readers listing the same dir. At the
  peak that is **~12× the rwsem baseline**, and the txn walk **leads at every
  reader count** (even two readers). It no longer pays a per-child chain walk: the
  `d_host` skip pointer (below) resolves the content host in O(1), so the earlier
  low-reader crossover — where the rwsem's plain `d_sib` walk led — is gone.
- **Writer load** (32 readers, sweep writers, namespace fixed): the txn walk leads
  **~6–7×** at light write load and **stays ~2× above** the rwsem even under a
  saturating rename load (48 writers: txn ~27–35 vs rwsem ~14). It still declines
  as writers rise, but that is now **write-side** pressure — concurrent MCAS churn
  on the child hlists plus coherence traffic — not the reader path, which is O(1)
  in chain depth. (Before the skip pointer this axis was the one wrinkle: the
  per-child chain walk made listing churn-sensitive and it dipped *below* the
  rwsem at 48 writers. Now it does not.)

**Gen-independence.** `readdir` reads **no generation counter at all**, so
`txn-global` and `txn-pernode` run near-identical listing code and their reader
curves track together — the global-vs-per-node distinction that decided the lookup
path is **moot** for listing (any divergence at saturating write load is a
writer-side effect, not the walk). Directory listing is thus the *easy* case for
the port: it dissolves to a bare, O(1)-resolved RCU child-hlist walk with no
`rename_gen`, no `d_seq`, no cursor. The former per-child chain walk that made it
churn-sensitive was removed by a **write-once `d_host` skip pointer overlaid on
`d_id`** (`host_of_rcu`): a host reads the slot as its id, a shell as a pointer
straight to the tail host, discriminated by `d_fwd==NULL` — one hop to the host at
zero memory cost.

## Concurrent operations mid-transition

- **Re-rename** (stack): a plain new stack on the current top; no coordination.
- **Rename a child** of a mid-transition content host: independent — the child's
  parent key `&C` is stable.
- **Add a child**: `&C` is a valid parent address; hashes under `hash(&C, name)`
  as always.
- **Unlink** the entry: requires `C.d_children == NULL`. Remove the named top
  from **both** indexes in one commit **without demoting it** (`d_back` stays
  `NULL`) + bump `rename_gen`. If the top was the settled host, free it directly;
  if it was a live shell, its pending fold reads the still-`NULL` `d_back` on an
  `-ENOENT` transfer as an unlink and **RECLAIMs** the orphaned chain (detach +
  promote-without-reindex cascade, host freed at the tail) — see *Implementation
  status*. Self-free is preserved; no chain is freed twice.

## Implementation status (landed + stress-validated)

Implemented in `dcache_txn.c`: `stack_shell()` commits the whole stack in **one
MCAS** — del old top ∘ insert shell in **both** indexes (name-hash *and* the
child-hlist, the shell is the vehicle in each), `rename_gen += 2`, and demote the
old top (`d_back: NULL → S`) atomically with its removal, so a fold worker never
reads a `d_back` inconsistent with whether the node is still indexed (no
plain-store window to spin on). `fold()` / `fold_cb()` run the transfer/splice
cascade above from `call_rcu`, over a **transacted** doubly-linked `d_fwd`/`d_back`
chain (`DC_FWD_TAG`); readers resolve `d_fwd` with `urcu_mcas_read()`.

Validated: single-thread harness 66/66 on **both** engines + ASan-clean; both
repros PASS (`repro_dcache`, `repro_fold`); and the primary validator
`stress_dcache.c` — W writers moving disjoint leaves among fixed dirs (exercises
a writer's next stack racing its own chain's pending fold: transfer↔splice
re-classification and the stack's `-ENOENT` re-find) + readers asserting
`POSITIVE ⇒ id==name` live — passes namespace conservation with **zero** torn
reads, ASan-clean, across contention regimes up to 8 writers.

**Empirical finding — the async fold is grace-period-bound.** Deferred folds
drain only as fast as GPs advance. Anything that keeps a *registered* thread from
reporting a quiescent state under write load stalls **all** reclamation: the
harness's `main()` sitting in `pthread_join` (online, never quiescing) held every
GP open, so `folds=0` for the whole active run and chains grew without bound
(`max_chain` into the thousands, RSS into the hundreds of MB). In steady state,
fixing the workload (`main` goes `rcu_thread_offline()` while joining) restores
concurrent drain: chains reach a **bounded** depth ≈ rename-rate × GP-latency.
Crucially, since every access to the chain is now **O(1)** (the `d_host` skip
pointer — reader, readdir, walk, fold, *and* the writers all resolve the host in
one hop), a stalled GP costs only **memory**, not time: a deep chain is never
traversed. That was not always true — an earlier writer path *walked* the chain
to measure its depth, which turned a GP stall into an O(n²) time collapse; see
the liveness note below.

TSAN-clean too: on a liburcu built `--enable-compiler-atomic-builtins` +
`-fsanitize=thread` (`urcu-txn-tsan-build/`, `make stress-tsan`), the leaf stress
reports **zero** races; a deliberately-injected plain-store race fires, proving
the pipeline is live.

**Concurrent directory moves — landed + validated.** `d_parent` is now a
transacted slot (`DC_PARENT_TAG`), and a cross-parent rename folds the whole
`new_parent → root` ancestry walk into its commit's validate set via
`urcu_txn_load_validate()` (§ *Cross-directory loop check*), rejecting `-EINVAL`
if the victim is on the path, with a `DC_LOOP_MAX` cap that retries on a
transient (in-flight) cycle.  `stress_dcache_dirs.c` (`make stress-dirs` /
`-asan` / `-tsan`) drives (a) conserved subtree relocation among fixed anchors
and (b) a mutual-cycle race — two threads per pair each nesting X under Y / Y
under X, where the loop check must make a joint cycle impossible (each side
validates the d_parent edge the other writes, so at most one commits; the loser
re-walks and rejects).  It passes conservation, a live checker that never sees
either node detach from the root, and thousands of genuine `-EINVAL`
rejections — **ASan-clean and TSAN-clean**.  No cycle ever forms.

**The bistable liveness collapse — root-caused and fixed (`08c069b`).** This
supersedes two earlier notes here (a synchronous "fold-ahead" relief valve, and a
claim that the lane-parking collapse was a `-DDC_STRESS_DEBUG` artifact); both
were treating symptoms of one bug.

The bug: to decide whether a chain needed relief, the writer *walked* it —
`chain_host_depth_rcu()`, inside its own `rcu_read_lock()`, on every rename, just
to measure depth. That walk is O(chain) exactly when the chain is pathological
(the GP-bound fold having fallen behind). A writer stuck in it never reports a
quiescent state, so grace periods stall, so the fold drains even less, so the
chain grows and the next walk is longer still — the relief valve's own trigger
drove the starvation it existed to relieve. The result is a **bistable ~60×
collapse**: `bench_dcache_height --move-height 6` (8 writers exchanging sibling
subtrees onto ~16 hot nodes) runs in 1.06 s normally and >60 s about one run in
15, with every writer caught in `chain_host_depth_rcu` while every `call_rcu`
worker sits in `synchronize_rcu`. It reproduces on a **clean** build (correcting
the earlier "DEBUG artifact" note — `dc_dbg_max_chain` amplifies but is not
necessary) and it is **not** lane-parking (proven by a `-DDC_NO_LANE` control:
same collapse, zero threads parked).

The fix: the host was already reachable in one hop via the write-once `d_host`
skip pointer (`host_of_rcu`), so the walk existed *only* to count hops. Point the
writers at `host_of_rcu`, delete both chain walkers, and delete the depth, its
trigger, and the whole `fold_ahead()` machinery (with it the transacted
`d_spliced` self-marker it needed). Nothing traverses a chain now: reader,
readdir, walk, fold, and writers are all O(1) in depth. `fold_ahead` was retired
rather than re-homed because it had **no regime where it helped** — redundant when
GPs advance (`call_rcu` already batches the fold drain per GP) and unable to run
at all when they stall (no `fold_cb` runs). A GP stall now degrades to
bounded-rate **memory** growth (§ *grace-period-bound* above), the honest
consequence: it halts *all* reclamation process-wide and is a quiescing bug to fix
at its source (`rcu_thread_offline()` while blocking), not something a per-rename
chain walk should paper over. Measured on the repro: 10/45 stalls → 0/45.

**Mid-transition unlink — landed + validated.** `dc_unlink` removes the current
named top from **both** indexes and bumps `rename_gen` in one commit, retrying if
a concurrent fold promotes a successor between find and del. When the top is the
settled host (`top == host`, no fold queued) unlink frees it directly; when the
top is a live rename **shell** (`top != host`) it removes it *without demoting it*
(`d_back` stays `NULL`) and lets the shell's pending fold do the reclamation. The
fold reads that state — its `TRANSFER` replace returning `-ENOENT` while `d_back`
is *still* `NULL` (a re-rename would have set `d_back` atomically with the del) —
and enters a **RECLAIM** cascade: detach the orphan top (`store d_fwd = NULL`,
which *conflicts* with a concurrent `SPLICE` of the successor so the two cannot
commit inconsistently) and promote the successor **without re-indexing**, so it
stays out of every index and its own fold reclaims in turn; the content host at
the tail (no fold queued) is freed by whoever reaches it. Self-free is preserved
(each shell freed once, by its own fold). `stress_dcache.c` now devotes a
fraction of iterations to rename-then-**unlink** with no quiesce between, so the
unlink lands on a live shell and the reclaim races the writer's own and earlier
folds draining on the worker — conservation holds with chains up to depth ~45,
**ASan- and TSAN-clean**; single-thread `test_dcache` covers 1- and 2-shell
reclaim deterministically (drained at `dc_destroy`, ASan-clean).

**Atomic exchange — landed + validated.** The per-entry stack is factored into
`stack_one_prepare()` (records only, no commit), so `dc_rename_exchange` composes
**two** shell stacks into **one** MCAS commit — both index del/insert pairs, both
demotes, the `rename_gen` bump, and **both** cross-parent loop-check walks +
`d_parent` reparents — replacing the old three-sequential-stacks scratch-name
placeholder. A cycle-forming swap is rejected `-EINVAL` atomically (either loop
check sees the other host on its path, over a read set that already reflects the
swap's reparents via read-your-own-writes). `stress_dcache_xchg.c` (`make xchg` /
`-asan` / `-tsan`) drives a permutation over fixed slots with disjoint-owner
pairs contending on the shared dir child-hlists, and asserts the property the
sequential placeholder could **not** satisfy: a slot path is never momentarily
empty, so every reader lookup of a valid slot is `POSITIVE` — **zero** ABSENT
reads across contention up to 8 writers over 3 dirs, permutation conserved,
ASan- and TSAN-clean.

## Costs

- One shell allocation + one `call_rcu` fold + (on collapse) `call_rcu` frees per
  rename; a re-rename adds one shell + fold before the chain compresses.
- `d_fwd`/`d_back` (16 B) + `d_parent` (8 B) per dentry; idle in steady state
  (`d_back==NULL`, `d_fwd==NULL`).
- Transient extra forward hops for lookups descending into an entry that is
  *right now* mid-rename.
- The reader fast path pays **none** of it: inline name, no seqcount, no global
  anchor, no `d_parent`. That is the number S3 weighs against the seqlock
  engine's per-lookup `d_seq` read *and* its `rename_lock`-serialized writers.

## Consequence for the experiment framing

The txn engine takes **no writer lock** for the tree surgery itself — the shell
stack/fold is lock-free MCAS — so the S1/S3 "one `mutator_lock` as a controlled
variable" framing no longer applies to the txn side. But the walk-causality
counter (above) qualifies the "wins on both axes" claim, and by how much depends
on which arm:

- **Global arm (default).** The reader deletes `d_seq` but still brackets the
  walk on one global `rename_gen`, so it retries on any rename (reader curve falls
  off with rename fraction like the baseline), and folding the counter into every
  commit serializes renames on that slot (writer concurrency at kernel parity).
  Honest headline: *delete `d_seq`, cheaper reader than kernel's
  `rename_lock`+`d_seq`, rename-serialization at parity.*
- **Per-node arm (A/B knob).** Restores reader-flat-under-rename-fraction and
  lock-free concurrent renames, at a per-hop reader bracket. This is the arm that
  would substantiate a "wins on both axes" claim; S3 measures whether its
  fast-path cost is worth it and where the two arms cross.

The comparison is thus kernel-scheme (serialized renames + `d_seq`) vs txn with a
*single* global counter (or per-node, under the knob) — not "fully lock-free txn
with no counter at all." The README's controlled-variable note is updated to
match.

## Open questions / later phases

- **`..` / getpath (phase 2+).** A tombstoned content host's `d_iname` is stale
  for a *direct* reader — none exist on the downward path, but an upward
  `..`/getpath would see the old name until the fold. Revisit with `..`.
- **Negative dentries (phase 2).** `d_instantiate` is a single-slot `d_inode`
  publish, orthogonal to this; a rename onto a negative target folds into the
  same shell machinery.
- **Exchange (`RENAME_EXCHANGE`).** Two entries swap names/parents; expressible
  as two shell stacks in one commit, but the loop check must validate *both*
  ancestor paths and the two writes alias on shared bucket/edge slots — needs the
  read-your-own-writes handle + `expect_conflict`. Spec separately.
- **Same-bucket rename.** `hash(&D,old) == hash(&D',new)`: the `del T ∘ insert S`
  shares a bucket-head slot — run on the RYW handle, not `declare_disjoint`.
- **Escalation.** A wide cross-dir commit (deep validate set) that keeps aborting
  escalates into the engine's fair-mutex lane; that is the engine's job, not a
  new lock here.
- **Provenance.** Relativistic-move / RCU-resize (an object reachable by two
  routes during a transition, collapsed after quiescence) applied to *identity*.
```
