// Minimal public API for sortbench core (phase 1 migration)
// Provides types and functions to run a single benchmark in-process
// without any CLI parsing, file I/O, or plotting.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace sortbench {

// Distributions supported by the generator
enum class Dist : int {
  random = 0,
  partial = 1,
  dups = 2,
  reverse = 3,
  sorted = 4,
  saw = 5,
  runs = 6,
  gauss = 7,
  exp = 8,
  zipf = 9,
  organpipe = 10,
  staggered = 11,
  runs_ht = 12,
};

// Output-friendly distribution names
std::string_view dist_name(Dist d);
const std::vector<std::string_view> &all_dist_names();

// Element types
enum class ElemType : int { i32, u32, i64, u64, f32, f64, str };
std::string_view elem_type_name(ElemType t);

struct CoreConfig {
  std::size_t N = 100000;
  Dist dist = Dist::random;
  ElemType type = ElemType::i32;
  int repeats = 5;
  int warmup = 0;
  std::optional<std::uint64_t> seed;     // fixed default if not set
  std::vector<std::string> algos;        // exact names (empty = all)
  std::vector<std::regex> algo_regex;    // optional regex filters
  // Exclusions (applied after inclusion filters)
  std::vector<std::string> exclude_algos;     // exact names to exclude
  std::vector<std::regex> exclude_regex;      // regex filters to exclude
  int partial_shuffle_pct = 10;          // for Dist::partial
  int dup_values = 100;                  // for Dist::dups/zipf
  bool verify = false;                   // verify vs std::sort
  bool assert_sorted = false;            // assert each run sorted
  int threads = 0;                       // OMP/TBB max threads (0 = default)
  std::vector<std::string> plugin_paths; // (phase 2) optional .so to load
  std::optional<std::string> baseline;   // compute speedups vs baseline algo
  // Extra distribution params
  double zipf_s = 1.2;        // Zipf skew parameter
  double runs_alpha = 1.5;    // heavy-tail alpha for runs_ht
  int stagger_block = 32;     // block size for 'staggered'
};

struct TimingStats {
  double median_ms = 0.0;
  double mean_ms = 0.0;
  double min_ms = 0.0;
  double max_ms = 0.0;
  double stddev_ms = 0.0;
};

struct ResultRow {
  std::string algo;
  std::size_t N = 0;
  std::string dist; // stable string name
  TimingStats stats;
  double speedup_vs_baseline = 1.0;
};

struct RunResult {
  ElemType type = ElemType::i32;
  std::size_t N = 0;
  std::string dist; // stable string name
  int repeats = 0;
  std::optional<std::uint64_t> seed;
  std::optional<std::string> baseline;
  std::vector<ResultRow> rows; // 1 per algorithm
};

// Execute a single benchmark run for the given config.
// Returns timing stats per algorithm. Throws std::runtime_error on invalid
// input and uses exceptions for fatal errors.
RunResult run_benchmark(const CoreConfig &cfg);

// Return available algorithm names for a given element type.
// Overload without plugin paths returns built-in (and header-available) algos only.
std::vector<std::string> list_algorithms(ElemType t);

// Return algorithms including any provided plugin shared objects for discovery.
// Loads plugins transiently, collects names for the specified type, then closes them.
std::vector<std::string> list_algorithms(
    ElemType t, const std::vector<std::string>& plugin_paths);

// Supported element types and distributions (metadata helpers)
std::vector<ElemType> supported_types();

// Formatting helpers (pure; no file I/O)
std::string to_csv(const RunResult& r,
                   bool with_header = true,
                   bool include_speedup = false);

std::string to_json(const RunResult& r,
                    bool include_speedup = false,
                    bool pretty = true);

std::string to_jsonl(const RunResult& r,
                     bool include_speedup = false);

} // namespace sortbench
