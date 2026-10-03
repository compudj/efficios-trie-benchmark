# dcache-in-userspace: can urcu-txn dissolve `rename_lock`?

Status: **S1–S4 done** (2026-08-01); **every figure re-swept 2026-10-01** on
the corrected methodology and the chain-free rename shells — see
[Results](#results-re-swept-2026-10-01). Three
engines behind one interface —
the kernel-style `seqlock` baseline, `dcache_txn` (global / per-node / mark
causality arms) and `dcache_bucketlock` (per-bucket lock + SW txn, the winner on
writes) — all at 103/103 with ASan- and TSAN-clean stress. The txn rename
mechanism is specified in [`rename-shell-transition.md`](rename-shell-transition.md);
the simplification and invariant-surface analysis is in
[`simplification-s4.md`](simplification-s4.md); and
[`REVIEW.md`](REVIEW.md) is the retrospective — verdict, the design and
methodology rules the experiment produced, and what is still open.

Read `REVIEW.md` §4 before adding a benchmark here. Most of the wrong numbers
this experiment produced came from a harness that passed while measuring
nothing, not from an engine that was slow.

### Files

| File | Role |
|---|---|
| `dcache.h` | engine-agnostic interface + inline qstr/path helpers; the two bolt-on seams |
| `seqcount.h` | userspace seqcount + seqlock over urcu barriers (the `rename_lock`/`d_seq` machinery) |
| `dcache_seqlock.c` | faithful kernel-style baseline (RCU hlist + global `rename_lock` + per-dentry `d_seq`) |
| `dcache_txn.c` | urcu-txn engine; `-DDC_PER_NODE_GEN` / `-DDC_MARK_GEN` select the causality arm |
| `dcache_bucketlock.c` | per-bucket lock + SW txn |
| `krwsem/` | vendored Linux `rw_semaphore` (GPL-2.0): the seqlock baseline's default per-directory lock (`i_rwsem`) |
| `dcache_node.h` | LRU shard axis: this CPU's NUMA node (rseq, else `getcpu()`) |
| `dcache_qspinlock.h` | every engine's LRU shard lock: a port of the kernel's queued spinlock (`list_lru_one.lock` is a `spinlock_t`) |
| `dcache_bench_rand.h` / `dcache_bench_pace.h` | the harnesses' PRNG (xorshift64* + multiply-shift) and writer pacer (`--rename-rate` / `--churn-rate`) |
| `dcache_bench_setup.h` | the harnesses' setup placement: seeded namespace on the first worker's node (memory policy across a re-exec) |
| `dcache_bench_run.h` | the harnesses' run phases: an untimed `--warmup` (200 ms) before the timed window |
| `rename-shell-transition.md` | the lock-free rename design: shell-stacking + call_rcu fold + ancestor-validate loop check (the transition chain it describes was retired 2026-10-01: see its update note) |
| `simplification-s4.md` | S4: LOC + invariant-surface analysis, the 1-cacheline hot line, S3 scaling curves |
| `REVIEW.md` | retrospective: verdict, design rules, **methodology rules**, open items |
| `test_dcache.c` | single-threaded correctness + namespace-conservation harness |
| `repro_dcache.c` | deterministic 1-writer/1-walker repro of the walk-causality race (`make repro`) |
| `stress_dcache*.c` | concurrent stress: namespace, cross-dir moves, exchange, shared renamers (`stress_dcache_shared.c`: several writers rename the same objects, so one entry's folds run concurrently), directory unlink vs adds under it (`stress_dcache_rmdir.c`, with must-fail mutation arms) (ASan/TSAN arms) |
| `bench_dcache.c` | path-lookup bench: role-split readers/writers, `--op-mix`, conservation-gated |
| `bench_dcache_churn.c` | add/unlink churn + `readdir`; `--readdir-names` builds a real per-dirent `qstr`; `--share K --share-stride S` states which writers share a directory, and each run prints how its directories split into private / same-node / cross-node |
| `bench_dcache_height.c` | directory-op bench at a chosen subtree height (`--op`) |
| `Makefile` | `make check` builds+runs; `ENGINE=` swaps engine; `check-*` are the gates |

Build/run: `cd experiments/dcache && make check` (needs `make urcu-txn` at the
repo root once).

## Thesis

The Linux dentry cache is the hardest RCU user in the kernel, and the thing that
makes it hard is *rename*. A `d_move()` can relocate a live dentry to an
arbitrary point in the namespace tree while lockless path walks are mid-flight
through it. The kernel copes with two sequence counters (checked against Linux
v7.3 `fs/namei.c` / `fs/dcache.c`):

- a per-dentry `seqcount_spinlock_t d_seq`, validated **hand-over-hand** by the
  RCU walk: `__d_lookup_rcu()` samples the child's before its name/parent
  compare, `lookup_fast()` re-checks the parent's once the child is found, and
  `step_into()` re-checks the child's before stepping into it.  That is the
  fast path's only per-hop validation; and
- a system-wide `seqlock_t rename_lock`, bumped by every `d_move()`.  The fast
  path does **not** validate it on a hit (`lookup_fast`: *"Rename seqlock is not
  required here"*; unchanged since rcu-walk landed in 2.6.38): `path_init()`
  only samples it, which waits out a `d_move` in flight, and a *miss* falls back
  to `d_lookup()`, which retries while it moved.  The kernel brackets a whole
  walk on `rename_lock` only for its **reverse** walks — `d_path` /
  `dentry_path_raw`, `d_walk`, `is_subdir` — with the bounded
  `read_seqbegin_or_lock()` (lockless, then a retry holding the lock).

So a kernel lookup guarantees that consecutive components overlapped in time,
not that the whole path existed at one instant; that **snapshot** guarantee is
what the reverse walks pay `rename_lock` for.  ⚠ An earlier version of this
note, and of the `dcache_seqlock` baseline, had every RCU walk bracket on
`rename_lock` and retry on *any* rename *anywhere*.  The kernel does not do
that; every rename-concurrent reader ratio measured against that baseline
(including the "~25×" headline) is void.  The figures were re-swept on
2026-09-30 against the kernel-faithful baseline — [Results](#results-re-swept-2026-10-01)
— and `scripts/check_dcache_figures.sh` verifies each one carries data from the
current sources.

Both are **global-or-per-object sequence counters** the kernel keeps. This
experiment asks whether an urcu-txn (rcu-mcas) formulation — where a rename is a
single multi-slot commit and a lookup validates only *the slots it actually
consumed* (the engine's "help iff the slot is in the txn's own read/write set"
read policy) — can:

1. **Simplify** — delete `rename_lock`, `d_seq`, and the RCU-walk→ref-walk
   `unlazy_walk()` fallback machinery, replacing them with local slot
   validation; and
2. **Scale** — remove the global rename serialization so both concurrent walks
   *and* concurrent renames stop contending on one counter/lock.

We measure both: an LOC / invariant-surface diff for (1), a rename-fraction ×
core-count sweep for (2), gated by a namespace-conservation invariant so a
corrupted run can't masquerade as a fast one (same discipline as
`bench_txn_3skiplist`'s conservation check).

### What the txn win is — reader *and* writer

An earlier draft of this note argued the txn win was reader-only, because the
kernel *already* serializes cross-directory renames on a per-superblock
`s_vfs_rename_mutex` (to prevent directory loops), so no engine could scale
cross-dir renames. **That turned out to be wrong**: the loop check can be made
lock-free by folding the `is_subdir` ancestor walk into the rename commit's
MCAS validate set (a concurrent reparent of any target-ancestor aborts the
commit and forces a re-check). See `rename-shell-transition.md`. So the txn
engine takes **no rename lock at all** and wins on *both* axes:

- **Reader side.** The kernel's lookup validates per-dentry `d_seq`s
  hand-over-hand and samples the global `rename_lock` once per walk (waiting
  out a `d_move` in flight); every rename writes that line.  The txn reader
  does an inline name compare, touches no `d_seq`, and — on the per-node and
  mark arms — gives every lookup the whole-path snapshot the kernel reserves
  for its reverse walks.
- **Writer side.** The seqlock engine serializes *all* renames on the
  `rename_lock` seqlock (plus an `s_vfs_rename_mutex`-analog for cross-dir loop
  safety). The txn engine's renames are lock-free — one MCAS to stack a shell,
  compressed by per-node `call_rcu` fold workers — so disjoint renames proceed
  concurrently.

The comparison is therefore **kernel-scheme (serialized renames + `d_seq`) vs
fully-lock-free txn**. Headline axis stays rename fraction: as it rises, the
seqlock engine degrades on *both* the reader path (global-retry storms) and the
writer path (serialized renames), while the txn engine stays local and
lock-free on both.

## Results (re-swept 2026-10-02)

Every `figures/dcache_*.png` comes from one sweep of the current code
(provenance id in each CSV row's `src`; `scripts/check_dcache_figures.sh`
checks it).  Ratios are an engine's throughput ÷ the seqlock baseline's at the
same point; every comparison figure draws that ratio for every engine in a
strip under its panel.

**How the methodology changed, because each change moved a conclusion:**
- the baseline's lookup is the kernel's RCU walk (hand-over-hand `d_seq`,
  `rename_lock` only at `path_init` and on a miss); the whole-walk bracket is a
  separate `seqlock-snapshot` arm;
- **writers are paced** wherever readers are compared (`--rename-rate`,
  `--churn-rate`; 12.5k ops/s per writer): flat out, each engine's readers
  faced its own writers' rate, which differed by up to 260×;
- the harness PRNG no longer correlates consecutive draws (plain xorshift64 +
  `%` made each name visit 4 of 128 directories at 128 threads);
- the positive-hit and reverse-walk readers target only the writers' objects:
  they used to own leaves nothing moves, so only 8/(readers+8) of their targets
  were moving (80% at 2 readers, 4% at 184), and the reader axis of those
  panels confounded the two;
- churn has a kernel-faithful **in-place** mode (`d_delete` to a negative +
  `d_instantiate`, no allocation, no LRU traffic) beside the allocating one;
- the baseline's per-directory lock is the vendored kernel `rw_semaphore`;
- the locks are the kernel's: the bucket bit locks test-and-test-and-set like
  `bit_spin_lock` and release with a plain store like `__bit_spin_unlock`
  (an atomic RMW until 2026-10-02), and every engine's LRU shard lock — the
  kernel's `list_lru_one.lock`, a `spinlock_t` — is a port of the queued
  spinlock (`dcache_qspinlock.h`).  It was test-and-test-and-set until
  2026-10-02, which COLLAPSED as waiters grew: one shard ran 15.2 M
  allocating toggles/s with 4 writers and 7.1 with 8.  The queued lock holds
  ~11.5 from 4 to 8, but it has one cost the kernel's does not, below;
- churn's directory sharing is a stated axis (`bench_dcache_churn --share`,
  column `dirs`), checked against the bench's own classification of every
  directory from the pinned CPUs' NUMA nodes.  The old `--ndirs 16×writers`
  paired writer i with writer i+W/2 — on one node up to 8 writers, on two
  from 16 — and cross-node sharing alone moved the allocating ratios ~6%;
- the setup's memory lives on the benchmark's first node: a preferred-node
  memory policy set before a re-exec (the allocator's first chunks are faulted
  in before `main()`), reset before the workers start.  Left to the scheduler,
  the seeded namespace landed on either socket from one session to the next.
  (Not by pinning the CPU instead: jemalloc sizes its arenas from the affinity
  mask it starts with, and one CPU meant one arena for 200 threads — a sweep
  run that way halved the bucket lock's writers and was discarded.);
- every run warms up first: the workers run their real, paced workload for
  200 ms (`--warmup`) before the timed window opens and restart their
  throughput counters when it does, so no window starts on cold caches,
  first-touch faults and an empty grace-period/fold pipeline.  The transient was
  small (every engine within ~3% between 0.25 s and 4 s windows) except in
  flat-out runs at high rename fractions.

**Changed in this sweep (2026-10-02).**  Besides the two locks above: the
seqlock baseline's LRU and unlink had races the kernel's `d_lock`, victim
`i_rwsem` and `S_DEAD` close — a walk could re-arm a dentry being killed, an
add could link a child under a directory being unlinked (6726 children lost
under `stress_dcache_rmdir`, which had never run on seqlock) — fixed with an
LRU membership state word, the victim directory lock and an `IS_DEADDIR`
check, at 1–8% of its allocating churn and gated by `make check-rmdir`; the
bucket lock enqueues a new dentry on the LRU before publishing it, under the
locks it publishes with; and liburcu (`c21f5a38`) retires txn descriptors in
batches by default.

**Changed in the previous sweep (2026-10-01): rename shells no longer chain.**  An
entry is now its host plus at most one shell naming it (the host's `d_top`); a
shell's fold either hands the name back to the host or, if a later rename
demoted it or an unlink removed it, just frees it
([`rename-shell-transition.md`](rename-shell-transition.md), update note).  The
chain it replaced had a use-after-free under shared renamers and made the
reverse walk climb one shell per unfolded rename.  Against the previous engines
on the same harness: reverse walks 1.66–2.02× faster at 100k renames/s;
flat-out renames up to 1.89× (per-node) and 1.62× (mark) at high rename
fractions and up to 2.10× on leaf exchanges; lookups, readdir and paced height
unchanged (medians within 2%).  The one cost: the bucket lock's unlink now also
locks the victim directory's own child list, closing a race where an add could
land under a directory being unlinked (the kernel's `vfs_rmdir` locks the
victim too) — ~5% of empty-directory churn against 32–64 concurrent listers,
~1% without.

**And (2026-10-01) the bucket lock's hash-chain decode tests instead of masking.**  Every
link of a bucket walk used to clear the lock and mark bits with two ANDs before
the next load went through the pointer; it now tests them (they are almost
never set) and uses the word as loaded, with an empty asm keeping GCC from
folding the test back into a mask (`DC_OPAQUE` in `dcache_bucketlock.c`).  Its
readers gained 1.03–1.09× on lookups and 1.19× on reverse walks, both at rest
and under renames; writers unchanged.  The same test on `d_iparent`, whose tag
bits are data, lost 11–15% under renames and was not kept.

**Readers** (184 readers, 8 writers, `dcache_s3.png`): at a realistic 10k
renames/s every arm is at parity (1.00–1.03×).  The localized arms (per-node,
mark, bucket lock) pull ahead as the rename rate climbs — 1.16–1.20× at 100k/s,
1.72–1.83× at 300k/s — and seqlock's writers cannot carry 1M/s at all.  At a
fixed 100k/s the localized arms lead 1.02–1.20× from 8 readers up.

Where seqlock still LEADS is where every lookup lands on an object being
renamed (`dcache_hit.png`): up to 32 readers txn-global is at parity
(0.96–0.99×), txn-mark 0.88–0.93×, per-node 0.83–0.90× and the bucket lock
0.84–0.89×.  A
rename shell that has not folded yet turns a hit into three cachelines instead
of one (the shell, its host pointer, the host), and shells live ~5 ms (the
`call_rcu` worker's batching; the grace period itself is ~20 µs), so at 100k
renames/s over 256 objects 80–90% of these hits land on one.  That is an
L1-capacity cost each reader pays privately; seqlock's in-place rename costs
every reader a coherence miss instead, cheap with few readers and growing with
them.  Per-node and the bucket lock also pay 10–15% at rest (per-hop generation
sampling; per-hop pointer decoding).  Above 32 readers the arms sit at
0.86–1.04×.  At 128–184 readers the bucket lock sits at 0.77–0.86× (its writers
fall short of the offered rate at 160): readers holding a stale path miss on
the name a rename just vacated and all cache a negative at once, on the bucket
and child-list locks its renamers need (0.95× without negative caching).  It
was 0.88–0.91× in the previous sweep, before the bit locks' release became a
plain store; this region is noisy and the cause is not established.  With the
old test-and-set spin the herd cost it 0.67–0.75×.

The reverse walk (`dcache_dpath.png`): every txn arm now leads at every reader
count — 1.12–1.46× at 2 readers, 1.65–2.14× at 16, 5.3–7.2× at 32–64 (from 48
readers seqlock's reverse walks starve its own renamers).  Until the chain was
retired seqlock led at 2–8 readers (0.63–0.75×): a txn reverse walk climbed one
shell per unfolded rename of the object, 2–3 of them at 100k renames/s.
readdir: txn 1.08–1.27× at 2–8 readers, 1.82–1.89× at 16 and 2.25–2.35× at 32;
beyond that seqlock's renamers cannot keep up (`dcache_readdir.png`).
Directory exchanges at every height: lookups within 0.90–1.14× at 100k/s
(`dcache_height.png`).

**At rest** (`dcache_idle.png`: the same four reader panels with the writers
idle, same namespace and placement, no rename in the timed window).  Probing
lookups: every engine within 0.96–1.09× of seqlock, parity from 32 readers.
Positive hits: txn-global 1.08–1.10×, txn-mark 0.97–1.00×, per-node 0.90–0.94×,
the bucket lock 0.90–0.95× — so under renames txn-mark's hit gap is the shell
(the trade above), while per-node pays its generation sampling at rest too.
Most of the bucket lock's at-rest gap (it was 0.83–0.88×) was its hash-chain
decode masking every link's pointer before the next load; it tests the bits
instead since 2026-10-01 (above).  Reverse walk: every
txn arm leads at every reader count, 1.12–1.56×, with nothing to defend
against.  readdir: txn 1.24–1.26× at 2 readers, 2.1× at 16, 3.7–5.0× at
128–184 — seqlock's readdir stops scaling at ~420–440 Mreaddir/s from 96 readers
with no writer at all: its readers RMW the directory's rwsem count (as the
kernel's `iterate_shared` takes `i_rwsem` shared), so the readdir wall of the
loaded panel is the rwsem's read side, not the renames.

**Interconnect sensitivity** — a result in its own right.  With the seeded
namespace homed on the far socket (16 readers + 8 writers, all on socket 0),
seqlock's readdir drops 93 → 65 Mreaddir/s and its reverse walk ~375 → ~270
Mdpaths/s (−30–40%); the txn arms move 2–5%.  seqlock's readers RMW lines (a
directory's rwsem) or re-read `rename_lock`, so every transfer pays the trip to
the line's home node; the txn readers only read shared lines.  Where lookups
miss to DRAM (the probing panels) the home costs every engine alike.  Spreading
the readers one per CCD for more L3 does the opposite of helping: seqlock −35%
(readdir) and −55% (reverse walk), txn −13–16% — they share a small set of hot
lines, and spreading turns same-CCD transfers into cross-CCD ones.  The sweeps
fix the home on the benchmark's own node, seqlock's favourable case.

**Writers**: flat out, the bucket lock renames 4.1–21.5× the seqlock baseline
(6.3–21.5× on leaf ops, 4.1–4.5× on directory ops) and txn-mark 2.6–5.5×
(`dcache_optaxonomy.png`; exchanges at height 3.2–5.1× and 1.7–2.8×);
txn-pernode reaches 20.1× on leaf exchanges.
Part of that gap is the per-directory rwsem and cross-directory rename mutex the
baseline takes (the kernel's `i_rwsem`, `s_vfs_rename_mutex`) and the txn
designs do not need: their readdir is lock-free, so no writer has a reader to
exclude — a legitimate improvement axis, not an accounting artifact.

The queued shard lock's one cost the kernel does not pay: where many threads
rename across NUMA nodes — only the homogeneous-mix panel of `dcache_s3.png`
does that, 48 threads on 6 nodes — the txn and bucket-lock engines' renames
run at 0.86–0.93× of a test-and-test-and-set lock's rate at a 10% rename
fraction and 0.66–0.75× at 50% (2026-10-02 A/B on one build).  Part of it is a
harness convoy: those engines fold shells in `call_rcu` callbacks, which
delist them, on a worker co-pinned with its writer, so the worker can preempt
its writer while the writer is queued; the kernel takes a lock that RCU
callbacks also take with bottom halves disabled, which rules this out.  With
the workers on the writers' SMT siblings the gap at a 20% fraction closes from
0.75× to 0.97× (per-node) and from 0.73× to 0.82× (bucket lock); the rest is
FIFO handoffs crossing nodes.  Every other rename panel runs 8 writers on one
node and is within 5% of the old lock.

**Create/delete** (`dcache_churn.png`, private directories): in place — the
kernel's path for a name removed and created again — txn-global/per-node
1.24–1.29×, bucket lock 1.14–1.23×, txn-mark 0.93–0.99× the baseline, flat
across 1–48 writers; on 2026-09-30 the rwsem was 74–79% of the bucket lock's
lead at 16–48 writers.  Allocating, the bucket lock is at parity (0.99–1.03×
from 1 to 48 writers, 0.98–1.04× to 192 in `dcache_churn_scaling.png`) and
the MW txn engines trail at 0.65–0.69× with 1–2 writers and 0.93–0.97× from 8
up: a descriptor per commit, which batch retirement now cuts at every writer
count (1.09–1.39× over one `call_rcu` per descriptor, `dcache_slabroute.png`).
The "allocating churn is LRU-bound" of the previous sweeps was largely the
test-and-test-and-set shard lock collapsing, and its bucket-lock gap (0.91×)
was cross-node directory sharing.  Readers under churn: 0.91–1.07× in both
modes.

**Directory sharing** (`dcache_churn_share.png`, `dcache_churn_scaling.png`):
a pair of writers sharing directories on ONE node costs every engine little
(the bucket lock 0.98–1.09× the baseline); on TWO nodes it costs everyone,
seqlock most when toggling in place (138 → 94 M toggles/s at 16 writers), where
txn-global then leads 1.84× and the bucket lock 1.65–1.69×; allocating, the
bucket lock goes from 0.95× at 16 writers to 1.02–1.04× from 64 up.  When every writer shares
one set of directories, seqlock's create/delete stops scaling (15 M toggles/s
at 48 writers, 9.7 at 192) and txn-global and the bucket lock run 2.4–3.4× and
2.4–2.9× from 48 writers up.

**Controls**: the matched-name-width control sits at 0.98–1.02× of the shipped
arm (median of 7 runs, `dcache_namewidth.png`).

Before the setup placement was fixed, seqlock's churn throughput swung ~25%
between sessions.  Under the fixed placement it is stable: across three full
sweeps (2026-10-01) its writer-only churn varied by at most 2.5% per point
(median 0.6%).

## What we actually port (the RCU-relevant core)

The dcache is enormous; most of it is orthogonal to the rename/RCU question. We
carve out the part that *is* the question:

| Ported | Kernel counterpart |
|---|---|
| dentry: parent ptr, name (`qstr`), children/sibling links, hash-bucket link | `struct dentry` (`d_parent`, `d_name`, `d_children`/`d_sib`, `d_hash`) |
| the `(parent, name) → dentry` hash table + lockless lookup | `dentry_hashtable` (`hlist_bl`), `__d_lookup_rcu` |
| insert / unlink | `d_add` / `__d_add`, `d_delete` / `__d_drop` |
| **rename, incl. exchange** | `d_move` / `__d_move` (`RENAME_EXCHANGE`) |
| multi-component path walk | `link_path_walk` → `walk_component` → `lookup_fast` |

**Deliberately out of scope** (stubbed or omitted — none change the rename/RCU
story, all add bulk): LRU + shrinker, negative-dentry lifecycle, mounts/
`d_splice_alias`, inode alias/hardlink management, external-name refcounting,
`lockref` cmpxchg refcounting (we use a plain atomic refcount), security/audit
hooks, case-folding.

Why multi-component walk is *in* scope: a single-component lookup can't be
misdirected by a rename — the cross-tree hazard only appears when a walk holds a
dentry from step *k* and dereferences its child at step *k+1* after that dentry
has been moved. That is the exact race `rename_lock` exists to catch, so the
harness must walk paths of depth > 1 or it isn't testing anything.

## Two implementations, one interface

Both satisfy the same `dcache.h` API (`dc_lookup_path`, `dc_add`, `dc_unlink`,
`dc_rename`, `dc_rename_exchange`), so the harness is engine-agnostic:

- **`dcache_seqlock`** — faithful kernel-style port: `hlist_bl`-equivalent
  buckets, global `rename_lock` seqcount, per-dentry `d_seq`, the kernel's
  RCU-walk (hand-over-hand `d_seq`, `rename_lock` sampled at walk start and
  consulted only on a miss).  This is the **baseline we are trying to beat and
  simplify** — the honest comparison is "can txn beat the kernel's own scheme,"
  not "can txn beat a coarse mutex."  Two lookup arms, because the txn arms
  give a stronger guarantee than the kernel's lookup: the default is the
  kernel's fast path (pair it with txn per-node / mark, and say the txn arms
  give more); `-DDC_SEQ_SNAPSHOT` brackets the whole forward walk on
  `rename_lock` with the kernel's `read_seqbegin_or_lock` (same guarantee as
  every txn arm; pair it with txn-global).  `dc_dentry_path()` is the reverse
  walk: a line-for-line `dentry_path_raw()` port here, and the same
  snapshot guarantee built from each txn arm's own causality mechanism there.
  Parity is of guarantee, not of cost: the seqlock arms bump `rename_lock` on
  every rename, file or directory, as the kernel does, while txn-global bumps
  `rename_gen` only for directory moves -- so with file leaves the snapshot
  arm pays for renames txn-global does not.
- **`dcache_txn`** — **lock-free** urcu-txn port (full design:
  `rename-shell-transition.md`). Names stay **inline** on the dentry (kernel
  `d_iname` locality); the reader is a plain RCU walk with an inline name compare
  and no `d_seq`. A rename keeps the dentry's address (children never rehash) by
  **stacking a transient named "shell"** — one MCAS — that forwards to the
  content host; per-node `call_rcu` fold workers compress the chain back to a
  single node, doing the one in-place name write inside a grace-period window
  where no reader can see it. Cross-dir loop safety folds the `is_subdir` walk
  into the commit's validate set (no rename lock).

An optional coarse **`dcache_rwlock`** (one rwlock over the whole cache) can
anchor the low end of the scaling plot — it makes the seqlock port's cleverness
legible, but it isn't the point.

## Benchmark design (`bench_dcache`)

Modeled on `bench_txn_3skiplist` / the `scripts/run_*.sh` + `plot_*.py` +
`figures/` flow:

- Build a synthetic namespace tree (fixed fan-out × depth) and warm every engine
  uniformly (warm-vs-warm — see the "prime all engines" project rule).
- **Negative dentries (2026-09-29).**  A reader asks for a leaf name in a
  *random* directory and each leaf lives in one, so ~99% of lookups name
  something absent.  Until this date the bench served those as MISSES on every
  engine — the S3 reader panels measured the miss path.  A kernel serves the
  first one through `lookup_slow`, which caches a negative dentry, and every
  later one as a negative HIT on the RCU fast path.  The bench now does the
  same: a priming phase looks up every (dir, name) a reader can ask for and
  caches a negative on each miss, a reader that misses during the run caches
  one, and a rename onto a negative replaces it (`d_move`).  Each run prints
  its positive / negative / absent mix; `--no-negatives` restores the legacy
  workload.
- `--nthreads` workers each running a mix: `--rename-frac` of ops are renames
  (disjoint subtree moves + a slice of `RENAME_EXCHANGE`), the rest are
  full-path lookups of depth `--depth`.
- **Headline independent variable: rename fraction.** The seqlock baseline's
  writers serialize on `rename_lock`; its readers retry only when a rename
  touches a component they use (default arm) — or any rename at all, bounded to
  one locked retry (snapshot arm) — and pay one read of the `rename_lock` line
  per walk.  Secondary axis: core count at a fixed rename fraction.  The
  reverse walk (`--dpath`) is the like-for-like reader: the one operation the
  kernel itself serves with a whole-path snapshot.
- Metric: lookup throughput **and rename throughput** (Mops/s), plus walk-retry
  rate and rename-serialization for the seqlock engine (the mechanism behind any
  gap). Rename throughput is now a co-headline, not a footnote — the txn engine's
  lock-free renames are half the story.
- **Invariant gate:** every run ends by verifying namespace conservation — the
  set of full paths reachable from the root equals the set implied by the
  recorded rename log. Failure prints `CONSERVATION FAILED` and exits nonzero.

## Correctness bar (what must be *shown*, not asserted)

The txn port only earns the simplification claim if it demonstrably handles the
races `rename_lock`/`d_seq` were built for:

1. **Cross-tree misdirection** — a walk holding dentry `X` at depth *k* whose
   child edge is consumed after `X` was moved elsewhere must not silently walk
   the wrong subtree; its slot validation must abort/retry or observe a
   consistent snapshot.
2. **Rename loop / A-into-B-into-A** — `RENAME_EXCHANGE` and ancestor-descendant
   moves must not livelock a concurrent walk or produce a cycle.
3. **d_seq's job** — name / parent / bucket-membership must be mutually coherent
   at the point of match, from slot validation alone.

Plan: reproduce each deterministically under a single writer + single walker
(as `bench_txn_3skiplist` did for the torn-tower bug), *then* run under the
contended sweep. A race we can't trigger on demand we don't claim to have fixed.

## Staging

- **S0** (this session) — directory + this plan + open-decision sign-off.
- **S1** — `dcache.h` interface; `dcache_seqlock` baseline; single-threaded path
  walk + correctness harness.
- **S2** — `dcache_txn` behind the same interface; the 3 correctness
  reproductions above.
- **S3** ✅ — concurrent `bench_dcache` (homogeneous rename-fraction mix **and** a
  role-split reader-vs-writer mode), 3-arm sweep + conservation gate;
  `scripts/run_dcache.sh` + `plot_dcache.py` → `figures/dcache_s3.png`. **Finding:
  the txn port deletes `d_seq` (real simplification) but the DEFAULT reader still
  brackets one *global* `rename_gen`, so on the reader path it ties the seqlock
  baseline — the scaling win needs the per-node arm.** Landed a third arm,
  `dcache_txn` under `-DDC_PER_NODE_GEN`: a per-content-host generation bumped only
  by the moved entry, validated by a versioned descent+up-pass double-collect
  (sample host gen → confirm the matched top is still indexed via the O(1)
  hlist-delete MARK → revalidate all path hosts on the way up). Same correctness
  bar (103/103, walk-causality repro, ASan/TSAN-clean stress: `make check-pernode`).
  In the homogeneous mix the collapse is *writer*-bound (a rename ≈ 50× a lookup),
  so the reader-gen difference is masked; the role-split isolates it and the
  per-node arm runs materially faster than global on the contended reader path.
  Sweeping readers to 184 (8 writers, filling all 192 cores one-thread-per-core via
  an hwloc-derived pin list), per-node **keeps scaling to ~450 Mops/s** while the
  global bracket saturates ~110–120 and seqlock never scales cleanly — **3.7×
  global, 5.8× seqlock at the full-machine point** (see `rename-shell-transition.md`
  S3 results).
- **S3-readdir** ✅ — directory-listing companion (`bench_dcache --readdir`,
  panels `readdir_scale`/`readdir_w` → `figures/dcache_readdir.png`). The seqlock
  baseline was upgraded to an honest **per-directory rwsem** (the kernel inode-rwsem
  analogue, not one global lock). **Finding: listing is the *easy* case — the txn
  `readdir` reads no generation counter at all, so `txn-global` ≡ `txn-pernode`
  (they overlap), and it dissolves to a bare lock-free RCU child-hlist walk.** It
  **scales to ~355 listings/s at 160 readers (~12× the rwsem, which saturates ~15–29**
  — its read-side is a shared cacheline), and it **leads at every reader count**.
  A **write-once `d_host` skip pointer overlaid on `d_id`** (`host_of_rcu`) resolves
  the content host in O(1) regardless of chain depth, so the walk is no longer
  churn-sensitive: under a saturating rename load it **stays ~2× above** the rwsem
  (48 writers: ~27–35 vs ~14) instead of dipping below. Both engines
  conservation-clean + ASan-clean.
- **S4** ✅ — simplification analysis (LOC + invariant surface: which
  counters/fallbacks disappear) + scaling figures, in
  [`simplification-s4.md`](simplification-s4.md). Also produced the 1-cacheline
  reader hot line (and the lesson that a "1-CL" claim is about the *object*, so
  it must be enforced at allocation, not inferred from field offsets).
  [`REVIEW.md`](REVIEW.md) closes the experiment out.
- **S5** ✅ — **the allocator underneath**. The engines allocate one transaction
  descriptor per mutation, so above a few writers this experiment stops
  measuring the dcache and starts measuring liburcu's descriptor slab. Three
  defects came out of chasing that, and the order matters — each was hidden by
  the one before it:

  1. **The harness stalled every grace period.** `bench_dcache_churn` left the
     main thread RCU-*online* in `nanosleep()` for the whole measured window,
     and one stuck online QSBR thread blocks all reclaim: no `call_rcu` callback
     ran, so no freed descriptor returned, so every allocation carved a fresh
     block. It read as an allocator leak — reuse ~11%, footprint growing
     linearly to 48.7 GiB at 8 s. Every churn number this harness had ever
     produced was measured that way.
  2. **The reclaim worker shared a cpu with its writer.** `URCU_CALL_RCU_RT`
     does not sleep, so co-pinning it with a writer that never yields halves
     reclaim. `DC_CRDP_CPU_OFFSET` moves it; the SMT sibling wins (75% reuse vs
     62% co-pinned), and a free core on the *other socket* is far worse (33%) —
     cache locality with the writer beats dedicated execution resources.
  3. **The slab's batch retirement had no in-tree user.** liburcu carried
     `urcu_slab_free_pending()` — retire a whole batch with one `call_rcu`
     instead of one per descriptor — and both engines still freed one at a time.
     Wiring it up (`URCU_TXN_SLAB_BATCH`) is worth **3.0–3.5×** on churn at 48
     writers *and* bounds the footprint at 194 MiB instead of growing without
     limit.

  Two liburcu bugs fell out of wiring it: a per-call store to a process-wide
  word that cost ~55% of cycles (and made batching look 2× slower than what it
  replaces), and a plain/atomic race on the arena floor that failed all six
  TSAN gates. Both fixed upstream.

  **The grace period turned out to be the batching clock, not the threshold.**
  The slab closes a batch either at `batch_max` blocks *or* on a closer armed at
  the first deferred free and re-armed only once the previous has run — and the
  second is the mechanism. Closing early cannot make a block reusable sooner
  (it still owes a grace period from the close), so it buys nothing and costs a
  `call_rcu`. Counted at 48 writers, the GP-clocked half is identical either
  way — 37941 vs 37921 closes — and the threshold merely interleaves 19151 more
  on top, +20% `call_rcu` traffic for the same reclaim latency, footprint and
  reuse. The default is now out of reach (`ULONG_MAX`), which measures the same
  as any sufficiently high value: the effect saturates.

  Sweep: four descriptor-slab arms (`scripts/dcache_*{,_rseq,_batch,_batch_rseq}.csv`,
  `figures/dcache_slabroute.png`). **rseq per-cpu local lists are not merely a
  null on the default route — they cost**, and the cost grows with contention
  (1.04 at one writer to 0.88 at 48, against a control band flat at 0.98–1.01):
  the fast path needs a same-cpu free, reclaim runs elsewhere, so every op pays
  the rseq checks and takes the atomic path anyway. Under batching, where the
  committing writer frees on its own cpu, it turns positive — ~3%, on the churn
  workload only.

  The standing lesson: a footprint cap is a **memory safety valve**, not a
  tuning knob. Raising it does buy throughput, by letting the leak run further;
  fixing the recycling buys more, with the memory bounded.

## Locked decisions (2026-07-15)

1. **Port scope** — **RCU-core-only** (the table above), with the two bolt-on
   seams designed into the interface from S1: (a) `dc_lookup` returns tri-state
   *positive / negative / absent*, and (b) unlink does honest RCU-deferred
   reclaim (real `call_rcu` free), so phase 2/3 reuse the same reclaim path
   instead of forcing a redesign. Phasing:
   - **Phase 1** (now): RCU-core — tree + hash + rename/exchange + walk.
   - **Phase 2** ✅: negative dentries + `d_instantiate`.  It DOES touch the
     rename mechanism, contrary to the note this line used to carry: inode-ness
     is authoritative on the content host, so the fold's TRANSFER preserves it
     rather than adopting the top's.  See REVIEW.md section 6 -- it is where the
     "`d_seq` dissolves" claim gets priced, because the baseline brackets the
     transition in a seqcount it still has and the txn engines must publish it
     through a commit instead.
   - **Phase 3**: LRU + shrinker (makes lifetime/refcount first-class; the
     shrinker is "just another unlink mutator" over the S1 reclaim primitive).
2. **Baseline** — **faithful `rename_lock` seqcount + per-dentry `d_seq` port**.
   The honest "can txn beat the kernel's actual design" comparison. A coarse
   rwlock engine may be added later only to anchor the low end of the plot.
   The kernel's lookup does not validate `rename_lock` on a hit; the snapshot
   arm (`-DDC_SEQ_SNAPSHOT`) is the guarantee-matched partner, not the default.
3. **Headline axis** — **rename fraction** at fixed cores, with core-count as
   the secondary axis, and the reverse walk (`--dpath`) as the reader whose
   kernel guarantee matches the txn engines'.

## References

- Design: [`rename-shell-transition.md`](rename-shell-transition.md) — the
  lock-free rename mechanism (shell-stacking, `call_rcu` fold, ancestor-validate
  loop check) the `dcache_txn` engine implements.
- Kernel: `fs/dcache.c` (`__d_lookup_rcu`, `__d_move`, `d_move`, `__d_add`,
  `dentry_hashtable`), `fs/namei.c` (`link_path_walk`, `walk_component`,
  `lookup_fast`, `unlazy_walk`), `include/linux/dcache.h`.
- Engine: `urcu-txn-build/include/urcu/rcu-txn.h`, `.../rcu-txn-hlist.h`,
  `.../rcu-mcas.h`; the read-policy rule and RYW/chaining idioms in
  `design/rcu-txn-*.md` and the `rcu-pseudo-transaction` skill.
