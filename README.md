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
| `artrowex` | ART-ROWEX, concurrent ART (Read-Opt. Write Excl.) | `third_party/artolc/ROWEX` (Apache-2.0); MT |
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
for e in ft_eager ft_spec judyl judysl judyhs qp art hot cuckoo masstree artolc; do \
  printf '%-10s ' "$e"; ./bench_one_st dns "$e"; done
```

Useful env vars (see `src/bench_one_st.c`): `FT_BENCH_COMPACT` (compact between
build and query), `FT_DUMP_STATS`, `N_KEYS` / `WARMUP` / `RUNS` (compile-time).

### Results across datasets (1M keys, single thread)

Lookup time, ns/op (best of `RUNS` timed passes after `WARMUP`), on the hardware
below (2× EPYC 9654; `cuckoo`, `art`, `masstree` built `-O3` — `cuckoo` also
`-flto` + 2 MiB hugepages; every other engine, including FT, is `-O2`, which it
saturates: `-O3`/LTO move them <2% — see opt-level note below).
Every engine now runs on every dataset — the byte-keyed engines key integers as
big-endian bytes; `judysl` (string radix) and `judyl` (integer array) are the
two split-out Judy variants, `judyhs` is Judy's hash array.

**String keys** (`dns` DNS names, `dict` words, `paths` filesystem paths),
fastest-first by `dns`:

| Engine     | `dns` | `dict` | `paths` |
|------------|------:|-------:|--------:|
| `hot`      |    99 |    106 |      77 |
| `wormhole`†|   113 |    101 |     114 |
| `qp`       |   118 |    127 |     177 |
| `ft_spec`  |   119 |    112 |     143 |
| `judyhs`‡  |   145 |    121 |     149 |
| `art`      |   183 |    167 |     223 |
| `ft_eager` |   185 |    158 |     202 |
| `judysl`   |   202 |    250 |     263 |
| `artolc`   |   218 |    212 |     238 |
| `masstree` |   231 |    193 |     207 |
| `cuckoo`   |   335 |    301 |     332 |

**Integer keys** (`u32/u64` × `d`ense sequential / `s`parse random),
fastest-first by `u64d`:

| Engine     | `u32d` | `u32s` | `u64d` | `u64s` |
|------------|-------:|-------:|-------:|-------:|
| `judyl`    |     11 |     38 |     11 |     64 |
| `art`      |     11 |     46 |     12 |     47 |
| `qp`       |     13 |     13 |     13 |     13 |
| `ft_spec`  |     14 |     33 |     15 |     33 |
| `ft_eager` |     13 |     42 |     16 |     44 |
| `hot`      |     20 |     49 |     20 |     49 |
| `artolc`   |     22 |     96 |     24 |     98 |
| `judyhs`‡  |     24 |     46 |     38 |     78 |
| `wormhole`†|     34 |     88 |     38 |     92 |
| `masstree` |     46 |    172 |     46 |    168 |
| `cuckoo`   |     96 |    114 |    118 |    117 |

† `wormhole` is the separate **GPL-3.0** binary (`bench_wormhole_gpl [dataset]`),
never linked into `bench_one_st`; shown here for comparison. A trie of hash
tables — distribution-sensitive like the hashes (dense ints ~34–38 ns, sparse
~90), mid-pack on strings.

‡ **`judyhs` is hash-based — not order-preserving** (no ordered iteration or
range queries). Every other engine here keeps keys ordered and supports an O(n)
in-order scan, which `judyhs` trades away for hash speed — including `wormhole`
(an ordered trie-of-hashes) and even `cuckoo` (the Cuckoo Trie holds its leaves
in a sorted linked list and exposes `ct_iter_goto` lower-bound seek +
`ct_iter_next` forward iteration).

Takeaways:
- **FT's two validating modes**: `ft_spec` (speculative — skip-compressed
  encoding, one end-of-walk `memcmp`) is the better default, beating `ft_eager`
  (eager-optimized — per-step exact compares on compressed bytes) on strings
  (`dns` 119 vs 185), sparse integers (`u64s` 33 vs 44) and dense integers
  (`u64d` 15 vs 16). Both return validated results — `ft_cand` (the raw,
  *unvalidated* candidate primitive) is excluded from these tables since it skips
  the compare every other engine pays.
- **`qp` is uniquely distribution-insensitive on integers** — ~12–13 ns on *all
  four* sets, including the sparse random ones where everything else degrades 2–6×
  (`judyl` 11→64, `ft` 15→33, `art` 12→47). Its bit-popcount nodes don't care
  whether keys cluster.
- **`judyl` wins dense integers** (11 ns) but collapses on sparse (64); **`judyhs`
  beats `judysl` on strings** (hash suits these distributions better than the
  radix tree), and **`hot` has the fastest string lookups** (77–106 ns).
- **`cuckoo` is slowest throughout** (its trie nodes live in a cuckoo hash
  table — extra hashing and bucket probes per descent step);
  **Masstree and ART-OLC carry their concurrency machinery** even single-threaded,
  so they trail the dedicated ST engines (ART-OLC the closer of the two).
- **Opt level: only `art` profits from `-O3`** (~11% on strings, ~18% on dense
  integers — its node-256 scan and path-compression loops unroll/vectorize), so
  it's built `-O3` like `cuckoo`; at `-O3` it even edges past FT on dense ints
  (`u64d` 12 vs FT's 15–16). `masstree` gains a marginal ~3% (also `-O3`).
  `qp`, `hot`, ART-OLC, **and FT** are flat (<2% across `-O2`/`-O3`/`-flto`,
  measured interleaved 30×) — pointer-chasing radix walks are latency/cache-bound
  and already saturated at `-O2`; FT additionally hand-codes its `popcnt`/`bmi`
  hot path. LTO buys nothing (each engine is effectively a single TU).
- Single snapshot, best-of-`RUNS` (figures move a few % run to run).

> **Validation fairness.** Every engine stores its own **copy** of each key
> (FT/qp/ART/Masstree/ART-OLC in a dense `cds_ft_external_arena`; Judy/Cuckoo
> internally; HOT in the same arena) and the timed loop consumes the lookup
> status and force-reads the returned leaf (`FORCE_READ_LEAF`), so each pays a
> real validating compare against cold memory and no validation is dead-code-
> eliminated. HOT (integer) uses map-mode (value = a pointer to the key record),
> not its cheaper set-mode, so it touches a cold value like the others.

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

### Result — FT spec vs HOTRowex vs Masstree vs ART-OLC/ROWEX on this workload

HOT's concurrent ROWEX trie (`hotrowex`), Masstree (`masstree`), ART-OLC
(`artolc`), and ART-ROWEX (`artrowex`) are all wired into load-names, so the same
read-only, sequential-access, real-names sweep compares them against `ft_spec_il`
on equal footing — all validate every lookup and store key copies in a
NUMA-interleaved arena (HOT keys on a NUL-terminated qpkey copy; Masstree on the
binary qpkey bytes; both ARTs on a `\0`-terminated qpkey, since ART needs
byte-prefix-free keys). Median of 4 runs, query Mops/s (fresh process per thread
count):

| Threads | `ft_spec_il` | `hotrowex` | `artolc` | `artrowex` | `masstree` |
|--------:|-------------:|-----------:|---------:|-----------:|-----------:|
| 64      | 317          | **355**    | 208      | 203        | 166        |
| 128     | **758**      | 719        | 428      | 424        | 262        |
| 192     | **1212**     | 1021       | 685      | 701        | 267        |

**ART-ROWEX tracks ART-OLC closely and edges ahead at 192** (701 vs 685) — its
read-optimized write exclusion costs a hair at low counts (readers wait on a
node only while a writer holds it) but avoids OLC's optimistic-read restarts as
contention rises. Both ARTs still trail `ft_spec_il` and `hotrowex` here.

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
| `bench_scale_artrowex` | **ART-ROWEX** (concurrent ART, Read-Opt. Write Excl.) | ART-ROWEX + oneTBB |

They share `bench_scale_common.c` (key generation, RSS sampling, the
thread-sweep driver, and a dense `bench_arena` bump allocator); each
`bench_scale_<engine>.c` supplies that engine's build / lookup / churn callbacks
and a thin `main`. **Only `bench_scale_b9qp` links bind9.** For a fair lookup
comparison every engine stores its keys as **copies in the arena** (so the
validating compare each lookup does hits cold, separate memory — not the shared
query buffer, which would make validation almost free), and each reader
force-reads the returned leaf so that compare is real and not optimized away.

`bench_scale_hotrowex`, `bench_scale_masstree`, `bench_scale_artolc`, and
`bench_scale_artrowex` are built separately by the **top-level Makefile** (they
link neither bind9 nor liburcu — HOTRowex, ART-OLC and ART-ROWEX use oneTBB
[`libtbb-dev`] for their epoch reclamation; Masstree links its own vendored
sources), and land in the repo root rather than `bind9-src/build/`.
`run_scale_rw.sh` looks there too:

```sh
make bench_scale_hotrowex bench_scale_masstree bench_scale_artolc bench_scale_artrowex
ENGINES="ft hotrowex masstree artolc artrowex" scripts/run_scale_rw.sh 192
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

### Result — read throughput vs reader threads

Read throughput on 1M DNS keys (priming on), **threads pinned one per physical
core** (worker `i` → CPU `i`, so the 192-thread points fill the 2× EPYC 9654's
192 physical cores with no SMT-sibling contention), on the same box as the
`load-names` result above. All engines are on **equal footing**: each stores its
lookup keys as copies in a dense `bench_arena` external-node region (FT: the
`ft_entry` embedding the `cds_ft_node`; HOTRowex / Masstree / ART: the byte
copies their values point at — *not* pointers into the shared query buffer),
every reader validates (FT via `cds_ft_speculative_lookup_key`, ART via
`loadKey`, HOT via `contentEquals`) and force-reads the returned leaf, so each
lookup pays a real validating compare against cold memory that is never
dead-code-eliminated. Two workloads, each filling all 192 cores:

**1 writer + N readers** — a writer churns insert/remove the whole window;
readers cap at 191 so reader + writer = 192 threads. Medians (5–10 runs/cell), read Mops/s:

| Readers | `ft` | `ft_qsbr` | `hotrowex` † | `artolc` | `artrowex` | `masstree` |
|--------:|-----:|----------:|-------------:|---------:|-----------:|-----------:|
| 64  | 160 | 166 | 177 | 127 | 123 | 116 |
| 96  | 226 | 234 | 252 | 188 | 182 | 171 |
| 128 | 286 | 290 | 320 | 250 | 243 | 227 |
| 191 | 380 | 377 | **426** | 367 | 356 | 330 |
| RSS | 320 MB | 320 MB | **110 MB** | 141 MB | 141 MB | 185 MB |

**Readers only** (`BENCH_NO_WRITER`, no concurrent mutation; readers reach 192).
Median of 7, read Mops/s:

| Readers | `ft` | `ft_qsbr` | `hotrowex` | `artolc` | `artrowex` | `masstree` |
|--------:|-----:|----------:|-----------:|---------:|-----------:|-----------:|
| 64  | 166 | 173 | 179 | 129 | 127 | 116 |
| 96  | 236 | 248 | 259 | 192 | 188 | 172 |
| 128 | 298 | 301 | 323 | 255 | 251 | 228 |
| 192 | 396 | 393 | **431** | 378 | 372 | 333 |

At 192 the order is **HOTRowex > FT ≈ FT-QSBR > ART-OLC > ART-ROWEX > Masstree**.
HOTRowex leads reads (~1.08× over FT) and footprint (110 MB), but FT is a close
second and **ahead of all three ART/Masstree variants** — and it is the only
engine doing full concurrent insert **and** remove under RCU (HOTRowex's ROWEX
has no concurrent delete; it churns by `upsert`). FT's higher RSS (320 MB) is
dominated by the default-on ordered-list cells (32 B/key of update-side state
the point-lookup reader never touches), not the descent working set.

> **This result depended on getting the measurement right.** On top of the
> earlier fairness fixes — every reader now *validates* against a key copy in a
> dense arena (an un-validated reader once let the compiler dead-code-eliminate
> the compare; HOTRowex once stored pointers into the shared query buffer and
> kept no copies) — three later bench bugs had made FT look *last*, all now
> fixed: (1) **no thread pinning** — unpinned, the scheduler stacked readers on
> SMT siblings and left physical cores idle, and FT's larger footprint paid the
> contention most; (2) under QSBR the **churn writer ran online**, stalling the
> grace periods that reclaim removed nodes; and (3) `rcu_barrier()` was **gated
> on `FT_BENCH_COMPACT`**, so a plain build's deferred node frees drained
> *during* the timed window, stealing cores from the readers. Pinning
> one-thread-per-core plus a fully offline, promptly-reclaimed FT build erased a
> spurious ~13% QSBR gap and a larger SMT-contention penalty, lifting FT from
> last to a close second.

> **† HOTRowex (ROWEX) does not support `remove`.** Upstream HOT's concurrent
> ROWEX variant implements lookup / scan / insert / `upsert` only — there is no
> concurrent delete. It is therefore **not a drop-in replacement** for a trie
> that must delete keys (DNS zones, routing tables, caches with eviction…). In
> this benchmark its writer churns by `upsert` instead of the insert/remove
> toggling every other engine does, so its read numbers are directly comparable
> but its workload is strictly easier on the write path. The Fractal Trie
> supports full concurrent insert **and** remove under RCU.

**RCU flavor is genuinely not the variable.** `bench_scale_ft` (membarrier) and
`bench_scale_ft_qsbr` (`-DBENCH_FT_QSBR`, QSBR — its only diff) now read **within
~1%** of each other at every thread count (readers-only @192: memb 396, QSBR 393,
overlapping distributions). That is the expected result — the reader brackets one
`rcu_read_lock`/`unlock` pair around a whole 1000-lookup batch, so the read side
is amortized to ~nothing under both flavors. An earlier ~13% QSBR deficit was
*not* the flavor but a benchmark artifact: under QSBR an online registered thread
stalls grace periods, so deferred frees piled up and the FT node layout
scattered. The fix is uniform discipline now applied to both: the exclusive FT
build **and** the churn writer run `rcu_thread_offline()` (they hold the writer
lock and never read under RCU), and `rcu_barrier()` drains the deferred frees
before each timed window — so QSBR's grace periods are never stalled and its
layout is as compact as membarrier's.

**NUMA interleaving is a wash here.** `BENCH_NUMA_INTERLEAVE` (default on,
`numa_set_interleave_mask`) spreads each engine's keys + arena across all 24
nodes. With threads pinned one-per-core, an interleave on/off A/B is within
run-to-run noise at every thread count for this latency-bound pointer-chase —
neither helps nor hurts. (Contrast `load-names`' read-only `ft_spec_il`, where an
interleaved arena wins at ≥128 threads, and the bandwidth-bound ordered-iteration
sweep, where it is a 10-20× swing.) FT keeps its own 2 MiB-coarse arena
interleave either way; opt out of the process interleave with
`BENCH_NUMA_INTERLEAVE=0`.

### The non-FT concurrent structures

The four non-FT engines in the tables above join the sweep on the same fair
footing (key copies in the dense arena, validated descent, force-read leaf):

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
- `bench_scale_artrowex` — **ART-ROWEX** (Leis et al., DaMoN'16; same
  flode/ARTSynchronized repo, Apache-2.0): the Read-Optimized Write EXclusion
  ART. Same loadKey-validated, prefix-free-key footing as ART-OLC; the
  difference is the read discipline — ROWEX readers never restart (writers take
  per-node write locks that *exclude* concurrent readers from that node), and
  unlike HOT's ROWEX it supports `remove`, so its writer churns insert/remove
  like the others. Wired into both `bench_scale_artrowex` and load-names
  (`artrowex`); its vendored sources are byte-identical to upstream, and its
  Epoche object coexists with ART-OLC's via weak/COMDAT symbols.

Their per-engine read numbers are in the two tables above. Two notes on the
writer workload: unlike `load-names`, here **ART-ROWEX trails ART-OLC** slightly
— the random-access write/read churn keeps a writer constantly touching nodes,
so ROWEX readers pay the node-exclusion wait more often than they save on avoided
restarts. And the engines differ sharply on **mutator** throughput: Masstree has
the fastest insert/remove churn (~5000 Kops/s), then ART-OLC (~1850) and FT
(~500); HOTRowex's ROWEX has no concurrent `remove` at all. So FT trades a modest
read deficit (~1.08× behind HOTRowex) and a larger RSS for being the only engine
with full concurrent insert **and** remove under RCU at the lowest read-side
cost.

### Mutator throughput vs reader concurrency

The sweep above scales *readers* against one writer; this one **inverts** it —
fix **one mutator thread** doing insert/replace/remove and scale **readers 0 →
191** — to show how reader concurrency throttles a single writer. Same binaries
(`BENCH_MUTATOR=1`); per-op throughput in **kops/s**, median of 3. "Replace" is
each engine's natural value update where it has one (Judy/qp/ART/Masstree update
in place), an `upsert` for HOTRowex (no delete), or remove+reinsert where there
is no value-update API (FT swaps the leaf node + RCU-frees the old; ART-OLC/ROWEX;
BIND9 `dns_qp` delete+insert in one write txn).

```sh
# per engine; bind9 engines need LD_LIBRARY_PATH and live in bind9-src/build/...
BENCH_MUTATOR=1 ./bench_scale_artrowex 200
LD_LIBRARY_PATH=urcu-build/src/.libs BENCH_MUTATOR=1 \
    bind9-src/build/tests/bench/bench_scale_ft 200
```

**Insert (kops/s)** — readers across the top:

| Engine | 0 | 1 | 16 | 64 | 191 | sync |
|------------|------:|----:|-----:|----:|----:|:-----|
| `masstree` | 11272 |11123|10589 |10508| 9494| optimistic |
| `hotrowex` |  5680 | 5610| 5235 | 4713| 4152| ROWEX |
| `artolc`   |  4091 | 3272| 2160 | 1796| 1457| OLC |
| `artrowex` |  3421 | 2849| 2087 | 1639| 1469| ROWEX |
| `ft`       |  1340 |  592|  666 |  326|  498| **RCU** |
| `b9qp`     |   441 |  419|  288 |  268|  191| **RCU** |
| `judy`     | 10628 |**2.8**| 19 |  92 | 429 | rwlock |
| `qp`       |  9170 |**3.8**|  33 | 134 | 513 | rwlock |
| `art`      | 10197 |**2.8**| 34 | 113 | 461 | rwlock |

**The cliff is the result.** The three **rwlock** engines have the *fastest*
single-thread mutation (~9–11M ops/s), then **fall off a cliff the instant a
reader appears** — judy/art `10628 → 2.8`, qp `9170 → 3.8` kops, a ~1000–3000×
collapse — because the writer-preferring rwlock writer must wait for readers, and
each reader holds the rdlock across a whole 1000-lookup batch. (The noisy partial
"recovery" at higher reader counts is scheduling churn in the starved regime, not
a real trend; the rwlock numbers there are not reproducible point-to-point.)
**RCU and the lock-free concurrent tries do not collapse** — FT, b9qp, Masstree,
HOTRowex and both ARTs keep mutating within the same order of magnitude all the
way to 191 readers, because their readers never hold a lock the writer needs.
Masstree is barely touched (−16% over the whole sweep); FT is the noisiest of the
robust set (RCU reclamation timing) but never starves.

**Replace** and **Remove (kops/s)** at the endpoints (0 / 191 readers):

| Engine | replace 0 | replace 191 | remove 0 | remove 191 |
|------------|----------:|------------:|---------:|-----------:|
| `masstree` |     12913 |       12183 |    12175 |       9207 |
| `hotrowex` |      6185 |        4516 |   *n/a*  |     *n/a*  |
| `artolc`   |      1243 |        1170 |     1193 |       1057 |
| `artrowex` |       790 |         702 |     1144 |        929 |
| `ft`       |      1057 |         375 |     2412 |        754 |
| `b9qp`     |       446 |         196 |      471 |        206 |
| `judy`     |     14923 |         669 |     9015 |        358 |
| `qp`       |     17265 |        1358 |    16343 |       1050 |
| `art`      |     15361 |         800 |    12311 |        482 |

Per-op shape follows the mechanism: **Masstree's in-place update makes replace its
*cheapest* op** (12.9M ops/s, above its own insert and remove — no node split or
merge). The remove+reinsert engines (FT, ART-OLC/ROWEX) pay replace ≈ the harmonic
mean of their remove+insert. **HOTRowex has no remove** (ROWEX); its insert and
replace are both `upsert`. (Caveat: the rwlock collapse magnitude is tied to the
reader lock-hold granularity — readers batch 1000 lookups per rdlock here; finer
locking would starve the writer less, but the qualitative RCU-vs-rwlock gap
stands.)

**Compaction accounting.** Each engine's kops includes whatever maintenance it
does *inline*: b9qp's `dns_qp_compact(…, NOW)` (when `dns_qp_memusage().fragmented`)
and Masstree's epoch advance run inside the timed `writer_op`, so that
compaction/reclamation time is in the denominator (lowering their kops) though
it is not counted as an op. **FT does no inline compaction** — its reclamation is
asynchronous `call_rcu` on a separate thread — so FT's figure is the
no-compaction sustained mutation rate (it does spend a background core on the
deferred frees). Read FT vs b9qp with that asymmetry in mind: b9qp pays for
staying compact within the figure, FT does not.

### Ordered iteration throughput vs reader threads

A third axis: instead of point lookups or mutation, each of **1 → 192 reader
threads** loops a **full in-order traversal** of every key, and we report
aggregate **`next`-op** (key-visit) throughput. Read-only; `BENCH_ITERATE=1`.
Each engine uses its native ordered traversal — a cursor (FT
`cds_ft_for_each_rcu`, qp `Tnextl`, JudySL `JSLN`, HOTRowex `begin()`/`++`, BIND9
`dns_qpiter`), a callback scan (libart `art_iter`, Masstree `masstree_scan`), or
a range fetch (ART-OLC/ROWEX `lookupRange`).

```sh
BENCH_ITERATE=1 ./bench_scale_hotrowex 192
LD_LIBRARY_PATH=urcu-build/src/.libs FT_ORD=1 FT_BENCH_COMPACT=1 FT_BATCH=64 \
    BENCH_ITERATE=1 bind9-src/build/tests/bench/bench_scale_ft 192
```

Median of 3, next Mops/s — readers across the top:

| Engine | 1 | 16 | 64 | 192 | traversal |
|------------|------:|------:|-------:|-------:|:----------|
| **`ft`**   | **510** | **8168** | **32678** | **88124** | **batched cell gather, compacted (phys-next MLP; cell-native, no node touch)** |
| `hotrowex` |   175 |  2540 |  9759 | 27471 | inlined header-template + contiguous leaves |
| `b9qp`     |    91 |  1467 |   5851 |  15072 | `dns_qpiter` `.so` call + DFS-compacted chunks |
| `art`      |    22 |   339 |   1243 |   3745 | recursive callback |
| `masstree` |    19 |   261 |   1065 |   2972 | B+tree leaf scan |
| `artolc`   |    18 |   224 |    840 |   2532 | range-into-buffer |
| `artrowex` |    15 |   194 |    818 |   2227 | range-into-buffer |
| `judy`     |   5.2 |    82 |    337 |    968 | JSLN cursor (materializes key) |
| `qp`       |   4.3 |    65 |    263 |    785 | Tnextl cursor (materializes key) |

**FT is now the fastest ordered iterator** — ~3.2× over hotrowex at 192T, a full
reversal of the previous result (FT was *last*, 365 Mops/s). It got there in four
steps, each measured on the same 1M-key set:

| FT ordered-scan config | 192T Mops/s | what changed |
|---|--:|:--|
| `cds_ft_next` descent (pre-cell) | 350 | re-descend per step (the old result) |
| + ordered cell list (`FT_ORD`) | 3641 | O(1) cell hop; cells still in insert order |
| + compaction (`FT_BENCH_COMPACT`) | 20008 | cells packed in key order → contiguous walk |
| + batched gather (`FT_BATCH=64`) | **88124** | one call per batch + phys-next MLP; cell-native (no node touch) |

The final step folds three things into `cds_ft_cell_next_batch`: it amortizes the
library-call boundary over a whole batch; it predicts the physically-next cell
(post-compaction the cells are contiguous at a fixed stride, so a `cmm_ptr_eq`
arithmetic guess validates and the next `ord_next` load issues off arithmetic,
breaking the dependent-load chain → MLP); and it is **cell-native** — the walk
hands back opaque cell handles and the cursor is itself a cell, so it never
touches an external head node. (The earlier node-yielding batch recovered each
cell from the head's body via `node->prev` — a scattered cache miss per batch
that needed an `O(1)` resume cache to hide; making the walk cell-native removes
that touch structurally, so it is **cap-insensitive** — `FT_BATCH=16` already
reaches ~87k — and a count- or key-only scan, which never dereferences the node,
touches *no* external-head cachelines at all.) The node is recovered lazily, only
when the consumer wants the value, via `cds_ft_cell_node()`; the key via
`cds_ft_cell_get_key()`.

**Two findings.** (1) **Ordered iteration is embarrassingly parallel** — every
engine scales near-linearly (~120–170× from 1 to 192 threads): a full traversal
is read-only and touches no shared mutable state, so threads stream the structure
independently, bounded mainly by memory bandwidth. (2) **What wins is contiguity
plus a tight inner loop, not the data structure.** The old "cursor engines
re-descend, so they lose" framing was an artifact of the *un-compacted,
per-element* FT cursor. Once the cells are compacted (contiguous in key order) the
walk is a leaf-scan in all but name; once the per-step library-call boundary is
amortized by a batched fill, and the dependent `ord_next` load is broken by a
`cmm_ptr_eq` physical-next prediction (the next cell is `cur + 32 B`, validated,
so its body load issues off arithmetic rather than waiting on the pointer load),
the gather pipelines (MLP) and streams faster than even HOTRowex's fully-inlined
header-template scan. `b9qp` leads the non-FT engines on the strength of its
DFS-compacted chunk layout despite paying an un-inlined `dns_qpiter_next` call per
step — exactly the call boundary FT's batched iterator amortizes away.

**The cell scheme + compaction are required for the headline number.** It is the
*batched, compacted* path (`FT_ORD=1 FT_BENCH_COMPACT=1 FT_BATCH=64`, lib built
`-DFEATURE_FT_ORD_CELL`); the plain `cds_ft_for_each_rcu` cursor on an
un-compacted trie is ~24× slower (the 3641 row). Compaction trades RSS and a
one-time pack for the scan speed, so it suits read-mostly / snapshot scans rather
than churning tries. The batched walk is hidden behind a drop-in macro,
`cds_ft_for_each_batched_rcu(ft, cell, buf, cap)` (iterator-free: a hidden cell
cursor over `cds_ft_cell_next_batch`; recover the node lazily with
`cds_ft_cell_node`, no `cds_ft_iter` in scope).

(All engines visit the same ~995,830 unique keys — the generated DNS set has
~4,170 duplicates the dedup'ing tries collapse; ART keeps all 1,000,000 inserts,
a <0.5% difference, immaterial to throughput.)

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

`make bind9` also overlays `qpmulti_ft.c` — FT vs BIND9's native `dns_qpmulti`
in bind9's own `isc_loopmgr` micro-benchmark (the `vary_ft_*` sweeps). It is now
**ported to the current FT API and built by default** (it was previously
attempted-and-skipped against a stale API). The port carried over the same
methodology the other benches use: NUMA-interleaved allocation by default,
hwloc one-PU-per-core loop pinning (`isc_loopmgr` doesn't bind loops itself), a
build-time `rcu_barrier` reclaim drain before the FT read window, and — because
`isc_loopmgr` runs real concurrent readers — an RCU-safe mutate path that defers
each removed node's memory reuse past a grace period (`call_rcu`) instead of
zeroing it immediately under live readers. Run it with our liburcu on the path:

```sh
LD_LIBRARY_PATH=urcu-build/src/.libs DNS_NAMES_FILE=datasets/names-1M-shuf.csv \
  ISC_TASK_WORKERS=32 bind9-src/build/tests/bench/qpmulti_ft
```

#### Result — FT vs `dns_qpmulti` in bind9's event loop (192 cores)

Same 1M DNS names; each trie holds ~500k entries over that key space, so **~50% of
lookups miss**. Both engines are NUMA-interleaved and core-pinned. FT is shown in
two *fair* modes — **speculative** (skip-compressed descent + a validating key
compare, the API contract, mirroring qp's `leaf_qpkey`+`qpkey_compare`) and
**eager** (exact byte-by-byte descent). Aggregate read throughput (Mops/s), `loop`
column, readers across:

**Read-only:**

| readers | `qp` | FT eager | FT speculative |
|--:|--:|--:|--:|
| 1   | 2.3 | 2.2 | 2.3 |
| 16  | 42.7 | 39.9 | 40.3 |
| 64  | 170 | 159 | 161 |
| **192** | **508** | **443** (0.87×) | **462** (0.91×) |

**Mutate + read** (N readers alongside 192−N mutators):

| readers | `qp` | FT eager | FT speculative |
|--:|--:|--:|--:|
| 1   | 1.3 | 1.5 (1.17×) | 1.7 (1.31×) |
| 16  | 34.2 | 34.6 | 35.8 |
| 64  | 133 | 140 | 148 (1.11×) |
| **191** | **450** | **430** (0.95×) | **450** (1.00×) |

**Two findings.** (1) **Speculative is FT's best fair mode — it beats eager
everywhere.** Eager compares every key byte against the compressed-node encoding at
each level; speculative skips those compares and pays a *single* validation memcmp
at the leaf, and that wins even at a 50% miss rate. (2) **The result is
workload-dependent.** On the miss-heavy read-only sweep FT trails `qp` ~9% — qp's
sparse-branch descent exits early on a miss, while FT's speculative descent runs to
a candidate leaf before the validating compare rejects it. Under write contention
FT pulls ahead (1.0–1.31×, largest when mutators dominate) because its RCU read
path dirties no shared memory while qp's write-shares. This is the same mechanism
as the 100%-hit `load-names` result (FT ~1.3×) seen from the *other* end of the
hit-rate axis: **FT wins hit-heavy and write-contended; qp wins miss-heavy
read-only.**

> The speculative path **must validate the candidate** — `cds_ft_lookup_candidate_key`
> returns the unvalidated descent result, so counting any non-NULL candidate as a hit
> both miscounts misses and skips the compare qp performs, inflating FT ~14% and
> falsely showing it ahead on read-only. `FT_RAW_CANDIDATE=1` runs that unvalidated
> path as a (non-comparable) ceiling: ~527/517 Mops/s read-only/mut+read at the top.

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
src/bench_scale_artrowex.cpp     ART-ROWEX (concurrent ART) MT engine; same driver
third_party/masstree/            vendored Masstree, C++ (MIT) + generated config.h
third_party/artolc/              vendored ART-OLC + ART-ROWEX, C++ (Apache-2.0)
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
  Concurrent ART, both variants vendored byte-identical to upstream: Optimistic
  Lock Coupling (`OptimisticLockCoupling/`, the `bench_scale_artolc` / load-names
  `artolc` engines) and Read-Optimized Write EXclusion (`ROWEX/`, the
  `bench_scale_artrowex` / load-names `artrowex` engines). Each is a unity build
  (`<variant>/Tree.cpp` `#include`s the rest, incl. the shared `Epoche.cpp`) and
  needs oneTBB; the two share `Key.h`/`Epoche`. See `LICENSE`.
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
