# rcu-mcas install gate: promoting install-once from per-record to per-transaction

Design analysis for replacing the MCAS engine's two per-record install-word
CASes (`<urcu/rcu-mcas.h>` in userspace-rcu-txn) with a single per-transaction
install gate, and — the harder half — the helping/eviction rewrite that forces.
This is a core-protocol change to the most delicate code in the tree (A-B-A
safety, install-once, self-settle, stealing, priority helping), so it is written
to be reviewed before any code is touched. Companion to
[rcu-txn-use-cases](rcu-txn-use-cases.md). 2026-07-10.

Verdict up front: the ceiling is real but modest (~15% at 192 writers, measured),
and the gate does **not** obviously capture it — it trades the two CASes for a
coarser install-hold window that rseq TSE cannot cover and for the loss of
parallel multi-helper co-driving. It must be prototyped behind a build flag and
A/B'd, not committed on the strength of the ceiling. The point of this note is to
make that trade explicit.

## 1. What costs what today

A committed transaction installs each record through `urcu_mcas_plant()` under a
per-record tri-state install word `state` ∈ {FREE, BUSY, DONE}, then the owner
runs `urcu_mcas_settle()` to convert each proxy to a plain value. Per transacted
edge that is **four atomic read-modify-writes**, and they are not
interchangeable — each buys a distinct guarantee:

1. **plant slot CAS** — `old_value → tagged proxy`. Detects a cross-transaction
   conflict (a concurrent writer or a higher-ranked thief). *Irreducible*: the
   slot is shared and the commit's atomicity rests on this CAS.
2. **plant latch CAS** — `state: FREE → BUSY`. Serializes co-drivers on the *same
   record* so exactly one slot CAS fires, and is the install-once flag. This is
   what makes **parallel multi-helper co-driving** safe: two helpers grab two
   different records' latches and plant concurrently; on one record the loser
   sees BUSY and waits (a bounded, few-instruction window) or sees DONE and
   advances.
3. **settle claim CAS** — `state: FREE → DONE`. The reclaim contract: after
   `settle()` returns, every record's word is DONE, so **no plant can ever
   begin** and no proxy can reappear. Without it a driver that read the txn
   UNDECIDED, loaded the slot, stalled, and resumed after the txn was evicted and
   settled would transiently republish a proxy of the retired descriptor — a
   use-after-free for a reader whose read-side section the grace period did not
   wait for. (Documented at length above `urcu_mcas_settle()`.)
4. **settle slot CAS** — `tagged proxy → plain new/old`. Makes the owner's slots
   plain again; a thief that already stole the slot makes this CAS fail
   harmlessly.

`-DURCU_MCAS_NO_ABA_FIX` removes exactly RMWs **2 and 3** (the two `state`
CASes), reverting `plant()` to a bare value-CAS with no install-once, no
self-settle, no settle claim. It is A-B-A-unsafe and for regression tests only —
but it is the right instrument for the *ceiling*, because the per-txn gate also
removes exactly those two per-record CASes and replaces them with one per-txn
acquire (amortized ~0 per edge for a wide transaction).

## 2. The ceiling (measured 2026-07-10, urcu-txn-dev @ 0b22e8b9)

`bench_txn_3skiplist`, `--movesper 3 --ryw 1`, jemalloc, best-of-3, 1 s. Stock
vs `-DURCU_MCAS_NO_ABA_FIX`, ns/key-move and aggregate:

    n = 960 keys/skiplist, 192 writers:   13.88 → 16.00 Mmoves/s   (+15.3%)
    n = 3840 keys/skiplist, 192 writers:  32.24 → 37.37 Mmoves/s   (+15.9%)

    n = 3840, scaling the writer count (ceiling tracks contention):
       1 writer   1.036 → 1.082   (+4.4%)
      32 writers 21.45  → 23.57   (+9.9%)
     192 writers 32.59  → 38.00   (+16.6%)

Two things to read off this:

- **The lever is ~15% and now roughly size-independent.** The old headline (13%
  at n=3840, 29% at n=960) was measured before the read-policy hybrid
  (`cb7b072e`). That commit stopped the skiplist from reading its write-set
  predecessors optimistically, which cut the *doomed* plant CASes; with fewer
  wasted RMWs overall the two `state` CASes are a smaller, flatter share. Do not
  quote the old 29%.
- **The win is contention-driven.** +4.4% at one writer is just the two removed
  instructions on an uncontended line; the line only becomes a bottleneck when
  many drivers share it. **Design constraint: the gate's single acquire must not
  add uncontended cost** — at one writer it replaces two uncontended per-record
  CASes with one, so it should be net-neutral-to-positive there. Good.

The ceiling is an **upper bound the gate may not reach**, because NO_ABA_FIX also
throws away guarantees the gate must keep (§4) and keeps a form of parallelism
the gate gives up (§5).

## 3. The mechanism

Add one **install-gate word** to the descriptor, next to `status`. The gate
holder acquires exclusive install rights over *all* of the transaction's records
and installs each with the plant slot CAS (RMW 1) only — no per-record latch, no
settle claim. Sketch of the state:

    gate ∈ { OPEN, HELD, SEALED }

    OPEN   → HELD    a driver (owner or helper) wins the gate; it alone installs
    HELD   → SEALED  the holder observed t terminal and closed installation
    HELD   → OPEN    a preempted holder is relieved (gate-stealing, §5)

`SEALED` is the reclaim fence: once sealed, no plant can begin, which is exactly
the guarantee RMW 3 (settle claim) gave per record (§4). The owner path becomes

    drive: acquire gate (OPEN→HELD); for each record, plant slot CAS;
           on the last, status UNDECIDED→SUCCEEDED; SEAL; settle slots.

Per edge that is the plant slot CAS plus the settle slot CAS — two RMWs, matching
NO_ABA_FIX — with the gate acquire and seal amortized over `nr` records (≈0/edge
for the skiplist's 8.7).

The slot ops stay **CASes, not plain stores**, even under the gate: a
higher-ranked transaction can still evict `t` (status → FAILED) and steal its
slots, so the holder's store may race a thief and must detect it. The gate makes
installation *single-writer per transaction*; it does not make the slots private.

## 4. Correctness: the gate must re-earn what the two CASes bought

- **Install-once (was RMW 2).** Only the gate holder plants, so two slot CASes on
  one record can't both fire from co-drivers — there is only ever one driver.
  Self-settle stays (the holder, after a plant, re-reads status and converts if
  terminal) so a proxy planted just after `t` linearized is still cleaned up.
- **Reclaim fence (was RMW 3).** The stale-first-install UAF requires a *second*
  installer of `t` that resumes after eviction+settle. Under the gate there is no
  second installer — planting requires the gate, and a `SEALED` gate can never be
  re-acquired. So "no proxy can appear after seal" follows from the gate
  discipline instead of from a per-record claim, *provided* seal strictly
  precedes `call_rcu`. The owner already seals-then-settles-then-`call_rcu` in
  `commit()`; the invariant to prove is that a relieved holder (HELD→OPEN, §5)
  cannot leave a plant in flight past a later seal.
- **A-B-A on the slot value.** Unchanged and still handled by the plant slot CAS
  plus install-once: a value that recurred to `r->old_ptr` is inert because the
  gate/holder, not the slot value, gates the second install.

## 5. The hard part: helping, eviction, and preemption

This is where the gate is not a drop-in, and where it can lose.

**Helping loses its parallelism.** Today a thread meeting `t`'s undecided proxy
that `t` outranks calls `drive_install_depth(t)` and co-drives — many helpers
plant many records of `t` at once. Under the gate a helper cannot co-drive; it
must **take the gate** to install `t`, and only one thread holds it. The rewrite
of the `urcu_txn_load` / `drive_install` helping decision becomes:

- **We outrank `t`** → evict (`status → FAILED`) and steal. A FAILED `t` resolves
  every slot to `old_ptr`, so our plant lands. Unchanged, and cheap.
- **`t` outranks us**, gate OPEN → take it, drive `t` to terminal, re-read. This
  is "help by becoming the sole installer."
- **`t` outranks us**, gate HELD → we cannot help in parallel. Either **wait**
  for the holder (an unbounded wait on `t`'s whole install, not a few-instruction
  latch window) or **escalate** past a patience cap (abort self, retry with
  higher aging priority — the same backstop `URCU_MCAS_HELP_MAX_DEPTH` already
  uses). Waiting must also handle a **preempted holder**: the gate has to be
  steal-able (HELD→OPEN after the holder is deemed stalled) or a descheduled
  holder blocks every dependent transaction.

**Two regressions against the per-record latch fall out of this:**

1. **The install-hold window is no longer TSE-coverable.** The per-record BUSY
   window is a single slot CAS — a few instructions rseq transactional-store
   extensions can protect against preemption (see
   [rcu-mcas-install-latch-design]). The gate's HELD window is the *entire*
   install, O(`nr`) slot CASes; TSE cannot cover it, so preemption of a gate
   holder is a first-class event needing an explicit relief protocol, not a lever
   we already have.
2. **Wide transactions install serially.** The skiplist's 8.7-record commit is
   driven by one thread with no help; today a stalled owner's records get planted
   by passing traffic. Whether this matters depends on how much real parallel
   co-driving happens now versus how much is already threads waiting on BUSY — the
   profile ([rcu-mcas-read-helping-storm]) showed helpers pinned in
   `drive_install`, which the gate would serialize rather than speed up. Unknown
   without measurement.

So the gate buys ~15% of removed CASes and spends an unknown amount on serialized
install and a new preemption-relief protocol. It is credible that on the hash
(narrow, 3–5 records, low per-record contention) it is a clean win, while on the
skiplist (wide, hot shared upper-level slots) the serialization eats it. The hash
is where the ceiling is smallest, though, so the win where it's safe is smallest.

## 6. Recommended path

1. **Prototype behind `-DURCU_MCAS_INSTALL_GATE`**, leaving the per-record path as
   the default, so the two can A/B in one build matrix (as NO_ABA_FIX already
   does). Reuse the read-policy test harness's discipline: the gate must pass the
   full TAP suite, `test_rcu_mcas_aba`, `test_rcu_mcas_republish`, and the
   skiplist/hlist move-conservation stress at MAX_LEVELS 1 and 8, with and without
   RYW — a gate that breaks the reclaim fence shows up as a republish/aba failure,
   not a throughput number.
2. **Start with the simplest holder-relief**: no gate-stealing, escalate on a HELD
   gate past a small patience cap. This is wait-free-adjacent (bounded retries)
   and sidesteps the preemption-relief protocol; if even this beats the
   per-record path on the hash, the harder relief is worth building.
3. **Measure against the ceiling.** The prototype's job is to answer one question:
   of the +15%/+16.6% NO_ABA_FIX leaves on the table, how much survives the
   serialized install and the escalation churn — on the hash *and* on the
   skiplist separately.
4. If it lands on the hash but not the skiplist, ship it **domain-scoped** (a
   per-structure flag), not engine-wide.

## Open questions

- Can the gate acquire fold into the existing `status` word (an `INSTALLING`
  intermediate: UNDECIDED → INSTALLING(holder-id) → SUCCEEDED/FAILED) so no new
  descriptor field and no extra cache line is added? The holder-id is needed for
  relief; a bare INSTALLING bit cannot say *who* to relieve.
- Does eviction of a gate-HELD transaction interact cleanly with seal? A thief
  sets `status → FAILED` while the holder is mid-install; the holder's next plant
  must observe terminal (it re-reads status each record, as `drive_install` does
  today) and stop, then the relief/settle path converts the already-planted
  records to `old_ptr`. Needs the same self-settle ordering proof as today.
- Is the +4.4% at one writer even worth the added complexity if the skiplist case
  nets flat? Possibly the honest outcome is "gate on the hash domain only."

Related: [rcu-txn-skiplist](rcu-txn-skiplist.md),
[rcu-txn-use-cases](rcu-txn-use-cases.md); memory
[rcu-mcas-read-helping-storm], [rcu-mcas-install-latch-design],
[rcu-mcas-help-depth-cap].
