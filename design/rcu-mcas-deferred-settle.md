# rcu-txn MCAS: a lock-free engine by deferring the settle past a grace period

Design analysis for a helping / abort-others variant of the multi-writer engine
(`<urcu/rcu-txn-mcas.h>` in userspace-rcu-txn) that is *lock-free*, in which the
install-time A-B-A is closed neither by a sole driver (today's engine) nor by a
per-record install latch (the engine retired in `a07b67ba`), but by **not
writing a plain value back into a slot until a grace period has elapsed** --
the technique of Guerraoui, Kogan, Marathe and Zablotchi, *Efficient Multi-word
Compare and Swap*, DISC 2020 (arXiv 2008.02527). Companion to
[rcu-mcas-install-gate](rcu-mcas-install-gate.md), whose latch this would have
made unnecessary, and to [mixed-sw-mw-txn](mixed-sw-mw-txn.md). 2026-10-03.

**Status: design only. Nothing here is built, measured or proven.** It is
written to be reviewed before any code is touched, and it is deliberately not
being prototyped yet.

**It gates P2.** Mathieu, 2026-10-03: this is to be investigated before the
sole-driver paper is published. P2 argues that bounded-blocking is the right
target; that argument currently rests on a measurement against the *latched*
helping engine and on a design argument against this one, which was never built.

Verdict up front, in three parts:

- **The premise holds.** Deferring the settle closes the A-B-A under helping
  with no latch, no RDCSS and no version bits, at `k+1` compare-and-swaps. The
  helping engine that lost to sole-driver (+22.0% at 192 writers) paid `4k+1`.
  That A/B says little about this design.
- **Writers need not stall for the grace period**, provided a *decided*
  descriptor stops being a lock: install over it. Today's engine waits on any
  proxy (§3.2), so this is a real change, not a flag.
- **There is one problem that could sink it, and it is not the reader
  indirection.** A deferred detach is the first write to a transacted slot that
  happens *outside* the read-side critical section its transaction ran in. The
  embedder's lifetime contract -- reclaim transacted memory through `call_rcu`
  -- no longer covers it (§5). The eager settle has no such write. This needs an
  answer before the first line of the prototype.

## 1. Why this is open again

### 1.1 What was measured, and what was not

| Engine | Install A-B-A closed by | CAS per uncontended k-slot commit | Progress | Built here |
|---|---|---|---|---|
| Harris-Fraser-Pratt 2002 | RDCSS | 3k+1 | lock-free | no |
| helping + latch (`-DURCU_MCAS_STOCK`, retired) | per-record FREE/BUSY/DONE | 4k+1 | bounded-blocking | yes |
| **helping + deferred settle** (Guerraoui 2020) | unlock behind an epoch | **k+1** | **lock-free** | **no** |
| sole-driver (shipped) | no second driver | k (+ k+1 stores) | bounded-blocking | yes |

The `4k+1` is [rcu-mcas-install-gate](rcu-mcas-install-gate.md) §1's count:
plant slot CAS, plant latch CAS, settle claim CAS, settle slot CAS, plus the
status CAS.

Two existing numbers bear on what the missing row might do, and neither is a
measurement of it:

- Sole-driver over the latched helping engine: **+22.0%** at 192 writers
  (`bench_txn_3skiplist`, both arms at `97443472`,
  `scripts/p2_engine_remeasure.csv`).
- The latched helping engine with its two latch CASes compiled out
  (`-DURCU_MCAS_NO_ABA_FIX`, A-B-A-**unsafe**, a regression instrument): **+15.3%
  to +16.6%** at 192 writers ([rcu-mcas-install-gate](rcu-mcas-install-gate.md)
  §2, engine `0b22e8b9`).

Read together, most of what sole-driver won was the latch, and a latch-free
helping engine could land within a few percent of it *on that writer-only
benchmark*. That is an inference across two commits and two sessions, with a
settle CAS still on the critical path of the instrument. It is a reason to
measure, not a result.

### 1.2 What the latch was for

The hazard, on a list `A -> B`, with `T` inserting `X` after `A` (record
`A->next: B -> X`) and `H` helping `T`:

1. `H` loads `A->next`, finds the plain value `B`, prepares its CAS.
2. `H` stalls.
3. `T` is completed by its owner, and **settles**: `A->next` holds plain `X`.
4. Another transaction removes `X` and **settles**: `A->next` holds plain `B`.
5. `H`'s CAS succeeds. `A->next` names `T`'s record, `T` is SUCCEEDED, and the
   slot resolves to `X`: a removed node is back in the list.

`H` is inside a read-side critical section throughout, so nothing was freed;
the structure is simply wrong. The latch cut the trace at step 5. Sole-driver
cuts it at step 2. **Deferral cuts it at steps 3 and 4:** if a decided
transaction leaves its proxy in the slot and a plain value is written back only
after a grace period, `A->next` cannot hold plain `B` again while `H` is still
inside the critical section in which it loaded it.

### 1.3 The goal question (Mathieu, 2026-10-03)

An engine close to the published algorithm would be *truly lock-free*, at the
cost of overhead: a progress-guarantee-for-throughput trade. And the series'
throughput target is already met elsewhere -- P1's plain stores under the
embedder's fine-grained locks. So what P2 is *for* is open again: **lock-freedom
may be a more appealing goal for the multi-writer facility than
bounded-blocking.**

That gives the series three points, not two:

| | Writer synchronization | Per k-slot commit | Progress | Slot after commit |
|---|---|---|---|---|
| P1, single-writer | embedder's locks | 0 CAS, 2k+1 stores | blocking (the embedder's locks) | plain |
| sole-driver | none | k CAS, k+1 stores | bounded-blocking | plain |
| deferred settle | none | k+1 CAS, detach later | lock-free | proxy, for a grace period |

The middle row is the one whose niche narrows. It is neither the cheapest (P1
is) nor lock-free (the third row would be). What it keeps over the third row is
a plain steady state for readers, a one-grace-period lifetime contract (§5), a
decision that is a store, and one CAS fewer. Whether that is a paper or a
measured variation of one is what this investigation decides (§9).

## 2. Prior art, and a constraint on building it

The mechanism below is Guerraoui et al.'s, not ours. What is in their paper:
plain-CAS install; decided descriptors left in place; install-over; detach by
CAS during reclamation; reclamation by thread-local epochs "similar to RCU" with
no reference count, in two stages; readers that do not write unless they meet an
in-flight operation, which they then help; a lower bound of `k` CASed locations
for any lock-free disjoint-access-parallel k-CAS.

What their paper does not settle, and we would owe ourselves:

- **Their proof does not cover the detach.** Appendix A.2 proves the algorithm
  "under the assumption that no memory is ever reclaimed" (their §6); Lemma 10
  says a location once acquired is never un-acquired. Detach is the one
  transition back to a plain value. Its safety is argued informally.
- **Their pseudocode's epochs do not nest.** `readInternal` helps by calling
  `MCAS(parent)`, which runs its own `epochStart()`/`epochEnd()`; under a parity
  scheme a helper in a nested call reads as quiescent exactly while it holds a
  stale read. liburcu's read-side critical sections nest, so this does not
  transfer -- but it is the kind of thing an implementation gets wrong.
- **They do not discuss freeing the memory the target words live in** (§5).

**Before prototyping: re-read the Oracle patent family** that the articles
tree's patent sweep lists for MCAS (US 10,824,424 / 11,216,274; the paper's
authors were at Oracle Labs, so presumably this algorithm -- not confirmed).
That sweep recorded one distinguishing element for the *sole-driver* engine: the
claim requires writing the status by CAS, and sole-driver writes it with a plain
store. An engine with helpers decides by CAS (§3.3). So moving toward
lock-freedom moves toward that claim language, not away from it. This is a
question for Mathieu and counsel, not a conclusion; it is recorded here because
it bears directly on whether this engine can ship in liburcu and on what a
defensive publication of it would be worth.

## 3. The mechanism

Unchanged from today: the descriptor, the tri-state status word
(UNDECIDED / SUCCEEDED / FAILED, written once), the frozen record set, the
shared resolve header, the per-record tag, and the reader's resolve path.

### 3.1 The invariant everything rests on

**I1 (no content recurrence inside a critical section).** Let a thread load the
raw content `c` of a slot inside an RCU read-side critical section. If the slot's
content later differs from `c`, it does not return to `c` before that critical
section ends.

Content is either a plain value or the tagged address of one record. I1 holds if
both of these do:

- **I1a.** A plain value is written into a slot that held a proxy only by
  *detach*, and a record is detached only after a grace period that began after
  its transaction was decided.
- **I1b.** A record's tagged address reappears in a slot only by a second plant
  of that record or by reuse of its descriptor's memory. A second plant would
  need the planting driver's own expected content to have recurred first, so the
  *earliest* recurrence is never of this kind (the induction of the paper's
  Lemma 12). Reuse is excluded because a descriptor's memory is recycled only
  after a second grace period, which begins after its detach.

Given I1, a stalled driver's late CAS fails whenever anything happened to the
slot since it looked. That is the whole A-B-A argument; §4 says what it still
needs.

### 3.2 Plant: install over a decided proxy

Today any foreign proxy is a lock. `urcu_txn_install_mw_depth()` spins
`while (urcu_txn_is_proxy(slot))`, and the age-0 path's bare
`CAS(slot, old_ptr, proxy)` fails on every proxied slot. With a deferred settle
that would stall a writer for a grace period. The plant becomes:

    for (;;) {
        c = load(slot);
        if (c == tagged(r))                 /* planted, by me or a helper */
            break;
        if (is_proxy(c)) {
            f = untag(c);
            if (status(f->desc) == UNDECIDED) {
                conflict(f->desc);          /* help, abort, or wait: §6 */
                continue;
            }
            v = resolve(f);                 /* decided: as good as a value */
        } else {
            v = c;
        }
        if (v != r->old_ptr) { decide(t, FAILED); return; }
        if (status(t) != UNDECIDED) return; /* someone finished t */
        if (CAS(slot, c, tagged(r)))        /* expected = RAW content */
            break;
    }

The only lock is an *undecided* proxy. A decided one is a value with an
indirection, and the next transaction replaces it without waiting for anyone.
Costs: a load before every CAS, and a resolve (two or three dependent loads) on
a proxied slot.

### 3.3 Decide

A CAS, `UNDECIDED -> SUCCEEDED | FAILED`, because the owner is no longer the
only thread that may write its status. One extra CAS per commit over
sole-driver; uncontended in the common case.

### 3.4 Detach, and the second grace period

No settle inside the commit. The descriptor is retired to `call_rcu()`. After
the first grace period the callback detaches:

    for each record r of t:                 /* ALL of them, not a prefix */
        CAS(r->slot, tagged(r), status(t) == SUCCEEDED ? r->new : r->old);

then re-queues the descriptor, and it is freed after a second grace period.

- The CAS fails harmlessly if a later transaction installed over the record.
- It must scan every record. With helpers the owner does not know which records
  were planted, and a helper may plant a record of a FAILED transaction late --
  harmless, it resolves to the old value, but it is a proxy the detach must find.
  That late planter is inside a critical section that began before the decision,
  so the first grace period waits for it.
- The second grace period is for readers that loaded the proxy just before the
  detach.
- **This is the write §5 is about.**

### 3.5 Readers

Unchanged, and this is a place to do better than the paper. Their reads help an
in-flight operation. Ours resolve an UNDECIDED record to its old value at once
and never drive anything: a reader stays a pure reader. What changes for the
reader is only *how often* it meets a proxy (§7).

## 4. What the A-B-A argument still needs

- **Every driver's load-to-CAS window inside one read-side critical section.**
  Owners and helpers alike. Under QSBR: no quiescent state between the load and
  the CAS, at any nesting depth of helping.
- **No other path that writes a plain value into a transacted slot.** No eager
  settle on abort; an aborted transaction leaves FAILED proxies and takes the
  deferred path like any other. Restoring the raw content it replaced is not an
  option either: the descriptor it would restore may already have run its
  detach and concluded it was superseded, and would then be freed while a slot
  names it.
- **A proof of I1 that covers detach.** Theirs does not (§2).
- **A test that builds the shape.** A stalled helper holding a stale plain
  value, a recurrence of that value's *logical* content, then the late CAS --
  with a hook that widens the load-to-CAS window. Zero occurrences in a stress
  run is not coverage. `test_rcu_mcas_aba` and `test_rcu_mcas_republish` were
  the latched engine's gates; this design needs their equivalents.

## 5. The problem that could sink it: detach outlives the read-side section

Today every write to a transacted slot -- plant, settle, and a loser's
settle-back -- happens inside the read-side critical section the attempt runs
in. That is what lets one grace period cover both *free* and *reuse* of the
slot's backing memory: the contract is "reclaim transacted memory through
`call_rcu`", and the callbacks never touch a slot.

The deferred detach breaks that. It runs from a callback, a grace period after
the decision, in no critical section that has anything to do with the slot.

    T1 commits a record on a slot in node N.          (descriptor D1 retired)
    T2, another thread, unlinks N and call_rcu()s it.
    Both wait a grace period. Different call_rcu queues, different workers,
    or a batched retire: N's free runs BEFORE D1's detach.
    D1's detach does CAS(&N->slot, ...) on freed memory.

The same holds for a *loser*: a transaction that aborts after planting in a
node that is concurrently removed leaves a FAILED proxy there, and its detach
comes a grace period later still. The CAS would almost always fail its compare,
but it is a read and a locked write to memory that may have been returned to the
allocator, reused, or unmapped.

Batched descriptor retirement (the default since `c21f5a38`) makes it worse: the
batch's single `call_rcu` is issued later than the commit.

Candidate answers, none chosen:

1. **Type-stable slot memory.** If transacted memory stays mapped and stays
   *slot* memory after it is freed -- an embedder slab that recycles nodes only
   as nodes -- a late detach is a CAS whose compare fails: the proxy's address
   is unique until its descriptor's second grace period, and a recycled node's
   slots were re-initialized by plain stores. This is a new, real contract on
   the embedder, and jemalloc-backed nodes do not meet it.
2. **Order frees behind detaches inside the engine.** Reclaim transacted memory
   through an engine entry point that guarantees every detach which may name a
   slot has *run* before that slot's memory is freed. Per-CPU queues do not
   order across each other, so this is a generation barrier: an engine-owned
   reclamation layer, not raw `call_rcu`. A single shared queue would do it and
   would be the reclaim-thread ceiling the harness rules forbid.
3. **Detach only from inside a critical section that reached the slot.** The
   callback only *marks* the descriptor "grace period elapsed"; whichever thread
   next meets that proxy while traversing the structure may detach it. Lifetime
   is then covered by the visitor's own critical section. But the descriptor
   stays pinned until every one of its slots has been visited or has died, so
   something must release it when a node is freed with a proxy still in it --
   an embedder hook at free time, and a count on the descriptor. A reader that
   detaches is a reader that writes, which the series declines; a writer-only
   version leaves cold slots proxied indefinitely.
4. **Keep the settle eager where that is safe, so detach is rare** (§8). This
   shrinks the problem; it does not remove it.

How the published algorithm's own implementation handles a target word whose
memory is freed is not known to us. Check their code before assuming it is
solved.

This is also, possibly, the strongest thing the sole-driver engine has going for
it, and P2 does not say it: **an eager settle keeps every slot write inside the
commit's read-side critical section.**

## 6. Progress

### 6.1 Meeting an undecided foreign transaction

Three things a committer can do, and the policy is the part to settle by
measurement rather than here:

- **Help**: drive the foreign transaction to completion -- plant its remaining
  records in *its* order, CAS its status -- then re-read.
- **Abort it**: CAS its status `UNDECIDED -> FAILED`, then install over its now
  decided proxy. No per-record latch is needed for this either.
- **Wait a bounded spin first**, then help or abort. This keeps the common case
  looking like sole-driver, with no helper traffic unless an owner is slow.

### 6.2 What lock-freedom requires

- **Helpable transactions install in address order.** The published liveness
  argument is that helping chains are finite because every operation acquires
  in one global order. The engine's age-0 attempt installs flat, in caller
  order; two such transactions can each hold what the other wants, and helpers
  would chase each other around the cycle. Either every multi-writer commit
  sorts (a cost the age-0 path exists to avoid), or an unsorted transaction is
  *abortable but not helpable*.
- **Abort alone is obstruction-free, not lock-free.** Two transactions can abort
  each other forever. A rank is needed -- the handle's age already is one: abort
  what is younger, help what is older.
- **No domain-wide funnel.** The fair-mutex lane is a lock; a preempted lane
  holder stalls the lane. A lock-free engine keeps it out of the progress
  argument, at most as an opt-in starvation remedy.
- **Lock-free operations, not lock-free reclamation.** A thread stalled inside
  a read-side critical section blocks every grace period. Operations still
  complete -- they install over decided proxies -- but nothing is detached and
  nothing is freed. This is the standing caveat of anything reclaimed by RCU,
  and it has to be said next to the word "lock-free" every time.
- **Allocation.** A commit that takes the allocator's lock is not lock-free.
  The per-CPU descriptor slab's fast path avoids it; its refill goes to the
  allocator.

### 6.3 What lock-freedom buys

Immunity to a stalled owner: preemption, a page fault, a signal handler, a
thread that dies. In the kernel the owner can disable preemption, so the case is
weaker there; in userspace it is the whole point, and it is the tail the
`rseq` time-slice-extension lever was meant to thin for sole-driver.

## 7. What it costs

- **Readers, between commit and detach.** A proxied slot resolves through two or
  three dependent loads. The dentry-cache model already shows the shape: a
  rename leaves a shell for about 5 ms until a callback folds it back, and a hit
  reads three cache lines instead of one.
- **Write-hot slots stay proxied for good.** Each commit installs over the last
  before any detach lands.
- **Guards leave proxies too.** A load-validate guard is a record `v -> v`; it
  is planted like any other, and it now stays in the *guarded* slot for a grace
  period. A slot that is read-validated often is as proxied as one that is
  written often.
- **Two invalidations of the slot's line per commit**, a grace period apart,
  instead of two back to back.
- **The reclaim worker writes every committed slot.** Slot lines migrate to the
  worker's CPU: the traffic the batch retire just removed from the slab path.
- **A heavier plant** (§3.2), a CAS decision, and a descriptor that lives two
  grace periods instead of one -- twice the slab footprint at a given rate.
- **Helper traffic under contention** is unchanged by deferral. Deferral removes
  the latch's cost, not the duplicated work; independent work reports the same
  collapse and throttles its helpers.

## 8. A possible hybrid: eager where nobody could be stalled

Unproven. The obvious form is wrong, and the wrong form is worth recording.

**Wrong:** "helpers announce themselves with a CAS on the status word before
driving; if the owner's decide-CAS succeeds from the un-announced state it was
the sole driver, so it may settle eagerly." An eager plain settle by *any*
transaction can hand a stalled helper of *another* one the value it is waiting
for:

    T1 (helped):   B -> X.  Its helper H1 loaded plain B, and stalls.
    T2 (un-helped) installs over T1's proxy:  X -> B.  Settles eagerly: plain B.
    H1's CAS succeeds.  T1's record is resurrected.

**Candidate rule, per record:** settle eagerly only if the transaction was
un-helped *and* that record was planted over a plain value; otherwise defer that
record. Then, by induction, a slot that has held a helped proxy stays proxied
until a grace-period-gated detach, and I1a survives. An eager settle must be a
CAS, since another transaction may already have installed over the decided
proxy.

If it holds, an uncontended slot keeps a plain steady state and its settle stays
inside the commit's critical section, which also takes it out of §5. It needs a
proof and a stress gate before it is believed. It is the candidate with a claim
to being ours rather than theirs.

## 9. Interactions

- **Mixed SW/MW records.** A helper cannot perform an exclusive park: it is a
  plain store, and a late one would clobber. Simplest first cut: only all-shared
  transactions are helpable; one carrying exclusive records is abortable while
  its shared prefix is in flight, and its owner alone parks and decides.
  Lock-freedom is moot for it anyway -- its owner holds the embedder's lock.
  Exclusive slots keep their eager plain settle: the kind is global, so no
  shared-kind helper ever targets one.
- **Logical deletion and the one guard** carry over; see the guard cost in §7.
- **"No re-link before a grace period"** becomes "no reuse before the detach has
  run", which is §5 again.
- **Read-your-own-writes, the declarations, the per-record tag**: untouched.

## 10. Recommended path, when this is taken up

Nothing below starts without an explicit go, and no sweep without one.

0. **Before any code:** an answer to §5, the patent re-read of §2, and a look at
   how the published implementation frees target memory and nests its epochs.
1. **Prototype behind a build flag**, sole-driver staying the default, so the
   two A/B in one tree: install-over, CAS decide, `call_rcu` detach, second
   grace period. First cut: bounded wait, then help; sorted transactions only;
   type-stable nodes in the bench as the §5 stopgap, stated as such.
2. **Gates before any number:** conservation on every run; ASan, which is what
   catches §5; TSAN on a liburcu built with compiler atomic builtins; the
   built-shape A-B-A test of §4.
3. **Measure three things**, with per-writer pinned `call_rcu` workers, warm-up,
   pinned layout:
   - writers only, against sole-driver, on `bench_txn_3skiplist` and the list;
   - **readers under paced writers** -- the dentry-cache hit and reverse-walk
     rows, where a proxied slot shows;
   - **the tail under oversubscription** (more runnable writers than CPUs, or an
     owner stopped mid-commit). This is what lock-freedom is for, and no
     benchmark in the tree measures it today.
4. **Then the hybrid of §8**, if step 3 says the read side is what it costs.

## 11. What this does to P2

Either outcome makes the paper better than it is now.

- **Deferred settle is competitive.** P2's subject becomes a lock-free
  multi-writer facility; sole-driver becomes the measured throughput-leaning
  variation; the progress argument is rewritten around the goal question of
  §1.3. What would be ours shrinks to what the published algorithm lacks:
  readers that never help, the RCU integration, an answer to §5, the mixed
  record kinds, and the hybrid of §8 if it holds. The prior-art and patent
  questions of §2 then sit at the centre, not the margin.
- **It is not.** Sole-driver stays, and P2 gains the measured row it lacks: the
  closest prior design, built on the same substrate and compared with readers
  running, instead of a design argument and the other paper's numbers.

## Open questions

- §5, first and above the rest.
- Is §8's rule sound, and does the taint on a write-hot slot ever decay? Letting
  a transaction that plants over a proxy settle eagerly once that proxy's grace
  period is known to have elapsed needs a grace-period stamp on the descriptor.
- Help, abort, or wait-then-help, and by what rank?
- Does every multi-writer commit sort, or is age-0 abort-only?
- Is there any embedder for whom lock-freedom matters and type-stable memory is
  acceptable? If not, §5's first answer is no answer.
- How much of the reader cost is the indirection and how much the second
  invalidation?

Related: [rcu-mcas-install-gate](rcu-mcas-install-gate.md),
[mixed-sw-mw-txn](mixed-sw-mw-txn.md), [rcu-txn-use-cases](rcu-txn-use-cases.md);
articles tree `p2-sole-driver-mcas/SCOPE.md`, section dated 2026-10-03; memory
[guerraoui2020-mcas-prior-art], [rcu-mcas-install-gate-design],
[bench-harness-percpu-callrcu].
