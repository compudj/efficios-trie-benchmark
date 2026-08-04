# Handoff — the LRU-onto-deque rewire is DONE; a PORT-WIDE defect is open

> ⚠ Read the **2026-08-04 (later)** section at the end FIRST: it supersedes two
> conclusions in the middle of this file.

2026-08-04. Supersedes the pre-rewire version of this file. Everything below is
committed.

## What landed

The MCAS LRU now runs on `<urcu/rcu-txn-deque.h>`. `rcu-txn-list.h` is no longer
involved in the LRU at all.

- `d_lru` on the MCAS arm is `struct urcu_txn_deque_node dnode` and **nothing
  else**: `shard` is gone (it survives only on the lock arm). Membership is
  `urcu_txn_deque_owner()`.
- The claim protocol is deleted — `lru_claim` / `lru_unclaim` /
  `lru_del_claimed` / `lru_unlink_claimed`, `DC_LRU_BUSY`, and `DC_LRU_OFF` /
  `DC_LRU_ON` on the MCAS arm. `DC_LRU_OFF`/`DC_LRU_ON` are `#ifndef
  DC_LRU_MCAS` so a surviving use fails to compile rather than quietly
  reintroducing a second membership record.
- `lru_move_tail` is gone; the sweeper's second chance is
  `urcu_txn_deque_rotate_head()`.
- `dc->lru[i]` is a `struct urcu_txn_deque`; its own approximate `count` is the
  only counter, so there is nothing left to drift against it.
- Evict-first is kept. `dc:claim` / `dc:wedge` tracepoints are gone (they
  instrumented a protocol that no longer exists); `dc:commit` stays.
- `DC_TS_LRU_EVICT` was dead; it is now `DC_TS_LRU_ROT` and counts rotates.

**The decision the old handoff asked to make first**, now stated in
`dcache_lru_shrink.h`: with no claim, two sweepers can both reach
`lru_evict_settled()` for one victim. Safe — bucketlock re-verifies the hlist
mark under `bl_lock2`, txn's `hlist_del_prepare` answers `-ENOENT` and two
commits on one slot cannot both win — but it **moves the serialization point
from the LRU word to the eviction**.

Gates green: `make check`, `check-bucketlock`, `check-lru-arms` (9 PASS + 2 ASan
stress), `check-deque` (8 arms), `check-deque-tsan` (4 arms, 0 warnings).
390/394 checks, 0 failures.

⚠ **BEFORE ANY TSAN RUN, CHECK THE TSAN TREE IS CURRENT.** `urcu-txn-tsan-build`
is a *copy* of `urcu-txn-build`, and it silently rots:

    md5sum urcu-txn-tsan-build/include/urcu/rcu-txn-mcas.h \
           urcu-txn-build/include/urcu/rcu-txn-mcas.h

`check-deque-tsan` refuses to run when they differ. It cost a whole wrong
conclusion this session. Rebuild recipe: the comment above `stress-tsan`.

⚠ Also known: `urcu-txn-build/include/urcu/rcu-txn-slab.h` is behind the dev
tree (the dev version adds `__attribute__((aligned(64)))` to the stats counter
block, which changes `struct urcu_slab`'s layout **unconditionally** — it is not
behind an ifdef). The build tree is self-consistent (old header, old library) so
this session's numbers are sound, but syncing that header REQUIRES rebuilding
the library with it or the .so and callers disagree about field offsets.

## RESOLVED: the legacy arm's collapse is a CALLER bug

⛔ An earlier revision of this file called it "a structural violation the deque's
contract forbids". **That was wrong.** The five-minute witness that settled it:
`-DDC_LRU_FREE_ASSERT` checks, in `dentry_free_cb`, whether the dentry is still
queued when its storage is released.

    default (evict-first)     FREE-WHILE-QUEUED  0 / 5
    -DDC_LRU_READD_LEGACY     FREE-WHILE-QUEUED  3 / 3

A dentry is freed while a deque still points at it; the storage is reused, the
next dentry memsets it (`owner` NULL, links NULL), and the neighbours still name
it. THAT is the "ring reaches an owner==NULL node" — a consequence, not a cause.

Which re-add? `DC_LRU_NO_READD` gated two sites at once, so every result from it
was uninterpretable. Split into `DC_LRU_NO_SHRINK_READD` /
`DC_LRU_NO_RETAIN_READD`:

| shrinker put-back | lru_retain re-arm | hits |
|---|---|---|
| ON | ON | 3/3 |
| **OFF** | ON | **0/3** |
| ON | **OFF** | 3/3 |

It is the **shrinker's own put-back**: it removes the victim, holds it by RCU
alone, a concurrent `dc_unlink` finds the node already off (so its `lru_del` does
nothing) and `call_rcu`s it, and the sweeper then pushes a dentry pending free
back onto a deque. That is precisely the window **evict-first closes**. The
legacy shape is not merely slower — it is unsafe for this caller, whatever
structure the LRU is built on.

⚠ **"which is why the default arm is clean" — SUPERSEDED.** That was measured
under `--evict bursty` only. Under `--evict continuous` the default arm fires
too, through a SECOND pusher (`lru_retain`'s re-arm) that this 2x2 could not see
because bursty's window is too narrow. And the LOCK arm fires under BOTH
cadences. See the 2026-08-04 (later) section at the end of this file.

The remaining half of the legacy collapse (lane holder moving, everyone parked,
call_rcu worker in `synchronize_rcu()`) is escalation-lane starvation plus the
park-while-online QSBR stall, which is a **liburcu** matter. Still open, still
not this file's.

## The deque is tested along both axes now

`test_deque.c` covers many deques (migration, `owner` naming the right one,
remove deriving from a hint) and reuse (retire + re-init in place, resetting
`seq`). `make check-deque`: 8 arms, all PASS — 2/8/32 writers × 4 deques, 1 deque
(single-ring regression), 16 deques, `NO_SEQ_GUARD` mutation, ASan, harness
self-check.

Three things worth keeping from building it:

- **The harness was wrong first**, and in the instructive way: its retire waited
  for a grace period BEFORE removing the node but not AFTER. A rotator holding
  the node as the head then read `h->next` out of storage the retirer had just
  zeroed — a NULL deref inside `rotate_head_prepare` that looked exactly like a
  deque defect. Reuse needs the GP *after* the node is off; `call_rcu` gives the
  dcache that for free. 7/8 SEGV before, 16/16 after.
- **The audit masked it** (8/8 PASS with it on). It is opt-in now
  (`-DRETIRE_AUDIT`) and one arm runs it. A probe that widens every window is not
  a control.
- **⚠ The `seq` guard is STILL UNPROVEN.** Reuse was the hypothesis that would
  make it load-bearing; compiled out at 32 writers with reuse on, the test still
  passes. And reuse *resets* `seq` to zero, so its stated premise ("never
  decreases") does not survive recycled storage: it is monotone per membership,
  not per address. Worth a header caveat, not yet written.
- **⛔ "TSAN cannot gate the deque test" was WRONG.** Retracted: `make
  check-deque-tsan` is **0 warnings**. The original claim rested on 20-38
  reports blamed on TSAN not modelling a QSBR grace period. In fact (a) the TSAN
  liburcu was a **Jul 31 copy against an Aug 3 engine**, so those runs exercised
  a different engine from every other gate; `--enable-compiler-atomic-builtins`
  was set and TSAN *was* instrumenting the atomics (`__tsan_atomic64_load` in
  the output), so the flag was never the issue — the staleness was. And (b)
  after a correct rebuild all 16 surviving reports were **real defects in the
  harness**: 15 from resolving proxies off `CMM_RELAXED` loads (resolving a
  proxy dereferences the writer's descriptor, so the slot load must be ACQUIRE
  or there is no happens-before to that descriptor's init) and one from
  `volatile int g_stop`, which is not atomic.
  ⭐ The control that settled it: `make stress-tsan` on the rebuilt tree is
  clean, so the engine was not the source and the reports had to be this test's.

## The former open defect — kept for the reasoning, superseded above

`deque-design.md` predicted the `DC_LRU_READD_LEGACY` control could be deleted
once the deque landed. **That prediction is refuted.** The shape is still
expressible (remove, then push) and it still collapses 0/5.

The residual is a **different failure from the list's**, and confusing the two is
exactly the trap this project fell into before:

| | list (old) | deque (now) |
|---|---|---|
| lane holder | stuck on a CAS that can never land | **moving** (3 gdb samples 2s apart: `remove_prepare` → `urcu_txn_end` → `urcu_txn_sort_recs`) |
| other writers | parked | parked in `cds_fair_mutex_park` |
| call_rcu worker | in `synchronize_rcu()` | in `synchronize_rcu()` |

So the starvation half (escalation lane + park-while-online under QSBR) survives,
and that half is a **liburcu** matter, not this file's.

But it arrives with a structural violation the deque's contract forbids:
`dc_lru_validate` reports a shard whose ring reaches a node with `owner == NULL`
(`first-bad=2`, `walked=9 count=13`). Only three writers touch a node's `next` —
push (which also sets `owner`), rotate, and remove's `&prev->next : n -> next` —
so a remove installed an edge naming a node that had **already departed**, i.e.
its `next` read was stale despite `load_validate` and the per-node `seq` guard.

The default arm is clean on the same measurement: 3 trials, ~10M `lru_del` each,
**0 escalations, maxretry 3, 0 disowned nodes in any shard**. So this is not
"the deque is broken"; it is "the remove-then-push shape reaches a state the
deque test does not cover".

**Where to start**: `test_deque.c` hammers **one** deque over a **static** node
array. The dcache has many shards (a node can migrate between deques) and frees
nodes via `call_rcu` (memory is reused). Those are the two uncovered axes. Also
re-run the `-DURCU_TXN_DEQUE_NO_SEQ_GUARD` mutation test here — the guard was
committed on argument and is still unproven, and this workload is the first that
might actually exercise it.

⚠ `disowned` is an **amplified** count, not a defect count: a removed node keeps
stale links by design, so one bad edge sends the walk off the live ring for as
many hops as those links chain. `first-bad` is the localiser.

## Measured facts — do not re-derive

`--writers 8 --readers 8 --evict bursty --evict-cap 32`, bucketlock engine:

| arm | completion | Mchurn/s |
|---|---|---|
| deque, evict-first (default) | 5/5 | 2.50 2.53 2.54 2.51 2.50 |
| deque, `-DDC_LRU_READD_LEGACY` | 0/5 (timeout 25-30s) | — |

⚠ Those Mchurn/s are **not** comparable to the pre-rewire 1.39-1.42 figure: the
command line behind that number was never recorded. A list-vs-deque throughput
claim needs both arms re-run under one command line, and nobody has done that.

## Two upstream fixes that stay (independent of the LRU)

- `750572af` — `urcu_txn_list_del_prepare` load-validates its derivation read of
  `&elem->prev`.
- `b69b4a53` — all five list convenience brackets leaked the escalation lane on
  their `-ENOENT` terminal bail (no `urcu_txn_abandon()` before `end()`).

⭐ The general rule behind both: **an operation that READS a slot it does not
WRITE must validate that read.** `insert_before`/`move_tail` write the slot they
derive from, so they need nothing; `del`/`remove` do not.

## Claims retracted before the rewire — do not resurrect

- the stale-prev *forward rescan* (`prev_repair`) — its termination test walks
  from a hint that may itself be off-ring.
- "a corrupt ring / cycle without the sentinel" on the list — `closed=1
  marked=0 count==walked` every time.
- the six-edge `move_tail` as the cause — `DC_LRU_NO_MOVE`, 6/6 still collapsed.
- word-OFF-while-linked — the claim-site assertion never fired.
- "plain stores exonerated" — a TIMEOUT was counted as THE WEDGE. They differ.
- "the slab lfstack pop is spinning" — one gdb sample of a thread passing
  through; four samples showed it moving.
- "the deque reproduces the dcache wedge at engine level" — same symptom,
  different cause.

## Method rules earned the hard way

- **Grep for the actual violation marker, not the exit status.** A timeout is not
  the wedge.
- **One gdb sample is not a spin.** Take three, two seconds apart, and check
  whether frames move. (That is what separated the two failures above.)
- **Run the control under the same methodology.** The disowned-node finding is
  only meaningful because the default arm was SIGTERM'd mid-run through the same
  walk and came back clean.
- **Verify a probe is live before trusting its zero.** Print on the FIRST call
  and run a with/without control. Bitten 3x.
- **Mutation-test every new guard.** If removing it changes nothing, the test
  does not exercise it; say so instead of claiming a fix.
- **The bench compiles against a COPY** of the urcu headers in
  `urcu-txn-build/include/urcu/`. Editing the dev tree alone changes nothing —
  `md5sum` both after every edit. Cost two measurement rounds.
- `sleep` is blocked in the agent's bash tool; a bare `for` loop is
  instantaneous. Use `python3 -c "import time; time.sleep(N)"`.

## Build recipes

    # deque test (engine level)
    gcc -O2 -g -pthread -march=native -DNWRITERS=16 \
        -I$U/include -I. -o t test_deque.c \
        -L$U/src/.libs -lurcu-qsbr -lurcu-common -lrseq -lpthread

    # the dcache arms (bucketlock engine)
    CPP="-I../../urcu-txn-build/include -I. -DDC_SPLIT_KEEPID"
    cc -O2 -g -pthread -march=native $CPP -DDC_MARK_GEN -DDC_LRU_MCAS \
       [-DDC_TXN_STATS] [-DDC_LRU_READD_LEGACY] \
       -o churn bench_dcache_churn.c dcache_bucketlock.c \
       -L../../urcu-txn-build/src/.libs -Wl,-rpath,../../urcu-txn-build/src/.libs \
       -lurcu-qsbr -lurcu-common -lrseq -lpthread

    # counters + the ring dump out of a wedged process
    ./churn --writers 8 --readers 8 --duration 1000 --evict bursty --evict-cap 32 &
    kill -TERM $!     # the stats handler dumps TXNSTATS + LRUCHK and _exit(3)s

---

## 2026-08-04 (later): re-measured, and the picture is WIDER than "the deque"

Question asked: is the MCAS deque still collapsing? Direct answer: **not under
the cadence I had been testing, and I had only been testing one.**

`--evict bursty`, bucketlock engine, MCAS deque, 5 trials per point:

| writers | 8 | 16 | 32 | 48 | 96 | 192 |
|---|---|---|---|---|---|---|
| complete | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |
| Mchurn/s | 2.44-2.60 | 3.40-3.61 | 3.98-4.19 | 3.41-3.61 | 1.81-1.89 | 0.98-1.02 |

30/30 complete, and `-DDC_LRU_FREE_ASSERT` fires **0** times at 8/32/96/192.

Then I ran the OTHER cadence, and the whole framing changed:

| engine / LRU arm | `--evict bursty` | `--evict continuous` |
|---|---|---|
| bucketlock, MCAS deque | clean 30/30, 0 hits | **FREE-WHILE-QUEUED** |
| bucketlock, LOCK | **FREE-WHILE-QUEUED 3/3** | **FREE-WHILE-QUEUED** |
| txn, MCAS deque | — | **hangs 3/3** (escalation lane) |
| txn, LOCK | — | 2/3 ok, 1 SEGV, 0 hits |

### ⛔ So it is a PORT-wide defect, not the deque's, and not even MCAS's

**The LOCK arm — the honest A/B control — is the worst affected**: it frees
linked dentries under BOTH cadences. Verified with a second, independent
witness, not just the state word: the free callback walks the shard chain under
that shard's lock and reports `chain-reachable=1`, with `prev=(nil)`, i.e. the
victim is the shard HEAD.

One root cause, two entry points. Nothing prevents a dentry from being
**(re-)added to the LRU after it has been evicted/unlinked and handed to
call_rcu**. Mainline is protected by the refcount — `dentry_lru_isolate` holds
one — and this port deliberately has no refcount (readers take none) and never
replaced that protection on the LRU side.

    pusher 1  lru_retain's re-arm          both arms; the ONLY one on the
                                           MCAS default arm.  Disabling it
                                           (-DDC_LRU_NO_RETAIN_READD) takes the
                                           MCAS arm to 9/9 clean under
                                           continuous.
    pusher 2  the shrinker's put-back      lock arm ALWAYS (unlink, try evict,
              after a failed evict         link_tail on failure); MCAS legacy
                                           arm.  Disabling pusher 1 does NOT
                                           help the lock arm -- 3/3 still.

### What this corrects

- "evict-first fixes it" (`c404d80`, and this file's earlier revision) is
  **incomplete**. Evict-first closes pusher 2 on the MCAS arm; pusher 1 is
  untouched and only shows under continuous eviction.
- "the default arm is clean" was true **for bursty only**, and I did not say so.

### Not corruption: the txn + MCAS + continuous hang

3 gdb samples 2s apart: 47 of 48 writers in `cds_fair_mutex_park` every time,
the 48th moving (`urcu_slab_alloc` -> `sysmalloc` -> `pthread_mutex_lock`), no
assert. That is the known escalation-lane starvation, not a structural failure.

### Next decision (NOT taken -- it is a design call)

Both pushers need the same thing: a way to know a dentry is still alive before
adding it to the LRU. Candidates, cheapest first:

1. **Drop `lru_retain`'s re-arm on the MCAS arm.** The shrinker there ROTATES
   in-use entries, so the header already says this path is "reachable only after
   an allocation failure" -- it is nearly dead code that costs a real bug.
   Measured cost: none visible; measured benefit: 9/9 clean under continuous.
2. **Gate every push on "still hashed"**, the port's stand-in for a refcount.
   Fixes both pushers and the lock arm, but the check has to be atomic with the
   push or it is just a narrower race.
3. **Give the shrinker a real reference.** Faithful to mainline, and the biggest
   change.
