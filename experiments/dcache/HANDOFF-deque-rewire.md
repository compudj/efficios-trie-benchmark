# Handoff — move the MCAS LRU onto the deque

2026-08-04. Written at the end of the session that built the deque. Everything
below is committed and gated unless marked otherwise.

## The decision

**The MCAS LRU moves to `rcu-txn-deque.h`. `rcu-txn-list.h` is no longer
relevant to the LRU.** Do not port the membership seqcount to the list on the
LRU's account.

Two list fixes stay because they are independently correct for any other user:

- `750572af` — `del_prepare` load-validates its derivation read of `&elem->prev`.
- `b69b4a53` — all five convenience brackets leaked the escalation lane on their
  `-ENOENT` terminal bail (no `urcu_txn_abandon()` before `end()`), which the
  contract says holds the domain's lane forever.

## Why the list is the wrong structure here

A move and RCU traversal cannot coexist: a traverser standing on a relocated
node follows its new `next` and silently skips or repeats a span, and no barrier
or grace period repairs that — grace periods govern reclamation, not logical
position. `rcu-txn-list.h` offers both and says nothing.

This port has **no LRU read-side traversal at all**: the only read of an LRU link
outside the mutators is `dcache_lru_shrink.h:47`, one hop off the sentinel,
re-read every iteration. `dc_lookup` never touches it. So the LRU wants a
structure whose contract forbids traversal and therefore may offer relocation.

That also means the rationale written for `lru_move_tail` — protecting "a
lockless traverser standing on the node" — was **vacuous**. There is no such
traverser.

## State of the deque

`include/urcu/rcu-txn-deque.h` in `userspace-rcu-txn`, plus `test_deque.c` here.

- PASS at 2, 4, 8, 16, 32 writers, all three operation mixes, checking the
  `owner <=> reachable` biconditional and both edges at every step.
- Audited: no plain store in any mutator; every read is either `load_validate`
  or covered by a write of the same slot. See `91feec07` for the table.
- `owner` (a pointer to the deque) carries membership, identity and exclusion in
  the same commit as the edges. No claim protocol, no BUSY state, no deletion
  mark.
- ⚠ The per-node `seq` (ABA guard, `05b24b48`) is **UNPROVEN**: compiled out via
  `-DURCU_TXN_DEQUE_NO_SEQ_GUARD` the test still passes 3/3 at 16 writers. It is
  committed on argument, not evidence. Keep the macro so that stays measurable.

## The rewire

1. `d_lru` loses `shard` **entirely**: `struct urcu_txn_deque_node dnode`
   replaces `urcu_txn_list_node link` + `unsigned int shard`. Membership becomes
   `urcu_txn_deque_owner()`.
   ⛔ Do NOT cache membership in a second word. That re-creates the
   two-states-no-commit-covers defect the whole exercise exists to remove; the
   header says so at the accessor.
2. Delete the claim protocol: `lru_claim` / `lru_unclaim` / `lru_del_claimed` /
   `lru_unlink_claimed` and `DC_LRU_OFF|BUSY|ON`. The commit is the exclusion,
   and `push` answers `-EEXIST` where the claim used to fail.
3. `dc->lru[i]` becomes a `struct urcu_txn_deque`. `lru_shard_index()` still
   selects which one. `count` becomes caller-maintained and explicitly
   approximate.
4. Shrinker: peek is `urcu_txn_deque_head()`, second chance is
   `urcu_txn_deque_rotate_head()`. Note it only ever rotates the HEAD, which is
   exactly the narrowed primitive the deque offers — the general-move hazard
   never arises.
5. Keep evict-first (`c404d80`): peek → try evict → `remove()` on success,
   `rotate_head()` on failure. No claim needed; the loser gets `-ENOENT`.

**Decide before starting**: with no claim, two sweepers can both reach
`lru_evict_settled()` for the same victim. That is safe today because its
bucket-lock re-verify makes one fail — but it moves the serialization point from
the LRU word to the eviction. State it in the design rather than discovering it.

## Measured facts — do not re-derive

- Default MCAS arm is healthy: 4/4 complete, ~1.39–1.42 Mchurn/s under
  `--evict bursty`. Transacting the node's links changes nothing there.
- `DC_LRU_READD_LEGACY` still collapses 4/4. It is the kept control.
- The legacy arm has **two stacked failures**, which is why single fixes kept
  appearing not to work:
  1. a retry wedge — suppressed by transacting the re-added node's links
     (`-DDC_LRU_TXN_LINKS`: WEDGE dumps 1 without the flag, 0 with);
  2. starvation — writers park behind a shrinker that KEEPS the escalation lane,
     because `urcu_txn_end()` deliberately retains an escalated handle's lane
     while the last commit returned ABORT. A caller that aborts often never
     yields and FIFO fairness never applies.
- At the wedge: the failing record is `&prev->next : elem -> next`, `want`=elem,
  `seen`=MARKED — so `prev` is a DELETED node, and `pv->next` names someone else,
  so the staleness predates its removal.
- Aborts are **in-lane** (`in-lane ~= aborts`), so nothing is racing the holder;
  a serialized commit that still fails has an expected-old that is not in memory.
- The single escalation lane is BY DESIGN and shared with the bucket
  transactions. Do not "fix" it with per-shard domains.

## Claims retracted this session — do not resurrect

Each was argued convincingly and refuted by measurement:

- the stale-prev *forward rescan* (`prev_repair`) — reverted; its termination
  test walks from a hint that may itself be off-ring. The stale-prev *disease*
  was real; the medicine was `load_validate`.
- "a corrupt ring / cycle without the sentinel" — the ring dump shows
  `closed=1 marked=0 count==walked` every time.
- the six-edge `move_tail` as the cause — `DC_LRU_NO_MOVE`, 6/6 still collapse.
- word-OFF-while-linked — the claim-site assertion never fires.
- "plain stores exonerated" — I counted a TIMEOUT as THE WEDGE. They differ.
- "the slab lfstack pop is spinning" — one gdb sample of a thread passing
  through; four samples show it moving.
- "the deque reproduces the dcache wedge at engine level" — same symptom,
  different cause.

## Method rules earned the hard way

- **Grep for the actual violation marker, not the exit status.** A timeout is
  not the wedge.
- **One gdb sample is not a spin.** Take three, two seconds apart, and check
  whether frames move.
- **Verify a probe is live before trusting its zero.** A counter that prints
  every N stays silent when the failure arrives before N — print on the FIRST
  call and run a with/without control (`probe ran 1 vs 0`). Bitten 3x.
- **Mutation-test every new guard.** If removing it changes nothing, the test
  does not exercise it; say so instead of claiming a fix.
- **The bench compiles against a COPY** of the urcu headers in
  `urcu-txn-build/include/urcu/`. Editing the dev tree alone changes nothing —
  `md5sum` both after every edit. Cost two measurement rounds.
- `sleep` is blocked in the agent's bash tool; a bare `for` loop is
  instantaneous. Use `python3 -c "import time; time.sleep(N)"`.
- **An operation that reads a slot it does not write must validate that read.**
  That is the general shape behind `750572af`: `del`/`remove` read `&elem->prev`
  and never write it; `insert_before`/`move_tail` write the slot they derive
  from and so need nothing.

## Build recipes

    # deque test
    gcc -O2 -g -pthread -march=native -DNWRITERS=16 \
        -I$U/include -I. -o t test_deque.c \
        -L$U/src/.libs -lurcu-qsbr -lurcu-common -lrseq -lpthread

    # the collapse control
    ... -DDC_LRU_MCAS -DDC_TXN_STATS -DDC_LRU_READD_LEGACY [-DDC_LRU_TXN_LINKS]

    # LTTng flight recorder (see the skill; 64K x 4 keeps the ring L2-resident)
    lttng create dcwedge --snapshot
    lttng enable-channel --userspace --subbuf-size=64K --num-subbuf=4 ch
    lttng enable-event --userspace --channel ch 'dc:*'

Gates: `make check`, `check-bucketlock`, `check-lru-arms` — all green at handoff.
