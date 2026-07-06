# FT allocator scaling: keeping the node arenas ahead of multi-writer txn updates

How to scale the fractal-trie (FT) internal-node allocator once the txn
integration (`ft-txn-integ`) makes updates themselves scale — without breaking
the compact API, whose economics turn the allocator into a compacting GC.
Companion to [rcu-txn-use-cases](rcu-txn-use-cases.md) and
[rcu-txn-bitmap](rcu-txn-bitmap.md). **Design discussion, not prototyped.**
Code references are against `userspace-rcu` branch `ft-txn-integ`
(commit `3755d42e`). 2026-07-05.

## 1. The problem: one mutex per (group, kind, order), on both sides of every update

Every internal-node, compressed-node and ordinal-cell allocation funnels
through a single `pthread_mutex_t` per (group, kind, order) arena
(`fractal-trie-alloc.c:124`), taken on **every** item alloc
(`cds_ft_arena_alloc`, `fractal-trie-alloc.c:1047`) and **every** free
(`cds_ft_do_free_item`, `fractal-trie-alloc.c:1349`). Three amplifiers make
this the first wall the multi-writer work will hit:

1. **Recompact-on-insert is the multi-writer-safe default**
   (`fractal-trie-internal.h:512`): every new-occupancy insert is a whole-node
   alloc-and-copy plus a deferred free of the retired node. Allocation is on
   the critical path of essentially *every* update, not just node growth. A
   typical insert costs ~2–4 arena lock acquisitions (node, maybe compressed,
   maybe cell) and the same again on the free side after the grace period.
2. **Frees execute on call_rcu workers** (concurrent-mode frees route through
   `cds_ft_free_item` → `call_rcu`, `fractal-trie-alloc.c:1516-1529`), so the
   mutex is already contended writer-vs-reclaim-workers under a *single*
   writer — and both sides scale with writer count.
3. Two more per-update heap hits ride along: the external (leaf) arena's
   single buddy mutex (`fractal-trie-alloc.c:1676`), hit once per insert; and
   `ft_flip_txn_create_bounded` = raw `malloc`/`free` per update op
   (`ft-mutation-helpers.h:134,177`).

A workload dominated by one node size drives all writers and all reclaim
workers through **one** mutex. That saturates at a few M lock handoffs/s —
an order of magnitude below where the hlist/txn bench results (README,
`figures/`) say the trie itself can go.

## 2. Constraints: this allocator is not replaceable, only frontable

The allocator is a locality engine with hard structural invariants; scaling
work must sit *in front of* it, not swap it out:

- **Address-arithmetic layout.** Item ↔ metadata ↔ range resolution is pure
  pointer masking (`cds_ft_item_to_range`, `fractal-trie-internal.h:1698`;
  the bitmap reverse-array). Items *must* live in strided ranges — no foreign
  allocator can back nodes.
- **Range-granular accounting.** `nr_live` / `next_unused` / the per-range
  LIFO freelist (`struct cds_ft_alloc_range`,
  `fractal-trie-internal.h:1618-1653`) drive O(1) drained-range reclaim: a
  fully-bumped range whose `nr_live` hits 0 has its node-body page(s)
  `MADV_DONTNEED`'d and is parked for recycling
  (`ft_arena_reclaim_range`, `fractal-trie-alloc.c:719`;
  `range_recycle`, `:568`).
- **The compactor's contract.** `cds_ft_compact` relocates every live node
  into dense private ranges (per-thread ctx, `fractal-trie-alloc.c:931-994`)
  so the *old* ranges drain and self-reclaim (`ft-compact.h:18-42`). The
  "already relocated this pass" test is the per-range `recompact_private`
  flag; frees into private ranges are special-cased
  (`fractal-trie-alloc.c:1352-1371`). The whole pass assumes range accounting
  sees every free promptly.
- **The reserve API** (`cds_ft_alloc_reserve_add`,
  `fractal-trie-alloc.c:1417`) already pre-draws items so bulk commits are
  infallible — an existing, working precedent for alloc-side caching (§4).

## 3. Why a naive slab / per-CPU magazine breaks

The obvious move — per-CPU magazines of freed slots, à la umem/jemalloc —
fails against §2 in four distinct ways:

1. **Reclaim/recycle aliasing (the killer).** A magazine of *freed* slots must
   pick one of two poisons. (a) Decrement `nr_live` at free time: a range can
   then drain, be `MADV_DONTNEED`'d, parked, recycled and re-bumped from
   `next_unused = 0` **while the magazine still holds pointers into it** — two
   owners per slot. (b) Defer the decrement to magazine flush: cached slots
   become invisible-live, the range never drains, and reclaim stalls
   unboundedly on whatever the magazines happen to hold.
2. **The compactor GC is defeated silently.** Same invisible-live problem,
   compounded: a compaction pass relocates the live set precisely so old
   ranges drain. Magazine-held slots pin those ranges against the drain, so
   the pass pays full relocation cost and recovers nothing.
3. **`recompact_private` classification.** No allocation source may hand a
   non-compactor thread a slot inside a private range (pass idempotence
   depends on it). A magazine that captured a free from a pre-merge private
   range violates this; the current code prevents it only because private
   frees bypass the shared lists.
4. **Density.** The MRU partial-range discipline
   (`fractal-trie-alloc.c:1397-1400`) hands the next allocation the hottest
   just-freed slot. Per-CPU caches of freed slots would hand writers slots
   scattered across the arena, trading the locality the design measures for.

## 4. The load-bearing asymmetry: alloc-side caching is safe, free-side is not

A slot that is **allocated-but-unused** is `nr_live`-accounted: its range
cannot reclaim under it, the compactor never encounters it (it walks the
trie), and the only cost is bounded range-pinning. The reserve API already
exploits exactly this. A slot that is **freed-but-cached** is what breaks §3.

Corollary: the **range — not the CPU — is the natural free-side shard**, since
every free-side invariant (`nr_live`, freelist, drain, private classification)
is already per-range. This asymmetry shapes every approach below — including
§9, whose invisible-live rule works precisely by reclassifying a cached freed
slot as allocated-but-unused, i.e. moving it to the safe side of this
asymmetry.

## 5. Approach A — per-CPU frontier ranges (alloc side)

Each CPU (rseq `cpu_id`, or TLS per writer thread) owns the active bump range
per (kind, order). The arena lock is taken only to acquire or retire a range;
slot handout within the frontier is single-owner and lock-free, and
`next_unused` needs no atomics. `nr_live` becomes `uatomic` (concurrent frees
decrement it). Lock frequency drops from per-item to per-range: ×64 for 64 B
items on 4 KiB pages, ×32k for `FT_FAR_METADATA` 2 MiB ranges.

Why it is safe: frontier ranges are *ordinary* (non-private) ranges; a
frontier is by construction not fully bumped, so it is never reclaim-eligible
while owned — the same property today's head-of-`arena->ranges` frontier has.
Compactor semantics are untouched.

Wrinkles:

- **Large orders.** Near-metadata ranges hold `page_size >> order` items — 1–2
  items for big nodes — so the amortization unit must move up to per-CPU
  *superblock* carving (`FT_SUPERBLOCK_SIZE`, `fractal-trie-alloc.c:72`), with
  the lock taken per superblock mmap only.
- **NUMA.** Per-CPU ownership must *not* mean first-touch-local placement:
  published nodes are read globally, so the per-2 MiB interleave
  (`ft_apply_interleave`) stays applied per range regardless of whose frontier
  it is.
- **Retirement.** A partially-bumped frontier returns to the arena as an
  ordinary partial range on thread exit / CPU migration / mode change.
- **RSS floor.** nr_cpus × nr_orders × page-unit of partially-filled
  frontiers. Negligible for near-metadata; for far-metadata + THP it is
  2 MiB × classes × CPUs — wants a fault-limited or lazily-promoted policy.
- **Reserve fill** (`cds_ft_alloc_reserve_add`) becomes cheap automatically —
  it is just N frontier bumps.

## 6. Approach B — two-GP batched frees (kfree_rcu discipline)

Frees already arrive in grace-period batches; exploit that instead of fighting
it. `cds_ft_free_item` pushes the `metadata_alloc` onto a per-CPU gather list
and enqueues **one** `rcu_head` per batch. The callback detaches the batch and
**re-arms itself for one more GP** before applying.

The second GP is mandatory, and is the subtle point: items pushed onto the
gather list *after* its `rcu_head` was enqueued have not had a full grace
period when that head's callback fires (the GP is measured from head enqueue,
not from item push). Detach-then-wait-one-more-GP gives every item ≥ one full
GP after its push — the same discipline as the kernel's `kfree_rcu` page
batching.

The apply step groups the batch by range and takes each arena lock **once per
range per batch**. Slots are never cached across batches and `nr_live`
decrements at apply time, so *zero* §2 invariants change; MRU locality
degrades gracefully from slot-granular to batch-granular. Expected 10–100×
reduction in free-side lock acquisitions — on the side that cannot be sharded
by CPU. Composes with §5. Cheapest first step.

Cost: reclaim latency grows by one GP, and the gather list is a new per-CPU
structure the exclusive-mode synchronous path must bypass (it already
bypasses call_rcu).

## 7. Approach C — per-range lock-free freelist push, if §5+§6 leave a residue

Make the free fast path: atomic LIFO push on `range->free_list_head` (CAS) +
atomic `nr_live--`, with the arena lock only for state transitions — first-free
registration onto `partial_ranges` (replacing the per-free MRU move, which is
a locality heuristic, not correctness; a per-CPU "last-freed-into range" hint
recovers most of it) and the drain-seal-reclaim transition.

The hard part is **pop vs reclaim**: a lock-free pop racing a drain check can
resurrect a slot in a range being sealed. Two escape hatches, in order of
preference:

1. **Asymmetric locking.** Keep *pop* under the arena lock — once frontiers
   (§5) exist, freelist reuse is a rare slow path — and make only *push*
   lock-free. Push is the side that scales with reclaim workers.
2. **Seal + generation.** A seal state folded into `nr_live`
   (fetch-add-unless-sealed) plus a range generation counter. The layout is
   already friendly: the header+metadata page **stays mapped across reclaim**
   (only the node-body is `MADV_DONTNEED`'d, `fractal-trie-alloc.c:727-738`),
   so a stale reference to a recycled range's header is memory-safe and
   detectable by generation.

## 8. Approach D — log-structured mode: lean into the compactor (architectural)

FT already *has* a compacting GC, so the endgame shape is the log-structured
one: in multi-writer mode, alloc = per-CPU bump only (§5), free = atomic
`nr_live--`, **no per-slot freelist reuse at all**. Drained ranges self-reclaim
as today; sparse-but-live ranges are recovered by compaction, triggered by a
per-arena occupancy ratio (live × item_len / range bytes). The resumable
`cds_ft_compact_begin/step/end` API and the private-range machinery
(`ft-compact.h:429-585`) already exist to run this incrementally. Free-side
contention disappears entirely (one atomic decrement into the range header).

Costs, stated honestly: churn grows RSS between passes (garbage accumulates at
churn rate × trigger period), and a pass costs O(live), not O(garbage) — the
mark-compact cost model. And compaction currently requires writer exclusion,
which in a multi-writer world is a stop-the-writers event.

The convergence worth designing toward: the §4.B freeze-on-free tombstone +
flip-txn commit — the relocated node's forward edge and the old copy's
tombstone in **one atomic commit** (`ft-compact.h:63-110`) — is exactly the
primitive that makes relocation safe against *concurrent* writers: a writer's
MCAS against a relocated-away node fails on expected-value/tombstone and
retries. Concurrent incremental compaction via txn is where "allocator
scaling" and "compactor GC" stop being in tension. Mode-gate it: exclusive /
single-writer tries keep today's freelist + MRU reuse (measured locality
value); multi-writer groups run bump-only + occupancy-triggered compaction.

## 9. Approach E — compact-integrated per-CPU magazines (unified alternative to A+B+C)

Proposed alternative: a per-CPU magazine per arena — per (group, kind,
power-of-two order); the natural home is a per-CPU array hanging off
`struct cds_ft_alloc_arena` — holding range-resident slots, with one new
contract: **the magazines drain when the compact API runs**, so cached slots
never pin ranges against the very pass whose purpose is to drain them. Where
§5–§7 avoid free-side caching because of §3, this approach *repairs* §3
directly. Five rules make it sound:

1. **Invisible-live invariant.** A cached slot keeps its range's `nr_live`
   counted; the decrement happens only when the slot leaves the magazine into
   its range freelist (drain or spill), never at cache entry. Reclaim/recycle
   aliasing (§3.1) becomes structurally impossible: a range cannot drain, be
   `MADV_DONTNEED`'d or recycled while any magazine holds a pointer into it.
   (The other choice — decrement at entry — is unsound regardless of drain
   policy.)
2. **Fill post-GP only.** The magazine is fed from `cds_ft_do_free_item`
   (after the grace period, on the reclaim worker's CPU — per-CPU pinned
   workers keep this local) and by batched refill on allocation miss: N
   contiguous bump slots drawn under one arena lock take (density at batch
   granularity; the reserve API can draw through the same path). Cached slots
   are immediately re-allocatable. Pops re-zero the body + metadata exactly
   like the freelist-reuse path (`fractal-trie-alloc.c:1124-1131`).
3. **Fill-bypass while a pass is in flight.** From `compact_begin` to
   `compact_end`, the *fill* side is disabled: post-GP frees push straight to
   range freelists (today's path), so the relocated-away ranges drain as the
   pass's contract requires — without this, the compactor's own deferred
   frees would re-fill the magazines and the pass would recover nothing. The
   *pop* side stays enabled (consuming the magazine during a pass only
   helps). Private-range exclusion (§3.3) then holds for free: the private
   special case (`fractal-trie-alloc.c:1352`) runs before any cache logic,
   and private ranges only exist during a pass, exactly when fill is off and
   the magazines were just drained.
4. **Drain sweep at begin.** `compact_begin` sets the bypass flag, then
   pop-alls every CPU's magazine for each arena of the group, pushing slots
   to their range freelists batched per range under the arena lock. In-flight
   fills race the flag-set; close the straggler window with a second sweep at
   the first step boundary (under today's writer exclusion only reclaim
   workers race; under future concurrent compaction, writers too — same
   sweep). The pop-all must be atomic against concurrent push: plain
   atomic-xchg wfstack push/pop-all has this property, an rseq-push variant
   does not without quiescence — and a same-CPU uncontended CAS is nearly
   free, so plain atomics are the right primitive here.
5. **Recompact-ctx routing stays above the magazine.** The compactor's own
   allocations must come from private ranges (pass idempotence classifies by
   `recompact_private`); a magazine hit there would relocate nodes into
   ordinary ranges. Ordering in `cds_ft_arena_alloc`: recompact-ctx check
   first, magazine pop second, arena lock third. (The reserve check in
   `cds_ft_alloc_item_from` stays above everything, as today.)

What it buys over the layered A+B+C: **one mechanism, both sides**. Frees
become entirely lock-free (§6 still takes one lock per range per batch);
alloc misses amortize like §5's frontiers (batched refill); and per-CPU LIFO
reuse has arguably *better* temporal locality under multi-writer than today's
shared MRU — which hands the hottest just-freed slot to whichever CPU
allocates next, i.e. usually a different one. RSS stays flat under churn
(unlike §8 between passes). And it is the proven rcu-txn-slab shape — the
per-CPU size-classed magazines that tie percpu jemalloc on the list benches —
adapted to slots that must stay range-resident (§2 still bans owning the
memory: the magazine holds pointers into ranges, nothing more).

The honest costs:

- **Pinning between passes is bounded but real**: worst case nr_cpus × cap
  ranges pinned (every cached slot in a distinct otherwise-drained range).
  Irrelevant for near-metadata (4 KiB node-body pages); for far-metadata
  (2 MiB bodies) an adversarial 192 CPUs × cap 64 is tens of GiB, so either
  keep far-layout caps small or — better — add **pressure-triggered drain**:
  before mmap'ing a fresh superblock or range, sweep the magazines and retry.
  That makes the bound self-correcting without waiting for a compaction and
  gives the drain machinery a second, compaction-independent trigger.
- A three-way free path (private-range / magazine / range-freelist) and a
  cross-thread drain protocol whose flag/sweep ordering is a new — if small —
  proof obligation.
- A behavior change for single-writer mode too (per-CPU LIFO replaces shared
  MRU); if the MRU locality is measured value there, mode-gate the magazine
  to multi-writer groups.

If E works, §5 and §6 dissolve as separate mechanisms (E subsumes their
wins); §7 survives only as the spill/drain path's locking discipline; §8
remains the alternative philosophy (no reuse at all) with different
RSS-vs-compaction economics.

## 10. The two adjacent funnels

- **External (leaf) arena — the second slab cache, and the simpler one.**
  The per-CPU cache here is a *distinct* cache from §9's — one per group's
  external arena, sharded per (CPU, buddy order class) — not another
  instance of the same design, because every §9 hard case is absent by
  construction: external ranges are **pinned** — never `MADV_DONTNEED`'d,
  recycled or compacted, only `munmap`'d wholesale at destroy
  (`fractal-trie-alloc.c:2012`) — leaves are application-owned, the
  compactor never relocates them, and there is no `nr_live`/drain
  accounting to keep honest. So none of §9's five rules apply: no
  invisible-live bookkeeping, no fill-bypass, no drain-at-begin, no
  private-range routing.

  The clean shape: **intercept the free before the buddy sees it.** A
  magazine-cached block keeps its `cells[]` entry as "allocated @ order k"
  — that cell is immutable while the block is allocated
  (`fractal-trie-alloc.c:1958`) — so magazine reuse is metadata-free: pop +
  `memset`, no arena lock, skipping the alloc path's freelist scan + split
  loop and the free path's merge loop entirely. Spill/flush = real
  `cds_ft_external_arena_free` calls on watermark (merging stays global,
  under the existing lock, off the fast path). The fill flow matches §9
  rule 2: leaves are freed post-GP (call_rcu workers, per-CPU pinned)
  while allocs run on writer CPUs — same plain-atomic wfstack
  push / pop primitive.

  Honest costs, both bounded by the cap: cached blocks stall buddy
  coalescing (the merge guard requires the buddy *free*, so an
  allocated-looking cached block blocks its whole merge chain) and pin
  their order class against redistribution to other sizes — a shifting
  size profile leans on the watermark flush. With 19 classes
  (16 B–4 MiB, `FT_EXT_ARENA_{MIN,MAX}_ORDER`), caps must be byte-, not
  slot-denominated — a few cached 4 MiB blocks per CPU is real RSS —
  or caching disabled above a threshold order (the far-metadata question
  again). Destroy discards magazine contents (the ranges die wholesale).

  Alternative: port the rcu-txn-slab per-CPU superblock design outright
  (ties percpu jemalloc on the list benches) — unlike the node arenas
  (§2), the external arena *is* replaceable, since `ft_ext_arena_range_of`
  masking and the guard-tail over-read property
  (`FT_EXT_ARENA_GUARD_SIZE`) are implementation-internal — but fronting
  the existing buddy is less new code for the same lock-free fast path.
  This lock is hit once per insert: exactly as hot as the node arenas.
- **flip-txn descriptors.** Raw `malloc` per update
  (`ft-mutation-helpers.h:134,177`) should route to rcu-txn-slab
  (`urcu/rcu-txn-slab.h`, already generic and per-CPU). Same disease,
  already-proven cure.

## 11. Demand reduction: the cheapest allocation is the one that doesn't happen

Supply-side scaling (§5–§9) composes with cutting the multi-writer allocation
rate at the source:

- **[rcu-txn-bitmap](rcu-txn-bitmap.md)**: composing a transacted occupancy
  bitmap flip with an O(1) node mutation turns qualifying recompact-on-insert
  cases back into in-place commits — no fresh node, no memcpy, no deferred
  free.
- **Sticky pigeon hints** (`fractal-trie-internal.h:501-511` FUTURE note):
  pigeon slots are direct-indexed, so an atomic-OR sticky hint bitmap keeps
  pigeon insert/delete O(1) in place, with an occasional cleanup recompact.

These change *how much* the allocator must scale; they do not remove the need
(popcount tiers still recompact, and churn still frees).

## 12. Plan and open questions

Measure before building. A microbench hammering
`cds_ft_alloc_item`/`cds_ft_free_item` (call_rcu-deferred frees, per-CPU
workers) from N threads on one order gives the arena-mutex saturation curve
*today*, independent of trie-side multi-writer existing yet. The same harness
then A/B's the two roadmaps:

- **Layered (§5+§6, then §7-vs-§8):** each step invariant-preserving and
  independently landable; free side still takes one lock per range per batch.
- **Unified (§9):** one mechanism subsuming §5+§6+§7, fully lock-free on the
  free fast path; more new code up front, and the drain integration is its
  one novel proof obligation (flag/sweep ordering, straggler window).

§9 is allocator-local (no trie-semantics change), so it can be prototyped and
falsified against the microbench alone before committing either way. Either
roadmap ends with **two slab caches** — the internal-node magazines (compact-
integrated, §9, or the layered A+B+C equivalent) and the simpler pinned-range
external cache (§10) — plus **§10**'s flip-txn → rcu-txn-slab routing, **§11**
(demand reduction), and the long game of **§8-style concurrent incremental
compaction via txn**.

Open questions:

- Magazine/frontier granularity: per-CPU (rseq or atomics, bounded by
  nr_cpus) vs per-writer-thread (TLS, unbounded but no migration races)?
  rcu-txn-slab's per-CPU choice is the precedent; §9's cross-CPU drain sweep
  wants plain-atomic wfstack ops rather than rseq push.
- §9 drain-race closure: is the second sweep at the first step boundary
  enough, or does the bypass flag need a stronger publication barrier before
  the first sweep (cheap under writer exclusion; revisit for concurrent
  compaction)?
- Pressure-triggered drain (§9): right trigger — fresh-superblock mmap,
  free_ranges starvation, or an RSS watermark? Useful independently of §9
  (e.g. to cap far-metadata frontier floors in §5)?
- Far-metadata pinning: per-order magazine caps vs disabling magazines for
  orders whose body unit is 2 MiB?
- Occupancy trigger for §8: per-arena live/capacity watermark, or feedback
  from reclaim (free_ranges starvation)?
- Who runs incremental compaction in multi-writer mode — a dedicated
  background thread per group, or writer-piggybacked steps
  (cf. the aging/escalation pattern in the bidir-list work)?
- Does §6's +1 GP reclaim latency interact with the bench's reclaim-domain
  sensitivity (per-hwthread call_rcu workers)? (Moot if §9 is chosen — it
  adds no GP.)
