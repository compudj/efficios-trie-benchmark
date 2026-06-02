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
| `judy`     | JudyL / JudySL                                  | system `libJudy`                |
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
#   engine:  ft_eager ft_eager_on_spec ft_cand ft_spec judy qp art
# output: <ns/op> <RSS_kB>      ('-' for string engines on integer datasets)
```

Example sweep:

```sh
for e in ft_eager ft_eager_on_spec ft_cand ft_spec judy qp art; do printf '%-17s ' "$e"; ./bench_one_st dns "$e"; done
```

Useful env vars (see `src/bench_one_st.c`): `FT_BENCH_COMPACT` (compact between
build and query), `FT_DUMP_STATS`, `N_KEYS` / `WARMUP` / `RUNS` (compile-time).

### The qp-trie `qp` vs `fn` gotcha

qp-trie dispatches through `Tbl.o`, which links with **exactly one** backend
object (`qp.o`, `fn.o`, ...) — all export the same symbols, so the linker takes
whichever you pass. We vendor and link **only `qp.o`**, so the `qp` engine is
always the real qp-trie. (The original `build_one_st_bench.sh` linked `fn.o`,
silently benchmarking a different structure under the `qp` label.)

## Multithreaded benchmark — bind9 `load-names`

The MT scaling test is bind9's "lookup names" benchmark (`load-names.c`), which
compares the Fractal Trie against BIND9's own QP-trie (`qp_il`, `qp_local`)
under a reader/writer thread sweep, with an optional `FT_PRIME` cache-priming
phase. The FT engines form the 2×2 of build attr × lookup — `ft_eager`
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
LD_LIBRARY_PATH=urcu-build/src/.libs FT_PRIME=1 \
  bind9-src/build/tests/bench/load-names datasets/names-1M-shuf.csv
```

`load-names` takes the CSV as its single argument and, by default, sweeps the
thread counts `1 2 4 8 16 32 64 96 128 192`, pinning worker `i` to CPU `i`
(distinct physical cores up to 192 on a 2×96-core EPYC). `BENCH_THREADS=N`
restricts the sweep to a **single** thread count `N` (it is parsed as one
integer, not a list — handy with a high `QUERY_LOOPS` for clean perf-stat
runs). Other env vars: `QUERY_LOOPS`, `BENCH_ENGINE` (filter),
`BENCH_CACHE_FLUSH_MB`, `FT_BENCH_CHURN`, `FT_BENCH_COMPACT`.

**Why `LD_LIBRARY_PATH`:** bind9's own libraries link the system `liburcu-cds`
(found via pkg-config) and pull it in transitively; without our build's `.libs`
first on the library path, an older system `liburcu-cds` is loaded and the
newest FT symbols are missing. `make bind9` prints the exact command to use.

### Result — FT vs BIND9 QP-trie at 192 cores

Lookup throughput on 1M DNS names (`datasets/names-1M-shuf.csv`), comparing the
Fractal Trie reference engine `ft_spec_il` (speculative descent + library-side
memcmp validation) against BIND9's `dns_qpmulti` (`qp_il`), apples-to-apples:
both use the same NUMA-interleaved (`il`) leaf/payload placement.

| Engine        | Query throughput @ 192 cores | vs BIND9-QP |
|---------------|------------------------------|-------------|
| `ft_spec_il`  | **≈ 1257 Mops/s** (1072–1279) | **≈ 3.1×**  |
| `qp_il`       | ≈ 405 Mops/s (391–419)        | 1×          |

Median of 5 runs (min–max in parentheses); `QUERY_LOOPS=1`, `FT_PRIME=1`.
`qp_il` and `qp_local` are statistically tied at this scale (≈ 405 vs 410 Mops/s
median, overlapping ranges). The FT lead grows with core count — ≈ 1.3× at 1
thread, ≈ 3× at 192 — because the FT RCU read path dirties no shared memory,
while BIND9-QP's read path write-shares and stops scaling past ~96 threads.

**Hardware:** 2× AMD EPYC 9654 (Zen 4 "Genoa"), 96 cores/socket = **192
physical cores**, SMT2 = 384 logical CPUs, 2 sockets, 24 NUMA nodes. The
benchmark pins worker `i` to CPU `i` (CPUs 0–191 = one thread per physical
core), so the 192-thread point runs one worker per physical core (private
L1/L2/FPU, no SMT-sibling contention).

## Multithreaded benchmark — read/write scaling (per engine)

This test runs **one writer** doing continuous insert/remove churn while **N
reader** threads look up keys, comparing the Fractal Trie against Judy,
qp-trie, ART, and BIND9's QP-trie. So each structure's resident-set size (RSS)
can be measured in isolation, **each engine is its own executable** — one
process holds exactly one trie:

| Executable          | Engine                          | Links             |
|---------------------|---------------------------------|-------------------|
| `bench_scale_ft`    | Fractal Trie (candidate+memcmp) | liburcu           |
| `bench_scale_judy`  | JudySL, rwlock                  | libJudy           |
| `bench_scale_qp`    | qp-trie, rwlock                 | vendored qp-trie  |
| `bench_scale_art`   | ART, rwlock                     | vendored libart   |
| `bench_scale_b9qp`  | BIND9 `dns_qpmulti`, RCU        | liburcu + bind9   |

They share `bench_scale_common.c` (key generation, RSS sampling, the
thread-sweep driver); each `bench_scale_<engine>.c` supplies that engine's
build / lookup / churn callbacks and a thin `main`. **Only `bench_scale_b9qp`
links bind9.**

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
third_party/{qp-trie,libart}/    vendored competitors
datasets/                        names CSVs (1M shuffled / trie-sorted + smoke)
urcu-build/                      our liburcu clone (fractal-trie-dev), gitignored
bind9-src/                       our bind9 clone + overlay + build, gitignored
scripts/build-bind9.sh           clones/overlays/builds the bind9 MT benches
scripts/run_scale_rw.sh          runs the per-engine scaling benches, combined table
```

## Licensing of vendored code

- `third_party/qp-trie` — CC0 / public domain (Tony Finch). See `NOTICE`.
- `third_party/libart` — BSD-2-Clause (Armon Dadgar). See `LICENSE`.
