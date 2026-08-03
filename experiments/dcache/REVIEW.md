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
  is how the failure *manifests*, not why it starts.

  **DOMAIN AUDIT — clean, and it rules out the obvious suspects.**  Every
  transaction in both txn engines is accounted for:

  | site | domain | abortable? |
  |---|---|---|
  | `dcache_txn.c` ×8 (add, unlink, instantiate, delete, stack, fold, exchange) | `&dc->domain` | yes — correct |
  | `dcache_bucketlock.c` ×3 (stack_shell ×2, exchange) | `&dc->domain` | yes — correct |
  | `dcache_bucketlock.c` fold TRANSFER | `NULL` | **no** — `commit_sw`, pure SW, cannot contention-abort, so it has no lane by design |
  | `urcu_txn_sw_init` ×3 (SW chain ops) | none by construction | no |
  | LRU list add/del (MCAS arm) | `&dc->lru_domain` | yes — correct, and it IS initialised (`lru_shards_init`) |

  So there is no MCAS commit running off-domain, and no abortable commit without
  a lane.  ⚠ Note `gdb` reports the `domain=` argument differently in every
  inlined frame at `-O2` (one pointed into the stack) — that is inlined-argument
  noise, not evidence; do not read those values.

  ⛔ **Four hypotheses tested and FALSIFIED** — recorded so they are not retried:
  | hypothesis | result |
  |---|---|
  | sweep breadth / batch size | wedges at `--evict-batch 1` |
  | park-while-online is the *cause* | it is the manifestation; fixing around it made things worse |
  | the sweeper's shard migration | **was a real bug, fixed** — wedge unchanged |
  | LRU on a separate domain from the index | forcing both onto `&dc->domain`: still wedges |
  | shrinker not reporting quiescent states | `dc_quiescent()` after every single eviction: still wedges |

  ⭐⭐ **INSTRUMENTED (`-DDC_TXN_STATS`), and it answered on the first run** —
  after five guesses had not.  Snapshot taken by signalling a *currently wedged*
  process (`kill -ALRM`), since the run never reaches its own dump:

  | site | attempts | aborts | escalated | published | maxretry | poison |
  |---|--:|--:|--:|--:|--:|--:|
  | `lru_add` | 61,683 | 3,981 | **0** | 0 | 3 | 0 |
  | `lru_del` | 161,247,900 | 161,190,238 | 161,189,563 | 161,189,563 | **161,189,627** | **0** |

  Healthy arm for contrast (`--evict continuous`, same build): 362k commits,
  **zero** aborts, zero escalations.

  What that says, and none of it was guessable:
  - **`lru_del` livelocks.**  `maxretry` ≈ attempts means ONE handle retrying
    161M times and never completing — not many transactions each retrying a few.
  - **Not poison** (0) — the descriptor is well-formed, so it is not a
    mis-computed expected-old.  **Not `-EAGAIN`** (22) — not the
    successor-mid-delete retry path either.  These are genuine *contention*
    aborts.
  - ⭐ **Escalation is not helping it.**  The victim escalates and publishes
    `domain->active` on essentially every attempt, yet its competitor `lru_add`
    shows **0 escalations** — the fast path never enters the lane, so holding
    the lane never confers exclusivity and the victim can be invalidated
    indefinitely by transactions that never queue behind it.

  **FUNNEL INSTRUMENTED (begin-time counters + gdb), and the "escalation is not
  helping" reading above was itself an artifact.**  `DC_TS_COMMIT` fires after a
  commit, and a thread that funnels *parks inside `begin()`* — so it reaches no
  commit and is counted nowhere.  `lru_add` showing 0 escalations did not mean it
  ignored `domain->active`; it meant its threads were **blocked in the funnel at
  that moment**.  Confirmed directly: with `lru_add` reporting `funnel-on 0`,
  gdb showed *both* writer threads in `cds_fair_mutex_park()` inside
  `urcu_txn__enter_fallback()` called from `lru_add`.

  The funnel is therefore working exactly as designed.  What is left is sharper
  and stranger:

  1. the shrinker's `lru_del` retries, crosses the aging threshold, enters the
     lane and publishes `domain->active`;
  2. every writer's next `begin()` sees `active` and parks — correctly;
  3. the shrinker holds the lane (`want_fallback` is `!in_fallback && …`, so it
     never re-enters and never yields it) and keeps retrying **inside** it;
  4. ⭐ **it aborts 32M times for contention while every competitor is parked.**

  That is the anomaly to chase: a transaction that keeps taking *contention*
  aborts when there is no live competitor left to contend with.  Not poison (0),
  not `-EAGAIN` (0). ⚠ Note step 3 also means the lane, once taken by a
  transaction that cannot complete, is never released — so this is absorbing by
  construction rather than merely slow.

  ⭐⭐ **INSTRUMENTED THE INSTALL CAS (new `URCU_TXN_CAS_FAIL` hook in
  `rcu-txn-mcas.h`, same embedder contract as `URCU_TXN_STAT`), and the answer is
  that THE CAS IS NOT FAILING.**  Per-record-index histogram of failed install
  CASes, against the abort count, from three wedged runs:

  | run | commit aborts | install-CAS failures | `-EAGAIN` from `del_prepare` |
  |---|--:|--:|--:|
  | A | 3 | 3 | **380,354,944** |
  | B | 26,640,926 | **2** | 0 |
  | C | 25,750,551 | **9** | 1 |

  26 million aborts against *two* failed CASes.  The abort therefore happens
  **before installation**, on the pre-install validation — i.e. on
  `del_prepare`'s neighbour guard, the `{v -> v}` load-validate it folds in on
  the neighbour whose `next` it does not write (the tombstone guard).  Run A is
  the same fault surfacing at prepare time instead of commit time, which is why
  the split between the two columns moves between runs while the total does not.

  So the loop is: **`lru_del` cannot validate its neighbour guard, forever**, and
  it does so while holding the escalation lane with every competitor parked
  behind it — which is what makes it absorbing rather than slow.  Nothing here
  is contention in the ordinary sense; the counters say there is no competitor
  left to contend with.

  ⭐ **DEL-OUTCOME COUNTER ADDED to separate the candidates** (`del ok / peer /
  FAIL(still-linked) / RELINK-AFTER-FAIL`).  It **falsified** the hypothesis it
  was built for: `lru_del_claimed` returns 1 unconditionally even when the list
  del failed, so a rotate could re-insert a node that was never unlinked —
  doubly linking it, which would make its neighbours' guards unvalidatable
  forever.  Measured across every wedged run: `FAIL 0, RELINK-AFTER-FAIL 0`.
  Not the cause.  (The unconditional return is still sloppy and worth fixing on
  its own; it is simply not this.)

  ⚠ **And it exposed a hole in my own instrumentation, which had produced a
  confident wrong reading.**  `URCU_TXN_CAS_FAIL` covered only
  `install_mw_flat()` — the path a FIRST attempt takes.  A *retrying*
  transaction takes `install_mw_depth()`, and a lone MW edge below the
  escalation threshold CASes inline in `desc_commit()`.  So it reported ~0
  failures against 142M aborts and read as "the CAS is not what is failing".
  Hooking one path out of several is worse than hooking none: it yields a number
  that is wrong in the direction of exonerating the uncovered code.  Now fires on
  all four loss paths (`67022589` upstream).

  **Two distinct wedge modes, across runs of the identical command line:**

  | run | commit aborts | `-EAGAIN` from `del_prepare` | install-CAS failures |
  |---|--:|--:|--:|
  | A | 13 | **337,475,430** | 13 |
  | B | **124,943,648** | 0 | 1 |
  | C | **25,939,515** | 0 | 0 |

  ⛔ **The "pre-install abort path" does not exist — that table mixed two
  builds.**  Runs B and C used the binary from *before* the header copy was
  fixed, so their "≈0 CAS failures" measured a hook that was not there.  Rebuilt
  and re-measured, aborts and CAS failures match 1:1:

  | run | aborts | `-EAGAIN` | failing record index |
  |---|--:|--:|---|
  | 1 | 25,430,784 | 0 | **[3] = 25,430,777** |
  | 2 | 1 | 299,653,846 | — |
  | 3 | 124,462,389 | 0 | **[0] = 124,462,385** |
  | 4 | 6 | 298,772,519 | — |

  So there are two failure modes, both now fully attributed:

  - **Mode A** — the successor-tombstone guard firing forever (`-EAGAIN` ~300M):
    a neighbour left MARKED-but-linked.
  - **Mode B** — **one single record loses its install CAS ~100% of the time.**
    Which index varies by run (0 or 3) because records are slot-address-sorted
    once `retry != 0`, so the position is an allocation artifact; the shape —
    *one* record, essentially every attempt — is not.

  ⭐⭐ **ANSWERED: the slot holds a MARKED pointer.**  Logging the failing
  record's expected `old` against what the slot actually held, and classifying:

  | run | aborts | `-EAGAIN` | same-as-old | **MARKED** | proxy | other |
  |---|--:|--:|--:|--:|--:|--:|
  | 1 | 21,873,334 | 0 | 0 | **21,873,321** | 7 | 6 |
  | 2 | 21,978,031 | 0 | 0 | **21,978,029** | 2 | 0 |
  | 3 | 12 | 249,132,454 | 0 | 3 | 1 | 8 |
  | 4 | 21,950,297 | 0 | 0 | **21,950,289** | 3 | 5 |
  | 5 | 22,062,232 | 0 | 0 | 2 | 3 | 22,062,227 |
  | 6 | 11 | 130,970,550 | 0 | 3 | 6 | 2 |

  Raw, from three wedges: `seen=0x…842 (MARKED)`, `seen=0x…402 (MARKED)`,
  `seen=0x…e82 (MARKED)` — bit 1 set, i.e. a deletion tombstone, where a plain
  successor pointer was expected.

  ⭐ **So the two "modes" are ONE bug seen from two sides.**  Mode A catches it at
  PREPARE (`del_prepare`'s guard finds the successor's `next` marked → `-EAGAIN`);
  mode B catches it at INSTALL (a record's expected-old loses to a marked slot).
  Both say the same thing: **a node is left MARKED-BUT-STILL-LINKED, permanently**,
  so every neighbour operation retries forever.  `same-as-old` is 0 everywhere,
  which rules out ABA and spurious CAS failure.

  The obvious suspect was `del_prepare`'s own documented hazard — two records on
  one slot merging into *"a corrupt edge (a marked-but-still-linked node)"*.
  **Three tests say it is not that:**

  | test | result |
  |---|---|
  | build with `-DDEBUG_RCU` (enables `urcu_assert_debug`) | **no assertion trips**, 4/4 wedges |
  | new `URCU_TXN_POISON` hook on the silent `t->poisoned = 1` merge | **never fires** |
  | raw read of the victim's own `next` vs what `del_prepare` sees | **never marked**; no stale-read either |

  So there is no same-slot merge, no poisoned descriptor, and the victim node
  itself is clean.  What the losing CAS sees is a **MARKED value on a NEIGHBOUR's
  slot**, consistently:

      slot=0x7fab6c124680  old=0x7fab6c2b5bc0  seen=0x7fab6c213502  (MARKED)
      slot=0x7fd2000b4ec0  old=0x7fd200215f80  seen=0x7fd2000b5002  (MARKED)
      slot=0x7f91481d0200  old=0x7f91481cc880  seen=0x7f91481d0342  (MARKED)

  ⭐ **The live hypothesis, and what makes it plausible:** `del_prepare` checks
  the victim (up front, `-ENOENT`) and the SUCCESSOR (the tombstone guard,
  `-EAGAIN`) — but it never checks the **PREDECESSOR**.  A predecessor left
  marked-but-linked is therefore invisible to it: it records
  `prev->next : elem -> next` expecting a plain `elem`, the slot holds
  `MARK(elem)`, and the CAS must lose on every attempt forever.  That fits every
  measurement — the marked neighbour, the untouched victim, no poison, no ABA
  (`same-as-old` 0), and the two modes being the same corruption seen from the
  successor side (`-EAGAIN`) or the predecessor side (CAS loss).

  **`settle` checks out**: it restores `[0..planted)` to `old_ptr` on FAILED, and
  `planted` is the true prefix from both installers.  Not the leak.

  ⭐⭐ **THE CORRUPT STATE IS NOW OBSERVED DIRECTLY**, not inferred.  A validator
  (`dc_lru_validate()`) walks every shard at the wedge:

  | run | shard | `count` | walked | **marked-but-linked** |
  |---|--:|--:|--:|--:|
  | 1 | 0 | 20 | 23 | **5** |
  | 2 | 1 | 16 | 15 | **1** |
  | 4 | 15 | 24 | 16 | **1** |

  So nodes really are MARKED yet still reachable from the sentinel — and the
  shard counter disagrees with the actual list length in both directions.

  ⚠ **The first version of this validator was wrong and would have been a ninth
  false lead.**  Bit 0 is the engine's proxy tag and bit 1 the deletion mark; a
  slot holding a parked DESCRIPTOR has bit 0 set and arbitrary bits above it, so
  testing bit 1 on an unresolved value reports proxies as marks — and at a wedge,
  where a stuck transaction has a planted prefix, that is the common case.  It
  reported whole runs of consecutive "marked" nodes that were simply work in
  flight.  Fixed to classify the two apart and stop at a proxy rather than follow
  it; the numbers above are post-fix.

  ⛔ **AND THE CORRUPTION CONCLUSION DOES NOT SURVIVE EITHER.**  A post-commit
  audit now checks every successful `del` against the three slots it was supposed
  to change, at the instant it reports OK.  Result: **zero violations** — the
  mark always lands and the forward unlink always happens.  `del` is atomic and
  correct.

  Which means the marked-but-linked nodes the walker sees are almost certainly a
  **legitimate transient**, not corruption: `urcu_txn_settle()` writes its
  records back **one at a time**, so between the record that marks `elem->next`
  and the record that repoints `prev->next` the node genuinely IS marked and
  still linked.  A snapshot taken mid-settle sees exactly that.  The
  count-vs-walked discrepancies have the same explanation.

  ⚠ **This audit was ALSO wrong on its first run**, in the mirror-image way to
  the list validator: it resolved the slot before testing the mark, and
  `urcu_txn_list_resolve()` returns the LOGICAL pointer with the mark stripped —
  33,359 false "not marked" reports.  **Raw for the mark, resolved for the
  pointer, and neither for a value mid-commit.**  Between them these two probes
  got the tag handling wrong in both possible directions.

  **What is actually established**, and all that is:
  - the wedge is real, absorbing, MCAS-only, and needs a separate high-frequency
    evictor thread;
  - the spinning transaction is `lru_del`, holding the escalation lane, with
    every competitor correctly parked behind it;
  - it loses to a **MARKED** value — `same-as-old` 0 rules out ABA;
  - it is **not** poison, not a same-slot merge, not a stale read of the
    victim's own next, not `settle`, and **not a broken `del`**.

  ⛔ **AND THE INSERT "ROOT CAUSE" IS WRONG TOO — retracted.**  The audit does
  fire (`aborted-after-clearing-mark` 1–3 per run), and the claim built on it was
  that `insert_before_prepare`'s plain `newp->next = pos` clears a *live* node's
  tombstone.  It does not, because the node is not live:

  - the rotate is `lru_del_claimed()` **then** `lru_add_at()`, and the del has
    already COMMITTED — the post-commit audit proved it atomic, marked *and*
    unlinked;
  - `lru_add_at()` claims `OFF -> BUSY` before touching the list.

  So at insert time the node is already **out of the list and exclusively owned
  by this thread**.  Clearing its tombstone is exactly what re-insertion means,
  and an aborted insert leaves it unmarked-and-unlinked in a state **no other
  thread can reach or claim** — the owner simply retries.  `del_prepare` can
  never see it: `lru_del_claimed` requires `shard >= ON(0)` and ours reads BUSY.
  The counter was measuring correct behaviour.

  (`bad-tail-edge` is likewise a check artifact — it reads `head->prev` after the
  commit without exclusion, so a concurrent tail insert legitimately moves it.)

  **So the bug is still not found.**  What survives is the negative space, which
  is at least large and solid: not poison, not a same-slot merge, not a stale
  read of the victim's own `next`, not `settle`, not `del` (atomic, audited), not
  the insert's prepare-time store, not the funnel, not the shard axis, not batch
  size, not eviction volume, not domain count, not quiescence.  The wedge remains
  `lru_del` retrying forever on a `prev->next` CAS that loses to a MARKED value,
  with `same-as-old` 0, while every competitor is parked in the lane it holds.

  ⭐⭐ **THE STRONGEST REMAINING LEAD — stale RCU traversers, and it is the only
  idea that explains the LOCK-vs-MCAS asymmetry** (the earliest and most solid
  fact we had, and the one every hypothesis above failed to use).

  RCU list deletion deliberately leaves the removed node's `next` pointing into
  the list, so a traverser already standing on it can finish its walk.  The
  rotate is `del` immediately followed by `add_tail` **on the same node with no
  grace period in between**, and `insert_before_prepare`'s plain
  `newp->next = pos` then rewrites the very pointer such a traverser is about to
  follow — teleporting it to wherever the node was re-inserted.

  Why that is arm-specific, exactly:
  - **shard lock** — the rotate runs UNDER the lock, and every traversal takes
    that same lock, so no traverser can be inside the list at that moment.
    Immediate re-insertion is safe by exclusion.
  - **MCAS** — there is no lock; traversal is a lockless
    `urcu_txn_list_next_rcu()`, run by every writer under `--evict continuous`
    and by the shrinker under `--evict bursty`.  A traverser CAN be standing on
    the node.

  It also fits the rest: needs a separate high-frequency evictor (more overlap),
  stochastic onset (needs the window hit), absorbing (a latched stale
  predecessor can never satisfy `prev->next : elem -> next`), and the MARKED
  value the losing CAS sees (that stale predecessor is itself deleted, hence
  marked).

  ⚠ **STILL UNTESTED after two attempts, both of which failed as TESTS rather
  than as answers:**
  1. disabling the rotate — still collapsed, but the rotate is only ONE of two
     paths that re-insert a previously-removed node with no intervening grace
     period; `lru_retain()`'s "re-arm after an LRU_REMOVED" was left in place;
  2. deferring EVERY insert through `call_rcu` so a full grace period elapses
     first — the arm **hangs with `--evict off`**, a configuration the baseline
     runs clean, so the arm is broken and its result says nothing.

  A working test has to survive `--evict off` first.  That is the check the
  second attempt skipped, and it is the same "verify the probe before trusting
  it" rule this section already carries — applied to a whole experiment rather
  than a counter.

  ⭐⭐ **THE REFRAMING THAT DISSOLVES IT (M. Desnoyers): a rotate is a MOVE TO
  TAIL, not a remove followed by an add.**  The node should never leave the list
  at all — so there is no tombstone, no re-add-after-remove window, no
  grace-period question and no shell to fold.

  And that is exactly the arm asymmetry, finally stated properly:
  `lru_rotate_locked()` is unlink + link_tail **under one lock**, so it is
  atomic and the node never observably leaves; the MCAS arm decomposed the same
  operation into TWO independent commits, which is what manufactured the
  marked-and-unlinked window in the first place.  The lock arm is not "luckier"
  — it expresses the right operation and the MCAS arm does not.

  `rcu-txn-list.h` has no move primitive (only `replace`, which still marks the
  old node), and composing the two existing ones does NOT work: `del_prepare`
  records `elem->next : next -> MARK(next)` while `insert_before_prepare`
  plain-stores `newp->next = pos` — the same slot with disagreeing values, i.e.
  precisely the poison case its own comment warns about.

  So the fix is a new primitive, `move_tail_prepare`, recording SIX edges in one
  commit and **no mark**:

      prev->next   : elem    -> next        (unlink forward)
      next->prev   : elem    -> prev        (unlink backward)
      elem->next   : next    -> head        (elem is the new tail)
      elem->prev   : prev    -> oldtail
      oldtail->next: head    -> elem
      head->prev   : oldtail -> elem

  A stale traverser standing on `elem` then follows to the sentinel and
  terminates cleanly rather than being teleported — well-defined, and harmless
  for a CLOCK that is fuzzy by design.

  ⭐ **And it must NOT declare the transaction disjoint** (M. Desnoyers).  Every
  single-op bracket in `rcu-txn-list.h` calls `urcu_txn_declare_disjoint()`, but
  the header is explicit that the line may not be copied into a composed bracket
  "unless the COMBINED write set is provably distinct too … that is
  data-dependent — not knowable from the keys", and that on a disjoint handle a
  colliding prepare "blind-appends a duplicate record: silent corruption".

  A move's six edges over four node pointers alias in ordinary shapes:

  | shape | aliasing |
  |---|---|
  | `elem` already near the tail | `oldtail == prev`, or `oldtail == elem` |
  | moving the second-to-last | `next == oldtail` |
  | moving the sole element | `prev == next == head`, `oldtail == elem` |

  So the move takes the DEFAULT read-your-own-writes handle, where a collision
  chains into one record instead of appending a second.  That is also why
  `del_prepare` carries its `next != prev` skip: the same aliasing, handled by
  hand for the one case a single del can hit.

  ⚠ **Control gap worth recording**: the two builds measured all session differ
  here and I did not notice.  The normal build calls the library's
  `del_rcu`/`add_tail_rcu`, which DECLARE disjoint; the `-DDC_TXN_STATS` build
  uses my `_prepare` wrappers, which do not.  Both wedge, so it is not the
  differentiator and the measurements stand — but an instrumented build that
  changes transaction semantics is not the control it was being used as.

  ✅ **IMPLEMENTED AND TESTED** — `urcu_txn_list_move_tail_prepare()` upstream,
  wired into the MCAS shrinker's rotate.  Correctness holds: 394 checks, and all
  gates including `check-lru-arms` (11 configurations).

  ⛔ **It does NOT fix the wedge** — still collapses, 3/3.  So the
  del+add decomposition, though genuinely the wrong operation, was not what was
  biting.  **Keep it anyway**: it is one commit instead of two, sets no
  tombstone, opens no unlinked window, and removes a real correctness hazard on
  its own terms.  A fix justified by its own semantics rather than by a
  measurement it did not deliver.

  ⚠ Note the rotate is not the last del+add pair: `lru_retain()`'s re-arm after
  an `LRU_REMOVED` still adds a previously-removed node.  That one is a genuine
  re-add (the node really did leave the list), so a move cannot express it — it
  is the case the shell/fold sketch below would actually be for.

  ⚠ This supersedes the shell/fold sketch below AS A ROTATE FIX, which solved the
  wrong problem:
  the shell exists to let a node be re-added while stale traversers hold it, and
  a move means it is never removed, so nothing needs re-adding.  Keeping the
  sketch only because the reasoning is reusable if a genuine remove-then-re-add
  is ever needed.

  ⭐ **Superseded sketch — if in-place RE-ADD were needed at all**,
  the shape is the engine's own rename mechanism, reused.  Stamp a node with the
  RCU epoch at removal; on re-add, compare against the current epoch, and if no
  grace period has elapsed — so a traverser may still hold it — link a **SHELL**
  rather than the host, then **FOLD** the shell out once a grace period has
  passed.  That preserves the RCU contract (the removed node's `next` stays
  valid for stale traversers) while still allowing an immediate re-add, and it
  reuses machinery whose proofs already exist here.

  Two things to weigh before building it: a rotate is the shrinker's COMMON
  action, so a shell per rotate makes reclaim allocate — precisely what you do
  not want under memory pressure — and the same-epoch test will be true for most
  of them under an aggressive shrinker.  The cheaper alternative is to not re-add
  at all: answer LRU_REMOVED and let `retain_dentry` re-arm it on the next touch,
  which is what the kernel does and what this code already does for the in-use
  case.  That costs CLOCK quality (a second-chance entry leaves the list instead
  of moving to the tail) and costs no allocation.

  ⚠ Judge that design on CORRECTNESS grounds, not as a fix for this wedge: the
  one experiment that would have connected them was the broken one.

  ⚠⚠ **Ten hypotheses, nine refuted and one untested — and the pattern in HOW
  they failed is the real output of this investigation.**  Almost every one died not to better
  reasoning but to a probe that was itself wrong: one installer hooked out of
  four; a stale build header; a counter conflating prepare failures with commit
  aborts; a counter read after `end()` had cleared it; a tag tested on an
  UNRESOLVED value (proxies read as marks); the same tag tested on a RESOLVED
  value (marks stripped); and finally a state read without establishing who was
  allowed to touch it.  Rules earned, in order of what they cost:

  1. **Verify a counter can be made non-zero before trusting its zero.**
  2. **Raw for the tag, resolved for the pointer, neither mid-commit.**
  3. **Before calling a state corrupt, establish who is allowed to touch it.**

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
