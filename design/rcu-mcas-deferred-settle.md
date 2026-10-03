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

**Where the discussion of 2026-10-03 left it: lock-freedom may well not be
pursued** (Mathieu). The reason is that no route to it keeps what makes the
present engine elegant -- one compare-and-swap per slot, and a settle that is
done when the commit returns. Worked through (§1.2, §5, §5a, §8), every route
gives up one of the two, or the property itself:

- a *conditional install* (RDCSS, CCAS) keeps the eager settle and pays a second
  CAS on every install;
- a *deferred settle* keeps the single CAS and then has to keep the slot alive
  until the settle: a grace period on the writer's side, which is not lock-free
  without qualification (§8.2), or a contract on the embedder's memory (§5);
- *hazard pointers on the slot* pay a store and a barrier per record;
- *abort without helping* keeps the bare install and the eager settle, though
  the settle becomes a CAS, and it is obstruction-free, not lock-free.

This is a leaning, not a decision, and nothing below has been measured. If it
stands, this note is the record of why: the bare install with an eager settle is
available only because nobody helps, which is the sole-driver engine's argument
stated from the other side.

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
| Harris-Fraser-Pratt 2002, Fraser 2004 | RDCSS / CCAS (conditional install) | 3k+1 | lock-free | no |
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

Reduced to its cause, the race needs two things, and every known fix removes
one of them.

1. **A stale test.** "Is the transaction still undecided?" and the slot CAS are
   two operations. `H` tests at step 1 and acts at step 5; the decision falls
   between. For a helper the test may be only implicit -- it plants a record of
   a transaction it believes is live.
2. **A compare that cannot see the change.** The slot's content came back to
   exactly what `H` expects. Had it not, the stale test would be harmless: the
   plant by the other driver changed the slot, and `H`'s CAS would fail.

| Fix | Removes | How |
|---|---|---|
| RDCSS, CCAS | 1 | test and CAS become one conditional operation, at an extra CAS per install |
| per-record latch | 1 | a planter claims the record before the slot; the settle claims it last, so no plant can begin |
| sole-driver | 1 | the only planter is the thread that decides, and it decides after its last plant |
| deferred settle | 2 | the content cannot come back while a tester is in its window |
| version embedding | 2 | the content comes back with a different version |

For a helping engine that is to be lock-free, the decision must be able to fall
while some driver is stalled between its test and its CAS -- that is what
helping a stalled thread means -- and that driver's CAS must then fail when it
resumes. So the choice is exactly between a conditional install (remove 1, pay
per install, settle eagerly) and no recurrence (remove 2, pay in deferral or in
bits). The latch and sole-driver remove 1 by making someone wait, or by having
no one to wait for.

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

Behind both stands Fraser's thesis (*Practical Lock-Freedom*,
UCAM-CL-TR-579, 2004), which is the source for most of what §6 needs and should
be read before theirs:

- §3.2.1: the three phases (acquire, decision point, release), the conditional
  install and its reason -- a helper must not "reacquire a location after the
  MCAS operation has already succeeded" -- and acquisition in **address order**,
  which is what bounds recursive helping.
- §3.3.2: a transactional memory whose reads resolve through the owner's
  descriptor "rather than helping the owner", and whose **aborts are ordered by
  descriptor address** so that mutual aborts cannot livelock.
- §5.2.2: descriptors are reference-counted and their memory is **type-stable**
  (after Greenwald and Cheriton) -- never retasked, because a stale reference
  may still touch it. That is §5's first answer, applied to descriptors.
- §5.2.3: epoch-based reclamation, with the caveat in so many words: it is "not
  strictly lock-free", since a stalled process stops reclamation for everyone.

The original algorithm releases eagerly and pays a conditional install for it;
Guerraoui et al. install bare and pay by deferring the release. Sole-driver has
both -- the bare install and the eager release -- and pays in progress class.
That triangle is the whole design space this note moves in.

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

## 5a. How to delay the settle: three mechanisms (discussion of 2026-10-03)

Design level, unproven, like the rest. Mathieu's three candidates were a chained
scheme over two grace periods, two RCU domains, and hazard pointers.

### Who has to be waited for

Not "every thread that may have observed the value". Readers observe values all
the time and never compare-and-swap. The settle must wait for every **driver
that was inside its install phase when the transaction was decided**: only such
a thread can hold a raw slot content, loaded before our proxy went in, as the
expected value of a CAS it has not issued yet. That set is small and
short-lived, and a reader-wide grace period waits for far more than it.

Why the install phase and not the whole write-side operation, which also read
the slot while it was *building* its transaction: the delay guards against one
event only, **a plant that lands after its transaction was decided**. A CAS that
succeeds on a value which went away and came back is not that event by itself.

- A writer reads `B` while building, the slot goes `B -> X -> B`, and its own
  plant then succeeds. Its transaction is still undecided -- nobody but its
  owner has seen it -- so this is the ordinary value-CAS semantic, exactly what
  today's engine already allows. Nothing is resurrected.
- A plant can be *late* only relative to a decision someone else made. Someone
  else can decide a transaction only once it is visible, which is from its first
  plant on: inside its owner's install phase. And a helper becomes a driver of
  it only when it meets that proxy, inside its own.

So every thread that can still issue a plant for a transaction decided at time
`t` was inside an install phase at `t`. What a thread merely *observed* earlier
cannot turn into a wrong write. The precise rule is therefore that W covers
every stretch of code that can issue a plant: the owner's install loop, and any
helping episode. If a build-phase load were ever allowed to help an undecided
foreign transaction instead of resolving it to its old value, that episode
would have to be inside W as well.

### A second lifetime hazard, which helping has by itself

§5 is about the detach. Helping has the same exposure without any deferral. A
helper writes into slots it never traversed to; their memory is pinned only by
the *owner's* read-side critical section:

    N is unlinked, then helper H enters its critical section.
    Owner O (in its section since before the unlink) has a record on a slot of
    N in a transaction D that is going to fail.  H meets D, helps it.
    D is decided.  O returns and leaves its critical section.
    N's grace period never waited for H.  N is freed.
    H loads, or CASes, the slot in N.

The latched engine fenced at least the write: its settle-claim CAS made a record
DONE, after which no plant could begin
([rcu-mcas-install-gate](rcu-mcas-install-gate.md) §1 calls it the reclaim
contract). So the latch was a reclaim fence as well as an A-B-A fence, and a
latch-free helping engine has to replace both.

### The three mechanisms

**Chained grace periods, one domain** -- §3.4 as written. The first callback
detaches, the second frees. It answers the conflict it was proposed for: the
first grace period delays the settle, the second keeps the descriptor alive for
whoever loaded the proxy just before. It does not answer §5 or the helper
hazard: both are about the *slot's* memory, and a callback is in no critical
section that pins it. It needs type-stable slot memory, or a fence.

**Two domains** -- the one that answers both. A thread cannot wait for a grace
period of a domain it is itself a reader of, which is exactly why one domain
forces the detach into a callback. It *can* wait for a grace period of a second
domain while still inside the first. And W is nothing ad hoc: it is an ordinary
RCU domain used the ordinary way, with its read side paired against a grace
period on the update side (Mathieu, 2026-10-03).

- Domain **R** is today's: read-side critical sections, memory lifetime.
- Domain **W, read side**: `rcu_read_lock()` / `rcu_read_unlock()` of W around
  the install phase. Helping nests inside the helper's own install phase.
  Exactly, per plant attempt, the section must span three steps in this order:
  the **status check** that authorizes the attempt ("still undecided"), the
  **slot load**, and the **plant CAS**. The check is the essential one: a plant
  issued on a stale "undecided" is harmful even with a freshly loaded slot
  content (the slot may hold a later decided proxy that resolves to the
  expected value). The load is inside for the helper's sake, so that the
  owner's R section still pins a foreign slot when the helper touches it.
- **Not** in a W section: building the transaction; the status flip, by owner
  or helper; the settle CASes, whose expected value is the transaction's own
  proxy address and cannot recur; the retire. Exclusive (single-writer) parks
  need none either, since no shared-kind helper ever targets such a slot.
- Domain **W, update side**: the store that makes the status decided is the
  *removal* -- from then on no new W reader can find the transaction undecided.
  A **W grace period** waits out the readers that did. The **settle** is the
  *reclaim*: it retires the state those readers could still act on. So the
  order is: decide; W grace period; settle. The grace period must begin after
  the settler has *observed* the status decided, since a helper may be the one
  that decided it. The ordering between "read status inside W" and "decide,
  then wait" is the flavor's own; nothing is hand-rolled.

Three ways to take that grace period:

| W grace period by | Who settles | Does the committer wait? | Slot write inside the commit's R section? |
|---|---|---|---|
| `synchronize_rcu()` of W, in the commit | the owner, at once | **yes** | yes |
| `call_rcu()` of W | W's worker, which then chains `call_rcu()` of R to free | no | only if the owner holds its R section open until the callback has run |
| polled: `start_poll_synchronize_rcu()` / `poll_state_synchronize_rcu()` of W | the owner, once the poll says it has elapsed | no | yes, the owner holding its R section open until then |

- **`synchronize_rcu()`** is the simplest and is not lock-free. Every committer
  blocks, after its decision, behind any installer stalled in W: each thread can
  finish one more commit and then nothing completes. It also costs a grace
  period per commit, taken under the flavor's own locks. Good for a first
  correctness prototype, and for nothing else. It must be called outside any W
  section, and may be called inside an R one.
- **`call_rcu()` of W, chained into R** is the chained scheme with its first
  link in W and its second in R: one W grace period before the settle, one R
  grace period before the free. No committer waits. The settle runs in a worker,
  so for §5 the owner has to keep its R section open until a per-descriptor
  "settled" flag is set -- or the slots have to be type-stable.
- **Polling** keeps the settle in the owner's hands and in the owner's R
  section, with no flag and no second thread writing the slots.

"Holds its R section open" means, under QSBR, no quiescent state while one of
its own descriptors is unsettled; at a quiescent boundary the owner checks, and
skips the quiescent state if something is still pending.

What this buys: every slot write is again inside the read-side section the
commit ran in, so the embedder's contract is unchanged; no helper is still
inside a W section on the transaction's slots when the owner leaves, which
closes the helper hazard; the descriptor needs one R grace period after the
settle; and a slot stays proxied for a W grace period, which waits for
installers only, not for readers. With `call_rcu()` or polling no operation
waits: a stalled installer delays the owner's *quiescence*, hence reclamation,
which is the failure this design already accepts.

What it costs: a W read lock and unlock per install phase, so W wants a flavor
whose read side is cheap and delimited -- not QSBR, whose read side is
everything between two quiescent states; the settle is `k` CASes (the record
may have been installed over), so about `2k+1` CASes per commit plus the W read
side, against today's `k`; a W grace period per commit, or per batch of them;
and R grace periods that now wait for writers to have settled.

**Hazard pointers on the slot** -- per-slot precision. A driver announces the
slot before loading it; a settler that finds no announcement for a slot settles
it at once, inside its own section. The steady state stays plain and the common
case needs no deferral at all. But the announcement is a store and a full
barrier *per record*, and the check is a scan per settle: the per-record
synchronization the latch was, at about the latch's price. And a slot found
announced still needs one of the two schemes above.

### If helping is not required: abort without helping keeps the eager settle

The A-B-A needs a second *installer*. A thread that only aborts a foreign
transaction -- one CAS on its status -- and then installs over its decided
proxy is not one: every record is still planted by its owner alone. So the
sole-driver argument holds, the settle stays eager and inside the commit, and
none of this section's machinery is needed. What changes from today: the
decision becomes a CAS, the settle becomes a CAS (the owner may have been
aborted and installed over), and an undecided foreign proxy is aborted rather
than waited on. `2k+1` CASes; plain steady state; contract unchanged.

Its progress class is obstruction-freedom plus a contention manager, not
lock-freedom: with no helping, a rank cannot guarantee that anyone finishes.
But it has the practical property lock-freedom was wanted for -- **no thread
ever waits on a stalled owner** -- and it is the cheapest design here that
does. "Steal without help" was never measured by itself: the two were removed
together.

### A constraint all the deferred schemes share

A single-record commit is today a bare `CAS(slot, old, new)`, plain to plain,
with no descriptor. It must **never write a plain value over a proxy**: that is
an un-gated return to a plain value, the very thing being deferred. On a
proxied slot it has to take the descriptor path. Under a callback detach, a
write-hot slot is proxied all the time, so the cheapest commit the engine has
disappears exactly where it is used most.

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

- **Helpable transactions install in address order.** The liveness argument,
  Fraser's and after him Guerraoui et al.'s, is that helping chains are finite
  because every operation acquires in one global order: each level of recursion
  is a conflict at a strictly higher address. The engine's age-0 attempt installs flat, in caller
  order; two such transactions can each hold what the other wants, and helpers
  would chase each other around the cycle. Either every multi-writer commit
  sorts (a cost the age-0 path exists to avoid), or an unsorted transaction is
  *abortable but not helpable*.
- **Abort alone is obstruction-free, not lock-free.** Two transactions can abort
  each other forever. A rank is needed. Fraser's transactional memory uses the
  descriptor's address, and lets one transaction abort only another that follows
  it in that order; the handle's age is the rank this engine already has -- abort
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

"Un-helped" has a race-free test at no cost to the owner: a helper must first
CAS the status from `UNDECIDED` to `UNDECIDED|HELPED`, and the owner's decide
CAS expects bare `UNDECIDED`. If that CAS succeeds, no helper engaged before the
decision and none can after it.

If it holds, an uncontended slot keeps a plain steady state and its settle stays
inside the commit's critical section, which also takes it out of §5. It needs a
proof and a stress gate before it is believed. It is the candidate with a claim
to being ours rather than theirs.

### 8.1 The two together: delay only what must be delayed, and let the owner pin the slot

The difficulty stated in one line (Mathieu, 2026-10-03): the settle should be
delayed, but the settle needs the slot to still exist. At settle time the slot's
existence can come from only two places.

- **A read-side section that already pins it.** The owner's does: it pins every
  slot the transaction names. So the settle has to happen before the owner
  leaves that section, and the delay has to be short enough to hold it open
  for. That is the real argument for a W domain: a W grace period waits for
  installers, an R one for every reader, and no thread can wait for an R grace
  period from inside R.
- **Not needing it.** Either the settle is not delayed at all, or the slot is
  type-stable, or its death is visible from the descriptor (§5).

Put with the rule above, that gives a layered commit:

1. Records of an un-helped transaction that were planted over a plain value
   settle at once, in the commit. Nothing is delayed and §5 does not arise.
   Without contention this is every record.
2. Every other record -- helped, or planted over a proxy -- waits for a W grace
   period, and the owner keeps its R section open until it has settled them
   (§5a).

No callback ever writes a slot, the embedder's reclamation contract is
unchanged, and the cost of holding R open is paid only by a writer that was
actually helped or that wrote a contended slot. What the embedder gains as an
obligation is on the writer side instead: a thread with unsettled records must
not announce a quiescent state, go offline or block, so those have to go
through the engine. And a stalled installer stalls R grace periods, hence
reclamation, for as long as it stalls.

### 8.2 A grace period on the update path is not lock-free (Mathieu, 2026-10-03)

The objection is right as far as it reaches, and how far it reaches has to be
stated exactly. A grace period is a blocking construct: one stalled reader
stalls it. So whatever is gated on a W grace period can be stalled by one
stalled installer.

- If the **commit** is gated on it -- `synchronize_rcu()`, or any setting where
  the R section cannot be kept open past the operation -- the engine is not
  lock-free. The kernel is such a setting: a read-side section cannot be held
  across a sleep or a return to user mode, so a port would have to wait.
- In the deferred form only the **settle** and the **owner's quiescence** are
  gated. The operation is complete at its decision; the thread returns and
  starts its next one; later commits install over the decided proxy. With an
  installer stalled for ever, operations keep completing, while no slot is
  given back its plain value and nothing is reclaimed.

That second case is the class Fraser and Guerraoui et al. both claim and both
qualify: lock-free operations, reclamation that is not. Two things keep it from
being worse than what the base design already has. A stalled installer is
inside an R section anyway, so it was already stalling every R grace period:
the W domain adds no thread whose stall blocks reclamation. And nothing an
operation needs is behind the grace period.

It is still a qualified claim, and these are the places where it gives:

- memory is unbounded while an installer is stalled, so progress is finite;
- a writer thread that must go offline or block has to settle first, and so
  waits there;
- it presumes a reclamation domain whose read side a writer can leave open --
  QSBR in user space. Elsewhere the wait becomes synchronous.

So the defensible wording is "lock-free operations; settle and reclamation are
blocking, and are stalled by exactly the threads that stall reclamation today".
"Lock-free", unqualified, would be an overclaim.

If nothing on the writer's side may depend on a grace period, there are three
ways out, and each pays somewhere else:

| | What depends on a grace period | Who keeps the slot alive for the settle | Price |
|---|---|---|---|
| Callback settle, chained (§3.4) | callbacks only | the embedder: type-stable slots, or a hook on its free path (§5) | a contract on the embedder's memory |
| Hazard pointers on the slot (§5a) | nothing | the settler's own section, since it settles at once | a store and a barrier per record |
| Abort without helping (§5a) | nothing | the commit itself: the settle is eager | obstruction-free, not lock-free |

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
- **It is not pursued at all** -- the leaning as of 2026-10-03. Then P2 keeps
  its subject and has no new measurement, but it has a better argument than it
  had: not "helping lost", which is true only of the latched engine, but "a
  lock-free engine cannot have both the single CAS per slot and the eager
  settle". Each lock-free route costs a second CAS per install, a deferred
  settle with the slot-lifetime problem it brings, or a barrier per record
  (§1.2, §5a, §8.2). That argument is a design one and would have to be written
  as such.

## Open questions

- §5, first and above the rest -- and §5a's candidate answer to it: is the
  two-domain scheme sound, which of the three ways of taking the W grace period
  (callback or polling; `synchronize_rcu()` only to prototype), and which flavor
  for W?
- Is obstruction-freedom enough? If "no thread waits on a stalled owner" is the
  real requirement, abort-without-helping (§5a) meets it with an eager settle
  and no grace-period machinery, and should be measured before anything else.
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
