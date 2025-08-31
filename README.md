# bench-sort — Sorting Algorithm Benchmark (CLI + API)

sortbench is a high‑performance sorting benchmark with:
- A self‑contained C++ CLI for benchmarking algorithms across sizes, element types, and input distributions.
- A reusable C++ core library used by the CLI and server.
- An optional Go HTTP API (shell‑out or cgo) exposing sync and async benchmarking endpoints.
It reports per‑run stats (median/mean/min/max/stddev), can render plots via gnuplot, and supports runtime‑loadable plugins (multi‑type ABI). A compact Go HTTP API wraps the core to provide sync runs, async jobs, a small web UI, and metrics.

## 90‑Second Path

1) Install deps (Ubuntu):
```
sudo apt-get update && sudo apt-get install -y build-essential g++ make libtbb-dev curl jq
```

2) Build the CLI and run a quick check:
```
make
./sortbench --N 1e6 --format table --verify
```

3) Start the HTTP API (serves the web UI at http://localhost:8080/):
```
make api-go
PORT=8080 api/go/sortbench-api
```

4) One cURL demo (/run):
```
curl -s -X POST http://localhost:8080/run \
  -H 'Content-Type: application/json' \
  -d '{"N":100000,"dist":"runs","type":"i32","repeats":1,"algos":["std_sort"],"assert_sorted":true}' | jq
```

Read more: [Build](#build) · [Go HTTP API](#go-http-api) · [Web UI](#web-ui-apigostatic) · [Cross‑language plugins](#crosslanguage-plugins-c-abi) · [CI/DB](#ci) · [Full flags](#full-flag-reference)

## Features

- Algorithms: `std::sort`, `std::stable_sort`, heap sort, iterative merge sort, `timsort`, quicksort hybrid, quicksort 3-way, radix (for integral types), optional PDQSort, and user plugins. Additional educational/experimental algorithms are available: insertion sort, selection sort, bubble sort, comb sort, shell sort.
- Distributions: `random`, `partial`, `dups`, `reverse`, plus `sorted`, `saw`, `runs`, `gauss`, `exp`, `zipf`, `organpipe`, `staggered`, `runs_ht`.
- Element types: `i32`, `u32`, `i64`, `u64`, `f32`, `f64`, `str`.
- Repeats, warmup, verification, CSV/table/JSON/JSONL output, per‑run stats.
- Plotting: single plot or multiplot across multiple distributions (boxes or lines style).
- Plugins: simple C interface, v1 (int only) and v2 (multi‑type) ABIs.

## Build

Requirements: `g++` (C++20), OpenMP, `libtbb`/`dl`, optional gnuplot for plotting.

```
make            # build sortbench
make plugins    # build example plugins

# Build + run the Go API (shell-out mode)
make api-go     # (cd api/go && go mod tidy && go build .)
PORT=8080 api/go/sortbench-api   # or: cd api/go && go run .

# Build + run the Go API (CGO mode; calls core directly)
make            # builds libsortbench_core.a
make api-go-cgo
PORT=8080 SORTBENCH_CGO=1 api/go/sortbench-api
```

Notes:
- The API can run in two modes:
  - shell-out (default): spawns the CLI for discovery and runs.
  - CGO (set `SORTBENCH_CGO=1` and build with `-tags sortbench_cgo`): links core in-process for faster runs and discovery.
- When `SORTBENCH_CGO=1` is set but the binary wasn’t built with the tag, the API automatically falls back to shell-out. Startup logs indicate the effective mode.

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
- `organpipe`: values increase then decrease, forming an organ-pipe pattern.
- `staggered`: values arranged in staggered blocks (size via `--stagger-block`).
- `runs_ht`: sorted runs with heavy-tailed run lengths (alpha via `--runs-alpha`).

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

UI tips:
- Algorithms are populated from `/meta`. If you add plugin paths in the UI (Advanced → Plugins), the server will include those for discovery and you’ll see extra algorithms (e.g., from C/Rust/Zig plugins). Use absolute plugin paths if running the API from a subdirectory (like `api/go`).

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
- `--baseline NAME` — compute and report speedups vs this algorithm (adds `speedup_vs_baseline` to outputs and a per-run winner summary).
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

Dual-Pivot QuickSort plugin is included under `plugins/quicksort_dp.so`.

```
# List including the plugin
./sortbench --plugin plugins/quicksort_dp.so --list | grep dualpivot

# Run with the plugin
./sortbench --plugin plugins/quicksort_dp.so --algo std_sort,dualpivot_quicksort --N 2e5 --dist runs --repeat 3 --warmup 1 --format table
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

---

# Core library and Go API

## Core library

The CLI is now a thin wrapper over `libsortbench_core.a`.

Public headers:
- `include/sortbench/core.hpp` — Core API (`CoreConfig`, `RunResult`, `run_benchmark`, `list_algorithms`, `to_json`, `to_csv`, `to_jsonl`).
- `include/sortbench/capi.h` — Minimal C ABI for cgo/FFI (`sb_run_json`, `sb_list_algos_json`).

Linking example (C++):
```
#include "sortbench/core.hpp"

int main() {
  sortbench::CoreConfig cfg;
  cfg.N = 1'000'000; cfg.type = sortbench::ElemType::i32; cfg.dist = sortbench::Dist::runs;
  cfg.repeats = 3; cfg.algos = {"std_sort", "timsort"};
  auto res = sortbench::run_benchmark(cfg);
  std::string json = sortbench::to_json(res, /*include_speedup=*/false, /*pretty=*/true);
  // ...
}
```

## Go HTTP API

Two execution modes:
- Shell‑out (default): API spawns `./sortbench --format json ...` per request.
- cgo (set `SORTBENCH_CGO=1` and build with `-tags sortbench_cgo`): API calls the C++ core in‑process via a small C layer.

Endpoints:
- `GET /healthz` — liveness.
- `GET /readyz` — readiness (algo discovery + tiny smoke run).
- `GET /metrics` — Prometheus metrics.
- `GET /meta` — returns supported types, dists, and available algos per type (optionally with `?plugin=...`).
- `POST /run` — sync run. Body (JSON): `{ N, dist, type, repeats?, warmup?, seed?, algos?, threads?, assert_sorted?, baseline?, plugins?, timeout_ms? }`. Returns array of rows (median/mean/min/max/stddev).
- `POST /jobs` — async run. Returns `{ job_id }`.
- `GET /jobs/{id}` — job status/result.
- `POST /jobs/{id}/cancel` — cancel a running job.

Examples:
```
curl http://localhost:8080/healthz
curl http://localhost:8080/readyz
curl http://localhost:8080/meta | jq
curl -X POST http://localhost:8080/run \
  -H 'Content-Type: application/json' \
  -d '{"N":100000,"dist":"runs","type":"f32","repeats":2,"algos":["std_sort","timsort"],"assert_sorted":true}' | jq

curl -X POST http://localhost:8080/jobs \
  -H 'Content-Type: application/json' \
  -d '{"N":1000000,"dist":"runs","type":"i32","repeats":3,"algos":["std_sort","timsort"]}'
curl http://localhost:8080/jobs/<job_id> | jq
```

Frontend auto-detects `plugins/minmax_qs.so` and includes it in `/meta` and run requests if present. You can override or add more via the “Plugins” fields.

### Metrics

Prometheus at `/metrics`, including:
- `sortbench_requests_total{route,status}`
- `sortbench_request_duration_seconds{route}`
- `sortbench_jobs_running` (gauge)
- `sortbench_jobs_submitted_total`
- `sortbench_jobs_completed_total{result=done|failed|canceled}`

### Limits and configuration

The API enforces caps; all configurable via env vars (defaults in parentheses):
- `MAX_N` (10_000_000)
- `MAX_REPEATS` (50)
- `MAX_THREADS` (0 = unlimited)
- `MAX_JOBS` (64)
- `TIMEOUT_MS` (120000)
- `PORT` (8080)
- `SORTBENCH_BIN` (path to CLI for shell‑out mode)
- `SORTBENCH_CGO` (set to `1` to prefer in‑process core; build with `-tags sortbench_cgo`)

### Docker

Multi‑stage image builds the core + Go API (cgo mode) and runs as a small Debian base:

```
docker build -t sortbench-api .
docker run --rm -p 8080:8080 -e SORTBENCH_CGO=1 sortbench-api
```

### CI

GitHub Actions workflow builds the core, runs C++ core tests, and builds the Go API.

DB‑backed integration tests:
- A Postgres 15 service is started and the API runs with `DATABASE_URL` set. The workflow waits for Postgres readiness (`pg_isready`) and then gates on `/limits.db_enabled == true` before submitting jobs.
- Job polling tolerates transient HTTP errors and has a sane timeout. The workflow also performs a cancel test.
- Rate limits are raised in CI to avoid 429s during tight polling (`RATE_LIMIT_R` and `RATE_LIMIT_B`).

Local DB run:
```
export DATABASE_URL='postgres://postgres:postgres@localhost:5432/postgres?sslmode=disable'
export BENCHSORT_API_KEYS_FILE=keys.txt  # create with one key per line
cd api/go && go run .
# or build and run the static binary under api/go
```

The `/limits` endpoint shows whether DB is enabled and the effective mode (shell/cgo).

---

## Full flag reference

- `--N size|start-end`
- `--dist random|partial|dups|reverse|sorted|saw|runs|gauss|exp|zipf` (repeatable or comma‑list)
- `--repeat K`, `--warmup W`, `--seed S`
- `--algo name,name...`, `--algo-re REGEX,REGEX...`
- `--type i32|u32|i64|u64|f32|f64|str`
- `--format csv|table|json|jsonl`, `--no-header`, `--results PATH`
- `--verify`, `--assert-sorted`
- `--threads K`
- `--list`, `--plugin lib.so`
- `--print-build`
- `--build-plugin src.cpp --out lib.so`
- `--init-plugin [path.cpp]`
- `--plot out.png|.jpg`, `--plot-title T`, `--plot-size WxH`, `--plot-style boxes|lines`, `--plot-layout RxC`, `--keep-plot-artifacts`

Defaults: when no `--algo`/`--algo-re` is provided, sortbench excludes very slow educational algorithms from runs by default: `bubble_sort`, `insertion_sort`, `selection_sort`. They still appear in `--list` and can be run explicitly by naming them or via regex. Use `--exclude`/`--exclude-re` to add more exclusions.

## Custom algorithms shim (optional)

The static core library can optionally include a custom algorithm shim. This requires sources that are not part of this repo. To enable locally:

```
make ENABLE_CUSTOM_SHIM=1
```

This defines `SORTBENCH_HAS_CUSTOM_SHIM` and includes `custom_algo_shim.cpp`, registering `custom`/`customv2` for `i32`/`f32` where supported. CI and default builds do not include the shim.
## Notes

- Plotting requires gnuplot in PATH. CSV/JSON/JSONL outputs work without it.
- PDQSort, GNU parallel std::sort, and `std::execution` parallel variants appear only if headers are available at build time.
- JSONL is best for accumulating many runs; CSV is also append‑friendly within one invocation.
