# Step 1: move FT FINE to a DLM one-commit lock-set acquire (on MW content)

Scoping note for the first of the two-step engine move
(`ft-inplace-under-dlm-sw.md` §8): **DLM first, on
MW (MCAS) content; then MW→SW as Step 2.** Implements the escalation model's
already-specified one-commit acquire (`mw-writer-lock-escalation-model.md` §5,
§9) — turning FINE's *incremental* COPYING-mark acquire into a single up-front
atomic lock-set acquire. **Scope/plan only.** 2026-07-19.

## 0. Goal and non-goal

**Goal.** Hoist FINE's per-node COPYING-lock acquire from *incremental* (a
standalone `uatomic_cmpxchg` per node as the op descends/builds) into **one
up-front atomic MCAS commit** that sets `{clean→COPYING}` on the op's whole,
conservatively-complete lock-set — address-order-free, all-or-none,
bail-at-first-conflict — then build/mutate under the held locks and release as
today (recorded terminals riding the content flip-txn). This is the
"acquire-all-then-mutate" shape Step 2's SW content requires; it is the reason
Step 1 exists.

**Non-goal (Step 2).** Content stays **MCAS**. Reader side is untouched. The
three lock-set residuals stay on net-A (see §2). No SW txn, no seqcount, no
transacted bitmap here.

## 1. Where FINE is today (the starting point)

- **Acquire = INCREMENTAL.** `ft_meta_copying_mark` (`ft-mutation-helpers.h:523`)
  is a one-word CAS; a multi-node op marks each node separately as it descends —
  recompact marks C (`ft-mutation-node.h:1060`), then P (`:1210`), then GP
  (`:1712`); graft marks `cn` (`ft-graft.h:86`) then `publish_parent`
  (`ft-graft.h:1714`). No single commit sets them together.
- **Deadlock-free via try-or-bail + abort-and-regrow**, not address-order: a mark
  miss is `-EAGAIN` (`helpers.h:520-525`), the op unwinds holding nothing and
  re-descends (recompact `ft-mutation-node.h:1210-1217`/`1937-1941`; graft
  `goto retry_attach`). Cross-trie deadlock is precluded by the exclusive-src
  contract (`ft-graft.h:1029`).
- **Release = recorded MCAS terminal** riding the content commit
  (`ft_flip_txn_record_release_copying` `{COPYING|s→s}`, helpers.h:1446;
  `_tombstone_copying` `{COPYING|s→TOMBSTONE|s}`, :1394), abort-cleared via
  `ft_flip_txn_copying_clear_all` (:642). **This part does not change.**
- **Post-drop, the guard-fallback is live**: `ft_flip_txn_lock_or_guard_parent`
  (:1594) degrades a value-swap target to a §4.B guard on a mark miss.

## 2. Decisions (Mathieu, 2026-07-19)

1. **Keep the residuals under net-A; do NOT close them in Step 1.** The three
   incomplete-lock-set sites — I-1 in-place `nr_child` (guard `ft-insert.h:1638`/
   `:1833` + latch-CAS), I-4b skip-dual `P` (unguarded `record_reserved`), §8.3
   welded `parent_slot_offset` in `state` — stay guard/CAS-arbitrated. MCAS
   content's expected-old (net A) covers them, exactly as today's 1600/1600-passing
   post-drop tree. Closing them (esp. §8.3's state-word layout split) is Step-2
   (SW) work where net A vanishes. So Step 1's acquire is atomic **over the locked
   members**, with the residuals a documented net-A exception.
2. **Gate behind a new default-off flag** (`FEATURE_FT_MW_DLM_ACQUIRE`), the
   incremental scheme staying the fallback — same reversible discipline as the
   FT-wide-lock drop.
3. **Per-op rollout**, each op holding the §11.4 1600/1600 @16w gate + the
   cross-trie oracles before the next.
4. **Escalation lane added LATE.** The per-FT FIFO fairness lane (escalation §3)
   is a bounded-progress refinement, not a correctness prerequisite (try-or-bail
   is already deadlock-free). Land the acquire restructure first; add the lane
   after the core is validated.
5. **Benchmarks are not a Step-1 decision tool.** The exclusion-vs-optimistic
   difference already exists in today's incremental FINE, so an intermediate
   sweep would not settle much. Instead, **pin the OPTIMISTIC baseline now** as a
   fixed reference for the later phases (SW in-place), using the
   `efficios-trie-benchmark` tree. (See §6.)

## 3. The DLM acquire shape (per op)

Restructure each FINE op from "descend-mark-build interleaved" to:

1. **Derive** — a read-only descent that computes the op's conservatively-complete
   lock-set (over-approximate where the exact set is build-discovered). No marks.
2. **Acquire** — ONE MCAS commit recording `{clean→COPYING}` for every lock-set
   member, checking each word PROXY|TOMBSTONE|COPYING-clear. All-or-none; a
   conflict bails the whole commit, holding nothing → **abort-and-regrow**
   (re-derive against current structure, never carry a stale set).
   **Constraint (escalation §5:246): this MCAS is a SEPARATE commit from the
   content flip-txn — it must NOT ride the content txn lane, or it circular-waits
   on the `urcu_txn_domain`.**
3. **Mutate** — build/edit under the held locks (MCAS content, unchanged).
4. **Commit + release** — the content flip-txn commits the structural edges and
   the `{COPYING|s→s}`/`{s→TOMBSTONE|s}` release terminals, as today.

## 4. Engine composition (no helper exists)

There is no `rcu-mcas.h` lock primitive and no multi-lock-acquire helper. FT
composes the acquire from the raw multi-slot MCAS on the **MW** handle
(`ft_flip_txn.mtxn`, a `struct urcu_mcas_txn *`, helpers.h:236):

- Today FT records only the *release* into the txn; the *acquire* is the
  standalone `uatomic_cmpxchg` at helpers.h:523.
- Step 1 adds a **separate acquire flip-txn** whose edges are the `{clean→COPYING}`
  transitions (`urcu_txn_store` / the FT `ft_flip_txn_record_tag` wrapper) +
  `urcu_txn_commit`. A single-member acquire reduces to a bare CAS (no proxy, no
  GP) — matching today's cost for the common single-node op; proxies appear only
  under real lock contention.
- New helper to build: `ft_dlm_acquire(lockset[], n) -> {0 | -EAGAIN}` (name TBD),
  plus its abort-clear. Mirrors the existing `copying[]` registry
  (`FT_FLIP_TXN_MAX_COPYING=8`, helpers.h:233) but on the acquire side.

## 5. Per-op lock-sets — escalation §9 is CLOSED (pinned); this is the reconciliation to the hoist

**No new lock-set design is needed.** Escalation §9.1–§9.6 pin every op and §9.6
declares "§9 closed", reducing the whole mutation surface to **six lock-set
atoms** (§9.6 line 896):

| Atom | Lock-set | Source |
|---|---|---|
| in-place child add/remove | `{node}` | §9.1 I-1, §9.2 R-1 |
| recompact (grow / shrink / relocate) | `{C, P}` (+ `GP` iff `P` compressed) | §9.3 |
| edge publish / graft attach | `{GP_dst}` (+ recompact-pull) | §9.1 I-4, §9.5 |
| detach + prune | `{unlink, orphan-chain[<=depth], recompact {BP,GP}}` | §9.2 R-3, §9.6 |
| dup-chain splice | `{L}` (bare hlist, engine value-CAS; no state word) | §9.1 I-3, §9.2 R-1/2 |
| ordered-list cell splice | pred/succ cells, **same commit** | §9.2 |

Every op composes from these, each acquired in **one domain**; cross-trie ops are
**detach-then-attach** (§9.4.1, decided) so the acquire MCAS never spans two
domains — the sole would-be exception (whole-trie `graft_swap` dual-root) is also
decomposed into three single-domain commits (§9.5 option (b)), so "(C) with zero
exceptions." **So Step-1 design = 0; Step-1 work = build each op's *plan phase*
(read-only derivation of its atom composition) and *acquire* it in one MCAS**,
replacing the incremental marks. Reconciled per op, rollout order:

- **recompact** — `{C, P}` (+ `GP`). **Known immediately, no climb**: `P` is
  `ft_resolve_parent_slot(C)`; add `GP` iff `P` is compressed (a bounded check).
  Today marks C @`ft-mutation-node.h:1060`, P @`:1210`, GP @`:1712` incrementally;
  the hoist resolves the full set first, then one MCAS. Build-invisible
  (`cluster_leaf`) recompact stays unfenced -> `{P}` / nothing (mut-node:1039-41).
  **The simplest hoist; do it first — it is also the atom insert-grow / remove-
  shrink / compactor all reuse (§9.3).**
- **insert** — `{leaf}` (in-place add) | recompact-grow `{C,P}` (§9.3) |
  edge-publish `{GP_dst}`. `parent_nf` is already a RELEASE lock (§9.1 landed);
  the hoist derives `{leaf, parent-iff-split}` up front. The template.
- **remove** — the **plan climb** computes `{BP, GP, orphan[<=depth]}` (+ R-4/R-5
  chain-merge trio `{B, parent_CN, child_CN, publish_parent}` skip-on) + the
  pred/succ cells; the read-set = each climbed ancestor's `nr_child` /
  `external_nodes`. Widest set. The climb + the `-EAGAIN` bails (2035/1169/1875/
  1956) **already exist** as re-plan hooks; the change is *compute-the-full-set-
  in-the-plan* (never grow in place mid-climb) then one MCAS.
- **merge_at** — composes: commit 1 = §9.2 **detach** in `src` domain
  `{BP_src, GP_src, orphan_src}`; commit 2 = **attach** in `dst` domain — `{GP_dst}`
  (M-3/graft) or the M-2 overlap-spine (plan = the read-only `ft_merge_count`
  pre-pass @`ft-merge.h:1142`, bounded by the overlap, not `FT_MAX_DEPTH`).
- **graft / graft_swap** — attach `{GP_dst}` (GLUE) / `{graft-pt}` (NOSPLIT,
  high-byte add pulls its parent via §9.3). graft_swap = three single-domain
  commits (§9.5 (b)); the whole-trie swap's `{dst->root, swap->root}` stays regular
  (no cross-domain acquire).
- **bulk `cds_ft_detach`** — R-3 detach at a *prefix*: `{unlink, orphan, recompact}`,
  **src-side only** (result trie exclusive), **O(prune-depth) not O(subtree)** (the
  subtree moves wholesale by pointer; only its root back-edge re-homes). No new atom.

**Plan-phase inventory (the read-only derivation each op needs):** recompact =
resolve-P + compressed-check (no climb); remove/detach = the prune up-climb;
merge M-2 = `ft_merge_count`; graft = the graft-point descent + GLUE/NOSPLIT
classify. Each already runs *some* descent today; the hoist folds the lock-set
computation into it (§7's "no double-descent" check) and defers the marking to the
single post-plan acquire MCAS.

## 6. Validation + the OPTIMISTIC baseline

- **Correctness gate, per op:** §11.4 point-op 1600/1600 @16w + the cross-trie
  oracles (`inv_concurrent_crosstrie_*_fine_lock`, `inv_concurrent_writers_fine_lock`),
  plain + ASAN, gated build vs default (fallback) build byte-identical when the flag
  is off. Adversarial skeptic on the acquire's abort/re-derive paths (the new
  unwind surface).
- **Baseline pin (parallel, not gating):** capture OPTIMISTIC's current
  update-throughput + reader-latency numbers on the `efficios-trie-benchmark`
  bench as a *fixed reference point*, so the later SW-in-place phase has an
  honest before/after. Survey that tree's existing FT benches first; this is a
  reference artifact, not a Step-1 go/no-go.

## 7. Open before implementation

- **Standalone vs folded:** the acquire-hoist's direct MW-content win is modest
  (fail-fast on conflict); its real value is the Step-2 prerequisite. Kept as a
  standalone bisectable step (Mathieu, per-op rollout confirmed) rather than
  folded into the Step-2 cutover.
- **Derive-pass cost:** the read-only lock-set descent adds a pass before the
  acquire; for the common single-node op it should fold into the existing descent
  (the op already descends to the mutation point) — confirm no double-descent on
  the hot path.
- **Abort-and-regrow convergence** for the build-discovered sets (recompact GP,
  graft spine) — the per-op proofs live in §5's work items.
