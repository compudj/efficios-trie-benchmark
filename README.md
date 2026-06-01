# efficios-trie-benchmark

Benchmarks for the **Fractal Trie (FT)** against competing trie / ordered-map
implementations:

| Engine     | Implementation                                  | Source                          |
|------------|-------------------------------------------------|---------------------------------|
| `ft`       | Fractal Trie (eager)                            | userspace-rcu (built by us)     |
| `ft_skip`  | Fractal Trie, speculative skip                  | userspace-rcu                   |
| `ft_cand`  | Fractal Trie, candidate (memcmp-verified)       | userspace-rcu                   |
| `qp`       | qp-trie (quadbit popcount), Tony Finch          | `third_party/qp-trie` (vendored)|
| `art`      | Adaptive Radix Tree (libart), Armon Dadgar      | `third_party/libart` (vendored) |
| `judy`     | JudyL / JudySL                                  | system `libJudy`                |
| BIND9 QP   | `dns_qpmulti` (multithreaded test only)         | external bind9 (TODO)           |

## Dependency model (hybrid)

The competitors that are *stable* are vendored into this repo (`third_party/`).
The Fractal Trie itself lives in **userspace-rcu** and is under active
development, so it is **not** vendored. Instead we build our **own** out-of-tree
liburcu from the userspace-rcu source into `urcu-build/` (configurable). This
keeps the FT we link in sync with the live source (e.g. new API such as
`cds_ft_compact`) while never modifying the userspace-rcu tree itself.

## Building

Edit `config.mk` if your paths differ (defaults assume userspace-rcu at
`/home/efficios/git/userspace-rcu`), then:

```sh
make urcu     # build our own liburcu (the Fractal Trie) from $(URCU_SRC)
make          # build the benchmark binaries
```

Requires `libjudy-dev` (provides `<Judy.h>` and `-lJudy`).

Re-run `make urcu` whenever the userspace-rcu FT source changes.

## Single-threaded benchmark — `bench_one_st`

Single dataset, single engine, run in its own process for accurate RSS:

```sh
./bench_one_st <dataset> <engine>
#   dataset: u32d u32s u64d u64s dns dict paths   (all generated synthetically)
#   engine:  ft ft_skip ft_cand judy qp art
# output: <ns/op> <RSS_kB>      ('-' for string engines on integer datasets)
```

Example sweep:

```sh
for e in ft ft_skip ft_cand judy qp art; do printf '%-9s ' "$e"; ./bench_one_st dns "$e"; done
```

Useful env vars (see `src/bench_one_st.c`): `FT_BENCH_COMPACT` (compact between
build and query), `FT_DUMP_STATS`, `N_KEYS` / `WARMUP` / `RUNS` (compile-time).

### The qp-trie `qp` vs `fn` gotcha

qp-trie dispatches through `Tbl.o`, which links with **exactly one** backend
object (`qp.o`, `fn.o`, ...) — all export the same symbols, so the linker takes
whichever you pass. We vendor and link **only `qp.o`**, so the `qp` engine is
always the real qp-trie. (The original `build_one_st_bench.sh` linked `fn.o`,
silently benchmarking a different structure under the `qp` label.)

## Multithreaded benchmark — TODO

The bind9 "lookup names" MT scaling test (`load-names.c`, with the `FT_PRIME`
cache-priming phase) and the standalone `bench_scale_rw_bind9.c` read/write
scaling test will be added next. Both pull in BIND9's QP-trie, so they require a
patched bind9 build.

## Licensing of vendored code

- `third_party/qp-trie` — CC0 / public domain (Tony Finch). See `NOTICE`.
- `third_party/libart` — BSD-2-Clause (Armon Dadgar). See `LICENSE`.
