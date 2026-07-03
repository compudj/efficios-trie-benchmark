# urcu-txn red-black tree: simplifying rbtree3, then going multi-writer

How urcu-txn could simplify the 2010–2011 RCU red-black tree prototype
(userspace-rcu branch `rbtree3`, `urcu-rbtree.c` ~1370 lines +
`urcu/rcurbtree.h`; CLRS ch. 12/13 interval tree with `max_end`
augmentation, COW node clusters, single writer under mutex), and whether the
result can support concurrent writers. Companion note to
[rcu-txn-use-cases](rcu-txn-use-cases.md) and
[rcu-txn-prio-heap](rcu-txn-prio-heap.md). 2026-07-02.

## 1. Where rbtree3's complexity lives

1. **Cluster copy-on-update** — `dup_decay_node()` plus the decay machinery
   needed because every rotation invalidates the writer's own pointers:
   `decay_next`, `get_decay()`/`is_decay()`, `rcu_rbtree_min_dup_decay()`,
   `rcu_rbtree_min_update_decay()`, ~30 decay asserts across both fixup
   loops. Rotations copy 3 nodes; the remove teleport copies the whole left
   spine of z's right subtree.
2. **Multi-step publication ordering** — `wmb`-then-publish-top-first
   discipline, `C__CMM_STORE_SHARED`, children-reparenting passes after each
   publication.
3. **Parent-pointer coherency for next/prev** — the ~100-line
   rotation/transplant/teleportation "black box" traversal-equivalence
   argument at the top of the file; exists solely because readers walk *up*
   via parent pointers.
4. **`max_end` propagation with path copying** — `populate_node_end()`
   duplicates every parent up to the root whose `max_end` changes: O(log n)
   allocations + O(log n) `call_rcu` frees per insert/remove.
5. **Unhandled allocation failure** — the file's TODO; copies can fail
   mid-update with no unwind.
6. **API scar tissue** — node identity is unstable (nodes are copied by
   rotations), so remove-by-pointer is illegal; users must re-search.

## 2. The crux: what COW is really doing

The naive port — "each rotation becomes a ~6-slot commit, drop the copies" —
is **wrong**. The cluster copies are not compensating for missing multi-word
atomicity; they give *in-flight readers a frozen suffix of their descent*.
With an in-place rotation, however atomic, a reader already stationed at x
when left_rotate(x) commits reads `x->_right` and gets y_left — but the key
it chases may now live in yr, *above* x, unreachable downward: a false miss
for a continuously-present key. Atomicity at the commit cannot repair state
the reader accumulated before it. Whatever replaces COW must make descents
safe by other means (§4) — or keep COW (§3).

## 3. Design 1 — keep COW clusters, transact the splice

Conservative port: keep `dup_decay_node()` for the 2–3 rotated nodes, but
publish each cluster with one commit that flips the external child edge, the
`max_end` path (updated in place — `populate_node_end()` no longer copies),
and the list links (§4) together.

Eliminated: item 2 entirely (one commit is the only publication point; no
barriers, no publication order); most of item 1 (a rotation returns its new
nodes; decay chains collapse to return values); item 4's allocation churn;
item 5 (all allocation precedes any publication → clean -ENOMEM). Read-side
guarantees unchanged. Roughly halves the file. Robust under write-heavy
load (no reader retry path).

## 4. Design 2 — copy-free single-writer: "hits are RCU, misses validate"

Three observations gut the file:

1. **Hits are self-certifying.** A found node matches the query by its own
   immutable `begin`/`end`. Topology races manufacture only false *misses*,
   never false hits. RCU linearization ("present at some point during the
   search") is preserved — identical to the 2010 semantics.
2. **Only misses need validation.** Single-writer variant: per-tree even/odd
   generation counter (seqlock); a reader reaching nil re-reads it and
   retries the descent if it changed. The counter bumps per *commit*, not
   per logical operation — Design 2 deliberately splits one insert/remove
   into several commits — so the miss-retry rate is a multiple of the op
   rate (§7 measures the tax accordingly). Hit path stays tier-1, zero cost.
   `max_end` pruning errors also only manufacture misses → same validation
   covers interval search. (Multi-writer replaces this — §5.3.)
3. **In-order traversal is rotation-invariant.** Rotations never change the
   in-order sequence; only insert/remove do. Thread a `urcu_txn_list` bidir
   list through the nodes, spliced in the same commit as the attach/detach
   edge (`_prepare` composition). next/prev become list reads. **Parent
   pointers and colors become writer-private plain fields** — never copied,
   never ordered, never read by readers. Item 3 (teleportation argument)
   disappears entirely.

Writer structure, keeping the existing mutex: attach/detach + list splice as
one commit; each fixup rotation as its own small commit (~3 edge slots + 2
local `max_end` — intermediate states are valid BSTs; balance violations are
invisible because colors are private); final `max_end` path as one commit.
All commits uncontended → run on **`rcu-txn-sw`**. Remove's teleport is one
~6-slot commit computed entirely from pre-state, so buffered-writes-invisible
never bites. Engine feature this motivates: **read-your-own-writes in the sw
engine** (trivial there — the single writer owns the buffer) would collapse
each operation to exactly one commit.

User-visible wins: stable node identity (remove-by-pointer legal — fixes
item 6), `call_rcu` only on true removal (not per rotation), next/prev with
no caveats, and a file that is essentially CLRS + `urcu_txn_store()` on
child edges + a miss-retry wrapper — well under 500 lines.

Costs: miss-path retries and the generation-counter line degrade under
write-heavy load (seqlock tax, confined to misses). If that matters, use the
§5.3 local miss certification even in the sw variant, or fall back to §3.

## 5. Multi-writer on the mcas engine

Intuition says the root serializes writers. It mostly doesn't: a balanced
tree's apex is **read-hot but write-cold** — the opposite of the prio-heap,
where every pop writes slot 0.

### 5.1 What actually writes near the root

- Rotations: O(1) worst case (≤2 insert, ≤3 remove), located where the fixup
  terminates — within a couple levels of the attach point w.h.p.
- Recolor cascades climb 2 levels/step only while the uncle is red;
  P(climb j) ~ 2^-Θ(j); fraction of ops whose write set reaches the top of an
  n-node tree ~ n^-Θ(1). Invisible for n ≥ ~1e5.
- `max_end`: an ancestor updates only if the new end beats a size-s subtree's
  max (~1/s); s doubles per level → O(1) expected slots, root w.p. ~1/n.
- Root pointer slot: root rotations and first/last-node transplants only.

Physically the top levels sit in every descent's read path, but read-mostly
lines replicate in S-state for free; the rare apex commit invalidates
everyone once, amortized to nothing.

### 5.2 Bookkeeping that would fake root contention — remove it

1. **`rbtree->root->color = COLOR_BLACK` after every insert fixup**
   (urcu-rbtree.c:1015): unconditional root-color write = universal
   conflict. Make it conditional — only cascades that actually turned the
   root red touch the slot (exactly the rare cascade-to-root case).
2. **`x != rbtree->root` loop tests in remove fixup** read the root slot each
   iteration. Keep as plain reads (or test via parent field); never a
   validated read-set member.
3. **§4's global generation counter does not survive multi-writer**: parity
   is meaningless with overlapping writers, and "retry if any commit happened
   during my descent" starves miss readers under sustained writes. Replaced
   by §5.3.

### 5.3 O(1) local miss certification via the threaded list

A miss for key k landing at `y->_left == nil` is certified by
"`y->_left == nil` AND `list_prev(y).key < k`" coexisting — a **2-slot
guarded read** ({v→v} commit), validating purely local state, indifferent to
distant commits. Same trick that makes optimistic descent safe in leaf-linked
B-trees / B-link trees. No global word; miss cost O(1) CASes, hit cost zero.
(Symmetric right form: `y->_right == nil` AND `list_next(y).key > k`.) Note
the certification leans on §5.4's dead-marks: for a concurrently *removed*
y, soundness requires the nil child slot to have been tombstoned — an
un-tombstoned nil would certify a miss against a node no longer in the
tree. Lemmas 1 and 3 of §6 must therefore be proven jointly, not
independently.

### 5.4 The keystone: descents must not be validated

Full-path read-set validation would recreate root contention through the
read sets (every apex-region commit aborts every in-flight writer). The way
out is order-theoretic, not topological:

- Key k belongs at slot `y->_left` iff pred(y) < k < y — a property of key
  order, not topology. Rotations preserve in-order sequence, so no rotation
  invalidates an attach target while y is in the tree and the slot is nil.
  Any insert that would move pred(y) past k must target the *same* slot →
  caught as a plain write-write slot conflict.
- The one hole: y removed concurrently (its child slots survive with stale
  nil). Plug with **dead-marks**: the removal transaction also tombstones the
  victim's two child slots (2 extra words, same commit), so a racing attach
  fails its old=nil check. (Two-child removal needs no marks on the
  detached node z — with two live children z has no nil slot to attract an
  attach — and the teleported successor stays in the tree with its key
  unchanged: its child slots are either rewritten by the teleport itself,
  so racing attaches conflict on the slot naturally, or remain *valid*
  attach targets, their key intervals surviving the relocation.)

Resulting transactions: insert = {attach slot (old nil→z) + O(1) fixup
slots}; remove = {~6 splice slots + 2 dead-marks + O(1) fixup slots};
descents contribute nothing to the conflict graph. Writers conflict only on
genuine key adjacency (~w/n for w concurrent writers) plus the geometric
fixup tail. Expected abort rate O(w/n): at n = 1M, w = 192, well under 1% —
the many-buckets regime of the write-contention study, with leaves as
buckets. Colors and parent pointers re-enter the transacted set in
multi-writer (writer-vs-writer state), but their write locations follow the
same geometric decay.

### 5.5 Degradation modes

- **Skewed keys**: monotonically increasing keys drive every insert to the
  right spine — permanent rotations on one shared path incl. near-root; the
  bucket-count→1 regime; no engine fixes the workload.
- **Small trees** (n ≲ 1k): everything is "near the root" until it grows.
- **Colliding cascades**: two writers' recolor chains meeting mid-tree abort
  one; retries redo the cascade; compounds at high w on small n. Escalation
  fallback bounds it.

## 6. Verification plan

Three lemmas deserve model-checked (or at minimum carefully written) proofs
before code:

1. Hits-self-certifying / miss-certification linearization (§4.1, §5.3).
2. Rotation-invariance of the threaded list as the sole iteration order (§4.3).
3. The no-descent-validation attach lemma incl. dead-marks and teleport
   (§5.4) — the rb-tree analogue of the B-link-tree reachability lemma, and
   the correctness keystone of the multi-writer design.

## 7. Benchmark plan (when prototyped)

Engines: (a) rbtree3 as-is (COW + mutex), (b) Design 2 on rcu-txn-sw +
mutex, (c) multi-writer on rcu-mcas, (d) mutex + plain CLRS rbtree
(readers excluded) as the naive baseline. Workloads: random keys at n ∈
{1k, 100k, 10M}; read-mostly and write-heavy mixes; sequential-key
adversarial case; interval-search mixes exercising `max_end`. Metrics:
reader throughput (hit and miss paths separately — the miss-seqlock tax and
the guarded-miss cost are distinct), writer scaling curves, abort rate
**bucketed by tree depth of the conflicting slot** (directly falsifies
"conflicts live at the leaves, geometric decay upward"), allocation +
call_rcu rate per op (vs rbtree3's O(log n) churn).
