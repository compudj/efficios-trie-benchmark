# dcache lookup: the re-check pass, and whether it can compare tops — handoff

Written 2026-10-06 on `thinkos`, from a session that started on the LPC deck
and turned into a design discussion with Mathieu about how the model's lookup
survives a concurrent directory move. For the session on the test machine.
Self-contained: what was read, what was said, and what was only reasoned are
kept apart below.

**No benchmark was run for this file, and this file authorizes none.** Nothing
was built either. Every measurement in §8 needs Mathieu's explicit go-ahead.

Trees, as read:

- This tree at `a16d192`. `dcache_bucketlock.c` and
  `rename-shell-transition.md` are unchanged since `253d8e4`, the commit the
  LPC deck's sweep (`3225b8ec0e79-2793224e`) ran on, so every line number
  below holds for both.
- liburcu, branch `ft-txn-integ` at `9484d87ed` (same as
  `github-dev/ft-txn-integ`). On `thinkos` the checkout is
  `~/doc/userspace-rcu`; Mathieu names it `~/git/userspace-rcu`, presumably
  the test machine's path. Check before reading.

## 1. Where the thread stopped

Mathieu's last two messages, verbatim:

> two descents is ONE way to achieve it. why not upwalk through parent ?

> the idea here would be to tweak our upward 2nd pass and compare the _top_
> shell for each dentry node.

The first was answered (§5, §6). The second was answered only in summary,
after this file was first written: §7 holds what was found, and he has the
four points of §7.1–7.4 in short form. He then closed one branch himself:

> I don't think this change to validate the top address between 2 passes vs
> validating the mark would help much in terms of wait-freedom guarantee: a
> sequence of exchange swapping two sets of directories both being matches
> for a reader could technically trigger an endless retry with both schemes

Agreed with him (§7.5). What the tweak is for, if not a bound, he has not
said (§7.4).

Decisions he did make in this thread:

- The deck needs no change for any of this ("it's ok, I'll just keep that in
  mind"). The talk is 2026-10-06, 10:00, Prague.
- The trie's mode gate is ruled out for the dcache: "I don't want directory
  renames and moves to be that expensive" (§6).

## 2. What `dc_lookup()` does today (read in the code)

`dcache_bucketlock.c:1643`. The bucket lock engine is `DC_MARK_GEN` only (an
`#error` enforces it, line 1567), so the per-node loop at 1719–1769 is the
only reader.

- **Descent, per component.** Find the named node in the hash bucket
  (`find_top_raw_rcu()`), resolve its content host, then test the deletion
  mark on its `d_hash.next` (`top_unhashed_rcu()`, line 1741). Marked: re-walk
  from the root. Otherwise store the node in an on-stack array
  (`hosts[DC_PATH_MAX]`, line 1649) and continue under the host.
- **Re-check pass.** After the leaf, re-test the mark on every stored node
  (lines 1761–1765). Any marked: re-walk from the root.
- **Retry.** A bare `for (;;)`. No bound, no fallback to a lock.
  `rcu_read_lock()` is outside the loop (1654, unlock at 1771), so one
  read-side section spans every retry.
- **Not found.** Only when a component is missing from its bucket (1734), and
  the prefix stored so far still goes through the re-check.

The node stored per component is the **named top**: the node in the name
index, a shell while a rename is outstanding, the content host once settled.
The stamp is literally its address (`dc_stamp_of()` returns `(uintptr_t) top`,
line 1611), and `dc_stamp_reread()` (line 1636) returns that address if the
node is still unmarked, 0 otherwise.

What a rename does: `stack_shell()` allocates a fresh shell (line 2528), and
one SW commit removes the old top from both indexes with the mark and inserts
the shell (`stack_one_prepare()`, lines 2442–2455). The content host keeps its
address; its children key on it.

The case this guards is `repro_dcache.c`: a walker resolving `/A/B/K` has
passed `B`; the writer renames `/A/B` to `/Z/B` and adds `/Z/B/K`; without the
re-check the walker returns `K` for a path that never existed.

## 3. Not wait-free (confirmed with Mathieu)

The loop has no bound: a stream of renames, unlinks or folds on a reader's own
path restarts it indefinitely. The reader takes no lock and never waits on a
writer, and it restarts only when a node on its own path is marked. A reader
stuck retrying holds off the grace period for that whole time.

Said to Mathieu and **not checked against kernel source**: the kernel's RCU
walk drops to ref-walk on a `d_seq` mismatch and `d_lookup()` loops on
`rename_lock`, so the baseline is not wait-free either.

Deck wording this touches, unchanged at his word: slide 15 "no seqcount, no
retry" and backup slide 31 "Here a reader never retries" are about the
facility's readers; slide 26 says "No `d_seq`, no `rename_lock`", and its
notes already say the whole-path consistency is "the embedder's re-check of a
deletion mark, not the facility's".

## 4. Why a mark means re-walk and not ENOENT

Mathieu asked whether finding a mark proves the path was absent at some
instant of the read-side section, so the lookup could return ENOENT at once.

The mark is one bit, set on whatever node stops being the indexed one for a
name. Four commits set it (read in the code):

| Commit | The name resolves to, afterwards | ENOENT on mark |
|---|---|---|
| unlink | nothing | sound |
| rename, source name (`stack_one_prepare()`, 2442) | nothing | sound |
| fold (`fold()`, 2742) | the same entry: shell replaced in place by its host | wrong |
| exchange (`dc_rename_exchange()`, 2995) | the other entry | wrong |

For the first two the argument holds (session's reasoning): take the earliest
mark among the stored nodes; at that commit every node above it was still
indexed and that component left.

The fold breaks it and is not rare: every rename is followed by one (the deck
says about 5 ms later). A lookup of the new name that stored the shell finds
it marked after the fold, yet the name resolved to the same object for that
reader's whole section: the fold is a `call_rcu` callback (`fold_cb()`, 2772),
so any reader it hits started after the rename committed. The design note
says the same of the cost: "a fold landing in a reader's window costs a
spurious re-walk" (`rename-shell-transition.md`, "The mechanism: the deletion
mark is the version", line 542 on).

Session's reasoning, not in the tree, not tested: a mark that recorded why the
node left would let "left" marks return ENOENT; "replaced" marks would still
re-walk. Fewer retries, not wait-freedom.

## 5. Two passes: which comparison is wrong, which is right

Mathieu recalled a scheme of two consecutive lookups comparing the addresses
of the live nodes, and asked why it was not correct, or whether the mark was
only an optimization.

- **Wrong** (`rename-shell-transition.md` line 420, "The versioned
  double-collect is the sound form"): walking `d_parent` back up and checking
  the two passes agree. A content host keeps its address across a rename, so
  a move away and back passes the comparison; and a same-directory rename
  does not change `d_parent`.
- **Right**: comparing the indexed node per component, since a rename always
  publishes a fresh one. That is what the mark re-check decides, without a
  second bucket scan. Against a literal second lookup the mark is an
  optimization. That the two give the same verdict is the session's
  reasoning; nothing in the tree tests a literal two-lookup form, and none
  was ever built here.
- The one way an address returns: the fold puts the host back in the index.
  It is grace-period gated and the reader holds the read lock across both
  passes (the note argues this at line 625 on, for the retired skip arm).

History in the note: global `rename_gen` bracket (line 339), then a per-host
counter `d_seq` (line 382, `DC_PER_NODE_GEN`), then the mark (line 491,
`DC_MARK_GEN`, commit `8a29609`), which retired the counter: 8 bytes back on
the hot line, nothing added to the rename commit.

## 6. The Fractal Trie does use two descents (read in the code)

His recollection matches the trie, not this model.
`src/fractal-trie/ft-lookup.h:91`, `ft_lookup_two_descents()`:

- **Gate.** A mover sets `move_active`, waits a grace period, then mutates
  (`fractal-trie-internal.h:2528` on). A reader that sampled 0 runs the
  ordinary single descent (`ft-lookup.h:136`). Mathieu: "FT uses a two-mode
  flip so readers skip the overhead typically, at the cost of a rcu
  synchronize to flip the mode".
- **In a move window.** Two descents from the root. Each folds the address of
  every visited node into an order-dependent 64-bit hash plus a count
  (`struct ft_visit_witness`, `fractal-trie-internal.h:4356`). Accepted if
  status, result node and witness agree; otherwise both re-run. The comment
  says lock-free, not wait-free, inside a move window.
- **Why sound** (comment at `fractal-trie-internal.h:4346`): a move copies its
  stitch points, so they get fresh addresses, and RCU cannot recycle a node
  inside a read section.
- **Opt-in.** `cds_ft_group_attr_set_rekey()`, default off; a trie that never
  rekeys carries no coherent lookup at all. An external-sync trie that opts
  in is always-coherent: `ft_move_active()` returns true outright, every
  exact lookup runs the second walk, and the gate owes no grace period
  (`doc/design/ft-lockset-inventory.md`, line 1591 on).

**The up-walk came first in the trie.** Commit `39d5fc9c4` (2026-07-26)
replaced an up-walk key witness with the two descents. Its message calls the
up-walk sound for a hit and gives three reasons: it was hit-only (a miss has
no leaf to walk up from, so a torn descent gave a false miss with nothing to
check); it needed the ordered-list cell as its source; it read the metadata
cache line a forward descent does not touch.

Read-side cost, from the code, **not measured**:

| | Trie, two descents | This model, mark |
|---|---|---|
| No move in flight | one load of `move_active`, one branch | per component: a mark test on a word already loaded, a store to the stack, then one re-read |
| Move in flight | a second descent over lines just loaded, three multiplies per visited node | the same; there is no mode |
| Who is affected | every reader of that trie, for the window | lookups whose path holds a marked node |
| Writer | one grace period on the 0→1 transition, shared by a burst | nothing added to the rename commit |

Mathieu first said "it may have slightly more read-side overhead, we should
characterize that first", then ruled the gate out for the dcache on the
writer's cost. Without the gate every lookup would walk twice.

## 7. Comparing the top on the upward pass (said to him in summary only)

### 7.1 The re-check already compares the top

The stored node is the top and the stamp is its address (§2). "Is this still
the top for its name" is what the mark test answers. The tweak would change
how the current top is obtained, not what is compared.

### 7.2 The host already points at its top, and the reverse walk uses it

`d_top` (struct comment at lines 264–292, field at 294): on a host, the shell
that currently names it, or NULL when the host names itself. A stack or an
exchange moves it in the same commit as the index change; a fold resets it.
`named_top_rcu()` (line 3258) reads it.

`dc_dentry_path()` (line 3272), the `dentry_path_raw()` port, is an upward
walk through the parent that goes host → top (`d_top`) → `DC_IPARENT(top)` →
parent host, stores each top, and validates with the same mark re-test
(3299–3320). So an upward walk over tops exists, for the reverse direction.

### 7.3 The version of this that compares the host→top pointer was built and retired

`DC_IPARENT_SKIP` (`rename-shell-transition.md` lines 571–753; retired by
commit `571e420`). The host's `d_iparent` carried a direct pointer to the
current top and the reader compared that word across its two passes. The
note's verdict:

- Throughput within noise of the mark on every panel (table at line 688 on).
- 8 more bytes per dentry (`d_origiparent`), for the same name budget.
- Overwriting the host's `d_iparent` broke the match: a reader past the
  bucket head saw neither the old nor the new node and reported ABSENT
  ("The write-once trap"). Repairing it took `d_origiparent`.
- The value can revert (stack then fold), so soundness needed the
  grace-period argument; the mark needs none.
- "Its one unique asset was O(1) host→top navigation, which nothing in the
  reader used."

One sentence of that section matters for §7.4: a fold hitting a reader that
entered after the rename is sound to accept there, because "TRANSFER is
identity-preserving, so the path stays valid and accepting is correct"
(line 650 on).

### 7.4 What the tweak could buy, and what it would cost (session's reasoning)

- **Cost.** With `d_top` as it is, the comparison reads CL1 of each host, the
  writer's line; the forward lookup today stays on CL0 for a settled
  component (layout at line 246: `d_iparent` 8, `d_iname` 48, `d_hash.next`
  8). Moving the pointer onto CL0 is what the skip arm did.
- **What the mark cannot give and a top pointer can:** on a mismatch the
  reader learns which node replaced the one it stored. That would tell a fold
  (same entry, same name) from a move.
- **Fold.** Accepting across a fold without a re-walk looks possible: the
  binding from name to host is continuous across it. The reader would have
  to take the host as the new top and test its mark. Not worked through for
  a rename or an exchange that follows the fold inside the same section.
- **ENOENT.** "This entry left this name" is not "this name is absent": an
  exchange puts the other entry at the name in the same commit. The host's
  top pointer does not show that.
- **Wait-freedom.** None of it bounds the lookup: §7.5.

Which of the first three he is after is the first thing to ask.

### 7.5 No bound from either scheme (Mathieu's point, agreed)

Both schemes decide the same predicate, "is the node stored for this
component still the top for its name". Every exchange allocates two fresh
shells (lines 2885–2886) and replaces both old tops in place, which marks
them (2995–3006). So each committed exchange on a reader's path invalidates
whichever pass straddles it, whatever word is compared, and a stream of
exchanges between two directories that both hold the rest of the path
restarts the reader without end.

The exchange is also where accepting in place cannot work (§7.4, fold): after
it the name resolves to the other directory, so the rest of the path has to
be resolved again under that one, and the next exchange can land meanwhile.

Session's reasoning, not in the tree: a bound would have to come from a
different mechanism, such as capping the passes and then excluding movers for
one pass. That is blocking, so starvation-free at best, not wait-free. From
memory and unchecked: kernel seqlock readers do this with
`read_seqbegin_or_lock()`.

## 8. Open

1. What the tweak is for: no spurious re-walk on a fold, ENOENT without a
   re-walk, or something else. Not a bounded lookup: he ruled that out
   himself (§7.5).
2. Whether a top replaced by a fold can be accepted in place (§7.4), with the
   exchange and rename-after-fold cases argued, and a deterministic repro in
   the style of `repro_dcache.c` before any claim.
3. Any cost claim about reading `d_top` on the forward path is unmeasured.
   The skip arm's numbers are the nearest evidence and they are noise-level.
4. The trie's two-descent cost has no number anywhere: nothing in its notes,
   and no benchmark in either tree exercises it (this tree does not mention
   rekey). A three-way lookup throughput comparison would isolate it: rekey
   not opted in; opted in with the gate idle; always-coherent, which needs no
   mover. Mathieu has not asked for it since ruling the gate out.

## 9. Standing rules

- No benchmark, sweep or heavy build without Mathieu's explicit go-ahead.
- Commits carry Mathieu's `Signed-off-by`, with the address the tree's own
  history uses, and no `Co-Authored-By`.
- Claims about other systems name the design point, not the family.
- Say what was read in the code, what a note says, and what is only reasoned,
  separately. He checks.

## 10. After this file: the walk hold-off (2026-10-06, test machine)

Sections 1 and 8 stop at "what is the tweak for". The thread moved on the same
day, on the test machine, to bounding the lookup instead. **Built and tested;
no benchmark had been run when this section was written.**

### 10.1 What Mathieu decided

The direction, verbatim:

> I think our current conclusion is to try a "lock writers after a few retry"
> in the reader [...] to at least become starvation free

Two designs from the session were turned down before any of it was kept: a
gate that waits a grace period to drain the writers, and a locked pass that
takes the bucket locks along the path. His design, verbatim:

> the way I envision it: the reader would never have to wait for a grace
> period, and could increment a global "reader exclude writers reference
> count" to temporarily prevent a steady stream of writers from making the
> reader retry. the reader always need to be checking the mark on 2nd pass,
> but at least it prevents a steady-flow of mutations

Then, on three questions: the count is **global** (not per node), a held-off
writer **sleeps** (does not spin), and the whole thing is **opt-in**. Bucket
lock engine only.

### 10.2 What was built

`-DDC_WALK_HOLDOFF=N` in `dcache_bucketlock.c`; the comment above
`walk_hold_raise()` is the description of record.

- **Reader.** `dc_lookup()` counts its passes. Entering pass N + 1 it
  increments `dc->walk_hold`, and decrements it on the way out. The loop and
  the mark re-check are unchanged.
- **Writers.** The five commits that set a mark load the count first.
  Rename, exchange and unlink sleep at their entry, offline, on a futex
  (`walk_hold_enter()`); the fold re-queues itself through `call_rcu`; the
  shrinker's `lru_evict_settled()` skips its victim.
- **Without the flag** the engine's code is byte-identical to `f930aec`
  (`.text` and `.text.unlikely` compared, bench flags).

Also new: `repro_walk_holdoff.c` and the `check-walk-holdoff` target.

### 10.3 What was run (2026-10-06, machine idle, load 0.03)

`make check-walk-holdoff`, all passing:

- The deterministic repro, with Mathieu's own scenario (`/A/B` and `/A/C`
  exchanged, both holding `K`). N=3: 5 passes. N=2: 4 passes. That is N
  failing passes, one failed by a commit that was past its check when the
  count rose, and one that stands. Control (same source, no flag): 65 passes
  for 64 exchanges.
- In the same repro a shrinker sweeping while the count is raised evicts 0,
  with 4 attempts counted at the skip; the same sweep evicts 2 afterwards.
- The single-thread suite and the three existing repros, built with N=1.
- Four concurrent harnesses under ASan at N=1. Slow paths reached, per run:

  | Harness | Raises | Writer sleeps | Fold re-queues |
  |---|---|---|---|
  | `stress_dcache` | 8 | 2 | 0 |
  | `stress_dcache_dirs` | 1001 | 620 | 243 |
  | `stress_dcache_xchg` | 371 | 71 | 131 |
  | `mixed_cycle` | 6082 | 2941 | 1997 |

Two mutations of the engine, each making the repro fail as it must: the
eviction skip removed (the shrinker evicts the walker's leaf), and the
exchange's entry check removed.

Not run: any benchmark, TSAN, the churn harness (it is a benchmark binary).

### 10.4 Reasoned only, not shown

- **The bound.** A thread has at most one commit past its check when the
  count rises, so with W threads able to mark, at most W more marks land and
  the lookup returns within N + W + 1 passes, without waiting on anyone. The
  repro shows the three kinds of pass, one in-flight commit included; it does
  not exercise the bound itself. If it holds, the lookup is wait-free with a bound that
  depends on the number of marking threads, which is stronger than what was
  asked for. Mathieu has not reviewed this argument.
- **Cost.** A marking commit loads one more global line; a lookup carries one
  more counter. Neither is measured.

### 10.5 Open

1. No number exists for the cost, or for how often a real workload reaches
   the threshold. Needs Mathieu's go-ahead (§9).
2. The stress harnesses run no shrinker, so the eviction skip is covered by
   the repro only.
3. Under the flag, rename, exchange and unlink can pass through a quiescent
   state at their entry. The harnesses of §10.3 tolerate it; a caller
   holding its own RCU-protected reference across them would not.
4. A held-off writer is not promised a turn (the count can rise again before
   it looks). Readers get priority; one hot path under a mutation stream
   throttles every writer of the cache. Per-directory counts were discussed
   as the refinement and not built.
5. `dc_dentry_path()` has the same unbounded loop and is untouched.
6. **Found while reading, independent of the hold-off, not reproduced.**
   `stack_shell()` and `dc_rename_exchange()` call `urcu_txn_conflict()` on
   their retry paths. At 64 retries of one operation the next
   `urcu_txn_begin()` takes the escalation lane and parks for it with
   `rcu_thread_offline()` (`urcu_txn__enter_fallback()`, `rcu-txn.h`). That
   is a quiescent state in the middle of a rename that still holds
   `from_parent` and `new_parent`, resolved before its loop, and goes on to
   lock their child heads. `struct dcache` calls the domain "vestigial"; the
   `conflict()` calls still reach it. Passing a NULL domain, as `fold()`
   does, would remove the lane, and would change the measured build.
