# urcu-txn skiplist: read-healed balance (self-adjusting towers)

Design sketch for making `urcu_txn_skiplist` (see
[rcu-txn-skiplist](rcu-txn-skiplist.md)) *self-balancing under read traffic*:
readers detect local balance defects on the descent they are already doing and
repair them with atomic tower promotions/demotions through the MCAS engine.
Because the structure allows concurrent multi-writer updates, any reader can
momentarily become a writer to heal the region it just traversed. 2026-07-09.

## 1. Motivation

A randomized skiplist's `O(log n)` rests entirely on the tower heights being
geometrically distributed. That assumption is fragile in ways a long-lived,
adversary-facing structure actually meets:

- a weak or biased RNG (or a cheap per-thread RNG reused across many inserts);
- adversarial *insertion order* or a key stream chosen to cluster;
- churn that deletes the tall towers out of a region, leaving a long level-0 run
  with no express lane above it (a "gap");
- **key-distribution flooding** — the ordered-map analogue of hash flooding: an
  attacker who can drive inserts into a narrow key range collapses the express
  lanes there and turns lookups linear.

The randomized structure has no way to notice or recover. But the txn skiplist
already has the one capability needed to fix it online: **atomic structural
edits under concurrency.** Promoting a node into a higher level, or demoting it
out of one, is exactly the kind of small composable MCAS the engine is built for
— and it is safe against concurrent inserts/deletes for free. So the question is
not *can* we rebalance online (we can), but *when to trigger it cheaply*. The
answer proposed here: **on the read path, where the traversal is free and the
traffic is.**

## 2. Reframe: a lazily-maintained deterministic skiplist

The sharp way to see this is that it converts a *randomized* skiplist into a
lazily-maintained *deterministic* one. The deterministic (1-2-3) skip list
(Munro, Papadakis, Sedgewick 1992) drops randomness entirely and keeps a local
invariant instead:

> between two consecutive level-(L+1) nodes there are **at most 3** level-L nodes.

That bound alone gives worst-case `O(log n)` — no randomness quality required.
The classic algorithm restores it *eagerly on every insert/delete*. The proposal
here restores it **lazily, driven by reads**: writes stay simple (insert at a
random or even fixed low height, don't rebalance), and the invariant is repaired
opportunistically by whoever next reads through a violating region. The structure
*converges* to the deterministic shape under read pressure instead of being held
there at all times.

Crucially, the 1-2-3 invariant is exactly what a descending reader can check
**for free**: as it drops from level L+1 to level L at some predecessor and walks
to the next level-(L+1) node, it is already stepping over those level-L nodes —
counting them costs nothing. `count > 3` at a boundary *is* the defect, whatever
caused it (bad RNG, adversarial order, cluster deletes, flooding). You never need
to observe the *global* height histogram — a reader can't cheaply see it and does
not need to. The defect is local and the repair is local.

## 3. The enabling property: balance is performance, not correctness

This is what makes the whole scheme tractable, and it deserves to be stated
first among the mechanics:

> A mis-balanced skiplist is still a **correct** ordered map — search ordering is
> independent of tower heights. Balance affects only *how fast* a lookup is,
> never *whether it is right*.

So the repair path has an enormous safety margin. It may be:

- **sampled** — only 1-in-N reads bother to check/repair, keeping the common read
  cheap;
- **deferred** — detect inline, enqueue a repair hint, do the mutation off the
  read path (a per-CPU healer queue, or a background drainer);
- **best-effort** — if the promotion txn aborts on conflict, just drop it; a later
  read re-detects the same gap;
- even **occasionally wrong** — an over- or under-promotion only makes some
  lookups slightly slower; it can never corrupt the map or lose a key.

Contrast a structure where repair could violate an invariant (a tree rotation
that must preserve order): there, a racy repair is a correctness bug. Here it is
at worst a missed optimization. "Gradually improve" is therefore not a compromise
— it is the natural, safe operating point.

## 4. Detection — free, on the descent

During `search`/`lookup`, at each level track the run length since the last
higher-level node. Concretely, when the descent leaves level L+1 at predecessor
`P` and walks level L to the next node that also exists at L+1, count the level-L
hops. Carry three cheap registers per level being watched: run start, run length,
and a midpoint candidate (the node at `⌈run/2⌉`). On `run > GAP_MAX` (= 3 for the
1-2-3 band), the boundary is a promotion candidate and the midpoint is the node
to raise. No extra loads, no global state: the reader is walking those nodes
regardless.

The symmetric signal for over-density: a descent that makes 0 hops at level L for
many consecutive levels means those levels are redundant (towers too tall) — a
demotion candidate. Promotion (gap-fill) is the higher-value half; sparsity is
what actually breaks `O(log n)`, while over-density only wastes some work and
space.

## 5. Repair mechanism

**Promote** the midpoint node `X` of an over-long level-L gap to level L+1: link
`X` at L+1 between the bounding level-(L+1) predecessor and successor. Choosing
the midpoint bisects the gap, restoring `≤ GAP_MAX` on both sides — the
deterministic-skiplist repair.

**The one real hazard: changing a node's height concurrently with its own
deletion.** A delete that read `X`'s `toplevel` *before* a promotion bumped it
will not mark/unlink the new top level, leaving a logically-deleted node
reachable on the express lane — corruption. Two ways out, both native to the
engine:

- **Reserved height** (`next[MAX_LEVELS]` on every node): promotion is a narrow
  txn (link at one new level), but `delete` must mark up to the *live* top, so
  `toplevel` must join the conflict set — awkward, since it is a scalar field, not
  a transacted slot. Costs a fixed `MAX_LEVELS` pointers per node regardless of
  actual height.
- **COW-replace-to-promote** (preferred): promotion allocates a taller copy `X'`,
  atomically swaps it for `X` across *all* of `X`'s predecessor slots and links
  the new level, in one composable MCAS; then RCU-frees `X`. Height becomes
  **immutable per identity**, so the mid-flight-`toplevel` race disappears, and a
  concurrent delete of `X` conflicts with the swap on the shared predecessor slots
  — the engine serializes them for free. Wider txn + one reclaim, but correctness
  falls out, and it reuses the same atomic multi-slot swap the list's lock-free
  *replace* already provides. Demotion is the mirror (COW a shorter copy, or
  simply unlink the top level if height stays immutable by convention).

Either way the repair is a *txn*, so it obeys the same optimistic/escalation and
composability rules as any mutation, and its old-value checks make it safe against
every concurrent insert/delete without a single new lemma.

## 6. Where the repair runs

The read that triggers a promotion pays a write cost, so keep it off the hot path:

- **Sample**: check the gap registers on a `1/K` fraction of reads (or only when a
  read's total hop count exceeds a whole-lookup threshold — "this lookup was
  slow, fix something on the way out"). `K` trades convergence speed against read
  latency.
- **Defer**: the natural default. The reader records `{predecessor, level}` hints
  into a per-CPU healer queue and returns; a healer thread (or the next writer
  passing through) drains hints into promotions. Keeps the read path read-only and
  batches repairs.
- **Best-effort inline**: for a low-contention deployment, do the promotion txn
  inline after the read section closes; drop it silently on abort.

All three are correct (per §3); they differ only in latency distribution.

## 7. Emergent properties

- **Traffic-weighted balance (a working-set property for free).** Repair happens
  where reads happen, so hot regions converge to well-indexed while cold regions
  stay lazily unbalanced — which is fine, nobody is paying for their imbalance.
  The structure spends its balancing budget exactly where lookups occur, like a
  splay tree's amortized self-adjustment but without moving keys or hurting
  concurrent readers.
- **Online defense against distribution attacks.** Because balance no longer
  depends on RNG quality or insertion order, an adversary who floods a key range
  gets that range *healed under the read traffic the attack itself generates* —
  the attack pays for its own mitigation. This is the ordered-map sibling of the
  [txn-rehash](rcu-txn-hlist.md) idea (use the engine's atomic restructuring to
  defend against a pathological distribution online); same move, one structure
  over. It also composes with the [txn-vs-existence](txn-vs-existence-3hash.md)
  read/write-tax framing: the healing cost is a *write-side* tax paid lazily by
  readers, only under provocation.

## 8. Knobs and cost

- `GAP_MAX` (promotion threshold): 3 for the textbook 1-2-3 band. The band gives
  built-in **hysteresis** — promote at gap > 3, demote only when a level is
  consistently redundant — so promote/demote do not oscillate.
- Sample rate `1/K` and inline-vs-deferred repair (§6).
- Node footprint: reserved-height (`MAX_LEVELS` pointers/node) vs COW-replace
  (variable height + one reclaim per promotion). COW-replace keeps footprint at
  the randomized structure's average and pays per-repair; reserved-height pays
  standing memory for cheaper repairs. Measure both.

## 9. Open questions and plan

- **Convergence vs. churn.** Under heavy concurrent insert/delete the gaps a
  reader sees are moving targets; repairs are best-effort so this is safe, but the
  *rate* of convergence vs. the rate of disruption needs measuring — is read-healed
  balance a stable equilibrium under write-heavy churn, or only under read-mostly?
- **First-scan cost.** A bulk-insert-then-scan pattern makes the first scan do a
  lot of repair. Batch via the deferred healer, or seed heights at insert to avoid
  a cold, flat structure.
- **Demotion value.** Confirm whether demotion earns its keep or whether
  promotion-only (never shrink) plus occasional full rebuild is simpler and enough.

Prototype plan: build on the committed header. Start read-only detection
(gap registers + a counter of detected violations, no repair) to characterize how
bad a deliberately-poisoned RNG / adversarial insert order actually gets and how
often reads would trigger. Then add deferred COW-replace promotion and measure
lookup-latency convergence under (a) a poisoned RNG, (b) a key-flooding attack,
against the randomized baseline and `cds_lfht`/`std::map`-style references. Pin
down reserved-height vs COW-replace on the same workload. The
detection-only step is worth landing first — it is pure read-side, cannot affect
correctness, and quantifies whether the problem is real for the target workloads
before any mutating machinery is written.
