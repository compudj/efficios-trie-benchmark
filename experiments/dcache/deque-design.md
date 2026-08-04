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
| stale `prev` naming a departed node | every edge CAS'd against prior state |
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
- The `DC_LRU_READD_LEGACY` control can go once the deque lands, since the shape
  it reproduces will no longer be expressible.

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
