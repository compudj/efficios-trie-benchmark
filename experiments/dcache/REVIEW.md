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
   rename / file move / directory rename / directory move table — two cells
   still have no perf bench).

4. **Isolate the path under test.**  Homogeneous mixes are writer-bound
   (a rename ≈ 50× a lookup) and mask reader effects — role-split
   readers/writers.  Decontend what is not being measured (dirs per
   writer), pin one HW thread per core from hwloc, pin the allocator
   config, prime all engines, keep NUMA placement of the measured role
   stable across the sweep variable.

5. **Strip harness ALU that hides memory effects.**  The per-lookup
   `snprintf` was ~74% of instructions and ran concurrently with descent
   stalls — it hid the entire 1-CL layout win ("neutral" was a harness
   artifact).  Per-op counters (instructions, L1d/LLC misses) are the
   robust observable when absolute throughput is pinning-sensitive; on a
   loaded box, interleave A/B runs instead of running arms sequentially.

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

- **Coverage gaps** (`dcache-rename-taxonomy`) — *harness landed, sweep not yet
  run*.  All four cells now have a bench.  `bench_dcache --op-mix
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
- **Mark-arm figures** — *control landed, sweep not yet run*, and the mechanism
  turned out not to be the one recorded.  The **dentry** is not where the arms
  differ: measured, `sizeof(dentry)` is 168 (176 bucketlock) and `d_hash` is at
  @56 on the mark arm and on every other txn arm, so the Makefile's standing
  claim holds.  `struct qstr` is shared with `struct dc_path`, though, so
  `DC_NAME_MAX` also sets the **harness path object**: `sizeof(dc_path)` 964 →
  1156 B, a 48- instead of 40-byte struct copy per path component, and a 20%
  bigger precomputed leaf-qstr table — all on the reader's hot path, none of it
  the mechanism under test, and all of it running *against* the mark arm (so the
  published mark reader numbers are, if anything, conservative).  Note this also
  contradicts the "the leaf-qstr table is a co-footprint, identical for every
  binary" aside in `bench_dcache.c`: it is identical for every binary *except*
  the mark arm.  Two controls, since one knob moves two things:
  `-DDC_NAME_MAX=32 -DDC_NAME_PAD=8` leaves the dentry byte-identical and
  matches only the harness path (the arm to publish alongside), `-DDC_NAME_MAX=32`
  alone also shrinks the dentry 8 B and moves `d_hash` to @48.  Sweep:
  `scripts/run_dcache_namewidth.sh` → `figures/dcache_namewidth.png`; `make
  namectl` builds the arms.  A control landing on the shipped curve retires this
  caveat with evidence.
- **Phase 2 (negative dentries)**: `stack_shell` must copy pos/neg into the
  shell (today every node is born positive, so top==host coincidentally);
  this is a recorded dependency of the `d_iparent`-race fix.
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
  nothing.  Rule 4.3 fired on the test for rule 4.3.
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
| Sweeps / plots | `scripts/run_dcache*.sh`, `scripts/plot_dcache*.py` |
| Figures | `figures/dcache_{s3,readdir,readdir_churn,churn,churn_scaling,height,optype,optaxonomy,namewidth,swmw}.png`, `figures/perf_dcache_*` |
| Hybrid-engine design notes | `design/dcache-dlm-sw.md`, `design/mixed-sw-mw-txn.md`, `design/dcache-lru-txn.md` |
