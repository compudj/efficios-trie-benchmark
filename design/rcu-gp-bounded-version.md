# GP-bounded versioning: torn-free multi-word reads that stay wait-free

Design for a versioning scheme that lets an RCU reader take a **consistent
snapshot of data spanning more than one word** without giving up the wait-free
reads RCU is chosen for. The mechanism is a seqcount shrunk to a handful of bits,
aliased into a word the reader already loads, with its *exposure* bounded by the
grace period and **COW recompaction as the overflow valve**. Bounding the version
that way turns out to buy more than wraparound safety: it upgrades the read from
"obstruction-free and starvable" to **bounded-retry**, and at the one-bit extreme
to genuinely **wait-free**.

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
- **Writer (cold path): overflow detection.** Snapshot the version at each GP
  transition (a per-node value on the cold metadata line, plus the global GP epoch
  polled per update). Before an in-place update, check whether the version could
  advance a full period within the live-reader window; if so, **fall back to
  recompaction (COW)** for that update instead of incrementing in place.
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
not.** The forced COW freezes the very node a reader is on within `2^N` updates;
once frozen, that reader's version stops moving and it completes on its next
attempt. So retries are **bounded by the overflow threshold**, upgrading the read
from "obstruction-free, starvable" to **bounded-retry**. Strictly better than any
plain seqcount, and the reason to prefer this construction even where wraparound
was never a practical worry.

The other property worth stating: the **fallback is rare and benign**. With
updates-per-node-per-GP well under `2^N` it never fires; only a node hammered
`2^N` times within a single grace period recompacts, and that node was write-hot
anyway.

## Width is the knob: write throughput ↔ read latency

Version width `N` sets the retry bound at `≤ 2^N` and the overflow threshold at
`2^N` updates per GP per node. The two move in opposite directions, and that is
the whole tuning axis:

| `N` | retry bound | in-place updates per GP per node | character |
|---|---|---|---|
| **1 bit** | **≤ 1 — wait-free** | ≤ 1, everything else recompacts | tightest reads, most recompaction |
| **7 bits** | ≤ 128 | ≤ 128 | the practical middle (see layout) |
| wider | ≤ `2^N` | ≤ `2^N` | fewer recompactions under bursts, looser read bound |

Narrower → smaller reader footprint, tighter retry bound, *more* frequent
recompaction. Wider → the reverse. Pick per structure, or per node size class.

### Concrete layout: 7 bits, sharing the low byte with the txn tag

The practical point on that curve, for a version sharing an MCAS-transacted word:

```
bit 0      : txn proxy tag   — reserved by the engine, not available
bits 1..7  : VERSION         — 7 bits ⇒ retry bound ≤ 128
bytes 1..7 : other state     — occupancy bits, lock/liveness, payload lanes
```

**The engine's tag bit takes bit 0, so a version sharing that word gets 7 bits,
not 8 — a retry bound of 128, not 256.** Sizing it as "a byte" is the mistake to
avoid here; the byte is shared. Seven bits is ample: it bounds reader retries at
128 while allowing 128 in-place updates per node per grace period before the
recompaction valve opens, which no realistic per-node write rate approaches. And
it keeps the version inside the low byte, leaving the word's **other seven bytes
entirely free for other state** — which is exactly the 7+1 bytewise lane encoding
[rcu-txn-blob](rcu-txn-blob.md) already uses, so the version costs no lane.

### The extreme: a single-bit latch, wait-free for real

Push `N` to its floor — **one bit** — and the retry bound collapses to its
minimum. Gate the flips so a reader's window holds **at most one**: after flipping
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

**This dissolves the wait-free-vs-torn-free tension.** The classical statement is
that a snapshot mechanism categorically costs wait-freedom, leaving COW as the
only way to keep reads wait-free. The GP-gated single-bit latch gets **both** —
torn-free (the latch catches the flip) and wait-free (≤ 1 retry) — with the cost
moved entirely to the write side. So **recompaction stops being the price of
wait-free reads and becomes the price of write throughput above one in-place
update per GP per node.** Same knob, with the read side pinned at wait-free.

## Preconditions and caveats

- **It needs a recompaction fallback to exist.** The node must be
  pointer-reachable and replaceable (swap the parent pointer). A **truly
  fixed-address** structure — a pool slot others point *into* by raw pointer — has
  no COW escape, so the valve is unavailable and the wait-free loss **stands**
  there. Every result above is for the recompactable case.
- **The version catches a *completed* update; the in-progress window needs its own
  cover** — either the content update is atomic at the flip (a flip-selector
  engine, where the latch simply *is* the selector, resolved through proxies), or a
  companion in-progress bit for a plain-load data path. So it is one-to-two bits,
  still aliased into padding.
- **Writer-side cost:** GP-epoch polling and a per-node GP-transition snapshot on
  the cold line, plus a dependency on the RCU flavor exposing a readable GP epoch
  (urcu QSBR/memb do).
- **Correctness hinges on a conservative window bound** — recompact before the
  version *could* alias for any live reader, accounting exactly for the ≤ 2-GP
  span and the in-progress bit.
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
capped at `2^N` per GP.

## Summary: what each option gives the reader

| mechanism | torn-free | progress class | footprint |
|---|---|---|---|
| single-word resolve (no version) | n/a — single word | **wait-free** | none |
| COW recompaction | yes (single publish) | **wait-free** | alloc + copy per rank change |
| plain seqcount | yes | obstruction-free, **starvable** | a word, hot |
| occupancy-as-version | yes, if entries ≤ 1 word | obstruction-free, starvable | ~1 padding bit |
| **GP-bounded `N`-bit** | yes | **bounded-retry (≤ 2^N)** | `N` padding bits |
| **GP-gated 1-bit latch** | yes | **wait-free (≤ 1 retry)** | 1–2 padding bits |
| validating read-only txn | yes, and **composes** across structures | not wait-free; contends | proxy per validated word |

The validating transaction is the only row that composes a snapshot across *other*
structures; every version-based row snapshots one structure alone. That, not
progress class, is the reason to reach for it.
