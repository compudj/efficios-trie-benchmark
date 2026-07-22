# The dcache under bucket lock + SW — the smallest DLM: bucket-head lock (no txn to acquire), then commit with the SW txn

A **minimal writer-engine swap** for the userspace dentry cache, keeping the
existing `dcache_txn.c` architecture intact — the shell-stacking rename, the
async `call_rcu` fold, the `d_host` skip, the reader path, and the walk-causality
version (global / per-node / mark) all **unchanged**. The *only* change is how a
structural commit is made atomic against other writers: replace the multi-writer
MCAS (many CAS per op) with **one or two CAS to lock the affected bucket head(s),
then the same transaction committed via the SW form (plain stores)**. Builds on
[ft-inplace-under-dlm-sw](ft-inplace-under-dlm-sw.md) and
[rcu-gp-bounded-version](rcu-gp-bounded-version.md). **Design only.** 2026-07-20.

## 0. The hypothesis — add/unlink cost is the CAS count

The insert/remove re-sweep on the kernel-faithful seqlock baseline
(`figures/dcache_churn*.png`) gave a clean split: the plain **bit-lock baseline
WINS the write path** ~1.4–2× (a bit-lock + hlist splice beats the txn engine's
per-op MCAS), while the **MW-txn WINS the read path** ~1.3–1.6× (inline compare,
wait-free, no `d_seq` spin).

The conjecture this note acts on: **the MW writer's add/unlink cost is dominated
by the atomic RMW (CAS) instructions of the MCAS commit**, not by anything the
transaction expresses. An MCAS install plants and installs a descriptor per slot
— several `lock cmpxchg` per structural op, plus helping under contention —
whereas the bit-lock does exactly **two** CAS (acquire + release) and plain
stores for the splice. If the conjecture holds, the fix is not to change *what*
the dcache does but *how the commit excludes concurrent writers*:

> **acquire the bucket head lock (1–2 CAS), then commit the existing transaction
> with the SW txn form (plain stores) instead of the MW MCAS.**

The SW form (`include/urcu/rcu-txn-bitmap.h`, MW **and** SW variants, 12/12 TAP)
plain-stores the changed words and installs one selector; it is correct **given
single-writer exclusion on the touched slots**, which the bucket-head lock now
provides. The reader-visible result is identical to the MCAS commit — the same
selector, the same old⊕new atomic flip — so **the reader is untouched, and its
read-path win is kept for free.**

This is the [ft-inplace §3](ft-inplace-under-dlm-sw.md) **DLM + SW** pattern applied at its
smallest. The DLM concept is to acquire a data structure's whole write-set of locks
atomically **via an MW txn** — many locks, deadlock-free, in one transaction. dcache is the
**degenerate** case: its lock-set is just two heads (the hash bucket + the parent's child
list), taken by plain address-ordered CAS with **no txn to acquire them at all** — the SW
txn is *only the commit*. That degenerate acquire is what the engine calls the **bucket
lock**: **bucket lock = the bucket-head lock(s); SW = the commit.** Nothing else moves. (So
"bucket lock" is dcache's specific 2-lock case; "DLM" below is the general pattern it is the
smallest instance of.)

### 0.1 The payoff is concentrated on the shell-FREE ops

The shell is a **rename/move-only** vehicle — add and unlink never touch it. And
rename/move is *not* where the kernel seqlock is strong: the kernel serializes
every rename on `rename_lock`, and cross-directory ones additionally on the global
`s_vfs_rename_mutex`. So on the shell path the txn engine is competing against a
**weak** baseline and already does well; there is little to reclaim there.

Where the txn engine actually **loses** is exactly the **shell-free** path —
add/unlink, the churn workload where the bit-lock baseline wins ~1.4–2×. So the
bucket lock + SW swap's whole payoff lands there, and there it is almost free: an add is a
*single* hlist head-store and an unlink a *single* splice (the deletion mark),
each already one atomic pointer flip. Under the held bucket lock the "SW txn" for
these is therefore barely more than the bit-lock's plain store — **no selector
needed** (a single-pointer publish is already tier-1 atomic to the reader). Net,
for add/unlink:

> **bucket lock + SW add/unlink = the bit-lock's two-CAS write budget, committed into the
> txn engine's transacted hlist, keeping the txn reader unchanged** — i.e. the
> seqlock's writer *and* the txn's reader, by construction, with essentially no
> new machinery. The shell/selector cost is confined to rename/move, where it was
> already the right tool against a weak baseline.

**Exclusion must stay uniform, though.** The moment add/unlink take a bucket-head
lock, every writer of that bucket must — a lock-free MCAS rename mutating the same
chain would race a lock-held add. So rename/move **also** acquires the head
lock(s); it keeps the shell, committed SW under the two-bucket lock. That is for
exclusion-uniformity and to keep the reader/composability, **not** to out-run the
seqlock on a path where the seqlock is already slow.

### 0.2 The durable argument: commit cost is flat in transaction size

Beyond today's add/unlink ballpark, the structural case for DLM is how the two
commits scale with the *number of records* an op touches:

- **MW MCAS = O(records) atomic RMWs.** Each recorded slot gets a descriptor
  planted and installed (a `cmpxchg` per slot), plus read-**helping** if any of
  those slots is contended.
- **DLM + SW = O(distinct locked heads) atomic RMWs + O(records) plain stores +
  at most one selector release-store.** The *atomic* cost is decoupled from the
  record count: every extra slot folded into a transaction is `+1 cmpxchg
  (+ helping risk)` under MCAS but a **plain store** under DLM.

So as the per-op transaction grows, the gap widens. **LRU is the concrete
driver** (and the next dcache feature): an eviction list re-points ~2–4 list
slots on every add/unlink/lookup to move the dentry to the LRU head — +2–4
records/op under MCAS, and the LRU head is a *single hot shared slot*, the MCAS
worst case (colliding updaters pay the read-helping storm, not just extra CAS).
Under DLM those are plain stores beneath the LRU-head lock. So LRU widens the gap
*and* lands it on MCAS's softest spot.

**Honest bound.** The O(1)-vs-O(N) win is largest when the added records
**cluster under few locks** (an LRU list is one structure → one lock). A
*scattered* write-set (e.g. RLU touching many unrelated objects → one lock each)
pushes DLM's lock count toward the record count; there it still trades
`cmpxchg + helping` for `lock + plain store` (a real win on the helping term)
but not the asymptotic one. And a hot shared LRU head serializes *either* engine
(lock contention vs helping) — the real scaling fix there is per-CPU LRU
regardless of engine. Net: DLM's advantage grows with records, most sharply for
clustered structures like the LRU.

This reframes the goal from "match the seqlock on today's churn" to **a commit
whose atomic cost is flat in transaction size** — the durable reason to prefer
DLM as the dcache gains an LRU and beyond.

## 1. What changes, and only this

Per structural op, the commit path goes from

- **today (MW):** build the transaction (transacted hlist edges, shell edges,
  reparents) → `urcu_txn_commit` → MCAS plants/installs a descriptor per slot
  (N CAS) + helping.

to

- **proposed (bucket lock + SW):** **lock** the affected bucket head(s) — one CAS for a
  single bucket (add / unlink), two for a two-bucket op (rename / exchange, an
  all-or-none acquire) → commit the **same** transaction via the **SW** form
  (plain stores + one selector install) → **unlock**.

Everything the transaction *contains* is byte-for-byte what `dcache_txn.c` builds
today. The shell is still stacked; the fold still compresses it; `d_host` /
`host_of_rcu` still give the one-hop content resolve; the walk-causality version
(`GLOBAL rename_gen` / `PER-NODE host gen` / deletion `MARK`) still rides the
commit exactly as now. **No reader change, no layout change, no semantic change.**

## 2. Why it is correct — the lock supplies what SW assumes

The MW MCAS is lock-free/bounded-blocking: it tolerates *concurrent writers on the
same slots* via descriptors + helping. The SW form drops that machinery and
assumes **one writer at a time on the slots it stores**. The bucket-head lock
supplies exactly that assumption and no more:

- **Granularity is preserved.** Writers on *different* buckets take *different*
  head locks and proceed concurrently — the bit-lock's scaling, the reason the
  baseline won the write path. Only same-bucket writers serialize, which the
  bit-lock already did.
- **Two-bucket ops** (rename / exchange) acquire both head locks as one all-or-none
  set (a two-slot acquire, or address-ordered CAS), so the shell stack — both
  index del/insert pairs — runs single-writer over both chains.
- **Reader unaffected.** The reader never takes the lock (it excludes writers
  only) and resolves old⊕new through the selector the SW commit installs — the
  same wait-free view the MCAS commit gave. The read path that already wins is
  not on this change's surface at all.

## 3. CAS accounting — the whole point

| op | MW today | bucket lock + SW proposed |
|---|---|---|
| add | MCAS: ~plant+install per slot (hash edge, child edge) + helping | **1 CAS** lock + plain stores + 1 CAS unlock |
| unlink | MCAS: del from both indexes | **1 CAS** lock + plain stores + 1 CAS unlock |
| rename (shell stack) | MCAS: both index del/insert, loop check, reparent | **2 CAS** lock both heads + plain stores + 2 CAS unlock |

If the conjecture is right, add/unlink fall from *several* CAS to the bit-lock's
**two**, and their throughput should climb onto (or near) the seqlock curve —
while the reader stays on the winning MW-txn curve. That is the single, testable
claim: **bucket lock + SW = the bit-lock's writer CAS budget with the MW-txn's reader.**

## 4. What explicitly does NOT change

To keep the experiment a clean isolation of the CAS-count variable:

- the **shell** vehicle and the **async fold** — kept, and *confined to
  rename/move* where they already were; this is *not* an in-place / COW-retirement
  redesign, and add/unlink remain shell-free;
- `d_host` / `host_of_rcu`, the reader walk, `readdir`, the census;
- the **walk-causality** version and all three arms;
- the dentry **layout** and `d_seq` slot;
- the **cross-directory loop check** — it stays on whatever `dcache_txn.c` uses
  today (`urcu_txn_load_validate` cycle rejection / the mutex), a documented
  residual, since folding it into the acquire is a separate step.

## 5. The fold is just another writer — it takes the lock too (decided)

The SW form is single-writer *on the slots it stores*, so **every** agent that can
touch a locked bucket's slots must go through that bucket's lock. The async fold
worker mutates the shell chain (demote a shell, splice a middle relay), so **the
fold acquires the bucket lock(s) it touches and commits SW**, exactly like the
foreground ops — it is not special, just another writer. This converts the fold's
own commit from MW MCAS to lock + SW as well, keeping the engine uniform.

Consequences to hold in the design:

- **Lock-set / ordering.** A fold that spans a shell's old and new buckets acquires
  both under the *same* all-or-none (or address-ordered) discipline as a rename, so
  fold-vs-rename cannot deadlock. The fold's slot-set is O(1) per node
  (`host_of_rcu`, no chain walk under the lock), so the hold is bounded.
- **Liveness — do not re-introduce the GP stall.** The fold runs on a `call_rcu`
  worker; it must take the lock, do its bounded per-node splice, and drop it —
  **never** walk a chain or block while holding it, or the reclaim worker stalls
  the grace periods the fold itself needs (the prior `fold_ahead` collapse). The
  O(1) fold makes this safe; keep it O(1).
- **Contention is localized.** The fold contends only on the specific buckets it
  compresses, so it serializes against foreground writers of *those* buckets only —
  the same fine granularity as everything else.

## 5a. The store-set, traced (`dcache_txn.c`)

Both add and unlink touch **two transacted heads**, not one:

- **`dc_add`** — `urcu_txn_hlist_add_rcu(&d->d_hash, bucket_of(...))` **then**
  `children_add` = `urcu_txn_hlist_add_rcu(&child->d_sib, &parent->d_child_head)`.
  **Two separate full txn commits.**
- **`dc_unlink`** — `urcu_txn_hlist_del_prepare(&top->d_hash)` +
  `urcu_txn_hlist_del_prepare(&top->d_sib)` in **one composed** txn — same two
  heads.

So the bucket lock lock-set for both is **{hash bucket head, parent `d_child_head`}** — a
**two**-lock acquire (the "or two CAS" case), because a concurrent same-parent
writer races the child head just as a same-bucket writer races the hash head.
Both heads are head words that can carry a bit-0 lock; both are uncontended under
the decontended-ndirs churn (own dirs, own buckets), like the seqlock's per-dir
rwsem + bit-lock pair.

**The CAS-count hypothesis needs the perf check, because it is not obvious from
the trace.** A single `urcu_txn_hlist_add_rcu` is a full txn cycle
(`init→begin→insert_head_prepare→commit→end` + retry loop); the MCAS install is
~1–2 CAS per head, while a bit-lock is lock+unlock = **2 CAS per head** — so the
raw CAS counts are *comparable*, not obviously fewer. The measured seqlock win is
therefore most plausibly **the absence of the txn commit machinery** (descriptor
plant/install/resolve, prepare bookkeeping, the retry loop, and for add the *two
separate commit cycles*) rather than a lower `cmpxchg` count. Two consequences:

- **`dc_add` is the ONLY multi-commit mutator (audited) — this is a bug to fix,
  not just a control.** unlink (both dels), rename (`stack_shell`: both index
  del/insert + reparent), exchange (both shells), and fold (one of three exclusive
  branches) are each **one composed commit that spans both the hash and child-list
  indexes**. `dc_add` alone splits its hash-add and child-add into two separate
  `urcu_txn_hlist_add_rcu` commits with a hand-rolled rollback (`dcache_txn.c:980`),
  so it is **not atomic across the two indexes** (a fresh dentry is hash-visible to
  `lookup` before it is child-visible to `readdir`) and pays two commit cycles.
  **Compose it into one** (both heads in a single txn, matching every other op):
  fixes the atomicity gap *and* halves the machinery. Measure this on the current
  MW engine first — if it closes much of the churn write gap, the culprit was the
  two commit cycles, and the engine swap's marginal value shrinks accordingly.
- The `perf stat` in §5b.3 is now **load-bearing**: it distinguishes "cost = CAS
  count" (swap wins big) from "cost = commit protocol overhead" (swap still wins,
  but compose-first captures much of it).

## 5b. Remaining hazards to verify, not assume

1. **Lock-set completeness — settled above:** two heads (hash bucket + parent
   `d_child_head`). The fold's slot-set (§5) must be traced the same way before it
   takes locks.
2. **Deadlock** across the two-bucket acquire + the child head + the fold — resolve
   by one all-or-none acquire or a fixed global order over {bucket heads, child
   heads}; the loop-check residual (§4) is why the global rename serialization
   stays for now.
3. **The win is CAS-count *or* commit-machinery — measure to tell which.** A
   `perf stat` on `mem_inst_retired.lock_loads` (or equivalent) per op, plus the
   compose-add control above, settles the mechanism rather than inferring it from
   throughput alone.

## 6. Validation

A **fourth arm** (`txn-bucketlock-sw`) on the existing, now-fair sweeps: it must land on
the **upper envelope** — writes on/near the seqlock (bit-lock) curve, reads on the
MW-txn curve — on `dcache_churn*` and the read-under-churn panels, with the
current stress suite (103 + churn / rename / exchange / dir-move conservation) +
TSAN green and a fallback build byte-identical when the flag is off. Roll out
per-op behind a default-off flag: unlink → add → rename → exchange, each gated
before the next, the `ft-step1` discipline.
