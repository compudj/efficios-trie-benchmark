# In-place vs COW under DLM + SW: the writer engine that separates FINE from OPTIMISTIC

How the fractal trie's structural-writer engine moves the **update-throughput ↔
reader-latency** frontier, and why the choice of writer strategy is really a
choice of *how a rank-changing node update is made atomic to readers*. Builds on
[rcu-gp-bounded-version](rcu-gp-bounded-version.md) and
[rcu-txn-bitmap](rcu-txn-bitmap.md), and **corrects §6 of the latter**: its
"rank-compressed stays COW, pigeon only" verdict is an *MCAS-engine* result, not
an engine-independent one. **Design only.** 2026-07-19.

## 0. The question

Three writer strategies now coexist (`fractal-trie.h`
`cds_ft_writer_strategy`): **OPTIMISTIC** (default, lock-free MCAS with a
fair-mutex escalation valve), **LOCK_FINE** (per-node COPYING lock-set, FT-wide
mutex dropped by default as of the 2026-07-19 flip), and **LOCK_COARSE** (one
FT-wide mutex). An A/B/C throughput sweep is the obvious next experiment — but it
only measures the right thing once the engine below FINE is understood, because
FINE's real advantage is not "a different flavor of exclusion." It is a
*capability OPTIMISTIC does not have*: **in-place** rank-changing mutation. This
note pins that capability down.

## 1. Why a rank change is hard: the {occupancy, slot@rank} pair

A popcount-compressed node stores a child for byte `k` at
`array[rank(k)] = popcount(bitmap & below(k))`. A rank *change* (insert/delete)
therefore flips an occupancy bit **and** shifts the array suffix — a multi-word,
non-atomic edit of the `(bitmap, array)` pair. A lockless reader that computes a
rank from the bitmap and then indexes the array can straddle the edit and see an
old rank against a new array. Today FT avoids this by **COW recompaction**
(`ft_node_recompact`): build a fresh node, rebuild bitmap + rank-compressed array,
republish the parent edge. COW is correct because *the single parent-edge store
is the linearization point* — the reader resolves one pointer to old-or-new,
never a torn pair. The price is an allocation, an O(node) `memcpy`, and a
grace-period reclaim, and it serializes writers behind single-writer COW.

The whole design space is: **how else can that `(bitmap, array)` pair be made to
flip atomically for the reader, without COW's alloc + copy + reclaim?**

## 2. What does NOT work: a plain-store seqlock

The tempting shortcut — mutate in place with plain stores, and bracket readers
with a seqcount — **fails**, and the failure is instructive.

- If the seqcount is a *monotonic counter not tied to the write*, a reader can
  read torn data mid-shift, and if both its samples land after the last
  *completed* update it sees no change and accepts the tear.
- Fixing that needs a **busy/odd state** (writer goes even→odd→stores→even;
  reader retries on odd) — i.e. a classic seqlock. But a seqlock's retry trigger
  is "is a write *in flight right now?*", and under a write stream there is
  always *some* in-flight window. A reader can hit busy windows back-to-back with
  **no bound** — the reads are starvable, *theoretically endless*, not `2^N`.

The `2^N`-bounded / `≤1`-wait-free results of
[rcu-gp-bounded-version](rcu-gp-bounded-version.md) are properties of the
**atomic-flip** construction, where the only retry source is "a *completed*
commit straddled my two samples" (which the GP + recompaction valve bounds). They
do **not** rescue a busy-window seqlock. **Conclusion: in-place must be an atomic
commit, not a seqlock.** Everything the update touches — occupancy word, every
shifted slot, and the version — must flip at *one* linearization point.

## 3. What works: the DLM + SW two-level engine

The atomic in-place commit is delivered by two layers, and keeping them distinct
is the key to the cost model:

1. **DLM (distributed lock manager) — CAS.** Acquire the node's per-node
   lock(s) (the COPYING lock-set). This is a **bounded** number of CAS — the
   lock-set size, a small constant *independent of node size* — and it is the
   only CAS in the update. It provides writer exclusion: single-writer on the
   node.
2. **SW txn under the held lock — plain stores + one selector commit.** With the
   node owned, the structural update is a *software* transaction: plain-store the
   new values (the shifted slots, the occupancy bit), then **one commit** installs
   the old/new **selector**. The seqcount rides *this* commit.

**The reader resolves wholly-old XOR wholly-new, atomically with that commit** —
it follows the selector (proxy) to a consistent world view and is done. No busy
bit, no "write in progress," no retry-until-quiescent: the reader never observes
the intermediate stores, only the pre-commit or post-commit world. This is why
the endless-busy-wait of §2 never arises — there is no torn window to spin on.
Reads stay **wait-free** (bounded proxy-resolve; a plain masked load in the
settled common case). The SW txn engine exists in both **MW and SW** forms
(`include/urcu/rcu-txn-bitmap.h`, 12/12 TAP), so a composed commit is legal under
either OPTIMISTIC's MCAS or FINE's sw path.

Two vocabulary corrections this makes explicit:

- **In-place ≠ plain-store seqlock.** In-place = the SW txn atomically committing
  the changed words in the same node.
- **COW ≠ "riding the txn."** COW is the *overflow / promotion valve* (version
  wrap, or capacity growth): alloc a new node, one publish. The normal in-place
  path is the atomic SW commit; COW is the exception.

## 4. Re-pricing in-place vs COW: §6 was an MCAS verdict

[rcu-txn-bitmap §6](rcu-txn-bitmap.md) concludes "in-place beats COW only for a
direct-indexed (pigeon) tier; every rank-compressed tier stays COW." That is
**correct for the MCAS engine and only for it**: it prices the O(n) rank shift as
O(n) *proxy-CAS* (~250 CAS for a 125-slot shift) — a node-wide k-CAS latch that
loses to COW on both cost and contention. Under **DLM + SW** the same shift is
O(n) **plain stores** + one selector commit, and the DLM cost is a constant
lock-acquire, not O(n) CAS. The comparison becomes:

> **in-place (SW):** DLM CAS (const) + O(n) plain stores + O(n) selector installs + GP-settle
> **COW:**          malloc + O(n) memcpy + one publish + call_rcu reclaim

Same O(n) core; the read side is ~neutral (wait-free proxy-resolve either way —
the "read throughput should not move" of [rcu-txn-bitmap §8](rcu-txn-bitmap.md)).
So in-place-vs-COW collapses to a **write-side** comparison, and in-place drops
exactly the **allocation and the grace-period reclaim churn**. That should win
**broadly — not pigeon-only** — with a *much weaker* node-size dependence than the
MCAS analysis implied. The crossover (does O(n) plain-store + selector-install +
settle beat malloc + memcpy + reclaim, and at what fanout) is now a real
empirical question rather than a foregone COW win.

Per tier, restated under SW:

- **Pigeon** (direct-indexed, 256-way, 2 KB): in-place = `{store ptr[k]} + {set
  bit k}` = **2 edges**, wins under *both* engines and by the most (COW here is a
  2 KB copy). Settled — do it. The transacted bitmap becomes authoritative
  instead of a relaxed hint (`rcu-txn-bitmap` §6 already targets this).
- **Rank-compressed** (`popcount_1l/2l`): in-place is O(n) — *loses under MCAS*
  (§6), *plausibly wins under SW* (drops alloc/reclaim). **SW-only capability;
  benchmark decides the fanout crossover.**
- **Small tiers** (≤14–16): the deeper lever is *representation* — an unsorted
  `{key,ptr}` array (ART node4/16) makes insert an append = O(1) edges, in-place
  under either engine, at the cost of search-vs-rank lookup ([rcu-txn-bitmap
  §6](rcu-txn-bitmap.md) tail). Orthogonal to this note; noted as the way to push
  the direct-vs-compressed boundary down.

## 5. Reader coherence: what FT actually needs

FT point lookups do **not** need a version. Reads are **tier-1** (per-slot
linearizable): the pointer is the source of truth and the reader revalidates the
key at the child, so a torn `{rank, array}` straddle yields a miss/retry — never a
wrong answer (`rcu-txn-bitmap` §5). The version / selector is the **opt-in tier-2
coherent snapshot** for a reader that genuinely needs a coherent `{bitmap, slot}`
pair — chiefly **iteration**, which reads occupancy to enumerate populated slots.
So "upgrade pigeon iteration from stale-tolerant hint to a coherent snapshot" is
the tier-2 guarded read, not a bespoke counter.

## 6. Version placement and budget (popcount, if it goes in-place)

If a rank-compressed tier adopts SW in-place, its seqcount must be **transacted**
(flips with the commit) **and** on the point-read's cacheline. Both hold at once
because the **occupancy word is itself a txn slot**: the seqcount rides in that
word's spare bits — committed with the rank change and on the occupancy CL. The
transacted bitmap is the 63-bit-per-word encoding (`rcu-txn-bitmap` §3: bit 0 =
`URCU_MCAS_TAG`, data in bits 1..63, the engine's `<<1` discipline, no engine
change). Consequences, from the layout survey (`ft-tables.h`, 64-bit):

- **Capacity.** 256 occupancy bits at 63/word = **5 words (40 B), not 4 (32 B)** —
  +1 word. The 5th word spends 4 bits on occupancy (4×63=252, +4), leaving **59
  free transacted bits on the occupancy CL** — the seqcount's home. So the
  63-bit conversion and the version slot are **one cost, not two**.
- **Per-tier.** The two 2-level tiers (`scan_32_8`, `scan_64_4`) had 16 / 8 spare
  bits in plain 64-bit words; the flat `popcount_1l` tiers (28/60/124 children)
  are byte-exact with a fully-meaningful 256-bit bitmap, so the +1 word costs one
  child slot (−3.5% / −1.7% / −0.8%). idx0 (cap 3) is a −33% carve → keep it COW.
  **The budget must be re-run under 63-bit words** (bit 0 becomes the tag on every
  word, shrinking the plain-word spares).
- **Read path.** rank = popcount must mask bit 0 per word (`AND-NOT` before
  popcount), and bitmap-word loads become proxy-resolving (plain masked load in
  the settled case). Small, on the hot path.
- The 44 free `state`-word bits are **not** a version home: the SoA split
  (`range->metadata[]`) keeps the state word off the point-read cacheline. (It
  *is* on the *iteration* cacheline — line `ft-ordered-query.h:280` loads
  metadata per node — which is why a *pigeon iteration-only* seqcount could live
  there for free; but point-read coherence must be on the node body.)

## 7. Why this separates FINE from OPTIMISTIC (and what the benchmark must include)

The through-line of the whole analysis is one separation:

- **OPTIMISTIC = MCAS.** Every structural word is a proxy-CAS; a rank shift is
  O(n) CAS; a node is the unit of contention but the engine pays per-slot. So it
  is **COW-bound** by construction — §6's verdict, *for this engine*. Its
  fine granularity is all cost and no benefit, because rank changes are
  node-global (they perturb the whole rank layout), so there is no intra-node
  concurrency for slot-granularity to exploit.
- **FINE = DLM + SW.** One bounded lock-acquire, then plain-store data + one
  selector commit. The shift is cheap stores; in-place is viable; the alloc +
  reclaim churn is removed. **In-place-capable.**

So an A/B/C sweep run against *today's* tree — where FINE still COWs and still
MCAS-publishes — measures **COW-FINE-with-locks vs COW-OPTIMISTIC** and shows
FINE carrying only lock overhead with no upside. The honest comparison is
**COW-bound OPTIMISTIC vs in-place-capable FINE(sw)**, and FINE(sw) does not
exist until the SW in-place path (composed bitmap commit) lands. Benchmarking
before it is measuring the wrong engine.

Corollary for the sweep design: include a **rank-preserving value-update** column
(two writers changing existing values, no occupancy change). Those are
single-word atomic — no exclusion needed either way — so OPTIMISTIC pays nothing
and FINE pays a lock it did not need. That is the one regime where fine
granularity could claw back, and it keeps the result from being "FINE wins
everywhere" by construction.

## 8. Roadmap and status

Built / landed:

- The transacted 63-bit bitmap engine exists in **both MW and SW** forms (12/12
  TAP, `include/urcu/rcu-txn-bitmap.h`), flowing to `ft-txn-integ`.
- The FT-wide-lock drop is landed and **default-on for FINE** (2026-07-19) — the
  first step of the DLM move (per-node COPYING locks, FT-wide mutex gone).

The engine move is **two bisectable steps**, in this order — *not* an op-first
integration (a composed-commit op is a consumer of step 2, so building it before
the content engine flips would exercise the MW/MCAS bitmap, the wrong engine):

**Step 1 — move FT to the DLM scheme, still on MW (MCAS) content.** Adopt the
composable multi-lock (deadlock-free lock-set *acquire*) as FT's sole exclusion,
replacing the ad-hoc COPYING-mark + guard-fallback; the FINE-lock-drop is its
precursor. Content stays MCAS, so rank changes **still COW** (§4/§6: MCAS in-place
loses) — this step buys the *exclusion* change, not in-place. It is separately
measurable: **DLM+MCAS FINE vs OPTIMISTIC** isolates whether the lock manager's
bounded-CAS-then-arbitrate beats OPTIMISTIC's per-slot MCAS on exclusion alone
(the "too fine-grained" question), before in-place enters the picture.

**Step 2 — flip content MW → SW.** Replace MCAS content with the SW txn
(plain-store data + one old/new **selector** commit) under the DLM locks, and
integrate the two missing pieces: the **seqcount** and the transacted 63-bit
bitmap — specifically the bitmap's **SW variant, not the MW one**. Under the DLM
lock the writer is single-per-node, so the SW bitmap's plain-store + selector
commit is the right fit; the MW bitmap would pay per-word MCAS *on top of* a lock
that already provides exclusion — redundant CAS. This is where reads become
selector resolves and **in-place becomes viable**. The in-place ops fall out as
consumers — pigeon composed commit first (2 edges, the built code), then the
rank-compressed crossover (§4). The A/B/C sweep is meaningful only *here*: it is
the first point at which FINE(sw)-in-place exists to compare against COW-bound
OPTIMISTIC.
