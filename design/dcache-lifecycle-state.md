# dcache: a per-dentry lifecycle state machine

Design note. Derived from the four terminal markers the code already enforces,
not invented alongside them.

> **⛔⭐⭐ CORRECTION (step 1 is now implemented; see `experiments/dcache/dcache_state.h`
> and `make check-lifecycle`).** The central claim below — that R5 has no
> witness — **was wrong**, and building it is what showed that. Two corrections,
> both recorded in §2a rather than edited out of §2, because the reasoning that
> produced them is the useful part.

The question that prompted it: the per-structure presence markers live in
separate words that are not transacted by every mutation — is that racy, and
would one state-machine word participating in each TXN be better?

## 1. The markers today, and why they are NOT racy within a structure

Every terminal marker is written **into the slot that that structure's own
insertion must CAS**, and in the **same commit** as the removal. So it is not
side state: it is transacted by exactly the operations that could violate it,
and there is no instant at which the dentry is gone from an index but still
accepting entries.

| structure | membership slot | terminal value | refused by |
|---|---|---|---|
| child hlist | `d_child_head.first` | marked NULL (parent seal) | `insert_head_prepare` → `-ENOENT` |
| name hash | `d_hash.next` bit 1 | deletion mark (`DC_MARK_GEN`) | expected-old at install |
| LRU, lock arm | `d_lru.shard` | `DC_LRU_DEAD` | `lru_retain` (asks OWNED) |
| LRU, MCAS arm | deque node | `URCU_TXN_DEQUE_POISON` | deque insert |

⭐ Each marker is therefore **already in the optimal slot for its own
observer**, and costs zero extra conflict-set entries. A unified word cannot
beat this on cost. It can only beat it on **auditability** — which is the real
argument, see §4.

## 2. Where the gap actually is: ACROSS structures

Nothing makes the *set* of markers atomic with each other. A dentry can be
sealed in the name hash and not yet off the LRU. Global correctness rests on a
hand-proved ordering between structures, with nothing checking it — and that
ordering has been violated twice in this tree, both times found the hard way:

- **re-add inside the unlink's own grace period** (MCAS LRU wedge) — fixed by
  the rule *evict BEFORE unlinking*; a grace period at that point is not
  implementable.
- **`fold()` freed dentries that were still on the LRU** — fixed by taking the
  dentry off the LRU before the free.

Both are the same shape: **an LRU operation racing a namespace teardown**.
That is the invariant with no witness, and it is what a lifecycle state would
add.

## 2a. ⛔ CORRECTIONS from implementing it

**(i) R5 is already witnessed.** `dentry_free_cb()` already calls
`lru_assert_not_queued()` under `-DDC_LRU_FREE_ASSERT`, and `check-lru-arms`
already builds with it. That is the same check §2 proposed to add, in the right
place, already gated. The in-tree comment even records that this engine had no
probe at all until it was added, and that figures taken before it were vacuous
zeros — so the gap was real, and it was closed before this note was written.

**(ii) "Off the LRU before the free" is the wrong phrasing, and the check
belongs post-grace-period.** The first implementation asserted at the
`call_rcu` sites and fired instantly on a healthy tree. That was the CHECK
being wrong, not the code: the shrinker deliberately queues the reclaim FIRST
and drops LRU ownership after, both inside one read-side section — *"which is
the only reason the free cannot have landed before this store"*
(`dcache_lru_shrink.h`). So the rule is:

> **R5: a dentry must be off the LRU before the GRACE PERIOD completes** — not
> before the reclaim is queued.

**(iii) A membership question must be asked in the membership form.** The first
version asked `lru_listed()`, which deliberately answers *"queued OR SEALED"*
because both mean "do not re-arm". A correctly removed-and-sealed node still
answers yes. Membership is `urcu_txn_deque_queued()`, which maps POISON to
NULL. Both `dcache_lru.h` and `rcu-txn-deque.h` say so explicitly; I read past
both. ⚠ It failed in the SAFE direction (over-reporting), which is why one run
caught it — the opposite error would have been a silently vacuous check.

**What survives.** Membership is covered twice over, so the lifecycle word's
remaining value is NOT R5. It is the thing no single marker sees: the lifecycle
itself — illegal EDGES (a resurrection, `DYING -> LIVE`) and reclaim of a
dentry that never passed through the states. That is real but **smaller than
§2 claimed**, and it does not by itself justify putting the word in every
transaction. Step 2 (retiring any marker in its favour) is NOT recommended on
this evidence.

## 3. Layout audit (shipped MCAS config: `DC_MARK_GEN` + `DC_SPLIT_KEEPID` + `DC_LRU_MCAS`)

`sizeof(struct dentry) == 256`, `posix_memalign(64)`.

| line | bytes | contents |
|---|---|---|
| **CL0** | 0–63 | `d_iparent`(8) + `d_iname`(48) + `d_hash.next`(8) — **exactly full, 0 free bytes** |
| CL1 | 64–127 | `d_hash.pprev`, `d_fwd`, `d_back`, `d_parent`, `d_moving`, `d_dc`, `d_child_head`, `d_sib.next` |
| CL2 | 128–191 | `d_sib.pprev`, `d_id`/`d_host`, `d_inode`, `d_isdir`, `d_rcu`, **24-byte hole @168–191** |
| CL3 | 192–255 | `d_lru` (deque node + `referenced`), 24-byte padding @232–255 |

Two results:

- ⭐ **A separate word is FREE.** The 24-byte hole at offset 168 costs no
  growth (`sizeof` stays 256) and sits on a cold line the reader never touches.
  The "it will break the 1-CL reader" objection does not apply — CL0 is exactly
  full, so a new word was never going there anyway.
- **Free low bits.** Dentries are 64-byte aligned, so bits 0–5 are free in any
  pointer to one. Current users:

  | slot | bit 0 | bit 1 | bit 2 | bits 3–5 |
  |---|---|---|---|---|
  | `d_iparent` @0 | `URCU_TXN_TAG` (proxy) | `DC_TAG_SHELL` | `DC_TAG_NEG` | **free** |
  | `d_parent` @88 | `URCU_TXN_TAG` | free | free | free |
  | `d_fwd`/`d_back` | `URCU_TXN_TAG` | free | free | free |

## 4. ⚠ The trap in the obvious host

`d_iparent` looks ideal: transacted (`DC_IPARENT_TXN`), on CL0, **already
loaded into a register by every reader** for the identity compare, 3 free bits.
Liveness would be free to *read*.

But **writes to it dirty the reader's hot line**. The LRU deliberately sits on
its own cacheline (CL3) precisely so that LRU churn does not invalidate CL0 —
and a rotate is frequent. Hosting *LRU membership* in `d_iparent` would undo
that on a read-mostly cache.

⭐ **So split by mutation frequency, not by concern:**

- **Monotone lifecycle** (rare — once or twice per lifetime) → `d_iparent`
  bits 3–5. Dirtying CL0 twice per lifetime is negligible, and the reader gets
  liveness on a word it already holds. This is the same shape as the tags
  already there: `DC_TAG_SHELL`/`DC_TAG_NEG` are documented as
  "stable-or-identity properties", and a monotone state is exactly that.
- **LRU membership** (frequent, non-monotone) → stays exactly where it is on
  CL3. It is not lifecycle; it is a per-shard property with its own word.

## 5. The transition table

Per **lifetime**, not per address — `dentry_alloc()` memsets, so a recycled
allocation restarts at `NEW`. Same rule the existing markers already rely on,
and the reason there is no ABA: the state is **monotone increasing**.

    NEW (0) --publish: hash insert + sib insert----------------> LIVE  (1)
    LIVE(1) --unhash (d_hash del mark) + child-head seal-------> DYING (2)
    DYING(2)--off the LRU, off every index--------------------> DEAD  (3)
    DEAD(3) --call_rcu-------------------------> freed; recycled -> NEW

Enforcement — note these are the rules the code ALREADY implements, each now
with a single witness instead of four:

| rule | today's witness | state-machine form |
|---|---|---|
| R1 no child added to a dying parent | marked `d_child_head.first` | `dc_add` validates parent `== LIVE` |
| R2 no LRU re-arm of a dying entry | `DC_LRU_DEAD` / `lru_retain` asks OWNED | `lru_retain`/`lru_add_at` validate `< DYING` |
| R3 no deque re-insert after removal | `URCU_TXN_DEQUE_POISON` | subsumed by R2 |
| R4 unhash is the walk-causality stamp | `d_hash.next` bit 1 | unchanged — this one is a *version*, not presence; keep it |
| **R5 off-LRU BEFORE unhash/free** | **nothing** | `LIVE→DYING` transition asserts off-LRU |

⭐ **R5 is the whole point.** It is the only rule with no witness today, and it
is the one both historical bugs broke. In state-machine form the re-add race
becomes an illegal `DYING → LIVE` edge and the `fold()` bug becomes a `DEAD`
entry still holding LRU membership — both *abort at the commit* instead of
corrupting silently.

## 6. Cost, honestly

`urcu_txn_validate()` is **not** a read-only guard: it records
`{slot: expected -> expected}`, i.e. a degenerate MW write that goes through
the same install path as a store. So each validating operation pays one record,
and two transactions validating the same slot contend on it.

That contention is **per-dentry**, not global — the `domain->active` funnel is
not a valid analogy. And for the dominant path it is already paid: `dc_add` /
`dc_unlink` under a parent already serialize on `d_child_head.first`, so a
parent-state validate replaces a funnel rather than adding one.

What is genuinely new is contention between operations that today touch
disjoint slots of the same dentry — chiefly **LRU maintenance vs namespace
mutation**, which is exactly the pair R5 governs. That is the number to
measure, and it is a contained A/B on `bench_dcache_churn` with
`--evict continuous`.

Readers pay nothing: they never validate, and under the §4 split they never see
a write to CL0 that they do not see today.

## 7. Order of work

1. ✅ **DONE** — the state word is landed **debug-gated** and cross-checked
   against the markers: `-DDC_LIFECYCLE_STATE`, `make check-lifecycle` (2 LRU
   arms x 2 harnesses + a must-fail control). It immediately corrected three
   things in this note (§2a), which is exactly what step 1 was for.
   ⚠ The control is load-bearing: `-DDC_LC_SELFTEST` drops the to-DEAD
   transition so a reclaimed dentry arrives still LIVE and the run MUST abort.
   Without it a clean pass is indistinguishable from assertions that never ran.
2. ⛔ **NOT recommended on this evidence** — retiring any existing marker in
   its favour. The seals are individually optimal (§1) and membership is
   already witnessed twice (§2a); the residual value is illegal-edge detection,
   which does not pay for a word in every transaction.
3. If it is ever made unconditional, measure the LRU-vs-namespace validate cost
   first — that is the only genuinely new contention (§6).

⚠ Do NOT read a state word with a plain `urcu_txn_load`: it is settled/RYW and
is not exclusion. This tree has already lost a `d_delete` to exactly that
mistake (`store_sw` parked a plain store). A state word read without validation
looks safe and is worse than the in-band seals, because the seals are enforced
by the CAS the peer must do anyway.
