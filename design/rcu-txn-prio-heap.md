# urcu-txn priority heap: port analysis and a scalable-writer design

Can the babeltrace `bt_heap_` priority heap (stable-2.0,
`src/lib/prio-heap/prio-heap.{c,h}` — static-sized CLRS ch. 6 binary heap over
an array of `void *` slots) be ported to urcu-txn, and would writers scale?
Companion note to [rcu-txn-use-cases](rcu-txn-use-cases.md). 2026-07-02.

Answer in two parts: a **naive port is mechanically clean but writers do not
scale** (the ADT and the `len` word serialize every mutation); a **restricted
workload plus a semantic change to the array makes writers genuinely scale**.
The end state is a new small data structure — a *NULL-sentinel sparse-tail
d-ary heap* — rather than the CLRS heap with a lock swap.

## 1. The source structure

`struct ptr_heap`: `{ len, alloc_len, void **ptrs, gt() }`. Operations:

- `maximum` — read `ptrs[0]`, O(1).
- `insert` — append at `len`, sift-up in "hole" form (moves parents down,
  writes the new element once). O(log n) slots.
- `remove` / `replace_max` — take root, move last element to root, sift-down
  (swap form). O(log n) slots.
- `cherrypick` — O(n) scan, then O(log n) fix-up.
- `heap_grow` — allocate-copy-free of the whole array (calloc, so the tail is
  zero-filled — relevant below).

## 2. Naive port: mechanics

The array-of-pointer-slots shape is exactly the engine's native currency; each
operation becomes one transaction over one root-to-leaf path.

- **Sift form.** `urcu_txn_store()` buffers writes invisibly to the bracket's
  own loads, so `heapify()`'s swap loop (which re-reads `ptrs[i]` after
  `i = largest`) must be rewritten in **hole form**: carry the sifting value in
  a local, load both children per level, store the winner up into the hole.
  Insert's sift-up is already hole-form. No read-after-write remains.
  Insert: ≤ d loads, ≤ d+1 stores; remove: ≤ 2d loads, ≤ d+1 stores
  (d = ⌈log₂ n⌉).
- **Old-value discipline.** `urcu_txn_store()` wants the caller-supplied old
  value; every overwritten slot was just loaded. Natural fit.
- **ABA.** Heap traffic is ABA-heavy (the same pointer migrates between slots;
  `len` oscillates through the same values). Covered since the per-record
  install latch: the engine tolerates all slot-value ABA.
- **Grow.** Cannot transact an O(n) copy. Either fix the capacity (the
  babeltrace usage is one slot per stream — effectively static), or treat grow
  like a `cds_lfht` resize: rare, mutex-guarded, RCU-swap the array, and every
  transaction carries the array pointer as one extra read-set member.
- **Cherrypick.** As a transaction it is disqualifying: an O(n) scan puts the
  whole array in the read set. The fix is use-case B composition: maintain a
  node→position index updated *in the same transaction* as each sift move.
  Cherrypick becomes an O(log n) transaction; write sets double but stay
  logarithmic. No lock-based design gets that atomicity this cheaply.
- **Reads.** `maximum` is a single resolved slot read — wait-free, always a
  committed value, and always a legal answer (every committed state has the
  max at the root). Tier-1, zero cost.

## 3. Naive port: why writers do not scale

Three universal serialization points sit in every writer's footprint:

1. **`len`** — every insert and remove writes it. One word written by all
   writers: structurally the bucket-count→1 regime of the write-contention
   study, as a floor.
2. **The root slot** — every remove/replace_max writes `ptrs[0]`. Two
   concurrent replace_max conflict *by definition*: a strict priority queue
   linearizes delete-max on THE extremum. This is the ADT, not the engine —
   it is why the concurrent-PQ literature abandoned strict heaps for skiplist
   PQs (Lotan–Shavit, spray) and relaxed MultiQueues.
3. **Cache geometry** — 8 slots/line, so levels 0–2 share one line; every
   writer's descriptor install/settle invalidates it.

Property C (writers scale) holds for disjoint write sets; a single strict heap
is the anti-case. Expect flat-to-negative scaling past one writer — graceful
degradation (property D) means no livelock, but the ceiling is ~single-writer
throughput. One nuance: **inserts alone are nearly scalable in principle** — a
random insert sifts up O(1) levels in expectation, so its write set is
typically {leaf, `len`}; `len` is the only universal insert-insert conflict.
That observation is the lever for §5.

## 4. The workload that changes the analysis

**Single popper, many remote inserters**: a scheduler pops the min from its
own per-CPU heap; migration code on other CPUs inserts into it. Then:

- The worst conflict (root vs root) vanishes — exactly one thread ever writes
  slot 0.
- Insert-insert and insert-pop conflicts reduce to `len` plus the tail region
  (pop reads/NULLs `ptrs[len-1]` exactly where inserts append).
- Transactions are tiny (a runqueue heap is hundreds of entries → depth ≤ 10).

What urcu-txn buys over an `rq->lock` here:

1. **No lock-holder preemption.** A preempted remote migrator's transaction is
   helped or aborted by the local scheduler (bounded blocking + install
   latch/TSE). For a *userspace* scheduler this is the headline feature.
2. **Wait-free remote peeks.** Load-balancer sampling of other CPUs' heap
   minima is a tier-1 single-slot read — no lock, no dirtying of the observed
   runqueue. (MultiQueue-style best-of-k sampling for free.)
3. **Atomic cross-runqueue migration.** Remove-from-heap-A + insert-into-heap-B
   in one commit: the task is never in limbo, "in exactly one queue" holds at
   every instant, no double-runqueue-lock ordering dance. With the position
   index, remote removal (kill, affinity change) is O(log n) too.

Incumbent to beat: the **MPSC inbox** (remotes push a wfstack/llist, owner
splices into a private, synchronization-free heap — Linux's `ttwu` wake-list
pattern). On raw insert throughput the inbox wins. rcu-txn earns the port when
the workload needs what the inbox structurally cannot give: immediate
priority-ordered visibility (no unordered-inbox latency), remote removal,
atomic cross-queue moves, or lock-free observers. A scheduler with affinity
changes, remote cancellation, or sampling-based balancing needs two or three
of those. Engine choice: **multi-writer `rcu-mcas`** — migrators are writers,
even with a single popper (`rcu-txn-sw` does not apply).

Still with `len` in every write set, though: inserters serialize on one short
commit — roughly well-behaved-lock throughput, not scaling. §5 removes it.

## 5. NULL-sentinel sparse-tail heap: making writers scale

Proposal: drop `len` as a transacted count; the array is NULL-terminated and
`alloc_len` (power of 2, calloc'd → tail pre-NULLed) changes only on rare
grow.

**Warning — the literal version buys nothing.** With strict contiguity, "where
does the heap end" is still one shared datum, reified into the boundary slots:
two inserts must claim the *same* first-NULL slot (write-write conflict, same
as both CASing `len`); insert must validate its predecessor is non-NULL, which
pop NULLs (read-write conflict, same as `len`). Conflict graph isomorphic.

The gain requires letting the NULL sentinel carry the synchronization role,
via **local validation** and **transient holes**:

1. **NULL means "empty slot" everywhere**, not "end of array". Comparator
   treats NULL as +∞ (min-heap).
2. **Insert = claim any empty slot whose ancestors are live.**
   The claim is `urcu_txn_store(slot, /*old*/ NULL, p)` — the old-value check
   *is* the emptiness validation. The sift-up comparisons put the compared
   parents in the read set. Nothing else is validated. The frontier *search*
   (binary probe for the NULL boundary, or an advisory hint updated with plain
   relaxed stores) runs **outside** the transaction — it only picks a
   candidate; a stale guess loses the old=NULL check and retries.
3. **Pop = take a leaf, validated locally.** The victim's two child slots go
   in the read set (must be NULL); store NULL to the victim, move its value to
   the root, sift down. "Last non-NULL" degrades from correctness requirement
   to search heuristic; the single popper keeps a thread-private position
   hint.
4. **Tolerate holes near the frontier.** Pop NULLing slot t concurrent with an
   insert claiming t+1 commits *both* — transient hole at t. A missing node
   never breaks heap order (every live element ≤ its live descendants);
   sift-down treats a NULL child as +∞; holes self-heal because inserts prefer
   the lowest empty slot. Ancestor chains of frontier slots have indices ≈
   half as large, i.e. deep in the dense region — never holes. Hole count is
   bounded by concurrency; depth stays log n + O(P).
5. **Scatter.** Concurrent inserters that collide on a claim re-target with a
   small randomized offset among the empty frontier slots (Hunt-style path
   scattering, with the transaction engine replacing all of Hunt's tag/lock
   machinery for the interleavings).

Resulting conflict graph:

- `len`: **gone** from every read and write set.
- insert vs pop: only genuine slot overlap (pop's sift path crossing an
  insert's ~one-level sift-up, or same tail slot). Rare for random priorities.
- insert vs insert: only same-slot claims; scatter makes them mostly disjoint.
- Universal residue: the array pointer (read-only, changes only on grow — and
  grow is simpler now, the new tail arrives pre-NULLed) and *physical* line
  sharing at the frontier — bandwidth, not serialization; no aborts. §6 kills
  that too.

Invariant to state (and prove) explicitly: *heap-ordered forest of live slots;
holes confined near the frontier; leaf-ness of a pop victim certified by its
children-NULL reads at commit*. This is a distinct data structure with its own
correctness note, not the CLRS heap with a tweak. Tiny-heap edge cases (n < 4)
fall back to serialized behavior.

## 6. Memory layout: cacheline-bucketed d-ary heap

False sharing never caused aborts (conflicts are per-slot, logical), so
alignment is purely about coherence traffic — but naive one-slot-per-line
padding destroys traversal locality (2 misses/level for sift-down; the dense
top-of-heap line spreads over seven). The right layout:

- **d = 8-ary heap, sibling group = one 64 B line** (`aligned_alloc`).
  Sift-down fetches one line per level; depth drops to log₈ n (256 entries →
  3 levels). Shorter paths → smaller read/write sets → fewer logical
  conflicts, on top of the bandwidth win.
- **Scatter at group granularity** (stride 8): concurrent inserters never
  write the same line — false sharing eliminated by construction, no padding.
- **Root + hot metadata (advisory frontier hint) on a dedicated line**, so
  remote tier-1 peeks of the minimum don't collide with the popper's sift
  writes to former siblings.
- **Inline keys variant**: d-ary sift-down does up to d comparisons/level and
  `gt()` dereferences the elements — trading slot-line misses for element
  misses is a worse currency. Store the key inline (16 B slot: key + pointer,
  d = 4/line; scheduler key = deadline/vruntime fits a word). Sifts never
  touch elements. Each moved slot is two transacted words (write set ×2), but
  paths are 2–3× shorter from the arity change — transaction size roughly
  breaks even, element misses vanish. Caveat: key+pointer commit atomically
  only inside a transaction; an *untransacted* peek may read the key word
  alone (fine for sampling heuristics), but a peek that acts on the pointer
  must load both through the engine.

Recommended spec: **8-ary NULL-sentinel sparse-tail heap** for pointer-only
slots; **4-ary with inline keys** for the scheduler case. Root/metadata on a
dedicated line, group-granular scatter.

## 7. Honest constraints

- **Multi-popper does not scale, ever, in the strict ADT.** Concurrent
  delete-mins serialize on the root regardless of everything above. If the
  workload grows a second popper (work stealing beyond rare cases), switch
  designs: MultiQueue/per-CPU heap ensemble with best-of-k sampling (relaxed
  order — inadmissible for strict consumers like a trace merge), or a skiplist
  PQ under txn (inserts disjoint, delete-min still head-contended).
- **babeltrace itself gains nothing**: the merge iterator is a single-threaded
  replace_max loop and needs THE min (rank-O(P) relaxation reorders events).
  If live observers of the merge top are ever wanted, `rcu-txn-sw` gives that
  without MCAS cost on the writer.
- The sparse-tail invariant proof (holes-near-frontier under scatter +
  concurrent pop) is load-bearing and deserves a model-checked or at least
  carefully argued write-up before any port.
- Not prototyped; no measurements yet.

## 8. Benchmark plan (when prototyped)

Engines: rcu-mcas sparse-tail d-ary heap vs (a) per-CPU lock + dense heap,
(b) MPSC inbox (wfstack push, owner splice) + private heap — the real
incumbent, (c) naive rcu-txn port with transacted `len` (to isolate the §5
gain). Workloads: 1 popper + w inserters (w = 1..CPUs), random priorities;
add rare cross-queue migration and remote-removal mixes where only (a) and
the txn engine can compete at all. Metrics: per-op throughput, abort rate,
frontier cacheline traffic (perf c2c), pop latency tail (lock-holder
preemption A/B under CPU oversubscription).
