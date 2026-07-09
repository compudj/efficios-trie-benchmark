# urcu-txn skiplist: forward-only ordered map over MCAS

Design for `urcu_txn_skiplist` (`<urcu/rcu-txn-skiplist.h>` in userspace-rcu-txn):
a transactional, RCU-read-side ordered set/map built on the MCAS engine
(`<urcu/rcu-mcas.h>` / `<urcu/rcu-txn.h>`). Forward-only towers (no per-level
prev), a per-level tombstone mark in the next-pointer low bits, and every
mutation committed as a single multi-word CAS across all of a node's levels.
Companion note to [rcu-txn-use-cases](rcu-txn-use-cases.md) and
[rcu-txn-hlist](rcu-txn-hlist.md). 2026-07-09.

## 1. Motivation

The hlist (unordered hash buckets) and the bidir sentinel list (order-dependent,
true prev, O(1) tail) cover the "membership" and "sequence" cases. The missing
member of the family is an **ordered** associative structure: predecessor /
successor / range queries, sorted iteration, `O(log n)` point ops — a `std::map`
/ `cds_lfht`-can't-order shaped container. A skiplist gives that with
probabilistic balancing (no rotations, so no wide restructuring txns) and a pure
forward next-chain the RCU reader already knows how to walk.

Built on the txn engine it inherits the family's headline property: the
`_prepare` forms **compose** into one commit, so "move key k from map A to map B,"
"insert into an index and a freelist atomically," or the kaleidoscope
skiphash (skiplist-of-hashes) are single linearizable transactions, not
lock-step protocols.

## 2. Node and structure

```
node = { unsigned int toplevel;
         struct urcu_txn_skiplist_node *next[toplevel + 1]; /* flexible, transacted */ }
```

The head is a full-height sentinel (`toplevel = MAX_LEVELS - 1`, default 8) with
no key; every descent starts from it uniformly. A node of height `h+1` occupies
levels `0..h`; level 0 is the complete, linearizing chain. Tower height is drawn
geometrically at insert (`random_level`).

Low-bit discipline, compatible with the engine: **bit 0** is the MCAS proxy
discriminator (an in-flight descriptor), **bit 1** is the deletion *mark*. The
mark lives only on a node's **own** forward pointers (`node->next[L]`) — it tags
"this node is logically deleted," never "the node I point to is deleted." That
asymmetry is the source of every subtle bug below, so it is worth stating once,
loudly:

> `is_marked(pred->next[L])` reports **pred's** deletion, not the successor's.

Forward-only: there are no per-level `prev` pointers. Delete therefore locates
its predecessors by search, exactly like insert, rather than following back-links
(the price is that both ops re-derive the descent; the saving is half the
pointers and no back-edge coherency).

## 3. Search returns (pred, succ) per level — and why

`search(sl, key, update, succ)` descends from the head; at each level it advances
`pred` while `pred->next.key < key`, then records `update[L] = pred` and
`succ[L] = ` the first node with key `>= key` at that level. The pair it hands
back satisfies, per level:

> `update[L].key < key <= succ[L].key`   (strictly `<` on the right when key is absent)

**This pair — not a fresh reload — is what a mutator must transact against.** The
single hardest-won lesson of this design (see §5) is that the commit's old-value
check proves *consecutiveness* (pred still points at succ) but says nothing about
*ordering*. Ordering is established exactly once, by search, and must be carried
into the commit by using `succ[L]` as the store's expected value. A mutator that
re-reads `pred->next[L]` afresh to get its old value silently discards search's
ordering guarantee and can splice a node behind a concurrently-inserted closer
key — a backward link the old-value check waves through.

`lookup` is a plain read-side descent (its own loop, no `update`/`succ`).

## 4. Operations

All are RCU-read-side + MCAS. The `_prepare` forms buffer the edges without
committing; the self-contained `add_rcu` / `del_rcu` wrap begin…commit with a
retry loop.

**insert(newp, key)** — one MCAS over levels `0..newp->toplevel`. Per level, with
`pred = update[L]`, `succ = succ[L]`:

- load `pv = pred->next[L]`; require `!is_marked(pv)` (pred alive) and
  `resolve(pv) == succ` (pred still adjacent to search's successor);
- load-validate `succ->next[L]`, require it unmarked (succ not logically deleted);
- set `newp->next[L] = succ` (plain, unpublished) and store
  `pred->next[L] : pv → newp`.

The store's old value `pv` (which resolves to `succ`) is what re-proves
consecutiveness at the install point; the key ordering rides in from search and
needs **no runtime comparison**. `-EEXIST` if search found the key; `-EAGAIN` on
any adjacency/tombstone failure (re-search).

**delete(key)** — one MCAS over the victim's levels. Per level, with
`node = victim`, `pred = update[L]`, `nv = node->next[L]` (the victim's own
successor):

- `is_marked(nv)` ⇒ a peer already marked the victim: `-ENOENT` at level 0
  (someone else linearized the delete), else `-EAGAIN`;
- require the predecessor: `!is_marked(pv) && resolve(pv) == node`
  (`pv = pred->next[L]`) — a stale/tombstoned predecessor whose slot happens to
  still read `node` would splice a dead region;
- load-validate the new successor `resolve(nv)->next[L]` unmarked;
- **mark** `node->next[L] : nv → MARK(nv)` (logical delete + the shared-slot
  conflict that catches an insert using `node` as its predecessor) and **unlink**
  `pred->next[L] : pv → nv`.

Mark and unlink at every level land in **one commit**, so there is no committed
state in which the victim is marked-but-still-linked (see §5).

**Readers** walk level 0 (or descend for point/range), resolving proxies and
stripping marks per hop.

**Composable move** (`del_prepare` on A + `insert_prepare` on B in one txn) is
the flagship: exactly one of A/B holds the key at every instant, validated by the
TAP test's conservation check.

## 5. Correctness

The engine gives atomic multi-word commit with value-based (resolve-aware)
old-value checks; the skiplist supplies the ordering discipline on top. Four
lemmas carry it.

**(a) The old-value check guards value, not order — ordering comes from search.**
This is the crux and the bug that this design was shaped by. An insert links
`pred → newp → succ`. If the mutator re-reads `pred->next` to obtain the old
value, a concurrent insert of a *closer* key `Y` (`pred.key < Y.key < key`)
splices `pred → Y` in the window between search and the reload; the reload returns
`Y`, the mutator sets `newp->next = Y`, and the commit's check (`pred->next == Y`,
which holds) installs `pred → newp → Y` with `Y.key < newp.key` — a **backward
link**, from which duplicate keys then cascade (two inserts reach the same gap via
different level-0 predecessors). The fix is structural: transact against search's
`succ[L]`, whose `key > target` was established during the descent. Then
`pred.key < newp.key` (search) and `newp.key < succ.key` (search) and
`pred → succ` consecutive at commit (old-value check) together force correct
order — no runtime key comparison, and no reliance on address stability. *Search
never returns a mis-keyed predecessor* (`pred.key < key` is a loop invariant);
the danger was only ever in throwing that result away.

**(b) Predecessor validation on delete.** Search resolves through marks, so
`update[L]` can be a node a racing op has moved off the victim. Delete bets on
the *loaded* `pv` and requires `resolve(pv) == node` and `!is_marked(pv)`; a
racing change to `pred->next[L]` fails the commit's old-value check and the caller
re-searches. This is the delete-side mirror of (a)'s successor discipline.

**(c) Atomic mark + unlink ⇒ no marked-but-linked committed state.** Unlike
Harris/Michael, where a node is first marked and *later* physically unlinked (so
readers must skip marked nodes and searches must help-unlink), here both land in
one commit. A logically-deleted node is, in every committed state, also
physically gone from every level. Consequences: the reader's "resolve strips the
mark" is not a stale-read hazard (a marked node is never reachable in a committed
state); and the successor-tombstone load-validate guards only the *mid-commit /
pre-existing* window where a peer's delete of `succ` is in flight — folding
`succ->next[L]` into the conflict set so that peer either aborts us or is
serialized behind us.

**(d) Adjacent insert/delete serialize on the shared next slot.** An insert using
`node` as predecessor and a delete of `node` both target `node->next[L]` (the
insert's store, the delete's mark); the engine serializes them, and whichever
commits second fails its old-value check and retries. This is the same argument
as the bidir/hlist "adjacent ops CAS the same next slot" block, inherited
verbatim.

Decisive evidence the discipline is complete: under a race-free order-checking
reader (one `rcu_read_lock` across the whole walk), the full workload passes with
zero violations across `FALLBACK ∈ {0,1,256}`, `MAX_LEVELS ∈ {1,8}`, and with and
without reclaim. `MAX_LEVELS = 1` degenerates to the minimal Harris/Michael
ordered list, so the same header covers and is validated at both ends of the
tower-height range.

## 6. Optimism, escalation, and composability

Each attempt runs optimistically; under contention the engine's fair-mutex
**escalation** funnels writers into a bounded FIFO (see rcu-txn.h). Skiplist
mutations are *wide* (up to `2 × toplevel` slots for a delete), so their read-set
conflict surface is larger than the hlist's 2-slot txns — the escalation path is
what bounds progress when optimism thrashes. The self-contained `add_rcu` /
`del_rcu` retry loops do **not** emit a quiescent state on their `-EAGAIN` / abort
edges: retries are short-lived (a thrashing writer escalates into the bounded FIFO
after `FALLBACK` optimistic attempts, and a standalone caller quiesces between ops
of its own), so the retry window pins the grace period only briefly — not worth
the machinery, and consistent with the composability rule below (a mid-retry q.s.
would be just as unwelcome inside a caller's read-side section).

**Composability property (load-bearing): `urcu_txn_begin` does not report a
quiescent state.** It stays RCU-online across the whole attempt, escalation FIFO
wait included, so a transaction may be nested inside a caller's *own* RCU
read-side critical section (traverse an RCU structure, find a node, transact
against it — all under one section) without begin() publishing a spurious q.s.
that would let a grace period free the caller's live pointers (UAF). The cost is
that a writer parked in the escalation lane stays in the grace-period quorum for
its bounded turn; escalation is a fallback and the lane owner never blocks on a
grace period under the mutex, so reclaim lags by at most one episode. This is why
the engine's `begin()` takes its FIFO turn online rather than offlining across the
wait — correctness (no UAF in composed use) over reclaim latency under
pathological contention.

## 7. What it gives up — and relation to the family

Forward-only means no O(1) predecessor step and no backward iteration; range
scans go forward from a lower bound. Delete re-searches for its predecessors
rather than following prev, trading pointer footprint and back-edge coherency for
the search. These are the right trade for an ordered index; the bidir sentinel
list remains for true-prev / tail uses ([rbtree threaded list](rcu-txn-rbtree.md),
LRU), and the hlist for unordered buckets. Three complementary shapes over the one
engine, mirroring list / hlist / tree in the kernel.

A future extension — read-driven self-balancing, where readers detect
gap-invariant violations for free on the descent and repair them with atomic
tower promotions (turning the randomized structure into a lazily deterministic
one, and defending online against RNG / insertion-order / key-flooding
pathologies) — is worked out in
[rcu-txn-skiplist-selfheal](rcu-txn-skiplist-selfheal.md).

## 8. Packaging

`rcu-txn-skiplist.h` (multi-writer, on rcu-mcas). A `rcu-txn-sw-skiplist.h`
(single-writer, on rcu-txn-sw) is a future sibling if a read-mostly-with-one-
writer variant is wanted; the mark/proxy helpers are already shared. Naming:
`urcu_txn_skiplist_*`. Registered in `include/Makefile.am` and the TAP suite
(`tests/unit/test_rcu_txn_skiplist.c`).

## 9. Validation and benchmark plan

- **TAP test** (landed): deterministic single-thread (2000 shuffled inserts,
  fsck of tower structure, membership, `-EEXIST`, odd/even delete), plus a
  concurrent workload — writers (50/50 add/del), order-checking readers, and
  movers doing composed cross-list moves with a conservation check
  (every key present exactly once). 12/12.
- **3-skiplist head-to-head vs McKenney "existence"**: this txn skiplist vs the
  perfbook existence-based skiplist vs a lock-based baseline, on the atomic-move
  workload — the read-side-tax (existence) vs write-side-tax (MCAS) duality; see
  [txn-vs-existence-3hash](txn-vs-existence-3hash.md) and
  [perfbook import](rcu-txn-use-cases.md).
- **Kaleidoscope skiphash**: skiplist-of-hashes composition, exercising nested
  `_prepare` commits across the two structures.
- Read scaling (allocator-neutral) and update scaling under glibc vs jemalloc,
  matching the existing figure conventions.
