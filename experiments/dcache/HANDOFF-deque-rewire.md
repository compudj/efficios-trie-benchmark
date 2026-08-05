# Handoff — the free-while-queued defect is CLOSED on all four arms.

2026-08-04. Written current-state-first, as the previous cut was: what is true
now, then what is still open, then the retractions in one place at the end.

The LOCK arm's free-while-queued defect — the one open item the last handoff
left — is fixed and mutation-tested. Two new open items came out of the work,
both of them PRE-EXISTING and both confirmed against a clean HEAD control.

Commits, all UNPUSHED, oldest first:

    d4faf6b  move the MCAS LRU onto the deque
    f6eb10c  the legacy LRU collapse is a CALLER bug, and the deque is clean
    c55a11d  RETRACT "TSAN cannot gate the deque test" -- it can, and found 16
    fffc94a  the free-while-queued defect is PORT-WIDE, not the deque's
    961462b  restore retain_dentry's d_unhashed guard (MCAS arm closed)
    7b43b89  consolidate the handoff around the current position
    (this)   close the LOCK arm: shrink-list handoff + DEAD seal

---

## ▶ WHAT LANDED: the LOCK arm needed TWO guards, not one

`-DDC_LRU_FREE_ASSERT`, `--writers 48 --readers 48 --duration 3000
--evict-cap 32`, 5 trials, FREE-WHILE-QUEUED occurrences:

| arm | bursty before | bursty after | continuous before | continuous after |
|---|---|---|---|---|
| bucketlock LOCK | **5/5** | **0/5** | **5/5** | **0/5** |
| txn LOCK | (no probe) | **0/5** | (no probe) | **0/5** |
| bucketlock MCAS | 0/5 | 0/5 | 0/5 | 0/5 |
| txn MCAS | (no probe) | 0/5 | (no probe) | 0/5 |

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

## ▶ OPEN ITEM 1 — an intermittent heap-use-after-free in `lru_evict_settled`

TSAN, bucketlock LOCK arm, `--evict continuous`:

    heap-use-after-free
      bl_lock            dcache_bucketlock.c:506
      bl_lock2           dcache_bucketlock.c:523
      lru_evict_settled  dcache_bucketlock.c:3645   <- &parent->d_child_head
      lru_shrink_range   dcache_lru_shrink.h:328

`lru_evict_settled` derives `parent = parent_of_rcu(d)` and then takes
`bl_lock2(bucket, &parent->d_child_head)` — on a parent that has been freed.

**PRE-EXISTING, not introduced.** Present on a clean HEAD control built and run
under the identical TSAN methodology (HEAD 2 occurrences / 3 runs; this tree 4 /
3 runs, then **0 / 6** on a later batch). ⚠ **The rate is too low and too
variable to rank the two trees at this sample size — do not quote 2-vs-4 as a
regression.** Not chased to root cause. It is the next thing to look at.

Total TSAN warnings did drop (HEAD 119 → 82 over 3 runs), and the sites
`lru_listed`, `lru_del` and `lru_link_tail_locked` — races on the state word
itself — are **gone**, which is the state word becoming atomic everywhere.

The residual non-UAF reports are all one pre-existing shape: fields written
plainly under the shard lock and read atomically without it — `sh->count`
(`dc_lru_count`) and `d_lru.referenced` (`lru_retain` vs the shrinker's clear).
Both are deliberately approximate. Worth a pass of `uatomic_*` for TSAN
cleanliness; neither is a correctness bug.

## ▶ OPEN ITEM 2 — rename × LRU × shrinker is jointly UNTESTED

`fold()` frees dentries (`host_to_free`, `n` at dcache_bucketlock.c:2669, 3009,
3130) with **no `lru_del` on any path**. That is safe only if a folded-away node
is never on the LRU.

Measured, not assumed: `stress_dcache` (renames) with the probe live is clean
3/3. But `stress_dcache` never calls `dc_shrink` (grep: 0 hits) and
`bench_dcache_churn` has **no renames at all** — so no test drives renames and
the shrinker together. Closing this needs a shrinker thread in `stress_dcache`
or a rename mode in the churn bench. Until then the fold's frees are unproven,
not proven safe.

## ▶ OPEN ITEM 3 — txn + MCAS + `--evict continuous` is unstable (not ours)

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
    make check-deque          # 8 arms PASS

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

Probe flags: `-DDC_LRU_FREE_ASSERT` (BOTH engines now), `-DDC_LRU_NO_DEAD_SEAL`,
`-DDC_LRU_NO_SHRINK_OWN`, `-DDC_LRU_NO_SHRINK_READD`, `-DDC_LRU_NO_RETAIN_READD`,
`-DDC_LRU_READD_LEGACY`, `-DURCU_TXN_DEQUE_NO_SEQ_GUARD`, `-DRETIRE_AUDIT`.
