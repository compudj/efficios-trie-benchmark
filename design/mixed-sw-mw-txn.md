# Mixed SW / MW records in one urcu-txn commit

Extend the urcu-txn engine so a **single transaction can carry both SW (single-
writer) and MW (multi-writer) records**, committed atomically against one
linearization point. Today the two are separate engines (`rcu-txn-sw.h` flip-
proxy selector; `rcu-txn.h` / `rcu-mcas.h` install-latched descriptors) with
disjoint commit protocols, so a structural edit that must atomically touch a
*single-writer-owned* slot **and** a *multi-writer* slot has no single commit
that spans both — it must bridge them with an external lock. This note specifies
the unification that removes that bridge. **Design only.** 2026-07-21.

Assumes the current MW engine — **no helping** (the `NO_HELP` + per-record
install-latch/spinlatch work): a contended installer WAITS on the record's
install latch (bounded-blocking); readers/writers landing on an undecided
descriptor **observe** (`resolve`) its committed-or-not state and pick old/new;
nobody drives a foreign txn's commit. That is what makes the unification clean
(below): with no helper competing on the control word, the commit is a single
**owner-exclusive release store**, identical in shape to the SW selector flip.

## 0. Why — the cross-engine atomicity wall

A recurring pattern: an operation owns some slots exclusively (protected by a
lock it already holds) and shares others with concurrent writers, and it needs
all of them to flip **as one atom** so a concurrent observer never sees a torn
pair. Two instances:

- **The dcache chain (the motivating case).** The DLM+SW dcache
  ([dcache-dlm-sw.md](dcache-dlm-sw.md)) commits the hash/child **index**
  single-writer under a per-bucket lock (SW, cheap — the add/unlink win), but the
  host↔shell transition **chain** (`d_fwd`/`d_back`) is touched by concurrent
  `call_rcu` fold workers (multi-writer). Two structural edits must be atomic to a
  *fold*: the **demote** (`top->d_back = shell` ⊕ remove `top` from the index) and
  the **TRANSFER promote** (`m->d_back = NULL` ⊕ replace `n→m` in the index). A
  fold reads the pair `(d_hash mark, d_back)` to choose TRANSFER / SPLICE /
  RECLAIM; a torn pair makes it reclaim a node that is a live indexed top → UAF.
  With SW index and MW chain in *separate* engines there is no commit spanning
  both, so the current engine bridges them with a per-host **chain lock** — an
  8-byte-per-dentry word whose whole job is to be the rendezvous the two writes
  lack. A mixed txn makes the demote/promote *one* commit and retires that lock.

- **The LRU driver (the durable case).** [dcache-dlm-sw.md §0.2] identifies an
  eviction list as the next feature: every add/unlink/lookup re-points 2–4 shared
  LRU-list slots to move the dentry to the LRU head. Those slots are a hot shared
  structure (multi-writer); the object's own index slots are owned (single-
  writer). A mixed txn commits `{ SW index edits, MW LRU-head edits }` in one
  atom — the owned part stays cheap SW, the shared part gets MW concurrency, and
  they linearize together. This is the general shape the primitive is for.

The wall this dissolves: **"no single commit can span two engines."** Make it one
engine with two *record kinds* sharing one control word, and the commit spans
both by construction.

## 1. The mechanism — one control word, two record kinds

A transaction owns a single **commit control word** (`FREE → DONE`, the group
selector / status unified). Every record — SW or MW — references it and is
**resolved identically** by a reader: read the record's `old`, `new`, and
`*control`; return `new` if `DONE`, else `old`. The record kinds differ **only on
the writer side** (install and settle), never on resolve.

```
struct urcu_txn_record {            /* resolve header -- kind-agnostic */
    void        *old;
    void        *new;
    struct urcu_txn_group *group;   /* -> the shared control word */
    /* kind-specific writer-side state follows (install latch for MW, none SW) */
    unsigned int kind;              /* SW | MW */
};
struct urcu_txn_group { unsigned long state; /* FREE|DONE */ struct rcu_head rcu; };
```

Because `{old, new, group}` sit at a common offset and a reader touches only
those, **there is no ambiguous-tag / SIGSEGV hazard from mixing** — the historic
"an SW proxy and an MW descriptor collide on one bit-0 tag" problem was two
*incompatible* structures at one tag; here they share the resolve header, so any
tagged slot resolves uniformly regardless of kind.

### 1.1 Resolve (reader / observer)

```
resolve(slot):
    v = load(slot)
    if !tagged(v): return v                 /* plain value, no record parked */
    r = untag(v)
    return (r->group->state == DONE) ? r->new : r->old
```

Kind-agnostic, wait-free, no helping. This is exactly today's MW `urcu_mcas_read`
(post-`NO_HELP`) and SW `urcu_txn_sw_proxy_get` — merged.

## 2. Lifecycle

```
init(t)                         /* one group/control word, state = FREE */
record(t, slot, old, new, tag, kind)   /* SW or MW, per slot */
...
commit(t):
    install(t)                  /* park SW records; CAS-install MW records */
    if aborted: rollback(t); return ABORT
    release-store t->group->state = DONE     /* THE linearization point */
    settle(t)                   /* write direct new into every slot */
    defer-free(t) after a grace period
```

### 2.1 install — the only place the kinds diverge

- **SW record:** the caller guarantees single-writer exclusion on the slot (it
  holds the relevant lock — e.g. the dcache bucket lock). Park by a **plain store**
  `slot = tag(r)`; it cannot fail and cannot be raced.
- **MW record:** the slot has concurrent writers. Acquire the record's **install
  latch** (bounded-blocking; no helping), then **CAS** `slot: old → tag(r)`. If the
  CAS fails because the slot no longer holds `old`, a concurrent writer committed a
  change → **abort** the whole txn.

Install order does not matter for correctness (nothing is DONE yet, so every slot
still resolves to `old`), but installing MW records first bounds wasted SW parks
on an abort.

### 2.2 commit / settle — owner-exclusive, no helping

`state = FREE → DONE` is driven **only by the owning writer** (no helper competes,
post-`NO_HELP`), so it is a single **release store**, not a CAS — the same shape as
the SW selector flip. That store is the transaction's linearization point: before
it every record resolves `old`, after it every record resolves `new`, for SW and
MW slots alike. `settle` then writes the direct `new` into each slot (plain for an
SW slot; for an MW slot, a CAS `tag(r) → new` or a plain store, since it is
committed and the owner is the sole settler). Settle is cleanup only — a reader
between commit and settle still resolves correctly via the parked record.

### 2.3 abort / rollback

Abort arises **only from the MW side** (an install CAS-old mismatch). Roll back by
un-parking every already-installed record (`slot = old` for SW parks; `tag(r) →
old` CAS for MW installs — safe, still `FREE`), release latches, and retry. A
txn with **no MW records never aborts** — pure add/unlink stay on the cheap SW
path exactly as today.

### 2.4 reclaim

Unchanged: the group block (carrying the record array) is `call_rcu`-freed after a
grace period, so a reader still holding a parked record's pointer is safe. A
single-edge / empty commit owes no grace period (as SW today).

## 3. Correctness invariants

1. **One linearization point per txn** — the single `state` store. All records
   (both kinds) resolve against it, so the commit is atomic across kinds.
2. **SW slots require caller exclusion** — the embedder holds a lock over every SW
   slot from before install through settle (single-writer, no CAS). MW slots need
   no such lock (install latch + CAS-old handle concurrency).
3. **Abort is MW-only.** SW parks never fail. So progress/retry logic is needed
   only for txns that carry MW records.
4. **No helping.** The owner drives install/commit/settle end to end; peers on an
   MW slot wait on its install latch (bounded-blocking) or observe its state.
   Bounded-blocking overall, consistent with the DLM lock model.
5. **Resolve is kind-agnostic and never drives.** A reader only reads
   `{old, new, *state}` — so mixing kinds in one structure is safe by construction.

## 4. Application 1 — retire the dcache chain lock

In `dcache_dlm.c`, make the chain (`d_fwd`/`d_back`) **MW records** and the index
(`d_hash`/`d_sib`, reparent) **SW records**, committed in one mixed txn under the
bucket lock:

- **demote** = `{ SW: remove top from index; MW: top->d_back = shell }` — one commit.
- **TRANSFER promote** = `{ SW: replace n→m in index; MW: m->d_back = NULL }` — one commit.
- **SPLICE / RECLAIM** = MW-only chain edits (CAS-old serializes adjacent folds,
  as the all-MW fold does today).

A fold reads `(d_hash mark, d_back)` and resolves both against the txn's one
control word → it can never see the torn pair, so the promote-vs-`m`'s-fold
reclaim race (the thing that sank the "MCAS just the chain" hybrid — a bucketless
SPLICE had no lock to rendezvous on) is gone: the rendezvous is now the txn's
linearization point, which the bucketless splice observes for free.

Result: **the per-host chain lock and its `d_chain_lock` word are retired** (−8 B
/dentry, back under the seqlock baseline's footprint), the chain gets MW
concurrency (lock-free splices), and **add/unlink stay SW-only** (no MW records →
no descriptors, no abort — the cheap two-CAS path is untouched). The **bucket
lock stays** — the SW index records still need single-writer exclusion, which
add/unlink need regardless.

**Honest perf scope:** the [rename A/B](../figures/perf_dcache_dlm_writepath.txt
and this session's runs) showed the chain was **not** the rename bottleneck — the
multi-head **index** bucket-locking is, and the shared bucket head cannot go MW
without losing the add/unlink win. So for the dcache this buys the 8 bytes +
lock-free splices, **not** the rename-vs-all-MW gap. The bigger prize is §5.

## 5. Application 2 — the LRU driver (the general win)

An LRU eviction list is the canonical mixed op: add/unlink/lookup re-point 2–4
**shared** LRU-list slots (MW) while the object's **owned** index slots go SW,
atomic in one commit. Under the all-MW engine every one of those is a descriptor
(O(records) CAS, and the LRU head is the hot-shared worst case); under a mixed
txn the owned part is plain SW and only the genuinely-shared LRU slots are MW —
`O(shared slots)` descriptors + `O(owned)` plain, one linearization. This is the
"commit cost flat in transaction size, MW only where truly shared" goal of
[dcache-dlm-sw.md §0.2], made expressible.

## 6. Open questions to settle in the engine tree

1. **Record layout.** Confirm a shared `{old, new, *group}` header can front both
   the SW proxy and the MW descriptor without disturbing the MW install-latch /
   inline-record-arena assumptions (the arena's 16-byte stride; the latch/state
   offsets). If the MW descriptor's writer-side state must stay where it is, put
   the resolve header at a fixed offset both kinds honor.
2. **Tag bits.** One tag bit still marks "a record is parked here"; the *kind* is
   read from the record (not the tag), so no second bit is needed for resolve.
   Verify no kind needs a distinct tag for the writer side.
3. **API surface.** `record(t, slot, old, new, tag, kind)` with `kind ∈ {SW, MW}`,
   or two record calls (`urcu_txn_record_sw` / `_mw`) on one handle. The commit /
   abort / settle path is shared. Keep pure-SW and pure-MW callers on their exact
   current fast paths (an all-SW txn must not pay any MW machinery, and vice
   versa).
4. **Abort/retry contract for embedders.** Only mixed / MW-bearing txns abort;
   document that an SW-only commit is abort-free, so add/unlink keep the
   fail-once-at-alloc SW model.
5. **Slab.** One group block per txn carries the mixed record array; confirm the
   per-CPU size-classed slab ([mcas-descriptor-slab]) serves both kinds' records
   from one block.

## 7. Validation plan

- Unit TAP: a mixed txn with N SW + M MW records; a concurrent observer thread
  asserts it only ever reads all-old or all-new (never a torn subset) across the
  commit — the atomicity property, directly.
- MW-abort: force an MW install CAS-old mismatch under contention; assert clean
  rollback of the co-parked SW records and correct retry.
- Regression: an all-SW txn and an all-MW txn must be byte-for-byte the current
  fast paths (perf + TAP), proving the mix adds nothing to the pure cases.
- Then rewire the dcache chain (§4) onto it behind a flag and re-run the full
  `check-dlm` + `check-dlm-tsan` gate and the rename / churn A/B.

Related: [dcache-dlm-sw.md](dcache-dlm-sw.md), [ft-inplace-under-dlm-sw.md](ft-inplace-under-dlm-sw.md).
