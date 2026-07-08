# perfbook/ — McKenney's "existence structure" approach (vendored)

Source datastruct code from Paul E. McKenney's [*Is Parallel Programming
Hard, And, If So, What Can You Do About
It?*](https://mirrors.edge.kernel.org/pub/linux/kernel/people/paulmck/perfbook/perfbook.html)
("perfbook"), vendored here so we can compare **urcu-txn** (rcu-mcas /
transactional multi-word CAS) against McKenney's **existence-based** technique
for atomically moving an item between data structures.

See [`PROVENANCE.txt`](PROVENANCE.txt) for the exact upstream commit, what was
copied vs. dropped, and the one local patch. Licensed GPL-2.0
([`COPYING`](COPYING)); original copyright Paul E. McKenney et al.

## Where the "existence" approach lives

- **`datastruct/existence/`** — the existence structure itself
  (`existence.h`), plus its use to atomically move a key between three hash
  tables (`existence_3hash_*`) or three skiplists (`existence_3skiplist_*`),
  and the **kaleidoscope** generalization (`kaleidoscope*`) that rotates an
  item through N structures in one linearizable step. `procon.h` is the
  producer/consumer memory pool the update side leans on.
- **`datastruct/Issaquah/`** — the Issaquah relativistic-tree existence
  structure (`existence.c` + `tree.c`), the larger worked example.
- `datastruct/hash/`, `datastruct/skiplist/`, `datastruct/log/` — the
  supporting structures the existence tests `#include` and build against.

This is the analogue of what urcu-txn does with a single MCAS across two
structures (cf. the repo's `design/` notes on txn-based cross-table moves and
rehashing) — the point of interest for the comparison.

## Building

Everything is wired to build in-tree (the external `CodeSamples/` closure —
`api.h`, `Makefile.arch`, `depends.mk`/`recipes.mk`, `arch-x86/`,
`api-pthreads/`, `lib/`, `defer/` — is vendored alongside `datastruct/`).
Needs a system `liburcu` and a C toolchain.

```sh
cd datastruct/existence && make    # 10 targets (existence_*, kaleidoscope_*, procon_test)
cd datastruct/Issaquah  && make    # existence_test, treetorture
```

The `*_test` targets are functional/torture tests; the `*_uperf` targets are
update-scalability microbenchmarks. The perf targets default to glibc malloc;
opt into a scalable allocator for the scaling runs:

```sh
cd datastruct/existence && make MALLOC=-ljemalloc existence_3hash_uperf
```
