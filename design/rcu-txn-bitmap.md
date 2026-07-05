# urcu-txn bitmap: transacting a fractal-trie node's occupancy so recompaction composes

A fixed-size bitmap laid over transacted words (63 data bits each, bit 0 the
engine proxy tag) so that a bit set/clear rides the *same* MCAS commit as the
mutation it accompanies. The target: turn a fractal-trie (FT) node's
copy-on-write recompaction into a single in-place transaction. Companion to
[rcu-txn-use-cases](rcu-txn-use-cases.md). **Implemented and unit-tested**
(header `include/urcu/rcu-txn-bitmap.h` + `tests/unit/test_rcu_txn_bitmap.c`,
12/12 TAP, on `urcu-txn-dev`). 2026-07-05.

## 1. The problem: recompaction is COW because the bitmap and the array must agree

An FT node is an occupancy bitmap plus a **popcount-rank-compressed** child
array: `FT_ENTRY_PER_NODE = 256` (one key byte per level), and a child at byte
`k` lives at array slot `rank(k) = popcount(bitmap & below(k))`. The flat tiers
hold a 256-bit bitmap (`uint64_t bm[4]` for popcount_1l; a 32 B out-of-line word
for pigeon); the small tiers a 2-level 16-bit `root`+`sub` header.

Insert/delete go through **`ft_node_recompact`** (ft-mutation-node.h): it
allocates a *fresh* node, walks the live children, rebuilds the bitmap and the
rank-compressed pointer array from scratch, and republishes the parent edge —
copy-on-write of the whole node. It is COW precisely because a live, in-place
edit of the (bitmap, array) pair cannot stay mutually consistent for a lockless
reader that computes `rank` from the bitmap and then indexes the array: the two
words would disagree mid-edit. The price is an allocation, a `memcpy` of the
surviving children (the ~20 % the inequality profile blames on recompaction),
and a grace-period reclaim — and it serializes writers behind single-writer COW.
Today the occupancy bitmap is only a **relaxed hint**; the pointer load is the
source of truth (a reader that sees the bit but not the pointer just gets a
legitimate not-found).

## 2. The idea: transact the bitmap so its flip rides the node's commit

Make the occupancy bitmap a *transacted* slot. Then `{set bit k}` composes with
`{the node mutation}` — a pointer store, or a bounded array splice — into **one
MCAS commit, one linearization point**. No fresh node, no whole-node `memcpy`,
no reclaim, and — since disjoint nodes touch disjoint words — writers on
different nodes commit concurrently ([use-cases](rcu-txn-use-cases.md)
capability C). The bitmap stops being a lossy hint and becomes consistent with
the array *in every committed state*. Where that actually beats COW — and where
COW still wins — is the whole of §6: it holds only when the node update touches
O(1) words.

The cost of admitting the bitmap into the commit is the engine's tag bit: a
transacted slot owns bit 0, so a bitmap word keeps 63 data bits. That is the
"63-bit per `uintptr_t`" premise this design started from.

## 3. Encoding — the engine's own `<<1` discipline, no engine change

Bit 0 = `URCU_MCAS_TAG`; data in physical bits 1..63; logical bit `i` at physical
`i+1`; a settled word always has bit 0 clear. This is exactly the engine's
documented rule for non-pointer payloads — *"store small integers shifted left
by 1"* (`rcu-mcas.h:91-97`) — so there is **no engine change and no sentinel
carve-out** (contrast the hlist UNINIT/nulls values,
[rcu-txn-hlist §6/§8](rcu-txn-hlist.md)). A zero-filled region is a valid empty
bitmap, matching the FT's demand-zero / calloc'd node storage.

The tag is the *narrow* bit-0 `URCU_MCAS_TAG`, and the engine now carries the
tag **per record**, so a bitmap word (tag = bit 0) shares one commit with the
FT's typed-pointer slots (which carry the wider `FT_FLIP_PROXY_TAG` nibble) —
the per-record `proxy_tag` is what makes the heterogeneous commit legal. The FT
already runs this exact discipline for a non-pointer field: `FT_NR_KEYS_PROXY_TAG
= 1` stores `nr_keys` shifted `<<1` (`ft-helpers.h`). The bitmap generalizes it
to 63-bit words.

A 256-way node's bitmap is `ceil(256/63) = 5` transacted words (vs 4×64 today):
+8 B per node and `rank` spans five popcounts instead of four. Per operation
only the **one** word whose bit changes is transacted; the others are plain
resolved reads. So multi-word storage never inflates the per-op write set.

## 4. The API (as implemented)

Operating on a caller-provided `uintptr_t *words` (embedded in the node), sized
by `URCU_TXN_BITMAP_NR_WORDS(nbits)`:

- **Reads** (in an RCU read-side section; resolve proxies): `test_rcu`,
  `rank_rcu` (popcount-below = the compressed-array index), `weight_rcu`,
  `ffs_from_rcu` (next set bit — pigeon iteration), `select_rcu` (the *i*-th set
  bit, inverse of rank), `word_rcu`.
- **Compose** (`_prepare`, append edges to the caller's open txn):
  `set_prepare`, `clear_prepare`, `set_range_prepare`, `clear_range_prepare`.
- **Standalone** (`_rcu`, own begin/commit/end + retry): `set_rcu`, `clear_rcu`.

The word+mask work is the load-bearing layer; the bit-index wrappers pay a `/63`
that compiles to a multiply-shift. The unit test exercises the encoding across
every 63-bit boundary, the bit-0 invariant, concurrent no-lost-update under
word-granular contention, and composition atomicity (§5).

## 5. What readers get, and what they still must do

Plain reads are **tier-1**: per-slot linearizable, *not* a cross-word snapshot. A
reader that reads the bitmap to compute `rank` and then reads `array[rank]` as
two separate loads can straddle a commit and see an old rank against a new
array. That is fine and unchanged for the FT: the pointer is the source of truth
and the reader revalidates the key at the child, so a stale rank yields a
wrong/absent pointer → a miss or retry, exactly as with today's relaxed hint.

What transacting *buys* is on the writer side plus an opt-in reader guarantee:
every **committed** state has the bitmap and the array agreeing (they flip
together), and a reader that needs a coherent pair can take a **tier-2 guarded
snapshot** — `load_validate` the bitmap word and the paired slot, then commit —
which certifies both at one linearization point. The test drives six writers
toggling `{bit k, pointer k}` together against guarded-snapshot readers: across
~120 000 snapshots, **zero** saw `bit != (ptr != NULL)`. Were the two committed
separately, a snapshot would catch the intermediate state; it never does.

## 6. Where it pays: the O(1)-word rule

The §2 pitch — fold the bitmap flip into the mutation's commit, no COW — holds
only where the *whole* node update touches **O(1) words**. The reason is that
**COW already buys atomicity for free**: publishing a rebuilt node is a single
parent-edge store, and that store *is* the linearization point, so a bitmap
inside a COW'd node never needs to be transacted. A transacted bitmap earns its
keep only for an **in-place** update — no fresh node, so no single publish
point, so MCAS is what makes the several in-place words flip together. In-place
via txn therefore beats COW only when the edge count is a small constant.

Rank-compression breaks that condition. A child lives at `array[rank(k)]`, so
setting bit `k` shifts the whole suffix `[idx..n)`: an in-place insert is
`O(n − idx)` edges — up to ~124 for the 1024 B `popcount_1l` tier. That loses to
COW twice. On **cost**: a ~125-record k-CAS (sort + install ~125 proxies +
settle) is ~250 CAS, versus `malloc + memcpy(≈16 lines) + one parent-edge
publish + call_rcu`, whose linearization is a *single* store. On **contention**:
a 125-edge txn is a node-wide latch — every concurrent op touching any of those
slots must help-drive it or abort, tripping the help-depth cap into escalation,
where COW conflicts on one edge.

> **The rule: an in-place transacted update beats COW iff the tier is
> direct-indexed. For every rank-compressed tier COW is as good or better — and
> needs no transacted bitmap at all.**

- **Pigeon** (95–256 children, 2048 B) is the one direct-indexed tier:
  `child = pointers[k]`, so insert = `{store pointers[k]} + {set bit k}` =
  **2 edges**, delete the mirror — no shift, no fresh node, no `memcpy`, no
  reclaim. It is also where COW is *most* expensive (a 2 KB copy), so the win is
  largest exactly where it matters. The FT source already flags this seam (a
  proposed *sticky transacted* pigeon bitmap).
- **Rank-compressed tiers stay COW.** Their bitmap need not be transacted; the
  mutation still composes into a larger commit through the **parent-edge swap**
  (the rebuild's single publish, folded as one edge alongside a split/graft/
  sibling edge). No shift is ever transacted.

Separate the two goals — they have different answers. *In-place* (avoid
alloc/copy/reclaim) is pigeon only. *Composable* (fold a compressed-tier update
into one multi-node commit) is reachable without the shift by COWing **only the
pointer block** and composing `{bitmap, array_ptr}` — a 2-edge txn swapping in a
freshly `memcpy`'d array — at the price of one lookup indirection (node → block).
Usually a bad trade for a read-mostly trie (an extra miss per lookup), but the
honest way to get a compressed-tier update into a shared commit cheaply.

If you want cheap *in-place* insert on more than pigeon, the lever is
**representation, not the transaction**: an unsorted `{key, ptr}` array (ART
node4/16 style) makes insert an append and delete a swap-remove — `O(1)` edges,
trivially composable — at the cost of search-based lookup instead of rank
indexing. A fine trade for the *small* tiers (≤14–16 children, where linear/SIMD
search rivals a rank), pointless for the large ones. Equivalently: **the
direct-vs-compressed boundary is now the in-place-vs-COW line**, and the FT can
move it down (more direct-indexed tiers) to widen where in-place composition
applies, trading node footprint for update composability.

**In-place delete** follows the rule: pigeon delete is `{clear pointers[k]} +
{clear bit k}` — the FT today never clears a bit (it rebuilds), and
`clear_prepare` enables it for the direct tier, while compressed-tier delete
stays COW.

## 7. What it gives up

- **Word-granular conflict.** Two writers on different bits of the same 63-bit
  word conflict on the word-CAS; a raw `uatomic_or` would not. For the FT this
  is per-node and low; for allocator-style users it is mitigated by per-CPU word
  partitioning ([prio-heap §5-6](rcu-txn-prio-heap.md) scatter).
- **63 vs 64.** +8 B of bitmap per 256-way node and a rank over five words; the
  `/63` index math on the scalar wrappers (multiply-shift; the word+mask API
  avoids it on hot paths).
- **The shift is not free to transact.** Rank-compressed in-place insert is
  `O(n)` edges and loses to COW on both cost and contention (§6's rule) — so the
  composition win is the direct-indexed (pigeon) tier, not a blanket "transact
  every tier".
- **All writers through the engine.** An adopted tier's bitmap word becomes an
  authoritative transacted slot; it can no longer be written with a raw relaxed
  `uatomic_or` (that would clobber an in-flight proxy). All-transacted or
  all-hint, per tier.

## 8. Status and next steps

Landed on `urcu-txn-dev` (header + TAP test, 12/12), flowing to `ft-txn-integ`
via the shared remote. The integration step is the pigeon tier: rewrite its
insert/delete as a `{pointer store/clear} + {bitmap set/clear}` composed commit
in place of `ft_node_recompact`, and measure against the COW baseline —

- insert/delete-heavy churn on pigeon-sized nodes: composed-commit vs
  recompact (the allocation + `memcpy` + reclaim it removes);
- writer scaling across distinct nodes (capability C) vs single-writer COW;
- reader parity — the revalidating lookup path is unchanged, so read
  throughput should not move;
- footprint: +8 B/node bitmap at the 256-way tiers.
