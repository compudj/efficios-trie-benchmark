# S4 — simplification & invariant-surface analysis

What the urcu-txn port *removes* versus the faithful `rename_lock` + `d_seq`
baseline, and — just as important — what it deliberately *keeps* and why. The
qualitative analysis (§1–4), the cache-line locality work (§5), the correctness
invariants the per-node counter rests on (§6), and the S3 scaling curves that
measure the payoff (§7). The mechanical LOC accounting closes §1: the port is ~2×
the lines and fewer concurrency invariants.

The thesis this file defends: the port replaces **two kernel seqcount mechanisms
plus a global mutator serialization** with **one uniform node type discriminated
at runtime**, and the residual redundancies it carries are each a zero-cost
denormalization rather than accidental duplication.

## 1. What dissolves

The baseline (`dcache_seqlock.c`, faithful to the kernel) carries three
serialization mechanisms on the rename/lookup path:

| baseline mechanism | role | in the txn port |
|---|---|---|
| per-dentry `d_seq` (seqcount) | per-**component** coherence: a reader re-reads a dentry's name/parent under an even/odd seqcount so it never observes a half-applied rename | **dissolved outright.** Shell-stacking makes identity *write-once* — a rename stacks a fresh named shell, it never mutates a name in place — and the one in-place name write (the fold's TRANSFER into the host) lands inside a `call_rcu` grace-period window no reader can see. With nothing torn to guard, the per-component counter has no job. |
| `rename_lock` (global seqcount) | whole-**walk** causality: a path walk brackets its whole descent on a tree-global seqcount so it can't stitch together a path that existed at no instant | **demoted to one transacted counter.** Default build: a single `rename_gen` void\* folded into each rename's MCAS commit (still whole-tree, but a lock-free counter, not a lock). Per-node build (`-DDC_PER_NODE_GEN`): dissolves into per-host generations, bumped only by the moved entry — the whole-tree contention is gone, which is the scaling result in `rename-shell-transition.md` §S3. Sampling a counter on the *host* (not the parent that names it) is sound only because of two invariants — §6. |
| global mutator serialization (rename path takes the tree lock; the kernel also leans on `s_vfs_rename_mutex` for cross-dir) | writer/writer exclusion | **dissolved.** Renames are lock-free MCAS commits. The cross-dir loop check is folded into the commit's validate set (walk `new_parent→root` over the transacted `d_parent` chain), so cycle prevention is atomic with the move rather than a separate held lock. |

Net counter/lock surface: **2 seqcounts + 1 mutator lock → 1 transacted counter
(or 0, per-node) + a reactive escalation lane.** The escalation lane
(`fair-mutex.h`, entered only after an MCAS retry blows its cost budget) is the
port's analogue of the kernel's RCU→refcount fallback (`lockref`): both are the
"contended path" backstop, but the txn lane is *reactive* (engaged only under
measured contention) rather than taken on every write.

### LOC accounting

The port is **not fewer lines** — the opposite. Raw `wc -l`: `dcache_seqlock.c`
**747** → `dcache_txn.c` **1498** (+751, ~2.0×); comment share 18% → 29%. Where
the +751 goes, bucketed by operation (spans are approximate — a definition's
leading doc-comment is counted with the *preceding* definition — but the shape is
robust):

| bucket | seqlock | txn | Δ |
|---|--:|--:|--:|
| **lookup** (walk + helpers) | ~141 | ~248 | +107 |
| **rename** (shell-stack + fold + fold-ahead) | ~79 | ~461 | **+382** |
| **unlink** | ~39 | ~91 | +52 |
| **exchange** | ~69 | ~119 | +50 |
| **readdir** | ~33 | ~28 | **−5** |
| per-dir rwlock scaffolding (`dir_wlock`, `dirs_wlock2`, …) | ~26 | 0 | −26 |
| hand-rolled RCU-hlist (`dc_hnode`/`dc_bucket`/add/del) | ~33 | 0 → library | −33 |
| tag / cache-line layout machinery (§5) | 0 | ~47 | +47 |
| shared plumbing (structs, alloc, `dc_add`, walk, …) | ~268 | ~424 | +156 |
| file header / includes / macros | ~59 | ~80 | +21 |

Three things to read off it. **(1) The growth is almost entirely `rename`**
(+382 of +751): the seqlock rename is a ~79-line critical section (`dc_rename` +
`__d_move` + `is_subdir`) held under `mutator_lock → rename_lock → dir-rwsems`;
the txn rename is a ~461-line *lock-free state machine* — `stack_shell` +
composable `stack_one_prepare` + async `fold`/`fold_cb` + the `fold_ahead` relief
valve + chain resolution. That is the whole trade: a held lock is cheap in lines
and dear in invariants; a lock-free protocol is the reverse. **(2) The scaffolding
genuinely disappears** — the per-directory rwlock (~26) and the hand-rolled
RCU-hlist (~33) vanish from the `.c` (the hlist relocates to the reusable
`rcu-txn-hlist` library), and the seqcount read-retry loop inside the seqlock
lookup is *not* carried forward. **(3) `readdir` is the one bucket that shrinks**
(−5): the "easy case" of §7.2 is literally fewer lines — a bare `rcu_read_lock`
child-hlist walk beats the rwsem version.

So the file-size claim is honest and unflattering: **2× the lines.** The claim
that *does* hold is the one this document is organized around — **fewer
concurrency invariants.** The seqlock's 79-line rename leans on invariants that
live *outside* the function and outside the file: a global lock order that must
never invert, a `rename_lock` bracket every reader must take, and an even/odd
`d_seq` phase every reader must re-validate on *every component*. The txn port's
461 lines are self-contained: their correctness is the write-once / immutable-key
edge invariants of §6 plus MCAS commit atomicity — no lock order to preserve, no
per-reader seqcount phase to reason about, and (per-node) no whole-tree bracket.
The lines moved *up*; the things a reader or a reviewer must simultaneously hold
in their head moved *down*.

## 2. One node type, discriminated at runtime

The port uses a single `struct dentry` for every node and reads its *kind* from
field state rather than a type tag. There is no `d_kind`, no `is_dir`, no
`d_flags`. Two orthogonal axes, each read from an existing field:

- **host vs. shell** — `d_fwd == NULL` ⟺ this node is a content host (the
  address-stable tail); `d_fwd != NULL` ⟺ it is a transient rename shell. A
  *stable per-node property*: a node is born a host or a shell and never crosses
  over (TRANSFER frees the shell and re-promotes the pre-existing host — it does
  not reclassify a node). This single predicate also selects the `d_id`/`d_host`
  union member, so no separate discriminator is needed for that either.
- **directory vs. file** — *not distinguished at all.* A file is a directory
  with an empty `d_child_head`; `-ENOTEMPTY` is "is the child head empty" and a
  file passes trivially. (`-ENOTDIR` is not enforced by type — a scope choice,
  see §4.)

Which fields are live for which kind:

| field | host (settled) | host (demoted) | shell (top) | shell (relay) | notes |
|---|:--:|:--:|:--:|:--:|---|
| `d_hash` (name bucket) | ● | | ● | | *indexed* node only — one per chain at a time |
| `d_iparent`,`d_iname` | ● | | ● | | inline identity; matched by the reader |
| `d_fwd` | =NULL | =NULL | ● | ● | successor **and** host/shell discriminator |
| `d_back` | =NULL | ● | =NULL | ● | immediate predecessor; **fold workers only** |
| `d_spliced` | | | | ● | fold-ahead self-marker |
| `d_parent` | ● | ● | (birth) | | logical parent; transacted; load-bearing on hosts |
| `d_dc` | ● | ● | ● | ● | owner domain (a `call_rcu` fold must reach it) |
| `d_child_head` | dir only | dir only | | | children hang off the **host**, never a shell |
| `d_sib` (into parent list) | ● | | ● | | *indexed* node's link in its parent's child list |
| `d_id` / `d_host` (union) | `d_id` | `d_id` | `d_host` | `d_host` | host: identity; shell: skip-to-host |
| `d_inode` | ● | ● | | | read on the host; shell forwards to it |
| `d_seq` (per-node build) | ● | ● | | | on the address-stable host; fold-invariant |

**Invariant count is what shrinks, not necessarily line count.** The baseline
requires the reader to reason about even/odd seqcount phases on *every* dentry
and a global generation bracket; the port requires one predicate
(`d_fwd==NULL`) and a write-once discipline. The kind axes are read, not stored.

## 3. Redundant-but-kept fields (each a zero-cost denormalization)

Two fields carry information derivable from elsewhere. Both are kept
deliberately; neither is accidental duplication.

**`d_host` is a materialized shortcut of the `d_fwd` chain.** Its value is what
you get by walking `d_fwd` to `NULL` (exactly what the retired `chain_host_rcu`
did). Kept because materializing it buys three things at no cost: (1) O(1) host
resolution instead of O(depth) — the readdir churn fix in §S3; (2) hazard
avoidance — walking `d_fwd` under concurrent folds dereferences transient shells
that may be splicing/freeing, whereas `d_host` is write-once to the
fold-invariant tail and reads with a plain `rcu_dereference`; (3) zero space —
it is unioned onto `d_id`. Note the two fields *coincide* at chain depth 1
(`top->d_fwd == top->d_host == host`) and diverge only at depth ≥2, which is the
churn regime the skip pointer exists for.

**`d_id` is redundant with the host's (invariant) address**, since children key
on the host address and a live object's host never moves or is reallocated.
Kept because: (1) removing it saves no space (the union slot exists for the
shell's `d_host` regardless; a host would just carry an unused slot); (2) it
models the *inode* identity, deliberately distinct from the dentry's cache-slot
address — a dentry can be evicted and a fresh one allocated for the same file,
and the identity must survive that. Returning the address-as-identity is a
shortcut valid *only* under this experiment's "no realloc for a live object"
invariant, and it would break the moment reclaim/refill or negative-dentry
recycling is modeled (phases 2–3). Keeping `d_id` avoids baking that invariant
into the identity contract.

## 4. Sub-classing (dir / file / shell) — considered and declined

Splitting `struct dentry` into subclasses was evaluated and rejected for the
RCU-core scope:

- **shell vs. host** cannot be cleanly split: the chain is walked
  polymorphically and the kind is discriminated at runtime by `d_fwd==NULL`, so
  `d_fwd`/`d_back`/`d_spliced` must share offsets across both. The shell's only
  unique field (`d_host`) is already free via the union. A split saves zero
  bytes and removes the no-downcast property that lets `find_top_rcu` /
  `host_of_rcu` / `fold` treat settled hosts and mid-transition shells
  uniformly.
- **dir vs. file** is feasible (only `d_child_head` is dir-specific, and kind is
  stable per node) but is a *scope expansion*: the engine currently has no
  dir/file distinction, so a split would have to introduce a `d_kind` + downcast
  + `ENOTDIR` enforcement just to know when a child head exists. It moves toward
  faithful VFS typing, not toward simplification. ~8 bytes saved on leaves (~5%
  of a name-dominated struct) does not pay for the added type surface.
- **Cheap alternative if clarity is the goal:** a stable `d_kind` enum set once
  at allocation (safe — no node ever changes kind) used only for `assert`s and
  documentation, *without* splitting the layout. It preserves the
  polymorphic-chain design and turns the field-liveness table above into
  checkable predicates. Optional; its only new information over `d_fwd==NULL` is
  the dir/file axis the engine otherwise ignores.

The conclusion reinforces the thesis: the simplification win is **one uniform
node discriminated at runtime**. A class hierarchy would re-introduce the static
type structure the port dissolves.

## 5. Reader fast-path cache-line locality

`pahole` on the built engine (176 bytes, 3 cachelines):

```
d_hash        0..15    ┐ CL0 [0..63]  -- match (bucket walk + inline identity)
d_iparent    16..23    ├
d_iname      24..71    ┘ (spills 8B into CL1)
d_fwd        72..79    ┐ CL1 [64..127] -- reader loads this line ONLY for d_fwd
d_back .. d_sib 80..135┘ (else fold/writer/readdir-only; cold for a lookup)
d_id/d_host 136..143   ┐ CL2 [128..191] -- payload (id + inode + gen)
d_inode/d_seq/d_rcu    ┘
```

A **settled lookup touches 3 cachelines per hop**: CL0 (match), CL1 (loaded
purely for the `d_fwd` host/shell discriminator — `NULL` on every settled host,
while the rest of the line is fold/writer/readdir-only), CL2 (the `d_id`/
`d_inode`/`d_seq` payload). `d_seq` shares CL2 with `d_id`, so the per-node gen
adds no extra reader line over the global build; the two genuinely-wasted lines
are CL1 (discriminator) and CL2 (payload).

**Proposed fix — a pure field reorder (no semantic change):** cluster the
reader-hot set `{d_hash, d_iparent, d_iname, d_fwd, d_id/d_host, d_inode,
d_seq}` and exile `{d_back, d_spliced, d_parent, d_dc, d_child_head, d_sib,
d_rcu}` to the tail. A settled hop then touches **2 lines** (CL0 match + a
single payload-plus-discriminator line). The 48-byte inline `d_iname` makes the
match alone 72 bytes, so 2 lines is the floor without an out-of-line name
(rejected earlier for match-compare cost). Safety: `d_fwd`/`d_id`/`d_inode` are
write-once (no new false sharing); `d_seq` already co-resides with `d_id` today.
The only trade is that `d_fwd` leaves the `d_fwd`/`d_back`/`d_spliced` cluster,
so a fold touches 2 lines for the chain instead of 1 — the GP-bound slow path
paying so the read-mostly path wins. Also: the **global build's `d_seq` is dead
weight** (only the per-node reader samples it) and can be `#ifdef`-ed out,
closing the current 4-byte hole.

### Path to a 1-cacheline hot path

The reorder gets a settled hop to 2 lines; reaching **1 line** (64 B) is a
separate, harder target, because every byte placed on CL0 is bought by evicting
inline-name bytes. Working the budget requires first pinning the exact per-hop
read-set (verified against `dc_lookup`):

- **global build:** `d_iparent`, `d_iname`, `d_fwd` (discriminator), `d_id`,
  `d_inode`. No generation, no mark.
- **per-node build:** the above **plus** `d_hash.next` (the still-indexed mark,
  read by `top_unhashed_rcu`, line 507) **plus** `d_seq` (sampled every hop,
  re-read on the up-pass).

Densest encoding of each non-name field, and the cost of densifying it:

| field | naive | densest | mechanism | cost |
|---|--:|--:|---|---|
| parent `d_iparent` | 8 | 8 | full ptr; tags in low bits 1–2 (bit 0 stays the txn proxy) | one mask-AND per compare |
| host/shell `d_fwd` | 8 | 0 | tag bit in `d_iparent` | fold TRANSFER must re-set the bit when it copies identity into the host |
| pos/neg `d_inode` | 4 | 0 | tag bit in `d_iparent` | subsumes the flag; in a real port keeps the inode *pointer* off the hot line |
| identity `d_id` | 8 | 0 | return the dentry **address** | fast-path id = pointer, not logical id (VFS-faithful; the bench reads `d_id` cold) |
| mark `d_hash.next` | 8 | 8 pernode / 0 global | split node: `next` hot, `pprev` cold | the hlist API change — justified only because per-node reads `next` every hop |
| gen `d_seq` | 8 | 8 pernode / 0 global | — | irreducible while transacted (MCAS slots are pointer-width) |

Non-name hot subtotal, and the name budget it leaves (64 − subtotal):

- **global:** `d_iparent`(8) + `d_id`(8) = **16** → **48 B for the name** = the
  full current buffer. Drop `d_id` too (8) → 56 B, room to spare. **The global
  build reaches 1 CL with no name reduction**, and needs no hlist surgery
  (`next` is read only on collision walks there, so `pprev` can stay put).
- **per-node:** `d_iparent`(8) + `d_hash.next`(8) + `d_seq`(8) = **24** → **40 B
  for the name** = hash(4)+len(4)+**32-char buffer** (down from 40). **Per-node
  reaches 1 CL only by trimming the inline name to 32 chars**, unless one of its
  two extra hot reads is eliminated (fold the mark into the gen check, or find a
  cheaper gen — both causality-path redesigns, not layout tweaks).

So the tension is inherent and quantified: **the inline name is free in the
global build and costs 8 bytes (40→32 chars) in the per-node build**, because
per-node pays for whole-walk causality with two extra 8-byte hot reads (`d_seq`
+ the mark) that the global build never makes. Every causality byte is a name
byte. `d_seq` is the irreducible one; the mark is the candidate for reclamation
if the still-indexed check can be folded into the generation sample.

### First cut: fair-weather 1-CL in isolation (`-DDC_HOT1CL`, 2026-07-16)

The global-build 1-CL layout was prototyped behind `-DDC_HOT1CL`: `d_iparent`
low-bit tags (bit 1 host/shell, bit 2 pos/neg; bit 0 stays the txn proxy) remove
the `d_fwd` and `d_inode` reads, and the `d_id`/`d_host` union is hoisted onto
CL0 next to `d_iname`. `pahole` confirms the hot cluster
(`d_iparent`+`d_iname`+union) fills exactly `[0..63]`. Correctness holds: 103/103
single-thread (plain + ASan), and the concurrent ASan stress is
conservation-clean with zero wrong-id reads — validating that the fold TRANSFER
preserves each node's host/shell bit while adopting the moved identity.

**Throughput A/B (pure lookups, `rename-frac 0`, interleaved medians vs the
stock global build):**

| regime | config | ratio (hot1cl / stock) |
|---|---|---|
| cache-resident, load factor <1 | 1–184 threads, default geometry | **0.98–1.01** (neutral) |
| cache-cold, load factor <1 | 48 thr, ~295K leaves, buckets scaled | **0.96** |
| high load factor (~24) | 48 thr, ~98K leaves in 4096 buckets | **0.79–0.92** (worse) |

**It never wins, and loses under bucket collisions.** Two reasons the premise
(CL count is the fast-path bottleneck) fails on this box:

1. **The descent is latency-bound, not line-bound.** Each hop is a dependent
   load chain (bucket → node → next bucket); the extra node lines are fetched
   while the chain stalls, so removing them frees no critical-path time. The
   hardware adjacent-line prefetcher makes the stock layout's *consecutive*
   CL0/CL1/CL2 access nearly free — consolidating to one line saves nothing and
   even forfeits the streaming-prefetch the stock layout gets.
2. **Freeing CL0 by moving `d_hash` cold penalizes collision walks.** With the
   fixed 4096-bucket table, a large namespace runs at load factor ~24; each
   collision step then reads the match on CL0 *and* `d_hash.next` on CL2 —
   double the stock layout's single CL0 touch. Real directories collide, so this
   is not a corner case.

This first cut measures the dcache **monopolizing the cache** — the one
condition that never holds in production. That framing understates a fat
dentry, and the collision penalty is an artifact of moving `d_hash` cold. Both
are fixed below.

### True 1-CL + the footprint case (`-DDC_HOT1CL_SPLIT`, 2026-07-16)

The collision penalty is removed by **straddling** the 16-byte `d_hash` node at
offset 56 — `next`@56 lands on CL0 (so collision walks stay on the hot line),
`pprev`@64 spills cold — with *no* hlist API change. The 8 bytes that buys come
from **eliding `d_id`**: the reader returns the host **address**, a stable
write-once identity here and the VFS-faithful `__d_lookup` shape (the seqcount
brackets the *walk*, not the payload, so the elision is orthogonal to ordering —
`g0`/walk/`rmb`/`g1` is unchanged). Validation: with the `d_id` read retained
(`-DDC_SPLIT_KEEPID`) the straddle layout is 103/103 + ASan-stress-clean; the
elision itself is a sound return-value change. `pahole`:
`d_iparent`+`d_iname`+`d_hash.next` fill `[0..63]`, collision-safe.

Isolated single-thread throughput is still ~neutral (0.96–1.04) — **but the
hardware counters show the layout doing exactly its job.** `perf stat`,
DRAM-bound single-thread, ~11M lookups, identical throughput:

| per lookup | stock | split | Δ |
|---|--:|--:|--:|
| L1-dcache-load-misses | 6.02 | 3.22 | **−47%** |
| LLC (cache) misses | 4.48 | 2.84 | **−37%** |
| cache-references | 171M | 110M | **−36%** |
| instructions | 17.37B | 17.57B | +1.1% (the tag ANDs) |

`perf record` agrees (total cache-miss events 30.5M → 20.4M, −33%, the drop in
the lookup path). So split provably **halves the L1 miss traffic and cuts LLC
misses by a third** — latency-bound, so on a lone core the misses hide behind
dependent-load stalls and the prefetcher.

**But that "invisible" caveat was itself a harness artifact.** The residual in
the `perf record` above — and the reason isolated throughput looked neutral —
was `mk_leaf_path` `snprintf`ing a path *per lookup* (a cost a real VFS does not
pay: the caller already holds the component qstrs). That path work is ~1166
instr/lookup of pure ALU, ~74% of all instructions, and it runs *concurrently*
with the descent's memory stalls — perfectly hiding the footprint delta. The
`--precomp` knob precomputes the component qstrs once (applied identically to
both engines — same mechanism), stripping the snprintf. With it gone the descent
is latency-bound and the footprint reduction converts straight to throughput:

| `--precomp`, median-of-5 | stock | split | ratio |
|---|--:|--:|--:|
| 1 thr, DRAM (262 K leaves) | 8.75 | 9.74 | **1.113** |
| 8 thr, DRAM (65 K/thr) | 45.13 | 47.00 | 1.042 |
| 8 thr, L3-resident (8 K/thr) | 115.4 | 117.7 | 1.020 |

instr/lookup drops 410 → 397; L1-dmiss 4.50 → 3.48 (−23%); LLC-miss 6.22 → 4.48
(−28%). The gradient *is* the mechanism check: largest single-thread DRAM-bound
(+11%), shrinking at 8 threads (+4% — the shared precomp qstr table becomes a
bandwidth co-footprint that dilutes the node-footprint delta) and smallest
L3-resident (+2% — footprint barely matters when the tree fits in cache). Fewer
node lines → fewer misses → faster latency-bound descent, as predicted.

And it is decisive exactly where a monopolizing microbenchmark can't see it:

- **Shared L3 / bandwidth (multi-core).** 8 threads on one CCD (32 MB L3),
  aggregate leaf footprint crossing L3: the halved miss stream becomes
  throughput once cores contend memory.
- **Shared cache with the caller (co-tenant, `--pollute`).** A dcache never owns
  the cache in production; streaming an "application" buffer between lookups,
  split's relative standing rises with pressure. Modest here — the co-tenant's
  own misses dominate and are equal for both — but it is footprint the
  application does *not* lose to the dentry.

**Verdict: the 1-CL split layout is a keeper — on footprint, and (once the
harness `snprintf` artifact is removed) on single-thread throughput too.** With
the path-construction cost stripped so both engines run the same mechanism, split
is **+11%** single-thread DRAM-bound and stays ahead multi-core; halving the
dentry's per-lookup line traffic is additionally a system-level win (less L3
pressure on co-tenants, less bandwidth under multi-core) that a
dcache-owns-the-cache microbenchmark structurally understates. The cost is the
`d_iparent` tag discipline (one mask-AND per compare + a fold fixup) and
address-as-identity (VFS-faithful; harnesses that assert logical ids build with
`-DDC_SPLIT_KEEPID`). Kept behind `-DDC_HOT1CL_SPLIT`; promoting it to default
awaits migrating the harness id-checks to address identity.

### Landed as the default — true 1-CL for *both* readers (2026-07-16)

The split above got the **global** reader to 1 CL, but the **per-node** reader
was still 2-CL: `pahole` put `d_seq`@152 on CL2 sharing the line with the (now
cold) `d_id`, and the per-node walk samples `d_seq` every hop. Eliding `d_id`
alone could not fix that — `d_seq` needed its own CL0 seat. The "Path to a 1-CL
hot path" budget above named the price exactly, and it was paid: **`DC_NAME_MAX`
40→32** (the kernel's `DNAME_INLINE_LEN`, so `qstr` 48→40 B) frees the 8 bytes
for `d_seq`@48. New CL0 = `d_iparent`(8)+`d_iname`(40)+`d_seq`(8)+
`d_hash.next`(8) = 64; struct 176→168. `pahole`-verified; both the global and
per-node readers now touch CL0 only. Address identity became the **default**
(harness id-checks migrated; `-DDC_SPLIT_KEEPID` reads the cold `d_id` for
logical-id suites; a weak `dc_lookup_id_is_address` capability lets the bench
check a seed-time address table instead). `DC_HOT1CL_SPLIT` is now default-on
(`-DDC_NO_HOT1CL_SPLIT` restores the legacy 3-CL struct). 103/103 global +
per-node, stress/xchg/dirs ASan+TSAN-clean, bench conserves under
renames+exchanges (engine `0f4f626`).

**Fairness — the reference gets the same layout.** A footprint A/B is only
mechanism-vs-mechanism if both sides carry equal cacheline quality. The seqlock
reference was still a 3-CL struct (`d_seq` scattered onto CL1, a separate
`d_inode`/`d_unhashed` pair), so it was brought up to the identical 1-CL hot line
— `{d_name, d_parent, d_seq, d_hash.next}` on CL0, `d_unhashed`/`d_inode` folded
into `d_parent`'s low bits, address identity, same `DC_HOT1CL_SPLIT` gate and
`DC_SPLIT_KEEPID`/capability plumbing (engine `706e459`; `make check` 103/103,
ASan-clean, bench conserves). Note the seqlock's single-thread read-heavy A/B is
a **wash** (split ~32.7 vs nosplit ~32.3 Mlookups/s): the 1-CL win is a
*cross-core scaling* property (fewer reader lines a concurrent writer's stores
can invalidate), not a lone-core one — consistent with the txn split's own
"invisible on an isolated core" footprint story. What the equal layout buys is
that §7's counter-axis comparison can be re-run with the *layout axis held fixed
and 1-CL on every arm*.

## 6. Two invariants the per-node counter rests on

The per-node reader (`-DDC_PER_NODE_GEN`) does something that, stated baldly,
sounds unsound: to validate the edge to a child it **samples a seqcount that
lives on the child, not on the parent that names it** (`host->d_seq`, sampled per
hop in `dc_lookup`). Per-node seqcounts on a tree are exactly where edge-identity
bugs live — the counter on a node tells you about *that node's* mutations, not
about whether its parent still points at it. What makes it sound is that the path
from a parent to the seqcount-bearing node is **two edges**, each pinned by a
distinct invariant, so the reader validates the edge *above* the counter without
ever re-reading the parent's pointer as a separate step:

```
P (parent) ──[index: P's bucket]──▶ top ──[resolution: d_host skip]──▶ host  (d_seq)
              edge ①  (dynamic)              edge ②  (static)
```

**Invariant A — `(d_iparent, d_iname)` is immutable per node.** A rename never
re-keys a live node; it *mints a fresh shell* carrying the new (parent, name) and
demotes the old top. So a node never migrates hash buckets: its bucket is fixed
at birth to `hash(d_iparent, d_iname)`. This is what makes `top_unhashed_rcu(top)`
a *faithful* test of edge ① — "top is still hashed" is equivalent to "P still
indexes `name → top`," not the weaker "top is hashed somewhere." A demote/unlink/
fold-transfer marks `top->d_hash.next` in the **same MCAS commit** that bumps
`host->d_seq` (`stack_one_prepare` + `txn_bump_gen`), so the reader's `rmb`
between sample and confirm ties the index-membership check to the counter.

**Invariant B — `d_host` is write-once.** The resolution edge ② (`top → host`)
is set once when the shell is minted and never rewritten. A fold *splice* mutates
intermediate `d_fwd`/`d_back` and a *transfer* demotes the top, but neither ever
touches any node's `d_host`, and both preserve the tail. So `host_of_rcu(top)`
returns the same address for the life of `top`; an immutable edge needs no
runtime re-validation, which is why there is no explicit "top still points at
host" check. (In the settled case `top == host`, edge ② collapses to identity and
edge ①'s `top_unhashed` reads the host's own mark.)

With edge ① validated dynamically and edge ② validated by construction, the
`host->d_seq` window is left to catch only the third thing — `host` being renamed
to a *new* namespace position, its own counter bump. **Both invariants are
load-bearing:** were `(d_iparent, d_iname)` mutable, `top_unhashed` could report a
node hashed under a *different* key; were `d_host` mutable, a reader could resolve
`host = X`, sample X's stable `d_seq`, and miss `top` being re-pointed to forward
to a different host. Sampling the child's counter is sound *because* these two
edges above it cannot silently change identity.

## 7. The simplification's payoff — S3 scaling curves

The point of dissolving `d_seq` and demoting `rename_lock` is not fewer lines;
it is that the reader path stops sharing a whole-tree cacheline. §7.1–7.2 fold in
the S3 sweeps that measure it (full data + method in `rename-shell-transition.md`
§S3 / readdir; 2×96-core EPYC 9654, threads pinned one-per-physical-core via an
`hwloc-calc core:all.pu:0 → --cpulist`, best-of-5, every run gated on namespace
conservation — **0 failures across 141 rows**). Figures `figures/dcache_s3.png`
and `figures/dcache_readdir.png` (regenerate with `scripts/run_dcache.sh` →
`scripts/plot_dcache*.py`); the tables below are the committed anchors.

**Measured on the current default** — the true-1-CL split (§5 "Landed",
`0f4f626`/`706e459`) with `--precomp` on, both applied identically to all three
arms, so the layout axis is held fixed at 1-CL and the sweep isolates the
*counter* axis (global vs per-node vs seqlock). Two notes for anyone diffing
against the earlier 3-CL / no-precomp tables (this is a resweep of them): (1)
absolute lookup throughput is **~4× higher** here, almost entirely because
`--precomp` is now the default — it strips the per-lookup `snprintf` (~74% of
instructions, §5); readdir, whose cost is child enumeration not path
construction, barely moves. (2) With that ALU work gone the descent is
latency/bandwidth-bound, so the reader-path differences the `snprintf` used to
mask now appear at full size — the per-node lead **widens** vs the old tables
rather than being an artifact of the change. Machine was quiet at sweep time
(load ~0.3, the co-tenant qemu VM idle); best-of-5 absorbs residual jitter.

### 7.1 Path-lookup reader scaling

The homogeneous rename-fraction mix is the *wrong* instrument: a rename is ≈50×
a lookup, so throughput is writer-bound and the reader-generation difference is
masked (per-node only ~25% ahead at the high-frac end). The **role-split** mode
(dedicated readers + writers) isolates the reader path, and the localization
appears. Two axes, all figures Mops/s:

*8 writers, sweep readers toward the full machine (reported anchors, Mops/s):*

| readers | seqlock | txn-global | txn-per-node |
|---|--:|--:|--:|
| 2 (low end) | 21 | 25 | 58 |
| 32 | 100 | 156 | 637 |
| 160 (per-node peak) | 69 | 198 | **2011** |
| 184 (all 192 cores) | 61 | 262 | 1965 |
| ratio @184 | 32× | 7.5× | **1×** |

*32 readers, sweep writers (per-node lead over global, Mops/s):*

| writers | txn-global | txn-per-node | lead |
|---|--:|--:|--:|
| 1 | 357 | 764 | 2.1× |
| 4 | 119 | 693 | 5.8× |
| 1→24 (range) | | | **2.1–6.0×** |

**Reading.** The per-node host counter — read only by walks that pass through the
*moved* entry (§6) — has no shared ceiling, so reader throughput keeps climbing to
**~2011 Mops/s** @160 readers (easing to 1965 @184 as the 8 writers share the last
socket). The global `rename_gen`, read by every walk and written by every rename,
is one contended cacheline and **plateaus around ~200–260** (noisy, no clean
saturation but a firm ceiling ~8× below per-node); seqlock never scales cleanly
(reader-retry storms under the fixed rename load — non-monotonic 60–100 past
~32 readers). At the full machine (184) per-node = **7.5× global, 32× seqlock**.
This is the split the port's headline hides: `d_seq` **dissolves outright** and is
independent of the counter choice, but the whole-walk-causality role of
`rename_lock` **dissolves only under the per-node arm** — the global counter
reimports exactly the contention it was meant to remove. The scaling win is a
property of *where the counter lives*, which §6's two invariants are what license.

### 7.2 Directory-listing (`readdir`) scaling

`readdir` is a reader fast path in its own right (an in-memory dcache lists
straight from the child index). The seqlock baseline is upgraded to the honest
kernel analogue — a **per-directory rwsem** (`iterate_dir` under the inode rwsem),
not a global lock — so different dirs and concurrent readers of one dir don't
serialize on paper. Readers enumerate a random dir (~32 children) while writers
rename; namespace owned by writers, so dir size is fixed as readers scale.
Figures listings/s:

*Reader scaling, 8 writers, sweep readers (listings/s):*

| readers | per-dir rwsem | txn-global | txn-per-node |
|---|--:|--:|--:|
| 32 | 17 | 92 | 113 |
| 160 | ~18 (saturated) | 293 | 364 |
| 184 | 18 | 317 | **400** |
| ratio @184 vs rwsem | 1× | 18× | **~23×** |

*Writer load, 32 readers, sweep writers (namespace fixed, listings/s):*

| writers | rwsem | txn-global | txn-per-node | per-node lead |
|---|--:|--:|--:|--:|
| 1 (light) | 4.0 | 72 | 78 | **~19×** |
| 48 (saturating) | 13.5 | 56 | 62 | **~4.6×** |

**Reading.** `readdir`'s reader path reads **no generation counter at all** — the
dir resolve is a bare `txn_child_lookup_rcu` walk (no `rename_gen`, no `d_seq`, no
cursor) and the listing is an `rcu_read_lock`-only child-hlist walk — so the two
txn arms run **identical reader code**. Yet they no longer perfectly overlap:
per-node leads global by ~15–25% at scale. That gap is **not** the reader path; it
is a second-order *writer-side* effect. In the global build the 8 concurrent
writers all bump the single `rename_gen` cacheline, which throttles the writers
themselves (per-node sustains a higher rename rate — 0.24 vs 0.21 Mrn/s @160rd)
and, via coherence-bus pressure, indirectly the gen-free readers sharing the
machine. So even where the reader path is identical, the global counter's writer
contention leaves a visible mark — a smaller echo of the lookup-path story; the
old 3-CL/no-precomp sweep, at ~4× lower throughput, had it below the noise floor.
Both txn arms crush the rwsem (saturates ~18 — its read-side is a shared cacheline
bouncing among readers of one dir); the lock-free walk **leads at every reader
count**, even two. Crucially the txn walk pays no per-child chain walk: the
write-once **`d_host` skip pointer overlaid on `d_id`** (§3) resolves each child's
content host in O(1), which erased the earlier low-reader crossover and the
churn-sensitivity — under saturating write load it still leads ~4.6× and its
residual decline is *write-side* MCAS churn, not the reader walk. So the same
union that costs one branch on the lookup fast path (§2) makes listing O(1) in
chain depth.
