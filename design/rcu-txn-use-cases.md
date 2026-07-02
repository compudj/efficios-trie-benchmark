# urcu-txn: candidate data structures and use cases

An analysis of which core data structures — in the kernel, low-level libraries,
databases, networking, and other fields — could be improved by the urcu-txn API
(`<urcu/rcu-txn.h>` / `<urcu/rcu-mcas.h>` and the single-writer
`<urcu/rcu-txn-sw.h>`). Grounded in the API contract and the measured results in
this repository's [README](../README.md) (`bench_list_scale`: bidirectional list,
composable index, hash-of-lists, RLU comparison, LWN #667720 reproduction,
write-contention study). 2026-07-02.

## What urcu-txn uniquely offers

Five properties decide fit, and each maps to a different class of candidate
structure:

- **A. Plain-RCU reads over multi-word-coherent structures.** Readers pay
  zero — no per-node validation (RLU), no retry (seqlock), no descriptor
  resolution cost in the common settled case — yet `next`/`prev`-style edge
  pairs never disagree. Measured: bidir reads match forward-only `rculist`,
  1.6–1.8× RLU on a real (10k-node) working set.
- **B. Cross-structure atomic composition.** One commit is one linearization
  point across *every* structure folded into it (the `_prepare` forms).
  Measured: the composable index+list path scales to ~125 Mops/s at
  192 writers with the descriptor slab.
- **C. Writers that actually scale.** The only engine in the study whose write
  throughput rises with writer count (104–185 Mops/s at 192), where per-bucket
  locks, RLU's write-clock, and seqlock serialize.
- **D. Contention robustness.** Graceful degradation to a single hot slot
  (44 → 7.5 Mops/s vs `rcu_hlist`'s 52 → 0.9, a ~55× collapse) — safe under
  skew and adversarial load, not just uniform benchmarks.
- **E. A pay-per-use consistency dial.** Three read tiers (next section): the
  default read path stays free, and snapshot consistency is priced
  per-operation, only on the slots that need it. Seqlock taxes every reader
  with retry exposure, RLU taxes every dereference with clock validation,
  locks tax everyone; urcu-txn taxes only the reads that ask for more.

Plus the ABA tolerance from the per-record install latch, which is what makes
back-edges and value-recurring slots legal at all — most classic MCAS
deployments had to design around exactly that.

The sweet spot: **read-dominated structures that today pair RCU reads with a
serializing writer lock, structures that can't be RCU'd at all because an
update must flip several words coherently, and objects with membership in
several structures at once.**

## Read-consistency tiers

The API offers three strength levels; matching a candidate to the right tier is
most of the design work.

1. **Plain RCU read** — per-slot linearizable, zero cost, no snapshot. A long
   traversal may straddle a commit; no slot is ever torn.
2. **Guarded read** (`urcu_txn_load_validate()` on each slot, then commit) — a
   true atomic snapshot: commit OK certifies that every guarded slot
   simultaneously resolved to its recorded value at the single status-flip
   linearization point. For a pure guard the record is `{v → v}`, so the
   commit changes nothing observable and acts purely as a validation
   instrument. Value-CAS semantics are exactly sufficient for snapshot
   *existence*: even if a slot toggled away and back between the read and the
   commit, "these values coexisted at the flip instant" still holds. On ABORT
   the reader retries like a seqlock reader — but with the engine's progress
   machinery behind it (`urcu_txn_conflict()` ages a guard storm;
   `URCU_TXN_FALLBACK` escalation bounds starvation), so unlike a seqlock
   reader it cannot be livelocked indefinitely (the README shows seqlock
   readers going to ~0 under a steady writer).
3. **Stability-throughout / opacity** ("nothing changed while I worked",
   detecting an A-B-A excursion where the excursion matters) — the only tier
   that needs embedder versioning layered on top.

**What the snapshot tier costs.** A guarded read is a real MCAS commit:
descriptor (slab-served), proxy install + settle on each guarded slot (~two
CASes per slot), participation in the priority/steal protocol, and `call_rcu`
deferral of the descriptor. Three consequences bound its use:

- **It is a writer to the coherence protocol.** Guarded slots' cachelines get
  dirtied, and two overlapping guard-only readers contend with each other
  (each must evict/help the other's parked proxy) even when nothing is
  semantically changing. Right tool for targeted, small-*k* consistency, not a
  default read path on hot shared words.
- **Footprint scaling.** O(k) CASes and abort probability rising with
  footprint means whole-structure coherent iteration stays out of reach — that
  corner belongs to clock/version schemes (RLU's one genuine remaining edge)
  or to a stop-the-world bulk regime for batch workloads.
- **Coverage is transacted words only.** A guard certifies a slot whose every
  writer goes through the engine (bit-0 contract). A counter bumped with a raw
  `uatomic_add` cannot be guarded — retrofitting snapshot reads onto a block
  means routing its writers through txn stores too.

## Linux kernel

(Design transfers, not library uses — the RCU-MCAS scheme with install latch +
priority steal + escalation lane would need an in-kernel port, but every
ingredient — `call_rcu`, per-CPU slabs, priority protocols — is native there.)

- **`tasklist_lock` and the process tree** — the canonical candidate.
  Fork/exit/reparent must atomically splice a task into/out of
  parent/children/sibling lists plus the global task list and pid hashes; that
  multi-list atomicity is why a global rwlock survives decades of complaints.
  Capability B+C exactly; readers (procfs, signal delivery, cgroup iteration)
  become plain RCU.
- **dcache rename: `rename_lock` + `d_move()`** — a cross-directory rename
  moves a dentry between hash chains and between parents' child lists, today
  published under a global seqlock that forces RCU-walk to retry or fall back
  to ref-walk. One MCAS commit makes the move a single linearization point
  that lockless path walk simply observes before-or-after. The README's
  seqlock result (long read sections livelock under a steady writer) is
  precisely the rename_lock pathology.
- **`sb->s_inode_list_lock` / inode LRU / writeback lists** — an inode lives on
  the sb list, an LRU, and a writeback list simultaneously; eviction touches
  all of them. Multi-membership is capability B; these locks are documented
  contention points on large boxes.
- **nf_conntrack** — each flow inserts *two* hash entries (original + reply
  tuples) that must appear together; today a two-bucket lock dance with
  careful ordering. A 2-slot MCAS is the natural primitive, and capability D
  matters because conntrack sees adversarial skew (floods) by design.
- **FIB trie under RTNL** — route insert/replace touches trie structure plus
  leaf alias lists plus nexthop references; readers are per-packet and already
  RCU. RTNL decomposition is an active pain point; scalable concurrent route
  writers (BGP convergence churn) is capability C.

## Low-level userspace libraries

- **liburcu itself: online rehash for `cds_lfht`** — the strongest single
  application: split-ordered lists structurally cannot change the hash
  function, so hash-flooding pathology is unfixable in-place. MCAS's atomic
  cross-table item move makes online rehash a parallel drain — something *no*
  existing lock-free hash offers. Also: `cds_lfht` atomic multi-key ops
  (move/rename between tables) as API additions.
- **glibc dynamic loader: `dl_load_lock` and the link map** —
  `dl_iterate_phdr` serializes C++ exception unwinding across all threads;
  dlopen/dlclose must update the link-map list plus address-lookup structures
  coherently. Readers (unwinders, profilers, signal-context symbolizers) want
  exactly plain-RCU reads; writers are rare but multi-word. A known production
  scalability sore.
- **JIT runtimes** — code maps (pc → deopt/unwind metadata), inline caches,
  and class hierarchies: readers on every dispatch/exception, writers on
  compile/invalidate, and an invalidation must flip several entries
  coherently. Today mostly stop-the-world or biased locks.
- **LTTng registries** — probe/event/session structures with multi-list
  membership and RCU readers on the tracepoint fast path.

## Databases and storage engines

- **B-tree/trie node splits via composable commits** — a split touches child,
  parent, and sibling links; that's why OLC/ROWEX exist. MCAS was invented for
  this (Harris CASN; Microsoft's PMwCAS/BzTree revived it), but urcu-txn's
  differentiator is that **readers pay nothing** — PMwCAS readers still
  resolve descriptors, OLC readers validate versions. Concretely: **coherent
  leaf sibling back-links**, which B-link designs drop because they can't
  maintain `prev` safely — the bidir-list result transplanted; buys latch-free
  reverse range scans.
- **MVCC multi-index commit** — installing a new row version's entries in N
  secondary indexes as *one* linearization point. Today engines let indexes go
  transiently inconsistent and paper over it with revalidation at read time.
  Capability B removes an entire class of "index says yes, heap says no" logic
  for lockless readers. And with guarded reads, **consistent multi-index point
  reads** become on-demand — validate the heap slot plus its N index entries
  at one linearization point, no read timestamps needed for readers that only
  occasionally require cross-index consistency.
- **Buffer pool metadata** — page mapping table + LRU + free/dirty list
  membership: the hash+LRU multi-membership pattern, classically lock-juggled.
- **In-memory KV caches (memcached/redis-cluster-style)** — hash chain + LRU +
  expiry structure per item; eviction is a 3-structure atomic removal.
  Capability D matters: hot-key skew is the *normal* operating regime for
  caches, and the write-contention study shows exactly that regime is where
  txn holds up and RCU+lock doesn't.

## Networking (userspace)

- **DNS.** bind9's `dns_qpmulti` is single-writer COW transactions —
  urcu-txn's pitch is concurrent writers on shared state. More specifically:
  the **NSEC/NSEC3 chain is a sorted doubly-linked list** that must stay
  coherent for readers synthesizing negative answers while dynamic UPDATE
  churns it — the bidir-list benchmark is nearly a drop-in model of that
  problem. A DNS UPDATE touching an RRset plus its NSEC chain links plus an
  index is a natural composed transaction.
- **Software dataplanes (DPDK/VPP/FRR)** — FIB updates touch trie nodes +
  nexthop groups + ECMP arrays; DPDK's `rte_lpm` writer path is non-atomic
  multi-word with a lock, and its RCU integration is recent and partial.
  Per-packet readers with BGP-flap writer bursts is the C+D combination.
- **NAT/LB flow tables (Katran-class)** — the forward+reverse tuple pair, same
  as conntrack: a 2-slot MCAS with adversarial skew tolerance.
- **QUIC connection-ID maps** — several CIDs map to one connection;
  issue/retire must swap sets atomically while the packet path reads
  locklessly.

## Other fields

- **Dynamic/streaming graphs** — an edge insert touches two adjacency lists
  (out-edge of A, in-edge of B); coherent bidirectional edges under plain-RCU
  reads is the bidir-list result generalized. Streaming-graph systems
  currently batch deltas or take per-vertex locks precisely because of this
  two-list atomicity problem.
- **Spatial indexes (game servers, robotics, telco cell tracking)** — moving
  an object between grid cells/octree nodes = delete-from-A + insert-into-B in
  one commit, so a reader never finds it in zero or two cells. Today solved
  with epochs or coarse locks.
- **Schedulers/work orchestration** — task state machines where an item moves
  between queues (pending→running→done lists + a lookup index) atomically.
- **Stats/telemetry blocks** — small consistent-read blocks work via guarded
  reads. Under low write rates seqlock is cheaper per read, but under
  sustained writes the guarded read is the one that keeps making progress. The
  disqualifier isn't semantics — it's whether the block's writers can all be
  routed through the engine (bit-0 contract).

## Honest constraints (what to say no to)

- **Opacity-grade semantics need versioning.** Point-in-time snapshots are
  covered by guarded reads (tier 2 above), but "nothing changed while I
  worked" — detecting an A-B-A excursion where the excursion matters — still
  needs embedder versioning. And whole-structure coherent iteration is out of
  reach of guards (footprint scaling); that corner belongs to clock/version
  schemes or a bulk stop-the-world regime.
- **Every transacted slot access must go through the engine** (bit-0
  ownership, proxy resolution). Retrofitting into code with raw pointer access
  everywhere — kernel `list_head` macro users, mmap'd shared-memory readers —
  is invasive. Greenfield structures or well-encapsulated ones (liburcu APIs,
  bind9's qp layer, a DB's index layer) are the realistic targets.
- **Uncontended single-writer regimes don't need it** — RLU beat txn at 1–8
  writers, and `rcu-txn-sw` or a plain lock is cheaper there. The pitch starts
  where writer counts or contention are real.
- **Bounded-blocking, not lock-free** — writers can't run in signal/interrupt
  context, and each commit costs a descriptor + grace-period deferral.

## Where to aim first

Three candidates score highest on impact × fit × demonstrability:

1. **Online rehash / atomic cross-table move in liburcu's own hash** — a
   capability no lock-free hash has, entirely within the API's control, and
   the hash benchmarks are already built.
2. **bind9 NSEC-chain + zone-update transactions** — the tree, the harness,
   and a real single-writer baseline (`qpmulti`) to beat already exist here.
3. **A conntrack-style dual-tuple flow table** — a tiny, legible 2-slot
   showcase whose kernel analogue gives the work an obvious upstream story.

The dcache/tasklist arguments are the most compelling on paper but carry the
highest retrofit cost — better as design papers than first prototypes.
