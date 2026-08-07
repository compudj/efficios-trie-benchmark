# Handoff — the wedge was TWO defects: a leaked lane, and a livelock inside it.

2026-08-06 (was 2026-08-05). Written current-state-first: what is true now, then what is open,
then the retractions in one place at the end. Read the retractions — five
confident stories died to controls in the session that produced this file, and
each control cost minutes. The session AFTER it killed two more, one of them a
claim promoted to a headline in this very file.

**CLOSED, all mutation-proven:** free-while-queued on all four arms (lock arms
via a shrink-list handoff + a terminal state word; MCAS arms via a new liburcu
deque seal); the stale-`d_parent` use-after-free (a guard pair); `fold()`
freeing dentries that were still on the LRU; and the **census anomaly** — which
was never an LRU defect at all.

**The census anomaly, in one sentence:** `dc_add` tested for a duplicate name
OUTSIDE the bucket lock (bucketlock) / outside the transaction (txn), so two
concurrent adds of ONE name both published, and everything under the losing copy
became reachable by `dc_walk` and absent to `dc_lookup`. See "THE CENSUS
ANOMALY". The kernel-faithful **seqlock baseline never had it** — it does the
same test under the bucket lock and says so in a comment.

⚠⚠ **THE LIBURCU SIDE IS NOW PUSHED** (`github-dev/urcu-txn-dev` == `a816ba5b`).
Three rewrites in this file's history — dropping the move API, amending the
validate commit, splitting the lane fix — were all justified by "unpushed, so no
force-push and nobody else's history". **That justification is spent.** Anything
further on those commits is a NEW commit on top, not a rebase.

Commits below are in THIS repo (benchmark side), oldest first:

    d4faf6b  move the MCAS LRU onto the deque
    f6eb10c  the legacy LRU collapse is a CALLER bug, and the deque is clean
    c55a11d  RETRACT "TSAN cannot gate the deque test" -- it can, and found 16
    fffc94a  the free-while-queued defect is PORT-WIDE, not the deque's
    961462b  restore retain_dentry's d_unhashed guard (MCAS arm closed)
    7b43b89  consolidate the handoff around the current position
    9f62bc7  close the LOCK arm: shrink-list handoff + DEAD seal
    cc6778f  root-cause the stale-d_parent UAF: a guard pair on the parent
    3e72f00  close the MCAS arm: no re-arm where the guard cannot be transacted
    2420ba0  seal the deque node on kill; bucketlock keeps its re-arm
             (liburcu side: urcu-txn-dev a816ba5b)
    72587b8  fold(): take the dentry off the LRU before freeing it
    df8a84a  RETRACT the park-while-online open item; the OOM was the harness
    9140b83  measure the txn+MCAS+continuous instability instead of theorising
    d789a58  churn: make the census anomaly name itself, and gate the OK line
    ee00c67  narrow the census anomaly to the MCAS LRU arm

---


## ▶⭐⭐ 2026-08-06 — THE `--evict continuous` WEDGE: **a leaked escalation lane**

Root-caused under LTTng. **6/6 wedge without the fix, 0/6 with it**
(`-DDC_NO_LANE_GIVEBACK` is the arm that must wedge).

### What it is

`urcu_txn_end()` KEEPS an escalated handle's lane while the last commit
ABORTed, so the re-attempt need not go to the back of the FIFO. The lane is
therefore **operation-scoped, not bracket-scoped**. `rcu-txn.h` states both the
obligation and the penalty verbatim — end without re-attempting and *"the
domain's lane is held forever and every other writer parks behind it."*

### Where it leaks — **not at any bail**

The three bails that end their own bracket were already fixed and are NOT the
wedge. The retry loops **re-decide at their HEAD**, and those head checks exit
the operation:

    for (;;) {
            top = find_top_rcu(...);
            if (!top) { ret = -ENOENT; goto out; }   <-- lane still held
            ...
            if (p) { urcu_txn_conflict(&txn);        <-- keeps the turn
                     urcu_txn_end(&txn);             <-- so end() KEEPS it
                     continue; }                     <-- re-decides at the TOP
    }

The `conflict(); end(); continue;` is **correct** — it does re-attempt. The
lane is lost **one iteration later**, by a `goto out` that knows nothing about
transactions. A per-bail fix cannot reach it, so `dc_lane_giveback()` puts the
release at the **operation's exit**, in the only three functions whose retry
loop can `goto out` with a live handle: `dc_unlink`, `stack_shell`,
`dc_rename_exchange`.

### ✅ RESOLVED — the second path was a LIVELOCK, not a leak

The residual wedge is closed (`a458263`). It was never a second leak: a rename
whose destination parent has been **sealed** gets `-ENOENT` from
`insert_head_prepare(shell->d_sib -> new_parent->d_child_head)`, and a seal is
**terminal for that dentry's lifetime** — so the retry re-derives it for ever,
*inside* the lane, parking every other writer. That is why the thread dump was
indistinguishable from a leak.

⛔⭐⭐ **`-ENOENT` meant two opposite things**: from a DEL prepare, "the source
moved" (retryable); from an INSERT prepare, "the destination is gone"
(terminal). The insert side now answers `-ESTALE` and both callers treat it as
terminal, as they already did for `-EINVAL`/`-EEXIST`.

**Same defect class as `413fae5`**, and the warning was already written two
lines above the site. A terminal state reached through a retryable-*looking*
error code is now a known shape here.

Evidence: the trace named the line — **24650 identical `-ENOENT` retries from
one thread** in a 250k-event window, all from line 2512, with 4 threads parked.
A/B pooled over 112 rounds × 6 concurrent: **fix 0/112, mutation 17/112,
p = 4.0e-06**. ⚠ The first 40 rounds alone were 0/40 vs 3/40, **p = 0.12 — not
significant**; the run was extended rather than the number quoted.
Gate: `make check-rename-livelock`.

### ⭐⭐ The design point, worth raising upstream

The special case conflates **mutual exclusion** with **queue position**, and
only the position must survive a retry. `conflict()` vs `abandon()` is a
promise about what happens *after* the bracket closes, made *before* the loop
head re-decides — **the API asks you to declare your future before you know
it.** If `cds_fair_mutex` could release the lock while keeping the waiter
queued, `end()` would always unlock, `retrying` would drop out of the release
decision, and this defect class would be *unrepresentable*.

### ⛔⭐⭐ A regression I shipped, that the gate would have caught

`d45ff25` gave `stack_one_prepare()` a destination re-check. **An EXCHANGE's
destination is occupied BY DESIGN** — by the counterpart — so it fired on every
exchange, and `dc_rename_exchange` treats only `-EINVAL` as terminal ⇒ a
**deterministic single-threaded hang** in `test_midtransition` (3/3; parent
commit 3/3 pass). Fixed in `413fae5` with `@expect_dest` (NULL for a rename,
the counterpart's top for an exchange). **I did not run `check-lru-arms` after
committing it.**

⚠ That livelock ran *inside* the lane, so it presented as a whole-process
wedge — **the same thread dump as the lane leak. Two distinct defects that look
identical from a backtrace.**

### ⭐⭐ Method (the transferable part)

- **PROVE THE PROBE LIVE BEFORE ITS ZERO MEANS ANYTHING.** A bracket trace
  showed `in_fallback=0` on *every* thread and I nearly read it as "nobody
  escalated". `in_fallback` is set **after** the lock is taken, so caller-side
  events **cannot see the lane at all**. Events *inside* the acquire path
  answered it in one line: **ATTEMPT 940 / HELD 932 / RELEASE 931** — exactly
  one lane taken and never returned, 8 threads parked behind it.
- **AMPLIFY THE RARE PRECONDITION.** Escalation is essentially never reached
  naturally — **0 lane acquisitions in 111k healthy brackets** — which is why
  ~280 quiet runs and a 40-pair A/B all came back empty, and why I twice
  declared this hypothesis refuted. Forcing `want_fallback` true makes it
  deterministic.
- ⚠ `babeltrace2 <session-dir>` reads **every accumulated snapshot**; two
  "per-arm" traces shared vtids. Fresh session per arm or attribution is void.
- ⚠ `pgrep -f <pattern>` matches **my own monitoring shells** — cost ~30 min
  twice, once by reporting a finished gate as still running.
- ⚠⚠ **"No CPU progress" does NOT detect a livelock** — the spinner burns CPU.
  Only "still alive at 30s against a ~1s baseline" catches it.
- ⚠⚠ **THE AMPLIFIER IS NOT A DETECTOR.** Forcing `want_fallback` true
  serialises every txn through the FIFO lane and slows the stress harness ~100×,
  so a fixed timeout cannot tell a hang from the amplifier. It reported a false
  wedge, retracted after measuring CPU deltas (799 ticks in an 8s window =
  progressing). The mutation A/B stands because **both arms were equally
  amplified and the fixed arm completed** — a differential, never an absolute
  verdict.

### Reproducer

Probes live in the **gitignored** `urcu-txn-build` copy (weak hook, inert
without the flag) plus the `dc:txn` event in `dcache_tp.h`:

    # in urcu-txn-build/include/urcu/rcu-txn.h (gitignored, see the commit):
    #   URCU_TXN_LANE_TP(0/1/2, txn) at attempt / held / release
    # amplifier: make urcu_txn__want_fallback() return domain && !in_fallback
    cc ... -DDC_ENABLE_TRACING -DURCU_TXN_LANE_TRACE ... bench_dcache_churn.c \
           dcache_txn.c dcache_tp.c
    timeout 45 ./a.out --writers 8 --readers 8 --duration 500 \
           --evict continuous --evict-cap 32     # rc=124 == wedged


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
costs no liveness at all.

#### ✅ RE-MEASURED at 20 trials/cell — the cost is REAL, the REASON was WRONG

The promise this section used to end with — "kept as a build arm so it can be
re-measured once the park-while-online defect is fixed in liburcu, **which is
where that cost lives**" — has been cashed in, and it was wrong. Both candidate
mechanisms are now eliminated and the cost did not move.

`--evict bursty`, 48w/48r, **20 trials per cell**, runs not finishing in 120s,
run as a 2x2 so the dc_add duplicate re-check could not hide in the result:

| | dup-recheck ON (shipped) | dup-recheck OFF |
|---|---|---|
| **guard off** | **1/20** | **0/20** |
| **guard ON** | **9/20** | **8/20** |

* **The guard costs ~+40 points, in both rows** (Fisher 0/20 vs 8/20,
  p ≈ 0.003). Bigger than the 2/6 that first shelved it, and now measured at a
  sample size this configuration's instability cannot explain away.
* **The dup re-check costs nothing** — 1/20 vs 0/20, and 9/20 vs 8/20. No main
  effect, no interaction. It is exonerated as a liveness regression.

⛔⭐⭐ **RETRACTED: "that cost lives in the park-while-online defect."**
`dcf1310c` had ALREADY fixed park-while-online (the lane wait is bracketed
offline/online), and `fc663f5a` has since fixed the terminal-bail lane leak in
every `rcu-txn-hlist.h` bracket — the header this engine runs on. Two mechanisms
removed, cost unchanged. **The guard's price is not a liburcu bug waiting to be
fixed; it is the intrinsic cost of extra conflict-set entries on this engine's
hot paths, and the mechanism is now UNATTRIBUTED rather than explained.**
⚠ Do not re-shelve it behind another pending fix without measuring first — that
is exactly what the previous note did, and it cost two sessions of "once X is
fixed" before anyone checked whether X was still true.

### ✅⭐⭐ AND THE OPEN DIRECTION PAID OFF: `-DDC_TXN_PARENT_SEAL`

Buy the exclusion with a slot the transactions ALREADY WRITE — the shape that
made the `dc_add` duplicate re-check free. **The eviction SEALS the victim's
child head instead of guarding it:**

    urcu_txn_store_mw(&txn, &d->d_child_head.first,
                      NULL, urcu_txn_hlist_set_mark(NULL), TAG)

Same slot, same expected old (`NULL` = "still empty"), **written rather than
read**. `dc_add` publishes a child by writing `&parent->d_child_head.first`, so
the two contend on ONE slot with ONE expected old and the MCAS admits exactly
one — mutual exclusion in BOTH directions, where the guard needed two records:

* add wins → the store's old-value check fails, the eviction aborts, retries,
  sees a non-empty child list, answers `-EAGAIN`;
* evict wins → the head is MARKED, and the add's own
  `urcu_txn_hlist_insert_head_prepare()` answers `-ENOENT` **with no guard
  record of its own**.

⭐ It fits because the primitives existed: `<urcu/rcu-txn-hlist.h>` RESERVES a
marked head as its sealing primitive and `insert_head_prepare` already refuses
one; `urcu_txn_hlist_resolve()` STRIPS the mark, so a sealed head still reads as
EMPTY to `children_empty()`, `dc_readdir` and the census; and `dc_add` already
maps that `-ENOENT` to "the prefix went". Terminal per LIFETIME (`dentry_alloc`
memsets) and sealed in the SAME commit as the unlinks — the rule `DC_LRU_DEAD`
and `URCU_TXN_DEQUE_POISON` already follow. `dc_unlink` seals too (⚠ only when
SETTLED: when `top != host` the host is not freed here).

| `--evict bursty`, 48w/48r, 20 trials | timeouts |
|---|---|
| unprotected (shipped) | 1/20 |
| `DC_TXN_PARENT_GUARD` | **9/20** |
| **`DC_TXN_PARENT_SEAL`** | **0/20** |

⚠⚠ **THE NATURAL-TIMING TSAN A/B COULD NOT SETTLE CORRECTNESS — AND ITS CONTROL
NEVER FIRED.** Half its runs wedged at the cap having done ~0 work, so the
protected arm's zero would have proved nothing. Killed it and built
**`-DDC_TXN_PARENT_DELAY`**: 200 µs widening of the exact gap between
`children_empty()` and the commit. The control then fires, with the documented
backtrace — `lru_evict_settled` → `urcu_txn_commit` → `urcu_txn_try_cas`, CASing
into storage `dentry_free_cb` had already freed.

| reproducer, 8w/8r/15s/cap 8, 12 trials/arm | UAF runs | occurrences |
|---|---|---|
| unprotected | 3/12 | 6 |
| **SEAL** | **0/12** | **0** |

### ✅⭐⭐ NOW ON BY DEFAULT (`-DDC_TXN_NO_PARENT_SEAL` is the mutation arm)

The 200 µs reproducer only reached 3/12 (p ≈ 0.22), so the delay was **swept,
not cranked** — it sits on every eviction attempt, so too wide STARVES the
shrinker and detects LESS:

| delay | control fires | adds / evictions |
|---|---|---|
| 200 µs | 3/12 | ~13k / ~18k |
| 1 ms | 2/3 | ~25k / ~34k |
| **5 ms** | **6/6** | ~13k / ~18k |

At 5 ms the control is DETERMINISTIC (2 UAFs every run). 6 trials/arm, equal
work: unprotected **6/6 runs, 12 occurrences** · seal **0/6, 0** — Fisher
**p = 0.0022**.

Liveness, 20 trials/arm, ⚠ **BOTH CADENCES this time** (the first pass was
bursty-only, which is not good enough to change a default):

| | unprotected | GUARD | SEAL |
|---|---|---|---|
| `--evict bursty` | 1/20 | **9/20** | **0/20** |
| `--evict continuous` | 1/20 | — | **0/20** |

⚠⚠ **AND THE CONTINUOUS ROW IS WHY `check-churn-evict` CHANGED.** A gate run
wedged on txn+continuous with the seal on and looked like a regression the seal
had caused. It is not — the UNPROTECTED arm wedges 1/20 on that cadence too, so
it is **PRE-EXISTING, ~1 run in 20**. The gate had no per-run timeout, so that
wedge could hang it for ever; it now runs each arm under `timeout 120` and
reports 124 as WEDGED rather than hanging or silently passing.
⭐ The lesson is the one this file keeps re-learning: **a wedge on the arm you
just changed is not evidence the change caused it — measure the arm you did NOT
change.**

### ✅ TSAN residue of the seal: NO NEW RACE SHAPE (and a pre-existing one found)

⚠ The only TSAN data on the seal at first came from a REPRODUCER binary with a
5 ms sleep injected, where only `heap-use-after-free` was counted and 33 `data
race` warnings were ignored. A default was flipped on a partially-read run, so
this is the sweep that closes that: plain TSAN, seal ON vs OFF, 3 runs each,
8w/8r/15s/cap 8.

⚠⚠ **COUNTS ARE USELESS HERE** — off 132/128/98, on 116/12/98. The overlap says
nothing. Compare SHAPES.
⭐ And the shape function matters: hashing whole CALL STACKS reported ten shapes
the seal had supposedly *removed*, which is nonsense — stacks differ by inlining
and caller. The right key is the **pair of racing ACCESS SITES** (the `#0` frame
of each access).

    distinct access-site pairs   sealoff 15 · sealon 15
    pairs only with the seal     ONE, seen once:
                                 dc_lru_count <-> lru_link_tail_locked

and `dc_lru_count` already races with two OTHER link sites on BOTH arms
(off 7/on 4, off 2/on 3). Same field, same family, n=1 — not a new race.

⭐⭐ **DECISIVE: the seal's slots never appear as an access site at all.** Every
racing site across the whole sweep is one of six functions:

    lru_add 79 · lru_unlink_locked_to 47 · lru_shrink_range 32
    lru_retain 13 · dc_lru_count 8 · lru_link_tail_locked 5

No `d_child_head`, no `lru_evict_settled`, no `dc_add_typed`, no txn commit
path. ⚠ Beware the obvious grep: `dc_add_typed` DOES appear in these reports —
in the CALL STACK of a race whose access site is `lru_add`, because dc_add calls
lru_add. Grepping all frames scores 76 hits and means nothing.

### ▶ OPEN, and NOT the seal's: the txn LRU lock arm has ~100 races/run

This sweep is the first TSAN characterisation of the **txn** engine's LRU lock
arm; the residue documented earlier in this file (`sh->count` and
`d_lru.referenced`, both deliberately approximate) was measured on
**bucketlock**. On txn the set is larger and includes the LRU LIST LINKS
themselves — `lru_add`'s `d->d_lru.prev/next` and `sh->tail->d_lru.next` against
`lru_unlink_locked_to`'s stores — at ~20 occurrences for the top pair.

Those are written under a shard lock, so either the two sides hold DIFFERENT
shard locks (which this file already documents as true — `lru_add` uses the
CALLER's shard) and the exclusion is the state word rather than the lock, in
which case TSAN cannot see it; or something is genuinely unguarded. **NOT
DETERMINED.** It is identical with the seal on and off, so it is pre-existing
and orthogonal to that change — but "TSAN is clean on the txn engine" is NOT a
claim this sweep supports, and nobody should quote it as one.

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

### ⭐⭐ The real fix: `URCU_TXN_DEQUE_POISON` (liburcu `a816ba5b`)

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

## ✅ THE CENSUS ANOMALY — RESOLVED: two dentries, one name

**ROOT CAUSE: `dc_add` decided "this name is free" without holding anything.**

    if (__child_lookup(dc, parent, name))     <- no lock, no txn
            return -EEXIST;
    d = dentry_alloc(...);
    bl_lock2(bucket, &parent->d_child_head);  <- the exclusion starts HERE
    ... publish into both indexes ...

Two concurrent adds of one name both pass that test and both publish, so the
bucket ends up holding TWO dentries spelled alike. A lookup resolves whichever
the chain reaches first while a child-list walk descends the other, so
everything added under the loser is reachable by `dc_walk`/`dc_readdir` and
`DC_ABSENT` to `dc_lookup` — permanently, with nothing to unhash it. That is the
census `extra`, exactly: `0 missing, N extra, 0 stray, 0 dup`.

⭐⭐ **THE COMMENT SAYING IT WAS SAFE HAD A STALE PREMISE.** It read "racy under
concurrent same-name adds — *disjoint in the churn workload*; harden with a
re-check under the lock if ever needed". Writers do own disjoint slots — but
`rebuild_prefix()` was added later, to let the bench survive eviction taking a
directory, and it has EVERY writer recreate the SAME `d{i}` names and rely on
-EEXIST to arbitrate. 105k–158k rebuilds per 3 s run. The precondition stopped
holding the day that function landed, and the comment was never revisited.

⭐⭐ **THE SEQLOCK BASELINE WAS RIGHT ALL ALONG** and is the tell: it does the
existence test under `dir_wlock` + `bl_lock`, with a comment stating the
guarantee ("a racing add of the same (parent, name) — which hashes to this same
bucket — sees one or the other atomically"). The two fast engines diverged from
the baseline they exist to be compared against, and were doing strictly less
work per add than it.

### The fixes, and why they are different shapes

* **bucketlock** — re-check under `bl_lock2()`, the same lock the publish takes.
  Same (parent, name) hashes to the same bucket, so one lock covers every racer.
* **txn** — no lock to reuse, so the atom is A SLOT: read the bucket head ONCE
  with `urcu_txn_load`, scan for the name AFTER that read, and hand that SAME
  value to `urcu_txn_hlist_insert_at_slot_prepare` as its expected old. A peer
  that published earlier is seen by the scan; a peer that publishes later
  changes the head, so the install's old-value check fails and the retry
  re-scans. ⚠ `insert_head_prepare()` is opened up rather than called *because*
  it does its own load of the head — a scan placed before that load only
  NARROWS: the peer's publish lands between the two and gets adopted as this
  insert's expected old, which then validates cleanly. **Costs no new read-set
  entry**, which matters on the engine where `DC_TXN_PARENT_GUARD` had to be
  turned off for exactly that cost.

Mutation knob kept on both: `-DDC_NO_ADD_DUP_RECHECK`.

### Mutation matrix — 8 trials/arm, 48w/48r, `--evict continuous`

| engine | build | FAIL | runs with duplicate names |
|---|---|---|---|
| bucketlock | fix, MCAS | **0/8** | **0/8** |
| bucketlock | fix, lock | **0/8** | **0/8** |
| bucketlock | `NO_ADD_DUP_RECHECK`, MCAS | 1/8 | 3/8 |
| bucketlock | `NO_ADD_DUP_RECHECK`, lock | **4/8** | 4/8 |
| txn | fix, MCAS | **0/8** | **0/8** |
| txn | fix, lock | **0/8** | **0/8** |
| txn | `NO_ADD_DUP_RECHECK`, MCAS | 2/8 + 1 timeout | 3/8 |
| txn | `NO_ADD_DUP_RECHECK`, lock | 3/8 + 1 timeout | 5/8 |

### ▶ AND IT TOOK THE TIMEOUT MODE WITH IT

The other half of this configuration's bad behaviour — runs that never finish in
120 s, pooled at **32% (11/34)** across the batches above and never explained —
does not appear on a fixed build. txn + MCAS + `--evict continuous`, 20 trials
per arm, `timeout 120`:

| build | pass | consfail | **timeout** | runs with duplicates |
|---|---|---|---|---|
| fix | **20** | 0 | **0** | **0/20** |
| `-DDC_NO_ADD_DUP_RECHECK` | 8 | 9 | **3** | 8/20 |

Pooled with the 8-trial batch: **28/28 pass and 0 timeouts on the fix**, against
4 timeouts in 28 with the guard removed.

⚠ **Stated as evidence, not as a closed mechanism.** 0/28 against a ~14%
baseline is p≈0.014 — good, and consistent with the conservation failures
vanishing on the same builds, but this file's own instability table is the
reason to say it that way. The plausible story (stranded leaves under a losing
directory hold LRU capacity, so `dc_lru_count` stays over cap, so every writer
op shrinks, while `rebuild_prefix` storms) is UNTESTED. If a timeout ever shows
up on a fixed build, that story is why, and it should be measured rather than
argued — the `timeout 120` path SIGTERMs into a full TXNSTATS + LRUCHK dump,
which is the thing to read first.

### ⛔⭐⭐ RETRACTED: "it is the MCAS LRU arm, not the index engine"

`ee00c67` promoted a 6-trial sweep (txn-lock 0/6 · txn-MCAS 1/6 · bl-MCAS 3/6 ·
bl-lock 0/6) to a headline and sent the next session to read
`lru_evict_settled`. **It is all four arms.** The mutation rows above put
bucketlock-LOCK at 4/8 — the arm the sweep called clean twice. The LRU is not
involved in the mechanism at all; eviction only supplies the workload, because
it is what makes `rebuild_prefix` run.

⛔ **ALSO RETRACTED, before it reached a commit:** "the MCAS arm rotates in-use
entries instead of removing them, so directories stay queued and get evicted
more, so it rebuilds more." Plausible, mechanistic, and false — `prefix-rebuilds`
is 105k–158k on the lock arm against 56k–154k on MCAS. One column of an existing
report refuted it. **Sixth confident story killed by a control in two sessions.**

### ⭐⭐ THE TRAP THAT NEARLY LANDED IT ON THE WRONG CODE

The first self-diagnosing probe printed, for every offender:

    absent-at=3/4  want=/p0/p1/d13/S989  walk=/p0/p1/d13/S989

which reads as "the ancestors resolve, the LEAF is missing from its bucket" —
and would have sent me into `lru_evict_settled`'s two `bl_hlist_del_locked`
calls looking for a torn removal. **A path STRING cannot tell two dentries
spelled alike apart.** `dc_lookup` resolved one `d13` while `dc_walk` descended
the other, and both render identically. This is the same rule the
stale-`d_parent` UAF turned on — *when two things are spelled alike, check they
are the same OBJECT* — one level up, and the probe that settled it does not
argue: it enumerates a directory's child list and counts repeated names.

### ⭐ The new invariant is STRICTLY STRONGER than the census

`CHECK conservation` only notices once a leaf happens to be stranded under the
losing copy at the instant the run ends. The sibling-name scan notices the
duplicate itself. On the same mutation builds: census 1/8 vs names 3/8, and runs
that PASSED with duplicates sitting in the tree. It is now invariant 4, run
unconditionally, with `DC_DUPSCAN_SELFTEST` as its must-fail control (double-scan
each directory ⇒ every child is its own duplicate; it must report dups ==
children). ⭐ It printed `DUPNAME: none at any level (N children scanned)` on a
clean run from the start, so its silence is never confusable with not running.

### ⚠ It was also distorting the READER panel by ~3×

Same command line, 8 runs, `Mlookups/s` median: fixed 55.0 (MCAS) / 56.5 (lock)
against 17.8 / 16.9 with the re-check removed. The one mutation run that
finished with no duplicates measured 57.0 — on the fixed curve. So any
`--evict` churn reader number taken before this is suspect. ⚠ **Bounded:**
`rebuild_prefix` only runs when `--evict-cap` is set, so non-eviction panels
cannot have duplicates and are unaffected.

### The instability table that made it measurable (kept)

⚠⚠ **DO NOT CHARACTERIZE THIS AT SINGLE-DIGIT SAMPLES. The identical binary
gives contradictory pictures batch to batch**, and two wrong conclusions were
drawn from exactly that before this table existed:

| `df8a84a`, same binary, same command line | pass | consfail | timeout |
|---|---|---|---|
| batch of 8 | 3 | **0** | **5** |
| batch of 6 | 3 | **3** | **0** |
| batch of 20 | 12 | 2 | 6 |
| **pooled (34)** | **18 (53%)** | **5 (15%)** | **11 (32%)** |

The failure MODE flips between batches, not just the rate. On the 8-trial batch
this read as "the conservation anomaly is fixed, but I introduced timeouts"; the
6-trial batch says the opposite; the pooled figure says both modes are present
throughout.

⛔ **RETRACTED on the strength of the control:** "the fold fix put a transaction
on the reclaim thread and that caused the timeouts." The mechanism is REAL and
worth knowing — on the MCAS arm `lru_del_can_free(.., 1)` runs
`lru_dq_remove_seal` on `dc->lru_domain` from `fold_cb`, i.e. the `call_rcu`
thread, and `urcu_txn_begin` there can park in the fair-mutex FIFO behind the
writers — but it is not what is happening: `2420ba0` WITHOUT the fold fix
measured **4/6 timeouts against the fixed tree's 0/6**. "Can happen" is not "is
happening", and a plausible mechanism plus a single batch is how this session
produced three confident wrong stories in a row.

⛔ **ALSO RETRACTED:** "the conservation anomaly is gone." It is not. It was
3/8 at the original HEAD and it is 5/34 here; one batch of 8 happened to show 0.

**What was actually known then:** a real, intermittent, PRE-EXISTING conservation
failure on this configuration, which that session's work neither fixed nor
worsened. It was pre-existing because `rebuild_prefix` had been there for
several sessions; see "THE CENSUS ANOMALY" above for what it was.

### ✅ MEASURABLE NOW, and it named itself immediately

The harness was already more informative than the grep being applied to it: it
distinguishes STATE MISMATCH from CENSUS MISMATCH, and the timeout path sends
SIGTERM, which the bench handles with a full TXNSTATS + LRUCHK dump. Both were
being discarded by grepping for the verdict line. (Same error as grepping a
backtrace for the symbol I expected — twice in one session.)

Keeping the output, 24 trials: **9 of 11 non-passes are ONE signature**, and
STATE MISMATCH never fires:

    CENSUS MISMATCH: 0 missing, 1-2 extra, 0 stray, 0 dup

`d789a58` makes it self-diagnosing — the anomaly now prints the offending slot,
what `dc_lookup` says, and the owner's last op with its return:

    EXTRA gid=521 dir=9 lookup=0 id=... last-op=add last-ret=0

⛔ **RETRACTED before it was written down:** "extra is a harness accounting
artifact" — the owner clears `present[]` on `-ENOENT` from unlink, which under
eviction can mean THE PREFIX went rather than the leaf, and the reconciliation
pass only ever corrects present→absent, never the reverse. Good story; wrong.
The last op is a **successful add** (ret 0), reproducibly across three captures.

**What the signature actually says:** `dc_lookup` answers `DC_ABSENT` at the
slot's path while `dc_walk` reaches the id — and `census_cb` keys on ID and
ignores the path, so the entry is genuinely in the tree. `missing=0`, `stray=0`,
`dup=0` throughout. **The name-hash index and the child list disagree about one
entry.**

⛔⭐⭐ **AND HERE IS WHERE IT WENT WRONG — "IT IS THE MCAS LRU ARM, NOT THE
INDEX ENGINE" IS RETRACTED.** 6 trials each:

| arm | EXTRA | what 8 trials of the mutation build say |
|---|---|---|
| txn **lock** | 0/6 | 3/8 FAIL |
| txn **MCAS** | 1/6 | 2/8 FAIL |
| bucketlock **MCAS** | **3/6** | 1/8 FAIL |
| bucketlock **lock** | 0/6 | **4/8 FAIL** |

The right-hand column is the same defect measured on the arm the left-hand
column called clean — and it is the WORST arm, not a clean one. Six trials of a
~1-in-3 event ordered four arms by luck, the ordering looked mechanistic
("only under `-DDC_LRU_MCAS`, on BOTH engines"), and it was written down as a
place to start reading. **The rule this file already states — five trials cannot
call an arm clean — applies just as hard to five trials calling an arm GUILTY.**

⚠ Do NOT reconcile "extra" away in the harness to make the gate green — "extra"
is also exactly what a resurrect-after-delete looks like, which is why the
provenance print exists instead.

### Earlier note kept, because it is the same lesson one level up

⚠ A previous handoff said this configuration "hangs 3/3". It does not; that was
three trials of a stochastic outcome reported as if deterministic. The honest
figure is in the instability table above — and note that even the corrected
8-trial figure was itself unstable. **Every claim about this configuration that
rests on fewer than ~30 trials has been wrong so far.**

⛔ The escalation-lane paragraph that used to sit here — "the fix belongs in
liburcu: go offline across the park" — is RETRACTED; the bracket already exists
(`dcf1310c`). See the retraction section.

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

Two upstream fixes that stay: `43d35a7a` (`urcu_txn_deque_remove_prepare`
load-validates its derivation read) and `73236953` (the list convenience
brackets leaked the escalation lane on `-ENOENT` — five of them then, four now
that `move_tail_rcu` is gone).

⭐ The rule behind the first, stated properly, because the obvious version is
too strong and was applied to the list on that basis: **an operation that
DERIVES a slot from a read must have that read in its conflict set — but the
read itself need not be guarded if some slot the transaction WRITES is one that
every invalidating peer must also write.** On the list it is: every op that
rewrites `X->prev` also writes the old predecessor's `next` (`del`/`replace`
via the MARK), so `del`'s derivation is protected transitively by the
`&prev->next` store, and the validate there was removed as redundant. The deque
replaced that mark with `owner`, which is exactly what breaks the transitivity —
so it needs the guard and the list does not. ⚠ The invariant is now written at
the load in `rcu-txn-list.h`, because it is a property of the FILE, not of the
function, and losing it is silent.

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

**The session that CLOSED the census anomaly:**

- **"the census anomaly is the MCAS LRU arm, not the index engine"** — all four
  arms; the mutation build's worst arm is bucketlock-LOCK, which the 6-trial
  sweep scored 0/6 twice. Six trials cannot convict an arm any more than they
  can clear one.
- "the MCAS arm rotates in-use entries, so directories stay queued and get
  evicted more, so it rebuilds more" — `prefix-rebuilds` is 105k–158k on the
  LOCK arm vs 56k–154k on MCAS. Refuted by a column already in the report.
- "the leaf is missing from its bucket" (from `absent-at=3/4`, `want == walk`) —
  a path STRING cannot distinguish two dentries spelled alike. Both `d13`s
  render the same; the entry was under the OTHER one.
- "the duplicate check is safe because the churn workload is disjoint" (an
  in-tree comment) — true when written, falsified by `rebuild_prefix`.

**The session before it:**

- "txn MCAS 0/5" and "txn LOCK 0/5, 0 hits" — the probe was not in that file.
- "txn + MCAS + continuous hangs 3/3" — it does not hang; it intermittently
  fails conservation, on HEAD as much as here.
- "the shrinker's put-back is the pusher, so removing it is the fix" —
  `-DDC_LRU_NO_SHRINK_READD` changes NOTHING. The ownership was the fix.
- "the shard lock orders `lru_add` against `dc_unlink`" — it does not; the two
  sides pick DIFFERENT shards, and a killer that finds OFF takes no lock at all.
- "liburcu must go offline across the escalation-lane park" — **already done**
  (`urcu-txn-dev dcf1310c`). Carried as an open item across several handoffs and
  never once re-checked against the code; one `git log -S` retires it.
- "the txn shrink arm is blocked by that liburcu defect" — no: the ~1 GB/s OOM
  was MY OWN unthrottled sweeper allocating descriptors faster than `call_rcu`
  reclaims. All four arms are gates.
- "the fold fix put a transaction on the reclaim thread and caused the timeouts"
  — the mechanism is real, but the control without the fold fix measured **4/6
  timeouts against the fixed tree's 0/6**.
- "the conservation anomaly is gone" — one batch of 8 showed 0; it is 5/34.
- "the census `extra` is a harness accounting artifact" — the offending slot's
  last op is a **successful add**, 3/3 captures.

**Earlier:** "the `owner == NULL` node in the ring is a deque defect" (caller
bug); "evict-first fixes it" / "the default arm is clean" (bursty-only, stated
unqualified); "TSAN cannot gate the deque test" (it can — 16 real defects);
"evict-first ports to the lock arm" (SEGV); and, before the rewire, the
stale-prev forward rescan, the six-edge `move_tail` as cause, word-OFF-while-
linked, "plain stores exonerated" (a TIMEOUT counted as THE WEDGE).

## Method rules earned the hard way

⭐⭐ **THE DOMINANT FAILURE MODE OF THIS SESSION, five times over: reaching for a
mechanism that explains the data before checking whether the data is stable
enough to need explaining.** Every one of those five died to a control that cost
two minutes. Run the control FIRST — `git archive <base>` a clean tree and
measure both under one methodology.

⭐⭐ **DO NOT GREP FOR THE VERDICT WHEN THE TOOL PRINTS THE DIAGNOSIS.** Twice:
grepping a backtrace for `cds_fair_mutex_park` (the symbol my hypothesis
predicted) and stopping, when the full seven-thread dump named a different
thread entirely; and grepping runs for `RESULT:` while the harness was already
printing `STATE MISMATCH` vs `CENSUS MISMATCH` and SIGTERM-dumping
`TXNSTATS + LRUCHK`. Read the whole output before theorising about it.

⭐ **CHECK A NEW TEST ARM IS DOING WORK before believing its pass OR its
failure.** The first cut of the renames×shrinker arm emptied a 40-object
namespace in a few hundred iterations and then ran ~80000 no-op iterations. It
still caught the bug — on a workload that had stopped renaming.

- **Verify a probe is live before trusting its zero.** Four wrong negatives now.
- **Match the detector to the defect, and never read one tool's silence as
  coverage for another's finding.** ASan was clean 24/24 on a defect TSAN hit
  4-6 times in 8, because the allocator recycled the region first.
- **Five trials is not enough to call an arm clean** — and neither is forty at a
  low enough rate. The MCAS residual is ~2 in 64: it read as 0/5 for two
  handoffs, and a 40-trial mutation test returned 0/40 for the fix AND 0/40 for
  the control, i.e. the arm that had to fail passed.
- ⭐⭐ **NOR IS IT ENOUGH TO CALL AN ARM GUILTY.** The same six trials that
  cleared bucketlock-LOCK 0/6 convicted bucketlock-MCAS 3/6, and the ordering
  read as a mechanism ("only under `-DDC_LRU_MCAS`"). Under mutation the two
  arms are 4/8 and 1/8 — the ranking INVERTS. A small-sample sweep that produces
  a tidy story is the most dangerous output a harness has, because a tidy story
  is what stops you sampling more.
- ⭐⭐ **WHEN A PROBE ANSWERS IN NAMES, IT CANNOT ANSWER ABOUT OBJECTS.**
  `want == walk` and `absent-at=leaf` looked conclusive and pointed at the wrong
  file; two dentries spelled `d13` render identically. Ask the question in a
  form only one object can satisfy — here, "does any directory hold two children
  of one name", which needs no path at all.
- ⭐ **A COMMENT'S PRECONDITION EXPIRES.** "racy, but the workload is disjoint"
  was true when written and false once `rebuild_prefix` landed, several sessions
  later, in a different file. Grep for the callers a safety comment names,
  rather than trusting that it still describes them.
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

    make check                # 1 PASS (394 checks, 0 failures)
    make check-bucketlock     # 8 PASS
    make check-churn          # 3 PASS  (no eviction: see check-churn-evict)
    make check-churn-evict    # NEW: 8 runs + 1 must-fail control
    make check-lru-arms       # 15 PASS  (incl. the 4 renames-x-shrinker arms)
                              # ⚠ INTERMITTENT: the MCAS stress arm can still
                              # wedge on the lane (see the 2026-08-06 section)
    make check-deque          # 10 arms: 9 PASS + 1 must-fail control
    make check-deque-tsan     # 4 arms, 0 warnings

Plus: `-Wall -Wextra` clean across {bucketlock, txn} × {lock, MCAS, DC_NO_LRU} ×
{seal, no-seal, no-seal + re-arm} × {±`DC_NO_ADD_DUP_RECHECK`}.

⭐ **`check-churn` COULD NOT HAVE CAUGHT THE DUPLICATE DEFECT and never could
have** — it sets no `--evict-cap`, so `rebuild_prefix()` never runs, so the
workload never issues concurrent same-name creates. That is why
`check-churn-evict` exists: all four engine×LRU combinations, both cadences,
plus `DC_DUPSCAN_SELFTEST` as the detector's must-fail control. ⚠ One run per
arm catches only ~half (invariant 4 fires 3-5/8 per arm under
`-DDC_NO_ADD_DUP_RECHECK`); it is a screen, not a proof. Re-run it when touching
`dc_add`, with the mutation flag as the arm that must fail.

⚠⚠ **RUN EVERYTHING UNDER A MEMORY CAP** (`scratchpad/capped <max> <secs> cmd`).
A session was lost to OOM during this work. ⭐ And check WHOSE allocation it is
before blaming your own: the top consumer on this box was 28 concurrent
`test_urcu_ft_inv` processes from a DIFFERENT tree (`userspace-rcu`,
`scratchpad/ss/run.sh`) holding 96 GB and climbing. `ps -eo rss,comm --sort=-rss`
answers in one command what an afternoon of theorising does not — the same
lesson as the seven-thread dump.

⚠ **`make check-lru-arms` now runs a sweeper.** If you add another sweeper arm,
give it a CADENCE and cap its memory — an unthrottled `dc_shrink` loop allocates
transaction descriptors faster than `call_rcu` reclaims them and will take the
machine down (it did). `scratchpad/capped` wraps a run in
`systemd-run --user --scope -p MemoryMax=… -p MemorySwapMax=0`.

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

⚠ Use an ABSOLUTE `-Wl,-rpath` when the binary goes anywhere but this directory.
The relative one above makes it exit 127 from a scratch dir — which a loop that
records only `RESULT:` reads as eight silent failures.

Build arms: `-DDC_TXN_PARENT_SEAL` (txn parent UAF closed via the child-head
seal; OFF by default, **0/20 timeouts** — the one to promote, see above);
`-DDC_TXN_PARENT_GUARD` (the older guard pair; OFF, **9/20** — kept as the A/B
partner); `-DDC_TXN_PARENT_DELAY` (200 µs reproducer that makes the parent UAF
fire; **never ship**, and its control must fire before any zero is believed);
`-DDC_NO_ADD_DUP_RECHECK` (mutation arm for the duplicate-name fix, BOTH
engines).

Harness knobs: `DC_DUPSCAN_SELFTEST=1` (must-fail control for invariant 4).

Probe flags: `-DDC_LRU_FREE_ASSERT` (BOTH engines now), `-DDC_LRU_PUSH_DELAY`
(the targeted reproducer), `-DDC_LRU_MCAS_RETAIN_READD` (its mutation arm),
`-DDC_LRU_NO_DEAD_SEAL`,
`-DDC_LRU_NO_SHRINK_OWN`, `-DDC_LRU_NO_SHRINK_READD`, `-DDC_LRU_NO_RETAIN_READD`,
`-DDC_LRU_READD_LEGACY`, `-DURCU_TXN_DEQUE_NO_SEQ_GUARD`, `-DRETIRE_AUDIT`.
