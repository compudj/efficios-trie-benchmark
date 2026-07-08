# urcu-txn vs. McKenney's existence structure — the 3-hash atomic-move bench

First concrete comparison of urcu-txn (rcu-mcas multi-word CAS) against Paul
McKenney's *existence structure*, on the same workload his
`perfbook/datastruct/existence/existence_3hash_uperf.c` uses.

## The workload

Three chained hash tables. Each updater owns a disjoint key range and holds
`K = 3·nobjects` keys (`nobjects = (updatespacing-16)/3`, default 15 keys). A
**rotation** moves every one of its keys from its current table `t` to table
`(t+1)%3`. Metric: `ns/rotation = elapsed_ns · nupdaters / total_rotations`
(McKenney's formula). Updaters only — no reader threads (neither does his
`uperf`), so this isolates the **commit mechanism**, not the read side.

## The two commit mechanisms (the whole point)

| | existence | urcu-txn |
|---|---|---|
| Commit a batch of moves | one store to a shared word (`existence_flip`) | one MCAS over the touched bucket slots |
| Cost in batch size | **O(1)** — one word, unbounded batch | **O(width)** — grows with slots; bounded by descriptor capacity |
| Reader cost | +1 tagged-load "existence check" per lookup | none (plain traversal) |

Our engine (`src/bench_txn_3hash.c`) composes each move as
`urcu_txn_hlist_del_prepare` (unlink old from src bucket) +
`urcu_txn_hlist_insert_head_prepare` (link a fresh node into dst bucket), then
`urcu_txn_commit` — `--movesper` moves share one commit (`0` = whole rotation
in a single MCAS, the true `existence_flip` analogue). One escalation domain
(`g_dom`) is shared by all three tables, since a cross-table move is one txn.

## Build & run

```sh
make urcu-txn                     # once: builds urcu-txn-build/
make bench_txn_3hash              # target wired in the top-level Makefile
./bench_txn_3hash --nupdaters 4 --nreaders 4 --nbuckets 4096 --duration 1000 --movesper 0
# existence side (its uperf was patched to add --nreaders + ns/key-move):
perfbook/datastruct/existence/existence_3hash_uperf \
    --nupdaters 4 --nreaders 4 --nbuckets 4096 --duration 1000
```

Both harnesses take the same knobs and print the same two comparable metrics:
`ns/key-move` (update side, work-unit-normalized) and `Mqueries/s` (read side,
a 3-table membership query for a known-present key).

## Results (glibc malloc, 1 s, nbuckets 4096, this 384-CPU x86_64 box)

_These are low-core (≤ 4 thread) points on a shared box; robust to external load
(a handful of threads always find idle cores) but treat as indicative. The
high-core scaling numbers below are NOT (see the ⚠ scaling note)._

Work-unit-normalized (ns/key-move, lower = better) with per-CPU RT `call_rcu`
workers on both sides and the descriptor slab active (see fairness audit):

| config | metric | urcu-txn (`movesper 0`) | existence | winner |
|---|---|---|---|---|
| 1U update-only | ns/key-move | 137 | 141 | tie |
| 4U update-only | ns/key-move | **152** | 190 | txn 1.25× |
| 2U+2R mixed | ns/key-move | 410 | 459 | txn |
| 2U+2R mixed | Mqueries/s | **70.6** | 50.3 | txn 1.40× |
| 1U+4R read-heavy | ns/key-move | 624 | 650 | txn |
| 1U+4R read-heavy | Mqueries/s | **157** | 120 | txn 1.32× |

Reading:
- **The naive "existence is ~2× faster" was entirely the work-unit artifact.**
  Normalized per key-move, the two *tie* single-threaded (137 vs 141) and
  urcu-txn pulls **ahead under write contention** (152 vs 190 at 4 updaters):
  the MCAS descriptor slab + per-CPU reclaim make the commit cheap, while
  existence pays to allocate/reclaim its heavier group + 192 B nodes.
- **Read side: urcu-txn is consistently ~1.3–1.4× faster**, exactly as the
  footprint predicts — plain traversal of 48 B / 1-cacheline nodes vs
  existence's `existence_exists()` tagged-load tax on 192 B / 3-cacheline nodes.
- **MCAS width is not the bottleneck here:** a whole-rotation commit (15 moves,
  ~45 slots) runs with 0 aborts and beats the chunked variant, so the "flip is
  O(1), MCAS is O(width)" concern does not bite at this batch size.
- (Pre-fix, with the single default `call_rcu` worker, 4U was ~2× slower — the
  lone reclaim thread, not the commit, was the bottleneck.)

### Scaling to 192 cores (clean best-of-5, idle box)

Sweep: `scripts/run_txn_vs_existence_scale.sh` (worker i → core i; best-of-5 max)
→ `scripts/txn_vs_existence_scale.csv`; `scripts/plot_txn_vs_existence_scale.py`
→ `figures/txn_vs_existence_scale.png` (3 panels). Full tables + narrative are in
the README section "urcu-txn vs. McKenney's 'existence structure'".

- **Update — most of the glibc gap is an allocator artifact.** Under glibc the
  two tie at 1 core, then existence peaks ~120 M moves/s at 96 cores and declines
  while urcu-txn reaches ~327 M at 128 — a 3.4× gap at 192. But existence
  allocates a group + 3×192 B nodes per rotation, colliding on glibc arena locks.
  Under **jemalloc** existence is rescued (→ ~412 M at 128, 4× its glibc self)
  and urcu-txn's lead shrinks to **~1.1–1.4×** (leaner nodes + descriptor slab,
  not the commit), within **7 % at 192** (416 vs 388). Single-threaded existence
  is actually *faster* under jemalloc (9.5 vs 7.0 M/s). This is the fair result;
  the earlier glibc-only "3.4×" overstated urcu-txn by measuring allocator pain.
- **Read (N readers + 1 updater):** allocator-neutral (readers don't allocate);
  both scale near-linearly to ~3 G queries/s at 191 readers; urcu-txn leads
  ~1.3–1.44× at low counts, converging to ~parity (bandwidth-bound) at scale.

## Fairness audit (answers to three review questions)

1. **Per-CPU `call_rcu` workers — now yes.** Each updater creates its own
   `create_call_rcu_data(URCU_CALL_RCU_RT, cpu)` + `set_thread_call_rcu_data`,
   exactly as `existence_3hash_uperf` does. Originally the bench used the single
   default worker; fixing it ~1.9×'d 4-updater throughput. (The tree's
   `bench_list_scale` uses a more elaborate per-domain worker with manual
   pinning to dodge the liburcu throttled-repin issue — [[callrcu-affinity-init-pin-fix]];
   the per-thread form here matches the existence baseline 1:1, which is what
   fairness needs.)
2. **urcu-txn per-CPU descriptor slab — yes, active by default.** MCAS
   descriptors come from `urcu_mcas_slab` (per-CPU size-classed superblock slab,
   [[mcas-descriptor-slab]]), a process-wide instance in liburcu-common, used
   automatically unless `URCU_TXN_NO_CACHE` is set. Proven by A/B: 4-updater
   `movesper 0` = 4398 ns/rot with the slab vs 6786 ns/rot under
   `URCU_TXN_NO_CACHE=1` (malloc fallback) — a 1.54× slab win, so it is
   demonstrably in use. (Bench nodes themselves are plain `malloc`; only the
   MCAS descriptors use the slab.)
3. **No dead "existence" fields in the txn nodes.** `struct hnode` is purpose-
   built: `urcu_txn_hlist_node` (next+pprev) + key + val + rcu_head = **48 bytes,
   one cacheline**. The existence machinery lives only in the existence engine's
   node: `struct hash_exists` = **192 bytes (3 cachelines, cache-line-aligned)**,
   of which `struct existence_head` alone is 112 bytes. So urcu-txn is not
   carrying existence baggage — it touches ¼ the bytes / ⅓ the cachelines per
   node. That per-node overhead is intrinsic to the existence mechanism and is a
   footprint cost that would show up in a read-side (traversal) comparison.

## Resolved (were the "don't trust it yet" caveats)

1. ~~Work-unit mismatch~~ — **fixed by normalizing to ns/key-move.** Both
   harnesses now count exact key-moves (`existence`'s `hash_rotate` returns its
   move count; ours is `rotations × K`) and report `ns/key-move`, so the
   differing rotation shapes no longer distort the comparison. (Note the
   existence `hash_rotate` loop still only churns a subset of its 15 keys per
   flip — a genuine upstream quirk — but that no longer matters once we divide
   by actual moves.)
2. ~~No readers~~ — **fixed:** both harnesses grew a `--nreaders` membership-
   query engine (identical 3-table lookup of a known-present key). This is what
   exposed the read-side result above.

## Remaining caveats
3. **Allocator.** Both on glibc here; this tree cares about jemalloc/percpu
   (see [[mcas-descriptor-slab]]). The MCAS *descriptor* slab is on for txn;
   the *nodes* are glibc `malloc` on both sides, and existence's `procon` mpool
   recycles its group/node structs. Re-run under jemalloc for the final story.
4. **RCU flavor** differs (txn=QSBR to match the tree's build; existence=
   RCU_SIGNAL). Matters more now that readers are in play — worth equalizing.
5. **Node count is tiny** (15 keys/updater in a 4096-bucket table): near-empty
   buckets, so traversal cost is minimal and the read result is dominated by the
   per-lookup tax + node footprint, which is the intended signal but should be
   confirmed at larger fill.

## Next steps
- Re-run under jemalloc; equalize RCU flavor; sweep read/update ratio and fill.
- Sweep `--movesper` and batch size to find where MCAS width *does* bite.
- Larger key sets per updater (deeper buckets) to stress traversal.
