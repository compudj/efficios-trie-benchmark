# efficios-trie-benchmark

Benchmarks for the **Fractal Trie (FT)** against competing trie / ordered-map
implementations:

| Engine     | Implementation                                  | Source                          |
|------------|-------------------------------------------------|---------------------------------|
| `ft_eager` | Fractal Trie, eager attr + eager lookup         | our liburcu clone (`urcu-build/`)|
| `ft_eager_on_spec` | Fractal Trie, eager lookup on speculative trie | our liburcu clone        |
| `ft_cand`  | Fractal Trie, pure candidate lookup (no memcmp) | our liburcu clone               |
| `ft_spec`  | Fractal Trie, speculative lookup (lib-side memcmp) | our liburcu clone            |
| `qp`       | qp-trie (quadbit popcount), Tony Finch          | `third_party/qp-trie` (vendored)|
| `art`      | Adaptive Radix Tree (libart), Armon Dadgar      | `third_party/libart` (vendored) |
| `hot`      | Height Optimized Trie (Binna et al., SIGMOD'18) | `third_party/hot` (ISC, vendored) |
| `cuckoo`   | Cuckoo Trie (Zeitak & Morrison, SOSP'21)        | `third_party/cuckoo-trie` (Unlicense) |
| `judy` / `judyl` / `judysl` / `judyhs` | Judy — combined, JudyL (int), JudySL (string radix), JudyHS (hash) | system `libJudy` |
| `masstree` | Masstree, B+tree-of-tries (Mao/Kohler/Morris)   | `third_party/masstree` (MIT); ST + MT |
| `artolc`   | ART-OLC, concurrent ART OLC (Leis et al.)       | `third_party/artolc` (Apache-2.0); ST + MT |
| BIND9 QP   | `dns_qpmulti` (multithreaded test only)         | our bind9 clone (`bind9-src/`)  |

## Dependency model (hybrid)

The competitors that are *stable* are vendored into this repo (`third_party/`).
The Fractal Trie itself lives in **userspace-rcu** and is under active
development, so it is **not** vendored. Instead `urcu-build/` is our own git
**clone** of userspace-rcu checked out on the `fractal-trie-dev` branch and
built in-tree. `make urcu` clones it (or fetches + fast-forwards the branch)
from `$(URCU_UPSTREAM)` and rebuilds, so we track the live FT while pinning to a
consistent committed state we control. The upstream source tree is never
modified. `bind9-src/` is similarly our own clone of bind9 (see below).

## Building

Edit `config.mk` if your paths differ (defaults assume userspace-rcu at
`/home/efficios/git/userspace-rcu` and bind9 under
`/home/efficios/files/fractal-trie/...`), then:

```sh
make urcu     # clone (or fetch) our FT checkout + build liburcu in-tree
make          # build the single-threaded benchmark
```

Requires `libjudy-dev` (`<Judy.h>` / `-lJudy`); the bind9 MT build additionally
needs meson, ninja, and bind9's build deps (libuv, openssl, …).

Re-run `make urcu` to pull the latest `fractal-trie-dev` and rebuild the FT.

## Single-threaded benchmark — `bench_one_st`

Single dataset, single engine, run in its own process for accurate RSS:

```sh
./bench_one_st <dataset> <engine>
#   dataset: u32d u32s u64d u64s dns dict paths   (all generated synthetically)
#   engine:  ft_eager ft_eager_on_spec ft_cand ft_spec judy judyl judysl judyhs
#            qp art hot cuckoo masstree artolc
# output: <ns/op> <RSS_kB>   ('-' where an engine does not apply: judysl on
#         integers, judyl on strings)
```

Example sweep (all engines that apply, on one dataset):

```sh
for e in ft_cand ft_spec judyl judysl judyhs qp art hot cuckoo masstree artolc; do \
  printf '%-10s ' "$e"; ./bench_one_st dns "$e"; done
```

Useful env vars (see `src/bench_one_st.c`): `FT_BENCH_COMPACT` (compact between
build and query), `FT_DUMP_STATS`, `N_KEYS` / `WARMUP` / `RUNS` (compile-time).

### Results across datasets (1M keys, single thread)

Lookup time, ns/op (best of `RUNS` timed passes after `WARMUP`), on the hardware
below (2× EPYC 9654; `cuckoo` run with reserved 2 MiB hugepages, `-O3 -flto`).
Every engine now runs on every dataset — the byte-keyed engines key integers as
big-endian bytes; `judysl` (string radix) and `judyl` (integer array) are the
two split-out Judy variants, `judyhs` is Judy's hash array.

**String keys** (`dns` DNS names, `dict` words, `paths` filesystem paths),
fastest-first by `dns`:

| Engine     | `dns` | `dict` | `paths` |
|------------|------:|-------:|--------:|
| `hot`      |    98 |    109 |      77 |
| `ft_cand`  |   114 |    106 |     143 |
| `qp`       |   119 |    129 |     180 |
| `ft_spec`  |   120 |    111 |     142 |
| `judyhs`   |   145 |    120 |     142 |
| `judysl`   |   201 |    259 |     264 |
| `art`      |   209 |    181 |     231 |
| `artolc`   |   214 |    208 |     249 |
| `masstree` |   241 |    196 |     210 |
| `cuckoo`   |   337 |    313 |     333 |

(`wormhole`, the separate GPL binary, is ~118 ns on `dns`.)

**Integer keys** (`u32/u64` × `d`ense sequential / `s`parse random),
fastest-first by `u64d`:

| Engine     | `u32d` | `u32s` | `u64d` | `u64s` |
|------------|-------:|-------:|-------:|-------:|
| `judyl`    |     11 |     37 |     11 |     65 |
| `qp`       |     12 |     12 |     13 |     13 |
| `ft_cand`  |     15 |     32 |     15 |     32 |
| `art`      |     15 |     49 |     17 |     54 |
| `ft_spec`  |     16 |     36 |     17 |     36 |
| `hot`      |     20 |     49 |     20 |     50 |
| `artolc`   |     22 |     97 |     23 |    100 |
| `judyhs`   |     24 |     46 |     38 |     79 |
| `masstree` |     48 |    171 |     46 |    169 |
| `cuckoo`   |     95 |     98 |    118 |     98 |

Takeaways:
- **`qp` is uniquely distribution-insensitive on integers** — ~12–13 ns on *all
  four* sets, including the sparse random ones where everything else degrades 2–6×
  (`judyl` 11→65, `ft` 15→36, `art` 17→54). Its bit-popcount nodes don't care
  whether keys cluster.
- **`judyl` wins dense integers** (11 ns) but collapses on sparse (65); **`judyhs`
  beats `judysl` on strings** (hash suits these distributions better than the
  radix tree), and **`hot` has the fastest string lookups** (77–109 ns).
- **`cuckoo` is slowest throughout** (it hashes whole keys, no prefix sharing);
  **Masstree and ART-OLC carry their concurrency machinery** even single-threaded,
  so they trail the dedicated ST engines (ART-OLC the closer of the two).
- Single snapshot, best-of-`RUNS` (figures move a few % run to run).

> **Validation fairness.** Every engine stores its own **copy** of each key
> (FT/qp/ART/Masstree/ART-OLC in a dense `cds_ft_external_arena`; Judy/Cuckoo
> internally; HOT in the same arena) and the timed loop consumes the lookup
> status and force-reads the returned leaf (`FORCE_READ_LEAF`), so each pays a
> real validating compare against cold memory and no validation is dead-code-
> eliminated. HOT (integer) uses map-mode (value = a pointer to the key record),
> not its cheaper set-mode, so it touches a cold value like the others.

> **Validation fairness (why these differ from earlier numbers).** Every engine
> here stores its own **copy** of each key (FT/qp/ART in a dense
> `cds_ft_external_arena`; Judy/Cuckoo/Wormhole internally) and the timed loop
> both consumes the lookup status and force-reads the returned leaf
> (`FORCE_READ_LEAF`), so each pays a real validating compare against cold
> memory and no validation is dead-code-eliminated. HOT originally stored a
> pointer straight into the shared query buffer, so its `contentEquals`
> validated against already-hot memory — near-free, and ~71 MB because it kept
> no copies. Putting HOT on the same key-copy arena as the others is what moves
> it to ~100 ns / 110 MB (the lookup cost barely changes; the memory does).

### The qp-trie `qp` vs `fn` gotcha

qp-trie dispatches through `Tbl.o`, which links with **exactly one** backend
object (`qp.o`, `fn.o`, ...) — all export the same symbols, so the linker takes
whichever you pass. We vendor and link **only `qp.o`**, so the `qp` engine is
always the real qp-trie. (The original `build_one_st_bench.sh` linked `fn.o`,
silently benchmarking a different structure under the `qp` label.)

## Multithreaded benchmark — bind9 `load-names`

The MT scaling test is bind9's "lookup names" benchmark (`load-names.c`), which
compares the Fractal Trie against BIND9's own QP-trie (`qp_il`, `qp_local`),
HOT's concurrent ROWEX trie (`hotrowex`), Masstree (`masstree`), and ART-OLC
(`artolc`) under a lookup-scaling thread sweep
(cache priming on by default — set `BENCH_NO_PRIME` to skip; `BENCH_ENGINE=<name>`
runs one engine). The FT engines form the 2×2 of build attr × lookup — `ft_eager`
(EAGER attr + eager lookup), `ft_spec` (SPEC attr + speculative lookup), and the
crosses `ft_eager_on_spec` / `ft_spec_on_eager` — plus `ft_cand` (pure candidate,
no memcmp); each with `_il` / `_local` leaf-arena placement (and `ft_spec` also
`_extarena`), matching the engine naming used by `bench_one_st`. It builds *inside* bind9, so we
clone a clean upstream bind9 at a pinned commit and overlay our `tests/bench`
files (in `bind9-overlay/`), linking our own liburcu:

```sh
make urcu      # if not already built
make bind9     # clone bind9 @ pinned commit, apply overlay, build the benches
```

Run it (note the `LD_LIBRARY_PATH` — see below):

```sh
LD_LIBRARY_PATH=urcu-build/src/.libs \
  bind9-src/build/tests/bench/load-names datasets/names-1M-shuf.csv
```

`load-names` takes the CSV as its single argument and, by default, sweeps the
thread counts `1 2 4 8 16 32 64 96 128 192`, pinning worker `i` to CPU `i`
(distinct physical cores up to 192 on a 2×96-core EPYC). `BENCH_THREADS=N`
restricts the sweep to a **single** thread count `N` (it is parsed as one
integer, not a list — handy with a high `QUERY_LOOPS` for clean perf-stat
runs). Cache priming is **on by default** for **every** engine (an untimed warm
pass before the timed window, applied identically to FT and qp so comparisons
are fair); set `BENCH_NO_PRIME=1` to measure cold-start instead. Other env
vars: `QUERY_LOOPS`, `BENCH_ENGINE` (filter), `BENCH_CACHE_FLUSH_MB`,
`FT_BENCH_CHURN`, `FT_BENCH_COMPACT`.

**Why `LD_LIBRARY_PATH`:** bind9's own libraries link the system `liburcu-cds`
(found via pkg-config) and pull it in transitively; without our build's `.libs`
first on the library path, an older system `liburcu-cds` is loaded and the
newest FT symbols are missing. `make bind9` prints the exact command to use.

### Result — FT vs BIND9 QP-trie at 192 cores

Lookup throughput on 1M DNS names (`datasets/names-1M-shuf.csv`), comparing the
Fractal Trie reference engine `ft_spec_il` (speculative descent + library-side
memcmp validation) against BIND9's `dns_qpmulti` (`qp_il`), apples-to-apples:
both use the same NUMA-interleaved (`il`) leaf/payload placement, and **both are
cache-primed** (priming is on by default for every engine — see above).

| Engine        | Query throughput @ 192 cores | vs BIND9-QP |
|---------------|------------------------------|-------------|
| `ft_spec_il`  | **≈ 1246 Mops/s** (1227–1273) | **≈ 1.3×** |
| `qp_il`       | ≈ 938 Mops/s (804–976)        | 1×          |

Median of 5 runs (min–max in parentheses), `QUERY_LOOPS=1`, priming on. Across
the thread sweep the FT lead is a steady **~1.1–1.3×** (≈ 1.2× at 1 thread,
≈ 1.3× at 192) — FT scales a bit better at the top because its RCU read path
dirties no shared memory while BIND9-QP's read path write-shares, but the gap is
modest. `qp_il` and `qp_local` are close at this scale with high run-to-run
variance; medians slightly favor `qp_il`.

> Note: cache priming must be applied to *all* engines or the comparison is
> badly skewed — with priming on FT only (the old `FT_PRIME` default), `qp_il`
> measured ~405 Mops/s cold vs ~938 warm, inflating the FT lead to a spurious
> ~3.1×. Always compare warm-vs-warm (or cold-vs-cold).

**Hardware:** 2× AMD EPYC 9654 (Zen 4 "Genoa"), 96 cores/socket = **192
physical cores**, SMT2 = 384 logical CPUs, 2 sockets, 24 NUMA nodes. The
benchmark pins worker `i` to CPU `i` (CPUs 0–191 = one thread per physical
core), so the 192-thread point runs one worker per physical core (private
L1/L2/FPU, no SMT-sibling contention).

### Result — FT spec vs HOTRowex vs Masstree vs ART-OLC on this workload

HOT's concurrent ROWEX trie (`hotrowex`), Masstree (`masstree`), and ART-OLC
(`artolc`) are all wired into load-names, so the same read-only,
sequential-access, real-names sweep compares them against `ft_spec_il` on equal
footing — all validate every lookup and store key copies in a NUMA-interleaved
arena (HOT keys on a NUL-terminated qpkey copy; Masstree on the binary qpkey
bytes; ART-OLC on a `\0`-terminated qpkey, since ART needs byte-prefix-free
keys). Median of 4 runs, query Mops/s:

| Threads | `ft_spec_il` | `hotrowex` | `artolc` | `masstree` |
|--------:|-------------:|-----------:|---------:|-----------:|
| 64      | 317          | **355**    | 208      | 166        |
| 128     | **758**      | 719        | 428      | 262        |
| 192     | **1212**     | 1021       | 685      | 267        |

**FT-spec and HOTRowex cross over at ~128 threads** (robust across reps):
HOTRowex wins at lower core counts, FT-spec scales better and leads ~19% at 192.
This is the **inverse** of the random-access read/write `bench_scale` result
(where HOTRowex leads at 192) — load-names does *sequential* lookups
(prefetch-friendly) on real qpkeys with FT's leaf slots round-robin
**interleaved** across NUMA nodes. ART-OLC scales smoothly (208 → 428 → 685) but
trails the two radix tries, landing third. Across the full thread sweep its gap
to `ft_spec_il` is a **steady ~1.5× constant factor**, not a scaling defect: it
is already ~1.4× behind single-threaded (≈ 258 ns/op vs FT's ≈ 186 ns — ART's
radix descent + the `loadKey` validation read + constructing an ART `Key`, a
128-byte stack object, per lookup), and from 1 → 192 threads it scales ~183×
(~95% parallel efficiency), the **cleanest scaler** of the competitors here.

**Masstree does not scale here** — it plateaus at ~128 (~262) and does not climb
to 192 (~267), while the other three keep going. ART-OLC is the telling control:
it *also* uses optimistic, version-validated reads (and also first-touch nodes),
yet it scales cleanly to 192. So Masstree's stall is **not** optimistic
concurrency per se, but something specific to its wide B+tree-of-tries — heavy
version-counter contention on the shared upper nodes under correlated sequential
descent once readers span both sockets. (On the random-access `bench_scale`
sweep Masstree did scale, ~330 @ 192, so the stall is workload-specific.)

> **Caveat:** the HOT, Masstree, and ART-OLC *internal nodes* are first-touched
> by their building thread (none exposes an allocator hook, so unlike FT's leaf
> arena they cannot be `mbind`-interleaved). The key copies they validate against
> *are* interleaved, but the node placement is not — plausibly part of HOTRowex's
> gap behind `ft_spec_il` at 192. That ART-OLC (also first-touch) still scales is
> further evidence it is not the main factor in Masstree's stall.

## Multithreaded benchmark — read/write scaling (per engine)

This test runs **one writer** doing continuous insert/remove churn while **N
reader** threads look up keys, comparing the Fractal Trie against Judy,
qp-trie, ART, BIND9's QP-trie, HOT's concurrent ROWEX trie, and Masstree. So
each structure's resident-set size (RSS) can be measured in isolation, **each
engine is its own executable** — one process holds exactly one trie:

| Executable          | Engine                          | Links             |
|---------------------|---------------------------------|-------------------|
| `bench_scale_ft`    | Fractal Trie (`ft_spec`: speculative + lib-side memcmp) | liburcu (membarrier) |
| `bench_scale_ft_qsbr` | Fractal Trie, same engine, QSBR flavor | liburcu-qsbr |
| `bench_scale_judy`  | JudySL, rwlock                  | libJudy           |
| `bench_scale_qp`    | qp-trie, rwlock                 | vendored qp-trie  |
| `bench_scale_art`   | ART, rwlock                     | vendored libart   |
| `bench_scale_b9qp`  | BIND9 `dns_qpmulti`, RCU        | liburcu + bind9   |
| `bench_scale_hotrowex` | HOT (concurrent **ROWEX**)   | HOT + oneTBB      |
| `bench_scale_masstree` | **Masstree** (B+tree-of-tries) | Masstree (MIT)  |
| `bench_scale_artolc` | **ART-OLC** (concurrent ART, Opt. Lock Coupling) | ART-OLC + oneTBB |

They share `bench_scale_common.c` (key generation, RSS sampling, the
thread-sweep driver, and a dense `bench_arena` bump allocator); each
`bench_scale_<engine>.c` supplies that engine's build / lookup / churn callbacks
and a thin `main`. **Only `bench_scale_b9qp` links bind9.** For a fair lookup
comparison every engine stores its keys as **copies in the arena** (so the
validating compare each lookup does hits cold, separate memory — not the shared
query buffer, which would make validation almost free), and each reader
force-reads the returned leaf so that compare is real and not optimized away.

`bench_scale_hotrowex`, `bench_scale_masstree`, and `bench_scale_artolc` are
built separately by the **top-level Makefile** (they link neither bind9 nor
liburcu — HOTRowex and ART-OLC use oneTBB [`libtbb-dev`] for their epoch
reclamation; Masstree links its own vendored sources), and land in the repo root
rather than `bind9-src/build/`. `run_scale_rw.sh` looks there too:

```sh
make bench_scale_hotrowex bench_scale_masstree bench_scale_artolc
ENGINES="ft hotrowex masstree artolc" scripts/run_scale_rw.sh 192
```

It is the lone **ROWEX** engine here — readers are optimistic and lock-free
(they restart on a concurrent structural change) and self-guard HOT's epoch on
every operation, so unlike the FT/Judy/qp/ART threads they need no explicit
registration. One asymmetry: **ROWEX has no delete** upstream, so its writer
churns by `upsert()` (point value-updates that still drive the full ROWEX write
path) rather than the insert/remove toggling the other engines do — its
`*_wr` column therefore measures a cheaper operation and is not directly
comparable; the read columns are.

Run one engine directly — it prints its RSS (sampled after build) and per
thread-count throughput; the argument caps the reader thread count:

```sh
LD_LIBRARY_PATH=urcu-build/src/.libs \
  bind9-src/build/tests/bench/bench_scale_ft 16
```

Or run all five (each its own process) and assemble the combined table:

```sh
scripts/run_scale_rw.sh 16        # arg = max thread count (default 384)
```

Cache priming is **on by default** — an untimed warm pass of ~N_KEYS lookups,
identical for every engine, so the timed window reflects steady state rather
than cold-start misses. Set `BENCH_NO_PRIME=1` to disable it.

`FT_BENCH_COMPACT=1` (FT engine only) recompacts the trie after each
thread-count run's churn via `cds_ft_compact()` — a copying GC-style recompact
that restores descent locality, so each subsequent point measures a
freshly-shaped trie rather than one progressively fragmented by churn (closer
to BIND9-QP, which stays compact via `dns_qp_compact`). It affects
throughput/shape, not the reported RSS (sampled once after build).

### Result — FT (RCU) vs HOTRowex (ROWEX) at scale

Read throughput (1M DNS keys, 1 writer + N readers, priming on, **median of 10
runs** per point) on the same 2× EPYC 9654 / 192-physical-core box as the
`load-names` result above, worker `i` pinned to core `i`. Both engines are on
**equal footing**: each stores its lookup keys as copies in a dense
`bench_arena` external-node region (FT: the `ft_entry` embedding the
`cds_ft_node`; HOTRowex: the byte copies its values point at — *not* pointers
into the shared query buffer), the FT reader uses the validating
`cds_ft_speculative_lookup_key` (`ft_spec`), and both force-read the returned
leaf, so each lookup pays a real validating compare against cold memory that is
never dead-code-eliminated.

| Readers | `ft` (FT spec) Mops/s | `hotrowex` Mops/s † | HOTRowex vs FT |
|--------:|----------------------:|--------------------:|---------------:|
| 64      | 152                   | 194                 | ≈ 1.3×         |
| 128     | 244                   | 336                 | ≈ 1.4×         |
| 192     | 287 ‡                 | 446                 | ≈ 1.55×        |
| RSS     | 245 MB                | **110 MB**          | ≈ ½ the memory |

HOTRowex's lock-free optimistic reads pull further ahead as cores climb
(1.3× → 1.55×); on top of that it runs in ≈ half the RSS.

‡ Medians of 10 runs. HOTRowex (192-core reps 431–453) and FT-QSBR (252–262) are
tight and well-converged; **FT under membarrier stays noisy** (267–354, median
287) — its occasional highs are luck, not capability, so best-of would flatter
it. FT-QSBR's median is 256, so HOTRowex leads it by ≈ 1.7× at 192.

> **Earlier drafts of this result were built on unfair measurements.** Two bugs,
> both now fixed: (1) the FT reader left its result unused, so the compiler
> dead-code-eliminated the validating memcmp — it was really timing `ft_cand`,
> the *unvalidated* candidate lookup (it read ~431 Mops/s); and (2) HOTRowex
> stored pointers straight into the shared query buffer, so its `contentEquals`
> validated against already-hot memory for free and kept no key copies (79 MB,
> ~699 Mops/s). The intermediate half-fix (FT validated but in *scattered*
> calloc, HOTRowex still zero-copy) swung the other way to ~2.5×. With both
> engines validated **and** both storing key copies in a dense arena, FT-spec
> settled at ≈ 287 Mops/s and HOTRowex at ≈ 446 (RSS 79 → 110 MB): an honest
> ≈ 1.55× read gap plus ≈ ½ the RSS. (The original 1.6× ratio was coincidentally
> close — both numbers were inflated by roughly the same factor.)

> **† HOTRowex (ROWEX) does not support `remove`.** Upstream HOT's concurrent
> ROWEX variant implements lookup / scan / insert / `upsert` only — there is no
> concurrent delete. It is therefore **not a drop-in replacement** for a trie
> that must delete keys (DNS zones, routing tables, caches with eviction…). In
> this benchmark its writer churns by `upsert` instead of the insert/remove
> toggling every other engine does, so its read numbers are directly comparable
> but its workload is strictly easier on the write path. The Fractal Trie
> supports full concurrent insert **and** remove under RCU.

**RCU flavor is not the variable.** `bench_scale_ft` uses liburcu's membarrier
flavor; `bench_scale_ft_qsbr` is the identical engine built `-DBENCH_FT_QSBR`
against the QSBR flavor (its only diff — see `bench_scale_ft.c`). QSBR has the
cheapest possible read side (`rcu_read_lock`/`rcu_read_unlock` compile to
nothing), yet the two are within ~12% (192-core medians of 10: memb ≈ 287, QSBR
≈ 256) — membarrier slightly higher but much noisier (reps 267–354), QSBR tight
(252–262) and more reproducible. Expected: the reader brackets one
`rcu_read_lock`/`unlock` pair around a whole 1000-lookup batch, so the read-side
cost is amortized to ~nothing under membarrier too, and the `call_rcu` writer
makes grace periods (and membarrier's broadcast IPIs) infrequent — so flavor
barely moves the throughput, it mostly affects the variance. (QSBR does demand a stricter
discipline: its otherwise-idle main thread must go `rcu_thread_offline()` across
each timed window or it would stall every grace period — handled in `ft_build` /
`ft_cleanup_churn`.)

**NUMA interleaving does not help this benchmark.** `BENCH_NUMA_INTERLEAVE`
spreads each engine's keys + arena across all NUMA nodes
(`numa_set_interleave_mask`); at 192 cores it is a wash and at low thread counts
it *hurts*, because this writer-contended workload is latency-bound
pointer-chasing, not bandwidth-bound — the opposite of `load-names`' read-only
`ft_spec_il`, where an interleaved arena wins at ≥128 threads. Left off by
default; the numbers above are non-interleaved.

### Adding Masstree and ART-OLC

Two more concurrent structures join the sweep on the same fair footing (key
copies in the dense arena, validated descent, force-read leaf):

- `bench_scale_masstree` — **Masstree** (Mao/Kohler/Morris, EuroSys'12;
  kohler/masstree-beta, MIT): a B+tree of tries, optimistic version-validated
  readers, epoch-reclaimed removes. Its per-thread `threadinfo` follows
  Masstree's RCU-like epoch discipline (`rcu_start`/`rcu_quiesce`/`rcu_stop`).
- `bench_scale_artolc` — **ART-OLC** (Leis et al., DaMoN'16;
  flode/ARTSynchronized, Apache-2.0): a concurrent adaptive radix tree, readers
  optimistically validate per-node versions and restart, writers lock-couple.
  ART stores only a TID per leaf and validates via a `loadKey(TID)` callback, so
  we point the TID at the arena key copy. **ART needs byte-prefix-free keys**, so
  we key on the NUL terminator too (`len+1`) — without it ART mis-stores
  prefix-colliding keys (this bench doesn't check results, so it tolerated that
  silently; load-names' `CHECKN` caught it).

Medians of 5, reads (Mops/s):

| Readers | `ft_spec` | `masstree` | `artolc` | `hotrowex` |
|--------:|----------:|-----------:|---------:|-----------:|
| 64      | 157       | 118        | 132      | **179**    |
| 128     | 236       | 232        | 255      | **328**    |
| 192     | 280       | 330        | **376**  | **439**    |

At 192 the order is **HOTRowex (439) > ART-OLC (376) > Masstree (330) >
FT-spec (280)** — all four scale, none dominates every axis. HOTRowex leads reads
and footprint (110 MB); ART-OLC and Masstree slot above ft_spec on reads at the
top (RSS 144 and 184 MB); Masstree has the fastest insert/remove churn (~5000
vs ART-OLC ~1850 vs FT ~500 Kops/s); FT alone does full concurrent insert **and**
remove under RCU with the lowest read-side cost.

### Why one process per engine

BIND9's libisc ELF constructor (`isc__lib_initialize`) calls
`rcu_register_thread()` for the main thread and leaves it registered. The old
single-process benchmark *also* registered the main thread, adding the same
`urcu_reader` to liburcu's registry twice; under the release build (asserts
compiled out) that corrupted the registry's circular list, so the first
`call_rcu` grace period spun forever in `wait_for_readers()` and the program
deadlocked. Splitting the engines means only `bench_scale_b9qp` links libisc —
the other four register the main thread once themselves, and the
double-registration cannot happen.

### Status of qpmulti_ft

`make bind9` also overlays `qpmulti_ft.c`, which still tracks an **older
Fractal Trie API** than the current `fractal-trie-dev`. It is attempted and
**skipped on failure** until updated to the current API.

## Layout

```
src/bench_one_st.c               single-threaded benchmark
bind9-overlay/tests/bench/       MT benchmark sources + meson.build template:
                                   load-names.c, qpmulti_ft.c,
                                   bench_scale_common.[ch] (shared driver),
                                   bench_scale_{ft,judy,qp,art,b9qp}.c
src/bench_scale_hotrowex.cpp     concurrent (ROWEX) HOT MT engine; same driver,
                                   built standalone by the top-level Makefile
src/bench_scale_masstree.cpp     Masstree (B+tree-of-tries) MT engine; same driver
src/bench_scale_artolc.cpp       ART-OLC (concurrent ART) MT engine; same driver
third_party/masstree/            vendored Masstree, C++ (MIT) + generated config.h
third_party/artolc/              vendored ART-OLC, C++ (Apache-2.0)
third_party/{qp-trie,libart}/    vendored competitors (permissive)
third_party/hot/                 vendored HOT, header-only C++14 (ISC);
                                   single-threaded + rowex (concurrent) headers
src/bench_hot.cpp                C++ shim exposing HOT to bench_one_st
third_party/cuckoo-trie/         vendored Cuckoo Trie, C (Unlicense)
src/bench_cuckoo.c               C shim exposing Cuckoo Trie to bench_one_st
third_party/wormhole/            vendored Wormhole (GPL-3.0; bench_wormhole_gpl only)
src/bench_wormhole_gpl.c         GPL-3.0 single-threaded Wormhole benchmark
datasets/                        names CSVs (1M shuffled / trie-sorted + smoke)
urcu-build/                      our liburcu clone (fractal-trie-dev), gitignored
bind9-src/                       our bind9 clone + overlay + build, gitignored
scripts/build-bind9.sh           clones/overlays/builds the bind9 MT benches
scripts/run_scale_rw.sh          runs the per-engine scaling benches, combined table
```

## Licensing of vendored code

- `third_party/qp-trie` — CC0 / public domain (Tony Finch). See `NOTICE`.
- `third_party/libart` — BSD-2-Clause (Armon Dadgar). See `LICENSE`.
- `third_party/hot` — ISC (Robert Binna et al.). Header-only C++14; linked into
  `bench_one_st`'s `hot` engine via the `src/bench_hot.cpp` shim. See `LICENSE`.
- `third_party/cuckoo-trie` — Unlicense / public domain (Zeitak & Morrison). C;
  linked into `bench_one_st`'s `cuckoo` engine via `src/bench_cuckoo.c`. See
  `UNLICENSE`. Built with Cuckoo's own recommended `-O3 -flto
  -fno-strict-aliasing` — **LTO matters**: at `-O2` without LTO it is ~1.7×
  slower (~580 vs ~337 ns/op on dns). **Local change:** `util.c`'s
  `mmap_hugepage` falls back to a plain `mmap` + `MADV_HUGEPAGE` when reserved
  2 MiB hugepages are unavailable (upstream requires them and aborts); reserving
  hugepages (`echo N | sudo tee /proc/sys/vm/nr_hugepages`, a few hundred 2 MiB
  pages for 1M keys) mainly improves its **footprint** (~106 vs ~143 MB RSS),
  not its speed. Note: even built optimally and hugepage-backed, Cuckoo is the
  slowest engine on this workload (~337 ns/op vs ~100–120 for the radix/FT
  engines) — short DNS keys with heavy shared prefixes favor prefix-exploiting
  radix tries, whereas Cuckoo hashes whole keys and its memory-level-parallelism
  design targets a different regime.
- `third_party/masstree` — **MIT** (Harvard / MIT / UC Regents; Mao, Kohler,
  Morris). Concurrent B+tree-of-tries; the `bench_scale_masstree` MT engine via
  `src/bench_scale_masstree.cpp`. `config.h` is vendored as generated by
  Masstree's `./configure` — regenerate with `autoreconf -i && ./configure` if
  building on a materially different host. See `LICENSE` / `AUTHORS`.
- `third_party/artolc` — **Apache-2.0** (Florian Scheibner; ART of Leis et al.).
  Concurrent ART (Optimistic Lock Coupling); the `bench_scale_artolc` /
  load-names `artolc` engines. Unity build (`OptimisticLockCoupling/Tree.cpp`
  `#include`s the rest), needs oneTBB. See `LICENSE`.
- `third_party/wormhole` — **GPL-3.0** (Xingbo Wu). See `third_party/wormhole/LICENSE`.
  Because it is GPL-3.0, Wormhole is **never** linked into the permissively
  licensed benchmarks. It is built only into its own executable,
  `bench_wormhole_gpl` (which is therefore GPL-3.0), via `make bench_wormhole_gpl`
  — kept out of `make all`. This isolates the copyleft to one opt-in binary.

### Wormhole — separate GPL benchmark

`bench_wormhole_gpl` measures Wormhole (a "trie of hash tables" ordered index)
single-threaded on the same `dns` 1M-key set and identical harness as
`bench_one_st`, so its `<ns/op> <RSS_kB>` output is directly comparable:

```sh
make bench_wormhole_gpl     # GPL-3.0 binary; not built by `make all`
./bench_wormhole_gpl
```
