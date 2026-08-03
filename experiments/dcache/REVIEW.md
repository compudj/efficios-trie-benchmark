# dcache experiment — review and playbook

Reviewed 2026-07-31, covering the full arc 2026-07-15 → 2026-07-28 (commits up
to `7cd6a01`).  This document is the retrospective: what the experiment
established, which design and methodology rules earned their place, and the
checklist to apply to the next port of this kind.  It deliberately does not
duplicate the mechanism spec (`rename-shell-transition.md`), the plan
(`README.md`), or the simplification analysis (`simplification-s4.md`) — it
cites them.

## 1. Verdict

The question was: can an RCU pseudo-transaction formulation dissolve the
kernel dentry cache's `rename_lock` + `d_seq` machinery, on both the
simplification and the scaling axis?  The answer decomposed into four parts,
none of which was the answer we started with:

1. **`d_seq` dissolves outright.**  Shell-stacking makes per-node identity
   write-once, so there is no torn per-component state to guard.  The mark arm
   (`-DDC_MARK_GEN`) goes further and retires the counter *word*: the hlist
   deletion mark (bit 1 of `d_hash.next`) already flips on exactly the events
   a version would count, monotonically, with no ABA argument needed.  The
   freed 8 bytes became name capacity (`DC_NAME_MAX` 32→40).

2. **`rename_lock`'s whole-walk-causality role dissolves only if localized.**
   The first correct replacement — a single transacted `rename_gen` bumped by
   every rename — is *semantically* a win (no writer serialization) but
   *reintroduces the exact whole-tree cacheline contention being removed*:
   the global arm saturates ~110–260 Mlookups/s while the per-node and mark
   arms keep scaling to the full machine (~2200 Mlookups/s @184 readers,
   ~25× the seqlock baseline).  Localization is the scaling win, not the
   transaction per se.

3. **The writer story inverted twice, and the honest result is a split.**
   Making the seqlock baseline kernel-faithful on the write path (per-bucket
   `hlist_bl` bit-lock instead of a global mutator lock) revealed that plain
   locked stores beat the all-MW MCAS on pure add/unlink churn — the MCAS
   commit machinery costs ~1200 extra instructions and 3× the L1 misses per
   op.  The all-MW engine earns its keep on the *reader* path and on
   *renames/moves*, not on churn.

4. **The winning engine is a hybrid, and it ends on the kernel's own
   mechanism.**  `dcache_bucketlock.c` ("bucket lock + SW txn", fold-lock
   dequeue default) keeps the kernel's per-bucket bit-lock for exclusion and
   uses the single-writer transaction only to make the cross-bucket edit
   reader-atomic; the transition chain is SPMC (single producer under the
   bucket lock, many fold consumers).  It wins churn (matches the cheap
   bit-lock path, beats both other engines at scale), wins moves (~2.3× the
   all-MW engine, ~5–7.6× seqlock at every height), holds ~90–95% of the best
   reader (~8–60× seqlock), and escapes the readdir-vs-churn bias trade-off
   entirely (above both rwlock-preference extremes on both axes, where the
   per-dir rwlock forces a 4.6× swing by bias choice alone).

The headline that survives for a kernel conversation: *the best candidate
improves rename/move and reader scaling over the seqlock scheme while ending
on the same per-bucket bit-lock the kernel already uses* — the delta is
write-once identity (shells) + an SW txn for cross-bucket reader atomicity +
a localized (or mark-carried) causality signal, not a wholesale MCAS rewrite.

Honest limits: near-root directory moves erode the localized reader's edge
(the per-node arm inverts below seqlock at H7; the mark arm removes the
inversion — `figures/dcache_height.png`); every reader-ordering result is
x86 (`cmm_smp_rmb` is a compiler barrier here), so per-hop-fence costs and
the move-gate negative must be re-checked on weak-memory hardware before any
kernel claim; and the kernel's own `d_seq` bump is nearly free in-kernel, so
the file/dir bump-skip optimization is defensible *here* but not upstream.

## 2. The architecture that survived

One line each; full detail in `rename-shell-transition.md`.

- **Shell-stacking rename** — move a live dentry between buckets by
  publishing a transient named shell in one commit (both indexes), never by
  mutating identity in place; the one in-place write (fold TRANSFER) lands
  inside a `call_rcu` grace-period window where no reader can see it.
- **Fold cascade** — per-node async `call_rcu` workers compress chains;
  each node freed by its *own* fold (self-free ⇒ no double-free by splices).
- **SPMC chain decomposition** — enqueue (demote) is single-producer under
  the bucket lock (`store_sw` / plain store); dequeue (splice/reclaim/
  transfer) is multi-consumer.  Three dequeue strategies kept as build arms:
  per-host **fold lock** (default; plain stores, no producer coupling),
  legacy chain lock (`-DDC_CHAIN_LOCK`), MW dequeue (`-DDC_CHAIN_SWMW`).
- **Abort-free-under-lock** — every index-bearing commit's CAS-old values are
  provably stable while the bucket lock is held, so those commits carry MW
  records but cannot contention-abort; only lockless chain folds can.
- **`d_host` skip pointer** — write-once, union-overlaid on `d_id`, points
  at the fold-invariant tail host: O(1) host resolution for readers *and*
  writers, zero struct growth, never dereferences a splicing transient.
- **Deletion-mark-as-version** (`DC_MARK_GEN`) — the structural edit is the
  causality signal; nothing added to the rename commit.
- **Move-flag cycle check** — per-host `d_moving` cmpxchg lock + plain-read
  ancestry walk replaces the `load_validate` spine walk; kills the
  cross-CCD proxy ping-pong (spread writers 1.0 → 6.84 Mrenames/s).
- **1-CL hot line** — `{d_iparent, d_iname, (d_seq,) d_hash.next}` on CL0,
  enforced by `posix_memalign(64)`; cold fields grouped by *access pattern*
  (fold-lock line vs structural-reader line) after pahole + perf c2c.
- **Engine matrix** — `dcache_seqlock.c` (faithful baseline: rename_lock +
  d_seq + hlist_bl bit-lock + vfs_rename_mutex + per-dir lock with
  reader-pref/writer-pref/krwsem arms), `dcache_txn.c` (all-MW; global /
  `-DDC_PER_NODE_GEN` / `-DDC_MARK_GEN` causality arms), and
  `dcache_bucketlock.c` (the hybrid winner, `-DDC_MARK_GEN` default).

## 3. Design rules to carry forward

Each rule earned its place by a measured result or a reproduced failure.

1. **Do the hot-line field audit first.**  The mark-gen design *falls out* of
   auditing what CL0's 64 bytes are for and which bits are actually free; a
   whole wrong arm (`DC_IPARENT_SKIP`) was built before doing it.  Ask, for
   every fast-path word: who reads it, who writes it, which bits are free,
   and is its value load-bearing after removal.

2. **Enforce cacheline residency at allocation.**  "Fits in one line" from
   field offsets is fiction under 16-byte `calloc` alignment — 3/4 of nodes
   straddled two lines and halved reader throughput.  `posix_memalign(64)`,
   in every engine being compared.  Tell: identical reader code, 2× swing on
   struct-size change ⇒ suspect alignment.

3. **Make identity write-once; publish, don't mutate.**  It is what lets the
   reader drop per-component validation, turns the version counter into a
   pure freshness signal, and reduces the walk-causality check to
   sample-gen → confirm-top-via-mark.  Any in-place write that survives must
   be provably invisible (GP window) or read off a write-once witness (the
   pos/neg-off-the-top fix for the TSAN race).

4. **Localize or structuralize every coherence signal.**  A global counter
   on the read path re-creates the contention being deleted (global arm ==
   rename_lock's ceiling, and even *writer-side* global-counter contention
   leaked into gen-free reader curves as a second-order effect).  Prefer,
   in order: no signal (readdir needed none), the structural edit as signal
   (mark), a per-moved-node signal (per-node gen).

5. **Choose the primitive by per-slot concurrency, not per-operation.**
   The chain analysis (single producer / multi consumer) is what unlocked
   the hybrid: SW or plain-locked stores where a lock already excludes,
   MW/MCAS only where slots genuinely have concurrent writers, one mixed
   commit where the pair must be reader-atomic.  Corollary: a global lock is
   the enemy, a *fine-grained* lock plus plain stores is often the cheapest
   correct writer — MCAS descriptor machinery is O(records) atomic RMWs.

6. **Writer paths must be O(1) in transient-structure depth.**  The bistable
   liveness collapse came from an O(chain) walk inside a read-side section
   whose *purpose* was to relieve chain growth: stalled GPs → longer chains
   → longer walk → no quiescence → stalled GPs.  Depth must cost memory,
   not time; a GP stall then degrades to bounded memory growth instead of
   collapse.  Any relief valve whose trigger cost grows with the pathology
   it relieves is a self-igniting mechanism — delete it (fold-ahead had no
   regime where it helped).

7. **Deferred reclaim is grace-period-bound; protect the GP supply.**
   Frequent quiescence is not overhead for a fold-style engine (coarsening
   the cadence 16→256 was a net *loss*, up to 0.57×); per-writer pinned
   `call_rcu` workers are mandatory or you measure the reclaim thread's
   ceiling; threads must never park RCU-online (`sem_wait_quiescent`,
   offline across `pthread_join`) — that one is latent until the first
   writer waits on a GP, then it is an intermittent three-way deadlock.

8. **Denormalize with write-once pointers into union'd space.**  `d_host`
   cost zero bytes, made readers and writers O(1) in chain depth, and
   avoided every transient-node hazard by aiming at the fold-invariant
   tail.  A redundant field is fine when it is write-once, space-free, and
   hazard-avoiding — document why it is kept.

9. **Fair-mutex lane discipline (any front-end with an escalation lane):**
   `begin()` first; acquire flags/locks *after* begin (a parked writer must
   hold nothing another writer's walk can see); every terminal bail
   `abandon()` + `end()` — `conflict(); end(); goto out` on a reject path
   leaks the lane and deadlocks the domain.  The leak can be *latent*:
   HEAD dodged it only because age-0 caught the reject before escalation.
   Grep every early return out of a retry loop.

10. **Respect the measured negatives.**  Do not re-try without new evidence:
    the FT-style move gate (x86: 1-pass ≈ 2-pass inside noise; 0.19–0.29×
    under moves); rename coalescing (needs a serialization point);
    fold-ahead; node sub-classing (saves ~0 bytes, kills the polymorphic
    walk); `expect_conflict` on the fold (real abort rate ~0.001% — the 25%
    a naive counter showed was harness structure); "transacted `d_iparent`
    would tax every match" (the tax is ~1%; the real 4.3% cost was three
    uncacheable acquire loads per node — thread one raw word through).

## 4. Methodology rules

These are what made the results trustworthy, and each exists because its
absence produced a wrong conclusion at least once.

1. **Baseline fidelity is a review target — read the reference source.**
   The global mutator lock was a lock the kernel does not take; fixing it
   *inverted* the churn headline.  glibc's default rwlock is reader-pref —
   the exact policy the kernel's rwsem is engineered against (verified in
   `kernel/locking/rwsem.c`): bracket with both prefs and, for the exact
   policy, the vendored kernel rwsem (`krwsem/`, 56 B, embeds).  Fidelity
   claims about *any* baseline must cite the source, not folklore.

2. **Gate every benchmark on an invariant.**  Namespace conservation
   (permutation-tracked, exchange-aware) ran on every sweep; a corrupted
   run exits nonzero instead of reporting a throughput.  Hundreds of runs,
   zero tolerated failures.

3. **Prove the harness can fail (non-vacuity).**  The traps were passes:
   a census assert in a quiescent walk could never observe the bug; the
   cross-parent-only gating passed *every* harness because no harness did a
   same-dir interior rename.  Mutation-test the mechanism (sed out the
   cycle reject → the harness must wedge; inject a deliberate race → TSAN
   must fire), and audit bench coverage against the op taxonomy (the 2×2
   rename / file move / directory rename / directory move table — all four
   cells now have a perf number; see §6).

   **This rule has now fired three times, twice on tests written to satisfy
   it.**  (a) The name-guard mutation first sat in `dc_readdir`'s callback
   copy, which `bench_dcache` calls with `fn == NULL` — dead code, so the
   mutated build passed and proved nothing.  (b) The name-width sweep's
   readdir panel called `dc_readdir(…, NULL, NULL)` too, and
   `fn == NULL` *only counts*: no `qstr` is ever materialized, so a panel
   whose entire subject was per-dirent name width could not have detected a
   width no matter how large.  It duly reported "no effect" (ratio 1.001)
   and that null was an artifact.  Both were caught by asking *what would
   have to execute for this to be able to fail*, not by reading the result.
   The generalisation is sharper than "mutation-test the mechanism":
   **a benchmark whose subject is a cost must be shown to pay that cost** —
   print the evidence (`--readdir-names` now reports `READDIR names: 1` and
   a non-zero name sink) and have the sweep *assert* on it, so the vacuous
   configuration aborts instead of publishing a null.

4. **Isolate the path under test.**  Homogeneous mixes are writer-bound
   (a rename ≈ 50× a lookup) and mask reader effects — role-split
   readers/writers.  Decontend what is not being measured (dirs per
   writer), pin one HW thread per core from hwloc, pin the allocator
   config, prime all engines, keep NUMA placement of the measured role
   stable across the sweep variable.

   **And know which layer you are actually measuring.**  Every mutation here
   allocates a transaction descriptor, so past a few writers the writer numbers
   belong as much to liburcu's descriptor slab as to the dcache (§6).  Two
   consequences.  First, the harness must let reclaim RUN: a registered thread
   that never quiesces — main, parked in `nanosleep()` for the measured window —
   blocks every QSBR grace period, and the symptom is not a hang but an
   allocator that appears to leak while conservation passes and every gate stays
   green.  Second, reclaim needs its own cpu budget: a non-sleeping
   `URCU_CALL_RCU_RT` worker co-pinned with a writer that never yields simply
   halves it.  Neither shows up as a failure; both show up as the engine looking
   slow.

5. **Strip harness ALU that hides memory effects.**  The per-lookup
   `snprintf` was ~74% of instructions and ran concurrently with descent
   stalls — it hid the entire 1-CL layout win ("neutral" was a harness
   artifact).  Per-op counters (instructions, L1d/LLC misses) are the
   robust observable when absolute throughput is pinning-sensitive; on a
   loaded box, interleave A/B runs instead of running arms sequentially.
   **A control only controls for what it shares with the arm.**  The seqlock and
   bucket-lock arms are the standing "did the machine move" check here, and they
   are worthless for anything touching the transaction descriptor slab, because
   they do not use it.  Measured: one configuration (batch retirement,
   batch_max=1024, 48 writers) read 121.7 Mchurn/s in one sweep and 109.3 in
   another a day later -- ~11% apart -- while seqlock and bucket lock moved <1%
   across the same pair.  Whatever drifts between sessions (allocator state, THP
   availability, fragmentation after days of runs) reaches only the arms that
   allocate, so the controls stay flat and certify nothing.  Two consequences:
   a cross-sweep delta on a slab-using engine is not evidence below ~10%, and
   the only reliable A/B is INTERLEAVED reps of both arms on one binary in one
   session.  That is how the batch-clock question was actually settled, after a
   cross-sweep comparison pointed the opposite way.

   **Know which column is noisy before quoting a ratio from it.**  In the
   role-split configuration only 8 of 192 threads are writers, and the
   writer column's run-to-run spread is correspondingly wide: repeating one
   `txn-pernode` measurement 3× gave same-dir/cross-dir ratios of 1.19,
   0.79 and 0.97 — best-of-5 each.  The reader column at the same points is
   stable to a few percent.  A ratio built from two noisy writer numbers
   inherits both spreads, so quote it only after repeating the whole
   measurement, and prefer "no effect resolvable above noise" to a number
   the sweep cannot support.

6. **Sanitizer discipline.**  TSAN requires the compiler-atomic-builtins
   liburcu (else phantom races *and* missed real ones).  A TSAN-clean run
   is non-exhaustive — the `d_iparent` race survived one and was found on
   re-validation, invalidating an earlier "0 races" claim; re-run TSAN
   after structural changes, don't carry the claim forward.  Hand-rolled
   futex locks are invisible to TSAN's happens-before — annotate with
   `__tsan_acquire/release`.  ASan for memory; UBSan flags benign liburcu
   sentinel arithmetic (known noise).

7. **Liveness is its own test axis.**  Correctness harnesses all passed
   while the engine could bistably collapse ~1 run in 15 under an
   adversarial height workload — build harnesses that attack liveness
   (hot-node exchanges, GP starvation) and run them repeatedly.  Triage
   method that worked every time: `ps -L -o tid,stat,%cpu` (one thread at
   100% = livelock; all S at 0% = deadlock) → `gdb thread apply all bt` →
   inspect the engine's domain state from a parked frame.  Beware debug
   instrumentation: a hot shared debug counter changed the timing story
   once; sample it or make it per-thread.

8. **Attribute before optimizing, measure instead of re-guessing.**  perf
   c2c found the proxy ping-pong; two plausible guessed mechanisms for the
   swmw low-thread gap were both wrong before measurement settled it ("it's
   the fold dequeue") — and the follow-up deliberately stopped there rather
   than guess a third time.  Every headline number in this experiment that
   was later *inverted* (churn winner, 3hash existence ratio, split
   neutrality) was inverted by a fidelity or harness-artifact fix — hence
   rules 1–5.

## 5. Checklist for the next port

Run this list when building or reviewing the next transacted / hybrid
structure (it composes with the `rcu-pseudo-transaction` skill checklist;
items here are the ones that actually fired in this experiment):

- [ ] Hot-line field audit done before any layout or versioning design;
      allocation-enforced alignment; pahole the result.
- [ ] For every slot: who are the concurrent writers?  Plain store under an
      existing lock / `store_sw` / `store_mw` chosen per slot, globally
      consistently; `commit_sw` only on provably SW-only sets.
- [ ] Every reader-visible multi-slot pair either single-commit atomic or
      proven not to need co-atomicity (the chain-vs-index analysis).
- [ ] Writer paths O(1) in any transient-structure depth; no O(state) work
      inside a read-side section; relief valves audited for self-ignition.
- [ ] Lane hygiene: begin-first, flags-after-begin, `abandon()`+`end()` on
      every terminal bail (grep the early returns).
- [ ] `declare_disjoint` / `expect_conflict` decided per-op from structure,
      then *validated by an abort-rate counter*, not assumed.
- [ ] Reclaim: self-free ownership stated; aborted-attempt frees
      attempt-local; unlink-vs-intended-unlink distinguished; GP flavor of
      every deferral matches the readers.
- [ ] Baseline(s) faithful to the reference implementation, verified
      against its source; lock-policy defaults (rwlock bias!) explicit.
- [ ] Harnesses: invariant gate wired to exit status; mutation-tested for
      non-vacuity; op-taxonomy coverage table filled in; liveness harness
      exists and is run repeatedly; QSBR park discipline (no online parks,
      offline across join — except walkers under test).
- [ ] Sanitizer matrix: ASan + TSAN (atomic-builtins lib) on every arm and
      every new concurrency mechanism; TSAN re-run after structural change.
- [ ] Bench methodology: role-split, decontended, hwloc-pinned, allocator
      pinned, per-writer call_rcu, engines primed, harness ALU audited,
      per-op counters recorded, noise priced (best-of-N + spread).
- [ ] Results narrative separates: what dissolves structurally, what wins
      only under localization, what the baseline wins, and the measured
      negatives — one claim per mechanism, each with its figure.

## 6. Open items

- **Coverage gaps** (`dcache-rename-taxonomy`) — *closed: all four cells now have
  a number* (result block below).  `bench_dcache --op-mix
  rename=A,move=B,exchange=C` adds same-dir **rename**: each token owns a
  private name pair and a rename flips between them, which is what keeps the
  conservation census a permutation.  `bench_dcache_height --op` adds one-way
  **directory rename** and **directory move** beside the two exchange arms.
  The one-way arms move a *spare* subtree parked under reserved names no digit
  path spells: a one-way op in a complete tree needs a free name, a free name is
  a hole, and a hole costs `B^(H-D)` of reader walks — 0.4% at H=0 but 50% at
  H=D-1 — where an absent walk terminates early and would make the reader metric
  silently cheaper as the swept variable climbs.  So the two groups price
  different things by construction: the exchange arms keep the moved subtree on
  reader paths and price the `B^H` invalidation, the one-way arms price the op
  itself.  Sweep: `scripts/run_dcache_optaxonomy.sh` →
  `figures/dcache_optaxonomy.png`.

  **Result — leaf panel, file leaves, 8 writers / 184 readers, best-of-5,
  0 conservation failures.**  Writer Mrenames/s; `rn/mv` is the same-dir-vs-
  cross-dir ratio, i.e. what the `cross_parent` branch costs (cycle check +
  `d_moving` lock + reparent + 2nd child head) with no subtree attached:

  | engine | rename | move | exchange | rn/mv |
  |---|--:|--:|--:|--:|
  | seqlock | 0.47 | 0.36 | 0.35 | **1.30** |
  | txn-global | 1.92 | 1.63 | 1.23 | 1.18 |
  | txn-pernode | 1.90 | 1.98 | 1.21 | ~1.0 ¹ |
  | txn-mark | 1.85 | 1.88 | 1.11 | ~0.96 ¹ |
  | bucketlock (fold lock) | 2.74 | 2.34 | 2.17 | 1.17 |
  | bucketlock-chainlock | 2.10 | 0.88 | 1.62 | **2.38** |
  | bucketlock-swmw | 2.47 | 1.95 | 1.56 | 1.27 |
  | bucketlock-swmw-pad | 2.53 | 2.07 | 1.93 | 1.22 |

  ¹ ⚠ **The writer row on the txn arms is noisy — do not read a precise ratio
  off it, and note these two rows are not single-sweep numbers.**  Repeating the
  measurement (3× best-of-5) gave rn/mv of 1.19 / 0.79 / 0.97 for `txn-pernode`
  and 0.99 / 0.95 / 0.95 for `txn-mark`, against a 0.82 outlier for `txn-mark`
  in one sweep; the two txn rows above are the repeat medians rather than that
  sweep's row, which is why they disagree with `figures/dcache_optaxonomy.png`
  (the figure plots the sweep as measured, outlier included).  Only 8 of 192
  threads are writers, so the writer column carries far more variance than the
  reader column.  The supportable claim is **"no `cross_parent` cost resolvable
  above noise on the per-node and mark arms"**, not a number.  seqlock (1.30
  twice), bucketlock (1.17/1.20) and chainlock reproduced across independent
  sweeps and can be read as stated.

  Four readings the taxonomy pays for:
  - **The branch cost tracks how cheap the rest of the write path is.**  Where
    the write path is a bit-lock (seqlock) the extra cross-dir work is ~30%;
    where a commit dominates, it disappears into the noise.
  - **The chain lock is what made cross-dir moves expensive** — `chainlock` is
    the only arm where `rn/mv` is large (2.38), i.e. its moves cost 2.4× its
    same-dir renames.  That is the cost the fold-lock default was built to
    remove, and it removes it (`bucketlock` `rn/mv` 1.17).
  - **The published "fold lock ≈2.3× the all-MW engine on moves" does not
    reproduce against the all-MW arm.**  Measured here the fold lock is
    **1.20× `swmw` on moves and 1.11× on rename** (1.14× / 1.08× against the
    same-size `swmw-pad` control; the −8 B alone is worth 1.02–1.06×).  The
    ~2.3× is the **chain-lock** comparison (2.67× on moves), so the headline was
    almost certainly remembered against the wrong baseline.  The rule this sweep
    was built to settle still answers cleanly, though: the gap does *not* vanish
    on the op a filesystem actually issues most (1.11× rename vs 1.20× move), so
    the ordering stands — at a much smaller magnitude than recorded.
  - The **global-gen collapse** is now quantified on this panel too: `txn-global`
    readers fall 1578 → 193 Mlookups/s (8.2×) when leaves are directories and
    every rename bumps the one global counter, while `txn-pernode` (1655 → 1620)
    and `txn-mark` (1574 → 1610) are flat.  Same conclusion as §7.1 of
    `simplification-s4.md`, reached from the op axis instead of the reader axis.

  On the directory panel (`bench_dcache_height`, H=2) the split is the other way
  round: `bucketlock` wins the writer (2.30 Mrn/s one-way rename) but **loses the
  reader** to `txn-mark` (851) and `txn-pernode` (933) at 805, and by more on the
  exchange arms (486 vs 681/645).  The "bucket lock is the winner" shorthand is a
  writer-side statement; on directory-op reader throughput the mark and per-node
  arms are ahead.
- **Mark-arm figures** — *closed: the caveat retires, with evidence.*  The
  mechanism was not the one originally recorded.  The **dentry** is not where the
  arms differ: measured, `sizeof(dentry)` is 168 (176 bucketlock) and `d_hash` is
  at @56 on the mark arm and on every other txn arm, so the Makefile's standing
  claim holds.  `struct qstr` is shared with `struct dc_path`, though, so
  `DC_NAME_MAX` also sets the **harness path object**: `sizeof(dc_path)` 964 →
  1156 B, a 48- instead of 40-byte struct copy per path component, and a 20%
  bigger precomputed leaf-qstr table — all on the reader's hot path, none of it
  the mechanism under test, and all of it running *against* the mark arm.  Note
  this also contradicts the "the leaf-qstr table is a co-footprint, identical for
  every binary" aside in `bench_dcache.c`: it is identical for every binary
  *except* the mark arm.  Two controls, since one knob moves two things:
  `-DDC_NAME_MAX=32 -DDC_NAME_PAD=8` leaves the dentry byte-identical and matches
  only the harness path (the arm to publish alongside), `-DDC_NAME_MAX=32` alone
  also shrinks the dentry 8 B and moves `d_hash` to @48.
  `scripts/run_dcache_namewidth.sh` → `figures/dcache_namewidth.png`; `make
  namectl` builds the arms.

  **Result (245 lookup rows + 49 readdir rows, 7 runs per point, 0 conservation
  failures).**  The matched control lands on the shipped curve on both panels, so
  **the width was never a material confound and the caveat retires**:

  | control ÷ shipped | rd=8 | 32 | 64 | 128 | 184 | readdir |
  |---|--:|--:|--:|--:|--:|--:|
  | `txn-mark-w32` ÷ `txn-mark` | 1.067 | 1.047 | 1.018 | 1.020 | 1.023 | 0.985 |
  | `bucketlock-w32` ÷ `bucketlock` | 0.996 | 1.077 | 0.972 | 0.972 | 1.033 | 1.012 |

  Run-to-run spread is 3–18%, and on a common-language effect size
  (P(control run > shipped run), 7×7 pairs per point) no lookup point clears
  0.85 consistently — the mark arm leans ≤7% *in its own favour* and bucketlock's
  sign is mixed.  Since the residual leans against the mark arm, published mark
  reader numbers stay conservative.
  ⚠ Two cautions this sweep produced, both worth carrying:
  1. **The readdir panel was vacuous on its first run** and its "no effect" meant
     nothing — see the `--readdir-names` item below.  The numbers above are the
     re-run.  Even non-vacuous, readdir turned out *not* to be the sensitive
     panel it was predicted to be: building a real `qstr` per dirent costs 15–16%
     on the txn arms and ~5% on bucketlock, but matching 40-vs-48 bytes inside it
     is invisible.
  2. **The shrink is not the control** and must not be read as one.
     `-DDC_NAME_MAX=32` alone costs **bucketlock 4–11% on lookups** (P=0.00 at
     three of five reader counts) while being neutral-to-positive on `txn-mark`
     (+6.8% on readdir, P=1.00).  `pahole` says why: the shipped layout puts
     `d_hash`@56 so `next`@56 is the last thing in CL0 and `pprev`@64 spills
     cold — a deliberate straddle the engine comments call out.  The shrink moves
     `d_hash` to @48, which pulls **`pprev`@56 onto the reader's hot line**, and
     `pprev` is written by every hlist insert/remove.  Freeing 8 bytes bought a
     false-sharing surface.  Why `txn-mark` tolerates the same layout change is
     *not* established here (its hlist updates ride the MW commit and its writer
     rate is ~1.5× lower); treat the bucketlock mechanism as strongly indicated,
     not proven.
- **The allocator underneath** — *investigated, and it is where the writer
  numbers actually come from.*  Every mutation allocates one transaction
  descriptor, so past a few writers this experiment measures liburcu's
  descriptor slab as much as the dcache.  Three defects, recorded in the order
  found because each hid the next:

  1. **The harness stalled every grace period.**  `bench_dcache_churn` left main
     RCU-*online* in `nanosleep()` for the whole measured window, and one stuck
     online QSBR thread blocks all reclaim.  No `call_rcu` callback ran, so no
     freed descriptor returned to a freelist, so every allocation carved a fresh
     block — which presents as an *allocator leak*: reuse ~11%, footprint
     growing linearly to 48.7 GiB at 8 s.  It is not a leak; at ONE writer the
     same slab reuses 99.6% and sits at 20 MiB.  Every churn number this
     experiment had published was measured that way.
  2. **The reclaim worker shared a cpu with its writer.**  `URCU_CALL_RCU_RT`
     does not sleep, so co-pinning it with a writer that never yields halves
     reclaim.  `DC_CRDP_CPU_OFFSET` moves it: the SMT sibling is best (75% reuse
     vs 62% co-pinned), a free core on the same socket is *worse* (56.6%) and
     the other socket is catastrophic (33.2%) — locality with the writer beats
     dedicated execution resources.
  3. **Batch retirement had no in-tree user.**  liburcu carried
     `urcu_slab_free_pending()` — retire a whole batch with one `call_rcu`
     instead of one per descriptor — and both engines still freed one at a time,
     so the machinery contributed only its costs.  Wiring it up
     (`URCU_TXN_SLAB_BATCH`) is worth **3.0–3.5×** on churn at 48 writers --
     33.9/35.0/41.1 → 113.4/123.9/121.7 Mops/s for global/per-node/mark, against
     seqlock's 152.5 and bucket lock's 164.4, neither of which uses this slab --
     *and* bounds the footprint at 194 MiB with reuse at 99.6%.  Quoted as a
     range across the three engines rather than a median, because that is what
     was measured.

  Wiring it up also surfaced two liburcu bugs that nothing could have found
  while the path was dead: a per-call store to a *process-wide* word costing
  ~55% of cycles (which made batching look 2× slower than what it replaces, and
  hid the 3.4×), and a plain-write/atomic-read race on the arena floor that
  failed all six TSAN gates.  Both fixed upstream.

  ⚠ **A footprint cap is a memory safety valve, not a tuning knob.**  Raising it
  does buy throughput — ~36 → ~99 Mchurn/s here — but only by letting the
  footprint climb; with the cap out of the way the slab grows linearly and never
  plateaus.  Fixing the recycling instead buys *more* (~122) with memory
  bounded.  A cap sized to absorb an unbounded working set has stopped being a
  cap.  An earlier version of this work shipped exactly that mistake, as a
  "budget floor scaled to the arena count", before it was dropped.

  Sweep: four descriptor-slab arms
  (`scripts/dcache_*{,_rseq,_batch,_batch_rseq}.csv`, `figures/dcache_slabroute.png`).

  **rseq per-cpu local lists are only worth enabling WITH batching, and cost
  throughput without it.**  Its fast paths require a *same-cpu* free; on the
  default route the descriptor is freed from the call_rcu worker, a different
  cpu, so ~12.5% of frees took the local path and the list the allocator pops
  from stays empty.  Every operation then pays `rseq_registered()` and the
  `rseq_ok` load and falls back to the atomic path regardless, and the cost
  grows with contention:

  	writers        1     4     8    16    32    48
  	rseq/default 1.04  1.02  0.99  0.95  0.90  0.88

  monotone, below a control band that stays flat at 0.98–1.01 from 8 writers
  up — i.e. **−12% at 48 writers**, not a null.  (A median over all writer
  counts reads 1.007 and hides it; the trend is the finding.)  Under batching
  the committing writer frees on its own cpu, the fast path is reachable, and
  the same comparison turns positive: 21/21 points at 1.02–1.05 against a
  control flat at 0.99–1.00, i.e. ~3%, on the churn workload only.

  The lesson generalises past rseq: an arm whose fast path the workload never
  reaches does not return a clean null — it returns the *cost* of the disabled
  mechanism, which is worse than uninformative because it looks like evidence
  against it.  Rule §4.3 wearing a different hat.
- **Phase 2 (negative dentries)** — *landed, and it priced the thesis.*
  `dc_add_negative` + `dc_instantiate` across all three engines.  The recorded
  item (`stack_shell` must copy pos/neg into the shell) was retired rather than
  done: inode-ness is now authoritative on the content HOST, so a rename cannot
  disturb it and no shell -- including the exchange's two -- has to carry it.

  **The two engine families answer it differently, and that difference is the
  thesis being tested.**  The seqlock baseline brackets the transition in the
  per-dentry `d_seq` it still has, which is exactly what that seqcount is for.
  The txn engines deleted `d_seq` and paid for it by treating pos/neg as
  write-once-per-identity -- so `d_instantiate`, a live reachable node changing
  kind, is precisely the shape that assumption forbids.

  ⚠ **What replaces `d_seq` is an ATOMIC RMW, not a transaction** -- and the
  first answer here was wrong in a way worth keeping on the record.  Phase 2
  published the flip as a single-slot `urcu_txn_store_sw()` commit and claimed
  "the transaction stands in for the seqcount".  It does not: `store_sw` *"parks
  it with a plain store that never fails"*, an SW-only commit never
  contention-aborts, and **SW is a promise of EXCLUSION across every writer of
  the slot** -- which the fold's TRANSFER, another writer of that same word,
  breaks.  See the lost-update item below.

  So the honest form of the headline: **`d_seq` dissolves for the operations
  phase 1 implements**, all of which change the namespace; a state change IN
  PLACE needs the transaction to stand in for it.  It cost no bytes
  (`sizeof(dentry)` still 168, name still 48) and nothing per hop -- with no
  rename in flight the top IS the host, so the reader's match word is already
  the one carrying the state.
- **Phase 2 remainder (`dc_delete`)** — *landed, and it restated the no-bump
  proof rather than breaking it.*  `dc_unlink`'s proof says "unlink REMOVES, and
  the removed node is EMPTY, hence a TERMINAL a reader can only straddle at the
  leaf".  Neither clause survives a delete that leaves the node hashed.  The
  proof still holds once restated on the property the two operations share --
  **the node's LOCATION does not change**.  A bump is owed when a reader's stale
  prefix can be combined with a node's NEW location to name a path that never
  existed, and that needs a RELOCATION; a late reader finding something under a
  node still sitting where it always sat is reporting a real path at a real
  time.  So the general rule is relocation, and the terminal argument is a
  corollary.  Files first; **rmdir-to-negative followed**, and it produced the cleanest
  cost measurement of phase 2 (below).

- ⚠ **OPEN: `--evict bursty` on the MCAS LRU arm wedges, cause NOT yet found.**

  ⛔ **A first "root cause" was published here and was wrong**, which is the
  lesson worth keeping: an escalation-lane symptom is normally the EFFECT of a
  deeper bug, not the cause.  Acting on the symptom produced one fix that was
  measured *worse* (a bounded/yielding shrinker delete — escalation is a property
  of the DOMAIN, so a fresh handle still enters the lane and N attempts cost N
  futex handoffs) and one confident write-up that the next experiment refuted.

  Looking for the deeper bug did find a REAL one — the sweeper's rotate re-added
  through the caller's shard, migrating the whole cache onto the sweeper's shard
  over successive sweeps (see `lru_add_at` in `dcache_lru.h`).  Fixed, and worth
  fixing; it did **not** stop the wedge.

  Where the evidence stands, so the next attempt does not redo it:
  | observation | |
  |---|---|
  | needs a SEPARATE shrinker thread | writers self-evicting (`continuous`) never wedge — at **60,945 evictions/300 ms** |
  | needs FREQUENCY | `--evict-period 50` completes; `5` wedges |
  | not eviction VOLUME | continuous does 100× more evictions and is fine |
  | not the shard axis | per-node and per-CPU both wedge |
  | not the batch size | wedges at `--evict-batch 1` |
  | MCAS-only | the LOCK arm at identical settings: 446–480 evictions, no wedge |
  | shrinker thread alone is innocent | cap set unreachable → runs clean |
  | onset is STOCHASTIC | same command line completes on one run, wedges on the next |

  Symptom at the wedge: two threads parked in `cds_fair_mutex_park` (via
  `urcu_txn__enter_fallback`) and the `call_rcu` worker inside
  `urcu_qsbr_synchronize_rcu()` on samples **six seconds apart** — i.e. grace
  periods are stalled and it is absorbing, never recovering.  That park-while-
  online interaction is real and worth fixing on its own, but on the evidence it
  is how the failure *manifests*, not why it starts.  Next: instrument abort and
  escalation counts per call site to find which commit is actually starving.

  <!-- superseded first attempt kept below for the reasoning, not the verdict -->
  The chain that was proposed, and is at most half the story:

  `urcu_txn__enter_fallback()` → `cds_fair_mutex_lock()` → `cds_fair_mutex_park()`
  blocks on a futex **with the thread still RCU-online**.  Under QSBR an online
  thread that is not running holds off *every* grace period, so escalation stalls
  `call_rcu`, which stalls descriptor reclaim, which raises allocation pressure
  and conflict, which causes **more** escalation.  Self-reinforcing, and it never
  recovers.

  Evidence: two threads in `cds_fair_mutex_park` and the `call_rcu` worker still
  inside `urcu_qsbr_synchronize_rcu()` on samples six seconds apart, for a 300 ms
  benchmark.  ⚠ **Onset is stochastic** — the same command line completes on one
  run and wedges on the next — so a run that finishes is *not* evidence the
  configuration is safe.

  It is the same park-while-online hazard the repro harnesses guard with
  `sem_wait_quiescent()`, except the parking is inside liburcu's own lane where a
  caller cannot guard it.  The fix belongs there: go offline across the park.
  Whether that is safe at the fallback entry — the transaction is at `begin()`
  and holds no resolved pointers yet, which is the argument that it is — is a
  liburcu decision.

  ⛔ A bounded/yielding shrinker-side delete (mainline's `spin_trylock` +
  `LRU_SKIP`) was implemented to dodge it and **measured strictly worse**:
  escalation is a property of the DOMAIN, not the transaction, so a fresh handle
  still enters the lane and four attempts cost four futex handoffs instead of
  one.  A real trylock needs a front-end that can attempt a commit *without*
  entering the fallback lane.

- ⭐⭐ **rmdir-to-negative: free where a lock already covers the child list,
  and only there.**  The invariant a negative must hold is that it cannot GAIN a
  child.  For a FILE that is free on every engine (`d_isdir` is write-once, so
  `dc_add` already answers `-ENOTDIR`).  A directory can legitimately take one,
  so `children_empty` must still hold at the instant the state flips — which
  means excluding a concurrent `dc_add`.  Exposed as a capability
  (`dc_delete_dir_supported`) because the engines genuinely differ:

  | engine | cost | why |
  |---|---|---|
  | `seqlock` | **free** | `dc_add` takes its parent's dir lock; `dc_delete` takes the VICTIM's — the same lock.  Exactly why the kernel's `rmdir` holds the victim's `i_rwsem`. |
  | `bucketlock` | **free** | `dc_add` takes its parent's `d_child_head` bit-lock; `dc_delete` takes the victim's — the same head, paired with the bucket it already holds. |
  | `txn` (all-MW) | **`-ENOTSUP`** | lock-free by design: nothing it holds spans the check and the flip. |

  On the two lock-bearing engines the whole price is **one predicted
  load-and-branch inside a critical section `dc_add` already entered** — no new
  lock, no read-set entry, nothing on the reader.  It is that cheap only because
  the lock `dc_add` and the fold already share is the one the invariant needs.

  The all-MW engine's options are both real costs: transact `d_iparent` MW on
  every arm (a resolving read on the hottest field — what `d0e7955` rejected),
  or take a per-parent lock in `dc_add` (a cmpxchg on the hot add path).  Left
  explicit rather than chosen silently.

  ⭐ **This is a second instance of the review's own headline**: the hybrid wins
  by ending on the kernel's per-bucket lock, and the feature is free precisely
  where that lock already sits.  The lock-free engine pays for what the
  lock-bearing ones get for nothing — the same shape as the churn result.

  ⚠ Lock ordering: `dc_delete` must acquire the bucket and the victim's child
  head **address-ordered together** (`bl_lock2`), not escalate to the child head
  with the bucket in hand — both are bucket-head-class locks, so escalating
  deadlocks against a `dc_add` wanting the same pair.  seqlock has the same
  shape and peeks the victim locklessly to learn its type before locking
  (`d_isdir` is write-once, so the peek cannot be stale about the type; the
  re-find under the lock catches a stale *identity*).

- ⛔⭐⭐ **THE LOST STATE CHANGE — phase 2's own prediction, come true.**
  `d0e7955` left the fold's in-place identity write plain, said exactly why that
  was safe, and dated its own expiry: *"benign today only because rename
  preserves inode-ness ... but it is UB and a latent correctness bug once
  phase-2 negative dentries land."*  It landed.  `d_delete`/`d_instantiate`
  write a live host's `d_iparent` from another thread, so two plain
  read-modify-writes share one word: the fold reads the host positive, a
  concurrent delete publishes NEGATIVE, the fold writes back the bit it read,
  and **the delete is gone with both callers returning success**.

  **The two engines close it differently, and that split is the interesting
  result.**  `dcache_txn.c` is lock-free by design, so it leaves the fold and
  the state change concurrent and makes each an **atomic RMW**.  Not `store_mw`
  (it installs a descriptor every reader must resolve, and the global/per-node
  arms deliberately leave `d_iparent` untransacted).

  `dcache_bucketlock.c` **excludes them with the bucket lock it already holds**:
  the fold takes the named top's bucket head across its handover in all three
  chain variants, so `dc_delete`/`dc_instantiate` take the same bucket — plus
  the re-verify `dc_unlink` already does, since a concurrent transfer can make a
  *different* node the top of that *same* bucket between the find and the
  acquire — and the fold keeps a plain store.

  ⛔ **The FOLD lock is not the answer, and "bucket then fold lock if shelled"
  is a deadlock.**  The hierarchy is `{fold locks < bucket-head locks}`, chosen
  so that *"no bucket is ever held while waiting on a fold lock"*, and the fold
  acquires `fold_lock(host)` **before** its buckets — so reaching for it with a
  bucket in hand is ABBA.  It is also unnecessary: the TRANSFER writes the top's
  immediate SUCCESSOR while `dc_delete` writes the chain TAIL, and those coincide
  only when the chain is `top→host`, where both hold the same bucket.  SPLICE and
  RECLAIM never touch the word.

  ⭐ **Writer-writer exclusion does not make the store plain.**  Both engines
  keep a relaxed atomic store, because readers sample this word for pos/neg
  while holding no lock — a different race from the lost update, and the one
  TSAN caught.

  `repro_delete_fold.c` pins it on both engines through a new
  `dc_test_transfer_hook` fired between the fold's read and its write-back.  Its
  rendezvous is **timed, and the timeout is load-bearing**: the all-MW engine
  lets the deleter COMPLETE inside the window while the bucket-lock engine makes
  it BLOCK, so waiting unconditionally would deadlock the second design rather
  than test it.  Mutation-verified per engine — restoring the plain RMW (txn) or
  dropping the bucket lock (bucketlock) makes each report *"/d/g reads POSITIVE
  (id 42) -- the d_delete was LOST"*.

  ⚠ **TSAN then found the other half, which no gate had run**: phase 2 made
  `host_is_positive` re-read the host's `d_iparent` — the read `d0e7955` had
  *removed* — so the reader's plain load raced the fold's write.  `iparent_raw`
  is now a relaxed atomic load.  The rule this pays for: **when a fix works by
  removing a read, adding that read back is a change to the fix, not a use of
  it** — and the gate that would have caught it (TSAN) was not in the phase-2
  gate set.

- ⚠ **The gate matrix must cover the BUILD matrix.**  Phase 2 broke both txn
  `_nosplit` arms (legacy 3-CL: pos/neg is its own `d_inode` word, no
  `d_iparent` tags) and no gate noticed, because all seven gate configurations
  build the DEFAULT split layout while the benchmark matrix also builds
  `_nosplit` and `_hot1cl`.  A tag-encoded feature is exactly what that axis
  breaks.  `make check-layouts` now runs the tests — not just the compiler — on
  the arms the benchmarks build.
- **Phase 3 (LRU/shrinker)**: `design/dcache-lru-txn.md`; the mixed SW/MW
  commit is the enabler (SW-owned index + MW-shared LRU head in one commit).
- **Standing hazard** — *closed, now machine-checked*.  The fold TRANSFER's
  plain `d_iname` copy was guarded only by the comment-enforced "no reader reads
  a non-top node's name" invariant.  `-DDC_DEBUG_NAME_GUARD` makes the invariant
  say so out loud: the TRANSFER brackets its copy in a per-node odd/even counter
  and every reader-side name access validates it — a seqlock used as a detector
  rather than a retry loop, so an overlap aborts naming the node, and it cannot
  false-positive.  It beats relying on TSAN here because it fires in the ASan
  and plain stress builds, which run orders of magnitude more folds per second.
  `make check-nameguard` gates it, and gates it *both ways*:
  `-DDC_DEBUG_NAME_GUARD_MUTATE` points the bucket-scan match at the host's name
  — the realistic shape of the mistake — and that build MUST abort.  Worth
  recording that the first placement of the mutation was **vacuous**: it only
  touched `dc_readdir`'s callback copy, which `bench_dcache` calls with
  `fn == NULL`, so the mutated line was dead code and the passing run proved
  nothing.  Rule 4.3 fired on the test for rule 4.3 — and then fired again, the
  same way, on the name-width sweep's readdir panel (§4.3b).  `fn == NULL` has
  now produced two false negatives; treat any `dc_readdir` call site in a
  measurement or a test as suspect until it is shown to pass a real callback.
- **Weak-memory validation** — *still blocked, no non-x86 hardware available*.
  Per-hop ordering costs, the move-gate negative, and the mark/pernode reader
  brackets are x86-validated only; ARM/POWER runs are needed before any
  kernel-facing ordering claim, and until then the claims stay scoped to x86.
- **Kernel-facing next step**: map the bucketlock engine onto the kernel's
  `hlist_bl` (the mechanism is already shape-compatible), with the shell /
  fold machinery as the `__d_move` replacement and the mark as the
  causality carrier; the S4 invariant-surface table is the skeleton of
  that argument.

## 7. Where everything lives

| What | Where |
|---|---|
| Plan / thesis / staging | `README.md` |
| Rename mechanism spec (shells, folds, causality arms) | `rename-shell-transition.md` |
| Simplification & invariant-surface analysis, LOC, 1-CL | `simplification-s4.md` |
| Engines | `dcache_seqlock.c`, `dcache_txn.c`, `dcache_bucketlock.c` |
| Vendored kernel rwsem (exact-fair dir-lock arm) | `krwsem/` |
| Validation gates | `make check[-mark|-pernode|-bucketlock*][-tsan]`, `check-nameguard`, `stress*`, `repro*`, `mixed_cycle` |
| Non-vacuous readdir (real per-dirent `qstr`) | `bench_dcache_churn --readdir-names`; prints `READDIR names:` + name sink |
| Sweeps / plots | `scripts/run_dcache*.sh`, `scripts/plot_dcache*.py` |
| Descriptor-slab arms | `scripts/dcache_*.csv` = default; `_rseq`, `_batch`, `_batch_rseq` suffixes are the other three routes (§6) |
| Slab route selection | `URCU_BUILD=` picks the liburcu build; the sweeps and the Makefile DERIVE `-DURCU_SLAB_RSEQ` / `-DURCU_TXN_SLAB_BATCH` from it, since both are header-inline and must match the library |
| Figures | `figures/dcache_{s3,readdir,readdir_churn,churn,churn_scaling,height,optype,optaxonomy,namewidth,swmw}.png`, `figures/perf_dcache_*` |
| Hybrid-engine design notes | `design/dcache-dlm-sw.md`, `design/mixed-sw-mw-txn.md`, `design/dcache-lru-txn.md` |
