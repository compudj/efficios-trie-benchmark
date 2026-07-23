# The dcache LRU under the txn engine: fuzzy ordering, and where locks/MCAS actually belong

How a dentry-cache LRU should be built on top of the bucket-lock + SW txn engine
([dcache-dlm-sw.md](dcache-dlm-sw.md) §0.2 named the LRU as *the* concrete driver
for that engine but left the mechanism open). The headline: the LRU wants a
**fuzzy** order, it does **not** want the SW txn at all, and the one place MCAS is
genuinely on the table is write-side concurrency between the shrinker and dentry
ops — decided by reclaim cadence, not by readers. **Design + kernel analysis,
2026-07-22.** Kernel facts are from Linux 7.0.0 (`/mnt/data/efficios/git/linux`).

## 1. What the kernel dentry LRU actually is

`sb->s_dentry_lru` is a `struct list_lru` — **not** one list and **not** per-CPU:

- `struct list_lru { struct list_lru_node *node; … }` — an array indexed by
  **NUMA node id** (`&lru->node[nid]`, `mm/list_lru.c` `list_lru_add`).
- each node holds a `struct list_lru_one` per memcg (root inline + an xarray of
  per-memcg ones under `CONFIG_MEMCG`).
- each `list_lru_one { struct list_head list; long nr_items; spinlock_t lock; }`
  carries its **own spinlock** (`include/linux/list_lru.h`: *"protects all fields
  above"*).

So the sharding is **(NUMA node × memcg)** and every list mutation takes that
shard's spinlock (`lock_list_lru_of_memcg` → `list_add_tail`). Add is at the
**tail**; the shrinker walks from the **head** (oldest first). *(The
`s_dentry_lru_lock` in the `fs/dcache.c:48` comment is stale pre-`list_lru`
documentation — the real lock is `list_lru_one.lock`.)*

## 2. Access does not bump the list — a per-object "referenced" bit does

This is the load-bearing design decision and the reason a shared LRU head does
not melt under lookup load:

- A **pure RCU-walk lookup touches the LRU zero times** — `__d_lookup_rcu` has no
  reference to `d_lru` / `DCACHE_REFERENCED` / `list_lru`. It pins via `lockref`,
  which is independent of LRU position.
- A dentry is **added to the LRU only on the *last* `dput`** (`retain_dentry`,
  `fs/dcache.c:776`), at the tail.
- On a *subsequent* last-put of a dentry already on the LRU, the kernel does **not
  move it** — it sets a per-object flag `dentry->d_flags |= DCACHE_REFERENCED`
  (`fs/dcache.c:783`).
- The list is structurally mutated in only three places: **add** (last-put),
  **del** (`__dentry_kill`), and **rotate/isolate** by the shrinker.

"Recently used" is therefore a **plain per-object store**, never a list-head
write. Reader-side contention on the head simply does not exist.

## 3. The order is fuzzy (CLOCK / second-chance), by construction

`dentry_lru_isolate` (`fs/dcache.c:1179`) is a clock algorithm:

- `d_lockref.count != 0` → in use → **remove from LRU** (`LRU_REMOVED`);
- `DCACHE_REFERENCED` set → **clear it and `LRU_ROTATE`** (move to tail = second
  chance);
- else → move to the shrink freeable list → killed.

Precise recency is never attempted. **A precise LRU is doubly disqualified**: it
would (a) write the shared head on *every* access and (b) need reader-atomicity on
the list. The referenced-bit + second-chance sidesteps both. Fuzzy is not a
compromise here — it is the only thing that scales, and it is what mainline does.

## 4. Does the LRU want the SW txn? No.

The SW txn's one product is a **reader-atomic multi-word flip** (the selector,
old⊕new). The LRU has **no lockless reader** to consume it (only the shrinker
traverses `d_lru`, under the shard lock; a lookup never reads it). Walking the
value against the need:

- reader-atomic flip — no reader ⟹ dead weight (a selector nobody resolves);
- single-writer discipline — already supplied by the shard lock;
- atomic composition with the index edit — unnecessary: no reader spans
  {hash entry} and {on-LRU?} in one lockless read; mainline coordinates the two
  with `d_lock` + `DCACHE_LRU_LIST`/`DCACHE_REFERENCED`, which our per-op locks
  already give;
- walk-causality / generation — LRU ops are not part of walk causality (renames
  bump the gen, not `d_lru_add`).

So the LRU never wants the **SW** form: with no lockless reader, `store_sw`'s
resolve has no consumer, so the ≤3 list slots and the referenced bit are plain
`WRITE_ONCE` stores under the shard lock (in the lock design). This **sharpens**
[dcache-dlm-sw.md](dcache-dlm-sw.md) §0.2 rather than contradicting it: on the
READER axis the LRU is evidence for the **bucket lock**, never for the **SW
commit**.

⚠ But this is NOT evidence against **MW/MCAS**.  The engine has two products —
reader-atomic visibility (both SW and MW, dead here) and writer-atomic lock-free
multi-slot concurrency (MW/MCAS only, and very much alive) — and §5–§7 below show
MW/MCAS is a real candidate on the WRITE side (random-position removal, which
neither the SW form nor a lock-partition can handle).  So the rule is **"no SW
here", not "no txn here"** — the per-slot question is *does any slot have a
concurrent writer*, and a random removal makes the enqueue's own tail edges MW
even with one enqueuer.  (See the `rcu-pseudo-transaction` skill, *When it is the
right tool — two products*.)

Partition of an add/unlink (lock design):

- **hash bucket + parent `d_child_head`** → SW commit (lockless lookup / readdir
  readers need the atomic flip);
- **LRU shard head + `d_lru` links + referenced bit** → in the LOCK design, plain
  stores under the same held locks (no reader → no **SW**); in the MCAS design
  (§7), MW records for the concurrent edge re-points (no reader → still **MW** on
  write-side grounds).

## 5. The real axis: write-side concurrency (shrinker vs dentry ops)

The one genuine argument for richer machinery is **not** readers — it is
writer-vs-writer contention on a single shard lock, between:

- **enqueue** (the frequent last-`dput` add, at the tail), and
- **removal** (the shrinker's head-dequeue + rotate, and `unlink`'s splice).

A single shard lock serializes the flood of enqueues against the shrinker even
though they touch opposite ends. Splitting the lock by *operation* — an
**enqueue lock** (tail) and a **removal lock** (head/rotate) — is a textbook
Michael–Scott two-lock queue, needs **zero reader machinery** (plain stores under
each end), and decouples the shrinker from the hot enqueue path. Two orthogonal
knobs give full decoupling:

- **per-CPU shards** kill producer↔producer (every CPU enqueues its own shard),
- **enqueue/removal split** (or MCAS) kills producer↔consumer (dput vs shrinker).

For contrast, mainline does **neither** split: it sits on the per-node shard lock
and instead (a) shards per-NUMA-node, (b) has the shrinker **batch-isolate**
victims to a private list (`DCACHE_SHRINK_LIST`) under the shard lock then process
them *without* it, and (c) `spin_trylock(&d_lock)` + `LRU_SKIP` so the shrinker
never *blocks* a foreground op. It bounds the contention rather than eliminating
it — a userspace engine with per-CPU shards can go further.

## 6. Why lazy deletion fails under unlink churn (the deciding constraint)

The clean two-lock FIFO only supports {enqueue-tail, dequeue-head}, **not**
`unlink`'s arbitrary mid-list splice. The tempting fix — have `unlink` set a
*dead* bit and let the shrinker reap it later — **breaks under unlink churn with
no memory pressure** (i.e. exactly `dcache_churn`):

1. **Tombstone accumulation.** A pressure-driven shrinker is idle with no
   pressure, but churn keeps minting dead nodes → the LRU grows unbounded and the
   eventual walk is O(dead) to find anything live.
2. **Retention is gated on the shrinker, not the grace period** — the killer.
   The dentry's `call_rcu` free cannot fire while the list still points at its
   `d_lru` node, so the free waits for the shrinker to *physically* unlink it, not
   for the RCU grace period. Lazy-delete turns "freeable after one GP" into
   "freeable whenever reclaim wanders by" = unbounded live memory under
   churn-without-pressure.

A Harris-style logical-mark does **not** rescue it: Harris relies on *every
traverser* helping physically unlink marked nodes, but the LRU has exactly **one**
traverser (the shrinker), so a mark-and-defer is just lazy-delete again.

Conclusion: **`unlink` needs immediate physical mid-list removal.** That rules out
lazy schemes and the plain two-lock FIFO, leaving only two mechanisms for an O(1)
arbitrary splice on a doubly-linked list.

## 7. The remaining choice — decided by reclaim cadence

- **Per-CPU shard + single lock/shard + batch-isolate shrinker** (mainline-style,
  scaled). Common path stays cheap: enqueue/unlink take the *owner's* shard lock
  (uncontended under pinned writers) + plain stores; removal is immediate. The
  cost you wanted gone — shrinker↔owner contention on that shard lock — is
  **bursty**: it bites only while the shrinker isolates a batch, and batch-isolate
  bounds the hold. Best when **reclaim is bursty / pressure-driven**.
- **MCAS bidir list** (the machinery already in-tree). Immediate lock-free
  mid-splice, fully concurrent with enqueue/dequeue, shrinker never shares a lock
  with dentry ops. But it taxes **every** enqueue and **every** unlink with a
  descriptor + multi-CAS — a constant cost on the hot churn path to decouple the
  consumer. Best when **eviction is continuous** (bounded-size cache,
  evict-on-insert), where the consumer is always on.

**The crux, and the irony:** MCAS-on-LRU is the mirror image of the index
decision. On the hash/child index we moved *off* MCAS to plain-stores-under-lock;
putting MCAS back on the LRU pays per-op descriptors again — worth it only if the
consumer is permanent. So **fix the reclaim model first**:

- bursty/pressure-driven reclaim → per-CPU shard lock wins decisively (MCAS would
  tax every churn op to avoid a reclaim-storm-only contention);
- continuous eviction → the shrinker is a permanent consumer, so MCAS's constant
  decoupling buys constant benefit.

Across all of it, the SW txn never enters the LRU either way.

## 8. What to measure / next steps

- Make the **reclaim cadence explicit** in the harness: a bounded-size cache with
  a **continuous evictor** (evict-on-insert) is the realistic dcache and the
  adversarial case for the lock design; a pressure-triggered shrinker is the
  bursty case.
- Bench arm: `dcache_churn` driving a continuous evictor, **per-CPU-shard-lock vs
  MCAS-bidir**, watching *both* churn throughput *and* live-set size (to catch any
  tombstone/retention regression). If shard-lock contention shows up under
  continuous eviction, MCAS earns its keep; if reclaim is bursty, the locks win
  and MCAS is a tax.
- Layout: a per-CPU LRU shard head + `d_lru` prev/next + the `DCACHE_REFERENCED`
  analog bit; the referenced bit and (in the lock design) the `d_lru` links are
  cold, writer/shrinker-only fields — keep them off the reader's CL0 hot line.
- This slots under [dcache-dlm-sw.md](dcache-dlm-sw.md) §0.2 as the concrete LRU
  mechanism, and reuses the bidir-MCAS list already exercised by
  `bench_list_scale` for the MCAS arm.
