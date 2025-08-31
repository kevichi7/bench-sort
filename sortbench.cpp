#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#if defined(__has_include)
#if __has_include(<tbb/global_control.h>)
#include <tbb/global_control.h>
#define SORTBENCH_HAS_TBB_HEADER 1
#else
#define SORTBENCH_HAS_TBB_HEADER 0
#endif
#else
#define SORTBENCH_HAS_TBB_HEADER 0
#endif

#include "sortbench_plugin.h"
#include "sortbench/core.hpp"

#if defined(__has_include)
#if __has_include("pdqsort.h")
#include "pdqsort.h"
#define SORTBENCH_HAS_PDQ 1
#elif __has_include("pdqsort.hpp")
#include "pdqsort.hpp"
#define SORTBENCH_HAS_PDQ 1
#else
#define SORTBENCH_HAS_PDQ 0
#endif
#if __has_include("custom_algo.hpp")
#include "custom_algo.hpp"
#define SORTBENCH_HAS_CUSTOM 1
#else
#define SORTBENCH_HAS_CUSTOM 0
#endif
#else
#define SORTBENCH_HAS_PDQ 0
#define SORTBENCH_HAS_CUSTOM 0
#endif

#if defined(__has_include)
#if __has_include(<execution>)
#include <execution>
#define SORTBENCH_HAS_STD_PAR 1
#else
#define SORTBENCH_HAS_STD_PAR 0
#endif
#if __has_include(<parallel/algorithm>)
#include <parallel/algorithm>
#define SORTBENCH_HAS_GNU_PAR 1
#else
#define SORTBENCH_HAS_GNU_PAR 0
#endif
#else
#define SORTBENCH_HAS_STD_PAR 0
#define SORTBENCH_HAS_GNU_PAR 0
#endif

using Clock = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

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
  runs_ht = 12
};
static constexpr std::array<std::string_view, 13> kDistNames{
    "random",    "partial",  "dups",     "reverse",  "sorted",
    "saw",       "runs",     "gauss",    "exp",      "zipf",
    "organpipe", "staggered","runs_ht"};

enum class OutFmt : int { csv = 0, table = 1, json = 2, jsonl = 3 };
enum class PlotStyle : int { boxes = 0, lines = 1 };

enum class ElemType : int { i32, u32, i64, u64, f32, f64, str };

static inline std::string_view elem_type_name(ElemType t) {
  switch (t) {
  case ElemType::i32:
    return "i32";
  case ElemType::u32:
    return "u32";
  case ElemType::i64:
    return "i64";
  case ElemType::u64:
    return "u64";
  case ElemType::f32:
    return "f32";
  case ElemType::f64:
    return "f64";
  case ElemType::str:
    return "str";
  }
  return "i32";
}

// Command-line options
struct Options {
  std::size_t N = 100000;         // array size
  std::vector<std::size_t> Ns;    // optional sweep sizes
  Dist dist = Dist::random;       // primary distribution (compat)
  std::vector<Dist> dists;        // one or more distributions to run
  int repeats = 5;                // repeats for median
  int warmup = 0;                 // warm-up runs (not timed)
  std::optional<uint64_t> seed;   // seed
  std::vector<std::string> algos; // selected algorithms; empty = all
  bool csv_header = true;         // print header by default
  int partial_shuffle_pct = 10;   // percent of elements to shuffle in partial
  int dup_values = 100;           // cardinality for duplicates distribution
  bool verify = false;            // verify correctness
  bool list = false;              // list available algorithms and exit
  std::vector<std::string> plugin_paths;       // shared objects to load
  OutFmt format = OutFmt::csv;                 // output format
  bool print_build = false;                    // show compiler/flags used
  std::optional<std::string> build_plugin_src; // path to plugin .cpp
  std::optional<std::string> build_plugin_out; // output .so path
  std::optional<std::string>
      init_plugin_out;                     // path to write scaffold plugin .cpp
  std::optional<std::string> results_path; // results output path
  std::optional<std::string> baseline;     // baseline algo for speedup compare
  bool no_file = false;                    // suppress writing results files
  std::optional<std::string> output_dir;   // directory for plot artifacts
  // Plotting options
  std::optional<std::string> plot_path; // output image path (png/jpeg)
  std::string plot_title;               // optional plot title
  int plot_w = 1000;                    // plot width
  int plot_h = 600;                     // plot height
  bool keep_plot_artifacts = false;     // keep .dat/.gp files
  ElemType type = ElemType::i32;        // element type
  bool assert_sorted = false; // assert results are sorted after each run
  int threads = 0;            // max threads (0 = default)
  std::vector<std::regex> algo_regex; // optional regex filters for algo names
  std::vector<std::string> exclude_algos; // exact names to exclude
  std::vector<std::regex> exclude_regex;  // regex exclusions
  bool multi_plot_accumulate =
      false; // internal: accumulate plot data, no gnuplot per call
  std::optional<std::string>
      plot_dat_path; // internal: where to write .dat for this call
  // Plot layout/style
  int plot_rows = 0;                       // 0 = auto (Ndists), otherwise rows
  int plot_cols = 0;                       // 0 = auto (1), otherwise cols
  PlotStyle plot_style = PlotStyle::boxes; // boxes or lines
  // Extra distribution params
  double zipf_s = 1.2;        // Zipf skew parameter
  double runs_alpha = 1.5;    // heavy-tail alpha for runs_ht
  int stagger_block = 32;     // block size for 'staggered'
};

// Utilities
static inline uint64_t default_seed() {
  // Use a fixed default seed for deterministic runs when --seed is not provided
  return 0x9E3779B97F4A7C15ULL; // golden ratio constant
}

static inline std::string to_lower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::optional<Dist> parse_dist(std::string s) {
  s = to_lower(std::move(s));
  if (s == "random")
    return Dist::random;
  if (s == "partial")
    return Dist::partial;
  if (s == "dups")
    return Dist::dups;
  if (s == "reverse")
    return Dist::reverse;
  if (s == "sorted")
    return Dist::sorted;
  if (s == "saw")
    return Dist::saw;
  if (s == "runs")
    return Dist::runs;
  if (s == "gauss" || s == "normal")
    return Dist::gauss;
  if (s == "exp" || s == "exponential")
    return Dist::exp;
  if (s == "zipf")
    return Dist::zipf;
  if (s == "organpipe" || s == "organ-pipe")
    return Dist::organpipe;
  if (s == "staggered")
    return Dist::staggered;
  if (s == "runs_ht" || s == "kruns_ht")
    return Dist::runs_ht;
  return std::nullopt;
}

static std::size_t parse_size_expr(const std::string &s) {
  // Accept plain integers, scientific (e-notation), and k/m/g suffixes
  try {
    // fast path for pure integer
    size_t pos = 0;
    auto v = std::stoull(s, &pos, 10);
    if (pos == s.size())
      return static_cast<std::size_t>(v);
  } catch (...) {
  }
  // Suffixes
  if (!s.empty()) {
    char last =
        static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
    if (last == 'k' || last == 'm' || last == 'g') {
      auto base = s.substr(0, s.size() - 1);
      double d = std::strtod(base.c_str(), nullptr);
      double mul = (last == 'k' ? 1e3 : (last == 'm' ? 1e6 : 1e9));
      return static_cast<std::size_t>(d * mul);
    }
  }
  // Scientific or general double
  double d = std::strtod(s.c_str(), nullptr);
  if (d <= 0)
    throw std::runtime_error("Invalid size expression: " + s);
  return static_cast<std::size_t>(d);
}

static void print_usage(const char *argv0) {
  std::cerr << "Usage: " << argv0
            << " [--N size|start-end] [--dist "
               "random|partial|dups|reverse|sorted|saw|runs|gauss|exp|zipf|organpipe|staggered|runs_ht]"
               " [--repeat k] [--warmup w] [--algo name[,name...]] [--seed s] "
               "[--no-header] "
               "[--verify]"
               " [--partial-pct p] [--dups-k k] [--list] [--plugin lib.so ...] "
               "[--format csv|table|json|jsonl] [--algo-re REGEX] [--exclude name[,name...]] [--threads "
               "K] [--results PATH] [--init-plugin [path.cpp]]\n";
  std::cerr << "       --dist can be repeated or take multiple values (e.g., "
               "--dist random dups or --dist=random,dups)\n";
  std::cerr << "       --print-build (print compiler/flags used)\n";
  std::cerr << "       --build-plugin <src.cpp> --out <lib.so> (compile plugin "
               "with recorded flags)\n";
  std::cerr << "       --baseline NAME (compute speedups vs this algo)\n";
  std::cerr << "       --no-file (print to stdout only; no results files)\n";
  std::cerr << "       --plot <out.png|.jpg> [--plot-title T] [--plot-size "
               "WxH] (generate a plot with med/min/max)\n";
  std::cerr
      << "       --output DIR (write plot artifacts .dat/.gp under DIR)\n";
  std::cerr << "       --keep-plot-artifacts (keep temporary .dat/.gp next to "
               "the image)\n";
  std::cerr << "       --plot-layout RxC (multiplot grid when multiple --dist "
               "given; default = Nx1)\n";
  std::cerr << "       --plot-style boxes|lines (default boxes; lines uses "
               "linespoints + yerrorbars)\n";
  std::cerr << "       --type i32|u32|i64|u64|f32|f64|str (element type; "
               "default i32)\n";
  std::cerr << "       --assert-sorted (check each run result is sorted; fails "
               "fast)\n";
  std::cerr << "       --zipf-s S (Zipf skew, default 1.2)\n";
  std::cerr << "       --runs-alpha A (heavy-tail alpha for runs_ht, default 1.5)\n";
  std::cerr << "       --stagger-block B (block size for 'staggered', default 32)\n";
}

static Options parse_args(int argc, char **argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto get_value_inline =
        [&](std::string_view arg,
            std::string_view key) -> std::optional<std::string> {
      if (arg.size() > key.size() + 1 && arg.substr(0, key.size()) == key &&
          arg[key.size()] == '=') {
        return std::string(arg.substr(key.size() + 1));
      }
      return std::nullopt;
    };
    auto need_value = [&](std::string_view flag) {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") +
                                 std::string(flag));
      }
      return std::string(argv[++i]);
    };
    if (a == "--N" || a == "-N" || a.rfind("--N=", 0) == 0) {
      std::string v = get_value_inline(a, "--N").value_or(need_value(a));
      // Support range: start-end
      auto dash = v.find('-');
      if (dash == std::string::npos) {
        opt.N = parse_size_expr(v);
      } else {
        std::string s1 = v.substr(0, dash);
        std::string s2 = v.substr(dash + 1);
        std::size_t start = parse_size_expr(s1);
        std::size_t end = parse_size_expr(s2);
        if (start == 0 || end == 0 || start > end)
          throw std::runtime_error("Invalid --N range");
        opt.N = start;
        opt.Ns.clear();
        // Geometric sweep by powers of ~2
        std::size_t cur = start;
        while (cur < end) {
          opt.Ns.push_back(cur);
          if (cur > (std::numeric_limits<std::size_t>::max() >> 1))
            break;
          std::size_t next = cur << 1;
          if (next <= cur)
            break;
          cur = next;
        }
        if (opt.Ns.empty() || opt.Ns.back() != end)
          opt.Ns.push_back(end);
      }
    } else if (a == "--dist" || a.rfind("--dist=", 0) == 0) {
      // Support: --dist val [val2 val3 ...] and --dist=val1,val2
      std::string firstv =
          get_value_inline(a, "--dist").value_or(need_value(a));
      auto push_dist = [&](const std::string &s) {
        auto dd = parse_dist(s);
        if (!dd)
          throw std::runtime_error(std::string("Invalid --dist: ") + s);
        opt.dists.push_back(*dd);
      };
      // Comma-separated list in the first token
      size_t start = 0;
      bool any = false;
      while (start <= firstv.size()) {
        size_t pos = firstv.find(',', start);
        std::string token = firstv.substr(
            start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!token.empty()) {
          push_dist(token);
          any = true;
        }
        if (pos == std::string::npos)
          break;
        else
          start = pos + 1;
      }
      // Space-separated additional values: keep consuming until next flag
      while (i + 1 < argc) {
        std::string_view peek = argv[i + 1];
        if (!peek.empty() && peek[0] == '-')
          break;
        std::string sv = std::string(argv[++i]);
        push_dist(sv);
        any = true;
      }
      if (any)
        opt.dist = opt.dists.front();
    } else if (a == "--repeat" || a == "-r" || a.rfind("--repeat=", 0) == 0) {
      std::string v = get_value_inline(a, "--repeat").value_or(need_value(a));
      opt.repeats = std::stoi(v);
      if (opt.repeats <= 0)
        opt.repeats = 1;
    } else if (a == "--warmup" || a.rfind("--warmup=", 0) == 0) {
      std::string v = get_value_inline(a, "--warmup").value_or(need_value(a));
      opt.warmup = std::stoi(v);
      if (opt.warmup < 0)
        opt.warmup = 0;
    } else if (a == "--algo" || a == "-a" || a.rfind("--algo=", 0) == 0) {
      std::string v = get_value_inline(a, "--algo").value_or(need_value(a));
      // Allow comma-separated list
      std::string cur;
      for (char c : v) {
        if (c == ',') {
          if (!cur.empty()) {
            opt.algos.push_back(to_lower(cur));
            cur.clear();
          }
        } else
          cur.push_back(c);
      }
      if (!cur.empty())
        opt.algos.push_back(to_lower(cur));
    } else if (a == "--exclude" || a.rfind("--exclude=", 0) == 0) {
      std::string v = get_value_inline(a, "--exclude").value_or(need_value(a));
      std::string cur;
      for (char c : v) {
        if (c == ',') {
          if (!cur.empty()) { opt.exclude_algos.push_back(to_lower(cur)); cur.clear(); }
        } else cur.push_back(c);
      }
      if (!cur.empty()) opt.exclude_algos.push_back(to_lower(cur));
    } else if (a == "--seed" || a.rfind("--seed=", 0) == 0) {
      std::string v = get_value_inline(a, "--seed").value_or(need_value(a));
      opt.seed = std::stoull(v);
    } else if (a == "--no-header") {
      opt.csv_header = false;
    } else if (a == "--partial-pct" || a.rfind("--partial-pct=", 0) == 0) {
      std::string v =
          get_value_inline(a, "--partial-pct").value_or(need_value(a));
      opt.partial_shuffle_pct = std::stoi(v);
      if (opt.partial_shuffle_pct < 0)
        opt.partial_shuffle_pct = 0;
      if (opt.partial_shuffle_pct > 100)
        opt.partial_shuffle_pct = 100;
    } else if (a == "--dups-k" || a.rfind("--dups-k=", 0) == 0) {
      std::string v = get_value_inline(a, "--dups-k").value_or(need_value(a));
      opt.dup_values = std::stoi(v);
      if (opt.dup_values < 1)
        opt.dup_values = 1;
    } else if (a == "--verify") {
      opt.verify = true;
    } else if (a == "--list") {
      opt.list = true;
    } else if (a == "--plugin" || a.rfind("--plugin=", 0) == 0) {
      if (auto iv = get_value_inline(a, "--plugin")) {
        opt.plugin_paths.push_back(*iv);
      } else {
        if (i + 1 >= argc)
          throw std::runtime_error("Missing value for --plugin");
        opt.plugin_paths.push_back(std::string(argv[++i]));
      }
    } else if (a == "--format" || a.rfind("--format=", 0) == 0) {
      std::string v = get_value_inline(a, "--format").value_or(need_value(a));
      v = to_lower(std::move(v));
      if (v == "csv")
        opt.format = OutFmt::csv;
      else if (v == "table")
        opt.format = OutFmt::table;
      else if (v == "json")
        opt.format = OutFmt::json;
      else if (v == "jsonl")
        opt.format = OutFmt::jsonl;
      else
        throw std::runtime_error("Invalid --format (csv|table|json|jsonl): " +
                                 v);
    } else if (a == "--print-build") {
      opt.print_build = true;
    } else if (a == "--build-plugin" || a.rfind("--build-plugin=", 0) == 0) {
      opt.build_plugin_src =
          get_value_inline(a, "--build-plugin").value_or(need_value(a));
    } else if (a == "--out" || a.rfind("--out=", 0) == 0) {
      opt.build_plugin_out =
          get_value_inline(a, "--out").value_or(need_value(a));
    } else if (a == "--init-plugin" || a.rfind("--init-plugin=", 0) == 0) {
      if (auto iv = get_value_inline(a, "--init-plugin")) {
        opt.init_plugin_out = *iv;
      } else {
        // Optional value; default to plugins/my_plugin.cpp if none provided or
        // next token is a flag
        if (i + 1 < argc) {
          std::string_view peek = argv[i + 1];
          if (!peek.empty() && peek[0] != '-') {
            opt.init_plugin_out = std::string(argv[++i]);
          } else {
            opt.init_plugin_out = std::string("plugins/my_plugin.cpp");
          }
        } else {
          opt.init_plugin_out = std::string("plugins/my_plugin.cpp");
        }
      }
    } else if (a == "--plot" || a.rfind("--plot=", 0) == 0) {
      opt.plot_path = get_value_inline(a, "--plot").value_or(need_value(a));
    } else if (a == "--results" || a.rfind("--results=", 0) == 0) {
      opt.results_path =
          get_value_inline(a, "--results").value_or(need_value(a));
    } else if (a == "--baseline" || a.rfind("--baseline=", 0) == 0) {
      opt.baseline = get_value_inline(a, "--baseline").value_or(need_value(a));
    } else if (a == "--no-file") {
      opt.no_file = true;
    } else if (a == "--output" || a.rfind("--output=", 0) == 0) {
      opt.output_dir = get_value_inline(a, "--output").value_or(need_value(a));
    } else if (a == "--plot-title" || a.rfind("--plot-title=", 0) == 0) {
      opt.plot_title =
          get_value_inline(a, "--plot-title").value_or(need_value(a));
    } else if (a == "--plot-size" || a.rfind("--plot-size=", 0) == 0) {
      std::string v =
          get_value_inline(a, "--plot-size").value_or(need_value(a));
      auto x = v.find('x');
      if (x == std::string::npos)
        throw std::runtime_error("--plot-size must be WxH");
      opt.plot_w = std::stoi(v.substr(0, x));
      opt.plot_h = std::stoi(v.substr(x + 1));
    } else if (a == "--keep-plot-artifacts") {
      opt.keep_plot_artifacts = true;
    } else if (a == "--plot-layout" || a.rfind("--plot-layout=", 0) == 0) {
      std::string v =
          get_value_inline(a, "--plot-layout").value_or(need_value(a));
      auto x = v.find('x');
      if (x == std::string::npos)
        throw std::runtime_error("--plot-layout must be RxC");
      opt.plot_rows = std::stoi(v.substr(0, x));
      opt.plot_cols = std::stoi(v.substr(x + 1));
      if (opt.plot_rows <= 0 || opt.plot_cols <= 0)
        throw std::runtime_error("--plot-layout must be positive RxC");
    } else if (a == "--plot-style" || a.rfind("--plot-style=", 0) == 0) {
      std::string v =
          get_value_inline(a, "--plot-style").value_or(need_value(a));
      v = to_lower(std::move(v));
      if (v == "boxes")
        opt.plot_style = PlotStyle::boxes;
      else if (v == "lines")
        opt.plot_style = PlotStyle::lines;
      else
        throw std::runtime_error("Invalid --plot-style (boxes|lines)");
    } else if (a == "--zipf-s" || a.rfind("--zipf-s=", 0) == 0) {
      std::string v = get_value_inline(a, "--zipf-s").value_or(need_value(a));
      opt.zipf_s = std::strtod(v.c_str(), nullptr);
      if (!(opt.zipf_s > 0.0)) opt.zipf_s = 1.2;
    } else if (a == "--runs-alpha" || a.rfind("--runs-alpha=", 0) == 0) {
      std::string v = get_value_inline(a, "--runs-alpha").value_or(need_value(a));
      opt.runs_alpha = std::strtod(v.c_str(), nullptr);
      if (!(opt.runs_alpha > 0.0)) opt.runs_alpha = 1.5;
    } else if (a == "--stagger-block" || a.rfind("--stagger-block=", 0) == 0) {
      std::string v = get_value_inline(a, "--stagger-block").value_or(need_value(a));
      opt.stagger_block = std::stoi(v);
      if (opt.stagger_block <= 0) opt.stagger_block = 32;
    } else if (a == "--threads" || a.rfind("--threads=", 0) == 0) {
      std::string v = get_value_inline(a, "--threads").value_or(need_value(a));
      opt.threads = std::stoi(v);
      if (opt.threads < 0)
        opt.threads = 0;
    } else if (a == "--type" || a.rfind("--type=", 0) == 0) {
      std::string v = get_value_inline(a, "--type").value_or(need_value(a));
      v = to_lower(std::move(v));
      if (v == "i32")
        opt.type = ElemType::i32;
      else if (v == "u32")
        opt.type = ElemType::u32;
      else if (v == "i64")
        opt.type = ElemType::i64;
      else if (v == "u64")
        opt.type = ElemType::u64;
      else if (v == "f32")
        opt.type = ElemType::f32;
      else if (v == "f64")
        opt.type = ElemType::f64;
      else if (v == "str")
        opt.type = ElemType::str;
      else
        throw std::runtime_error("Invalid --type");
    } else if (a == "--algo-re" || a.rfind("--algo-re=", 0) == 0) {
      std::string v = get_value_inline(a, "--algo-re").value_or(need_value(a));
      // Allow comma-separated regex list
      size_t start = 0;
      while (start <= v.size()) {
        size_t pos = v.find(',', start);
        std::string pat = v.substr(
            start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!pat.empty()) {
          try {
            opt.algo_regex.emplace_back(pat, std::regex::icase);
          } catch (const std::regex_error &) {
            throw std::runtime_error(std::string("Invalid --algo-re regex: ") +
                                     pat);
          }
        }
        if (pos == std::string::npos)
          break;
        else
          start = pos + 1;
      }
    } else if (a == "--exclude-re" || a.rfind("--exclude-re=", 0) == 0) {
      std::string v = get_value_inline(a, "--exclude-re").value_or(need_value(a));
      size_t start = 0;
      while (start <= v.size()) {
        size_t pos = v.find(',', start);
        std::string pat = v.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!pat.empty()) {
          try { opt.exclude_regex.emplace_back(pat, std::regex::icase); }
          catch (const std::regex_error &) {
            throw std::runtime_error(std::string("Invalid --exclude-re regex: ") + pat);
          }
        }
        if (pos == std::string::npos) break; else start = pos + 1;
      }
    } else if (a == "--assert-sorted") {
      opt.assert_sorted = true;
    } else if (a == "--help" || a == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << a << "\n";
      print_usage(argv[0]);
      std::exit(2);
    }
  }
  return opt;
}

// Data generation
template <class T>
static std::vector<T> make_data(std::size_t n, Dist dist, std::mt19937_64 &rng,
                                int partial_pct, int dups_k) {
  std::vector<T> v;
  v.resize(n);
  if constexpr (std::is_same_v<T, std::string>) {
    auto to_key = [](uint64_t x) {
      // 12-char base-26 lowercase string for predictable lex order
      std::string s(12, 'a');
      for (int i = 11; i >= 0; --i) {
        s[static_cast<std::size_t>(i)] = static_cast<char>('a' + (x % 26u));
        x /= 26u;
      }
      return s;
    };
    auto pad_num = [](uint64_t x) {
      // zero-padded decimal for stable numeric-lex ordering
      std::string s = std::to_string(x);
      if (s.size() < 12)
        s.insert(s.begin(), static_cast<std::string::size_type>(12 - s.size()),
                 '0');
      return s;
    };
    if (dist == Dist::random) {
      std::uniform_int_distribution<uint64_t> d(0, UINT64_C(1'000'000'000'000));
      for (std::size_t i = 0; i < n; ++i)
        v[i] = to_key(d(rng));
    } else if (dist == Dist::sorted) {
      for (std::size_t i = 0; i < n; ++i)
        v[i] = pad_num(i);
    } else if (dist == Dist::reverse) {
      for (std::size_t i = 0; i < n; ++i)
        v[i] = pad_num(n - 1 - i);
    } else if (dist == Dist::dups) {
      int k = std::max(1, dups_k);
      std::uniform_int_distribution<int> d(0, k - 1);
      for (std::size_t i = 0; i < n; ++i)
        v[i] = pad_num(static_cast<uint64_t>(d(rng)));
    } else if (dist == Dist::partial) {
      for (std::size_t i = 0; i < n; ++i)
        v[i] = pad_num(i);
      std::size_t to_shuffle =
          (n * static_cast<std::size_t>(std::clamp(partial_pct, 0, 100))) / 100;
      if (to_shuffle > n)
        to_shuffle = n;
      std::uniform_int_distribution<std::size_t> d(0, n ? n - 1 : 0);
      for (std::size_t i = 0; i < to_shuffle; ++i) {
        std::size_t a = d(rng), b = d(rng);
        std::swap(v[a], v[b]);
      }
    } else if (dist == Dist::saw) {
      std::size_t period =
          std::max<std::size_t>(std::min<std::size_t>(n ? n : 1, 1024), 1);
      for (std::size_t i = 0; i < n; ++i)
        v[i] = pad_num(i % period);
    } else if (dist == Dist::runs) {
      std::uniform_int_distribution<uint64_t> d(0, UINT64_C(1'000'000'000'000));
      for (std::size_t i = 0; i < n; ++i)
        v[i] = to_key(d(rng));
      const std::size_t run_len =
          std::max<std::size_t>(1, std::min<std::size_t>(n ? n : 1, 2048));
      for (std::size_t i = 0; i < n; i += run_len) {
        std::size_t r = std::min(run_len, n - i);
        std::sort(v.begin() + static_cast<std::ptrdiff_t>(i),
                  v.begin() + static_cast<std::ptrdiff_t>(i + r));
      }
    } else if (dist == Dist::gauss) {
      std::normal_distribution<double> nd(0.0, 1.0);
      for (std::size_t i = 0; i < n; ++i) {
        double x = nd(rng);
        uint64_t k = static_cast<uint64_t>(std::llround((x + 4.0) * 1e10));
        v[i] = to_key(k);
      }
    } else if (dist == Dist::exp) {
      std::exponential_distribution<double> ed(1.0);
      for (std::size_t i = 0; i < n; ++i) {
        double x = ed(rng);
        uint64_t k = static_cast<uint64_t>(x * 1e10);
        v[i] = to_key(k);
      }
    } else if (dist == Dist::zipf) {
      int K = std::max(1, dups_k);
      const double s = 1.2;
      std::vector<double> cum(static_cast<std::size_t>(K));
      double Z = 0.0;
      for (int k = 1; k <= K; ++k)
        Z += 1.0 / std::pow(static_cast<double>(k), s);
      double run = 0.0;
      for (int k = 1; k <= K; ++k) {
        run += (1.0 / std::pow(static_cast<double>(k), s)) / Z;
        cum[static_cast<std::size_t>(k - 1)] = run;
      }
      std::uniform_real_distribution<double> ud(0.0, 1.0);
      for (std::size_t i = 0; i < n; ++i) {
        double u = ud(rng);
        auto it = std::lower_bound(cum.begin(), cum.end(), u);
        int idx = static_cast<int>(std::distance(cum.begin(), it));
        v[i] = pad_num(static_cast<uint64_t>(idx));
      }
    }
    return v;
  } else {
    if (dist == Dist::random) {
      if constexpr (std::is_integral_v<T>) {
        std::uniform_int_distribution<std::make_unsigned_t<T>> d(
            0, std::numeric_limits<std::make_unsigned_t<T>>::max());
        for (std::size_t i = 0; i < n; ++i)
          v[i] = static_cast<T>(d(rng));
      } else if constexpr (std::is_floating_point_v<T>) {
        std::uniform_real_distribution<T> d(T(0), T(1));
        for (std::size_t i = 0; i < n; ++i)
          v[i] = d(rng);
      } else {
        // Non-numeric types are handled above; leave default-initialized
      }
    } else if (dist == Dist::reverse) {
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(n - 1 - i);
    } else if (dist == Dist::dups) {
      int k = std::max(1, dups_k);
      std::uniform_int_distribution<int> d(0, k - 1);
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(d(rng));
    } else if (dist == Dist::sorted) {
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(i);
    } else if (dist == Dist::saw) {
      std::size_t period =
          std::max<std::size_t>(std::min<std::size_t>(n ? n : 1, 1024), 1);
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(i % period);
    } else if (dist == Dist::runs) {
      // Random values arranged into sorted runs of fixed length
      const std::size_t run_len =
          std::max<std::size_t>(1, std::min<std::size_t>(n ? n : 1, 2048));
      // fill random
      if constexpr (std::is_integral_v<T>) {
        std::uniform_int_distribution<std::make_unsigned_t<T>> d(
            0, std::numeric_limits<std::make_unsigned_t<T>>::max());
        for (std::size_t i = 0; i < n; ++i)
          v[i] = static_cast<T>(d(rng));
      } else if constexpr (std::is_floating_point_v<T>) {
        std::uniform_real_distribution<T> d(T(0), T(1));
        for (std::size_t i = 0; i < n; ++i)
          v[i] = d(rng);
      } else {
        // Non-numeric handled above
      }
      for (std::size_t i = 0; i < n; i += run_len) {
        std::size_t r = std::min(run_len, n - i);
        std::sort(v.begin() + static_cast<std::ptrdiff_t>(i),
                  v.begin() + static_cast<std::ptrdiff_t>(i + r));
      }
    } else if (dist == Dist::gauss) {
      if constexpr (std::is_integral_v<T>) {
        using Lim = std::numeric_limits<T>;
        std::normal_distribution<double> nd(0.0, 1.0);
        const double minv = static_cast<double>(Lim::min());
        const double maxv = static_cast<double>(Lim::max());
        const double mean = std::is_signed_v<T> ? 0.0 : (maxv / 2.0);
        const double stddev = (maxv - (std::is_signed_v<T> ? minv : 0.0)) / 8.0;
        for (std::size_t i = 0; i < n; ++i) {
          double x = mean + stddev * nd(rng);
          if (x < minv)
            x = minv;
          if (x > maxv)
            x = maxv;
          v[i] = static_cast<T>(x);
        }
      } else {
        std::normal_distribution<T> nd(T(0), T(1));
        for (std::size_t i = 0; i < n; ++i)
          v[i] = nd(rng);
      }
    } else if (dist == Dist::exp) {
      std::exponential_distribution<double> ed(1.0);
      if constexpr (std::is_integral_v<T>) {
        using Lim = std::numeric_limits<T>;
        const double minv = static_cast<double>(Lim::min());
        const double maxv = static_cast<double>(Lim::max());
        for (std::size_t i = 0; i < n; ++i) {
          double x = ed(rng);
          // scale into [0, maxv] or [minv, maxv] depending on signedness
          if constexpr (std::is_signed_v<T>) {
            double y = x;                  // positive skew
            double s = (maxv - 0.0) / 8.0; // scale to reasonable range
            y = 0.0 + s * y;
            if (y > maxv)
              y = maxv;
            v[i] = static_cast<T>(y);
          } else {
            double s = maxv / 8.0;
            double y = s * x;
            if (y > maxv)
              y = maxv;
            v[i] = static_cast<T>(y);
          }
        }
      } else {
        for (std::size_t i = 0; i < n; ++i)
          v[i] = static_cast<T>(ed(rng));
      }
    } else if (dist == Dist::zipf) {
      int K = std::max(1, dups_k);
      const double s = 1.2; // skew parameter
      std::vector<double> cum(static_cast<std::size_t>(K));
      double Z = 0.0;
      for (int k = 1; k <= K; ++k)
        Z += 1.0 / std::pow(static_cast<double>(k), s);
      double run = 0.0;
      for (int k = 1; k <= K; ++k) {
        run += (1.0 / std::pow(static_cast<double>(k), s)) / Z;
        cum[static_cast<std::size_t>(k - 1)] = run;
      }
      std::uniform_real_distribution<double> ud(0.0, 1.0);
      for (std::size_t i = 0; i < n; ++i) {
        double u = ud(rng);
        auto it = std::lower_bound(cum.begin(), cum.end(), u);
        int idx = static_cast<int>(std::distance(cum.begin(), it));
        v[i] = static_cast<T>(idx);
      }
    } else { // partial
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(i);
      // Shuffle a percentage of elements
      std::size_t to_shuffle =
          (n * static_cast<std::size_t>(std::clamp(partial_pct, 0, 100))) / 100;
      if (to_shuffle > n)
        to_shuffle = n;
      // pick indices and swap randomly
      std::uniform_int_distribution<std::size_t> d(0, n ? n - 1 : 0);
      for (std::size_t i = 0; i < to_shuffle; ++i) {
        std::size_t a = d(rng);
        std::size_t b = d(rng);
        std::swap(v[a], v[b]);
      }
    }
  }
  return v;
}

// Algorithms
namespace algos {

// insertion sort used as a small-array threshold helper
template <class Iter> inline void insertion_sort(Iter first, Iter last) {
  for (Iter i = first + (first == last ? 0 : 1); i < last; ++i) {
    auto key = *i;
    Iter j = i;
    while (j > first && *(j - 1) > key) {
      *j = *(j - 1);
      --j;
    }
    *j = key;
  }
}

// Heap sort using std heap primitives
template <class T> inline void heap_sort(std::vector<T> &v) {
  std::make_heap(v.begin(), v.end());
  std::sort_heap(v.begin(), v.end());
}

// Classic insertion sort (exposed as a full algo)
template <class T> inline void insertion_sort_full(std::vector<T> &v) {
  insertion_sort(v.begin(), v.end());
}

// Selection sort (O(n^2))
template <class T> inline void selection_sort(std::vector<T> &v) {
  const std::size_t n = v.size();
  for (std::size_t i = 0; i + 1 < n; ++i) {
    std::size_t min_i = i;
    for (std::size_t j = i + 1; j < n; ++j) if (v[j] < v[min_i]) min_i = j;
    if (min_i != i) std::swap(v[i], v[min_i]);
  }
}

// Bubble sort (with early-exit)
template <class T> inline void bubble_sort(std::vector<T> &v) {
  const std::size_t n = v.size();
  if (n < 2) return;
  bool swapped = true;
  for (std::size_t pass = 0; pass < n - 1 && swapped; ++pass) {
    swapped = false;
    for (std::size_t i = 0; i + 1 < n - pass; ++i) {
      if (v[i + 1] < v[i]) { std::swap(v[i], v[i + 1]); swapped = true; }
    }
  }
}

// Comb sort (gap shrink variant of bubble sort)
template <class T> inline void comb_sort(std::vector<T> &v) {
  const std::size_t n = v.size();
  if (n < 2) return;
  double gap = (double)n;
  const double shrink = 1.3;
  bool swapped = true;
  while (gap > 1.0 || swapped) {
    gap = std::floor(gap / shrink);
    if (gap < 1.0) gap = 1.0;
    std::size_t igap = static_cast<std::size_t>(gap);
    swapped = false;
    for (std::size_t i = 0; i + igap < n; ++i) {
      std::size_t j = i + igap;
      if (v[j] < v[i]) { std::swap(v[i], v[j]); swapped = true; }
    }
  }
}

// Shell sort (Ciura gap sequence extended)
template <class T> inline void shell_sort(std::vector<T> &v) {
  static const int ciura_base[] = {1, 4, 10, 23, 57, 132, 301, 701};
  std::vector<int> gaps(ciura_base, ciura_base + (sizeof(ciura_base)/sizeof(int)));
  // Extend Ciura sequence
  while ((long long)gaps.back() * 2.25 < (long long)v.size()) {
    gaps.push_back(int(gaps.back() * 2.25));
  }
  for (int gi = (int)gaps.size() - 1; gi >= 0; --gi) {
    int gap = gaps[gi]; if (gap <= 0) continue;
    for (std::size_t i = gap; i < v.size(); ++i) {
      auto tmp = v[i];
      std::size_t j = i;
      while (j >= (std::size_t)gap && tmp < v[j - gap]) {
        v[j] = v[j - gap];
        j -= gap;
      }
      v[j] = tmp;
    }
  }
}

// Iterative bottom-up merge sort with reusable buffer and std::copy
template <class T> inline void merge_sort_opt(std::vector<T> &v) {
  const std::size_t n = v.size();
  if (n < 2)
    return;
  std::vector<T> buf(n);
  // width = size of runs to merge
  for (std::size_t width = 1; width < n; width <<= 1) {
    for (std::size_t i = 0; i < n; i += (width << 1)) {
      std::size_t left = i;
      std::size_t mid = std::min(i + width, n);
      std::size_t right = std::min(i + (width << 1), n);
      if (mid >= right)
        continue;
      std::size_t a = left, b = mid, k = left;
      while (a < mid && b < right) {
        if (v[a] <= v[b])
          buf[k++] = v[a++];
        else
          buf[k++] = v[b++];
      }
      if (a < mid)
        std::copy(v.begin() + static_cast<std::ptrdiff_t>(a),
                  v.begin() + static_cast<std::ptrdiff_t>(mid),
                  buf.begin() + static_cast<std::ptrdiff_t>(k)),
            k += (mid - a);
      if (b < right)
        std::copy(v.begin() + static_cast<std::ptrdiff_t>(b),
                  v.begin() + static_cast<std::ptrdiff_t>(right),
                  buf.begin() + static_cast<std::ptrdiff_t>(k)),
            k += (right - b);
      // copy back
      std::copy(buf.begin() + static_cast<std::ptrdiff_t>(left),
                buf.begin() + static_cast<std::ptrdiff_t>(right),
                v.begin() + static_cast<std::ptrdiff_t>(left));
    }
  }
}

// Quicksort hybrid with median-of-three and insertion sort threshold
template <class Iter> inline void quicksort_hybrid_impl(Iter first, Iter last) {
  constexpr int INSERTION_THRESHOLD = 64; // simplified constant
  while (last - first > INSERTION_THRESHOLD) {
    Iter a = first, b = last - 1, m = first + (last - first) / 2;
    // median-of-three
    if (*m < *a)
      std::iter_swap(m, a);
    if (*b < *m)
      std::iter_swap(b, m);
    if (*m < *a)
      std::iter_swap(m, a);
    auto pivot = *m;
    // Hoare partition
    Iter i = first - 1, j = last;
    for (;;) {
      do {
        ++i;
      } while (*i < pivot);
      do {
        --j;
      } while (pivot < *j);
      if (i >= j)
        break;
      std::iter_swap(i, j);
    }
    // recurse into smaller part first; loop on larger (tail recursion)
    if (j - first < last - (j + 1)) {
      quicksort_hybrid_impl(first, j + 1);
      first = j + 1;
    } else {
      quicksort_hybrid_impl(j + 1, last);
      last = j + 1;
    }
  }
  insertion_sort(first, last);
}

template <class T> inline void quicksort_hybrid(std::vector<T> &v) {
  if (!v.empty())
    quicksort_hybrid_impl(v.begin(), v.end());
}

// 3-way quicksort (Dijkstra) good for many equal keys
template <class Iter> inline void quicksort_3way_impl(Iter lo, Iter hi) {
  while (hi - lo > 64) {
    Iter i = lo, lt = lo, gt = hi - 1;
    auto pivot = *(lo + (hi - lo) / 2);
    while (i <= gt) {
      if (*i < pivot) { std::iter_swap(lt++, i++); }
      else if (pivot < *i) { std::iter_swap(i, gt--); }
      else { ++i; }
    }
    // Recurse into smaller partition first
    auto left_size = lt - lo;
    auto right_size = hi - (gt + 1);
    if (left_size < right_size) {
      if (left_size > 1) quicksort_3way_impl(lo, lt);
      lo = gt + 1;
    } else {
      if (right_size > 1) quicksort_3way_impl(gt + 1, hi);
      hi = lt;
    }
  }
  insertion_sort(lo, hi);
}

template <class T> inline void quicksort_3way(std::vector<T> &v) {
  if (!v.empty()) quicksort_3way_impl(v.begin(), v.end());
}

// Simplified TimSort: detects natural runs, extends short runs to a minrun
// via binary insertion sort, and merges adjacent runs using stable merge.
template <class T>
static inline void binary_insertion_sort(std::vector<T> &v, std::size_t lo,
                                         std::size_t hi) {
  for (std::size_t i = lo + 1; i < hi; ++i) {
    T x = v[i];
    std::size_t left = lo, right = i;
    while (left < right) {
      std::size_t mid = left + ((right - left) >> 1);
      if (!(x < v[mid]))
        left = mid + 1;
      else
        right = mid;
    }
    for (std::size_t j = i; j > left; --j)
      v[j] = std::move(v[j - 1]);
    v[left] = std::move(x);
  }
}

template <class T>
static inline void stable_merge(std::vector<T> &v, std::size_t lo,
                                std::size_t mid, std::size_t hi,
                                std::vector<T> &buf) {
  std::size_t n1 = mid - lo, n2 = hi - mid;
  buf.resize(std::max(buf.size(), n1));
  for (std::size_t i = 0; i < n1; ++i)
    buf[i] = std::move(v[lo + i]);
  std::size_t i = 0, j = mid, k = lo;
  while (i < n1 && j < hi) {
    if (!(v[j] < buf[i]))
      v[k++] = std::move(buf[i++]);
    else
      v[k++] = std::move(v[j++]);
  }
  while (i < n1)
    v[k++] = std::move(buf[i++]);
  // right half already in place
}

template <class T> inline void timsort(std::vector<T> &v) {
  const std::size_t n = v.size();
  if (n < 2)
    return;
  // Compute minrun (like Python's timsort)
  auto calc_minrun = [](std::size_t n) {
    unsigned r = 0;
    while (n >= 64) {
      r |= (n & 1u);
      n >>= 1u;
    }
    return static_cast<std::size_t>(n + r);
  };
  const std::size_t minrun = calc_minrun(n);
  struct Run {
    std::size_t base;
    std::size_t len;
  };
  std::vector<Run> stack;
  std::vector<T> buf;
  std::size_t i = 0;
  while (i < n) {
    std::size_t lo = i;
    if (i + 1 == n) {
      i = n;
    } else {
      // Identify run: ascending or descending
      if (v[i + 1] < v[i]) { // descending
        std::size_t j = i + 2;
        while (j < n && v[j] < v[j - 1])
          ++j;
        std::reverse(v.begin() + static_cast<std::ptrdiff_t>(i),
                     v.begin() + static_cast<std::ptrdiff_t>(j));
        i = j;
      } else { // ascending
        std::size_t j = i + 2;
        while (j < n && !(v[j] < v[j - 1]))
          ++j;
        i = j;
      }
    }
    std::size_t run_hi = i;
    std::size_t run_len = run_hi - lo;
    // Extend to minrun
    if (run_len < minrun) {
      std::size_t end = std::min(lo + minrun, n);
      binary_insertion_sort(v, lo, end);
      run_hi = end;
      run_len = run_hi - lo;
      i = run_hi;
    }
    stack.push_back(Run{lo, run_len});
    // Merge to maintain non-increasing run sizes (simple invariant)
    while (stack.size() >= 2) {
      auto &A = stack[stack.size() - 2];
      auto &B = stack[stack.size() - 1];
      if (A.len <= B.len) {
        stable_merge(v, A.base, A.base + A.len, B.base + B.len, buf);
        A.len += B.len;
        stack.pop_back();
      } else {
        break;
      }
    }
  }
  // Merge remaining runs from bottom
  while (stack.size() > 1) {
    auto B = stack.back();
    stack.pop_back();
    auto &A = stack.back();
    stable_merge(v, A.base, A.base + A.len, B.base + B.len, buf);
    A.len += B.len;
  }
}

// Radix sort (LSD) base 256 for non-negative integers with reusable buffers
template <class T> inline void radix_sort_lsd(std::vector<T> &v) {
  static_assert(std::is_unsigned<T>::value || std::is_integral<T>::value,
                "radix_sort_lsd requires integral type");
  if (v.size() < 2)
    return;
  // For signed types, bias to treat bits as unsigned while preserving order
  using U = std::make_unsigned_t<T>;
  std::vector<U> a(v.size());
  std::vector<U> b(v.size());
  // copy with bias
  if constexpr (std::is_signed_v<T>) {
    constexpr U bias = U(1) << (sizeof(T) * 8 - 1);
    for (std::size_t i = 0; i < v.size(); ++i)
      a[i] = static_cast<U>(v[i]) ^ bias;
  } else {
    for (std::size_t i = 0; i < v.size(); ++i)
      a[i] = static_cast<U>(v[i]);
  }
  constexpr int BYTES = static_cast<int>(sizeof(U));
  std::array<std::size_t, 256> count{};
  for (int pass = 0; pass < BYTES; ++pass) {
    count.fill(0);
    const int shift = pass * 8;
    for (std::size_t i = 0; i < a.size(); ++i) {
      unsigned byte = static_cast<unsigned>((a[i] >> shift) & 0xFFu);
      ++count[byte];
    }
    // exclusive prefix sum
    std::size_t sum = 0;
    for (int i = 0; i < 256; ++i) {
      std::size_t c = count[static_cast<std::size_t>(i)];
      count[static_cast<std::size_t>(i)] = sum;
      sum += c;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      unsigned byte = static_cast<unsigned>((a[i] >> shift) & 0xFFu);
      b[count[byte]++] = a[i];
    }
    a.swap(b);
  }
  // un-bias and copy back
  if constexpr (std::is_signed_v<T>) {
    constexpr U bias = U(1) << (sizeof(T) * 8 - 1);
    for (std::size_t i = 0; i < v.size(); ++i)
      v[i] = static_cast<T>(a[i] ^ bias);
  } else {
    for (std::size_t i = 0; i < v.size(); ++i)
      v[i] = static_cast<T>(a[i]);
  }
}

} // namespace algos

// Registry of algorithms (templated by element type)
template <class T> struct AlgoT {
  std::string name;
  std::function<void(std::vector<T> &)> run;
};

template <class T> static std::vector<AlgoT<T>> build_registry_t() {
  std::vector<AlgoT<T>> regs;
  regs.push_back({"std_sort", [](auto &v) { std::sort(v.begin(), v.end()); }});
  regs.push_back({"std_stable_sort",
                  [](auto &v) { std::stable_sort(v.begin(), v.end()); }});
#if SORTBENCH_HAS_STD_PAR
  regs.push_back({"std_sort_par", [](auto &v) {
                    std::sort(std::execution::par, v.begin(), v.end());
                  }});
  regs.push_back({"std_sort_par_unseq", [](auto &v) {
                    std::sort(std::execution::par_unseq, v.begin(), v.end());
                  }});
#endif
#if SORTBENCH_HAS_GNU_PAR
  regs.push_back({"gnu_parallel_sort",
                  [](auto &v) { __gnu_parallel::sort(v.begin(), v.end()); }});
#endif
  regs.push_back({"heap_sort", [](auto &v) { algos::heap_sort(v); }});
  regs.push_back({"merge_sort_opt", [](auto &v) { algos::merge_sort_opt(v); }});
  regs.push_back({"insertion_sort", [](auto &v) { algos::insertion_sort_full(v); }});
  regs.push_back({"selection_sort", [](auto &v) { algos::selection_sort(v); }});
  regs.push_back({"bubble_sort", [](auto &v) { algos::bubble_sort(v); }});
  regs.push_back({"comb_sort", [](auto &v) { algos::comb_sort(v); }});
  regs.push_back({"shell_sort", [](auto &v) { algos::shell_sort(v); }});
  regs.push_back({"timsort", [](auto &v) { algos::timsort(v); }});
  regs.push_back(
      {"quicksort_hybrid", [](auto &v) { algos::quicksort_hybrid(v); }});
  regs.push_back({"quicksort_3way", [](auto &v) { algos::quicksort_3way(v); }});
  if constexpr (std::is_integral_v<T>) {
    regs.push_back(
        {"radix_sort_lsd", [](auto &v) { algos::radix_sort_lsd(v); }});
  }
#if SORTBENCH_HAS_PDQ
  regs.push_back({"pdqsort", [](auto &v) { pdqsort(v.begin(), v.end()); }});
#endif
#if SORTBENCH_HAS_CUSTOM
  if constexpr (std::is_same_v<T, int>) {
    regs.push_back({"custom", [](auto &v) { custom_algo::sort_int(v); }});
    regs.push_back({"customv2", [](auto &v) { custom_algo::sort_int_v2(v); }});
  } else if constexpr (std::is_same_v<T, float>) {
    regs.push_back({"custom", [](auto &v) { custom_algo::sort_float(v); }});
    regs.push_back(
        {"customv2", [](auto &v) { custom_algo::sort_float_v2(v); }});
  } else {
    // For other types, provide a safe fallback under the same name
    regs.push_back({"custom", [](auto &v) { std::sort(v.begin(), v.end()); }});
    regs.push_back(
        {"customv2", [](auto &v) { std::sort(v.begin(), v.end()); }});
  }
#endif
  return regs;
}

using PluginHandle = void *;
using get_algos_v1_fn = int (*)(const sortbench_algo_v1 **, int *);
using get_algos_v2_fn = int (*)(const sortbench_algo_v2 **, int *);

template <class T>
static void load_plugins_t(const std::vector<std::string> &paths,
                           std::vector<AlgoT<T>> &regs,
                           std::vector<PluginHandle> &handles) {
  for (const auto &p : paths) {
    void *h = dlopen(p.c_str(), RTLD_NOW);
    if (!h) {
      std::cerr << "Failed to dlopen '" << p << "': " << dlerror() << "\n";
      continue;
    }
    dlerror();
    // Prefer v2 multi-type interface
    if (auto fn2 = reinterpret_cast<get_algos_v2_fn>(
            dlsym(h, "sortbench_get_algorithms_v2"));
        fn2 && !dlerror()) {
      const sortbench_algo_v2 *arr = nullptr;
      int count = 0;
      int ok = fn2(&arr, &count);
      if (!ok || !arr || count <= 0) {
        dlclose(h);
        continue;
      }
      bool any_added = false;
      for (int i = 0; i < count; ++i) {
        const auto &a = arr[i];
        if (!a.name)
          continue;
        std::string nm = a.name;
        // Select the appropriate function pointer for T
        if constexpr (std::is_same_v<T, int>) {
          if (!a.run_i32)
            continue;
          auto run = a.run_i32;
          regs.push_back({nm, [run](std::vector<int> &v) {
                            if (!v.empty())
                              run(v.data(), (int)v.size());
                          }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, unsigned int>) {
          if (!a.run_u32)
            continue;
          auto run = a.run_u32;
          regs.push_back({nm, [run](std::vector<unsigned int> &v) {
                            if (!v.empty())
                              run(v.data(), (int)v.size());
                          }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, long long>) {
          if (!a.run_i64)
            continue;
          auto run = a.run_i64;
          regs.push_back({nm, [run](std::vector<long long> &v) {
                            if (!v.empty())
                              run(v.data(), (int)v.size());
                          }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, unsigned long long>) {
          if (!a.run_u64)
            continue;
          auto run = a.run_u64;
          regs.push_back({nm, [run](std::vector<unsigned long long> &v) {
                            if (!v.empty())
                              run(v.data(), (int)v.size());
                          }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, float>) {
          if (!a.run_f32)
            continue;
          auto run = a.run_f32;
          regs.push_back({nm, [run](std::vector<float> &v) {
                            if (!v.empty())
                              run(v.data(), (int)v.size());
                          }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, double>) {
          if (!a.run_f64)
            continue;
          auto run = a.run_f64;
          regs.push_back({nm, [run](std::vector<double> &v) {
                            if (!v.empty())
                              run(v.data(), (int)v.size());
                          }});
          any_added = true;
        }
      }
      if (any_added) {
        handles.push_back(h);
        continue;
      }
      // If no algorithms for this T, close handle and continue
      dlclose(h);
      continue;
    }
    // Fallback to v1 (int-only)
    auto fn1 = reinterpret_cast<get_algos_v1_fn>(
        dlsym(h, "sortbench_get_algorithms_v1"));
    if (!fn1 || dlerror()) {
      dlclose(h);
      continue;
    }
    const sortbench_algo_v1 *arr = nullptr;
    int count = 0;
    int ok = fn1(&arr, &count);
    if (!ok || !arr || count <= 0) {
      dlclose(h);
      continue;
    }
    bool any_added = false;
    if constexpr (std::is_same_v<T, int>) {
      for (int i = 0; i < count; ++i) {
        const auto &a = arr[i];
        if (!a.name || !a.run_int)
          continue;
        std::string nm = a.name;
        regs.push_back({nm, [run = a.run_int](std::vector<int> &v) {
                          if (!v.empty())
                            run(v.data(), (int)v.size());
                        }});
        any_added = true;
      }
    }
    if (any_added) {
      handles.push_back(h);
    } else {
      dlclose(h);
    }
  }
}

static bool name_selected(const std::vector<std::string> &selected,
                          const std::vector<std::regex> &selected_re,
                          const std::string &name) {
  if (selected.empty() && selected_re.empty())
    return true;
  std::string ln = to_lower(name);
  for (const auto &s : selected)
    if (s == ln)
      return true;
  for (const auto &re : selected_re) {
    if (std::regex_search(name, re) || std::regex_search(ln, re))
      return true;
  }
  return false;
}

template <class T>
static double benchmark_once_t(const std::function<void(std::vector<T> &)> &fn,
                               const std::vector<T> &original,
                               std::vector<T> &work, bool check_sorted = false,
                               const char *algo_name = nullptr) {
  // Reuse allocated buffer and copy data efficiently
  work.resize(original.size());
  std::copy(original.begin(), original.end(), work.begin());
  auto t0 = Clock::now();
  fn(work);
  auto t1 = Clock::now();
  if (check_sorted) {
    if (!std::is_sorted(work.begin(), work.end())) {
      std::cerr << "Assertion failed: output not sorted";
      if (algo_name)
        std::cerr << " (algo=" << algo_name << ")";
      std::cerr << ", N=" << work.size() << "\n";
      std::exit(4);
    }
  }
  return std::chrono::duration_cast<ms>(t1 - t0).count();
}

static double median(std::vector<double> v) {
  if (v.empty())
    return 0.0;
  std::nth_element(v.begin(),
                   v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2),
                   v.end());
  if (v.size() % 2 == 1)
    return v[v.size() / 2];
  auto a = *std::max_element(
      v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2));
  auto b = v[v.size() / 2];
  return 0.5 * (a + b);
}

static bool has_ext(const std::string &path, const char *ext1,
                    const char *ext2 = nullptr) {
  auto tolow = [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  };
  std::string p;
  p.reserve(path.size());
  for (unsigned char c : path)
    p.push_back(tolow(c));
  const std::string e1(ext1);
  if (p.size() >= e1.size() && p.substr(p.size() - e1.size()) == e1)
    return true;
  if (ext2) {
    std::string e2(ext2);
    if (p.size() >= e2.size() && p.substr(p.size() - e2.size()) == e2)
      return true;
  }
  return false;
}

static int write_gnuplot_and_run(
    const std::string &out_path, int W, int H, const std::string &title,
    const std::vector<std::tuple<std::string, double, double, double>> &series,
    bool keep_files, PlotStyle style,
    const std::optional<std::string> &artifact_dir_opt) {
  // Use temporary files in the system temp directory
  namespace fs = std::filesystem;
  fs::path dat_path;
  fs::path gp_path;
  if (artifact_dir_opt.has_value()) {
    fs::path outp(out_path);
    fs::path dir(*artifact_dir_opt);
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = outp.stem().string();
    dat_path = dir / (stem + ".dat");
    gp_path = dir / (stem + ".gp");
  } else {
    fs::path tmpdir = fs::temp_directory_path();
    auto now =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string base = std::string("sortbench_") + std::to_string(now);
    dat_path = tmpdir / (base + ".dat");
    gp_path = tmpdir / (base + ".gp");
  }
  std::ofstream df(dat_path);
  if (!df) {
    std::cerr << "Failed to open data file: " << dat_path << "\n";
    return -1;
  }
  df << "# algo\tmedian\tmin\tmax\n";
  for (const auto &t : series) {
    df << std::get<0>(t) << '\t' << std::get<1>(t) << '\t' << std::get<2>(t)
       << '\t' << std::get<3>(t) << "\n";
  }
  df.close();

  std::ofstream gf(gp_path);
  if (!gf) {
    std::cerr << "Failed to open gnuplot file: " << gp_path << "\n";
    return -1;
  }
  std::string term;
  if (has_ext(out_path, ".png"))
    term = "pngcairo";
  else if (has_ext(out_path, ".jpg", ".jpeg"))
    term = "jpeg";
  else
    term = "pngcairo";
  gf << "set terminal " << term << " size " << W << "," << H << "\n";
  gf << "set output '" << out_path << "'\n";
  if (!title.empty())
    gf << "set title '" << title << "'\n";
  gf << "set datafile separator '\t'\n";
  gf << "set xtics rotate by 45 right\n";
  gf << "set grid ytics\n";
  if (style == PlotStyle::boxes) {
    gf << "set style data histogram\n";
    gf << "set style fill solid 1.0 border -1\n";
    gf << "set boxwidth 0.6\n";
    gf << "plot '" << dat_path.string()
       << "' using 2:xtic(1) title 'median' with boxes, "
       << "     '' using 0:2:3:4 with yerrorbars notitle\n";
  } else {
    gf << "plot '" << dat_path.string()
       << "' using 0:2:xtic(1) title 'median' with linespoints, "
       << "     '' using 0:2:3:4 with yerrorbars notitle\n";
  }
  gf.close();

  std::string cmd = std::string("gnuplot '") + gp_path.string() + "'";
  int rc = std::system(cmd.c_str());
  if (rc != 0) {
    std::cerr << "gnuplot failed (rc=" << rc
              << ") — ensure gnuplot is installed. Script: " << gp_path << "\n";
  }
  if (!keep_files) {
    std::error_code ec;
    fs::remove(dat_path, ec);
    fs::remove(gp_path, ec);
  }
  return rc;
}

static bool write_plot_dat_file(
    const std::string &dat_path,
    const std::vector<std::tuple<std::string, double, double, double>>
        &series) {
  std::ofstream df(dat_path);
  if (!df) {
    std::cerr << "Failed to open data file: " << dat_path << "\n";
    return false;
  }
  df << "# algo\tmedian\tmin\tmax\n";
  for (const auto &t : series) {
    df << std::get<0>(t) << '\t' << std::get<1>(t) << '\t' << std::get<2>(t)
       << '\t' << std::get<3>(t) << "\n";
  }
  return true;
}

template <class T> static int run_for_type(const Options &opt) {
  // Discover-only mode
  if (opt.list) {
    sortbench::ElemType et;
    if constexpr (std::is_same_v<T, int>) et = sortbench::ElemType::i32;
    else if constexpr (std::is_same_v<T, unsigned int>) et = sortbench::ElemType::u32;
    else if constexpr (std::is_same_v<T, long long>) et = sortbench::ElemType::i64;
    else if constexpr (std::is_same_v<T, unsigned long long>) et = sortbench::ElemType::u64;
    else if constexpr (std::is_same_v<T, float>) et = sortbench::ElemType::f32;
    else if constexpr (std::is_same_v<T, double>) et = sortbench::ElemType::f64;
    else et = sortbench::ElemType::str;
    auto names = (opt.plugin_paths.empty()
                      ? sortbench::list_algorithms(et)
                      : sortbench::list_algorithms(et, opt.plugin_paths));
    for (const auto &n : names) std::cout << n << "\n";
    return 0;
  }

  sortbench::CoreConfig cfg;
  cfg.N = opt.N;
  cfg.dist = static_cast<sortbench::Dist>(static_cast<int>(opt.dist));
  if constexpr (std::is_same_v<T, int>) cfg.type = sortbench::ElemType::i32;
  else if constexpr (std::is_same_v<T, unsigned int>) cfg.type = sortbench::ElemType::u32;
  else if constexpr (std::is_same_v<T, long long>) cfg.type = sortbench::ElemType::i64;
  else if constexpr (std::is_same_v<T, unsigned long long>) cfg.type = sortbench::ElemType::u64;
  else if constexpr (std::is_same_v<T, float>) cfg.type = sortbench::ElemType::f32;
  else if constexpr (std::is_same_v<T, double>) cfg.type = sortbench::ElemType::f64;
  else cfg.type = sortbench::ElemType::str;
  cfg.repeats = opt.repeats;
  cfg.warmup = opt.warmup;
  cfg.seed = opt.seed;
  cfg.algos = opt.algos;
  cfg.algo_regex = opt.algo_regex;
  cfg.exclude_algos = opt.exclude_algos;
  cfg.exclude_regex = opt.exclude_regex;
  cfg.partial_shuffle_pct = opt.partial_shuffle_pct;
  cfg.dup_values = opt.dup_values;
  cfg.verify = opt.verify;
  cfg.assert_sorted = opt.assert_sorted;
  cfg.threads = opt.threads;
  cfg.plugin_paths = opt.plugin_paths;
  cfg.baseline = opt.baseline;
  cfg.zipf_s = opt.zipf_s;
  cfg.runs_alpha = opt.runs_alpha;
  cfg.stagger_block = opt.stagger_block;

  sortbench::RunResult r;
  try {
    r = sortbench::run_benchmark(cfg);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
  }

  struct Row {
    std::string algo;
    std::size_t N;
    std::string dist;
    double t; // median ms
    double tmin;
    double tmax; // min/max ms
    double tmean;
    double tstd;    // mean/stddev ms
    double speedup; // vs baseline (filled later)
  };
  std::vector<Row> rows;
  rows.reserve(r.rows.size());
  for (const auto &rr : r.rows) {
    rows.push_back(Row{rr.algo, r.N, r.dist, rr.stats.median_ms, rr.stats.min_ms,
                       rr.stats.max_ms, rr.stats.mean_ms, rr.stats.stddev_ms,
                       1.0});
  }

  // Compute baseline speedups and print winner summary
  double baseline_med = 0.0;
  std::string baseline_name;
  if (opt.baseline.has_value()) {
    baseline_name = to_lower(*opt.baseline);
    for (const auto &r : rows) {
      if (to_lower(r.algo) == baseline_name) {
        baseline_med = r.t;
        break;
      }
    }
    if (baseline_med == 0.0 && !rows.empty()) {
      std::cerr << "Baseline not found: '" << *opt.baseline
                << "' — speedups default to 1.0\n";
    }
  }
  for (auto &r : rows)
    r.speedup =
        (baseline_med > 0.0 ? (baseline_med / std::max(1e-12, r.t)) : 1.0);

  // Winner summary per run
  if (!rows.empty()) {
    const Row *best = &rows[0];
    for (const auto &r : rows) {
      if (opt.baseline.has_value()) {
        if (r.speedup > best->speedup)
          best = &r;
      } else {
        if (r.t < best->t)
          best = &r;
      }
    }
    if (opt.baseline.has_value()) {
      std::cerr << "Winner (N=" << opt.N
                << ", dist=" << kDistNames[static_cast<int>(opt.dist)]
                << "): algo=" << best->algo << ", median_ms=" << best->t
                << ", speedup_vs_baseline=" << best->speedup;
      if (baseline_med > 0.0)
        std::cerr << " (baseline '" << *opt.baseline
                  << "' median_ms=" << baseline_med << ")";
      std::cerr << "\n";
    } else {
      std::cerr << "Winner (N=" << opt.N
                << ", dist=" << kDistNames[static_cast<int>(opt.dist)]
                << "): algo=" << best->algo << ", median_ms=" << best->t
                << "\n";
    }
  }

  if (opt.format == OutFmt::csv) {
    std::string csv = sortbench::to_csv(r, opt.csv_header, opt.baseline.has_value());
    std::cout << csv;
    // Write to results file (unless suppressed)
    if (!opt.no_file) {
      namespace fs = std::filesystem;
      fs::path rp = opt.results_path.has_value() ? fs::path(*opt.results_path)
                                                 : fs::path("bench_result.csv");
      std::error_code ec;
      if (rp.has_parent_path())
        fs::create_directories(rp.parent_path(), ec);
      std::ios_base::openmode mode =
          opt.csv_header ? std::ios::out : (std::ios::out | std::ios::app);
      {
        std::ofstream rf(rp, mode);
        if (!rf) {
          std::cerr << "Failed to open results file: " << rp << "\n";
        } else {
          rf << csv;
        }
      }
    }
    // If plotting is requested and format is CSV, also write a CSV file next to
    // plot
    if (opt.plot_path.has_value() && !opt.no_file) {
      namespace fs = std::filesystem;
      fs::path csvp = opt.output_dir.has_value()
                          ? (fs::path(*opt.output_dir) /
                             fs::path(*opt.plot_path).filename())
                          : fs::path(*opt.plot_path);
      if (opt.output_dir.has_value()) {
        std::error_code ec;
        fs::create_directories(fs::path(*opt.output_dir), ec);
      }
      csvp.replace_extension(".csv");
      std::ofstream cf(csvp);
      if (!cf) {
        std::cerr << "Failed to open CSV file: " << csvp << "\n";
      } else {
        cf << sortbench::to_csv(r, true, opt.baseline.has_value());
      }
    }
  } else if (opt.format == OutFmt::json) {
    std::string js = sortbench::to_json(r, opt.baseline.has_value(), true);
    std::cout << js;
    // Write JSON file (overwrite per run)
    if (!opt.no_file) {
      namespace fs = std::filesystem;
      fs::path rp = opt.results_path.has_value()
                        ? fs::path(*opt.results_path)
                        : fs::path("bench_result.json");
      std::error_code ec;
      if (rp.has_parent_path())
        fs::create_directories(rp.parent_path(), ec);
      std::ofstream rf(rp);
      if (!rf) {
        std::cerr << "Failed to open results file: " << rp << "\n";
      } else {
        rf << js;
      }
    }
  } else if (opt.format == OutFmt::jsonl) {
    std::string jsonl = sortbench::to_jsonl(r, opt.baseline.has_value());
    std::cout << jsonl;
    // append to file
    if (!opt.no_file) {
      namespace fs = std::filesystem;
      fs::path rp = opt.results_path.has_value()
                        ? fs::path(*opt.results_path)
                        : fs::path("bench_result.jsonl");
      std::error_code ec;
      if (rp.has_parent_path())
        fs::create_directories(rp.parent_path(), ec);
      std::ofstream rf(rp, std::ios::out | std::ios::app);
      if (!rf) {
        std::cerr << "Failed to open results file: " << rp << "\n";
      } else {
        rf << jsonl;
      }
    }
  } else {
    std::size_t w_algo = std::string("algo").size();
    std::size_t w_N = std::string("N").size();
    std::size_t w_dist = std::string("dist").size();
    std::size_t w_med = std::string("median_ms").size();
    std::size_t w_mean = std::string("mean_ms").size();
    std::size_t w_min = std::string("min_ms").size();
    std::size_t w_max = std::string("max_ms").size();
    std::size_t w_std = std::string("stddev_ms").size();
    std::size_t w_spd = std::string("speedup").size();
    for (const auto &r : rows) {
      w_algo = std::max(w_algo, r.algo.size());
      w_N = std::max(w_N, std::to_string(r.N).size());
      w_dist = std::max(w_dist, r.dist.size());
      auto widen = [](double v) {
        std::ostringstream os;
        os << std::fixed << std::setprecision(3) << v;
        return os.str().size();
      };
      w_med = std::max<std::size_t>(w_med, widen(r.t));
      w_mean = std::max<std::size_t>(w_mean, widen(r.tmean));
      w_min = std::max<std::size_t>(w_min, widen(r.tmin));
      w_max = std::max<std::size_t>(w_max, widen(r.tmax));
      w_std = std::max<std::size_t>(w_std, widen(r.tstd));
      if (opt.baseline.has_value())
        w_spd = std::max<std::size_t>(w_spd, widen(r.speedup));
    }
    auto print_table_to = [&](std::ostream &os) {
      auto print_sep = [&]() {
        os << '+' << std::string(w_algo + 2, '-') << '+'
           << std::string(w_N + 2, '-') << '+' << std::string(w_dist + 2, '-')
           << '+' << std::string(w_med + 2, '-') << '+'
           << std::string(w_mean + 2, '-') << '+' << std::string(w_min + 2, '-')
           << '+' << std::string(w_max + 2, '-') << '+'
           << std::string(w_std + 2, '-');
        if (opt.baseline.has_value())
          os << '+' << std::string(w_spd + 2, '-');
        os << "+\n";
      };
      auto print_row = [&](std::string a, std::string n, std::string d,
                           std::string med, std::string mean, std::string mn,
                           std::string mx, std::string sd, std::string spd) {
        os << "| " << std::left << std::setw(static_cast<int>(w_algo)) << a
           << " | " << std::right << std::setw(static_cast<int>(w_N)) << n
           << " | " << std::left << std::setw(static_cast<int>(w_dist)) << d
           << " | " << std::right << std::setw(static_cast<int>(w_med)) << med
           << " | " << std::right << std::setw(static_cast<int>(w_mean)) << mean
           << " | " << std::right << std::setw(static_cast<int>(w_min)) << mn
           << " | " << std::right << std::setw(static_cast<int>(w_max)) << mx
           << " | " << std::right << std::setw(static_cast<int>(w_std)) << sd;
        if (opt.baseline.has_value())
          os << " | " << std::right << std::setw(static_cast<int>(w_spd))
             << spd;
        os << " |\n";
      };
      if (opt.csv_header) {
        print_sep();
        print_row("algo", "N", "dist", "median_ms", "mean_ms", "min_ms",
                  "max_ms", "stddev_ms",
                  opt.baseline.has_value() ? std::string("speedup")
                                           : std::string(""));
        print_sep();
      }
      auto fmt = [](double v) {
        std::ostringstream os;
        os << std::fixed << std::setprecision(3) << v;
        return os.str();
      };
      for (const auto &r : rows) {
        print_row(r.algo, std::to_string(r.N), r.dist, fmt(r.t), fmt(r.tmean),
                  fmt(r.tmin), fmt(r.tmax), fmt(r.tstd),
                  opt.baseline.has_value() ? fmt(r.speedup) : std::string(""));
      }
      if (opt.csv_header)
        print_sep();
    };
    print_table_to(std::cout);
    // Write table to file (overwrite per run)
    if (!opt.no_file) {
      namespace fs = std::filesystem;
      fs::path rp = opt.results_path.has_value() ? fs::path(*opt.results_path)
                                                 : fs::path("bench_result.txt");
      std::error_code ec;
      if (rp.has_parent_path())
        fs::create_directories(rp.parent_path(), ec);
      std::ofstream rf(rp);
      if (!rf) {
        std::cerr << "Failed to open results file: " << rp << "\n";
      } else {
        print_table_to(rf);
      }
    }
  }

  if (opt.plot_path.has_value()) {
    std::vector<std::tuple<std::string, double, double, double>> series;
    series.reserve(rows.size());
    for (const auto &r : rows)
      series.emplace_back(r.algo, r.t, r.tmin, r.tmax);
    if (opt.multi_plot_accumulate && opt.plot_dat_path.has_value()) {
      write_plot_dat_file(*opt.plot_dat_path, series);
    } else {
      std::string title =
          opt.plot_title.empty()
              ? (std::string("N=") + std::to_string(opt.N) + ", dist=" +
                 std::string(kDistNames[static_cast<int>(opt.dist)]) +
                 ", type=" + std::string(elem_type_name(opt.type)))
              : opt.plot_title;
      write_gnuplot_and_run(*opt.plot_path, opt.plot_w, opt.plot_h, title,
                            series, opt.keep_plot_artifacts, opt.plot_style,
                            opt.output_dir);
    }
  }

  // plugin handles are managed inside the core library
  return 0;
}

int main(int argc, char **argv) {
  try {
    Options opt = parse_args(argc, argv);
    // Print build info or build plugin if requested
#ifdef SORTBENCH_CXX
    const char *built_cxx = SORTBENCH_CXX;
#else
    const char *built_cxx = "g++";
#endif
#ifdef SORTBENCH_CXXFLAGS
    const char *built_cxxflags = SORTBENCH_CXXFLAGS;
#else
    const char *built_cxxflags = "-O3 -std=c++20";
#endif
#ifdef SORTBENCH_LDFLAGS
    const char *built_ldflags = SORTBENCH_LDFLAGS;
#else
    const char *built_ldflags = "";
#endif
    if (opt.print_build) {
      std::cout << "CXX=" << built_cxx << "\n"
                << "CXXFLAGS=" << built_cxxflags << "\n"
                << "LDFLAGS=" << built_ldflags << "\n";
      return 0;
    }
    if (opt.build_plugin_src.has_value()) {
      if (!opt.build_plugin_out.has_value()) {
        throw std::runtime_error("--build-plugin requires --out <lib.so>");
      }
      std::string cmd;
      cmd.reserve(512);
      cmd.append(built_cxx)
          .append(" ")
          .append(built_cxxflags)
          .append(" -fPIC -shared -o ")
          .append(*opt.build_plugin_out)
          .append(" ")
          .append(*opt.build_plugin_src)
          .append(" ")
          .append(built_ldflags);
      int rc = std::system(cmd.c_str());
      if (rc != 0) {
        std::cerr << "Plugin build failed (rc=" << rc << "): " << cmd << "\n";
        return 2;
      }
      std::cout << "Built plugin: " << *opt.build_plugin_out << "\n";
      return 0;
    }
    if (opt.init_plugin_out.has_value()) {
      namespace fs = std::filesystem;
      fs::path outp(*opt.init_plugin_out);
      if (!outp.has_extension())
        outp.replace_extension(".cpp");
      if (!outp.has_parent_path())
        outp = fs::path("plugins") / outp;
      std::error_code ec;
      fs::create_directories(outp.parent_path(), ec);
      if (fs::exists(outp)) {
        std::cerr << "Refusing to overwrite existing file: " << outp << "\n";
        return 2;
      }
      std::ofstream of(outp);
      if (!of) {
        std::cerr << "Failed to open output: " << outp << "\n";
        return 2;
      }
      // If placed under plugins/, use relative include to parent dir.
      std::string include_line;
      if (outp.has_parent_path() && outp.parent_path().filename() == "plugins")
        include_line = "#include \"../sortbench_plugin.h\"\n";
      else
        include_line = "#include \"sortbench_plugin.h\"\n";
      of << "// Generated by sortbench --init-plugin (v2 multi-type scaffold)\n"
         << "#include <algorithm>\n#include <vector>\n"
         << include_line << "\n"
         << "// Provide one or more type-specific entrypoints.\n"
         << "// Return algorithms via sortbench_get_algorithms_v2.\n\n"
         << "static void my_sort_i32(int* data, int n) { std::sort(data, "
            "data+n); }\n"
         << "static void my_sort_u32(unsigned int* data, int n) { "
            "std::sort(data, data+n); }\n"
         << "static void my_sort_i64(long long* data, int n) { std::sort(data, "
            "data+n); }\n"
         << "static void my_sort_u64(unsigned long long* data, int n) { "
            "std::sort(data, data+n); }\n"
         << "static void my_sort_f32(float* data, int n) { std::sort(data, "
            "data+n); }\n"
         << "static void my_sort_f64(double* data, int n) { std::sort(data, "
            "data+n); }\n\n"
         << "static const sortbench_algo_v2 k_algos[] = {\n"
         << "    {\"my_sort\", &my_sort_i32, &my_sort_u32, &my_sort_i64, "
            "&my_sort_u64, &my_sort_f32, &my_sort_f64},\n"
         << "};\n\n"
         << "extern \"C\" int sortbench_get_algorithms_v2(const "
            "sortbench_algo_v2** out_algos, int* out_count) {\n"
         << "    if (!out_algos || !out_count) return 0;\n"
         << "    *out_algos = k_algos;\n"
         << "    *out_count = (int)(sizeof(k_algos)/sizeof(k_algos[0]));\n"
         << "    return 1;\n"
         << "}\n\n"
         << "// Optional: also expose v1 (int-only) for older harnesses.\n"
         << "static const sortbench_algo_v1 k_algos_v1[] = {\n"
         << "    {\"my_sort\", &my_sort_i32},\n"
         << "};\n"
         << "extern \"C\" int sortbench_get_algorithms_v1(const "
            "sortbench_algo_v1** out_algos, int* out_count) {\n"
         << "    if (!out_algos || !out_count) return 0;\n"
         << "    *out_algos = k_algos_v1;\n"
         << "    *out_count = "
            "(int)(sizeof(k_algos_v1)/sizeof(k_algos_v1[0]));\n"
         << "    return 1;\n"
         << "}\n";
      of.close();
      std::cout << "Wrote plugin scaffold: " << outp << "\n"
                << "Build it via: ./sortbench --build-plugin " << outp.string()
                << " --out "
                << (outp.parent_path() /
                    (outp.stem().string() + std::string(".so")))
                       .string()
                << "\n";
      return 0;
    }
    // Sweep over N values and one or more distributions
    std::vector<std::size_t> sweep = opt.Ns;
    if (sweep.empty())
      sweep.push_back(opt.N);
    if (opt.dists.empty())
      opt.dists.push_back(opt.dist);
    bool first = true;
    int rc = 0;
    // Multiplot across distributions (single image)
    bool do_multi_plot = opt.plot_path.has_value() && opt.dists.size() > 1;
    std::vector<std::pair<std::string, std::string>>
        plot_parts; // dist name -> dat path
    for (std::size_t nval : sweep) {
      for (Dist d : opt.dists) {
        Options cur = opt;
        cur.N = nval;
        cur.dist = d;
        if (!first && cur.format == OutFmt::csv)
          cur.csv_header = false;
        if (do_multi_plot) {
          cur.multi_plot_accumulate = true;
          namespace fs = std::filesystem;
          fs::path img(*opt.plot_path);
          fs::path dat =
              opt.output_dir.has_value()
                  ? (fs::path(*opt.output_dir) /
                     (img.stem().string() + std::string(".") +
                      std::string(kDistNames[static_cast<int>(d)]) +
                      std::string(".dat")))
                  : (img.replace_extension(
                        std::string(".") +
                        std::string(kDistNames[static_cast<int>(d)]) +
                        std::string(".dat")));
          if (opt.output_dir.has_value()) {
            std::error_code ec;
            fs::create_directories(fs::path(*opt.output_dir), ec);
          }
          cur.plot_dat_path = dat.string();
        }
        switch (cur.type) {
        case ElemType::i32:
          rc = run_for_type<int>(cur);
          break;
        case ElemType::u32:
          rc = run_for_type<unsigned int>(cur);
          break;
        case ElemType::i64:
          rc = run_for_type<long long>(cur);
          break;
        case ElemType::u64:
          rc = run_for_type<unsigned long long>(cur);
          break;
        case ElemType::f32:
          rc = run_for_type<float>(cur);
          break;
        case ElemType::f64:
          rc = run_for_type<double>(cur);
          break;
        case ElemType::str:
          rc = run_for_type<std::string>(cur);
          break;
        }
        if (rc != 0)
          return rc;
        if (do_multi_plot) {
          namespace fs = std::filesystem;
          fs::path img(*opt.plot_path);
          fs::path dat =
              opt.output_dir.has_value()
                  ? (fs::path(*opt.output_dir) /
                     (img.stem().string() + std::string(".") +
                      std::string(kDistNames[static_cast<int>(d)]) +
                      std::string(".dat")))
                  : (img.replace_extension(
                        std::string(".") +
                        std::string(kDistNames[static_cast<int>(d)]) +
                        std::string(".dat")));
          plot_parts.emplace_back(std::string(kDistNames[static_cast<int>(d)]),
                                  dat.string());
        }
        first = false;
      }
    }
    if (do_multi_plot) {
      namespace fs = std::filesystem;
      fs::path img(*opt.plot_path);
      fs::path gp = opt.output_dir.has_value()
                        ? (fs::path(*opt.output_dir) /
                           (img.stem().string() + std::string(".gp")))
                        : (img.replace_extension(".gp"));
      if (opt.output_dir.has_value()) {
        std::error_code ec;
        fs::create_directories(fs::path(*opt.output_dir), ec);
      }
      std::ofstream gf(gp);
      if (!gf) {
        std::cerr << "Failed to open gnuplot file: " << gp << "\n";
        return 2;
      }
      std::string term;
      if (has_ext(img.string(), ".png"))
        term = "pngcairo";
      else if (has_ext(img.string(), ".jpg", ".jpeg"))
        term = "jpeg";
      else
        term = "pngcairo";
      gf << "set terminal " << term << " size " << opt.plot_w << ","
         << opt.plot_h << "\n";
      gf << "set output '" << img.string() << "'\n";
      if (!opt.plot_title.empty())
        gf << "set title '" << opt.plot_title << "'\n";
      gf << "set datafile separator '\t'\n";
      gf << "set xtics rotate by 45 right\n";
      gf << "set grid ytics\n";
      if (opt.plot_style == PlotStyle::boxes) {
        gf << "set style data histogram\n";
        gf << "set style fill solid 1.0 border -1\n";
        gf << "set boxwidth 0.6\n";
      }
      int rows = (opt.plot_rows > 0 ? opt.plot_rows
                                    : static_cast<int>(plot_parts.size()));
      int cols = (opt.plot_cols > 0 ? opt.plot_cols : 1);
      gf << "set multiplot layout " << rows << "," << cols << "\n";
      for (const auto &pp : plot_parts) {
        gf << "set title '" << pp.first << "'\n";
        gf << "plot '" << pp.second
           << "' using 2:xtic(1) title 'median' with boxes, \\\n";
        gf << "     '' using 0:2:3:4 with yerrorbars notitle\n";
      }
      gf << "unset multiplot\n";
      gf.close();
      std::string cmd = std::string("gnuplot '") + gp.string() + "'";
      int rc2 = std::system(cmd.c_str());
      if (rc2 != 0) {
        std::cerr << "gnuplot failed (rc=" << rc2
                  << ") — ensure gnuplot is installed. Script: " << gp << "\n";
        return rc2;
      }
      if (!opt.keep_plot_artifacts) {
        std::error_code ec;
        for (const auto &pp : plot_parts)
          fs::remove(pp.second, ec);
        fs::remove(gp, ec);
      }
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
