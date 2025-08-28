# sortbench — Sorting Algorithm Benchmark CLI

sortbench is a self‑contained CLI to benchmark sorting algorithms across data sizes, element types, and input distributions. It prints results (median/mean/min/max/stddev), can render plots via gnuplot, and supports runtime‑loadable plugins (with multi‑type ABI).

## Features

- Algorithms: `std::sort`, `std::stable_sort`, heap sort, iterative merge sort, quicksort hybrid, radix (for integral types), optional PDQSort, and user plugins.
- Distributions: `random`, `partial`, `dups`, `reverse`, plus `sorted`, `saw`, `runs`, `gauss`, `exp`, `zipf`.
- Element types: `i32`, `u32`, `i64`, `u64`, `f32`, `f64`, `str`.
- Repeats, warmup, verification, CSV/table/JSON/JSONL output, per‑run stats.
- Plotting: single plot or multiplot across multiple distributions (boxes or lines style).
- Plugins: simple C interface, v1 (int only) and v2 (multi‑type) ABIs.

## Build

Requirements: `g++` (C++20), OpenMP, `libtbb`/`dl`, optional gnuplot for plotting.

```
make            # build sortbench
make plugins    # build example plugins
```

Print the compiler flags used by the build:

```
./sortbench --print-build
```

## Quick start

```
# 1M random i32s, all algorithms, CSV
./sortbench --N 1e6

# Table output and verification
./sortbench --N 1e6 --format table --verify

# Only std::sort and radix; 7 repeats, 1 warmup
./sortbench --N 2e6 --algo std_sort,radix_sort_lsd --repeat 7 --warmup 1

# Floats, Gaussian distribution, JSONL (appends to bench_result.jsonl)
./sortbench --type f32 --dist gauss --format jsonl

# Plot medians with min..max (requires gnuplot)
./sortbench --N 1e6 --algo std_sort,quicksort_hybrid --plot out.png
```

## Distributions

- `random`: uniform ints/floats.
- `partial`: mostly sorted with random swaps; control with `--partial-pct P`.
- `dups`: limited cardinality; control with `--dups-k K`.
- `reverse`: strictly descending 0..N-1.
- `sorted`: strictly ascending 0..N-1.
- `saw`: sawtooth pattern (period around 1024 by default).
- `runs`: random values arranged into sorted runs (run length around 2048 by default).
- `gauss`: Gaussian/normal; ints mapped and clamped.
- `exp`: exponential (positive skew); ints/floats supported.
- `zipf`: skewed duplicates over K values (`--dups-k`), Zipf s≈1.2.

Specify multiple distributions:

```
--dist random reverse         # space separated
--dist=random,dups,sorted     # comma separated
```

## Algorithms

- `std_sort`, `std_stable_sort` (always).
- `heap_sort`, `merge_sort_opt`, `quicksort_hybrid` (always).
- `radix_sort_lsd` (integral types only).
- `pdqsort` (if header present at build).
- Parallel variants (if headers present): `std_sort_par`, `std_sort_par_unseq`, `gnu_parallel_sort`.
- `custom`, `customv2` (if `custom_algo.hpp` is available for the chosen type).

Filter algorithms:

```
--algo name[,name...]           # exact names
--algo-re 'regex1,regex2'       # case-insensitive regex filters
```

## Element types

```
--type i32|u32|i64|u64|f32|f64|str   # default i32
```

- `str` generates fixed-length, lexicographically sortable strings and supports the same distributions (random, partial, dups, reverse, sorted, saw, runs, gauss, exp, zipf).

## Output formats and files

- `--format csv|table|json|jsonl` (default `csv`).
- Results are printed to stdout and also written to a file by default:
  - CSV → `bench_result.csv` (appends on multi-run sweeps in one invocation).
  - JSON → `bench_result.json` (overwrites per invocation).
  - JSONL → `bench_result.jsonl` (appends, one JSON object per line).
  - Table → `bench_result.txt` (overwrites per invocation).
- Override result path with `--results PATH`.
- Suppress file writes for scripting with `--no-file` (prints to stdout only).

## Plotting

Enable plotting with `--plot out.png|.jpg` (requires gnuplot).

Common flags:

- `--plot-title T` — title text.
- `--plot-size WxH` — pixels (default 1000x600).
- `--keep-plot-artifacts` — keep temporary `.dat` and `.gp` files.
- `--output DIR` — write plot artifacts (`.dat`/`.gp` and plot-adjacent `.csv`) under `DIR` to keep your image directory clean.
- `--plot-style boxes|lines` — default `boxes`. `lines` uses `linespoints` + `yerrorbars`.

Single distribution:

- Renders medians as boxes (or lines) and overlays `yerrorbars` for `min..max`.

Multiple distributions in one run:

- sortbench writes a `.dat` for each distribution next to the image (e.g., `out.random.dat`). When `--output DIR` is set, these go under `DIR` as `out.random.dat` instead.
- Generates a single `.gp` with `set multiplot` and one subplot per distribution.
- Layout: default `Ndists x 1`; configure with `--plot-layout RxC`.

## Repeats, warmup, verification

```
--repeat K        # repeats per algorithm; default 5 (min 1)
--warmup W        # non-timed warmup runs per algorithm; default 0
--verify          # verify equality vs std::sort for correctness
--assert-sorted   # assert each run result is sorted (fast-fail)
```

## Data size and seeding

```
--N size            # e.g. 100000, 1e6, 10m, 2g
--N start-end       # geometric sweep start→end (powers of ~2)
--seed S            # RNG seed (default: fixed constant for determinism)
```

## Parallelism

```
--threads K         # limit OpenMP/TBB/gnu_parallel threads (if available)
```

## Plugins

List built-in and plugin algorithms:

```
./sortbench --list
./sortbench --plugin plugins/example_plugin.so --list
```

Load one or more plugins:

```
--plugin path/to/lib1.so --plugin lib2.so
```

Build a plugin source with the recorded build flags:

```
./sortbench --build-plugin plugins/my.cpp --out plugins/my.so
```

Scaffold a new plugin (multi‑type v2 template by default):

```
./sortbench --init-plugin                    # writes plugins/my_plugin.cpp
./sortbench --init-plugin plugins/Foo.cpp    # custom path
```

### Plugin ABI (v1 and v2)

Header: `sortbench_plugin.h`.

- v1 (back‑compat, int‑only):
  - `struct sortbench_algo_v1 { const char* name; void (*run_int)(int*, int); }`
  - Export: `int sortbench_get_algorithms_v1(const sortbench_algo_v1** arr, int* count)`
- v2 (preferred, multi‑type):
  - `struct sortbench_algo_v2 { const char* name; /* optional */ run_i32, run_u32, run_i64, run_u64, run_f32, run_f64; }`
  - Export: `int sortbench_get_algorithms_v2(const sortbench_algo_v2** arr, int* count)`
  - Provide only the entrypoints you support (others = nullptr).

The loader prefers v2 and registers only entrypoints matching the current element type `--type`. If v2 is absent, it falls back to v1 for `i32`.

## Full flag reference

- `--N size|start-end`
- `--dist random|partial|dups|reverse|sorted|saw|runs|gauss|exp|zipf` (repeatable or comma‑list)
- `--repeat K`, `--warmup W`, `--seed S`
- `--algo name[,name...]`, `--algo-re REGEX[,REGEX...]`
- `--type i32|u32|i64|u64|f32|f64|str`
- `--format csv|table|json|jsonl`, `--no-header`, `--results PATH`
- `--verify`, `--assert-sorted`
- `--threads K`
- `--list`, `--plugin lib.so`
- `--print-build`
- `--build-plugin src.cpp --out lib.so`
- `--init-plugin [path.cpp]`
- `--plot out.png|.jpg`, `--plot-title T`, `--plot-size WxH`, `--plot-style boxes|lines`, `--plot-layout RxC`, `--keep-plot-artifacts`

## Notes

- Plotting requires gnuplot in PATH. CSV/JSON/JSONL outputs work without it.
- PDQSort, GNU parallel std::sort, and `std::execution` parallel variants appear only if headers are available at build time.
- JSONL is best for accumulating many runs; CSV is also append‑friendly within one invocation.
