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

## 6. What it gives up — and why that is acceptable

No O(1) tail, no backward iteration (pprev points at a slot, not a node).
Buckets need neither: chains are unordered, insert-at-head. The bidir
sentinel list remains for order-dependent uses — the
[rbtree threaded list](rcu-txn-rbtree.md) needs true prev, LRU needs a
tail. The two are complements, exactly as list/hlist in the kernel.

## 7. Future option: hlist_nulls

With call_rcu/grace-period frees (the liburcu discipline), plain NULL
termination is correct. If SLAB_TYPESAFE_BY_RCU-style immediate reuse is
ever wanted (nf_conntrack does this), the extension is an `hlist_nulls`
variant: per-bucket nulls sentinels so a reader migrated mid-traversal by
node reuse detects the wrong-chain termination and retries. The encoding
leaves room (nulls values are odd immediates, distinguishable from node
pointers). Out of scope for the first cut.

## 8. Packaging

Sibling headers, not same-file cohabitation: `rcu-txn-hlist.h` (mw) and
`rcu-txn-sw-hlist.h` (sw), mirroring the existing pair; hoist the shared
mark/proxy helpers into a common internal header. Node/head types and every
op signature differ from the sentinel list, so a shared file saves nothing,
and the (already subtle) coherency comment blocks stay each about one
structure. Naming: `urcu_txn_hlist_*` / `urcu_txn_sw_hlist_*`.

## 9. Benchmark plan

- Swap the bench's hash-of-lists engines to hlist heads; measure read-side
  gain from head density (read-mostly, large bucket count) and writer-side
  parity (ops are the same edge counts ±1).
- Tail-delete microbenchmark to confirm the 2-edge win.
- Bucket-sealing drain prototype as the first step of the txn-rehash
  experiment (seal → drain → unseal-to-new-table on one bucket, measured
  against readers).
- Footprint: table RSS at 1M/16M buckets vs sentinel heads.
