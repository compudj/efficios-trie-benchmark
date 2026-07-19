# GP-bounded versioning: torn-free multi-word reads that stay wait-free

Design for a versioning scheme that lets an RCU reader take a **consistent
snapshot of data spanning more than one word** without giving up the wait-free
reads RCU is chosen for. The mechanism is a seqcount shrunk to a handful of bits,
aliased into a word the reader already loads, with its *exposure* bounded by the
grace period and **COW recompaction as the overflow valve**. Bounding the version
that way turns out to buy more than wraparound safety: it upgrades the read from
"obstruction-free and starvable" to **bounded-retry**, and at the one-bit extreme
to genuinely **wait-free** — the last of these requiring that content and latch be
published atomically (case (a) under *Preconditions*). (The wait-free *torn-free
reader property* is not itself new — Left-Right, DISC 2015, already has it; what is
ours is the single-instance footprint and the drain outsourced to the grace period.
See *Prior art and novelty boundary* below before claiming anything.)

Extracted from [rcu-txn-blob](rcu-txn-blob.md), where it grew as an optional
control layer, because it is not blob-specific: it applies to any RCU structure
whose readers must see two-or-more words as a unit and that can fall back to
replacing a node. The fractal trie is the driving consumer. Companion to
[rcu-txn-blob](rcu-txn-blob.md) and [rcu-txn-use-cases](rcu-txn-use-cases.md).
**Design only — not yet implemented.** 2026-07-19.

## When you need a version at all

**A version buys exactly one capability: reading a value that spans more than one
word as a single atomic unit.** Under the txn engines each *individual* word
already linearizes on its own, so anything whose reads are single-word resolves
needs nothing here:

- **pointer-linked structures** (list, hlist, skiplist) — a traversal is a chain
  of single-word resolves linked by the data itself; the reader never needs two
  slots from one instant. A version would be dead weight.
- **a single-bit bitmap op, a ≤ 7-byte blob, a word-contained field** — one word,
  inherently atomic.

Only a **flat multi-word value read as a whole** (or a field straddling two
words) needs it. That is a narrow case, which is why this is opt-in and off by
default. Turning it on is not free: writers must bracket their updates, and the
naive form costs the read side its progress guarantee — which is the whole
subject below.

## The baseline seqcount, and its two costs

The textbook answer is a generation counter: the writer bumps it before and after
mutating, the reader samples it before and after reading, and retries unless the
two samples agree and indicate no update in flight. Passive pure-load reads, no
proxy planting, no reader-reader aborts, never perturbs writers.

Two costs, and only one of them is the interesting one.

**Footprint.** A full generation word on the hot data cacheline is real estate.
For a small node it can displace enough entry capacity to tip a 1-cacheline read
into a 2-cacheline read — paying on *every* read for a consistency guarantee
needed only across updates.

**Progress class — the significant one.** The baseline RCU read is **wait-free**:
a single-word resolve completes in a bounded number of its own steps with no
retry. A seqcount reader is only **obstruction-free** — it retries until it
catches a quiescent window, so a sustained write stream can **starve it
indefinitely**. Classically, for a flat multi-word value you can have wait-free
*or* torn-free, not both; the wait-free way to snapshot many words is to make them
reachable through a **single pointer** (COW), which is what recompaction buys.

The rest of this document dismantles both costs. Footprint goes first and easily;
the progress class goes last, and is the result worth having.

## Placement, and the discipline that makes sharing safe

Where the version word lives is not cosmetic. It belongs **hot, inside the data
cachelines** — a consistent read samples it before and after the data reads, so on
a separate line every snapshot pays an extra cache miss, defeating the "passive
cheap read" point. The writer bumps it only while mutating data it already owns
exclusively, so co-location adds no write-side cross-core traffic.

This is the *opposite* of where a writer-exclusion **lock** belongs. Acquirers
spin-CAS a lock word under contention; on the data line every *failed* CAS from a
spinning writer invalidates readers' copy of data they never needed to reload —
false sharing that scales with writer contention and directly costs reader
scalability. Locks and liveness flags go in **cold metadata**; the version goes
with the data.

Sharing one physical word between version bits and lock/liveness bits is
nonetheless safe, and often desirable (one load yields both), for one reason:
**readers mask to the version field**, so a lock or tombstone transition leaves
the version bits untouched and never triggers a spurious retry.

**The discipline this rests on: the lock is writer-only and must never enter a
reader's decision.** A reader's consistency comes from exactly two reader-facing
things — per-word resolution and the version — and never from the lock.
Conflating the lock bit with the version parity is a bug, because:

- a **lock release with no content change must not perturb readers** — hence the
  masking rule above;
- a writer may **hold the lock across several version-bracketed sub-updates**,
  letting readers snapshot consistently *between* them while exclusion is held;
- the lock may be held for **non-content reasons** where no version bump is
  warranted.

The lock *enables* a cheap parity version (by providing the exclusion parity
needs) but is **not** the parity.

One wrinkle when the shared word is itself **MCAS-transacted**: while an escalated
acquire has a real proxy parked there, sampling the version must resolve the proxy
to recover it, so the sample is no longer a plain load. The common un-escalated
acquire is a bare CAS that never parks, so this bites only under lock
contention — precisely when you want the version split off onto a plain data word
anyway.

## Shrinking: a version needs very few bits

Wraparound fools a reader only if the counter advances a **full period within one
read window** — sample, the data loads, re-sample. That window is a few tens of
cycles. An update (acquire, mutate, release) is far slower, so a byte — even a few
bits — cannot wrap in time. **A full generation word was always overkill.**

### Aliasing onto data the reader already loads

Better than a small dedicated field: notice when existing data *is* a version.

In a popcount-compressed node, a **rank-changing** update by definition flips an
occupancy bit, so the occupancy word — already loaded to compute rank — **is** a
version for rank changes. Sample occ, read the target slot, re-sample occ;
unchanged ⇒ no rank change straddled the read ⇒ consistent. Zero dedicated bits.
One spare bit in that word's padding carries an **in-progress** flag covering the
torn window of an in-flight update (set before the shift, cleared after; retry if
set at either sample).

Two conditions make occupancy-as-version exact:

- **Single-word entries.** It catches every rank *change*, but a
  rank-*preserving* multi-word value update leaves occupancy unchanged and could
  slip entirely between the two samples, undetected. Where entries are pointers
  (≤ 1 word) rank-preserving updates are single-word atomic and never tear, so
  occupancy + in-progress is a *complete* version. A structure with multi-word
  in-place values still needs real generation bits.
- **Serialized writers.** A single in-progress bit assumes one writer at a time
  (single-writer or lock-serialized). Lock-free concurrent writers need an
  enter/exit count instead.

**The footprint cost is now essentially gone** — one padding bit on a cacheline
already loaded, for small and large nodes alike. What remains is the categorical
cost: aliased or not, the retry still makes reads obstruction-free.

(Minor: occupancy-as-version over-retries. A concurrent rank change elsewhere in
the node — even of a higher key that does not move the target — changes occupancy
and forces a retry. Masking to the below-key occupancy bits the reader already
computes for rank trims most of these; the node-global in-progress bit still
forces a retry on any in-flight update. Finer-grained versioning costs storage,
itself a knob.)

## The core idea: bound the version's exposure with the grace period

A sharper way to shrink the version — and one that drops the single-word-entry
condition — is to bound its *exposure* rather than its rate, using RCU itself, and
to use recompaction as the overflow safety valve.

**An RCU reader cannot outlive the grace period.** So the updates a live reader
can straddle are bounded by the updates in its (≤ 2-GP) lifetime — **not** by
total write volume. A counter only has to be wide enough to not wrap within *that*
window.

- **Reader (hot path): compares only the version bits.** Sample, read the data,
  re-sample; accept iff the bits are equal and not in-progress. The bits ride in
  padding the reader already loads, so placement stays ~free.
- **Writer (cold path): threshold detection.** Snapshot the version at each GP
  transition (a per-node value on the cold metadata line, plus the global GP epoch
  polled per update). Before an in-place update, check whether the version has
  advanced by the fallback threshold `K` since that snapshot; if so, **fall back to
  recompaction (COW)** for that update instead of incrementing in place. `K` is a
  policy number, constrained only by the field's period — see *Two knobs* below.
- **Recompaction both fixes and resets.** COW is version-independent (the whole
  node reached through one pointer, unconditionally safe) and starts a fresh node
  with a zeroed counter, while the old node freezes and its readers drain on the
  grace period. The version's exposure resets at every fallback.

This is strictly stronger than occupancy-as-version: a real incrementing counter
catches a completed-within-window **multi-word rank-preserving** update too, so
the single-word-entry condition disappears — while placement still aliases it into
padding for near-zero footprint.

### The payoff: retries become bounded

The GP + recompaction machinery is introduced for wraparound safety, but its
important consequence is elsewhere.

**A plain seqcount reader is starvable indefinitely by a write stream. Here it is
not.** The forced COW freezes the very node a reader is on within `K` updates;
once frozen, that reader's version stops moving and it completes on its next
attempt. So retries are **bounded by the fallback threshold**, upgrading the read
from "obstruction-free, starvable" to **bounded-retry**. Strictly better than any
plain seqcount, and the reason to prefer this construction even where wraparound
was never a practical worry.

Note what does the work here: the bound is `K`, the threshold the *writer* checks
against — **not** the version's field width. Wraparound safety is a separate
constraint, and conflating the two costs you the ability to tune the retry bound
independently of layout. The next section separates them.

The other property worth stating: the **fallback is rare and benign**. With
updates-per-node-per-GP well under `K` it never fires; only a node hammered `K`
times within a single grace period recompacts, and that node was write-hot anyway.

### When the bound isn't wanted: the ungated configuration

The bound is the point of this construction, but it is not free, and not every
consumer wants it. Everything above — the GP-epoch poll, the per-node transition
snapshot, the threshold `K`, the recompaction valve — exists to *bound* reader
retries. **A consumer whose readers already retry unboundedly for some other
reason has nothing to buy here** and should take a plain seqcount: no gate, no
`K`, no valve.

That is not a degenerate case, and it is not rare. Any structure whose reader
validates a multi-hop traversal by re-reading generations and re-walking on
mismatch has already spent its progress guarantee at the traversal level. Putting
a *bounded* per-node version underneath an *unbounded* walk-level retry buys
nothing — the walk is still starvable, so the node read may as well be too. The
apparatus would be pure cost.

Such a consumer pays only the two costs in *The baseline seqcount*, of which the
footprint one is already answered by aliasing. It needs a generation the writer
bumps and an in-progress bit, and it may write the content with **plain stores** —
the torn window is covered by the in-progress bit, and the unbounded retry that
implies was already the contract.

**So the realization follows the consumer's progress target, not a universal
ranking.** Target wait-free reads and you need the atomic flip *and* the gate
(case (a) under *Preconditions*). Accept retry and a plain-store seqcount is both
sufficient and substantially cheaper: no transacted content, hence no encoding
cost on the read path, no valve, and no fallback path to maintain.

## Two knobs, not one: width bounds wraparound, the threshold bounds retries

The version has **two** numbers, and they are independent. Earlier drafts of this
document collapsed them into a single `N`; that is wrong in general, and the
collapse hides the more useful of the two knobs.

- **Field width `W`** — how many bits the version occupies. It bounds
  **wraparound**: the reader compares `W` bits, so it is fooled only by an advance
  that is a nonzero multiple of `2^W` inside its read window. `W` is a **layout**
  property — how much padding the aliased word has to spare.
- **Fallback threshold `K`** — how many in-place updates a node accepts within the
  live-reader window before the writer recompacts instead. It bounds **retries**: a
  reader retries at most once per update it straddles, and after `K` the node
  freezes, so retries are `≤ K`. `K` is a **policy** number the writer compares
  against, chosen by measurement.

The only thing linking them is the safety constraint

> **`K ≤ 2^W − 1`** — the fallback must fire before the version could alias.

Within that bound `K` is free. (The bound is strict: an advance of *exactly* `2^W`
aliases, so a `W`-bit field admits `K = 2^W − 1`, not `2^W`. Earlier text saying
"7 bits ⇒ 128" was off by one; it is 127.)

The two were easy to conflate because the natural default is the maximum,
`K = 2^W − 1` — let the field's own period be the trigger, and one number does
both jobs. At the one-bit extreme they are *forced* to coincide (`W = 1 ⇒ K = 1`),
which is why the latch section below still reads as a single knob. But the
collapse is a default, not a law, and two cases break it:

- **Lowering `K` below the period** tightens the retry bound *without touching
  layout*. Read latency and write throughput then trade against each other along
  `K` alone, at fixed `W` — which is the knob you actually want at tuning time,
  since `W` is usually dictated by whatever padding happened to be available.
- **A version riding an existing wide counter** makes wraparound vacuous. `2^W` is
  then astronomically large, the constraint never binds, and `K` is the *only*
  knob — there is no width decision left to make.

| `K` | retry bound | in-place updates per GP per node | character |
|---|---|---|---|
| **1** | **≤ 1 — wait-free** (case (a) only) | ≤ 1, everything else recompacts | tightest reads, most recompaction |
| **8–16** | ≤ 8–16 | ≤ 8–16 | tight reads, recompaction already rare |
| **127** (`W = 7`) | ≤ 127 | ≤ 127 | the maximum a 7-bit field admits |
| any, wide `W` | ≤ `K` | ≤ `K` | `K` chosen purely by measurement |

Smaller `K` → tighter retry bound, *more* frequent recompaction. Larger `K` → the
reverse. Pick per structure, or per node size class; then pick `W` as whatever the
layout can spare, subject only to `2^W > K`.

### Concrete layout: `W = 7`, sharing the low byte with the txn tag

The practical point for a version sharing an MCAS-transacted word — the case where
`W` really is scarce, and `K = 2^W − 1 = 127` is the natural default:

```
bit 0      : txn proxy tag   — reserved by the engine, not available
bits 1..7  : VERSION         — W = 7  ⇒  admits K ≤ 127
bytes 1..7 : other state     — occupancy bits, lock/liveness, payload lanes
```

**The engine's tag bit takes bit 0, so a version sharing that word gets 7 bits,
not 8 — an admissible `K` of 127, not 255.** Sizing it as "a byte" is the mistake
to avoid here; the byte is shared. Seven bits is ample: at the default
`K = 2^W − 1` it bounds reader retries at 127 while allowing 127 in-place updates
per node per grace period before the recompaction valve opens, which no realistic
per-node write rate approaches — and if a tighter read bound is wanted, `K` can be
dropped to 8 or 16 with no layout change at all. It also keeps the version inside
the low byte, leaving the word's **other seven bytes entirely free for other
state** — which is exactly the 7+1 bytewise lane encoding
[rcu-txn-blob](rcu-txn-blob.md) already uses, so the version costs no lane.

### When `W` disappears: riding a counter the reader already samples

The opposite regime is worth naming, because it is the case where separating the
two knobs stops being pedantry. If the version can alias into a word that is
*already* a wide generation counter — one the reader loads and samples for some
other reason — then the version costs **zero new bits and zero new loads**, `2^W`
is far beyond any reachable advance, and the wraparound constraint is vacuous.
`K` is then the only design parameter, and it is pure policy: *after `K` in-place
updates to this node within one GP, take the fallback.*

A structure whose readers already run a per-node generation bracket for some
*other* invariant (walk causality, freshness, ABA rejection) gets the multi-word
snapshot essentially free by reusing that counter, provided the writer already
bumps it on the updates in question. Whether a given consumer qualifies is a
per-structure question — the point here is only that in this regime "how wide
should the version be?" is not a question, and the entire tuning surface is `K`.

### The extreme: a single-bit latch, wait-free for real

Push both knobs to the floor — `W = 1`, and therefore `K = 1` — and the retry
bound collapses to its minimum. This is the one point on the curve where the two
numbers are *forced* to coincide, which is why it reads as a single knob. Gate the
flips so a reader's window holds **at most one**: after flipping
the latch, the next in-place flip is allowed only once the grace period
*following* the previous flip has completed (every reader live during that flip
has drained); otherwise recompact. Then no reader spans two flips:

- read latch `b1`, read the data, read latch `b2`;
- `b1 == b2` → no flip straddled the read → accept;
- `b1 != b2` → exactly one flip → retry **once**, guaranteed to succeed (the
  reader was live during the flip it saw, so it drains before the next flip — its
  second window is flip-free).

≤ 1 retry is a constant independent of writers, so the read is **wait-free — for
real**, not merely bounded by a large constant.

**What this buys, stated without overclaiming.** Wait-free torn-free reads of a
mutable structure are not new — Left-Right (Ramalhete & Correia, DISC 2015) already
gets both, via a single-bit toggle gated on reader drain, and its reads are in fact
*retry-free* (0 retries), a hair stronger than our ≤ 1. So the reader *property* is
not the contribution. What is ours is the *footprint and drain*: the latch gets
torn-free + wait-free on a **single in-place instance** — no permanent second copy —
with the reader-drain **outsourced to the RCU grace period the system already pays**
(zero reader-side synchronization writes, unlike Left-Right's per-reader
arrive/depart indicator), and recompaction as a **rare overflow valve** rather than
a standing replica or a per-update copy. Framed that way, recompaction is not the
price of wait-free reads (it never was, for anyone holding a replica); it is the
price of write throughput above one in-place update per GP per node — *at
single-instance footprint*. The full prior-art boundary is below.

## Preconditions and caveats

- **It needs a recompaction fallback to exist.** The node must be
  pointer-reachable and replaceable (swap the parent pointer). A **truly
  fixed-address** structure — a pool slot others point *into* by raw pointer — has
  no COW escape, so the valve is unavailable and the wait-free loss **stands**
  there. Every result above is for the recompactable case.
- **The version catches a *completed* update; the in-progress window needs its own
  cover.** There are two realizations, and **which is right follows the consumer's
  progress target, not a universal ranking**:
  - **case (a), atomic flip** — the content update is atomic at the flip (a
    flip-selector engine, where the latch simply *is* the selector, resolved
    through proxies). **In the DLM realization this is the case: the seqcount is a
    transacted slot of the SW txn, flipped atomically with the content in the same
    selector store.** Required if reads must be wait-free.
  - **case (b), plain stores + in-progress bit** — content written with plain
    stores, a companion bit covering the torn window. Sufficient, and considerably
    cheaper, for any consumer that already tolerates unbounded reader retry (see
    *When the bound isn't wanted*): the content need not be transacted, so the
    read path pays no encoding or resolve cost.

  Either way it is one-to-two bits, still aliased into padding.

  Case (a)'s atomicity is load-bearing **for the wait-free claim specifically**,
  and it is worth being precise about why, because the obvious reason is not the
  operative one. It is **not** that a sustained write stream starves the case-(b)
  reader: GP-gating bounds the number of *distinct* updates any reader can straddle
  to `K` whatever the realization, so gated case (b) is already strictly better
  than a plain seqlock — bounded interference, not unbounded. The operative reason
  is **who the reader waits on**. In case (b) the in-progress bit makes a reader
  wait for the writer to *finish* a non-atomic multi-word write, so a writer
  preempted mid-write holds off every reader of that node until it is rescheduled.
  That is a dependency on writer progress, and it disqualifies wait-freedom however
  small `K` is — even at `K = 1`, where gating provably admits no *second* update,
  the reader is still stuck inside the *first* one's torn window. Case (a) has no
  in-flight state to observe: the reader sees all-old or all-new, a stalled writer
  is invisible to it, and it fails only on a *flip* between its two samples, which
  gating bounds to ≤ 1.

  **Atomicity removes the wait-on-writer; GP-gating bounds the interference
  count** — different hazards, neither substituting for the other. A consumer that
  wants wait-free reads needs both. A consumer that does not want the bound needs
  neither and should take **ungated case (b)**, where the wait-on-writer reduces to
  a single content write: acceptable when that write is O(1) and short (a
  fixed-size copy), and worth an explicit note at the site so the window is not
  grown into something unbounded later.
- **Writer-side cost:** GP-epoch polling and a per-node GP-transition snapshot on
  the cold line, plus a dependency on the RCU flavor exposing a readable GP epoch
  (urcu QSBR/memb do).
- **Correctness hinges on a conservative window bound** — recompact before the
  version *could* alias for any live reader, accounting exactly for the ≤ 2-GP
  span and the in-progress bit. Concretely: the writer's threshold `K` must be
  enforced against the *whole* live-reader window, not per-GP-epoch, and must
  satisfy `K ≤ 2^W − 1`. Both halves matter — a correct `K` counted over too short
  a window is as unsafe as a `K` too large for the field.
- **Version flavor follows the writer model:** **parity** where writers are
  serialized (single-writer, or lock-serialized), **enter/exit** in-flight count
  where they are concurrent. Parity is the single-writer collapse of enter/exit,
  so one code path serves both.
- **The writer must bracket its update.** A composed update that appends edges to
  a caller's transaction and does not own the install/settle lifecycle cannot
  bracket, so a structure updated that way is not version-readable. Mixing
  version reads with unbracketed composed writers is unsafe.

## Fractal trie: the driving consumer

FT is converging on the hybrid/DLM scheme (MCAS composable multi-lock + sw
content) and initially deferred the version entirely, for two reasons: a
generation word on the hot data cacheline is footprint it did not want, **and** a
seqcount would forfeit FT's wait-free reads. Both objections are answered above —
aliasing kills the first, GP-gating the second — so the version is now a live
tunable option rather than something deferred.

The accepted price of *not* having it is **recompaction**: without a version on
the hot line, readers cannot get a cheap consistent `{occupancy, slot@rank}`
snapshot, so a **rank-changing** update (one that sets or clears an occupancy bit
and shifts every higher entry's rank) is done by COW — build a new node cluster,
publish with one store, reclaim the old. Inherently torn-free via the single
publish, and wait-free to read. The price is allocate + copy per rank change.
**Rank-preserving** in-place updates (an existing entry's value, ≤ one word) stay
version-free single-word stores, so only rank *changes* recompact.

### The crossover is node-size-dependent, and the two effects reinforce

- *Recompaction cost* (paid without a version) **grows** with node size: a
  rank-changing update COW-copies the whole node — alloc + O(entries) + deferred
  free — and large nodes churn large blocks through the grace period and hold the
  node's lock longer.
- *Version cost* (paid to have it) **shrinks** with node size, and FT's
  **two-cacheline read guarantee** decides it. Every lookup is bounded to two CL
  loads regardless of node size (the occupancy/rank CL, then the target
  `slot@rank` CL), and the consistency a reader needs — a coherent
  `{occupancy, slot@rank}` pair — is itself **size-independent** (two words on two
  different CLs, so no single-CL atomic covers it). What differs is where the
  version lands relative to the CLs the read already touches. On a **large** node
  the occupancy bitmap sits well within one line, so the version **rides free on
  the occupancy CL** and enabling it adds **zero CLs**. On a **small** node packed
  into a *single* CL, a dedicated version word would displace entry capacity and
  tip a near-boundary node from a 1-CL to a 2-CL read.

So the curves cross: **small nodes favor COW recompaction**, **large nodes favor
in-place + version**. Because it is opt-in per embedding, FT can enable it **per
node size class** above a crossover threshold, with no change to the small-node
path.

**Expose the crossover as a tunable.** Toward update speed, lower the threshold so
more size classes go in-place (writes skip alloc/copy/reclaim). Toward read
latency, raise it so more classes COW-recompact. Because the mechanism is a
per-node *layout* property, the threshold sits best at a **size-class boundary**,
where crossing it coincides with FT's natural node promotion — a growing node is
rebuilt anyway, so it adopts the large-node layout for free. Per-domain /
per-size-class is the right granularity; per-node *dynamic* switching would cost a
layout conversion and is likely over-engineering.

Note that aliasing (occupancy padding) largely collapses the footprint axis of
this crossover, and GP-gating removes the progress-class asymmetry that would
otherwise make it a wait-free/obstruction-free split across size classes. What
remains is the genuine one: alloc+copy per rank change versus in-place updates
capped at `K` per GP.

## Summary: what each option gives the reader

| mechanism | torn-free | progress class | footprint |
|---|---|---|---|
| single-word resolve (no version) | n/a — single word | **wait-free** | none |
| COW recompaction | yes (single publish) | **wait-free** | alloc + copy per rank change |
| plain seqcount | yes | obstruction-free, **starvable** | a word, hot |
| ungated aliased seqcount, case (b) | yes | starvable — but *free* if the consumer already retries | 1–2 padding bits |
| occupancy-as-version | yes, if entries ≤ 1 word | obstruction-free, starvable | ~1 padding bit |
| **GP-bounded, threshold `K`** | yes | **bounded-retry (≤ `K`)** | `W` padding bits, `2^W > K` |
| **GP-gated 1-bit latch, atomic flip (a)** | yes | **wait-free (≤ 1 retry)** | 1 padding bit |
| **GP-gated 1-bit latch, plain stores (b)** | yes | blocks on writer, interference ≤ `K` | 2 padding bits |
| validating read-only txn | yes, and **composes** across structures | not wait-free; contends | proxy per validated word |

The validating transaction is the only row that composes a snapshot across *other*
structures; every version-based row snapshots one structure alone. That, not
progress class, is the reason to reach for it.

## Prior art and novelty boundary

Two adversarial prior-art reviews (2026-07-19) place this scheme precisely. The
verdict: it is a **novel combination, not a new primitive** — claim the combination,
cite-and-distinguish the neighbours, and never claim the version-as-validity *idea*.

The landscape is organised by two anti-correlated axes — reader **progress class**
and **footprint**:

| scheme | reader progress | footprint |
|---|---|---|
| **Left-Right** (Ramalhete–Correia, DISC 2015) | wait-free, **retry-free** (0 retries) | **two permanent full copies** + per-reader arrive/depart writes |
| **ARC** (Ianni–Pellegrini–Quaglia, IEEE TPDS 2019) | wait-free reads & writes | **N+2 permanent buffers** (worse) + reader writes |
| **plain RCU** | wait-free | **per-update whole-object copy** (always-COW) |
| **seqlock / StampedLock / TL2** | starvable / abortable (not wait-free) | single in-place instance |
| **LSA** | obstruction-free, abortable | multi-version store |
| **RLU** (SOSP 2015) | torn-free snapshot, *not certified wait-free* | single steady-state instance, but **copies every modified object per write** |
| **MV-RLU** (ASPLOS 2019) | non-blocking chain read | **master + version chain** (multi-version) |
| **this scheme** | **wait-free, ≤ 1 retry** | **single in-place instance, COW only on overflow** |

Every scheme with wait-free reads of a mutable multi-word value pays a permanent
replica or a per-update whole-object copy; every single-instance scheme has
starvable or obstruction-free reads. **No surveyed scheme occupies the corner this
one does** (single-instance-steady-state + wait-free/bounded-retry + copy-only-on-
overflow). RLU is the closest on footprint but copies per write and leans on a
TL2/LSA global clock; MV-RLU abandons single-instance for a version chain — neither
defeats the claim.

**Safe to claim:** the *combination* — single-instance-steady-state footprint,
wait-free/bounded-retry torn-free reads, zero reader-side synchronization writes via
the GP-outsourced drain, and COW-only-on-overflow. **Must cite-and-distinguish:**
Left-Right (owns the reader property, and is stronger on it — 0 retry — but pays a
standing 2× and per-reader writes), ARC, seqlock, plain RCU, and RLU. **Must not
claim:** the version-as-validity latch *idea* itself, which is anticipated by
seqlock / StampedLock / TL2 / LSA / RLU.

**The RCU-native question, settled.** A natural challenge is whether RCU's own
grace-period counter flip (Classic/Tree RCU's `->completed`/`->gpnum`, SRCU's
per-CPU flip, the `rcu_seq` cookies) already *is* a GP-gated flip serving as a
reader-facing data version. It is not: that counter is **global / per-gp-domain, not
per-node**, and is built of **per-CPU split counters** whose sum is expensive to
read — the opposite of a single bit aliased into a word a node's reader already
loads. Perfbook confirms the separation: a seqlock reader's data-version is the
seqlock's **own** sequence number ("snapshot the value before and after each
access"), while RCU is reached for as a **separate** mechanism ("some other
synchronization mechanism *in addition to* sequence locks"). RCU's flip detects
grace periods; it is not a per-node reader data-version.

**Footprint, stated precisely.** "Single in-place instance" means *no permanent
whole-structure replica* — the axis on which we beat Left-Right/ARC/RCU. The version
itself costs ~0 (1–2 bits aliased into a word already loaded). The *per-update* data
cost depends on the realization: the flip-selector case (a) parks transient proxies
for the *changed slots* (∝ commit width), settled at once — a small per-update
transient, not a standing replica and not a whole-object copy; the plain-store case
(b) copies nothing per update. A whole-node COW happens only on the **rare version
overflow**. So "copies only on overflow" is exact for case (b); case (a) adds the
flip-latch's transient per-slot proxies — in neither case a permanent replica.
