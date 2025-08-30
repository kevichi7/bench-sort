// Minimal core tests for sortbench
#include "sortbench/core.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace sortbench;

static void require(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
  }
}

static bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

static void test_list_algorithms_builtin() {
  auto algos_i32 = list_algorithms(ElemType::i32);
  require(!algos_i32.empty(), "i32 algorithms should not be empty");
  require(contains(algos_i32, std::string("std_sort")), "std_sort present (i32)");

  auto algos_f32 = list_algorithms(ElemType::f32);
  require(!algos_f32.empty(), "f32 algorithms should not be empty");
  require(contains(algos_f32, std::string("std_sort")), "std_sort present (f32)");
}

static void test_run_basic_int() {
  CoreConfig cfg;
  cfg.N = 1000;
  cfg.dist = Dist::random;
  cfg.type = ElemType::i32;
  cfg.repeats = 2;
  cfg.warmup = 0;
  cfg.assert_sorted = true;
  cfg.verify = true; // verify vs std::sort for selected algos
  cfg.algos = {"std_sort", "heap_sort"};

  auto res = run_benchmark(cfg);
  require(res.rows.size() >= 1, "rows >= 1 (i32)");
  bool found = false;
  for (const auto& r : res.rows) {
    if (r.algo == "std_sort") {
      found = true;
      require(r.stats.median_ms >= 0.0, "median non-negative");
    }
  }
  require(found, "std_sort row present");
}

static void test_to_json_csv() {
  CoreConfig cfg;
  cfg.N = 512;
  cfg.dist = Dist::runs;
  cfg.type = ElemType::f32;
  cfg.repeats = 2;
  cfg.algos = {"std_sort"};
  auto res = run_benchmark(cfg);
  auto csv = to_csv(res, true, false);
  require(csv.find("algo,N,dist,median_ms") != std::string::npos, "csv header present");
  auto js = to_json(res, false, true);
  require(js.find("\"algo\"") != std::string::npos, "json has fields");
  auto jl = to_jsonl(res, false);
  require(jl.find("\n") != std::string::npos, "jsonl has newline");
}

static void test_string_type() {
  CoreConfig cfg;
  cfg.N = 256;
  cfg.type = ElemType::str;
  cfg.dist = Dist::sorted;
  cfg.repeats = 1;
  cfg.algos = {"std_sort"};
  auto res = run_benchmark(cfg);
  require(!res.rows.empty(), "string rows non-empty");
}

int main() {
  try {
    test_list_algorithms_builtin();
    test_run_basic_int();
    test_to_json_csv();
    test_string_type();
    // Extra tests
    // Baseline speedup: std_sort baseline equals 1.0; others > 0
    {
      CoreConfig cfg;
      cfg.N = 1500;
      cfg.type = ElemType::i32;
      cfg.dist = Dist::partial;
      cfg.repeats = 2;
      cfg.algos = {"std_sort", "heap_sort"};
      cfg.baseline = std::string("std_sort");
      auto res = run_benchmark(cfg);
      bool saw_std = false, saw_heap = false;
      for (const auto& row : res.rows) {
        if (row.algo == "std_sort") {
          saw_std = true;
          require(std::abs(row.speedup_vs_baseline - 1.0) < 1e-9, "std_sort speedup == 1.0");
        }
        if (row.algo == "heap_sort") {
          saw_heap = true;
          require(row.speedup_vs_baseline > 0.0, "heap_sort speedup > 0");
        }
      }
      require(saw_std, "baseline row present");
      require(saw_heap, "other row present");
      auto js = to_json(res, true, true);
      require(js.find("speedup_vs_baseline") != std::string::npos, "json has speedup field");
    }
    // Algo filter: only nonexistent -> empty rows
    {
      CoreConfig cfg;
      cfg.N = 256;
      cfg.type = ElemType::i32;
      cfg.dist = Dist::random;
      cfg.repeats = 1;
      cfg.algos = {"does_not_exist"};
      auto res = run_benchmark(cfg);
      require(res.rows.empty(), "no rows when filter has no matches");
    }
    // Distributions coverage: runs + partial for f32
    {
      CoreConfig cfg;
      cfg.N = 512;
      cfg.type = ElemType::f32;
      cfg.dist = Dist::runs;
      cfg.repeats = 1;
      cfg.assert_sorted = true;
      cfg.algos = {"std_sort"};
      auto r1 = run_benchmark(cfg);
      require(!r1.rows.empty(), "runs dist rows");
      cfg.dist = Dist::partial;
      auto r2 = run_benchmark(cfg);
      require(!r2.rows.empty(), "partial dist rows");
    }
  } catch (const std::exception& e) {
    std::cerr << "Unhandled exception: " << e.what() << "\n";
    return 2;
  }
  std::cout << "OK\n";
  return 0;
}
