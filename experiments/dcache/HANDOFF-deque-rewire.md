# Handoff — MCAS LRU on the deque: done. One defect open, on the LOCK arm.

2026-08-04. Rewritten as a consolidated statement of the CURRENT position rather
than the chronology it had become — four conclusions in that chronology were
later retracted, and a reader who stopped halfway would have acted on one. The
retractions are kept, in one place, at the end.

Commits, all UNPUSHED, oldest first:

    d4faf6b  move the MCAS LRU onto the deque
    f6eb10c  the legacy LRU collapse is a CALLER bug, and the deque is clean
    c55a11d  RETRACT "TSAN cannot gate the deque test" -- it can, and found 16
    fffc94a  the free-while-queued defect is PORT-WIDE, not the deque's
    961462b  restore retain_dentry's d_unhashed guard (MCAS arm closed)

---

## ▶ START HERE: the one open defect

**A dentry can be added to the LRU after it has been evicted and handed to
`call_rcu`, and is then freed while the LRU still points at it.** Witness:
`-DDC_LRU_FREE_ASSERT`, which asks in `dentry_free_cb` whether the dentry is
still listed at the instant its storage is released. It covers BOTH arms
deliberately — asking only the MCAS arm invites blaming the mechanism when the
question is what the PORT allows.

Two independent pushers, and they are NOT the same fix:

| | pusher | where | status |
|---|---|---|---|
| 1 | `lru_retain`'s re-arm | both arms | **FIXED on the MCAS arm** (`961462b`); narrowed only on bucketlock; unfixed on the LOCK arm |
| 2 | the shrinker's put-back after a failed evict | LOCK arm always; MCAS legacy arm | **OPEN** |

Current state, `--writers 48 --readers 48 --duration 3000 --evict-cap 32`, 5
trials, FREE-WHILE-QUEUED occurrences:

| arm | bursty | continuous |
|---|---|---|
| bucketlock MCAS deque | 0/5 | 0/5 |
| txn MCAS deque | 0/5 | — |
| **bucketlock LOCK** | **5/5** | **5/5** |
| txn LOCK | — | 0/5 (1 SEGV, pre-existing) |

### What the LOCK arm needs, and why the cheap answer does not work

Mainline is protected by `d_lockref` **plus** two things that are not the
refcount:

- `retain_dentry` refuses to re-add an **unhashed** dentry — its FIRST test,
  before any count is consulted (`fs/dcache.c`);
- `d_lock` serialises `retain_dentry`'s `d_lru_add` against `__dentry_kill`'s
  unhash + `d_lru_del`.

And mainline's shrinker never does unlink-then-put-back: `dentry_lru_isolate`
calls `d_lru_shrink_move`, which moves the dentry to a PRIVATE shrink list while
keeping `DCACHE_LRU_LIST` set and adding `DCACHE_SHRINK_LIST`, so it is never
ownerless; `__dentry_kill` honours that by skipping `d_lru_del` and leaving
`can_free = false`, handing the free to `shrink_dentry_list`. **Isolation and
ownership are the same act there.**

Options for our lock arm, in the order I would try them:

1. **The full shrink-list handoff.** Faithful, and the ONLY one that actually
   closes it. Needs a `DC_LRU_SHRINK(i)` state and, critically, the `can_free`
   transfer: `dc_unlink` must not `call_rcu` a dentry the shrinker owns. That
   spans `dc_unlink` in both engines.
2. **Keep isolation, add the liveness guard to the put-back** (re-check "still
   hashed" under the shard lock before `lru_link_tail_locked`). Cheap; narrows
   only, same class as bucketlock's fix 1.
3. **Re-import a per-dentry `d_lock`.** Closes it exactly as mainline does; costs
   a word plus an acquire on the enqueue path.

⛔ **Do NOT try evict-first on the lock arm. It was tried and measured
(fix 2, reverted).** No improvement on either cadence and **5/5 SEGV** on the txn
lock arm, inside `urcu_txn_install_mw_depth` with a dangling `r->slot` from
`lru_evict_settled`'s own commit — the victim freed underneath the eviction.
Evict-first is safe on the MCAS arm only because
`urcu_txn_deque_remove`'s `&n->owner : d -> NULL` record makes exactly ONE
sweeper win — **the deque supplies ownership**. The lock arm got its ownership
from isolation, and evict-first removes precisely that with nothing to replace
it, so many sweepers evict one victim concurrently.

⚠ Salvaged from that revert and still true: after dropping the shard lock, a
membership re-check must be `shard == DC_LRU_ON(i)`, **not** `>= DC_LRU_ON(0)`.
`lru_retain` enqueues on the CALLER's shard, so the victim can be removed and
re-added on a DIFFERENT shard while the lock is down.

## ▶ Second open item, and it is not ours

`--evict continuous` on the **txn engine + MCAS deque** hangs. Three gdb samples
two seconds apart: 47 of 48 writers in `cds_fair_mutex_park` every time, the 48th
moving (`urcu_slab_alloc` → `sysmalloc` → `pthread_mutex_lock`), no assert. That
is escalation-lane starvation plus the park-while-online QSBR stall — a
**liburcu** matter. `urcu_txn__enter_fallback()` → `cds_fair_mutex_park()` blocks
on a futex while the thread is still RCU-online, which holds off every grace
period, which stalls `call_rcu` reclaim, which deepens the contention that caused
the escalation. Self-reinforcing and absorbing. The fix belongs there: go offline
across the park.

---

## What landed

### The rewire (`d4faf6b`)

The MCAS LRU runs on `<urcu/rcu-txn-deque.h>`; `rcu-txn-list.h` is out of the LRU.

- `d_lru` on the MCAS arm is `struct urcu_txn_deque_node dnode` and nothing else;
  `shard` survives only on the lock arm. Membership is `urcu_txn_deque_owner()`.
- The claim protocol is deleted (`lru_claim` / `lru_unclaim` / `lru_del_claimed`
  / `lru_unlink_claimed`, `DC_LRU_BUSY`). `DC_LRU_OFF` / `DC_LRU_ON` are
  `#ifndef DC_LRU_MCAS`, so a surviving use fails to compile rather than quietly
  reintroducing a second membership record.
- `lru_move_tail` → `urcu_txn_deque_rotate_head()`. Its old rationale
  ("protect a lockless traverser standing on the node") was **vacuous**: this
  port has no LRU traversal.
- `dc->lru[i]` is a `struct urcu_txn_deque`; its own approximate `count` is the
  only counter.
- `dc:claim` / `dc:wedge` tracepoints removed (they instrumented a protocol that
  no longer exists); `dc:commit` stays. Dead `DC_TS_LRU_EVICT` → `DC_TS_LRU_ROT`.

Stated in `dcache_lru_shrink.h` rather than discovered later: with no claim, two
sweepers can both reach `lru_evict_settled()` for one victim. Safe — bucketlock
re-verifies the hlist mark under `bl_lock2`, txn's `hlist_del_prepare` answers
`-ENOENT` — but it **moves the serialization point from the LRU word to the
eviction**.

### Fix 1: `retain_dentry`'s missing `d_unhashed` guard (`961462b`)

A bare check is only a narrower race, so the guard is COMPOSED INTO THE PUSH
(`lru_push_prepare`) via an engine-supplied predicate:

- **txn**: `urcu_txn_load_validate(&d->d_hash.next)`. That slot is MCAS-managed
  there, so the guard joins the push's conflict set — unhash-first aborts our
  push; push-first is caught by the `lru_del` that runs after the unhash commit.
  Cross-domain is fine: a domain owns only the escalation lane, conflict
  detection is per-slot.
- ⛔ **bucketlock: MUST NOT be transacted.** `bl_hlist_del_locked` writes that
  slot with a plain `__atomic_store_n` under the bucket lock, so an MCAS guard
  would plant a proxy the plain store overwrites — and this transaction's settle
  would then write the pre-mark value back OVER that writer's mark, resurrecting
  a node the index already deleted. Plain read only; narrowed, not closed.
  Closing it needs the bucket lock on the enqueue path.

## The deque itself

`test_deque.c` now covers the two axes the dcache exercises and it did not:
**many deques** (nodes migrate; `owner` must name the right one; remove derives
its deque from a hint) and **reuse** (retire + re-init in place, resetting
`seq`).

`make check-deque` — 8 arms, all PASS: 2/8/32 writers × 4 deques, 1 deque
(single-ring regression), 16 deques, the `NO_SEQ_GUARD` mutation, ASan, harness
self-check. `make check-deque-tsan` — 4 arms, **0 warnings**.

⚠ **The `seq` ABA guard is STILL UNPROVEN.** Reuse was the hypothesis that would
make it load-bearing; compiled out at 32 writers with reuse on, the test still
passes. Worse, reuse RESETS `seq` to zero, so its stated premise ("never
decreases") does not survive recycled storage at all: it is monotone **per
membership, not per address**. Worth a header caveat; not yet written.

⚠ The harness's own retire needs TWO grace periods — one before removing (drain
would-be pushers) and one after (drain peers holding the node as a derived
pointer). The second was missing at first and looked exactly like a deque defect.
`-DRETIRE_AUDIT` is opt-in because it widens every window and masked it 8/8.

---

## Measured facts — do not re-derive

MCAS deque, bucketlock engine, `--evict bursty --evict-cap 32`, 5 trials/point:

| writers | 8 | 16 | 32 | 48 | 96 | 192 |
|---|---|---|---|---|---|---|
| complete | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |
| Mchurn/s | 2.44-2.60 | 3.40-3.61 | 3.98-4.19 | 3.41-3.61 | 1.81-1.89 | 0.98-1.02 |

⚠ **NOT comparable to the pre-rewire 1.39-1.42 figure** — the command line behind
that number was never recorded. A list-vs-deque throughput claim needs both arms
re-run under one command line, and nobody has done that.

Instrumented (`-DDC_TXN_STATS`), MCAS deque default arm, 3 trials of ~10M
`lru_del` each: **0 escalations, maxretry 3, 0 disowned nodes in any shard.**

## Two upstream fixes that stay (independent of the LRU)

- `750572af` — `urcu_txn_list_del_prepare` load-validates its derivation read of
  `&elem->prev`.
- `b69b4a53` — all five list convenience brackets leaked the escalation lane on
  their `-ENOENT` terminal bail (no `urcu_txn_abandon()` before `end()`).

⭐ The rule behind both: **an operation that READS a slot it does not WRITE must
validate that read.**

---

## ⛔ Claims retracted — do not resurrect

**This session** (each was argued convincingly, then refuted by measurement):

- "the ring reaching an `owner == NULL` node is a deque defect" — it is a
  **caller** bug: the dentry was freed while queued, and the zeroes are reused
  storage.
- "evict-first fixes it" / "the default arm is clean" — true for `--evict
  bursty` ONLY, stated unqualified. `--evict continuous` fires the other pusher.
- "TSAN cannot gate the deque test; it cannot model a QSBR grace period" — it
  can. The TSAN liburcu was four days stale, and all 16 surviving reports were
  real harness defects.
- "evict-first ports to the lock arm" — no improvement plus 5/5 SEGV.

**Before the rewire:**

- the stale-prev forward rescan (`prev_repair`); "a corrupt ring without the
  sentinel"; the six-edge `move_tail` as the cause; word-OFF-while-linked;
  "plain stores exonerated" (a TIMEOUT counted as THE WEDGE); "the slab lfstack
  pop is spinning" (one gdb sample); "the deque reproduces the dcache wedge at
  engine level" (same symptom, different cause).

## Method rules earned the hard way

- **Sweep the KNOB, not just the thread count.** Testing one evict cadence and
  reporting the result unqualified cost two wrong conclusions in one session.
- **Grep for the violation marker, not the exit status.** A timeout is not the
  wedge; an abort is not a hang.
- **One gdb sample is not a spin.** Three, two seconds apart, and check whether
  frames move. That is what separated the retry wedge from lane starvation.
- **Run the control under the SAME methodology.** The disowned-node finding was
  only meaningful because the default arm was SIGTERM'd mid-run through the same
  walk and came back clean. Likewise `make stress-tsan` on the rebuilt TSAN tree
  is what proved 16 "phantoms" were mine.
- **A probe that widens every window is not a control** (`-DRETIRE_AUDIT`).
- **One macro must not gate two sites.** `DC_LRU_NO_READD` gated both re-adds,
  making every result from it uninterpretable; split into
  `DC_LRU_NO_SHRINK_READD` / `DC_LRU_NO_RETAIN_READD`.
- **Verify a probe is live before trusting its zero**; print on the FIRST call
  and run a with/without control. Bitten 3x.
- **Mutation-test every new guard**; if removing it changes nothing, say so
  instead of claiming a fix (`URCU_TXN_DEQUE_NO_SEQ_GUARD`).
- **Ask the cheap decisive question first.** `-DDC_LRU_FREE_ASSERT` took five
  minutes and overturned a conclusion I had already committed.

## Tree hygiene — both of these have cost real time

⚠ **`urcu-txn-tsan-build` is a COPY of `urcu-txn-build` and silently rots.** It
was a Jul 31 copy against an Aug 3 engine, so TSAN ran a different engine from
every other gate. `check-deque-tsan` refuses to run when they differ; check
manually before any other TSAN work:

    md5sum urcu-txn-tsan-build/include/urcu/rcu-txn-mcas.h \
           urcu-txn-build/include/urcu/rcu-txn-mcas.h

Rebuild recipe: the comment above `stress-tsan` in the Makefile. Verify
instrumentation in the GENERATED CODE (`cc -S -fsanitize=thread … | grep
__tsan_atomic64_load`), not in `config.h`. Pass `TSAN_SLAB_CPP`, not `SLAB_CPP`.

⚠ **`urcu-txn-build/include/urcu/rcu-txn-slab.h` is behind the dev tree.** The
dev version adds `__attribute__((aligned(64)))` to the stats counter block, which
changes `struct urcu_slab`'s layout **unconditionally** — it is not behind an
ifdef. The build tree is self-consistent (old header, old library), so this
session's numbers stand, but syncing that header REQUIRES rebuilding the library
with it or the `.so` and its callers disagree about field offsets.

## Gates

    make check                # 1 PASS
    make check-bucketlock     # 8 PASS
    make check-lru-arms       # 9 PASS + 2 ASan stress
    make check-deque          # 8 arms
    make check-deque-tsan     # 4 arms, 0 warnings

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
    ./churn ... & kill -TERM $!    # the stats handler dumps TXNSTATS + LRUCHK, _exit(3)

Probe flags: `-DDC_LRU_FREE_ASSERT` (both arms), `-DDC_LRU_NO_RETAIN_READD`,
`-DDC_LRU_NO_SHRINK_READD`, `-DDC_LRU_READD_LEGACY` (the known-unsafe control),
`-DURCU_TXN_DEQUE_NO_SEQ_GUARD`, `-DRETIRE_AUDIT` (test_deque).
