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
#            qp art rax hot cuckoo masstree artolc
# output: <ns/op> <RSS_kB>   ('-' where an engine does not apply: judysl on
#         integers, judyl on strings)
```

Example sweep (all engines that apply, on one dataset):

```sh
for e in ft_eager ft_spec judyl judysl judyhs qp art rax hot cuckoo masstree artolc; do \
  printf '%-10s ' "$e"; numactl --physcpubind=40 --membind=5 ./bench_one_st dns "$e"; done
```

**Pin it and bind its memory** (`numactl --physcpubind=<cpu> --membind=<that
cpu's node>`), as above. The benchmark does not pin itself, and on this
2-socket box an unpinned run is a NUMA lottery: with all memory on the other
socket every engine is ~50% slower (`dns`: `ft_spec` 123 → 182 ns, `qp`
116 → 174 ns). FT is the most exposed, because by default it interleaves its
node arena across all NUMA nodes, and so does the `cds_ft_external_arena`
that holds the string query keys and most engines' leaves; an explicit process
policy such as `--membind` is honored instead. Unpinned and unbound, the same
`ft_spec` binary measured anywhere from 134 to 178 ns on `dns`.

Useful env vars (see `src/bench_one_st.c`): `FT_BENCH_COMPACT` (compact between
build and query), `FT_DUMP_STATS`, `N_KEYS` / `WARMUP` / `RUNS` (compile-time).

### Results across datasets (1M keys, single thread)

Lookup time, ns/op: the median of 3 runs, each the best of `RUNS` timed passes
after `WARMUP`, every run pinned and node-bound (`numactl --physcpubind=40
--membind=5`), on the hardware below (2× EPYC 9654; `cuckoo`, `art`,
`masstree`, `rax` built `-O3` — `cuckoo` and `rax` also `-flto`, `cuckoo` with
2 MiB hugepages; every other engine, including FT, is `-O2`, which it
saturates: `-O3`/LTO move them <2% — see opt-level note below). FT is the
transaction-engine tree, `ft-txn-integ` @ 9484d87e. Raw per-run data:
`scripts/one_st_lookup.csv` (its `ft_*_june` rows are the same harness linked
against the June `fractal-trie-dev` library, 80a7013f, as an A/B).
**How to read these tables.** The comparison that matters is among the engines
that support concurrent readers and writers: `ft_spec` / `ft_eager` (RCU),
`artolc`, `masstree` and `cuckoo` (built `MULTITHREADING`; its API has no
delete). The rest — `hot` (its single-threaded variant), `wormhole` (through
its thread-unsafe API), `qp`, `art`, the Judy variants and `rax` — are
single-threaded structures, there as upper-bound references for what a lookup
costs without any concurrency support. The concurrent variants of HOT, ART and
qp are measured in the multithreaded sections below.

Every engine now runs on every dataset — the byte-keyed engines key integers as
big-endian bytes; `judysl` (string radix) and `judyl` (integer array) are the
two split-out Judy variants, `judyhs` is Judy's hash array.

**String keys** (`dns` DNS names, `dict` words, `paths` filesystem paths),
fastest-first by `dns`:

![Single-threaded string-key lookup latency: hot fastest (74–108 ns) with
wormhole, qp and ft_spec close behind; rax and cuckoo slowest at ~3×; FT's
ft_spec beats ft_eager on every dataset](figures/st_lookup_strings.png)

| Engine     | `dns` | `dict` | `paths` |
|------------|------:|-------:|--------:|
| `hot`      |    98 |    108 |      74 |
| `wormhole`†|   114 |    102 |     113 |
| `qp`       |   117 |    128 |     177 |
| `ft_spec`  |   123 |    115 |     145 |
| `judyhs`‡  |   143 |    120 |     147 |
| `ft_eager` |   166 |    169 |     210 |
| `art`      |   184 |    168 |     219 |
| `judysl`   |   201 |    249 |     263 |
| `artolc`   |   212 |    210 |     243 |
| `masstree` |   232 |    190 |     208 |
| `rax`      |   297 |    277 |     326 |
| `cuckoo`   |   334 |    301 |     330 |

**Integer keys** (`u32/u64` × `d`ense sequential / `s`parse random),
fastest-first by `u64d`:

![Single-threaded integer-key lookup latency: judyl and art win dense keys
(11 ns) but degrade 4–6× on sparse; qp is flat at ~13 ns on all four sets;
ft_spec runs 16–35 ns; rax sits mid-pack; masstree and cuckoo
trail](figures/st_lookup_ints.png)

| Engine     | `u32d` | `u32s` | `u64d` | `u64s` |
|------------|-------:|-------:|-------:|-------:|
| `judyl`    |     11 |     38 |     11 |     64 |
| `art`      |     11 |     46 |     12 |     48 |
| `qp`       |     13 |     13 |     13 |     13 |
| `ft_spec`  |     16 |     35 |     17 |     35 |
| `ft_eager` |     14 |     43 |     17 |     45 |
| `hot`      |     20 |     50 |     20 |     49 |
| `artolc`   |     22 |     97 |     23 |     99 |
| `rax`      |     29 |     85 |     29 |     94 |
| `judyhs`‡  |     24 |     47 |     38 |     78 |
| `wormhole`†|     35 |     88 |     38 |     91 |
| `masstree` |     46 |    173 |     47 |    171 |
| `cuckoo`   |     95 |     98 |    117 |     98 |

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
- **Among the concurrency-capable engines FT is the fastest on every
  dataset** (`ft_spec` on six, `ft_eager` on `u32d`), winning all seven
  against each of `artolc`, `masstree` and `cuckoo`; the next-fastest of those
  is 1.4–2.8× slower (`dns`: `artolc` 212 vs 123; `u64s`: `cuckoo` 98 vs 35).
  Against the single-threaded upper bound, `ft_spec` is 1.1–1.3× on `dns` /
  `dict`, 1.9× on `paths`, 1.5× on dense integers and 2.7–2.8× on sparse
  integers.
- **FT's two validating modes**: `ft_spec` (speculative — skip-compressed
  encoding, one end-of-walk `memcmp`) is the better default, beating `ft_eager`
  (eager-optimized — per-step exact compares on compressed bytes) on strings
  (`dns` 123 vs 166) and sparse integers (`u64s` 35 vs 45); on dense integers
  the two are level (`u64d` 17 vs 17) or `ft_eager` is ahead (`u32d` 14 vs
  16). Both return validated results — `ft_cand` (the raw,
  *unvalidated* candidate primitive) is excluded from these tables since it skips
  the compare every other engine pays.
- **`qp` is uniquely distribution-insensitive on integers** — ~12–13 ns on *all
  four* sets, including the sparse random ones where everything else degrades 2–6×
  (`judyl` 11→64, `ft` 17→35, `art` 12→48). Its bit-popcount nodes don't care
  whether keys cluster.
- **`judyl` wins dense integers** (11 ns) but collapses on sparse (64); **`judyhs`
  beats `judysl` on strings** (hash suits these distributions better than the
  radix tree), and **`hot` has the fastest string lookups** (74–108 ns).
- **`rax` (Valkey's radix tree) is second-slowest on strings** (277–326 ns,
  2.3–2.4× `ft_spec`) and in the lower half on integers (29 ns dense, 85–94
  sparse): a byte-at-a-time descent over individually `malloc`'d nodes, with a
  `memchr` over each node's edge bytes. Its RSS is lower — 230 MB on `dns`
  against `ft_spec`'s 404 MB (glibc `malloc` here, not Valkey's jemalloc) —
  but RSS is not the cache-hot working set: FT's strided allocator keeps
  per-node metadata in separate pages that a lookup never touches, so what
  decides speed is already in the latency column.
- **`cuckoo` is slowest on strings and dense integers** (its trie nodes live in
  a cuckoo hash table — extra hashing and bucket probes per descent step);
  **Masstree and ART-OLC carry their concurrency machinery** even single-threaded,
  so they trail the dedicated ST engines (ART-OLC the closer of the two).
- **Opt level: only `art` profits from `-O3`** (~11% on strings, ~18% on dense
  integers — its node-256 scan and path-compression loops unroll/vectorize), so
  it's built `-O3` like `cuckoo`; at `-O3` it even edges past FT on dense ints
  (`u64d` 12 vs FT's 17). `masstree` gains a marginal ~3% (also `-O3`).
  `qp`, `hot`, ART-OLC, **and FT** are flat (<2% across `-O2`/`-O3`/`-flto`,
  measured interleaved 30×) — pointer-chasing radix walks are latency/cache-bound
  and already saturated at `-O2`; FT additionally hand-codes its `popcnt`/`bmi`
  hot path. LTO buys nothing (each engine is effectively a single TU).
- Median of 3 pinned, node-bound runs, each best-of-`RUNS`; the spread across
  a cell's three runs has a median of 0.9% (90% of cells under 4.2%; worst
  15%, `judyl` `u64s`).
- **The transaction-engine FT costs lookups little**: against the June
  `fractal-trie-dev` library (80a7013f) on the same harness, `ft_spec` is
  +1–2% on strings (`dns` 120.5 → 123.2), +4–6% on dense integers (`u32d`
  15.4 → 16.3) and unchanged on sparse integers. Its footprint did grow: RSS
  350 → 404 MB on `dns`, 162 → 206 MB on `u32d`.

> **Validation fairness.** Every engine stores its own **copy** of each key
> (FT/qp/ART/Masstree/ART-OLC in a dense `cds_ft_external_arena`; Judy/Cuckoo
> internally; rax in its own nodes, returning a cold arena `kv_entry` like
> qp/ART; HOT in the same arena) and the timed loop consumes the lookup
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

The multithreaded figures in this README were measured against the
transaction-engine FT (`ft-txn-integ` @ 9484d87e), not `config.mk`'s default
branch. To reproduce them, build that branch into its own tree and point both
the build and the run at it:

```sh
U=$PWD/urcu-txn-build-ft-9484d87e
make urcu  URCU_BRANCH=ft-txn-integ URCU_BUILD=$U
make bind9 URCU_BUILD=$U
LD_LIBRARY_PATH=$U/src/.libs bind9-src/build/tests/bench/bench_scale_ft 192
```

Raw per-run data for every multithreaded table: `scripts/mt_scale.csv`.

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

![FT vs BIND9 QP at 192 cores: ft_spec_il ≈1241 Mops/s vs qp_il ≈902 — about
1.4×, whiskers spanning the min–max of 5 runs](figures/loadnames_ft_vs_qp.png)

| Engine        | Query throughput @ 192 cores | vs BIND9-QP |
|---------------|------------------------------|-------------|
| `ft_spec_il`  | **≈ 1241 Mops/s** (1212–1264) | **≈ 1.4×** |
| `qp_il`       | ≈ 902 Mops/s (842–1084)       | 1×          |

Median of 5 runs (min–max in parentheses), `QUERY_LOOPS=1`, priming on, FT at
`ft-txn-integ` @ 9484d87e. Across the thread sweep the FT lead **grows from
≈ 1.1× at 1 thread to ≈ 1.4× at 192** (1.16× at 64, 1.32× at 128) — FT scales
better at the top because its RCU read path dirties no shared memory while
BIND9-QP's read path write-shares. `qp_il` has the higher run-to-run variance
of the two. (`qp_local` was not re-measured.)

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
byte-prefix-free keys). Median of 5 runs, query Mops/s (fresh process per thread
count):

![load-names thread sweep: FT-spec leads at 64, 128 and 192 threads, 1.6×
HOTRowex at 192; both ARTs trail at about a third of FT; Masstree falls back
past 128](figures/loadnames_scaling.png)

| Threads | `ft_spec_il` | `hotrowex` | `artolc` | `artrowex` | `masstree` |
|--------:|-------------:|-----------:|---------:|-----------:|-----------:|
| 1       | 4.9          | **6.5**    | 4.3      | 4.2        | 4.2        |
| 64      | **289**      | 258        | 165      | 164        | 156        |
| 128     | **710**      | 501        | 309      | 307        | 251        |
| 192     | **1241**     | 760        | 457      | 453        | 178        |

**FT-spec leads from 128 threads up; at 64 it depends on placement.** HOTRowex
is ahead single-threaded (6.5 vs 4.9 Mops/s), FT-spec is 1.4× ahead at 128 and
1.6× at 192. This is the **inverse** of the random-access read/write
`bench_scale` result (where HOTRowex leads at 192) — load-names does
*sequential* lookups (prefetch-friendly) on real qpkeys with FT's leaf slots
round-robin **interleaved** across NUMA nodes. The 64-thread row is the one to
read with care: see the caveat below.

**The two ARTs are level** (457 vs 453 at 192) and trail both radix tries. Their
gap to `ft_spec_il` is not a constant factor here: 1.1× behind at 1 thread, 1.8×
at 64, 2.7× at 192. ART-OLC goes 4.3 → 165 → 309 → 457, about 106× from 1 to 192
threads.

**Masstree does not scale here** — it peaks at 128 (251) and falls back at 192
(178 median, with two of five runs at ~335). On the random-access `bench_scale`
sweep it does scale (373 @ 192), so the stall is specific to this workload.

> **Caveat — node placement.** The HOT, Masstree, and ART *internal nodes* are
> first-touched by their building thread (none exposes an allocator hook, so
> unlike FT's leaf arena they cannot be `mbind`-interleaved); the key copies
> they validate against *are* interleaved. In this sweep the builder ran on
> NUMA node 23 (socket 1) in every run, while the first 96 readers sit on
> socket 0. Forcing the placement with `numactl` moves HOTRowex at 64 threads
> between 246 (nodes on socket 1) and 323 (nodes on socket 0) Mops/s, against
> FT-spec's 289 — so which of the two leads at 64 is decided by placement. At
> 192 threads it makes no difference (737–757 whether interleaved or preferred
> on either socket).

> **Against the June figures.** The four C++ engines measured 25–35% higher at
> 192 threads in June (HOTRowex 1021, ART-OLC 685, ART-ROWEX 701, Masstree
> 267), before the hwloc pinning change; `ft_spec_il` and `qp_il` are where
> they were (1246 and 938 then). Placement does not account for it (above),
> and the cause is not established.

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

**Concurrent writers (FT engine).** `BENCH_WRITERS=N` runs N churn writers
instead of one, on the cores past the readers. Writer `w` of `n` owns the churn
keys `{w, w + n, w + 2n, ...}`, so no two writers ever toggle the same key; with
`n = 1` this is the single writer exactly as before (same keys, same seed).
`BENCH_THREADS=0` then gives a writers-only run. `BENCH_FT_WRITER=fine|coarse|external-sync`
picks the trie's writer strategy, and the FT library's `CDS_FT_LOCK_SPACING`
(per-node / exponential / root-only, when built with
`-DFEATURE_FT_LOCK_SPACING_ENV`) its lock granularity. The application mutex
the churn always took is then held only for `external-sync`, whose contract asks
the application for writer exclusion; `fine` and `coarse` exclude their own
writers, and each op runs inside an RCU read-side section because a peer now
retires nodes this writer's descents cross. Both knobs need a liburcu with the
writer-strategy API and the engine compiled with `-DBENCH_FT_WRITER_STRATEGY`;
without it the FT engine is the single-writer one and refuses `BENCH_WRITERS > 1`
(as do the other engines). For example, built standalone against a liburcu
build tree `$U`:

```sh
gcc -O3 -march=native -mpopcnt -msse4.2 -DNDEBUG -DBENCH_FT_WRITER_STRATEGY \
    -Ibind9-overlay/tests/bench -I$U/include -I$U/../include \
    bind9-overlay/tests/bench/bench_scale_{ft,common}.c \
    bind9-overlay/tests/bench/bench_topology.c -o bench_scale_ft_mw \
    -Wl,-rpath,$U/src/.libs $U/src/.libs/liburcu-cds.so $U/src/.libs/liburcu.so \
    -lhwloc -lnuma -lpthread
BENCH_THREADS=0 BENCH_WRITERS=8 BENCH_FT_WRITER=fine CDS_FT_LOCK_SPACING=per-node \
    ./bench_scale_ft_mw 64
```

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

![bench_scale read scaling with and without a churn writer, plus RSS:
HOTRowex leads reads (~1.1× over FT) and footprint (113 MB); FT and FT-QSBR
are second within ~1% of each other, 1.4× ahead of ART-OLC, ART-ROWEX and
Masstree; FT's RSS is the largest at 434 MB](figures/scale_rw_reads.png)

**1 writer + N readers** — a writer churns insert/remove the whole window;
readers cap at 191 so reader + writer = 192 threads. Medians (5 runs/cell for
the two FT builds and HOTRowex, 3 for the others), read Mops/s, FT at
`ft-txn-integ` @ 9484d87e:

| Readers | `ft` | `ft_qsbr` | `hotrowex` † | `artolc` | `artrowex` | `masstree` |
|--------:|-----:|----------:|-------------:|---------:|-----------:|-----------:|
| 64  | 188 | 179 | 202 | 128 | 129 | 127 |
| 96  | 280 | 268 | 309 | 191 | 192 | 187 |
| 128 | 364 | 359 | 410 | 256 | 255 | 249 |
| 191 | 529 | 532 | **598** | 377 | 373 | 363 |
| RSS | 434 MB | 419 MB | **113 MB** | 144 MB | 144 MB | 186 MB |

**Readers only** (`BENCH_NO_WRITER`, no concurrent mutation; readers reach 192).
Same run counts, read Mops/s:

| Readers | `ft` | `ft_qsbr` | `hotrowex` | `artolc` | `artrowex` | `masstree` |
|--------:|-----:|----------:|-----------:|---------:|-----------:|-----------:|
| 64  | 193 | 194 | 202 | 138 | 134 | 136 |
| 96  | 289 | 291 | 302 | 209 | 199 | 202 |
| 128 | 382 | 383 | 402 | 276 | 266 | 261 |
| 192 | 560 | 561 | **610** | 409 | 402 | 373 |

At 192 the order is **HOTRowex > FT ≈ FT-QSBR > ART-OLC > ART-ROWEX > Masstree**.
HOTRowex leads reads (~1.1× over FT: 1.09× readers only, 1.13× with the writer)
and footprint (113 MB), but FT is second and **1.4× ahead of all three
ART/Masstree variants** — and it is the only engine doing full concurrent
insert **and** remove under RCU (HOTRowex's ROWEX has no concurrent delete; it
churns by `upsert`). FT's RSS is the largest (434 MB). About 45 MB of it is the
default-on ordered list (390 MB with `FT_NO_ORD=1`); and RSS overstates what a
lookup touches, since the FT allocator keeps per-node metadata in separate
pages the read path never loads.

> **This result depended on getting the measurement right.** On top of the
> earlier fairness fixes — every reader now *validates* against a key copy in a
> dense arena (an un-validated reader once let the compiler dead-code-eliminate
> the compare; HOTRowex once stored pointers into the shared query buffer and
> kept no copies) — four later bench bugs had depressed FT's numbers, all now
> fixed: (1) **no thread pinning** — unpinned, the scheduler stacked readers on
> SMT siblings and left physical cores idle, and FT's larger footprint paid the
> contention most; (2) under QSBR the **churn writer ran online**, stalling the
> grace periods that reclaim removed nodes; and (3) `rcu_barrier()` was **gated
> on `FT_BENCH_COMPACT`**, so a plain build's deferred node frees drained
> *during* the timed window, stealing cores from the readers; and (4) the
> **result sink was one shared global**: every reader stored its batch result
> into it, and in the FT membarrier build and in HOTRowex the linker had put it
> on the cache line of the trie's root pointer, which every lookup loads — about
> −20% at 192 readers for both (FT 435 → 545, HOTRowex 449 → 576), and a 25% gap
> between the two FT flavors that neither flavor has. The sinks are per-thread
> now, in all five engines. Pinning one-thread-per-core plus a fully offline,
> promptly-reclaimed FT build had earlier erased a spurious ~13% QSBR gap and a
> larger SMT-contention penalty.

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
~1%** of each other from 96 readers up (readers-only @192: memb 560, QSBR 561;
with the writer @191: 529 and 532). That is the expected result — the reader brackets one
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
the fastest insert/remove churn (~5700–6700 Kops/s over this sweep), then
ART-OLC (~1600–1800) and FT (~240–460); HOTRowex's ROWEX has no concurrent
`remove` at all. So FT trades a modest read deficit (~1.1× behind HOTRowex), a
slower writer and a larger RSS for being the only engine
with full concurrent insert **and** remove under RCU at the lowest read-side
cost.

### Mutator throughput vs reader concurrency

The sweep above scales *readers* against one writer; this one **inverts** it —
fix **one mutator thread** doing insert/replace/remove and scale **readers 0 →
191** — to show how reader concurrency throttles a single writer. Same binaries
(`BENCH_MUTATOR=1`); per-op throughput in **kops/s**, median of 3. "Replace" is
each engine's natural value update where it has one (Judy/qp/ART/Masstree update
in place; FT's `cds_ft_replace()` swaps in a fresh leaf under the same key and
RCU-frees the old one), an `upsert` for HOTRowex (no delete), or
remove+reinsert where there is no value-update API (ART-OLC/ROWEX; BIND9
`dns_qp` delete+insert in one write txn).

FT has two rows. `ft` is the library default, which maintains the **ordered
cell list** on every insert and remove — a feature none of the other engines
has, and the one that makes FT's ordered iteration the fastest below. `ft`
(list off) is the same trie with that list disabled (`FT_NO_ORD=1`), the
configuration for a write-heavy trie that never iterates in key order.

```sh
# per engine; bind9 engines need LD_LIBRARY_PATH and live in bind9-src/build/...
BENCH_MUTATOR=1 ./bench_scale_artrowex 200
LD_LIBRARY_PATH=urcu-build/src/.libs BENCH_MUTATOR=1 \
    bind9-src/build/tests/bench/bench_scale_ft 200
LD_LIBRARY_PATH=urcu-build/src/.libs BENCH_MUTATOR=1 FT_NO_ORD=1 \
    bind9-src/build/tests/bench/bench_scale_ft 200     # ordered list off
```

![Single-mutator insert throughput vs reader count, log scale: the three rwlock
engines start fastest (~10M kops/s) then collapse ~3000–5000× the instant one
reader appears; RCU (ft, b9qp) and the optimistic/ROWEX tries never collapse,
staying within one order of magnitude out to 191 readers](figures/mutator_insert.png)

**Insert (kops/s)** — readers across the top:

| Engine | 0 | 1 | 16 | 64 | 191 | sync |
|------------|------:|----:|-----:|----:|----:|:-----|
| `masstree` | 11246 |11191|10787 |10346| 9133| optimistic |
| `hotrowex` |  5700 | 5712| 5543 | 5395| 5142| ROWEX |
| `artolc`   |  3872 | 3544| 2398 | 1915| 1444| OLC |
| `artrowex` |  3263 | 3007| 2183 | 1830| 1395| ROWEX |
| `ft` (list off) | 749 | 711 | 624 | 525 | 313 | **RCU** |
| `ft`       |   493 |  474|  427 |  366|  239| **RCU** |
| `b9qp`     |   446 |  438|  346 |  290|  210| **RCU** |
| `judy`     | 10728 |**2**|   14 |  130|  492| rwlock |
| `qp`       |  9262 |**3**|   19 |  109|  513| rwlock |
| `art`      | 10250 |**2**|   11 |  120|  470| rwlock |

**The cliff is the result.** The three **rwlock** engines have the *fastest*
single-thread mutation (~9–11M ops/s), then **fall off a cliff the instant a
reader appears** — judy `10728 → 2`, art `10250 → 2`, qp `9262 → 3` kops, a
~3000–5000× collapse — because the writer-preferring rwlock writer must wait for readers, and
each reader holds the rdlock across a whole 1000-lookup batch. (The noisy partial
"recovery" at higher reader counts is scheduling churn in the starved regime, not
a real trend; the rwlock numbers there are not reproducible point-to-point.)
**RCU and the lock-free concurrent tries do not collapse** — FT, b9qp, Masstree,
HOTRowex and both ARTs keep mutating within the same order of magnitude all the
way to 191 readers, because their readers never hold a lock the writer needs.
Masstree is barely touched (−19% over the whole sweep) and HOTRowex less still
(−10%); FT halves between 0 and 191 readers (493 → 239) and never starves.

![Replace and remove throughput from 0 to 191 readers, log-scale dumbbells:
the rwlock engines lose 13–24×, RCU and the optimistic engines degrade gently;
Masstree's in-place replace barely moves and HOTRowex has no
remove](figures/mutator_replace_remove.png)

**Replace** and **Remove (kops/s)** at the endpoints (0 / 191 readers):

| Engine | replace 0 | replace 191 | remove 0 | remove 191 |
|------------|----------:|------------:|---------:|-----------:|
| `masstree` |     13071 |       11799 |    12239 |       8530 |
| `hotrowex` |      6258 |        5570 |   *n/a*  |     *n/a*  |
| `ft` (list off) | 2346 |       1202 |      921 |        497 |
| `ft`       |      1410 |         720 |      711 |        364 |
| `artolc`   |      1221 |        1097 |     1191 |       1002 |
| `artrowex` |       781 |         756 |     1142 |        982 |
| `b9qp`     |       464 |         210 |      484 |        221 |
| `judy`     |     15385 |         769 |     9086 |        404 |
| `qp`       |     17509 |        1363 |    16431 |       1100 |
| `art`      |     15342 |         851 |    12399 |        520 |

Per-op shape follows the mechanism: **Masstree's in-place update makes replace its
*cheapest* op** (13.1M ops/s, above its own insert and remove — no node split or
merge). **FT's replace is its cheapest op too** (1410 kops/s, about 3× its
insert): `cds_ft_replace()` swaps one leaf and leaves the node structure alone,
which puts it ahead of both ARTs with no readers. The remove+reinsert engines
(ART-OLC/ROWEX) pay replace ≈ the harmonic mean of their remove+insert.
**HOTRowex has no remove** (ROWEX); its insert and replace are both `upsert`.
**The ordered list costs FT a third of its insert rate and a quarter of its
remove rate**: with it off, insert is 1.5× faster (749 vs 493), replace 1.7× (2346 vs 1410) and
remove 1.3× (921 vs 711). (Caveat: the rwlock collapse magnitude is tied to the
reader lock-hold granularity — readers batch 1000 lookups per rdlock here; finer
locking would starve the writer less, but the qualitative RCU-vs-rwlock gap
stands.)

**Compaction accounting.** Each engine's kops includes whatever maintenance it
does *inline*: b9qp's `dns_qp_compact(…, NOW)` (when `dns_qp_memusage().fragmented`)
and Masstree's epoch advance run inside the timed `writer_op`, so that
compaction/reclamation time is in the denominator (lowering their kops) though
it is not counted as an op. **FT does no inline compaction** — its reclamation is
asynchronous `call_rcu`, on the per-CPU worker of the mutator's own CPU (the
harness creates per-CPU workers), so the freeing shares the mutator's core and
is inside the figure. Read FT vs b9qp with the remaining asymmetry in mind:
b9qp pays for staying compact within the figure, FT does not compact.

**Where the reclaim worker runs matters a great deal.** The same library does
4× more or less depending on it: an FT from June did 1630 kops/s inserts with
the `call_rcu` worker on the mutator's CPU and 404 with the worker on another
CPU (cross-CPU contention on what the two hand each other). An earlier version
of this table reported 1340 for FT; that run pinned nothing, the two threads
happened to share a cache domain, and it reproduces today on four runs out of
five. At matched placement the transaction-engine FT is slower than that June
library for a lone writer — 21k instructions per update against 4.7k — the
price of multi-writer-safe updates and of the ordered list.

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

![Ordered iteration scaling, log scale: every engine is near-linear from 1 to
192 threads; FT's batched cell gather leads at 89,855 next-Mops/s at 192
threads, ~3.1× HOTRowex and ~6.0× BIND9-QP; the key-materializing judy/qp
cursors sit lowest](figures/ordered_iteration.png)

Median of 3, next Mops/s — readers across the top (FT at `ft-txn-integ` @
9484d87e):

| Engine | 1 | 16 | 64 | 192 | traversal |
|------------|------:|------:|-------:|-------:|:----------|
| **`ft`**   | **500** | **7979** | **32081** | **89855** | **batched cell gather, compacted (phys-next MLP; cell-native, no node touch)** |
| `hotrowex` |   176 |  2846 | 10977 | 29007 | inlined header-template + contiguous leaves |
| `b9qp`     |    92 |  1468 |   5609 |  14963 | `dns_qpiter` `.so` call + DFS-compacted chunks |
| `art`      |    22 |   322 |   1272 |   3772 | recursive callback |
| `masstree` |    17 |   270 |   1078 |   3041 | B+tree leaf scan |
| `artolc`   |    14 |   226 |    889 |   2583 | range-into-buffer |
| `artrowex` |    12 |   198 |    787 |   2258 | range-into-buffer |
| `judy`     |   5.3 |    88 |    355 |    981 | JSLN cursor (materializes key) |
| `qp`       |   4.4 |    72 |    298 |    844 | Tnextl cursor (materializes key) |

(`b9qp`'s 192-reader point is a separate `BENCH_THREADS=192` run: libisc caps
thread ids at 512, which the sweep's cumulative reader count passes there.)

**The batched FT row moves with where the linker puts the caller's loop.** The
harness's `ft_iterate()` is the same 208 instructions in two builds of this
benchmark that differ only in an unrelated writer function ahead of it, which
shifted its address by 0x120 bytes. Same library, same pinning, three runs
each within 0.3%: 443 and 500 Mops/s on one thread, 79,298 and 89,855 at 192.
The table is the current build. The two un-batched FT configurations moved by
1% and 3% between the same two builds.

**FT is the fastest ordered iterator** — ~3.1× over hotrowex at 192T, a full
reversal of the original result (FT was *last*, 365 Mops/s). It got there in four
steps on the same 1M-key set; the first row is the June measurement of the
pre-cell cursor, which today's library no longer has a configuration for, and
the other three are current:

![How FT ordered iteration got 257× faster, log-scale bars: cds_ft_next descent
350 → ordered cell list 7,260 (×20.7) → compaction 18,351 (×2.5) → batched
gather 89,855 Mops/s (×4.9)](figures/ft_iter_steps.png)

| FT ordered-scan config | 192T Mops/s | what changed |
|---|--:|:--|
| `cds_ft_next` descent (pre-cell, June) | 350 | re-descend per step (the old result) |
| + ordered cell list (`FT_ORD`) | 7260 | O(1) cell hop; cells still in insert order |
| + compaction (`FT_BENCH_COMPACT`) | 18351 | cells packed in key order → contiguous walk |
| + batched gather (`FT_BATCH=64`) | **89855** | one call per batch + phys-next MLP; cell-native (no node touch) |

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
reached ~87k of the 88k measured in June (not re-measured) — and a count- or key-only scan, which never dereferences the node,
touches *no* external-head cachelines at all.) The node is recovered lazily, only
when the consumer wants the value, via `cds_ft_cell_node()`; the key via
`cds_ft_cell_get_key()`.

**Two findings.** (1) **Ordered iteration is embarrassingly parallel** — every
engine scales near-linearly (~160–190× from 1 to 192 threads): a full traversal
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
un-compacted trie is ~12× slower (the 7260 row). Compaction trades RSS and a
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
FT_NO_SKIP_COMPRESSED=1 ...     # same, FT in its eager mode
```

#### Result — FT vs `dns_qpmulti` in bind9's event loop (192 cores)

Same 1M DNS names; each trie holds ~500k entries over that key space, so **~50% of
lookups miss**. Both engines are NUMA-interleaved and core-pinned. FT is shown in
two *fair* modes — **speculative** (skip-compressed descent + a validating key
compare, the API contract, mirroring qp's `leaf_qpkey`+`qpkey_compare`) and
**eager** (exact byte-by-byte descent). Aggregate read throughput (Mops/s), `loop`
column, readers across:

![qpmulti_ft in bind9's event loop, two panels: on the miss-heavy read-only
sweep qp leads FT speculative ~4% at 192 readers; with concurrent mutators FT
speculative is level to 1.4× ahead](figures/qpmulti_ft.png)

Median of 3 runs with `ISC_TASK_WORKERS=192` (6 for `qp`, which runs in both
the speculative and the eager process), FT at `ft-txn-integ` @ 9484d87e.

**Read-only:**

| readers | `qp` | FT eager | FT speculative |
|--:|--:|--:|--:|
| 1   | 2.6 | 2.4 | 2.4 |
| 16  | 45.0 | 43.2 | 44.0 |
| 64  | 180 | 172 | 175 |
| **192** | **534** | **498** (0.93×) | **515** (0.96×) |

**Mutate + read** (N readers alongside 192−N mutators; the FT mutators
serialize on an application mutex, as `dns_qpmulti`'s do on its writer mutex):

| readers | `qp` | FT eager | FT speculative |
|--:|--:|--:|--:|
| 1   | 1.4 | 1.8 (1.29×) | 2.0 (1.43×) |
| 16  | 36.1 | 37.3 | 38.2 (1.06×) |
| 64  | 149 | 149 | 152 (1.02×) |
| **191** | **455** | **452** (0.99×) | **466** (1.02×) |

**Two findings.** (1) **Speculative is FT's best fair mode — it beats or ties
eager at every point.** Eager compares every key byte against the compressed-node encoding at
each level; speculative skips those compares and pays a *single* validation memcmp
at the leaf, and that wins even at a 50% miss rate. (2) **The result is
workload-dependent.** On the miss-heavy read-only sweep FT trails `qp` ~4% — qp's
sparse-branch descent exits early on a miss, while FT's speculative descent runs to
a candidate leaf before the validating compare rejects it. Under write contention
FT pulls level or ahead (1.02–1.43×, largest when mutators dominate) because its RCU read
path dirties no shared memory while qp's write-shares. This is the same mechanism
as the 100%-hit `load-names` result (FT ~1.4×) seen from the *other* end of the
hit-rate axis: **FT wins hit-heavy and write-contended; qp wins miss-heavy
read-only.**

> The speculative path **must validate the candidate** — `cds_ft_lookup_candidate_key`
> returns the unvalidated descent result, so counting any non-NULL candidate as a hit
> both miscounts misses and skips the compare qp performs, inflating FT ~14% and
> falsely showing it ahead on read-only. `FT_RAW_CANDIDATE=1` runs that unvalidated
> path as a (non-comparable) ceiling: ~527/517 Mops/s read-only/mut+read at the top
> (June figure, not re-measured).

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
make urcu-txn            # clone urcu-txn-dev into urcu-txn-build/ + build liburcu
make bench_list_scale    # needs bind9-src/ isc headers (make bind9) for the iscrw engine
```

The lists are header-only (the `rcu-txn-mcas`/`rcu-txn` engine + the `rcu-txn-list`
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

Three defaults were chosen after the investigations below:
- **Segregated `rcu_head`** (default; `-DLIST_RCU_INLINE_RCU_HEAD` for the
  artifact build): the `rcu_head` lives *outside* the hot node and the stable
  nodes come from one arena, the way a metadata-segregating allocator behaves.
- **One node per cache line** (default since September; `-DLIST_NODE_ALIGN=0`
  restores the packed 24 B nodes): a writer linking a churn node writes its
  anchor's `next`, and at 24 B two or three anchors share a 64 B line, so
  writers on neighbouring anchors were false-sharing while the benchmark
  reported them as isolated. Every table below is on this layout unless it
  says *packed*.
- **Per-CPU `call_rcu` workers** (default; `BENCH_NO_PERCPU_CALLRCU` to disable):
  one reclaim worker per hardware thread, each pinned to its writer's PU
  (`BENCH_RECLAIM_DOMAIN=hwthread|core|l3|single`), instead of liburcu's single
  global worker.

### Results

Hardware: 2× AMD EPYC 9654 (192 physical cores / 384 threads, 24 NUMA nodes, 8
cores/node). Re-measured 2026-10-04 on an idle machine, against `urcu-txn-dev`
@ 2793224e built `-O2 -DNDEBUG` (`make urcu-txn`). Best of 2 runs of 3 s per
point unless a table says otherwise; the two runs of a read point agree within
1% for the RCU engines. `LIST_SIZE=1000`, `CHURN=200` unless stated. The tables
of the first four subsections come from `scripts/run_list_scale_base.sh`; the
later subsections name their own generator.

#### Read-only ceiling — Mvisits/s, readers 1 → 383

| readers   |   1 |    32 |    96 |   191 |   383 |
|-----------|----:|------:|------:|------:|------:|
| `rculist` | 818 | 26219 | 75442 | **153196** | 258019 |
| `txn_list`| 809 | 25921 | 72992 | 149059 | 222549 |
| `txn_sw_list`| 781 | 25002 | 73785 | 146659 | 246759 |
| `seqlock` | 611 | 19615 | 60014 | 116403 | 161249 |
| `iscrw`   | 618 | 18524 | 38921 | 40349 | 57205 |
| `rwlock_w`| 612 | 19035 | 29743 | 35082 | 27295 |
| `rwlock_r`| 616 | 19189 | 30186 | 34772 | 28341 |
| `mutex`   | 610 |   444 |   419 |   251 |   250 |
| `fairmutex`| 615 |  109 |    31 |    29 |    33 |

- **The two bidir lists scale linearly to 192 cores and beat seqlock by a
  quarter** (147–149 k vs 116 k @191). They sit 3–4 % under the forward-only
  `rculist` (153 k), a gap that repeats in both runs — so a coherent
  bidirectional reverse walk costs the read side a few percent, not a factor.
- `pthread_rwlock` plateaus ~30–35 k (its shared reader-count cacheline is the
  bottleneck); `iscrw` C-RW-WP does better but is still counter-bound.
- `mutex` (~250–440) and `fairmutex` (~30–110) **collapse** — "concurrent"
  readers take an *exclusive* lock, so they serialize; the MCS lock's futex
  park/wake is worst.

#### Why the node layout matters

Two layout choices, measured separately at 96 readers with no writer
(Mvisits/s):

| LIST_SIZE | node layout | seqlock | `txn_sw_list` |
|-----------|-------------|--------:|--------------:|
| 1,000  | packed, 24 B                      | 56056 | **76804** |
| 1,000  | packed, `rcu_head` inline, 40 B   |     – | 50468 |
| 1,000  | one node per line, 64 B (default) | 56551 | 73812 |
| 30,000 | packed, 24 B                      | 72306 | **77154** |
| 30,000 | packed, `rcu_head` inline, 40 B   |     – | 66437 |
| 30,000 | one node per line, 64 B (default) | 48927 | 66272 |

**Embedding the `rcu_head` costs the reader 14–34 %** (76804 → 50468 at 1,000
nodes, 77154 → 66437 at 30,000): the cold reclamation metadata rides in the
traversal's working set. That is why the segregated layout is the default. (A
production strided allocator that pairs hot data with cold metadata on separate
cachelines gets this for free.) An earlier version of this table, taken on a
shared machine, reported 2–4×.

**One node per line costs the reader as well, on a long list**: 4 % for
`txn_sw_list` at 1,000 nodes, but 14 % (`txn_sw_list`) and 32 % (seqlock) at
30,000, where the list spans 1.9 MB instead of 720 KB. It is the default for the
writer's sake (next subsection), and a reader figure must not be compared
across the two layouts.

(seqlock at 1,000 nodes has two modes 6 % apart from one process to the next,
56.8 k and 60.1 k in five back-to-back runs; the ceiling table above caught the
high one, this table the low one. `txn_sw_list` repeats within 0.1 %.)

#### Read throughput with a concurrent writer

Readers 1 → 191 plus one writer. Read Mvisits/s at 191 readers, and what that
one writer achieved (Mops/s):

| engine | read @191 | writer @191 readers | writer @1 reader |
|--------------|------:|-----:|------:|
| `txn_sw_list`| 92808 | 0.48 |  6.81 |
| `txn_list`   | 85356 | 0.19 |  4.53 |
| `rculist`    | 78422 | 0.68 |  7.36 |
| `rwlock_r`   | 35177 | 0.00 |  0.09 |
| `iscrw`      | 19301 | 0.03 |  7.82 |
| `rwlock_w`   |  2036 | 0.03 |  0.09 |
| `mutex`      |   229 | 0.00 |  0.01 |
| `fairmutex`  |    31 | 0.00 |  0.21 |
| `seqlock`    |   102 | 1.24 | 24.65 |

The three RCU lists scale to 78–93 k Mvisits/s; `rwlock_r` also scales (35 k)
but only by **starving the writer**; `rwlock_w` collapses to ~2 k (writers block
readers in bursts); `iscrw` sits in between (19 k) with a writer that barely
runs; `mutex`/`fairmutex` ~230/30; **`seqlock` → ~0** (a 2000-node read section
almost always overlaps the steady writer and retries forever) while its writer
is the fastest of all. **Caveat:** this mode conflates read scaling with each
engine's *writer rate* — every toggle invalidates lines the readers are walking,
so a faster writer caps its readers lower (`rculist` here), and every RCU writer
slows 11–24× between 1 and 191 readers for the same reason seen from the other
side. Use `BENCH_WRITE_RATE` to pin all engines to one mutation rate for a clean
comparison.

**Node alignment decides this table.** The same point — 191 readers and one
writer — on the default layout against packed 24 B nodes:

| engine | read, one node per line | read, packed | writer, one node per line | writer, packed |
|--------------|------:|------:|-----:|-----:|
| `txn_sw_list`| 92740 | 55620 | 0.45 | 1.15 |
| `txn_list`   | 86112 | 76066 | 0.19 | 0.30 |
| `rculist`    | 82042 | 41977 | 0.63 | 1.46 |

On packed nodes a toggle dirties a line that carries two or three nodes, and
the writer itself runs 1.6–2.6× faster, so the readers lose 12–49 %. The ~48 k
this section reported before the alignment change was the packed layout.

#### Writer scaling & allocation

`txn_list` is the **only** engine whose write throughput rises with concurrent
writers; the others take one writer lock. Its limiter is **reclamation and
allocation, not the MCAS**: with liburcu's single global `call_rcu` worker every
deferred free funnels through one thread (measured in July at ~8 Mops/s, about
5× below one worker per hardware thread; not repeated here). The default is
therefore one worker per hardware thread, each pinned to its writer's PU so the
producer→consumer free stays CPU- and NUMA-local
(`BENCH_RECLAIM_DOMAIN=hwthread`; `BENCH_NO_PERCPU_CALLRCU` to disable).

Three allocators for the per-attempt MCAS descriptor and the churn nodes: glibc
with the engine's descriptor slab off (`URCU_TXN_NO_CACHE=1`), jemalloc
`percpu_arena:percpu` with the slab off, and glibc with the slab — the shipping
default, described in the next subsection. `txn_list` write Mops/s (this box:
192 cores / 384 PUs), tiny default list (`LIST_SIZE=1000`, `CHURN=200`).

**Plain churn** (default `BENCH_WRITESCALE`) — each writer toggles churn nodes in
and out after spread anchors: a list insert/delete (2–3-edge MCAS).

| writers | glibc | jemalloc `percpu_arena:percpu` | glibc + descriptor slab |
|---------|------:|------:|------:|
| 1   | 5.9 | 6.8 | **9.3** |
| 8   | 17  | 41  | **51** |
| 32  | 32  | 50  | 51  |
| 64  | 37  | 40  | 40  |
| 128 | 48  | 53  | 52  |
| 192 | 92  | 83  | 82  |

jemalloc and the slab land on the same figure from 32 writers up — to the first
decimal at 64, 128 and 192, in both runs — so above that the allocator is no
longer what limits this 200-node churn set. Plain glibc trails up to 128 writers
and is bistable at 192 (63 and 92 in its two runs).

**Random transacted index** (`BENCH_RANDOM_POS`) — each writer atomically toggles
a randomly chosen slot of an external index that points at list cells; the index
update folds into the same MCAS (the composable path), so there is more compute
per op. The index has exactly `CHURN` slots and each writer picks one uniformly,
so the per-slot collision rate is `~ writers / CHURN`.

| writers | glibc | jemalloc `percpu_arena:percpu` | glibc + descriptor slab |
|---------|------:|------:|------:|
| 1   | 5.9 | 5.3 | **8.1** |
| 8   | 11  | 20  | **23** |
| 32  | 19  | 20  | 23  |
| 64  | 27  | 30  | **32** |
| 128 | 25  | 27  | 26  |
| 192 | 19  | 22  | 20  |

At `CHURN=200`, 192 writers collide ~1:1 on the index, so the random path is
**index-contention-bound**: it peaks near 64 writers and declines to ~20 Mops/s
at 192. That decline used to be a cliff. A contending transaction retries
optimistically up to a budget and then escalates into the domain's single
serial fair lane; under a flat budget of 64 retries the writers escalated *en
masse* (~0.1 Mops/s at 128–192 writers), and under a flat 256 they held 3–5
Mops/s at 192. The engine measured here no longer uses a flat budget: it scales
with the transaction's own cost — 11/4 retries per load or write-set record,
with a floor of 64 (`<urcu/rcu-txn.h>`) — and holds ~20. Escalation still
exists, so starvation-freedom is unchanged. (The 0.1 and 3–5 figures are the
July measurements of those two budgets on the engine of the time.)

Enlarging the index — raise `CHURN`, capped at `LIST_SIZE`, so raise both —
spreads the collisions and restores writer scaling. One process per point,
write Mops/s:

| index slots (`CHURN`) | @64 | @128 | @192 | | @64 | @128 | @192 |
|-----------------------|----:|-----:|-----:|-|----:|-----:|-----:|
|                       | **glibc + slab** | | | | **jemalloc** | | |
| 200 (default)         | 32  | 25   | 20   | | 29  | 27   | 22   |
| 10 000                | 53  | 81   | 111  | | 50  | 86   | 114  |
| 100 000               | 57  | 89   | 117  | | 66  | 130  | **168** |
| 1 000 000             | 55  | 89   | 116  | | 71  | 114  | **171** |

With a ≥10 k-slot index the composable random path scales to ~115 Mops/s at 192
writers on the shipping configuration, confirming that the fall-off at
`CHURN=200` is index contention (`writers / CHURN`), not the transaction engine.
On jemalloc it goes further, to ~170 at 100 k slots and up — 45 % above the slab
in these one-point-per-process runs. The full writer sweep of the next
subsection, one process for all writer counts, puts the two within 8 % at the
same 100 k index (135 and 125); the two ways of running disagree on jemalloc
(168 and 135) and that difference is not explained.

**The deferred-free backlog is large.** A writer shares its hardware thread with
its reclaim worker, and reclaim does not keep up with it: the cgroup holding the
index-size runs peaked at 177 GB and the one holding the next subsection's
sweeps at 165 GB. A first attempt under a 64 GB cap was OOM-killed at 128
writers on the 10 k index. Capping it needs reclaim backpressure, which this
benchmark does not have.

The engine measured here commits **single-driver**: the owner installs, decides
and settles its own transaction, a peer that meets a parked slot waits its
spinlatch out or escalates, and nobody helps, so the commit point is one
release-store of the status word (see `<urcu/rcu-txn-mcas.h>`). The July
tables were taken on the helping engine that preceded it. Committed
transactions are linearizable at that single store, atomic across every
structure folded into the commit.

#### MCAS descriptor slab — closing the allocator gap, no external allocator

The remaining allocator cost is the **per-attempt MCAS descriptor**. Every
`txn_list` update `posix_memalign`s a descriptor (a header plus its inline record
array) and hands it to `call_rcu`; the reclaim worker frees it a
grace period later. That free is the **producer→consumer cross-thread** case again
(writer allocates, worker frees), so a per-thread malloc cache cannot recycle it —
and servicing every attempt from glibc's arena at 192 threads grows the arena, which
`mprotect`s, which serializes on the process-wide `mmap_lock`: an `osq_lock` storm
that ate ~54 % of runtime in a `perf` profile taken in July. Linking jemalloc (per-CPU arenas) was
what had been hiding this.

A **per-CPU size-classed superblock slab** (`<urcu/rcu-txn-slab.h>`) removes it with
no external allocator. One arena per `(size class, CPU)` is a lock-free-stack
freelist plus a bump pointer into `mmap`'d 2 MiB superblocks. `free()` recovers a
block's **origin** arena from its superblock header (range-aligned, so
`ptr & ~(RANGE−1)`), so a descriptor allocated on CPU X and freed by the reclaim
worker — on whatever CPU it runs — returns to X's arena: the cross-thread free
stays CPU-local with no central structure. The freelist is an `lfstack` and not a
`wfstack` because the *writer* is the one popping: an `lfstack` chain is always
fully linked, so the pop never waits on a preempted reclaim worker. Alloc reuses a
freed block before carving a new one, so the mapped set tracks peak live
descriptors and then recycles. `URCU_TXN_NO_CACHE=1` disables it (back to
`posix_memalign`).

Same three allocators as above, each writer-scaled 1 → 192 in one process, on
a larger churn set and on the composable path (`scripts/run_list_scale_alloc.sh`,
best of 2):

![txn_list writer scaling by allocator: on plain churn and on random-access
writes alike, glibc with the descriptor slab runs within 8 % of jemalloc's
per-CPU arenas at 192 writers and far above plain glibc, with no external
allocator](figures/list_scale_alloc.png)

**Plain churn** (`LIST_SIZE=4096`, `CHURN=3072`; 2–3-edge MCAS), write Mops/s:

| writers | glibc | jemalloc `percpu_arena:percpu` | glibc + descriptor slab |
|--------:|------:|------------------------------:|------------------------:|
| 1   | 6.6 | 6.8     | **9.3** |
| 8   | 18  | 34      | **43**  |
| 32  | 38  | 84      | **90**  |
| 64  | 50  | 123     | **137** |
| 128 | 88  | 146     | 145     |
| 192 | 129 | **223** | 206     |

**Composable random 100 k-slot index** (index + list folded into one MCAS — heavier,
more descriptor pressure):

| writers | glibc | jemalloc `percpu_arena:percpu` | glibc + descriptor slab |
|--------:|------:|------------------------------:|------------------------:|
| 1   | 3.6 | 3.5     | **4.8** |
| 8   | 10  | 19      | **22**  |
| 32  | 22  | **35**  | 33      |
| 64  | 36  | **61**  | 58      |
| 128 | 63  | **98**  | 94      |
| 192 | 88  | **135** | 125     |

**The slab recovers what plain glibc loses, with no external allocator
linked.** At 192 writers it is 1.6× glibc on churn (206 vs 129) and 1.4× on the
composable path (125 vs 88), within 8 % of jemalloc's per-CPU arenas on both
(223 and 135), and ahead of both mallocs from 1 to 64 writers on churn. Plain
glibc is last at every writer count on both workloads. The slab and jemalloc
columns repeat within 1–2 % between the two runs at 192 writers on churn; plain
glibc does not (99 and 129).

An earlier version of this section had glibc *leading* plain churn (103 against
jemalloc's 81) and concluded that the two mallocs split the workloads; that
table predated the data behind the figure and does not reproduce.

The slab is upstreamed into the transaction engine's liburcu tree as a generic
component shared by the concurrent `rcu-txn-mcas.h` and the single-writer `rcu-txn-sw.h`
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
section, the floor). Best-of-2, `DURATION_SEC=3`, jemalloc `percpu_arena:percpu` for
both schemes, 0 coherence violations throughout
(`scripts/run_rlu_vs_txn_sweep.sh`).

![RLU vs txn (MCAS) across three workloads: disjoint writes (txn ~15× RLU-defer at
192), random-access writes on a hot 64-slot index (RLU leads at one writer, txn
leads 6–8× across the contended range), and hash-of-lists (txn ~1.7×
RLU-defer)](figures/rlu_vs_txn.png)

**Disjoint churn** — each writer owns a strided set of slots so writers almost
never collide (`LIST_SIZE=4096`, `CHURN=3072`, jemalloc, write Mops/s):

| writers      |   1 |  8 | 32 | 64 | 128 |     192 |
|--------------|----:|---:|---:|---:|----:|--------:|
| `txn_list`   |  10 | 45 | 97 | 131 | 167 | **230** |
| RLU-defer    |  15 | 32 | 26 |  26 |  19 |      15 |
| RLU-sync     |  18 | 10 | 7.8| 9.1| 8.3 |     8.2 |

RLU wins at 1 writer (its per-thread write-log + batched writeback is cheap
uncontended), the two tie at 4, and `txn_list` leads from 8 writers to reach
**~15× RLU-defer** at the full box: disjoint slots never escalate, so every MCAS
commit runs in parallel, while RLU's global write-clock serializes commit ordering.

**Multi-slot random** (`BENCH_RANDOM_POS`, `LIST_SIZE=1000`, `CHURN=64` — a hot
64-slot index, ~`writers/64` collision):

| writers      |    1 |    8 |  16 |  32 |  64 |  192 |
|--------------|-----:|-----:|----:|----:|----:|-----:|
| `txn_list`   |  7.7 |   23 |  17 |  18 |  19 |  9.4 |
| RLU-defer    |   12 |   12 | 5.5 | 4.3 | 3.2 |  1.2 |
| RLU-sync     |   13 |  9.5 | 5.2 | 4.5 | 3.4 |  1.3 |

Under real contention RLU owns only the lowest writer count (12–13 vs 7.7 @1; a
tie at 2); `txn_list` leads from 4 writers and owns the contended range — 19
Mops/s @64 and 9.4 @192 against RLU's ~3.2 and ~1.2, a **6–8× lead**. (Earlier
engines collapsed here, to ~1.6 Mops/s @192 under a flat retry budget of 64; see
the retry-budget note under *Writer scaling & allocation*.)

**Hash-of-lists** — RLU's native showcase, now with `txn_hlist` on the same
**singly-linked** structure as RLU (the 8-byte hlist head — see § *Dataset size*); one
shared escalation domain for the whole table (write Mops/s):

| writers      |   1 |   8 | 32 | 64 | 128 |    192 |
|--------------|----:|----:|---:|---:|----:|-------:|
| `txn_hlist`  | 1.1 | 8.4 | 12 | 16 |  23 | **29** |
| RLU-defer    | 1.3 |  10 | 14 | 15 |  16 |     17 |
| RLU-sync     | 1.3 | 4.8 | 4.9| 6.1| 6.4 |    6.4 |

On RLU's home turf RLU-defer leads up to 32 writers; `txn_hlist` passes it at 64
and scales to **~1.7× RLU-defer** at 192 (at 1000 buckets writers rarely land on
the same chain, so commits stay on the parallel optimistic MCAS path and the
single shared domain's fair lane is seldom entered); RLU-defer plateaus ~17 and
RLU-sync ~6.4.

**Reads: parity on the hash, txn ahead on a real list.** On the hash, read
throughput under one writer is within 4 % across all three (Mvisits/s @191
readers: `txn_hlist` 147, RLU-defer 141, RLU-sync 142) — a short chain is
dominated by per-op overhead, so RLU's per-node lock check barely shows. That
check is a *per-visit* cost that does not amortize: on a representative
10 000-node list `txn_list` reads pull **1.6–2.0× ahead** (next subsection) — txn
amortizes per-op overhead over the longer walk while RLU pays validation on every
node.

(One benchmark bug surfaced here: the multi-slot-random RLU driver read a neighbour
pointer before `RLU_TRY_LOCK`-ing it, so a concurrent unlink+free at the same
position could drop a freed node into the write-set — an intermittent use-after-free
in RLU's writeback, core-confirmed. Locking the anchor before reading its edge
closes it; the disjoint-slot churn driver keeps the simpler read-before-lock form,
safe there because no peer ever touches the same node.)

#### Read, write & 50/50 scaling — representative working set (10k nodes, 2% updates)

The workload tables above each isolate one case on a small list. This is the same
bidir list at a **10 000-node** structure with a **2 % update set** (200 churn
nodes) — large enough that a read traverses a real, cache-pressured span. Here
`txn_list` runs on its **shipping config, glibc + the descriptor slab** (no
jemalloc); RLU runs on glibc (`scripts/run_rlu_vs_txn_rw.sh`, best of 2).

![RLU vs txn read/write/50-50 scaling at 10k nodes, 2% updates: txn leads reads
~1.6-2.0x, scales writes to ~83 Mops/s while RLU collapses at scale, and dominates
the write half of a 50/50 mix](figures/rlu_vs_txn_rw.png)

**Read scaling** (readers + 1 writer, Gvisits/s):

| readers      |   1 |   8 | 32 | 64 | 128 |     191 |
|--------------|----:|----:|---:|---:|----:|--------:|
| `txn_list`   | 0.7 | 5.1 | 21 | 44 |  84 | **123** |
| RLU-defer    | 0.5 | 2.9 | 12 | 25 |  44 |      63 |
| RLU-sync     | 0.5 | 3.8 | 16 | 30 |  53 |      77 |

Reads scale linearly for all three, but **`txn_list` leads ~1.6–2.0×** (123 vs
RLU-defer 63 / RLU-sync 77 @191): RLU validates every dereferenced node against its
per-object lock/clock, a per-visit cost that grows with the traversal.

**Write scaling** (writers only, Mops/s):

| writers      |   1 |  8 | 32 | 64 | 128 |     192 |
|--------------|----:|---:|---:|---:|----:|--------:|
| `txn_list`   | 9.3 | 51 | 51 | 40 |  52 |  **83** |
| RLU-defer    |  20 | 51 | 16 | 14 | 8.2 |     6.0 |
| RLU-sync     |  17 | 11 | 7.9| 9.3| 8.6 |     8.4 |

RLU-defer wins at 1 writer and ties at 8 (cheap batched writeback, uncontended);
then its global write-clock serializes commit order and it falls to ~6, while
**`txn_list` reaches 83 — ~14× at the full box.** (`txn_list`'s curve is the
200-node churn set's, the same one as the plain-churn table under *Writer scaling
& allocation*, dip at 64 writers included.)

**50/50 balanced** (T/2 readers + T/2 writers): reads stay close (RLU's read path
edges ahead from 32 threads up), but on the **write** half txn dominates —
deferred writeback stalls under a steady reader stream, and synchronous writeback
(WS=1) nearly stops:

| 50/50 @ total threads       |   2 |   8 |  32 |  64 | 128 |     192 |
|-----------------------------|----:|----:|----:|----:|----:|--------:|
| `txn_list` write (Mops/s)   | 5.4 |  19 |  37 |  41 |  31 |  **40** |
| RLU-defer write (Mops/s)    | 2.5 | 4.1 | 1.9 | 1.8 | 1.6 |     1.4 |
| RLU-sync write (Mops/s)     | 0.0 | 0.1 | 0.3 | 0.5 | 0.7 |     0.8 |
| `txn_list` read (Gvisits/s) | 0.7 | 2.6 | 2.8 | 5.4 | 9.6 |      13 |
| RLU-defer read (Gvisits/s)  | 0.5 | 1.7 | 3.0 | 5.7 |  11 |      14 |
| RLU-sync read (Gvisits/s)   | 0.5 | 2.0 | 6.2 |  10 |  15 |      17 |

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
- **`txn_hlist`** — the MCAS transaction engine (glibc + descriptor slab), on the **8-byte
  single-pointer hlist head** (see § *Dataset size*). At this 1 000 × 100 config its numbers
  are within ~1 of the old 16-byte bidir-sentinel bucket: 100-node chains dwarf the head, so
  the head-density win only surfaces with short chains past cache.
- **`lfht`** — liburcu's `cds_lfht`, **pinned to 1024 fixed buckets, no auto-resize**, so
  it walks the same ~100-node chains as the others. This deliberately denies cds_lfht its
  whole design point (it is built to auto-resize so chains stay ~O(1)); it is here as an
  equal-chain *mechanism* comparison, not cds_lfht as you would deploy it.

RLU is glibc; all four sorted-list engines share the same key stream (rcu_hlist / rlu_hlist
on head + tail sentinels, txn_hlist on a bare 8-byte NULL-terminated head, lfht on
split-order bucket nodes). All engines measured together; best-of-2, `DURATION_SEC=3`,
0 coherence violations across all 240 points (`scripts/run_hash_sweep.sh`, which also
produces the next two subsections).

![LWN #667720 hash benchmark reproduced and extended: read-only all five tie; as the
update rate rises RCU / cds_lfht / txn scale to 192 while RLU-defer walls (flat past ~96
threads) and RLU-sync stays flat](figures/lwn667720_hash.png)

**Total ops/s at 192 threads (full box):**

| engine                    |  0% |  2% | 20% | 40% |
|---------------------------|----:|----:|----:|----:|
| `rcu_hlist` (RCU + lock)  | 280 | 139 |  78 |  54 |
| `lfht` (cds_lfht)         | 301 | 137 |  76 |  54 |
| `txn_hlist` (MCAS)        | 301 | 131 |  64 |  45 |
| RLU-defer                 | 303 | 105 |  37 |  26 |
| RLU-sync                  | 303 |  49 |  16 |  11 |

**Total ops/s at 64 threads (the article's 4-socket box):**

| engine                    |  0% |  2% | 20% | 40% |
|---------------------------|----:|----:|----:|----:|
| `rcu_hlist` (RCU + lock)  |  95 |  61 |  45 |  36 |
| `lfht` (cds_lfht)         | 102 |  61 |  42 |  32 |
| `txn_hlist` (MCAS)        | 102 |  59 |  38 |  28 |
| RLU-defer                 | 103 |  58 |  32 |  23 |
| RLU-sync                  | 103 |  43 |  14 |  11 |

**Read-only (0 %)** the five tie — equal chains, pure RCU-class read scaling to ~300 Mops.
A read-only run lands in one of two modes about 7 % apart whatever the engine (~280 and
~301 at 192 threads: `lfht` gave 301 and 279 in its two runs, RLU-defer 303 and 269), and
`rcu_hlist`'s two runs both landed in the lower one; its 280 is that, not a slower read
path. As updates appear the mechanisms separate and the article's headline holds:
**RLU-sync breaks first** (flat ~50 from 96 threads at 2 %), and **RLU-defer walls** on the
machine's upper half — it climbs to ~96 threads and then stays flat (20 %: 22 → 38 → 37
across 32 → 96 → 192; 40 %: 17 → 26 → 26), scaling further than on the paper's 4-socket
box but adding nothing past ~96 cores. Meanwhile **RCU, `cds_lfht` and txn all scale to
the full 192 cores.**

The new result is that **`txn_hlist` tracks the RCU baseline far more closely than RLU
does** — within 6 % at 2 % (131 vs 139), 17–18 % at 20 %/40 % — and scales where RLU
cannot, because at these update rates the RCU-class read path dominates and txn pays MCAS
only on the X % of ops that write. `cds_lfht` ties `rcu_hlist` at every update rate
(54 vs 54 at 40 %). So on RLU's own benchmark the ranking is **RCU ≈ lfht ≥ txn ≫
RLU-defer ≫ RLU-sync** — txn joins RCU and `cds_lfht` on the scaling side of the "RLU
doesn't scale writers" line. (Bucket count is held constant here, so this isolates the
mechanism; a follow-up shrinks the bucket count to stress *write* contention directly.)

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
| `lfht`       | 1.0 | 7.2 | 14 | 21 |  28 |  **34** |
| `txn_hlist`  | 1.0 | 7.7 | 12 | 18 |  23 |      29 |
| `rcu_hlist`  | 1.2 | 8.5 | 15 | 20 |  25 |      28 |
| RLU-defer    | 1.3 | 8.8 | 11 | 13 |  15 |      16 |
| RLU-sync     | 1.2 | 4.5 | 4.3| 5.5| 6.3 |     6.3 |

`lfht`'s lock-free updates top the write axis; `txn_hlist`'s MCAS and `rcu_hlist`'s
per-bucket lock scale together just behind (rcu ahead up to 128 writers, a tie at 160, txn
ahead at 192); RLU-defer plateaus ~16 (global write-clock) and RLU-sync ~6. Everything but
RLU scales.

**Read scaling** (readers + 1 writer): all five within 5 % — **146–153 Mvis/s @191**,
linear RCU-class reads (RLU-sync marginally top). On a short ~100-node chain the
per-node-cost differences that separate the engines on long list traversals wash out.

**50/50 balanced** — the read/write tradeoff is starkest here. On the **write** half
`lfht`/`rcu`/`txn` (23 / 22 / 19 Mops @192) dominate RLU-defer (10) and RLU-sync (2.6). But
on the **read** half **RLU-sync leads (35 Mvis/s @192)** — precisely *because* its writers
are nearly stalled: no pending write-sets means `RLU_DEREF` takes its fast path and readers
run uninterfered, whereas txn's fast writers dirty reader cachelines and give it the lowest
50/50 reads (14). RLU-sync buys read throughput by forfeiting writes; txn/rcu/lfht keep both
moderate and balanced.

#### Write contention: shrinking the bucket count

The article config keeps ~1 000 buckets, so writers spread across many independent lanes
and rarely collide. To isolate **write contention** we hold the chain length ~constant
(`HL_INIT = 100 × HL_BUCKETS`, ~100 nodes/bucket) and shrink the bucket count — the number
of independent write lanes — at 40 % updates. Fewer lanes → more writers per bucket.
(`BENCH_FIXED_THREADS=N` runs a single thread count for the per-bucket sweep.)

![Write contention: as buckets shrink, RCU+lock collapses (its per-bucket lock becomes a
global lock) while txn and cds_lfht degrade gracefully; once buckets outnumber the 192
writers all three converge at the lock-free ceiling, then dip together in the shaded
footprint-bound tail (working set > LLC); at 16 buckets only txn and lfht keep scaling to
192](figures/hash_contention.png)

**Throughput vs #buckets at 192 threads** (total Mops/s, 40 % updates):

| buckets      |   1 |   4 |  16 |  64 | 256 | 1024 | 4096 | 8192 |
|--------------|----:|----:|----:|----:|----:|-----:|-----:|-----:|
| `lfht`       |  13 |  29 |  46 |  52 |  53 |   53 |   52 |   44 |
| `txn_hlist`  |  12 |  23 |  36 |  41 |  43 |   45 |   45 |   39 |
| `rcu_hlist`  | 0.8 | 1.1 | 5.6 |  16 |  40 |   55 |   59 |   45 |
| RLU-defer    | 1.9 | 4.8 | 8.5 |  13 |  19 |   25 |   32 |   36 |
| RLU-sync     | 1.9 | 5.5 | 8.3 | 9.6 |  10 |   11 |   11 |   10 |

At the article's **1024 buckets** (low contention) `rcu_hlist` ties `lfht` at the top
(55 and 53) — the article's result — and at **4096** it is ahead of both (59 vs `lfht` 52,
`txn_hlist` 45): once the lanes outnumber the 192 writers the per-bucket lock is essentially
uncontended and costs no more than a lock-free CAS. (Its 3-word node also has a smaller
cache footprint than `lfht`'s split-order node. Past 4096 the fixed ~100 nodes/bucket pushes
the working set out of the LLC, so every cache-bound engine sags — the **shaded 4096→8192
band**, footprint- not contention-bound: at 8192 the three drop 14–24 %, to 45 / 44 / 39.
Only RLU-defer, still contention-limited rather than cache-bound, keeps climbing there.)
But as the bucket count *shrinks*, **`rcu_hlist` collapses**: its per-bucket lock becomes a
*global* lock, and throughput falls from 55 to **0.8** at a single bucket — best to worst,
a ~70× drop. `txn_hlist` and `lfht` degrade **gracefully** (45 → 12 and 53 → 13), staying on
top under contention and ending within 8 % of each other on one bucket; txn overtakes rcu
somewhere between 1024 and 256 buckets (at 256 it leads 43 to 40). RLU sits low-to-mid throughout —
even at 1024 buckets its global write-clock caps it (~25 defer / ~11 sync) under 40 % writes.

**Thread scaling at 16 buckets** (heavily contended): only the lock-free `lfht` (→47 @192)
and MCAS `txn_hlist` (→36) keep scaling; `rcu_hlist`, RLU-defer and RLU-sync all **peak
at 8 threads and decline** (rcu 13 → 6.3 @192, RLU-defer 15 → 8.7) as the lock / write-clock
serializes the concentrated writers. (RLU-sync completed every point this time; in July it
segfaulted past ~128 threads here.)

So the article's "RCU meets-or-beats RLU" holds **only at low write contention**.
Concentrate the writes and RCU's per-bucket lock serializes while `txn_hlist` (and `lfht`)
stay robust. `txn_hlist` on a single hot bucket nearly doubled since July (6.7 → 12
Mops/s), with the cost-scaled retry budget described under *Writer scaling & allocation*
in place of the flat one. `txn_hlist` is the one engine that both **tracks RCU at low
contention** *and* **stays contention-robust**.

#### Dataset size: the single-pointer head past the cache hierarchy

The sections above hold the table at 1 000 × 100 and vary the update rate, the thread mode,
or the bucket count. This one holds the **load factor** (`HL_INIT = HL_BUCKETS`, ~1 key per
bucket, chains ≈ 1) and grows the **dataset**, so the bucket array — and with it the
per-lookup working set — climbs from L2-resident to well past L3. It also swaps the
`txn_hlist` bucket from the bidirectional sentinel list (a 16 B `next`+`prev` head node) to
the kernel-shaped **single-pointer hlist** (`<urcu/rcu-txn-hlist.h>`): an **8 B**,
NULL-terminated head with **no sentinel node**, `pprev`-encoded so the head slot is an
ordinary "next" slot (see `design/rcu-txn-hlist.md`). That makes txn_hlist touch the
**fewest cache lines per lookup** of the four — one dense head line (8 heads/line) plus the
chain node — where `lfht` pays an extra split-order bucket node, `rlu_hlist` a head+tail
sentinel pair, and `rcu_hlist` a per-bucket lock.

64 threads, 10 % updates, mean of 5 runs (the figure's band is min–max), 2× AMD EPYC 9654
(**L2 1 MB/core, L3 32 MB/CCD**), idle machine (`scripts/run_hlist_crossover.sh`).

![Hash throughput vs dataset size: txn_hlist runs nearly flat while rcu_hlist starts highest
and crosses below it at ~1M buckets (past L3); lfht collapses fastest and rlu_hlist sits low;
the 8-byte head touches the fewest cache lines so it wins once the working set exceeds
cache](figures/hlist_crossover.png)

**Total ops/s vs dataset size** (Mops/s, leader in **bold**; head-array = 8 B × buckets):

| buckets (heads) | `txn_hlist` | `rcu_hlist` | `lfht` | `rlu_hlist` |
|-----------------|------------:|------------:|-------:|------------:|
| 64K (512 KB)    |         449 |     **559** |    349 |         135 |
| 512K (4 MB)     |         451 |     **477** |    353 |         207 |
| 1M (8 MB)       |         414 |         417 |    305 |         205 |
| 2M (16 MB)      |     **396** |         374 |    248 |         203 |
| 4M (32 MB)      |     **369** |         342 |    220 |         195 |
| 8M (64 MB)      |     **350** |         340 |    202 |         189 |

While the table is **cache-resident** (≤ 512K buckets) `rcu_hlist`'s cheap uncontended
per-bucket lock wins and `txn_hlist` runs 2nd. The **crossover lands at ~1M buckets** — where
the working set (heads + nodes) spills past L3 and the two tie (414 vs 417, ranges
overlapping) — and from 2M on txn_hlist leads with min–max ranges that do not overlap
(2M: 382–417 vs 370–376; 4M: 363–374 vs 338–345; 8M: 346–353 vs 339–343). The margin is
widest just past L3, 6–8 % at 2M–4M, and narrows to 3 % at 8M as both become memory-bound
on the *node* set. Against `lfht` the gap widens monotonically to **~1.7×** (350 vs 202 at
8M): the split-order dummy bucket node is a guaranteed extra miss per lookup that the bare
8 B head simply does not have. txn_hlist also **degrades most gracefully** overall
(449 → 350, a 1.3× drop across a 128× dataset increase, vs lfht's 1.7× and rcu's 1.6×).

The same 8 B sentinel-free head is a **footprint** win on the other axis. Table RSS at
**1 000 000 buckets / 10 000 keys** (isolating the head array; single-threaded so the per-CPU
MCAS slab stays minimal):

| engine       | RSS      | vs txn_hlist |
|--------------|---------:|-------------:|
| `txn_hlist`  | **10.9 MB** |         1× |
| `lfht`       |    27 MB |         2.5× |
| `rlu_hlist`  |    80 MB |         7.3× |
| `rcu_hlist`  |   151 MB |          14× |

txn_hlist is **2.5–14× leaner**: no per-bucket sentinel (`rlu_hlist` carries head+tail
*nodes* per bucket), no per-bucket lock (`rcu_hlist`), no split-order bucket node (`lfht`) —
just an 8-byte pointer. That density is exactly what buys the throughput lead once nothing
fits in cache. So on pure hash-of-lists ops txn_hlist is **mid-pack when cache-resident but
the leader once the working set exceeds L3**, and its real differentiator — composable atomic
cross-structure commits — isn't even exercised here. (Regenerate:
`scripts/run_hlist_crossover.sh`, then `python3 scripts/plot_hlist_crossover.py`; data in
`scripts/hlist_crossover.csv` and `scripts/hlist_crossover_rss.csv`.)

### Takeaways

- Coherent **bidirectional** RCU iteration costs the reader **3–4 %** against the
  forward-only `rculist`, scales linearly to 192 cores, and beats seqlock by a
  quarter. Node layout moves reads far more than the list algorithm does: an
  inline `rcu_head` costs 14–34 %, and one node per cache line — the default,
  for the writers' sake — costs 14–32 % on a 30,000-node list.
- `txn_list` is the only design whose **writers scale**; reclamation and
  allocation are the lever, not the commit. The engine's per-CPU descriptor slab
  gets within 8 % of jemalloc's per-CPU arenas with no external allocator, and
  1.4–1.6× above plain glibc at 192 writers.
- The classic reader/writer-preference rwlock tradeoff is stark (reader-pref
  scales reads but starves writers; writer-pref collapses reads); **seqlock is
  unusable for long read-side traversals under a steady writer.**
- The random path's high-writer **collapse was premature escalation**, not the
  commit mechanism: a flat retry budget funnelled contending writers into the
  domain's single serial fair lane. With the budget scaled by the transaction's
  cost the 200-slot index holds ~20 Mops/s at 192 writers (0.1 under a flat 64,
  3–5 under a flat 256, both measured in July).
- Against reference **RLU**: RLU wins uncontended / at very low writer counts (its
  batched writeback), but `txn_list` **scales past it with writers** — ~15× on
  disjoint churn, ~14× on a 10k-node/2%-update write sweep, ~1.7× on the hash — and
  on a representative (non-tiny) list its **reads also lead 1.6–2.0×** (RLU pays a
  per-node validation the short-chain microbench hides). RLU stays competitive only
  in the low-writer / read-mostly-on-tiny-structures corner.
- On **RLU's own paper benchmark** (LWN #667720: a 1 000 × 100 hash with per-thread
  `%` updates) the reproduction holds — RCU meets-or-beats RLU and RLU-defer stops
  scaling past ~96 threads here — and `txn_hlist` lands with `rcu_hlist`/`cds_lfht`
  on the *scaling* side of that line (within 6–18 % of the RCU baseline, far above
  RLU), while `cds_lfht` — pinned to equal chains, denying it its resize design
  point — ties the RCU baseline at every update rate.
- That RCU win is **contingent on low write contention**. Holding chains constant and
  shrinking the bucket count (= write lanes) at 40 % updates, `rcu_hlist`'s per-bucket
  lock becomes a global one and it **collapses ~70× (55 → 0.8 Mops)** from best to worst,
  while `txn_hlist` and `cds_lfht` degrade gracefully (to 12 and 13 on one bucket); at 16
  buckets only txn and lfht still scale to 192 (RCU/RLU peak at 8 threads). `txn_hlist` is
  the only engine that both tracks RCU at low contention *and* stays robust under it.
- The **single-pointer hlist head** (8 B, no sentinel; `<urcu/rcu-txn-hlist.h>`) turns the
  hash comparison on **dataset size**. While the table is cache-resident `rcu_hlist` leads
  and `txn_hlist` is 2nd, but past L3 (from 2M buckets) txn_hlist **crosses into the
  lead** by 3–8 % — it touches the fewest cache lines per lookup, so it degrades most
  gracefully and beats `cds_lfht` up to **~1.7×** once the working set no longer fits in
  cache. The same density is a **2.5–14× smaller table** at 1M buckets (10.9 MB vs
  27 / 80 / 151 MB for lfht / RLU / RCU+lock). Mid-pack in cache, leader beyond it — on
  both throughput and footprint.

Where these results could apply beyond the benchmark — candidate data structures
for the urcu-txn API across the kernel, low-level libraries, databases and
networking — is surveyed in
[design/rcu-txn-use-cases.md](design/rcu-txn-use-cases.md).

## urcu-txn vs. McKenney's "existence structure" — 3-hash atomic move

Paul E. McKenney's *existence structure* (perfbook `CodeSamples/datastruct/`,
vendored under [`perfbook/`](perfbook/)) solves the same problem urcu-txn does —
making a multi-structure update atomic to readers — by the **opposite**
mechanism. existence puts the atomicity on the **read** side: one shared commit
word (`existence_flip`) flips the existence of an arbitrarily large batch of
elements at once, and every lookup pays a fixed tagged-load "existence check."
urcu-txn puts it in the **write** primitive: a single multi-word CAS commits the
touched slots, and readers traverse tax-free. This is the dual worth measuring.

The workload mirrors McKenney's `existence_3hash_uperf`: three chained hash
tables, each updater on a disjoint key range repeatedly moving a batch of keys
around the three tables in one atomic step (one MCAS for urcu-txn; one flip for
existence), plus optional reader threads doing a 3-table membership query. Both
harnesses report the same work-unit-normalized metrics — `ns/key-move` (update)
and `Mqueries/s` (read). The urcu-txn engine is
[`src/bench_txn_3hash.c`](src/bench_txn_3hash.c) (`make bench_txn_3hash`); the
existence side is the vendored uperf (patched to add readers + the per-move
metric). Mechanism comparison and fairness audit:
[design/txn-vs-existence-3hash.md](design/txn-vs-existence-3hash.md).

**Fair by construction:** both engines get one per-CPU real-time `call_rcu`
worker per updater; urcu-txn's MCAS descriptors come from its per-CPU
size-classed slab; and the urcu-txn node is 48 B / 1 cacheline vs existence's
192 B / 3 cachelines (the `existence_head` machinery is intrinsic to that
approach, not carried by urcu-txn). Worker `i` pins to CPU `i` (distinct
physical cores 0–191). Because the update side allocates (existence: a group +
three 192 B nodes per rotation), it is run under **both glibc and jemalloc**;
the read side does not allocate, so it is allocator-neutral.

### Result — atomic-move scaling to 192 cores

```sh
make urcu-txn && make bench_txn_3hash
make -C perfbook/datastruct/existence existence_3hash_uperf
RUNS=5 scripts/run_txn_vs_existence_scale.sh      # four panels: grow / fixed / size / read
python3 scripts/plot_txn_vs_existence_scale.py    # -> figures/txn_vs_existence_scale.png
```

![urcu-txn vs. existence, four panels. (1) Growing problem (keys/table = 5×cores):
both climb together to ~600 M key-moves/s — but the structure grows 192× along the
axis, so this is not a scaling curve. (2) Fixed problem (960 keys/table, commit width
matched at 3): under jemalloc the two engines track within ~12% at every core count
(ns/key-move), while the dotted glibc control shows existence's per-rotation allocation
climbing to ~1900 ns as urcu-txn reaches ~990. (3) Size dependence at 192 cores (load
factor 0.25): within 10% across a 16× size range, existence ahead at the small end.
(4) Reads: urcu-txn's tax-free single-cacheline traversal leads 1.2–1.3× at every reader
count to ~7.7 G queries/s](figures/txn_vs_existence_scale.png)

Re-measured 2026-10-04 on an idle machine, `urcu-txn-dev` @ 2793224e, both sides of
the txn build `-O2 -DNDEBUG`, best of 5 runs of 1 s (`RUNS=5`).

**Update side — at equal structure and equal width, the commit is a tie.** The
earliest draft compared a *growing* problem (keys/table = 5×cores, so the structure
grows 192× along the x-axis) under *glibc*, and reported urcu-txn ~3.4× ahead at 192.
Two corrections dissolve that gap. First, hold the problem **fixed** — 960 keys/table
at every core count, so the load factor is constant — and match the commit **width** (an
existence flip moves a whole group, so pit it against a matched 3-key-move urcu-txn
transaction rather than a 1-key move). Second, use a **per-CPU allocator** (jemalloc),
because existence allocates a group + three 192 B nodes *per rotation*. With all three,
the two engines track within ~12% at every core count (ns/key-move, lower is better;
`ex ÷ txn` > 1 means urcu-txn is faster):

| updater cores | urcu-txn | existence | ratio (ex ÷ txn) |
|---|---:|---:|---:|
| 1   | 110 | 109 | 0.99× |
| 16  | 134 | 137 | 1.02× |
| 32  | 140 | 146 | 1.04× |
| 64  | 181 | 199 | 1.10× |
| 96  | 243 | 273 | 1.12× |
| 192 | 352 | 351 | 1.00× |

(ns/key-move, jemalloc, best-of-5.) The ratio never exceeds 1.12×: at equal structure,
equal width, and a fair allocator the two commit mechanisms are a wash. Under **glibc**
(dotted in the figure) existence's per-rotation allocation collides on the arena locks:
its ns/key-move goes from 161 @1 to **1863 @192** while urcu-txn goes 98 → 989, a
1.7–2.4× gap that is an allocator artifact, not a commit-cost difference. And the tie is
not a single-size coincidence: sweeping the structure from 960 to 15 360 keys/table at
192 cores (load factor held at 0.25) the two stay within 10 %, existence ahead at the
small end (txn ÷ existence 1.08 at 960 keys, 0.99 at 15 360). This is the hash-specific
result — a 3-hash key-move transacts a fixed 3–5 pointers regardless of *n*; the ordered
skiplist ([design/rcu-txn-skiplist.md](design/rcu-txn-skiplist.md)), whose key-move
transacts O(log n) pointers (delete costs two records per level), is the case where the
same fixed-size control leaves urcu-txn a standing factor behind rather than tied.

**Read side — urcu-txn leads throughout.** One background updater + N readers: both
scale near-linearly to billions of queries/s, and urcu-txn's tax-free single-cacheline
traversal leads at every reader count — by 1.32× on one reader, narrowing to 1.19× at
191 — because existence pays its per-lookup existence check on every visit while
urcu-txn reads the raw node:

| reader cores | urcu-txn (Mq/s) | existence (Mq/s) | ratio |
|---|---:|---:|---:|
| 1   | 49.4 | 37.5 | 1.32× |
| 32  | 1542 | 1255 | 1.23× |
| 64  | 3007 | 2475 | 1.21× |
| 96  | 3945 | 3274 | 1.20× |
| 191 | 7710 | 6464 | 1.19× |

Caveats (see the design note): nodes are glibc/jemalloc `malloc` (not pooled;
existence's `procon` mpool recycles its group/node structs); reader flavor
differs (urcu-txn QSBR vs existence RCU_SIGNAL); and at 960 keys/table over 4096
buckets the tables run at a ~0.06 load factor, so the read result isolates per-lookup
tax + footprint rather than deep-chain traversal.

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
third_party/rax/                 vendored Valkey rax radix tree, C (BSD-3-Clause)
third_party/hot/                 vendored HOT, header-only C++14 (ISC);
                                   single-threaded + rowex (concurrent) headers
src/bench_hot.cpp                C++ shim exposing HOT to bench_one_st
third_party/cuckoo-trie/         vendored Cuckoo Trie, C (Unlicense)
src/bench_cuckoo.c               C shim exposing Cuckoo Trie to bench_one_st
third_party/wormhole/            vendored Wormhole (GPL-3.0; bench_wormhole_gpl only)
src/bench_wormhole_gpl.c         GPL-3.0 single-threaded Wormhole benchmark
src/bench_list_scale.c           bidirectional RCU list scaling benchmark
                                   (txn_sw_list/txn_list vs mutex/rwlock/seqlock/iscrw)
design/rcu-txn-use-cases.md      candidate data structures for the urcu-txn API
src/bench_iscrw.c                isolation wrapper linking the real bind9 isc_rwlock
src/iscrw-shim/probes-isc.h      no-op SystemTap probes shim for that standalone build
datasets/                        names CSVs (1M shuffled / trie-sorted + smoke)
urcu-build/                      our liburcu clone (fractal-trie-dev), gitignored
urcu-txn-build/                  our liburcu clone (urcu-txn-dev engine), gitignored
bind9-src/                       our bind9 clone + overlay + build, gitignored
scripts/build-bind9.sh           clones/overlays/builds the bind9 MT benches
scripts/run_scale_rw.sh          runs the per-engine scaling benches, combined table
scripts/plot_trie_tables.py      renders the trie result tables above as the
                                   figures/ PNGs (data transcribed from the
                                   README tables — no benchmark rerun needed)
```

## Licensing of vendored code

- `third_party/qp-trie` — CC0 / public domain (Tony Finch). See `NOTICE`.
- `third_party/libart` — BSD-2-Clause (Armon Dadgar). See `LICENSE`.
- `third_party/rax` — **BSD-3-Clause** (Redis Ltd. / Valkey contributors). The
  radix tree Valkey uses internally; `rax.c`, `rax.h` and `serverassert.h` are
  vendored verbatim from Valkey 9.2.0-rc1 and linked into `bench_one_st`'s
  `rax` engine. **Local change:** `rax_malloc.h` maps the allocator onto libc
  `malloc` instead of Valkey's `zmalloc` (jemalloc), so its lookup time is
  comparable but its RSS is glibc's per-node footprint, not jemalloc's. Built
  with Valkey's own `-O3 -flto`. See `LICENSE` / `PROVENANCE.txt`.
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
