# rseq per-CPU call_rcu queue — design (2026-10-01)

Status: **prototyped, validated, SHELVED (2026-10-02)** — batch retirement
already removes the traffic it targets; see §10.  Target: liburcu-txn `urcu-txn-dev`
(`src/urcu-call-rcu-impl.h`), prototyped in a bench-local clone, never in the
live dev tree.  Companion to the descriptor slab's rseq local lists
(`--enable-slab-rseq`, `include/urcu/rcu-txn-slab.h`), whose protocol it
reuses.

## 1. Why again, and what to expect

An rseq per-CPU call_rcu was designed, torture-validated and A/B'd once
already (2026-07-02, in the `urcu-bidir-build` clone, since removed): a
double-buffered per-`call_rcu_data` stack with an index flip and a
`membarrier(EXPEDITED_RSEQ)` fence on wrong-CPU drains.  It was correct (0
lost, 0 double-drained across 76.8M nodes; the fence-off mutant double-drained
7567) and a **throughput wash** on the list-churn bench (0.90–1.14x, avg ~1.0),
like the rseq slab at the time.  It was dropped.

Three things differ now:

- **The enqueue path costs more than it did.**  `_call_rcu()` today
  (`urcu-call-rcu-impl.h:891`) executes, per callback:
  `cds_wfcq_enqueue()` (an `xchg` on the tail), `uatomic_add_return(&qlen)`
  (a locked add, and since `ac5d639a` it is control flow: the queue-length cap
  reads it) and, for a non-RT worker, `call_rcu_wake_up()`'s `cmm_smp_mb()`.
  Two locked instructions and a full fence, each of which on x86 waits for every
  older load and store to complete.
- **The target workload has outstanding misses.**  The list bench's write
  path was L1-resident; the dcache's add / unlink / rename paths write lines
  other cores own (bucket heads, child lists, neighbours' LRU links) and then
  call `call_rcu()`.  A locked op behind those misses stalls until they drain;
  a plain-store rseq commit does not.  That is the hypothesis this design
  exists to test — not "atomics are slow" in general, which the July
  measurement already refuted for uncontended per-CPU words.
- **The slab's rseq protocol matured** (`rseq_ok` read inside every committing
  section, one-way demotion by clear + membarrier, separate rseq-only and
  atomic lists that never meet).  The queue can use the same discipline and
  drop the July double buffer.

Expectation, stated up front so the benchmark is read honestly: a wash on
L1-resident write paths; a gain, if any, only where `call_rcu()` follows
outstanding misses.  Correctness is the deliverable either way.

## 2. Structure

Per `call_rcu_data` (already cache-line aligned), on a **line of its own**
apart from `cbs_tail` (which cross-CPU producers keep writing):

```c
struct cds_wfcq_node *local;     /* rseq-only LIFO of rcu_head.next; NULL = empty */
unsigned long local_pushed;      /* rseq-only: callbacks pushed on @local */
int home_cpu;                    /* the worker's pinned cpu; -1 = no rseq */
int rseq_ok;                     /* read INSIDE every committing section */
/* worker-private, on the worker's line: */
unsigned long local_drained;     /* callbacks taken off @local */
```

- `local` is touched **only** by rseq critical sections on `home_cpu` (pushes
  and the worker's steal) and, after demotion, by one plain drain (§5).  It is
  never the target of an atomic RMW: an rseq plain-store commit and an atomic
  RMW cannot share a word, so — as in the slab — they never meet.
- The existing `cbs_head`/`cbs_tail` wfcq stays exactly as is and carries every
  callback that is not a home-CPU push.
- `home_cpu` is the crdp's `cpu_affinity` when it is `>= 0`: per-CPU crdps
  (`create_all_cpu_call_rcu_data()`) and per-thread ones created with an
  affinity (the bench harnesses' `create_call_rcu_data(RT, cpu)`).  An
  unaffine crdp (the default one) never uses rseq.

## 3. Producer fast path

```c
void _call_rcu(head, func, crdp):
    head->func = func;
    if (crdp->home_cpu >= 0 && rseq_registered() &&
        rseq_current_cpu_raw() == crdp->home_cpu &&
        local_push(crdp, &head->next) == 0) {          /* rseq, below */
        local_count(crdp);                              /* rseq add, below */
        cap_check_local(crdp);                          /* relaxed loads */
        wake_local(crdp);                               /* §6 */
        return;
    }
    /* anything else: today's path, unchanged */
    cds_wfcq_enqueue(...); qlen = uatomic_add_return(&crdp->qlen, 1); ...
```

`local_push()`: one two-compare/one-store section,
`rseq_load_cbne_load_cbne_store__ptr(&crdp->local, old, &crdp->rseq_ok, 1,
node, home_cpu)` with `node->next = old` set before it.  It fails (and the
caller takes the wfcq path, nothing lost) if the thread migrated, if `local`
changed under it, or if the crdp was demoted.  Retrying on a changed head
within a bounded loop, then falling back, keeps the fast path non-blocking.

`local_count()`: `rseq_load_add_store__ptr()` (librseq, per-CPU mode) adding 1
to `crdp->local_pushed` on `home_cpu`; on
failure (migration) the count is simply skipped — it only feeds the cap, which
is a heuristic.  The queue length the cap sees is
`qlen + (local_pushed - local_drained)`, read relaxed.  No atomic RMW is added
anywhere: `local_pushed` has rseq writers only, `local_drained` the worker
only.

## 4. Worker drain (the normal case: on `home_cpu`)

The worker already pins itself before its first callback
(`do_set_thread_cpu_affinity()`), so in steady state it runs on `home_cpu` and
**steals** `local` with an rseq section: compare `local == observed`, compare
`rseq_ok == 1`, store `NULL`.  No atomic, no fence: producers on the same CPU
are serialized with it by rseq itself.

Order of a batch, and why:

1. splice the wfcq (as today);
2. `cmm_smp_mb()`, then steal `local`;
3. reverse the stolen chain (it is LIFO; reversal restores per-CPU FIFO);
4. after the grace period, invoke the local chain, **then** the spliced wfcq
   chain;
5. `local_drained += n` (plain store, worker-private).

**rcu_barrier() ordering.**  A barrier callback B must run after every
callback that happened-before its enqueue.  Rule: **barrier callbacks always
take the wfcq path** (`rcu_barrier()`, which today enqueues through
`_call_rcu()` at `urcu-call-rcu-impl.h:1113`, calls an internal enqueue that
skips the local fast path).  Then for C happened-before B: if C is in the wfcq, FIFO
puts it ahead of B; if C is on `local`, C's push happened-before B's enqueue,
which the splice in step 1 observed, so the steal in step 2 sees C, and step 4
runs it first.  (Allowing B on `local` breaks this: a C enqueued remotely
before B could land in a later splice than the steal that takes B.)  Ordering
between a single producer's local and wfcq callbacks is otherwise not
guaranteed across the two paths, which matches what call_rcu promises today
for a migrating thread.

## 5. Wrong-CPU drain: demotion

Wherever `local` must be emptied from another CPU — `_call_rcu_data_free()`
(any thread), a worker whose re-pin failed, `call_rcu_affinity_lost()` when the
home CPU goes away, the fork child — the drainer:

1. `uatomic_store(&crdp->rseq_ok, 0)`;
2. `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 0, 0)` — every section on
   `home_cpu` that was mid-commit is aborted; it re-reads `rseq_ok == 0` on
   restart and its producer takes the wfcq path;
3. reads and clears `local` with plain accesses (no rseq writer can commit any
   more) and appends the chain, reversed, to the wfcq.

Step 2 is load-bearing, not a latency trick: without it a producer that loaded
`local == H` before the drain commits `local = node` with `node->next == H`
after the drainer took H, and H's chain is drained twice (the July torture
test's 7567 double-drains).  Checking `rseq_ok` before the section instead of
inside it would not do: the restart would not re-sample it.

**Promotion back** (`call_rcu_affinity_regained()`, the worker re-pinned on
`home_cpu`): the worker itself stores `rseq_ok = 1` with release.  Safe because
while demoted nothing but the demoting drainer touched `local`, and it is done.
(The slab keeps demotion one-way because its pop and its atomic refill can be
in flight together; the queue has no such pair.)

Registration: rseq is enabled for a crdp only if `rseq_registered()` and
`membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ)` succeeded at init
— without the fence a local list could never be drained from elsewhere, so
there is no rseq at all (same rule as the slab).

## 6. Waking the worker

- **RT workers** (`URCU_CALL_RCU_RT`, the bench configuration) poll and are never
  woken: the local path costs one push section and one add section, no
  barrier.
- **Non-RT workers**, v1: keep `call_rcu_wake_up()` as is, `cmm_smp_mb()`
  included.  The rseq push is a plain store, so the Dekker pairing with the
  worker's `futex = -1; mb; check queue` needs that fence; this removes the two
  locked ops but not the fence.
- v2 (only if v1 shows the fence matters): make the pairing asymmetric — the
  producer keeps a compiler barrier only, and the worker, before it sleeps,
  issues `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)` and re-checks both
  queues.  The worker sleeps rarely under load; the cost moves to the idle
  transition (an IPI to every CPU running the process).

The queue-length cap keeps its rare-path machinery (`call_rcu_cut_delay()`'s
cmpxchg) unchanged; only its input changes (§3).

## 7. Knobs

- Build: `--enable-call-rcu-rseq` (AE_FEATURE, experimental, needs librseq),
  defining `URCU_CALL_RCU_RSEQ` for `src/` only.  Unlike the slab there is no
  identical-across-every-TU requirement: call_rcu is out of line in liburcu.
- Runtime: `URCU_CALL_RCU_RSEQ=0` disables the local path at crdp creation, so
  one build A/Bs both arms.

## 8. Validation before any number is quoted

- **Exactly-once torture**, rebuilt: pinned producers pushing unique ids on
  their home CPU, unpinned producers on the wfcq path, and a thread forcing
  demote / drain / promote cycles while pushes are live; every id invoked
  exactly once.  Mutant: drop step 2 of §5 — must double-invoke.
- **Barrier ordering test**: C local then B, C remote then B; B must run last.
- The dcache gates against the new build (they exercise call_rcu-heavy folds
  and frees), and liburcu's own `tests/`.  TSAN cannot see rseq commits; it
  still checks the wfcq path and the demotion's plain accesses.

## 9. Benchmark plan

- **Microbench**: `call_rcu()` cost per op (perf stat, cycles and instructions)
  with 1–192 pinned producers, each with its own pinned RT worker; wfcq vs local
  path.  Second arm: touch N cold lines owned by another core before each
  `call_rcu()`, to measure the locked-op-behind-misses hypothesis directly.
- **dcache**, interleaved A/B, 7 runs: allocating and in-place churn (`churn_w`),
  flat-out renames (`sat_scale` writers, optaxonomy leaf ops), and readers under
  churn as a control.  Builds at `18809ea8`:
  slab {atomic, rseq} × queue {wfcq, rseq} — the existing slab routes plus the
  queue — so the slab-route figure gains the queue axis.
- Report against §1's expectation: a wash on L1-resident paths is the predicted
  null, not a failure.

## 10. Outcome: shelved — batch retirement covers it

Prototyped as designed (configure `--enable-call-rcu-rseq`, +414 lines in
`urcu-call-rcu-impl.h`, `tests/regression/test_call_rcu_rseq.c`), in bench-local
clones of `18809ea8`.  Correct: the exactly-once torture passed 10/10 (0 double,
0 lost, 0 barrier-order failures, ~90–100M local pushes a run, RT and non-RT
workers), and its no-fence mutant failed 3/3 with a double free.  The dcache
gates all passed against it.

**What the microbench found** (`experiments/callrcu/`; P pinned producers, each
with its own RT worker on its cpu; median ns per call_rcu, 3 runs):

- d=0 (call_rcu alone): identical, ~8.5 ns on both paths — §1's predicted null.
- d=2 plain stores to lines other cores own, just before each call: the rseq
  path cuts the enqueue's increment by 40–57% at every P (P=8: +13.0 → +7.0 ns;
  P=192: +44 → +19 ns), 1.07–1.22× throughput.  The locked-op-behind-misses
  hypothesis holds.
- d=8, P≥48: rseq 0.83–0.95×.  The shared store pool saturates there, and
  slower store issue helps: the stores-only arm is the slowest.
- d=0, P=48–96: equal median, but lower wall-clock rate in 5 of 6 runs — worker
  side, not the enqueue.  Leading suspect, not confirmed: the worker walks every
  local callback twice (§4 step 3's LIFO→FIFO reversal, then invocation).

**Why it does not matter.**  `URCU_TXN_SLAB_BATCH` (`e1153413`, 2026-08-01)
already retires descriptors with one call_rcu per BATCH, through
`urcu_slab_free_pending()`, without touching the call_rcu machinery — and
removes the cost the queue cannot reach: **the worker no longer touches the
slab's cache lines.**  On the per-descriptor route the worker's callback
dereferences every descriptor (`urcu_slab_free()` pushes it onto its arena's
freelist) a grace period after the commit, by which time the line has usually
left the writer's cache.  On the batch route the FREEING thread links the chain
while the blocks are hot in its own cache, and the callback
(`urcu_slab_splice_cb()`) is O(1): one store to the tail block, one cmpxchg on
the freelist head.  Nothing else in the batch is walked, and order does not
matter on a freelist, so there is no reversal either.  The rseq queue makes the
enqueue cheaper but keeps the per-object walk, and its LIFO→FIFO reversal adds
a second one.

Counted with an LD_PRELOAD shim on `urcu_qsbr_call_rcu` (dcache churn, 48
writers).  The shim's shared counter itself capped throughput at ~19M calls/s,
so the rates below were measured separately, without it (2 runs each):

| engine, mode | route | call_rcu / op | Mchurn/s |
|---|---|---|---|
| txn-global, allocating | per-descriptor | 1.80 (1.2 descriptors + 0.6 dentries) | 34.1–34.3 |
| txn-global, allocating | batch | 0.60 (dentries only; ~10k batch closes/s) | 33.2 |
| bucket lock, allocating | either | 0.60 (dentries only) | 30.2–30.5 / 29.6–29.7 |
| every engine, in place | either | 0 | — |

So at 48 writers batch is not faster in this harness (−3%): each writer has its
own RT worker co-pinned on its cpu (see `bench_dcache.c`), which is the
per-descriptor route's best case, and allocating churn is LRU-lock-bound there.
What the queue could save: ≤25 ns × 1.8 calls in a ~1.4 µs op (≤3%) on the
per-descriptor route, and ≤1% under batch.  The remaining dentry frees, if they
ever matter, want the same batch pattern (a slab with `free_pending()`: the
kernel's `kfree_rcu()` batching), not a faster per-object enqueue.

**Lesson.**  §1 listed what had changed since the July attempt, but not the
route that had already removed the traffic.  Before designing a faster path,
count the calls under the best EXISTING route.
