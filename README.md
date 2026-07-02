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

## Bidirectional RCU list scaling — `bench_list_scale`

A separate benchmark (not a trie) comparing the **new userspace-rcu bidirectional
RCU lists** against the state-of-the-art ways to make a doubly-linked list
concurrent. The bidir lists (from `compudj/userspace-rcu-dev`, branch
`rcu-bidir-list-dev`) publish the forward **and** backward edges coherently, so a
reader may walk the ring in either direction — or reverse mid-walk — and never
see `next`/`prev` disagree, something the classic forward-only `rculist` cannot
offer. The question this answers: *what does that coherence cost, and how do the
two new lists scale against locks / seqlock at 192 cores?*

| engine      | synchronization                                   | reader | writer |
|-------------|---------------------------------------------------|--------|--------|
| `txn_sw_list`  | `<urcu/rcu-txn-sw-list.h>` — RCU, single updater   | lock-free | 1 (mutual excl.) |
| `txn_list`  | `<urcu/rcu-txn-list.h>` — RCU, MCAS    | lock-free | bounded-blocking (N) |
| `rlu_list`  | reference **RLU** (Read-Log-Update, MIT) — own SMR + per-object locks | lock-free (coherent snapshot) | bounded-blocking (N) |
| `rculist`   | classic `<urcu/rculist.h>` — **forward-only ref** | lock-free | 1 |
| `mutex`     | one `pthread_mutex`                                | serialized | serialized |
| `fairmutex` | liburcu `cds_fair_mutex` (MCS/FIFO queue lock)    | serialized | serialized |
| `rwlock_r`  | `pthread_rwlock`, reader-preferring               | shared | exclusive |
| `rwlock_w`  | `pthread_rwlock`, writer-preferring               | shared | exclusive |
| `iscrw`     | bind9 `isc_rwlock` (C-RW-WP phase-fair)           | shared | exclusive |
| `seqlock`   | sequence lock + type-stable nodes                 | optimistic (retry) | serialized |

The `iscrw` engine links the **real** bind9 lock (`bind9-src/lib/isc/rwlock.c`),
compiled standalone with a no-op probes shim (`src/iscrw-shim/`) and an isolation
wrapper (`src/bench_iscrw.c`) so the `isc/` macros never reach the main TU and no
libisc constructor runs.

`rlu_list` is the vendored reference **RLU** (`third_party/rlu`, MIT) driving the
*same* doubly-linked churn workload as `txn_list`, so the two multi-pointer-update
schemes meet on identical ground (pinning, warm-up, timing). RLU carries its own
SMR (a global clock + `rlu_synchronize`), so it is not a liburcu flavor. Its
guarantee is **declared, not emulated**: an RLU reader observes a *coherent
snapshot* of the objects it dereferences — strictly stronger than `txn_list`'s
per-slot-linearizable (non-snapshot) reads — and we measure both as-is rather than
handicapping either. `BENCH_RLU_WS` sets RLU's deferral depth (`max_write_sets`):
`1` is synchronous writeback (the floor), `100` is the headline deferred mode. A
hash-of-lists variant (`rlu_hlist` vs `txn_hlist`) meets RLU on its own native
showcase structure. See the [RLU comparison](#rlu-read-log-update-comparison) below.

### Building

```sh
make urcu-bidir          # clone rcu-bidir-list-dev into urcu-bidir-build/ + build liburcu
make bench_list_scale    # needs bind9-src/ isc headers (make bind9) for the iscrw engine
```

The lists are header-only (the `rcu-mcas`/`rcu-txn` engine + the `rcu-txn-list`
headers); only a stock liburcu
build of that branch is linked. QSBR flavor, `_LGPL_SOURCE` (inlined read side —
verified: no out-of-line `urcu_qsbr_read_lock`).

### Workload & methodology

A sorted ring of `LIST_SIZE` permanent **stable** nodes (always present) plus
`CHURN` **churn** nodes toggled in/out just after a unique, spread-out stable
anchor — so every insert/delete is O(1) and the ring stays strictly sorted at all
times. Readers walk forward **then** reverse, counting node visits; because the
ring is always sorted, a forward walk must see strictly increasing keys and a
reverse walk strictly decreasing — so monotonicity is a **free coherence check**
(0 violations across every run here). Writers toggle churn nodes.

Modes (env): default = read-scaling (readers 1→191 + 1 writer); `BENCH_NO_WRITER`
= read-only ceiling (readers 1→192, +SMT); `BENCH_WRITESCALE` = writer-scaling
(writers 1→N); `BENCH_RW_BALANCED` = balanced 50/50 (at each total T, T/2 readers
+ T/2 writers); `BENCH_FIXED_READERS=N` = single point; `BENCH_WRITE_RATE=N` =
throttle each writer to N toggles/s. Worker pinning fills one PU per physical core
first (hwloc), and every sweep caps total workers at the physical-core count so
**writers never share an SMT sibling** (`BENCH_ALLOW_SMT` to override). NUMA
interleaving of the shared structure is on by default. Size knobs (env):
`LIST_SIZE` stable nodes, `CHURN` churn nodes — which is *also* the transacted
index's slot count in `BENCH_RANDOM_POS`, capped at `LIST_SIZE` — and
`DURATION_SEC` per point.

Two defaults were chosen after the investigation below:
- **Segregated `rcu_head`** (default; `-DLIST_RCU_INLINE_RCU_HEAD` for the
  artifact build): the `rcu_head` lives *outside* the hot 24 B node and stable
  nodes are arena-packed, the way a metadata-segregating allocator behaves.
- **Per-CPU `call_rcu` workers** (default; `BENCH_NO_PERCPU_CALLRCU` to disable):
  one reclaim worker per hardware thread, each pinned to its writer's PU
  (`BENCH_RECLAIM_DOMAIN=hwthread|core|l3|single`), instead of liburcu's single
  global worker.

### Results

Hardware: 2× AMD EPYC 9654 (192 physical cores / 384 threads, 24 NUMA nodes, 8
cores/node). **The box was shared during measurement, so treat absolute numbers
as indicative — the order-of-magnitude shapes are what matter.** `LIST_SIZE=1000`,
`CHURN=200`.

#### Read-only ceiling — Mvisits/s, readers 1 → 383

| readers   |   1 |    32 |    96 |   191 |   383 |
|-----------|----:|------:|------:|------:|------:|
| `txn_sw_list`| 796 | 25478 | 74859 | **148724** | 235828 |
| `txn_list`| 785 | 25185 | 74054 | 147180 | 233661 |
| `rculist` | 729 | 23401 | 68956 | 136065 | 247247 |
| `seqlock` | 599 | 19240 | 58270 | 113916 | 166750 |
| `iscrw`   | 599 | 18600 | 45390 | 39983 | 55245 |
| `rwlock_r`| 600 | 18714 | 30502 | 35103 | 28639 |
| `rwlock_w`| 598 | 18721 | 30381 | 34909 | 28207 |
| `mutex`   | 599 |   407 |   392 |   285 |   267 |
| `fairmutex`| 598 |  219 |   193 |    35 |    42 |

- **The two bidir lists scale dead-linear to 192 cores and *beat* seqlock**
  (148 k vs 114 k @191), matching the forward-only `rculist` — so a coherent
  bidirectional reverse walk is **free** on the read side.
- `pthread_rwlock` plateaus ~30–35 k (its shared reader-count cacheline is the
  bottleneck); `iscrw` C-RW-WP does better but is still counter-bound.
- `mutex` (~400) and `fairmutex` (~40) **collapse** — "concurrent" readers take an
  *exclusive* lock, so they serialize; the MCS lock's futex park/wake is worst.

#### Why the node layout matters (`rcu_head` footprint)

seqlock used to *win* read-only — but only because every RCU node embedded a 16 B
`struct rcu_head` inline, bloating it 24 → 40 B so the cold reclamation metadata
polluted the traversal's cacheline working set (40 KB spills L1d; 24 KB fits). A
controlled test at 96 readers (`-DLIST_RCU_INLINE_RCU_HEAD` vs the segregated
default):

| LIST_SIZE | seqlock (24 B) | txn_sw_list **inline** 40 B | txn_sw_list **segregated** 24 B |
|-----------|---:|---:|---:|
| 1,000  | 55938 | 36391 | **74916** |
| 30,000 | 72496 | 16773 | **69775** |

Segregating the `rcu_head` is a **2–4× read speedup** and lifts the bidir lists to
match/beat seqlock. The seqlock "edge" was a node-layout artifact, not a read-path
advantage — which is why the segregated layout is the default. (A production
strided allocator that pairs hot data with cold metadata on separate cachelines
gets this for free.)

#### Read throughput with a concurrent writer

bidir scales to ~48 k Mvisits/s @191; `rwlock_r` also scales (~35 k) but only by
**starving the writer**; `rwlock_w` collapses to ~2.6 k (writers block readers in
bursts); `iscrw` is the honest middle (~19 k); `mutex`/`fairmutex` ~400/40;
**`seqlock` → ~0** (a 2000-node read section almost always overlaps the steady
writer and retries forever). **Caveat:** this mode conflates read-scaling with
each engine's *writer rate* — a cheaper/faster writer dirties reader cachelines
more (e.g. `rculist`'s writer is ~6× faster than bidir's, so its readers cap
lower). Use `BENCH_WRITE_RATE` to pin all engines to one mutation rate for a clean
comparison.

#### Writer scaling & allocation

`txn_list` is the **only** engine whose write throughput rises with concurrent
writers; everything else serializes. Two write workloads are measured below, and
in both the limiter is **reclamation, not the MCAS**: liburcu's single global
`call_rcu` worker funnels every deferred free and caps throughput at ~8 Mops/s.
Distributing reclaim to **one worker per hardware thread, each pinned to its
writer's PU** so the producer→consumer free stays CPU- and NUMA-local is ~5× and
is now the default (`BENCH_RECLAIM_DOMAIN=hwthread`; `BENCH_NO_PERCPU_CALLRCU` to
disable). With reclaim co-located, the *allocator* lever mostly closes on plain
churn — glibc's thread-cache keeps up once frees are local. `txn_list` write
Mops/s by allocator, writers 1/8/32/64/128/192 (this box: 192 cores / 384 PUs):

**Plain churn** (default `BENCH_WRITESCALE`) — each writer toggles churn nodes in
and out after spread anchors: a list insert/delete (2–3-edge MCAS), so the op is
allocation-dominated.

| writers | glibc | jemalloc `percpu_arena:phycpu` |
|---------|------:|------:|
| 1   | 3.6 | 7.8 |
| 8   | 22  | 42  |
| 32  | 38  | 50  |
| 64  | 40  | 39  |
| 128 | 48  | 52  |
| 192 | **100** | 82 |

**Random transacted index** (`BENCH_RANDOM_POS`) — each writer atomically toggles
a randomly chosen slot of an external index that points at list cells; the index
update folds into the same MCAS (the composable path), so there is more compute
per op. The index has exactly `CHURN` slots and each writer picks one uniformly,
so the per-slot collision rate is `~ writers / CHURN`. Best-of-2 (the
per-hwthread reclaim pin removed the high-writer bistability that used to require
best-of-3), current engine, `URCU_TXN_FALLBACK=256`:

| writers | glibc | jemalloc `percpu_arena:phycpu` |
|---------|------:|------:|
| 1   | 2.7 | 2.5 |
| 8   | 14  | 16  |
| 32  | 18  | 26  |
| 64  | 24  | **32** |
| 128 | 6.4 | 8.1 |
| 192 | 3.1 | 5.0 |

At the default `CHURN=200`, 192 writers collide ~1:1 on the index, so the random
path is **index-contention-bound** — it peaks near 64 writers and then *falls* (the
decline is contention on the 200-slot index, not an engine limit). **The high-writer
end used to collapse off a cliff:** under the former default retry budget
(`URCU_TXN_FALLBACK=64`) contending writers escalated *en masse* into the domain's
single serial fair lane, dropping to ~0.1 Mops/s (jemalloc 0.14 @128 / 0.12 @192;
glibc similar). Raising the budget to **256** — now the liburcu default — keeps them
on the parallel optimistic priority path: the cliff becomes the graceful decline in
the table above, and write latency improves at every percentile (median at 192
writers ~10 ms → sub-µs). Escalation still fires, just later, so starvation-freedom
is unchanged.

Enlarging the index — raise `CHURN`, capped at `LIST_SIZE`, so raise both — spreads
the collisions (so escalation rarely triggers regardless of the budget) and restores
writer scaling (jemalloc, best-of-2, write Mops/s):

| index slots (`CHURN`) | @64 | @128 | @192 |
|-----------------------|----:|-----:|-----:|
| 200 (default)         | 24  | 8.1  | 4.1  |
| 10 000                | 57  | 92   | 117  |
| 100 000               | 57  | 95   | **116** |
| 1 000 000             | 56  | 93   | 118  |

With a ≥100 k-slot index the composable random path scales to ~117 Mops/s at 192
writers — still *above* plain churn (jemalloc ~82) — confirming the fall-off at
`CHURN=200` is index contention (`writers / CHURN`), not the transaction engine.

The composable index path is ~30–40 % slower per op (the extra transacted slot).
The **allocator lever is now modest on both**: at 32 writers glibc→jemalloc is
38→50 (1.3×) for plain churn and 18→26 (1.4×) for the random index — co-locating
reclaim already removed the cross-thread free penalty the per-CPU allocator used
to hide, so the remaining gap is just arena bookkeeping.

Out to the full box (192 writers), **co-locating reclaim rewrites the allocator
story.** With the per-hwthread worker pinned to its writer's PU, the
producer→consumer free (writer allocates, worker frees) stays on one CPU's pool,
so glibc's thread-cache no longer eats a cross-CCX penalty: plain churn under
**glibc climbs to ~100 Mops/s @192, slightly *ahead* of the jemalloc + node-slab
pool (~82)** — the per-CPU allocator's per-arena bookkeeping stops paying once
frees are already local. (Before the pin, the reclaim worker drifted to a remote
CCX and glibc plateaued ~42 there; the per-CPU allocator had been hiding that
drift, not a fundamental cross-thread cost.) The `CHURN=200` random-index path is
index-contention-bound, not allocator-bound, landing ~3 (glibc) / ~5 (jemalloc)
at 192. 0 coherence violations at every point through 192 writers.

Both tables are on the **A-B-A-hardened** engine, whose per-record install latch
is now a single tri-state word (`FREE → BUSY → DONE`): the FREE→BUSY CAS is the
install lock and DONE is the install-once gate that closes a slot-value A-B-A
use-after-free — a doubly-linked next-pointer recurs `B → X → B`, and without the
gate a stalled helper re-plants a proxy *after* the transaction linearized,
resurrecting a freed node. The one-word try-lock replaced an earlier per-record
`cds_fair_mutex` (whose fairness was never load-bearing — starvation-freedom is
the per-domain escalation lane's job); it is a modest per-install cost, the engine
is use-after-free-free and ThreadSanitizer-clean, and its committed transactions
are linearizable at the single status-word commit (atomic across every structure
folded into that commit; see `<urcu/rcu-txn.h>`).

Takeaways on allocation: (1) RCU reclamation is a **producer→consumer cross-thread
free** (writer allocates, reclaim worker frees) — the worst case for per-thread
allocator caches, which is why an allocator swap alone *hurt* until reclaim was
distributed. (2) **Pinning** each per-CPU `call_rcu` worker to its writer's PU
keeps alloc and free on one CPU's pool — enough that even glibc's thread-cache
keeps up on plain churn, so a per-CPU allocator adds little there and matters only
where frees still cross threads. (3) `tcmalloc_minimal` is deliberately omitted: it is
gperftools' *per-thread*-cache build, not a per-CPU/rseq allocator, so it is the
worst case for the cross-thread frees above and not a meaningful per-CPU
comparison — the real per-CPU rseq allocator is Google's `google/tcmalloc` (Bazel
build, not packaged for distros), which would be a second per-CPU data point
alongside jemalloc. (4) The one lever this leaves — **pooling the MCAS descriptor
itself** — is the subsection below: it closes the jemalloc gap with *no* external
allocator. Still open after that: a per-CPU *node* pool (recycle nodes through
`call_rcu` to the owning CPU's arena, the way descriptors now are).

#### MCAS descriptor slab — closing the allocator gap, no external allocator

The remaining allocator cost is the **per-attempt MCAS descriptor**. Every
`txn_list` update `posix_memalign`s a descriptor (`struct urcu_mcas` — a header plus
its inline record array) and hands it to `call_rcu`; the reclaim worker frees it a
grace period later. That free is the **producer→consumer cross-thread** case again
(writer allocates, worker frees), so a per-thread malloc cache cannot recycle it —
and servicing every attempt from glibc's arena at 192 threads grows the arena, which
`mprotect`s, which serializes on the process-wide `mmap_lock`: an `osq_lock` storm
that ate ~54 % of runtime in a `perf` profile. Linking jemalloc (per-CPU arenas) was
what had been hiding this.

A **per-CPU size-classed superblock slab** (`<urcu/rcu-txn-slab.h>`) removes it with
no external allocator. One arena per `(record-count class, CPU)` — classes
`{4,8,16,32,64,128}` — is a wait-free-stack freelist over a bump pointer into
`mmap`'d 2 MiB superblocks. `free()` recovers a block's **origin** arena from its
superblock header (range-aligned, so `ptr & ~(RANGE−1)`), so a descriptor allocated
on CPU X and freed by X's pinned reclaim worker returns to X's arena — the
cross-thread free stays CPU-local with no central structure. Frees `cds_wfs_push`
(wait-free, never blocking the writer); the single pinned writer per arena pops under
the wfstack pop-lock. Growth is **capped by construction** — alloc reuses a freed
block before carving a new one — so the mapped set tracks peak live descriptors and
then recycles (92–99 % reuse). `URCU_TXN_NO_CACHE=1` disables it (back to
`posix_memalign`).

Same three allocators, best-of-2, this box, fresh run (shared box → absolute numbers
differ from the tables above; the shapes are the point):

![txn_list writer scaling by allocator: on plain churn glibc and the descriptor slab
lead while jemalloc's per-CPU arenas trail; on random-access writes the slab ties
jemalloc's per-CPU arenas and both lead glibc — so the slab is top-tier on both with
no external allocator](figures/list_scale_alloc.png)

**Plain churn** (2–3-edge MCAS, allocation-light once reclaim is co-located):

| writers | glibc | jemalloc `percpu_arena:phycpu` | glibc + descriptor slab |
|--------:|------:|------------------------------:|------------------------:|
| 1   | 9   | 6   | 6   |
| 8   | 51  | 31  | 40  |
| 32  | 51  | 49  | 52  |
| 64  | 41  | 39  | 41  |
| 128 | 54  | 51  | 50  |
| 192 | 103 | 81  | 104 |

**Composable random 100 k-slot index** (index + list folded into one MCAS — heavier,
more descriptor pressure):

| writers | glibc | jemalloc `percpu_arena:phycpu` | glibc + descriptor slab |
|--------:|------:|------------------------------:|------------------------:|
| 1   | 3   | 3       | 4       |
| 8   | 14  | 16      | 16      |
| 32  | 24  | 29      | 29      |
| 64  | 38  | 51      | 52      |
| 128 | 73  | 81      | 89      |
| 192 | 91  | **128** | **125** |

The two mallocs **split the workloads**, and that is the point. On **plain churn**
glibc leads (its thread-cache keeps up once reclaim is co-located) while jemalloc's
per-CPU arenas actually *cost* a little — 81 vs 103 Mops/s at 192. On the **heavier
composable path** it inverts: descriptor pressure makes glibc's single arena contend,
so jemalloc's per-CPU arenas win big — 128 vs glibc 91 at 192. Neither malloc is
top-tier on both. The **descriptor slab matches whichever malloc wins each** — glibc's
103 on churn (slab 104) and jemalloc's 128 on the composable path (slab 125) — so it
is the *only* option top-tier on both, with **no external allocator linked** and the
tightest run-to-run variance of the three (per-CPU arenas are deterministic: the two
composable reps at 192 landed 123.4 / 124.5). It does **not** beat jemalloc's per-CPU
arenas on the composable path — it *ties* them (an earlier draft compared against
jemalloc's *default* arenas by mistake); the win is doing so from glibc, with no
`percpu_arena` link and no regression on churn. Peak RSS over the 100 k sweep is
bounded and no worse than the mallocs (slab ~19 GB, glibc ~27 GB); that ~20 GB is the
allocator-independent `call_rcu` reclaim backlog — present, and worst, under plain
glibc — not the slab, and capping it needs reclaim backpressure (a separate concern).

The slab is upstreamed into the transaction engine's liburcu tree as a generic
component shared by the concurrent `rcu-mcas.h` and the single-writer `rcu-txn-sw.h`
(which folds its former two-part transaction — record array + lazy group block — into
one slab block). `URCU_TXN_CACHE_STATS` dumps reuse/footprint, but **measure with the
stats build off**: its per-op `uatomic_inc` counters share a cacheline and collapse
throughput ~3–4× at 192 threads (they make the slab look 3× *slower* than glibc — the
counters, not the slab).

#### RLU (Read-Log-Update) comparison

`rlu_list`/`rlu_hlist` run the *same* workloads as `txn_list`/`txn_hlist`, so the
MCAS transaction engine meets the reference multi-word-update scheme on identical
ground. RLU is shown in both modes — **defer** (`BENCH_RLU_WS=100`, batched
writeback, how RLU is meant to run) and **sync** (`BENCH_RLU_WS=1`, writeback every
section, the floor). Best-of-2, `DURATION_SEC=3`, jemalloc `percpu_arena:phycpu` for
both schemes, 0 coherence violations throughout.

![RLU vs txn (MCAS) across three workloads: disjoint writes (txn ~12× RLU-defer at
192), random-access writes on a hot 64-slot index (RLU wins ≤8 writers, txn owns the
16–32 mid-range, both converge at the full box), and hash-of-lists (txn ~1.8×
RLU-defer)](figures/rlu_vs_txn.png)

**Disjoint churn** — each writer owns a strided set of slots so writers almost
never collide (`LIST_SIZE=4096`, `CHURN=3072`, jemalloc, write Mops/s):

| writers      |   1 |  8 | 32 | 64 | 128 |     192 |
|--------------|----:|---:|---:|---:|----:|--------:|
| `txn_list`   | 6.0 | 29 | 66 | 100 | 125 | **185** |
| RLU-defer    |  15 | 34 | 22 |  26 |  20 |      16 |
| RLU-sync     |  18 | 10 | 7.6| 9.1| 8.2 |     7.9 |

RLU wins at 1 writer (its per-thread write-log + batched writeback is cheap
uncontended), but `txn_list` overtakes by ~16 writers and reaches **~12× RLU** at the
full box: disjoint slots never escalate, so every MCAS commit runs in parallel,
while RLU's global write-clock serializes commit ordering.

**Multi-slot random** (`BENCH_RANDOM_POS`, `LIST_SIZE=1000`, `CHURN=64` — a hot
64-slot index, ~`writers/64` collision):

| writers      |    1 |    8 |  16 |  32 |  64 |  192 |
|--------------|-----:|-----:|----:|----:|----:|-----:|
| `txn_list`   |  3.0 | 16.0 | 12.6| 11.8| 5.1 | 1.1  |
| RLU-defer    | 11.9 | 12.3 | 5.5 | 4.3 | 3.2 | 1.2  |
| RLU-sync     | 12.7 |  9.5 | 5.2 | 4.6 | 3.5 | 1.2  |

Under real contention the two trade places: RLU owns the low-writer regime (≤8),
`txn_list` owns the mid-range (16–32, ~2.3–2.7× RLU-defer — which falls off after 8
writers), and they converge at the high end (~1.1–1.2 Mops, within ~10 %). Both degrade
**gracefully**; the `txn_list` cliff of the previous section appears only under the
former escalation default (`URCU_TXN_FALLBACK=64`) — at the current `256` it tracks
RLU.

**Hash-of-lists** — RLU's native showcase; per-bucket escalation domains (write
Mops/s):

| writers      |   1 |   8 | 32 | 64 | 128 |    192 |
|--------------|----:|----:|---:|---:|----:|-------:|
| `txn_hlist`  | 0.9 | 6.5 | 12 | 17 |  24 | **29** |
| RLU-defer    | 1.3 | 9.1 | 11 | 15 |  16 |     16 |
| RLU-sync     | 1.3 | 4.6 | 4.5| 5.6| 6.3 |    6.4 |

Even on RLU's home turf `txn_hlist` scales to **~1.8× RLU-defer** at 192 writers
(per-bucket domains keep each queue shallow, so writers rarely serialize); RLU-defer
plateaus ~16 and RLU-sync ~6.4.

**Reads: near-parity on a tiny list, txn ahead on a real one.** On the 1 000-node
list read throughput is within ~15 % across all four (Mvisits/s @191 readers:
`txn_list` 74 k, `txn_sw_list` 79 k, RLU-defer 69 k, RLU-sync 76 k) — a short
traversal is dominated by per-op overhead, so RLU's per-node lock check barely shows.
But that check is a *per-visit* cost that does not amortize: on a representative
10 000-node list `txn_list` reads pull **1.6–1.8× ahead** (next subsection) — txn
amortizes per-op overhead over the longer walk while RLU pays validation on every
node. On the hash RLU reads still edge ~15 % ahead (136 k vs 118 k lookups/s @191).

(One benchmark bug surfaced here: the multi-slot-random RLU driver read a neighbour
pointer before `RLU_TRY_LOCK`-ing it, so a concurrent unlink+free at the same
position could drop a freed node into the write-set — an intermittent use-after-free
in RLU's writeback, core-confirmed. Locking the anchor before reading its edge
closes it; the disjoint-slot churn driver keeps the simpler read-before-lock form,
safe there because no peer ever touches the same node.)

#### Read, write & 50/50 scaling — representative working set (10k nodes, 2% updates)

The workload tables above each isolate one case on a tiny 1 000-node, cache-resident
list. This is the same bidir list at a **10 000-node** structure with a **2 % update
set** (200 churn nodes) — large enough that a read traverses a real, cache-pressured
span and each writer touches a spread-out node instead of hammering one in L1. Here
`txn_list` runs on its **shipping config, glibc + the descriptor slab** (no jemalloc —
it ties jemalloc's per-CPU arenas, see the allocator subsection); RLU runs on glibc.

![RLU vs txn read/write/50-50 scaling at 10k nodes, 2% updates: txn leads reads
~1.6-1.8x, scales writes to ~104 Mops/s while RLU collapses at scale, and dominates
the write half of a 50/50 mix](figures/rlu_vs_txn_rw.png)

**Read scaling** (readers + 1 writer, Gvisits/s):

| readers      |   1 |   8 | 32 | 64 | 128 |     191 |
|--------------|----:|----:|---:|---:|----:|--------:|
| `txn_list`   | 0.8 | 5.2 | 21 | 43 |  81 | **122** |
| RLU-defer    | 0.5 | 2.9 | 12 | 25 |  46 |      67 |
| RLU-sync     | 0.5 | 4.2 | 17 | 31 |  55 |      78 |

Reads scale linearly for all three, but **`txn_list` leads ~1.6–1.8×** (122 vs
RLU-defer 67 / RLU-sync 78 @191): RLU validates every dereferenced node against its
per-object lock/clock, a per-visit cost that grows with the traversal, so the
near-parity on the 1 000-node list was a small-list artifact.

**Write scaling** (writers only, Mops/s):

| writers      |   1 |  8 | 32 | 64 | 128 |     192 |
|--------------|----:|---:|---:|---:|----:|--------:|
| `txn_list`   | 5.7 | 40 | 53 | 42 |  50 | **104** |
| RLU-defer    |  20 | 51 | 15 | 14 | 7.8 |     5.8 |
| RLU-sync     |  18 | 11 | 7.9| 9.1| 8.3 |     8.1 |

RLU-defer wins at 1–8 writers (cheap batched writeback, uncontended); then its global
write-clock serializes commit order and it falls to ~6, while **`txn_list` scales to
104 — ~18× at the full box.**

**50/50 balanced** (T/2 readers + T/2 writers): reads stay close (RLU-sync's read path
even edges ahead at the top), but on the **write** half txn dominates — deferred
writeback stalls under a steady reader stream, and synchronous writeback (WS=1) nearly
stops:

| 50/50 @ total threads       |   2 |   8 |  32 |  64 | 128 |     192 |
|-----------------------------|----:|----:|----:|----:|----:|--------:|
| `txn_list` write (Mops/s)   | 4.6 |  14 |  38 |  44 |  32 |  **40** |
| RLU-defer write (Mops/s)    | 2.4 | 4.1 | 1.8 | 1.7 | 1.6 |     1.4 |
| RLU-sync write (Mops/s)     | 0.0 | 0.1 | 0.3 | 0.5 | 0.7 |     0.8 |
| `txn_list` read (Gvisits/s) | 0.8 | 3.0 | 4.4 | 8.1 |  13 |      16 |
| RLU-defer read (Gvisits/s)  | 0.5 | 1.7 | 2.9 | 5.6 |  10 |      14 |
| RLU-sync read (Gvisits/s)   | 0.5 | 2.1 | 6.6 |  11 |  15 |      17 |

#### RLU-paper hash benchmark (LWN [#667720](https://lwn.net/Articles/667720/)) — mixed % updates

The sections above use dedicated reader/writer threads. The RLU paper's canonical
benchmark instead has **every thread do a mix**: a hash table of **1 000 buckets ×
100 nodes/bucket** (100 000 keys over a 200 000-key range), each thread running
`(100−X)%` lookups + `X%` updates (toggle a random key), swept at X ∈ {0, 2, 20, 40}.
This is the workload McKenney reproduced in LWN #667720, whose result was: **RCU meets
or beats RLU everywhere, and RLU stops scaling as the update rate rises** (past ~32
threads at 20 %, ~16 at 40 %). We reproduce it (`BENCH_UPDATE_PCT=X`; one op = one lookup
or one update; total ops/s) and add the MCAS engine plus a lock-free hash:

- **`rcu_hlist`** — the article's actual baseline: RCU readers + a **per-bucket lock**
  for writers (the classic RCU hash-of-sorted-lists).
- **`txn_hlist`** — the MCAS transaction engine (glibc + descriptor slab).
- **`lfht`** — liburcu's `cds_lfht`, **pinned to 1024 fixed buckets, no auto-resize**, so
  it walks the same ~100-node chains as the others. This deliberately denies cds_lfht its
  whole design point (it is built to auto-resize so chains stay ~O(1)); it is here as an
  equal-chain *mechanism* comparison, not cds_lfht as you would deploy it.

RLU is glibc; the four sorted-list engines share the same key stream and sentinels.
Best-of-2, `DURATION_SEC=3`, 0 coherence violations across all 240 points.

![LWN #667720 hash benchmark reproduced and extended: read-only all five tie; as the
update rate rises RCU / cds_lfht / txn scale to 192 while RLU-defer walls (~32 threads at
20 %, ~16–32 at 40 %) and RLU-sync stays flat](figures/lwn667720_hash.png)

**Total ops/s at 192 threads (full box):**

| engine                    |  0% |  2% | 20% | 40% |
|---------------------------|----:|----:|----:|----:|
| `rcu_hlist` (RCU + lock)  | 257 | 138 |  76 |  51 |
| `lfht` (cds_lfht)         | 257 | 136 |  75 |  55 |
| `txn_hlist` (MCAS)        | 257 | 129 |  62 |  44 |
| RLU-defer                 | 267 | 105 |  38 |  26 |
| RLU-sync                  | 267 |  51 |  16 |  11 |

**Total ops/s at 64 threads (the article's 4-socket box):**

| engine                    |  0% |  2% | 20% | 40% |
|---------------------------|----:|----:|----:|----:|
| `rcu_hlist` (RCU + lock)  |  88 |  61 |  43 |  33 |
| `lfht` (cds_lfht)         |  88 |  60 |  42 |  32 |
| `txn_hlist` (MCAS)        |  88 |  59 |  37 |  27 |
| RLU-defer                 |  91 |  59 |  31 |  23 |
| RLU-sync                  |  91 |  43 |  16 |  11 |

**Read-only (0 %)** the five are identical — equal chains, pure RCU-class read scaling to
~257–267 Mops (RLU marginally top, ~4 %). As updates appear the mechanisms separate and
the article's headline holds: **RLU-sync breaks first** (flat ~50 by 2 %), and
**RLU-defer walls** exactly where the paper says — barely moving past 32 threads at 20 %
(31 → 37 → 38 from 32 → 192) and past ~16–32 at 40 %. Meanwhile **RCU, `cds_lfht` and txn
all scale to the full 192 cores.**

The new result is that **`txn_hlist` tracks the RCU baseline far more closely than RLU
does** — within ~7 % at 2 % (129 vs 138), ~18 % at 20 %/40 % — and scales where RLU
cannot, because at these update rates the RCU-class read path dominates and txn pays MCAS
only on the X % of ops that write. `cds_lfht` ties `rcu_hlist` at 2–20 % and **overtakes
it at 40 %** (54 vs 51): once writes are frequent its lock-free updates beat the per-bucket
lock. So on RLU's own benchmark the ranking is **RCU ≈ lfht ≥ txn ≫ RLU-defer ≫
RLU-sync** — txn joins RCU and `cds_lfht` on the scaling side of the "RLU doesn't scale
writers" line. (Bucket count is held constant here, so this isolates the mechanism; a
follow-up shrinks the bucket count to stress *write* contention directly.)

#### Hash-of-lists: dedicated reader/writer scaling (all five engines)

The same 1 000 × 100 hash under the harness's dedicated-thread modes (rather than the
per-thread mix above): write-only, read-only-under-a-writer, and a 50/50 split, across all
five engines. Best-of-2, `DURATION_SEC=3`, txn on glibc + descriptor slab, the rest glibc,
0 violations.

![Dedicated reader/writer hash scaling, five engines: writes lfht > txn ≈ rcu ≫ RLU;
reads all parity; 50/50 writes lfht ≈ rcu > txn ≫ RLU, but 50/50 reads led by RLU-sync
because its writers stall](figures/hash_dedicated_rw.png)

**Write scaling** (writers only, write Mops/s):

| writers      |   1 |   8 | 32 | 64 | 128 |     192 |
|--------------|----:|----:|---:|---:|----:|--------:|
| `lfht`       | 1.0 | 6.7 | 14 | 21 |  28 |  **34** |
| `txn_hlist`  | 0.9 | 6.1 | 11 | 16 |  22 |      28 |
| `rcu_hlist`  | 1.2 | 7.8 | 14 | 19 |  22 |      25 |
| RLU-defer    | 1.3 | 8.2 | 10 | 14 |  16 |      16 |
| RLU-sync     | 1.2 | 4.3 | 4.3| 5.5| 6.3 |     6.3 |

`lfht`'s lock-free updates top the write axis; `txn_hlist`'s MCAS and `rcu_hlist`'s
per-bucket lock scale together just behind (txn edges rcu past ~128 writers); RLU-defer
plateaus ~16 (global write-clock) and RLU-sync ~6. Everything but RLU scales.

**Read scaling** (readers + 1 writer): all five within ~6 % — **125–132 Mvis/s @191**,
linear RCU-class reads (RLU marginally top). On a short ~100-node chain the per-node-cost
differences that separate the engines on long list traversals wash out.

**50/50 balanced** — the read/write tradeoff is starkest here. On the **write** half
`lfht`/`rcu`/`txn` (23 / 20 / 18 Mops @192) dominate RLU-defer (10) and RLU-sync (2.7). But
on the **read** half **RLU-sync leads (36 Mvis/s @192)** — precisely *because* its writers
are nearly stalled: no pending write-sets means `RLU_DEREF` takes its fast path and readers
run uninterfered, whereas txn's fast writers dirty reader cachelines and give it the lowest
50/50 reads (15). RLU-sync buys read throughput by forfeiting writes; txn/rcu/lfht keep both
moderate and balanced.

### Takeaways

- Coherent **bidirectional** RCU iteration is **free** vs forward-only `rculist`,
  and with a sane node layout the bidir lists are the **fastest read path here**,
  scaling linearly to 192 cores — while locks/seqlock cannot offer a safe reverse
  walk at all.
- `txn_list` is the only design whose **writers scale**; per-CPU reclaim +
  allocator is the lever (~5×), not the MCAS install (whose per-record latch is a
  modest, A-B-A-safe cost).
- The classic reader/writer-preference rwlock tradeoff is stark (reader-pref
  scales reads but starves writers; writer-pref collapses reads); **seqlock is
  unusable for long read-side traversals under a steady writer.**
- The random path's high-writer **collapse was premature escalation**, not the
  engine: the former default retry budget (`URCU_TXN_FALLBACK=64`) funnelled
  contending writers into the domain's single serial fair lane. Raising it to
  **256** (now the liburcu default) keeps them on the parallel optimistic path —
  the cliff becomes graceful degradation, and write latency improves at every
  percentile through p99.9.
- Against reference **RLU**: RLU wins uncontended / at very low writer counts (its
  batched writeback), but `txn_list` **scales past it with writers** — ~12× on
  disjoint churn, ~18× on a 10k-node/2%-update write sweep, ~1.8× on the hash — and
  on a representative (non-tiny) list its **reads also lead ~1.6–1.8×** (RLU pays a
  per-node validation the tiny-list microbench hid). RLU stays competitive only in
  the low-writer / read-mostly-on-tiny-structures corner.
- On **RLU's own paper benchmark** (LWN #667720: a 1 000 × 100 hash with per-thread
  `%` updates) the reproduction holds — RCU meets-or-beats RLU and RLU stops scaling
  past ~16–32 writers — and `txn_hlist` lands with `rcu_hlist`/`cds_lfht` on the
  *scaling* side of that line (within ~7–18 % of the RCU baseline, far above RLU),
  while `cds_lfht` — pinned to equal chains, denying it its resize design point — edges
  ahead once updates are frequent.

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
src/bench_list_scale.c           bidirectional RCU list scaling benchmark
                                   (txn_sw_list/txn_list vs mutex/rwlock/seqlock/iscrw)
src/bench_iscrw.c                isolation wrapper linking the real bind9 isc_rwlock
src/iscrw-shim/probes-isc.h      no-op SystemTap probes shim for that standalone build
datasets/                        names CSVs (1M shuffled / trie-sorted + smoke)
urcu-build/                      our liburcu clone (fractal-trie-dev), gitignored
urcu-bidir-build/                our liburcu clone (rcu-bidir-list-dev), gitignored
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
