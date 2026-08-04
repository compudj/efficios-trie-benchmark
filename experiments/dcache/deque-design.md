# A specialized concurrent deque for the LRU

## Why not a list

`rcu-txn-list.h` exports RCU traversal accessors (`urcu_txn_list_next_rcu`,
`urcu_txn_list_prev_rcu`, `urcu_txn_list_empty`, the last documented "CALL
WITHIN AN RCU READ-SIDE SECTION") *and* `urcu_txn_list_move_tail_rcu`. Those two
features cannot be used by the same caller, and nothing says so.

A move is not merely unsafe under RCU traversal, it is **unrepairable** under it.
A traverser standing on the moved node follows its new `next` and silently skips
or repeats an arbitrary span of the list. No barrier or grace period fixes that:
grace periods govern *reclamation*, not logical position, and there is no instant
at which the old `next` becomes safe again. The only RCU-correct relocation is
copy-publish-retire, at the cost of an allocation and a grace period per rotate.

Mainline has the same operation — `dentry_lru_isolate` calls `list_move_tail` —
and what makes it safe is `lru_lock`, i.e. exclusion, not RCU.

So the LRU does not want a list. It wants a structure whose contract says **there
is no read-side traversal**, and which therefore may offer a rotate.

## What the LRU actually does

| operation | caller |
|---|---|
| push at tail | `dc_add_typed` → `lru_add`; `lru_retain` re-arm |
| peek at head | the CLOCK sweeper |
| remove by node identity | `dc_unlink` → `lru_del`; the sweeper on a successful evict |
| rotate head → tail | the sweeper's second chance |
| **traverse** | **never** |

`dc_lookup` touches it zero times. The sweeper re-reads `sentinel.next` every
iteration (`dcache_lru_shrink.h:47`) rather than walking. That is the entire
access pattern.

## The design

### Node

```c
struct urcu_txn_deque_node {
	struct urcu_txn_deque_node *next;	/* transacted slot */
	struct urcu_txn_deque_node *prev;	/* transacted slot */
	struct urcu_txn_deque      *owner;	/* transacted slot; NULL = not queued */
};
```

`owner` is the load-bearing change. It is **pointer-width, so it can be an MCAS
slot**, and it is written *in the same commit* as the link edges. It replaces
`d_lru.shard` and subsumes all three jobs that word was failing to do at once:

- **membership** — `owner != NULL`, and it can no longer desynchronise from the
  links because it is not separately maintained;
- **which deque** — the pointer *is* the shard, so `count` is `owner->count`;
- **exclusion** — the commit is the exclusion, so the OFF/BUSY/ON claim protocol
  disappears entirely. Two concurrent pushes of one node both CAS
  `&n->owner : NULL → D`; one wins, the loser aborts, retries, sees non-NULL and
  reports "already queued".

### The deletion mark disappears

`owner` is the membership witness, so nothing needs to mark `next`. That removes
`urcu_txn_list_is_marked`, the raw-vs-resolved distinction on link slots, and the
"raw for the tag, resolved for the pointer" rule that produced two false-positive
audits in one session.

### No plain stores anywhere

Every slot a node's edges can be written through is a `store_mw` with a validated
expected-old. `insert_before_prepare`'s plain prepare-time writes to
`newp->next`/`newp->prev` — un-CAS'd, not undone on abort — are the mechanism
behind this session's live-lock, and they have no counterpart here.

That gives the invariant the list could not hold:

> For every queued node `n`: `n->prev->next == n` and `n->next->prev == n`, and
> every write to any of those slots is a CAS against the exact prior state.

`prev` therefore stops being a hint. Nothing can leave a node naming a departed
predecessor, so no `prev_repair`, no forward rescan, no stale-hint live-lock.

> The rewire briefly appeared to refute that paragraph — a live ring reaching a
> node with `owner == NULL`. It did not: the node had been **freed while queued**
> by the caller, and the zeroes were the reused storage. `test_deque.c` now
> covers many deques and reuse and the invariant holds. See "⛔ CORRECTION".

### Structure

```c
struct urcu_txn_deque {
	struct urcu_txn_deque_node sentinel;	/* circular; prev = tail, next = head */
	unsigned long              count;	/* APPROXIMATE: sweeper budget only */
};
```

Circular with a sentinel, so there are no NULL cases and interior removal is
O(1). `count` stays a relaxed counter outside the transaction, and is documented
as approximate — it is read only as a scan budget, never as truth. (It cannot be
transacted: it is not pointer-width. Making membership depend on it again is the
mistake this design exists to remove.)

### Operations

**`push_tail(D, n)`** — 5 slots:

```
  &n->owner         : NULL       -> D          (guard: fails if already queued)
  &n->next          : <read>     -> &D->sentinel
  &n->prev          : <read>     -> oldtail
  &oldtail->next    : &sentinel  -> n
  &D->sentinel.prev : oldtail    -> n
```

**`remove(D, n)`** — 3 slots:

```
  &n->owner      : D    -> NULL   (guard: fails if already removed)
  &prev->next    : n    -> next
  &next->prev    : n    -> prev
```

**`rotate_head_to_tail(D)`** — 6 slots, `h = sentinel.next`, `t = sentinel.prev`;
returns immediately if `h == t` or the deque is empty:

```
  &sentinel.next : h          -> h->next
  &h->next->prev : h          -> &sentinel
  &h->next       : h->next    -> &sentinel
  &h->prev       : &sentinel  -> t
  &t->next       : &sentinel  -> h
  &sentinel.prev : t          -> h
```

Note this is **head-to-tail only, not a general move**. That is deliberate and it
is what makes it verifiable: the head's predecessor is *always* the sentinel, so
the three-independent-loads hazard that produced the `next == sent` aliasing bug
in `move_tail_prepare` shrinks to two reads off one object. All six slots are
provably distinct once `h == t` returns early, including the two-element case.

A general `move(n, position)` is deliberately absent. If a caller needs one, the
answer is remove + push, and the caller must own the node across both.

## What still needs RCU, and what does not

Removing traversal does not remove RCU — it moves what RCU protects.

- **Readers**: none. No RCU requirement.
- **Mutators**: `remove` reads `prev` and `next`, then CASes into them. If a
  neighbour's dentry were freed in between, that is a use-after-free. So every
  mutator runs inside `rcu_read_lock()`, and node memory is freed via
  `call_rcu`. **RCU here protects the mutators' neighbour pointers, not readers.**

This is the subtle inversion and belongs in the header's first paragraph.

## What it fixes, by construction

| defect found this session | why it cannot recur |
|---|---|
| shard word desynchronising from membership | `owner` is written in the commit |
| `lru_move_tail` mutating without owning the node | no claim protocol; the commit is the exclusion |
| `lru_del` discarding the BUSY answer | no BUSY state exists |
| same-GP re-add clobbering edges via plain stores | no plain stores; and evict-first keeps the node queued |
| stale link naming a departed node | every edge CAS'd against prior state — and the one apparent counter-example was a dentry freed while queued, see "⛔ CORRECTION" |
| `next == sent` aliasing in a general move | rotate is head-only |

## Costs, stated honestly

- `push_tail` goes from 2 CAS + 2 plain stores to **5 CAS**. That is a real
  regression on the hottest path and must be measured, not assumed away. It is
  the price of the membership word being inside the commit.
- `remove` stays at 3 slots but loses the `-EAGAIN` successor guard and the mark,
  so it should get slightly cheaper.
- `rotate` stays at 6.
- `sizeof` per node: `owner` replaces `unsigned int shard`, so +4 bytes after
  padding on the LRU line. `d_lru.referenced` is unchanged.

## Consequences elsewhere

- `urcu_txn_list_move_tail_prepare` / `_rcu` should be **removed** from
  `rcu-txn-list.h` (reverting `ff667579` and `0291f5ed`). They are the API that
  should not exist on a traversable list, and this deque is where that operation
  belongs.
- The lock arm is unchanged: under exclusion, unlink+link is atomic and none of
  this applies. It stays the honest A/B control.
- ~~The `DC_LRU_READD_LEGACY` control can go once the deque lands, since the shape
  it reproduces will no longer be expressible.~~ **REFUTED BY MEASUREMENT.** The
  deque landed and the shape is still expressible (remove, then push), and it
  still collapses 5/5. The control has earned its keep — but as a reproducer for
  a **caller** bug, not a deque one: it frees a dentry while a deque still points
  at it. See "⛔ CORRECTION" below.

## Open question, and it is not about the deque

The rotate exists only because the sweeper **re-reads the head every iteration**,
so a referenced node must physically leave the head or be re-examined forever. A
CLOCK *hand* — a cursor that advances — would remove the need for rotate
altogether. But a cursor is a traversal, and reintroduces the hazard in a new
form: the hand can be standing on a node that gets removed.

That is a property of the sweeper, not of the deque, and it should be decided
before the deque's API is frozen — if the sweeper ever gets a hand, `rotate`
becomes dead weight and the hand needs its own contract (most likely: the hand is
a *node reference* whose removal transaction is responsible for advancing it,
which is expressible here precisely because `owner` is transacted).

## What the rewire measured

2026-08-04, bucketlock engine, `--writers 8 --readers 8 --evict bursty
--evict-cap 32`, uninstrumented 1s runs.

| arm | completion | Mchurn/s |
|---|---|---|
| deque, evict-first (default) | **5/5** | 2.50 2.53 2.54 2.51 2.50 |
| deque, `-DDC_LRU_READD_LEGACY` | **0/5** (all timed out at 25-30s) | — |

Instrumented (`-DDC_TXN_STATS`), SIGTERM mid-run, `lru_del` row:

| arm | attempts | aborts | escalated | maxretry | disowned nodes |
|---|---|---|---|---|---|
| default (3 trials, ~10M dels each) | 9.5-10.5M | 3.0-64k | **0** | **3** | **0 in every shard** |
| legacy | 57-60M | 57-60M | 57-60M | **57M in ONE call** | a departed node spliced into a live ring |

So the deque did *not* remove the legacy arm's collapse, and the residual is a
**different failure from the list's**, which matters because the two were
confused before:

- On the list, the lane holder was stuck on a CAS that could never land. Here it
  is **moving** — three gdb samples two seconds apart show it in
  `remove_prepare`, then `urcu_txn_end`, then `urcu_txn_sort_recs` — while every
  other worker is parked in `cds_fair_mutex_park` and the call_rcu worker is
  inside `urcu_qsbr_synchronize_rcu()`. That is the escalation-lane starvation
  plus the park-while-online QSBR stall, which is a liburcu matter, not this
  file's.
- It also comes with a ring that reaches a node with `owner == NULL`
  (`dc_lru_validate`: `first-bad=2`, `walked=9 count=13`).

⚠ Read `disowned` as amplified, not as a defect count: a removed node keeps stale
links by design, so one bad edge sends the walk off the live ring for as many
hops as those stale links happen to chain. `first-bad` is the localiser.

## ⛔ CORRECTION: that ring state is NOT a deque defect

Written first as "a remove installed an edge naming an already-departed node,
i.e. a stale read despite `load_validate` and the seq guard". **That was wrong,
and it is the third time this file's history has blamed the structure for a
caller's bug.** The witness that settled it took five minutes and should have
come first.

`-DDC_LRU_FREE_ASSERT` makes `dentry_free_cb` check whether the dentry is still
queued at the instant its storage is released:

| arm | FREE-WHILE-QUEUED |
|---|---|
| default (evict-first) | **0 / 5** |
| `-DDC_LRU_READD_LEGACY` | **3 / 3** |

So a dentry is freed while a deque still points at it. `free()` hands the storage
back, the next dentry to land on it is memset — `owner` NULL, links NULL — and
the neighbours still name it. That *is* the "live ring reaches an `owner == NULL`
node", arriving as a consequence rather than a cause.

Which of the two re-adds does it? `DC_LRU_NO_READD` used to gate both sites at
once, which made every result from it uninterpretable; split into
`DC_LRU_NO_SHRINK_READD` and `DC_LRU_NO_RETAIN_READD`, the answer is unambiguous:

| shrinker put-back | `lru_retain` re-arm | FREE-WHILE-QUEUED |
|---|---|---|
| ON | ON | 3/3 |
| **OFF** | ON | **0/3** |
| ON | **OFF** | 3/3 |
| OFF | OFF | 0/3 |

It is the **shrinker's own put-back**. The window: the sweeper removes the victim
from the deque, and while it holds it by RCU alone a concurrent `dc_unlink` runs
— finds the node already off the deque, so its `lru_del` does nothing — and
`call_rcu`s the dentry. The sweeper's eviction then fails, and it pushes a dentry
that is already pending free back onto a deque.

That is exactly the window **evict-first exists to close**, and closing it is why
the default arm is clean: the victim is never taken off, so `dc_unlink`'s
`lru_del` always finds it and the sweeper never re-adds. The legacy arm is
therefore not merely slower — it is *unsafe for this caller*, independently of
which structure the LRU is built on.

## The deque, tested along both axes

`test_deque.c` now covers what the dcache does and it did not: **many deques**
(nodes migrate; `owner` must name the right one; remove derives its deque from a
hint) and **reuse** (a node is retired and re-initialised in place, resetting
`seq` — the one event that breaks the ABA guard's monotonicity premise).

`make check-deque`, eight arms, all PASS: 2/8/32 writers × 4 deques, 1 deque
(the single-ring regression), 16 deques, the `NO_SEQ_GUARD` mutation, ASan, and a
harness self-check. So **the deque holds on both axes**, which retires the
suspicion above rather than leaving it hanging.

Three things that fell out and are worth keeping:

- **The harness was wrong first.** Its retire waited for a grace period *before*
  removing the node but not *after*, so a rotator that had already read the node
  as the head could read `h->next` out of storage the retirer had just zeroed —
  a NULL deref inside `rotate_head_prepare` that looked precisely like a deque
  defect. Reuse needs the grace period *after* the node is off, which is what
  `call_rcu` gives the dcache for free. 7/8 SEGV before the fix, 16/16 after.
- **The audit masked it.** An O(NNODES) self-check between the removal and the
  re-init made it pass 8/8. It is now opt-in (`-DRETIRE_AUDIT`) and one gate arm
  runs it, because a probe that widens every window is not a control.
- **The `seq` guard is still unproven.** Reuse was the hypothesis that would make
  it load-bearing; compiled out at 32 writers with reuse on, the test still
  passes. Worse, reuse *resets* `seq` to zero, so the guard's stated premise —
  "never decreases" — does not survive recycled storage at all. The guard is
  monotone per membership, not per address.
- **TSAN gates it fine, and every report it made was real.** See below — the
  claim that it could not was wrong twice over.

## ⛔ CORRECTION 2: "TSAN cannot gate this" was wrong

First written as: TSAN cannot model a QSBR grace period, so every
reclaim-vs-reader pair reads as a race — 27 reports with the reuse op, 38 with it
compiled out, therefore noise, therefore no TSAN arm. Two errors stacked:

1. **The TSAN liburcu was four days stale.** `urcu-txn-tsan-build` was a Jul 31
   copy; `rcu-txn-mcas.h` had moved on Aug 3. So those runs exercised a
   *different engine* from every other gate, and no conclusion drawn from them
   was worth anything. (`--enable-compiler-atomic-builtins` *was* set, and TSAN
   *was* instrumenting the atomics — verified by finding `__tsan_atomic64_load`
   in the generated code. The flag was not the problem; the staleness was.)
   The hand-rolled command line also omitted `TSAN_SLAB_CPP`, which the Makefile
   already warns about; harmless here only because both trees happen to build the
   default slab route.
2. **The reports that survived a correct rebuild were all real, and all mine.**
   The control that showed it: `make stress-tsan` on the rebuilt tree is
   **clean**, so the engine is not inherently TSAN-dirty and the reports had to
   be coming from something this test does and the dcache does not.

Sixteen races, and the classification is the whole lesson:

| n | what | verdict |
|---|---|---|
| 11 | `urcu_txn_resolve_record` / `desc_status` vs `desc_commit` / `urcu_txn_add` | **real** — the harness resolved proxies off `CMM_RELAXED` loads |
| 4 | `urcu_txn_read` vs `urcu_txn_deque_node_init` on `g_items` | **real** — same cause, the diagnostic reads |
| 1 | `g_stop` | **real** — `volatile int` is not atomic |

The first fifteen are one mistake: **resolving a proxy dereferences the writer's
descriptor, so the load of the slot must be `CMM_ACQUIRE`.** A relaxed load
carries no happens-before to that descriptor's initialisation — the reader can
see the proxy pointer without seeing the fields it points at. The library's own
accessors (`urcu_txn_deque_owner`, `_head`) get this right; the *diagnostic*
reads in `push_dbg` / `remove_dbg` / the livelock dump did not, precisely because
they are not part of the algorithm and got written casually.

Fixed, the arm is **0 warnings** at 4 and 8 writers × 1, 4 and 16 deques.
`make check-deque-tsan`, which refuses to run if the TSAN tree has drifted from
`urcu-txn-build`.
