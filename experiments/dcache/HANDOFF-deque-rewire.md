# Handoff — free-while-queued closed on all four arms, and PROVEN.

2026-08-04. Written current-state-first, as the previous cut was: what is true
now, then what is still open, then the retractions in one place at the end.

The LOCK arm's free-while-queued defect — the one open item the last handoff
left — is fixed and mutation-tested, and the stale-`d_parent` use-after-free
that came out of that work is root-caused and fixed on bucketlock.

The MCAS arm's residual is now closed too, and unlike the earlier claim it is
MUTATION-PROVEN rather than sampled: at natural timings the defect is ~2 in 64
runs, so a 40-trial mutation test came back 0/40 on the fix AND on the control
and proved nothing. A targeted reproducer settles it — see below.

Commits, all UNPUSHED, oldest first:

    d4faf6b  move the MCAS LRU onto the deque
    f6eb10c  the legacy LRU collapse is a CALLER bug, and the deque is clean
    c55a11d  RETRACT "TSAN cannot gate the deque test" -- it can, and found 16
    fffc94a  the free-while-queued defect is PORT-WIDE, not the deque's
    961462b  restore retain_dentry's d_unhashed guard (MCAS arm closed)
    7b43b89  consolidate the handoff around the current position
    9f62bc7  close the LOCK arm: shrink-list handoff + DEAD seal
    cc6778f  root-cause the stale-d_parent UAF: a guard pair on the parent
    3e72f00  close the MCAS arm: no re-arm where the guard cannot be transacted
    (this)   use the deque seal; bucketlock keeps its re-arm
             (liburcu side: urcu-txn-dev d9dd1a9e)
    (this)   fold(): take the dentry off the LRU before freeing it

---

## ▶ WHAT LANDED: the LOCK arm needed TWO guards, not one

`-DDC_LRU_FREE_ASSERT`, `--writers 48 --readers 48 --duration 3000
--evict-cap 32`, 5 trials, FREE-WHILE-QUEUED occurrences:

| arm | bursty before | bursty after | continuous before | continuous after |
|---|---|---|---|---|
| bucketlock LOCK | **5/5** | **0/6** | **5/5** | **0/6** |
| txn LOCK | (no probe) | **0/6** | (no probe) | **0/6** |
| bucketlock MCAS | 0/5 | 0/8 | ⚠ 0/5 was WRONG (~2 in 64) | **0/8** |
| txn MCAS | (no probe) | 0/8 | (no probe) | 0/8 |

⚠ The bucketlock MCAS "before" numbers are what this work proved unreliable: the
true rate is ~2 in 64, so five-trial batches read it as clean for two handoffs.
**Five trials cannot call an arm clean** — and at that rate neither can forty;
see the targeted reproducer below.

### Guard 1 — the shrink-list handoff (closes `--evict bursty`)

Mainline's `dentry_lru_isolate` does not disown its victim:
`d_lru_shrink_move()` takes it off the shared list onto a PRIVATE one with
`DCACHE_LRU_LIST` still set and `DCACHE_SHRINK_LIST` added; `__dentry_kill`
honours that by skipping `d_lru_del` and leaving `can_free = false`, handing the
free to `shrink_dentry_list`. **Isolation and ownership are the same act
there.** This port isolated by UNLINKING, so the victim was ownerless for the
whole eviction and a concurrent `dc_unlink` could unhash it, find nothing on the
LRU, `call_rcu` it, and let the put-back relink a dentry queued for reclaim.

Ported as two state bits on the existing shard word (`DC_LRU_SHRINK_BIT`,
`DC_LRU_KILL_BIT`) plus `lru_del_can_free(dc, d, freeing)` — the `can_free`
transfer — which both engines' `dc_unlink` now honours.

### Guard 2 — the `DC_LRU_DEAD` seal (closes `--evict continuous`)

⭐⭐ **THE TWO SIDES OF THIS RACE DO NOT SHARE A SHARD LOCK.** `lru_add`
enqueues on the CALLER'S shard; a killer locks the shard the dentry is
CURRENTLY on — different locks the moment a dentry migrates, and NO lock at all
when the killer finds the word already OFF and returns early. So no shard-lock
discipline can order an unhash against a concurrent enqueue, and `lru_add`'s
"still hashed" test is a plain read of a bucket-locked slot it does not hold the
bucket lock for: it NARROWS and never closes.

Fixed by making the word itself the exclusion — every transition out of OFF is
an RMW on that one word:

* `lru_add` — `cmpxchg OFF -> ON(j)`, **taken under shard j's lock** so the
  claim is never observable before the links exist (claiming outside the lock is
  the "claimed but not linked" defect the MCAS arm already paid for);
* a killer — `cmpxchg OFF -> DEAD`, terminal, so no enqueue can follow the free.

Exactly one CAS wins. If the adder wins, the killer reads `ON(j)`, blocks on
shard j's lock until the link is complete, and splices it out.

⚠ **A killer must never let the word pass through OFF on its way to DEAD** — an
adder holding a DIFFERENT shard's lock would see the transient and claim it.
That is why `lru_unlink_locked_to()` takes its successor state as an argument,
and why `lru_rotate_locked` and `lru_shrink_move_locked` were changed too (a
rotate dipping to OFF could put one node in two lists).

### The mutation matrix — both guards are load-bearing, on different cadences

bucketlock LOCK, 5 trials per cell:

| build | bursty | continuous | reads as |
|---|---|---|---|
| as landed | 0/5 | 0/5 | closed |
| `-DDC_LRU_NO_DEAD_SEAL` | 0/5 | **5/5** | the seal closes *continuous* |
| `-DDC_LRU_NO_SHRINK_OWN` | **5/5** | **5/5** | the ownership closes *bursty* |
| `-DDC_LRU_NO_SHRINK_READD` | 0/5 | 0/5 | the put-back was NEVER the cause |

That last row is the one worth keeping: removing the shrinker's put-back — the
"obvious" fix, and the shape the MCAS arm's evict-first took — changes NOTHING.
What fixed it was giving the victim an owner, not taking away the re-add.

---

## ⚠⚠ CORRECTION: two rows of the previous table were VACUOUS ZEROS

The last handoff reported "txn MCAS deque 0/5" and "txn LOCK 0/5, 0 hits".
**`-DDC_LRU_FREE_ASSERT` was never in `dcache_txn.c`** — its `dentry_free_cb`
was a bare `free()`. The probe existed only in `dcache_bucketlock.c`, so those
rows recorded that a probe which CANNOT FIRE did not fire. (`git log -S` on the
macro shows it never touched that file.)

Fixed structurally, not locally: the probe is now `lru_assert_not_queued()` in
`dcache_lru.h`, called from both engines' `dentry_free_cb`, so the divergence is
no longer expressible. With it live, the txn LOCK arm fired **5/5 under
`--evict continuous`** before the fix — i.e. it was never clean.

⭐⭐ That is the **fourth** dead-probe wrong-negative this project has recorded
(after `dc_readdir` fn==NULL twice and the populated-dir rule). THE RULE:
**verify a probe is live before trusting its zero** — print on the first call,
or run a control you know must fail.

---

## ✅ RESOLVED (was open item 1) — the stale-`d_parent` use-after-free

`lru_evict_settled` derives `parent = parent_of_rcu(d)` and locks
`&parent->d_child_head` — on a parent that has been freed. Root-caused and
**fixed on bucketlock**; on the txn engine the fix is implemented but OFF by
default (see below).

**ROOT CAUSE: `dc_add` publishes a child under a parent the shrinker is
evicting.** `lru_evict_settled(p)` checks `children_empty(p)` while holding
`&parent_of(p)->d_child_head`; `dc_add` of a child under `p` holds
`&p->d_child_head`. ⚠⚠ **THOSE ARE DIFFERENT LOCKS.** Reading them as the same
lock is exactly what hid this — it makes the exclusion look complete when
neither half exists. So the emptiness test was a check-then-act, the add landed
after it, and the child ended up hashed, on the LRU, naming a dentry one grace
period from release.

Fixed as a GUARD PAIR, the same shape `dc_set_negative_txn` already uses for
rmdir-to-negative, and **both halves are needed** — one alone is
one-directional:

* `lru_evict_settled` now locks THREE heads (`bl_lock_n`, address-ordered and
  de-duplicating, so no new deadlock edge): the bucket, the parent's child head,
  and **@d's own child head**. An add that got there first now blocks it and is
  seen.
* `dc_add` re-checks that a **settled** parent is still hashed, and answers
  -ENOENT ("the prefix went") if not — which callers already handle.

⚠⚠ **THE `dc_add` CHECK MUST BE GATED ON SETTLED-NESS.** A host with a shell
stacked above it is legitimately absent from the index — the shell carries the
entry — so its own `d_hash` reads MARKED while the directory is alive. Testing
unconditionally rejected every add under a renamed directory: `test_dcache`
"name recreated over a moved directory", 8 failures, on BOTH engines. A chained
parent needs no test anyway, since `lru_evict_settled` bails on `d_back`/`d_fwd`.

Measured, TSAN `--evict continuous`, 8 runs, runs containing a UAF:

| tree | bucketlock | txn |
|---|---|---|
| HEAD | 6/8 | — |
| + shrink-list & DEAD seal (`9f62bc7`) | 4/8 | 1/8 |
| + a "don't relink a detached victim" guard | 3/8 | — |
| **+ the guard pair (shipped)** | **0/8** | 2/8 (guard off) |

⛔ **ASan DOES NOT COVER THIS.** 24 ASan runs across all four arms at the
headline config found nothing while TSAN found it 4-6 times in 8. The churn
recycles the parent's 256-byte region before the sweeper's write lands, so ASan
sees a legal write to validly-allocated memory. ASan's clean sweep IS good
evidence for the free-while-queued defect (which writes the victim right after
its own free — HEAD 10/10, fixed tree 0/24); it is NOT evidence here. **Match
the detector to the defect.**

⛔ **TRIED AND MEASURED TO CHANGE NOTHING:** refusing the shrinker's put-back
when the victim is no longer hashed (`retain_dentry`'s `d_unhashed` test applied
to the put-back, which has no liveness test at all). 4/8 → 3/8, i.e. noise; and
once the real cause was fixed its mutation arm measured 0/8 either way. Removed
rather than kept — it carried a comment asserting a mechanism the measurement
refuted.

### ⛔ The txn half is OFF by default: `-DDC_TXN_PARENT_GUARD`

It is correct and it works (1/8 → 0/8 with both halves). It costs **liveness**.
Both halves add read-set entries to hot paths, and the extra conflict feeds the
escalation lane, which on this engine is an ABSORBING state (open item 3).
`--evict bursty`, 48w/48r, 6 trials, runs not finishing in 120s:

| build | timeouts |
|---|---|
| default (guard off) | 0/6 |
| both halves on | 2/6, and 4/6 on a second batch |
| add-side half only | 4/6 |

So it is not a matter of choosing the cheaper half. Against that cost the defect
is 1-2/8 here versus 6/8 on bucketlock, where the fix is a third bucket lock and
costs no liveness at all. Kept as a build arm so it can be re-measured once the
park-while-online defect is fixed in liburcu, which is where that cost lives.

### TSAN residue (unchanged, not correctness)

Total warnings dropped HEAD 119 → 82 over 3 runs, and the races on the state
word itself (`lru_listed`, `lru_del`, `lru_link_tail_locked`) are **gone**. What
remains is one pre-existing shape: fields written plainly under the shard lock
and read atomically without it — `sh->count` (`dc_lru_count`) and
`d_lru.referenced` (`lru_retain` vs the shrinker's clear). Both deliberately
approximate. Worth a `uatomic_*` pass for cleanliness; neither is a bug.

## ✅ RESOLVED — the MCAS arm, and then PROPERLY, with a deque seal

`lru_retain`'s re-arm was the last pusher. `lru_push_prepare`'s guard must be in
the SAME COMMIT as the edges, and on **bucketlock it cannot be**:
`bl_hlist_del_locked` plain-stores `d_hash.next`, so an MCAS proxy there would
be clobbered and the settle would resurrect a deleted node. The witness said so
directly — `owner == next == prev`, the victim was the SOLE element of its
shard, freshly pushed.

⚠⚠ **NATURAL TIMINGS COULD NOT GATE THIS.** The rate is ~2 in 64 runs, and a
40-trial mutation test returned **0/40 for the fix AND 0/40 for the control**.
Settled instead with `-DDC_LRU_PUSH_DELAY`, a targeted REPRODUCER that widens
the read→commit window. ⚠ A reproducer argues ONE way only: firing proves the
race and identifies the closing build; not firing says nothing about shipped
timings.

`3e72f00` fixed it by DROPPING the re-arm where the engine cannot transact the
guard (`DC_LRU_ALIVE_TRANSACTED`). **That is now the fallback, not the fix.**

### ⭐⭐ The real fix: `URCU_TXN_DEQUE_POISON` (liburcu `d9dd1a9e`)

`owner` gave push and remove a shared exclusion point but could not say "and
never again", because its free value is NULL and NULL is what a push wants. It
now takes a terminal third value:

    urcu_txn_deque_remove_seal_prepare()   owner : d    -> POISON
    urcu_txn_deque_seal_prepare()          owner : NULL -> POISON

`remove_seal` is the SAME three-slot commit as `remove` with one different
expected-new — not a second operation layered on it, because the instant between
a remove and a separate seal is exactly the window being closed. `push_tail`
then answers `-ESTALE` (deliberately not `-EEXIST`: one means try later, the
other means never). This is the deque's analogue of the LOCK arm's terminal
`DC_LRU_DEAD`.

With it, `lru_del_can_free(dc, d, 1)` removes-and-seals in one commit, the
shrinker's post-eviction removal seals too, and **both engines keep
`lru_retain`'s re-arm**. `-DDC_LRU_PUSH_DELAY`, bucketlock + MCAS, re-arm
ACTIVE, 10 runs:

| build | fires |
|---|---|
| `-DDC_LRU_NO_DEQUE_SEAL` (the A/B arm) | **10/10** |
| seal on (shipped) | **0/10** |

⚠⚠ **`owner != NULL` NO LONGER MEANS "QUEUED"** — it means "queued OR sealed".
Use `urcu_txn_deque_queued()` for membership; `while (owner(n)) remove(n);`
spins for ever on a sealed node. This bit immediately and instructively: the
FREE_ASSERT probe itself asked `owner() != NULL`, so it fired 10/10 the moment
the seal landed — **which looked exactly like the seal having failed rather than
the probe asking the wrong question.** A sealed node being freed is the
CORRECT state.

The seal is terminal PER LIFETIME, not per address: `node_init` clears it, which
is both correct and the only way to reuse a node — the same rule `seq` has.

### The deque test proves the seal replaces a grace period

`test_deque`'s retire path needed TWO grace periods, the first purely to drain
would-be pushers — the same window. `-DDEQUE_SEAL_RETIRE` drops it and seals
instead: PASS at 2/8/32 writers, ~100k retire/reuse cycles each. ⭐ And the
CONTROL is what makes that meaningful: `-DDEQUE_NO_GP1` drops the same grace
period WITHOUT sealing and **does not complete** (unbounded `REMOVE ABORT`). Both
are gates. Had the control passed, the seal arm would have proved nothing.

Shipped default, `-DDC_LRU_FREE_ASSERT`, 48w/48r, 8 trials, ALL FOUR ARMS x BOTH
cadences: **0/8 everywhere.**

## ✅ RESOLVED (was open item 2) — fold() freed dentries that were on the LRU

Not a race: an unguarded path. `fold()` frees `host_to_free` and `n` via
`call_rcu(dentry_free_cb)` and had **no `lru_del` on any path**, while
`resolve()` marks recency on the HOST (`txn_child_lookup_rcu` ends
`return host_of_rcu(top)`) — so hosts ARE on the LRU, and those two frees are
hosts.

It survived because **no test ran renames and the shrinker together**:
`stress_dcache` never called `dc_shrink`, and `bench_dcache_churn` — the only
thing that drives the LRU — has no renames. `-DSTRESS_SHRINK` adds a sweeper to
`stress_dcache` and the probe fired **3/3 immediately, on both engines**, with
`chain-reachable=1` (the dentry genuinely spliced into the shard chain).

Fixed by routing all four fold free sites through `lru_del_can_free(dc, X, 1)` —
the same kill the unlink and the shrinker use, which seals `DC_LRU_DEAD` on the
lock arm and remove-seals the deque node on the MCAS arm. bucketlock lock arm
3/3 → **0/10**, MCAS → **0/6**; churn unaffected (0/4 both engines).

⚠ **The first cut of the harness measured almost nothing** and looked fine: the
sweeper emptied the 40-object namespace within a few hundred iterations
(`evicted=37`) and the remaining ~80000 writer iterations were all `-ENOENT`
no-ops. It still fired — but on a workload that had stopped renaming. Re-seeding
on `-ENOENT` took it to ~20000 evictions per run. **Check that a new arm is
doing work before believing either its pass or its failure.**

⚠ The shrink arm makes NO conservation claim — the sweeper legitimately evicts
leaves, so "every leaf exactly once" is not the invariant. It checks the probe,
duplicate-free-ness, and that anything still present is RIGHT. Run both arms.

## ⛔ RETRACTED — "park-while-online needs fixing in liburcu"

**IT IS ALREADY FIXED, and has been.** `urcu_txn__enter_fallback()` brackets the
lane wait with `thread_offline()` / `thread_online()` (urcu-txn-dev `dcf1310c`,
"rcu-txn: take the escalation lane's blocking wait offline"). Grace periods DO
advance while writers are queued on the escalation lane. Every handoff that
carried "the fix belongs there: go offline across the park" as an open item was
repeating a stale note — including this file, two commits ago.

### And the OOM it was blamed for was THE TEST HARNESS

`stress_dcache -DSTRESS_SHRINK` grew ~1 GB/s and killed a session. The samples
that looked damning — five threads in `cds_fair_mutex_park` at RSS 7.2 → 9.8 →
12.3 GB — were read as cause when they were CONSEQUENCE. Sampling all seven
threads instead of grepping for the symbol I expected showed the actual grower:

    urcu_txn_create <- urcu_txn__record <- urcu_txn_hlist_del_prepare
      <- lru_evict_settled <- lru_shrink_range <- dc_shrink <- shrinker()

My sweeper thread had **no cadence**. It called `dc_shrink` as fast as the CPU
allowed, and on the txn engine every eviction ATTEMPT — including each one that
fails because the entry is mid-rename — opens a transaction and allocates a
descriptor. Those are freed via `call_rcu`, so a loop with no pause allocates
faster than grace periods reclaim. No real shrinker looks like that:
`bench_dcache_churn`'s continuous evictor does ONE `dc_shrink_local(.., 1)` per
writer op and its bursty evictor sleeps between passes.

A 200 µs pause (offline across it) fixes it completely: **txn 3/3 PASS,
bucketlock 3/3, zero OOM-kills under a 4 GB cap**, and the arm still catches the
defect it was built for — **4/4 on both engines with the fold fix reverted**. So
the throttle bought safety without costing detection, and all four arms are gates.

⭐⭐ **TWO METHOD FAILURES WORTH KEEPING.** First: I grepped the backtrace for
`cds_fair_mutex_park`, found it, and stopped — confirming a prior instead of
testing it. The full thread dump was two minutes away and said something else.
Second: an inherited "open item" was never re-checked against the code; one
`git log -S` would have retired it at any point in the last several sessions.

⚠ Still cap memory when writing new sweeper arms — `scratchpad/capped` wraps a
run in `systemd-run --user --scope -p MemoryMax=… -p MemorySwapMax=0`. The
lesson is not "the txn engine is dangerous", it is "an unthrottled allocator
loop is", and that is easy to write again.

### Previously recorded under this item, and NOT retracted

### Previously recorded under this item



⚠ **CORRECTION to the previous handoff, which said "hangs 3/3".** Re-measured 8
trials on a clean HEAD control: **5 pass / 3 CONSERVATION FAILED / 0 hangs**.
This tree over the same 8: 5 pass / 2 conservation / 1 timeout. Indistinguishable
— the LRU change does not touch it. The earlier "3/3 hangs" was a small sample
of a stochastic outcome reported as if deterministic.

The escalation-lane analysis still stands and is a **liburcu** matter:
`urcu_txn__enter_fallback()` → `cds_fair_mutex_park()` blocks on a futex while
the thread is still RCU-online, which holds off every grace period, which stalls
`call_rcu` reclaim, which deepens the contention that caused the escalation.
Self-reinforcing and absorbing. The fix belongs there: go offline across the
park.

---

## The state word (LOCK arm) — one word, every transition under the shard lock

    DC_LRU_OFF         ownerless
    DC_LRU_DEAD        terminal: unhashed and queued for reclaim; never link it
    DC_LRU_ON(i)       linked into shard i          -- DCACHE_LRU_LIST
    | DC_LRU_SHRINK_BIT   off the list, shrinker i holds it -- DCACHE_SHRINK_LIST
    | DC_LRU_KILL_BIT     ... and a killer handed it the free  -- can_free=false

`lru_listed()` asks **OWNED** (linked OR shrink-held), because that is the
`DCACHE_LRU_LIST` question and so the one `lru_retain` must ask — a victim under
eviction must not be re-armed. `DC_LRU_IS_LINKED` is the different question
"is it actually spliced in", and conflating the two re-disowns the victim.

A recycled allocation starts from zero (`memset` in `dentry_alloc`), i.e. OFF,
so DEAD is terminal per LIFETIME, not per address.

## What landed earlier (unchanged)

The MCAS LRU runs on `<urcu/rcu-txn-deque.h>`; `d_lru` there is a
`urcu_txn_deque_node dnode` and nothing else. Claim protocol deleted;
membership is `urcu_txn_deque_owner()`. `lru_move_tail` → `rotate_head` (its
"protect the traverser" rationale was VACUOUS — this port has no LRU traversal).
Two sweepers can reach `lru_evict_settled()` for one victim: safe, but it MOVES
THE SERIALIZATION POINT to the eviction's re-verify.

⛔ **DO NOT PORT EVICT-FIRST TO THE LOCK ARM** — tried, measured, reverted: no
improvement plus 5/5 SEGV in `urcu_txn_install_mw_depth`. On MCAS it is safe
because `remove()`'s `&n->owner : q -> NULL` makes exactly ONE sweeper win —
**the deque supplies ownership**. The lock arm got ownership from ISOLATION, and
evict-first removes exactly that. The shrink state is what replaces it, and that
is why `lru_del_can_free` on the MCAS arm is a one-liner that always answers 1.

Two upstream fixes that stay: `750572af` (`urcu_txn_list_del_prepare`
load-validates its derivation read) and `b69b4a53` (all five list convenience
brackets leaked the escalation lane on `-ENOENT`). ⭐ The rule behind both: **an
operation that READS a slot it does not WRITE must validate that read.**

## Deque status

`test_deque.c` covers **many deques** (migration, remove-via-hint) and **reuse**
(retire + re-init in place). `make check-deque` 8 arms PASS.
⚠ **`seq` ABA guard STILL UNPROVEN** — `NO_SEQ_GUARD` passes at 32 writers WITH
reuse; and reuse **resets seq to 0**, so "never decreases" fails for recycled
storage: monotone **per membership, not per address**.
⚠ Reuse needs a GP **after** the node is off, not just before; missing it looked
exactly like a deque defect.

---

## ⛔ Claims retracted — do not resurrect

**This session:**

- "txn MCAS 0/5" and "txn LOCK 0/5, 0 hits" — the probe was not in that file.
- "txn + MCAS + continuous hangs 3/3" — it does not hang; it intermittently
  fails conservation, on HEAD as much as here.
- "the shrinker's put-back is the pusher, so removing it is the fix" —
  `-DDC_LRU_NO_SHRINK_READD` changes NOTHING. The ownership was the fix.
- "the shard lock orders `lru_add` against `dc_unlink`" — it does not; the two
  sides pick DIFFERENT shards, and a killer that finds OFF takes no lock at all.

**Earlier:** "the `owner == NULL` node in the ring is a deque defect" (caller
bug); "evict-first fixes it" / "the default arm is clean" (bursty-only, stated
unqualified); "TSAN cannot gate the deque test" (it can — 16 real defects);
"evict-first ports to the lock arm" (SEGV); and, before the rewire, the
stale-prev forward rescan, the six-edge `move_tail` as cause, word-OFF-while-
linked, "plain stores exonerated" (a TIMEOUT counted as THE WEDGE).

## Method rules earned the hard way

- **Verify a probe is live before trusting its zero.** Four wrong negatives now.
- **Match the detector to the defect, and never read one tool's silence as
  coverage for another's finding.** ASan was clean 24/24 on a defect TSAN hit
  4-6 times in 8, because the allocator recycled the region first.
- **Five trials is not enough to call an arm clean** — and neither is forty at a
  low enough rate. The MCAS residual is ~2 in 64: it read as 0/5 for two
  handoffs, and a 40-trial mutation test returned 0/40 for the fix AND 0/40 for
  the control, i.e. the arm that had to fail passed.
- **When natural timings cannot gate a race, BUILD A REPRODUCER** that widens
  the specific window (`-DDC_LRU_PUSH_DELAY`), and use it only to argue one way:
  firing proves the race and identifies the closing build; not firing says
  nothing about shipped timings.
- **When two locks are spelled alike, check they are the same object.**
  `&parent->d_child_head` in the evictor and in dc_add are DIFFERENT dentries'
  heads; assuming otherwise made a missing exclusion look complete.
- **Ask the cheap decisive question first.** `-DDC_LRU_NO_RETAIN_READD` took
  five minutes and killed the leading hypothesis (still 5/5), which is what
  redirected the whole session.
- **READ THE WITNESS, do not reason from the design.** The `owner=0x10 ...
  chain-reachable=0` line said "freed while LINKED on shard 14", which is what
  exposed guard 2. Two rounds of careful argument had concluded guard 1 was
  sufficient.
- **Mutation-test every new guard**, and keep the mutation knob
  (`NO_DEAD_SEAL`, `NO_SHRINK_OWN`). A guard whose removal changes nothing is
  not a fix.
- **Run the control under the SAME methodology.** Every "pre-existing" claim
  here is against a HEAD tree built from `git archive` and run identically.
- **Sweep the KNOB, not just the thread count.** Guard 1 alone looked like a
  complete fix under `--evict bursty`.
- **One macro must not gate two sites** (`DC_LRU_NO_READD` gated both re-adds).
- **A probe that widens every window is not a control** (`-DRETIRE_AUDIT`).

## Tree hygiene — both of these have cost real time

⚠ **`urcu-txn-tsan-build` is a COPY of `urcu-txn-build` and silently rots.**
Verified in sync this session (md5 `20b17e74`). Check before any TSAN work:

    md5sum ../../urcu-txn-tsan-build/include/urcu/rcu-txn-mcas.h \
           ../../urcu-txn-build/include/urcu/rcu-txn-mcas.h

Verify instrumentation in the GENERATED CODE (`cc -S -fsanitize=thread … |
grep -c __tsan_atomic`, expect ~467 for `dcache_bucketlock.c`), not in
`config.h`. Pass `TSAN_SLAB_CPP`, not `SLAB_CPP` (currently empty — neither
`URCU_TXN_SLAB_BATCH` nor `URCU_SLAB_RSEQ` is set in that tree).

⚠ **`urcu-txn-build/include/urcu/rcu-txn-slab.h` is behind the dev tree.** The
dev version adds `__attribute__((aligned(64)))` to the stats counter block,
changing `struct urcu_slab`'s layout UNCONDITIONALLY. The build tree is
self-consistent, so this session's numbers stand, but syncing that header
REQUIRES rebuilding the library with it.

## Gates — all green on this tree

    make check                # 1 PASS (394 checks)
    make check-bucketlock     # 8 PASS
    make check-lru-arms       # 9 PASS + 2 ASan stress
    make check-deque          # 10 arms PASS (incl. the seal arm + its control)
    make check-deque-tsan     # 4 arms, 0 warnings

Plus: `-Wall -Wextra` clean across {bucketlock, txn} × {lock, MCAS, DC_NO_LRU} ×
{with, without FREE_ASSERT}.

## Build recipes

    # the dcache arms (bucketlock engine; swap dcache_txn.c for the txn engine)
    CPP="-I../../urcu-txn-build/include -I. -DDC_SPLIT_KEEPID"
    cc -O2 -g -pthread -march=native $CPP -DDC_MARK_GEN [-DDC_LRU_MCAS] \
       [-DDC_LRU_FREE_ASSERT] [-DDC_TXN_STATS] [-DDC_LRU_READD_LEGACY] \
       -o churn bench_dcache_churn.c dcache_bucketlock.c \
       -L../../urcu-txn-build/src/.libs -Wl,-rpath,../../urcu-txn-build/src/.libs \
       -lurcu-qsbr -lurcu-common -lrseq -lpthread

    # BOTH cadences, always
    ./churn --writers 48 --readers 48 --duration 3000 --evict bursty     --evict-cap 32
    ./churn --writers 48 --readers 48 --duration 3000 --evict continuous --evict-cap 32

    # counters + the ring dump out of a wedged process
    ./churn ... & kill -TERM $!    # dumps TXNSTATS + LRUCHK, _exit(3)

Build arms: `-DDC_TXN_PARENT_GUARD` (txn parent guard pair; OFF by default,
measured liveness cost -- see above).

Probe flags: `-DDC_LRU_FREE_ASSERT` (BOTH engines now), `-DDC_LRU_PUSH_DELAY`
(the targeted reproducer), `-DDC_LRU_MCAS_RETAIN_READD` (its mutation arm),
`-DDC_LRU_NO_DEAD_SEAL`,
`-DDC_LRU_NO_SHRINK_OWN`, `-DDC_LRU_NO_SHRINK_READD`, `-DDC_LRU_NO_RETAIN_READD`,
`-DDC_LRU_READD_LEGACY`, `-DURCU_TXN_DEQUE_NO_SEQ_GUARD`, `-DRETIRE_AUDIT`.
