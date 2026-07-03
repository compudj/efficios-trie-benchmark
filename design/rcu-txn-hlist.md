# urcu-txn hlist: single-pointer-head list for hash table buckets

Design for a `urcu_txn_hlist` variant of the transactional list
(`<urcu/rcu-txn-list.h>` / `<urcu/rcu-txn-sw-list.h>` in userspace-rcu-txn):
kernel-hlist-shaped, one-pointer bucket heads, pprev encoding. Companion
note to [rcu-txn-use-cases](rcu-txn-use-cases.md). 2026-07-02.

## 1. Motivation

The existing txn list is a circular sentinel design: `urcu_txn_list_head`
embeds a full node (16 bytes, next+prev), and `prev` is a node pointer. As a
hash bucket head that is 2× the kernel's `hlist_head` footprint, and the
kernel structures named as urcu-txn candidates (nf_conntrack, inode hash,
dcache) are all hlist — porting them today means translating to the sentinel
list first. A single-pointer head gives:

- 8 B/bucket instead of 16 B — 8 heads per cacheline; halves table footprint
  at kernel scale, doubles head density for the bench's hash-of-lists
  engines.
- 1:1 structural mapping for the kernel port narrative.
- Cheaper per-bucket work for the txn-rehash design (atomic cross-table item
  move; see §5).

## 2. Not just a smaller head: the pprev encoding

Keeping the current node type (node-pointer `prev`) with a 1-pointer head
does not work cleanly: the first node's `prev` has no node to name, so every
op and the coherency argument grow a head special case, and remove-by-node
needs the bucket. The kernel's answer transfers directly and is *more*
native to the MCAS engine than the current list:

> `pprev` = a pointer to the **slot** that names me (`&prev->next`, or the
> bucket head slot itself).

Slots are the engine's currency. With pprev, the head's single pointer is
just another "next" slot, handled uniformly with interior slots — no head
special case anywhere. This uniformity is exactly *why* hlist gets away with
a 1-pointer head.

Node: `{ struct urcu_txn_hlist_node *next;  /* node ptr, transacted */
         struct urcu_txn_hlist_node **pprev; /* slot ptr, transacted */ }`.

Bit encoding is compatible with the existing discipline (bit 0 engine proxy,
bit 1 deletion mark on next values): pprev *values* are addresses of
word-aligned pointer fields, so bits 0–1 stay free. The mark lives only on
next slots, as today.

## 3. Operations (isomorphic to the sentinel list)

- **delete(elem)** — 3-edge commit:
  - `&elem->next  : next        → MARK(next)`   (logical delete)
  - `*elem->pprev : elem        → next`          (unlink forward)
  - `&next->pprev : &elem->next → elem->pprev`   (unlink backward)

  When elem is last (`next == NULL`) the third edge vanishes: a **2-edge
  delete**, cheaper than the circular list's invariable 3.
- **insert-at-head(new)** — 2-edge commit (new's own fields plain,
  unpublished):
  - `head slot    : first → new`
  - `&first->pprev: &head → &new->next`
- **insert-before / insert-after** — same shapes as kernel
  `hlist_add_before/behind`, 2-edge commits against the anchor's slots.
- Readers: forward next-chain traversal only, tier-1 (resolve proxy, strip
  mark per hop). Empty-bucket test = one load.

`_prepare` forms compose into larger commits exactly like the bidir list
(hash+LRU, cross-bucket moves).

## 4. Correctness: inherited, not new

The next-only-mark argument in rcu-txn-list.h (the "adjacent insert and
delete CAS the SAME next slot" block) transfers verbatim: every adjacency
still serializes on the shared forward next slots, and pprev is never marked
for the same reason prev never is — no operation reaches a node *only*
through it. The single genuinely new lemma is trivial: *the head slot is an
ordinary next slot* (it is named by pprev values and CAS'd by inserts and
first-node deletes in just the way an interior `&prev->next` is).

## 5. Extras that fall out of the encoding

- **Remove-by-node without knowing the bucket.** pprev names the slot, so
  composable transactions ("unlink from hash + unlink from LRU + insert
  elsewhere") need only the node in hand — no bucket recomputation. Directly
  serves the memcached-style hash+LRU composition use case.
- **Bucket sealing for txn-rehash.** The head is now an ordinary slot in the
  mark discipline, so the rehash drain can **mark the head slot** to seal a
  bucket: racing inserts lose their old-value check, see the mark, and retry
  against the new table. A clean per-bucket migration protocol with no
  additional mechanism — something the sentinel list cannot express (its
  head node is never deleted, so its slots are never marked). The
  cross-table item move itself is {3-edge unlink + 2-edge insert} in one
  commit.

## 6. In-place growth: many small transactions, not one commit

Reserve virtual address space for the maximum table up front
(`MAP_NORESERVE` anonymous mapping; pages fault in as buckets materialize)
and the bucket array's address never changes across growth. The tempting
completion — one commit that resplices every head and updates the size
field — is the wrong shape: an O(#buckets)-edge txn's conflict footprint
is the whole table. For the whole commit window, every concurrent insert
CASes a head slot the resize owns, so every writer in the system either
aborts against it or is dragged into helping an O(N) transaction (tripping
the help-depth cap and escalating everyone). That is a stop-the-world
executed through the most expensive available mechanism; if
atomic-snapshot resize is ever truly wanted, the bulk-STW regime (writer
drain + reader gate + plain stores) is the honest way to buy it.

The atomicity the big commit buys — readers never see a size that
disagrees with the heads — is not needed. Substitute an invariant readers
can tolerate: **an unsplit new bucket redirects to its parent.** With
power-of-two in-place growth, bucket `h & (2N-1)` has parent `h & (N-1)`:

- Publish "growing to 2N" as a one-slot txn. New heads `[N, 2N)` hold an
  UNINIT sentinel (an odd immediate; the encoding has room — same family
  as the §8 nulls values).
- Readers hash with the wide mask; on UNINIT they re-mask to the parent
  and read there. One extra load, only during growth.
- Buckets split incrementally: seal the parent head (§5), resplice,
  install both heads — one modest txn per bucket — driven lazily by
  writers that touch an unsplit bucket (lfht's lazy bucket init,
  transplanted) and/or a background drainer. Retire the "growing" state
  once the drain completes.

No txn exceeds one chain, and the split unit can shrink further. A
per-bucket split is O(chain) edges — fine at expected O(1) occupancy, ugly
under flooding — but the §5 cross-table move ({3-edge unlink + 2-edge
insert} in one commit) works within a table too: seal the parent, migrate
item-at-a-time in constant-size txns, readers checking
new-bucket-then-parent. The move's atomicity keeps every item findable in
exactly one of the two chains at every instant. Resize txns become O(1)
regardless of chain pathology, and the drain parallelizes across buckets.

What the VM reservation is really for is not the big commit — it is that
**there is no second table**: no old/new table pointers for readers to
resolve, no RCU handoff between arrays, no double-lookup structure. Growth
is index arithmetic in one mapping, and parent fallback is a bit-mask.
(lfht's bucket-node infrastructure exists partly to avoid reallocating the
array; the reservation dissolves that problem instead.) Shrink is the same
protocol mirrored: seal both children, splice into the parent, publish the
narrower mask.

Versus split-order, stated precisely: lfht makes resize cheap by making
items *immobile* — a split moves zero items, the new bucket pointer aims
into the shared ordered chain — where the txn hlist pays real resplice
work per split. What that price buys back: unordered chains, growth and
shrink with no ordering infrastructure in the nodes, constant-size txns
via item-granularity migration, a drain that scales with drainers, and —
the capability split-order structurally forecloses — rehash under a
*changed* hash function. Parent fallback only works when the new placement
refines the old (same hash, more bits); a full rehash has no parent
relation and needs the cross-table drain of the txn-rehash design, for
which this section's machinery is the per-bucket building block.

## 7. What it gives up — and why that is acceptable

No O(1) tail, no backward iteration (pprev points at a slot, not a node).
Buckets need neither: chains are unordered, insert-at-head. The bidir
sentinel list remains for order-dependent uses — the
[rbtree threaded list](rcu-txn-rbtree.md) needs true prev, LRU needs a
tail. The two are complements, exactly as list/hlist in the kernel.

## 8. Future option: hlist_nulls

With call_rcu/grace-period frees (the liburcu discipline), plain NULL
termination is correct. If SLAB_TYPESAFE_BY_RCU-style immediate reuse is
ever wanted (nf_conntrack does this), the extension is an `hlist_nulls`
variant: per-bucket nulls sentinels so a reader migrated mid-traversal by
node reuse detects the wrong-chain termination and retries. The encoding
leaves room (nulls values are odd immediates, distinguishable from node
pointers). Out of scope for the first cut.

## 9. Packaging

Sibling headers, not same-file cohabitation: `rcu-txn-hlist.h` (mw) and
`rcu-txn-sw-hlist.h` (sw), mirroring the existing pair; hoist the shared
mark/proxy helpers into a common internal header. Node/head types and every
op signature differ from the sentinel list, so a shared file saves nothing,
and the (already subtle) coherency comment blocks stay each about one
structure. Naming: `urcu_txn_hlist_*` / `urcu_txn_sw_hlist_*`.

## 10. Benchmark plan

- Swap the bench's hash-of-lists engines to hlist heads; measure read-side
  gain from head density (read-mostly, large bucket count) and writer-side
  parity (ops are the same edge counts ±1).
- Tail-delete microbenchmark to confirm the 2-edge win.
- Bucket-sealing drain prototype as the first step of the txn-rehash
  experiment (seal → drain → unseal-to-new-table on one bucket, measured
  against readers).
- Incremental in-place growth prototype (§6: size publish + parent
  fallback + lazy per-bucket splits) vs cds_lfht auto-resize on a
  growth-heavy churn workload; measure the reader penalty of the UNINIT
  fallback during the drain, and per-bucket-split vs per-item-move txn
  variants under a flooded chain.
- Footprint: table RSS at 1M/16M buckets vs sentinel heads.
