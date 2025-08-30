// sortbench core library (phase 1)
// Extracts the non-CLI core to run a single benchmark in-process

#include "sortbench/core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#if defined(__has_include)
#if __has_include(<tbb/global_control.h>)
#include <tbb/global_control.h>
#define SB_HAS_TBB 1
#else
#define SB_HAS_TBB 0
#endif
#else
#define SB_HAS_TBB 0
#endif

#if defined(__has_include)
#if __has_include("pdqsort.h")
#include "pdqsort.h"
#define SB_HAS_PDQ 1
#elif __has_include("pdqsort.hpp")
#include "pdqsort.hpp"
#define SB_HAS_PDQ 1
#else
#define SB_HAS_PDQ 0
#endif
#else
#define SB_HAS_PDQ 0
#endif

// Optional custom algos: only enable if shim is compiled in (provides symbols)
#if defined(SORTBENCH_HAS_CUSTOM_SHIM)
  #if defined(__has_include)
    #if __has_include("../custom_algo.hpp")
      #include "../custom_algo.hpp"
      #define SB_HAS_CUSTOM 1
    #elif __has_include("custom_algo.hpp")
      #include "custom_algo.hpp"
      #define SB_HAS_CUSTOM 1
    #else
      #define SB_HAS_CUSTOM 0
    #endif
  #else
    #define SB_HAS_CUSTOM 0
  #endif
#else
  #define SB_HAS_CUSTOM 0
#endif

// Optional parallel headers
#if defined(__has_include)
#if __has_include(<execution>)
#include <execution>
#define SB_HAS_STD_PAR 1
#else
#define SB_HAS_STD_PAR 0
#endif
#if __has_include(<parallel/algorithm>)
#include <parallel/algorithm>
#define SB_HAS_GNU_PAR 1
#else
#define SB_HAS_GNU_PAR 0
#endif
#else
#define SB_HAS_STD_PAR 0
#define SB_HAS_GNU_PAR 0
#endif

#include <dlfcn.h>
#include "../sortbench_plugin.h"

namespace sortbench {

using Clock = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

static constexpr std::array<std::string_view, 10> kDistNames{
    "random", "partial", "dups",  "reverse", "sorted",
    "saw",    "runs",    "gauss", "exp",     "zipf"};

std::string_view dist_name(Dist d) {
  int i = static_cast<int>(d);
  if (i < 0 || i >= static_cast<int>(kDistNames.size()))
    return "random";
  return kDistNames[static_cast<std::size_t>(i)];
}

const std::vector<std::string_view> &all_dist_names() {
  static const std::vector<std::string_view> v(kDistNames.begin(),
                                               kDistNames.end());
  return v;
}

std::string_view elem_type_name(ElemType t) {
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

static inline std::string to_lower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static inline std::uint64_t default_seed() { return 0x9E3779B97F4A7C15ULL; }

// Data generation (subset matching CLI behavior)
template <class T>
static std::vector<T> make_data(std::size_t n, Dist dist, std::mt19937_64 &rng,
                                int partial_pct, int dups_k) {
  std::vector<T> v;
  v.resize(n);
  if constexpr (std::is_same_v<T, std::string>) {
    // Strings: generate random words; support sorted/reverse by ordering
    std::uniform_int_distribution<int> len(1, 16);
    std::uniform_int_distribution<int> ch('a', 'z');
    for (std::size_t i = 0; i < n; ++i) {
      int L = len(rng);
      std::string s;
      s.resize(static_cast<std::size_t>(L));
      for (int j = 0; j < L; ++j)
        s[static_cast<std::size_t>(j)] = static_cast<char>(ch(rng));
      v[i] = std::move(s);
    }
    if (dist == Dist::reverse) {
      std::sort(v.begin(), v.end());
      std::reverse(v.begin(), v.end());
    } else if (dist == Dist::sorted) {
      std::sort(v.begin(), v.end());
    }
    return v;
  } else {

    if (dist == Dist::reverse) {
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(n - 1 - i);
      return v;
    }

    if (dist == Dist::sorted) {
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(i);
      return v;
    }

    if (dist == Dist::dups) {
      int k = std::max(1, dups_k);
      std::uniform_int_distribution<int> d(0, k - 1);
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(d(rng));
      return v;
    }

    if (dist == Dist::saw) {
      std::size_t period =
          std::max<std::size_t>(std::min<std::size_t>(n ? n : 1, 1024), 1);
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(i % period);
      return v;
    }

    if (dist == Dist::runs) {
      const std::size_t run_len =
          std::max<std::size_t>(1, std::min<std::size_t>(n ? n : 1, 2048));
      if constexpr (std::is_integral_v<T>) {
        std::uniform_int_distribution<std::make_unsigned_t<T>> d(
            0, std::numeric_limits<std::make_unsigned_t<T>>::max());
        for (std::size_t i = 0; i < n; ++i)
          v[i] = static_cast<T>(d(rng));
      } else if constexpr (std::is_floating_point_v<T>) {
        std::uniform_real_distribution<T> d(T(0), T(1));
        for (std::size_t i = 0; i < n; ++i)
          v[i] = d(rng);
      }
      for (std::size_t i = 0; i < n; i += run_len) {
        std::size_t r = std::min(run_len, n - i);
        std::sort(v.begin() + static_cast<std::ptrdiff_t>(i),
                  v.begin() + static_cast<std::ptrdiff_t>(i + r));
      }
      return v;
    }

    if (dist == Dist::gauss) {
      if constexpr (std::is_integral_v<T>) {
        using Lim = std::numeric_limits<T>;
        std::normal_distribution<double> nd(0.0, 1.0);
        const double minv = static_cast<double>(Lim::min());
        const double maxv = static_cast<double>(Lim::max());
        const double mean = std::is_signed_v<T> ? 0.0 : (maxv / 2.0);
        const double stddev = (maxv - (std::is_signed_v<T> ? minv : 0.0)) / 8.0;
        for (std::size_t i = 0; i < n; ++i) {
          double x = mean + stddev * nd(rng);
          x = std::clamp(x, minv, maxv);
          v[i] = static_cast<T>(x);
        }
      } else {
        std::normal_distribution<T> nd(T(0), T(1));
        for (std::size_t i = 0; i < n; ++i)
          v[i] = nd(rng);
      }
      return v;
    }

    if (dist == Dist::exp) {
      std::exponential_distribution<double> ed(1.0);
      if constexpr (std::is_integral_v<T>) {
        using Lim = std::numeric_limits<T>;
        const double maxv = static_cast<double>(Lim::max());
        for (std::size_t i = 0; i < n; ++i) {
          double y = (maxv / 8.0) * ed(rng);
          if (y > maxv)
            y = maxv;
          v[i] = static_cast<T>(y);
        }
      } else {
        for (std::size_t i = 0; i < n; ++i)
          v[i] = static_cast<T>(ed(rng));
      }
      return v;
    }

    // random or partial
    if constexpr (std::is_integral_v<T>) {
      std::uniform_int_distribution<std::make_unsigned_t<T>> d(
          0, std::numeric_limits<std::make_unsigned_t<T>>::max());
      for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<T>(d(rng));
    } else if constexpr (std::is_floating_point_v<T>) {
      std::uniform_real_distribution<T> d(T(0), T(1));
      for (std::size_t i = 0; i < n; ++i)
        v[i] = d(rng);
    }
    if (dist == Dist::partial) {
      std::size_t to_shuffle =
          (n * static_cast<std::size_t>(std::clamp(partial_pct, 0, 100))) / 100;
      if (to_shuffle > n)
        to_shuffle = n;
      std::uniform_int_distribution<std::size_t> d(0, n ? n - 1 : 0);
      for (std::size_t i = 0; i < to_shuffle; ++i) {
        std::size_t a = d(rng), b = d(rng);
        std::swap(v[a], v[b]);
      }
    }
    return v;
  }
}

// Algorithms
namespace algos {

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

template <class T> inline void heap_sort(std::vector<T> &v) {
  std::make_heap(v.begin(), v.end());
  std::sort_heap(v.begin(), v.end());
}

template <class T> inline void merge_sort_opt(std::vector<T> &v) {
  const std::size_t n = v.size();
  if (n < 2)
    return;
  std::vector<T> buf(n);
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
      if (a < mid) {
        std::copy(v.begin() + static_cast<std::ptrdiff_t>(a),
                  v.begin() + static_cast<std::ptrdiff_t>(mid),
                  buf.begin() + static_cast<std::ptrdiff_t>(k));
        k += (mid - a);
      }
      if (b < right) {
        std::copy(v.begin() + static_cast<std::ptrdiff_t>(b),
                  v.begin() + static_cast<std::ptrdiff_t>(right),
                  buf.begin() + static_cast<std::ptrdiff_t>(k));
        k += (right - b);
      }
      std::copy(buf.begin() + static_cast<std::ptrdiff_t>(left),
                buf.begin() + static_cast<std::ptrdiff_t>(right),
                v.begin() + static_cast<std::ptrdiff_t>(left));
    }
  }
}

template <class Iter> inline void quicksort_hybrid_impl(Iter first, Iter last) {
  constexpr int INSERTION_THRESHOLD = 64;
  while (last - first > INSERTION_THRESHOLD) {
    Iter a = first, b = last - 1, m = first + (last - first) / 2;
    if (*m < *a)
      std::iter_swap(m, a);
    if (*b < *m)
      std::iter_swap(b, m);
    if (*m < *a)
      std::iter_swap(m, a);
    auto pivot = *m;
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

// TimSort (simplified) — copy of CLI version core pieces
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
      v[j] = v[j - 1];
    v[left] = x;
  }
}

template <class T>
static inline void merge_runs(std::vector<T> &v, std::vector<T> &tmp,
                              std::size_t lo, std::size_t mid, std::size_t hi) {
  std::size_t i = lo, j = mid, k = lo;
  while (i < mid && j < hi)
    tmp[k++] = (v[i] <= v[j] ? v[i++] : v[j++]);
  if (i < mid) {
    std::copy(v.begin() + static_cast<std::ptrdiff_t>(i),
              v.begin() + static_cast<std::ptrdiff_t>(mid),
              tmp.begin() + static_cast<std::ptrdiff_t>(k));
    k += (mid - i);
  }
  if (j < hi) {
    std::copy(v.begin() + static_cast<std::ptrdiff_t>(j),
              v.begin() + static_cast<std::ptrdiff_t>(hi),
              tmp.begin() + static_cast<std::ptrdiff_t>(k));
    k += (hi - j);
  }
  std::copy(tmp.begin() + static_cast<std::ptrdiff_t>(lo),
            tmp.begin() + static_cast<std::ptrdiff_t>(hi),
            v.begin() + static_cast<std::ptrdiff_t>(lo));
}

template <class T> inline void timsort(std::vector<T> &v) {
  const std::size_t n = v.size();
  if (n < 2)
    return;
  std::vector<T> tmp(n);
  auto next_run = [&](std::size_t i) {
    std::size_t j = i + 1;
    if (j >= n)
      return n;
    if (v[j] < v[i]) {
      while (j < n && v[j] < v[j - 1])
        ++j;
      std::reverse(v.begin() + static_cast<std::ptrdiff_t>(i),
                   v.begin() + static_cast<std::ptrdiff_t>(j));
    } else {
      while (j < n && !(v[j] < v[j - 1]))
        ++j;
    }
    return j;
  };
  const std::size_t MINRUN = 32;
  std::vector<std::pair<std::size_t, std::size_t>> runs;
  for (std::size_t i = 0; i < n;) {
    std::size_t j = next_run(i);
    if (j - i < MINRUN) {
      std::size_t hi = std::min(n, i + MINRUN);
      binary_insertion_sort(v, i, hi);
      j = hi;
    }
    runs.emplace_back(i, j);
    i = j;
  }
  while (runs.size() > 1) {
    std::vector<std::pair<std::size_t, std::size_t>> nr;
    for (std::size_t i = 0; i + 1 < runs.size(); i += 2) {
      auto [a, b] = runs[i];
      auto [c, d] = runs[i + 1];
      merge_runs(v, tmp, a, b, d);
      nr.emplace_back(a, d);
    }
    if (runs.size() % 2 == 1)
      nr.push_back(runs.back());
    runs.swap(nr);
  }
}

template <class T> inline void radix_sort_lsd(std::vector<T> &v) {
  static_assert(std::is_integral_v<T>, "radix_sort_lsd expects integral type");
  using U = std::make_unsigned_t<T>;
  const std::size_t n = v.size();
  std::vector<T> tmp(n);
  constexpr int B = 8;
  constexpr int K = (int)(sizeof(U) * 8 / B);
  std::array<std::size_t, 256> cnt{};
  auto pass = [&](int shift, bool signed_fix) {
    cnt.fill(0);
    for (std::size_t i = 0; i < n; ++i) {
      U x = (U)v[i];
      if (signed_fix)
        x ^= (U(1) << (sizeof(U) * 8 - 1));
      unsigned idx = (unsigned)((x >> shift) & 0xFFu);
      ++cnt[idx];
    }
    std::array<std::size_t, 256> pos{};
    std::size_t run = 0;
    for (int i = 0; i < 256; ++i)
      pos[(unsigned)i] = std::exchange(run, run + cnt[(unsigned)i]);
    for (std::size_t i = 0; i < n; ++i) {
      U x = (U)v[i];
      if (signed_fix)
        x ^= (U(1) << (sizeof(U) * 8 - 1));
      unsigned idx = (unsigned)((x >> shift) & 0xFFu);
      tmp[pos[idx]++] = v[i];
    }
    std::copy(tmp.begin(), tmp.end(), v.begin());
  };
  for (int pass_i = 0; pass_i < K; ++pass_i)
    pass(pass_i * B, std::is_signed_v<T>);
}

} // namespace algos

// Registry
template <class T> struct AlgoT {
  std::string name;
  std::function<void(std::vector<T> &)> run;
};

template <class T> static std::vector<AlgoT<T>> build_registry_t() {
  std::vector<AlgoT<T>> regs;
  regs.push_back({"std_sort", [](auto &v) { std::sort(v.begin(), v.end()); }});
  regs.push_back({"std_stable_sort",
                  [](auto &v) { std::stable_sort(v.begin(), v.end()); }});
#if SB_HAS_STD_PAR
  regs.push_back({"std_sort_par", [](auto &v) {
                    std::sort(std::execution::par, v.begin(), v.end());
                  }});
  regs.push_back({"std_sort_par_unseq", [](auto &v) {
                    std::sort(std::execution::par_unseq, v.begin(), v.end());
                  }});
#endif
#if SB_HAS_GNU_PAR
  regs.push_back({"gnu_parallel_sort",
                  [](auto &v) { __gnu_parallel::sort(v.begin(), v.end()); }});
#endif
  regs.push_back({"heap_sort", [](auto &v) { algos::heap_sort(v); }});
  regs.push_back({"merge_sort_opt", [](auto &v) { algos::merge_sort_opt(v); }});
  regs.push_back({"timsort", [](auto &v) { algos::timsort(v); }});
  regs.push_back(
      {"quicksort_hybrid", [](auto &v) { algos::quicksort_hybrid(v); }});
  if constexpr (std::is_integral_v<T>) {
    regs.push_back(
        {"radix_sort_lsd", [](auto &v) { algos::radix_sort_lsd(v); }});
  }
#if SB_HAS_PDQ
  regs.push_back({"pdqsort", [](auto &v) { pdqsort(v.begin(), v.end()); }});
#endif
  // Custom algorithms (if header available)
#if SB_HAS_CUSTOM
  if constexpr (std::is_same_v<T, int>) {
    regs.push_back({"custom", [](auto &v) { custom_algo::sort_int(v); }});
    regs.push_back({"customv2", [](auto &v) { custom_algo::sort_int_v2(v); }});
  } else if constexpr (std::is_same_v<T, float>) {
    regs.push_back({"custom", [](auto &v) { custom_algo::sort_float(v); }});
    regs.push_back({"customv2", [](auto &v) { custom_algo::sort_float_v2(v); }});
  } else {
    // Provide safe fallbacks with same names for other types
    regs.push_back({"custom", [](auto &v) { std::sort(v.begin(), v.end()); }});
    regs.push_back({"customv2", [](auto &v) { std::sort(v.begin(), v.end()); }});
  }
#endif
  return regs;
}

using PluginHandle = void*;
using get_algos_v1_fn = int (*)(const sortbench_algo_v1 **, int *);
using get_algos_v2_fn = int (*)(const sortbench_algo_v2 **, int *);

template <class T>
static void load_plugins_t(const std::vector<std::string> &paths,
                           std::vector<AlgoT<T>> &regs,
                           std::vector<PluginHandle> &handles) {
  for (const auto &p : paths) {
    void *h = dlopen(p.c_str(), RTLD_NOW);
    if (!h) {
      continue;
    }
    dlerror();
    if (auto fn2 = reinterpret_cast<get_algos_v2_fn>(
            dlsym(h, "sortbench_get_algorithms_v2"));
        fn2 && !dlerror()) {
      const sortbench_algo_v2 *arr = nullptr;
      int count = 0;
      int ok = fn2(&arr, &count);
      if (!ok || !arr || count <= 0) { dlclose(h); continue; }
      bool any_added = false;
      for (int i = 0; i < count; ++i) {
        const auto &a = arr[i];
        if (!a.name) continue;
        std::string nm = a.name;
        if constexpr (std::is_same_v<T, int>) {
          if (!a.run_i32) continue; auto run = a.run_i32;
          regs.push_back({nm, [run](std::vector<int> &v){ if(!v.empty()) run(v.data(), (int)v.size()); }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, unsigned int>) {
          if (!a.run_u32) continue; auto run = a.run_u32;
          regs.push_back({nm, [run](std::vector<unsigned int> &v){ if(!v.empty()) run(v.data(), (int)v.size()); }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, long long>) {
          if (!a.run_i64) continue; auto run = a.run_i64;
          regs.push_back({nm, [run](std::vector<long long> &v){ if(!v.empty()) run(v.data(), (int)v.size()); }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, unsigned long long>) {
          if (!a.run_u64) continue; auto run = a.run_u64;
          regs.push_back({nm, [run](std::vector<unsigned long long> &v){ if(!v.empty()) run(v.data(), (int)v.size()); }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, float>) {
          if (!a.run_f32) continue; auto run = a.run_f32;
          regs.push_back({nm, [run](std::vector<float> &v){ if(!v.empty()) run(v.data(), (int)v.size()); }});
          any_added = true;
        } else if constexpr (std::is_same_v<T, double>) {
          if (!a.run_f64) continue; auto run = a.run_f64;
          regs.push_back({nm, [run](std::vector<double> &v){ if(!v.empty()) run(v.data(), (int)v.size()); }});
          any_added = true;
        }
      }
      if (any_added) { handles.push_back(h); continue; }
      dlclose(h);
      continue;
    }
    // Fallback to v1 (int only)
    auto fn1 = reinterpret_cast<get_algos_v1_fn>(
        dlsym(h, "sortbench_get_algorithms_v1"));
    if (!fn1 || dlerror()) { dlclose(h); continue; }
    const sortbench_algo_v1 *arr = nullptr; int count = 0; int ok = fn1(&arr, &count);
    if (!ok || !arr || count <= 0) { dlclose(h); continue; }
    bool any_added = false;
    if constexpr (std::is_same_v<T, int>) {
      for (int i = 0; i < count; ++i) {
        const auto &a = arr[i];
        if (!a.name || !a.run_int) continue; std::string nm = a.name;
        regs.push_back({nm, [run=a.run_int](std::vector<int> &v){ if(!v.empty()) run(v.data(), (int)v.size()); }});
        any_added = true;
      }
    }
    if (any_added) handles.push_back(h); else dlclose(h);
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
  for (const auto &re : selected_re)
    if (std::regex_search(name, re) || std::regex_search(ln, re))
      return true;
  return false;
}

template <class T>
static double benchmark_once_t(const std::function<void(std::vector<T> &)> &fn,
                               const std::vector<T> &original,
                               std::vector<T> &work, bool check_sorted,
                               const char *algo_name = nullptr) {
  work.resize(original.size());
  std::copy(original.begin(), original.end(), work.begin());
  auto t0 = Clock::now();
  fn(work);
  auto t1 = Clock::now();
  if (check_sorted) {
    if (!std::is_sorted(work.begin(), work.end())) {
      std::string msg = "Assertion failed: output not sorted";
      if (algo_name) {
        msg += " (algo=";
        msg += algo_name;
        msg += ")";
      }
      throw std::runtime_error(msg);
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

template <class T> static RunResult run_for_type_core(const CoreConfig &cfg) {
  // thread limits
  if (cfg.threads > 0) {
#ifdef _OPENMP
    omp_set_num_threads(cfg.threads);
#endif
  }
#if SB_HAS_TBB
  std::unique_ptr<tbb::global_control> tbb_ctrl;
  if (cfg.threads > 0) {
    tbb_ctrl = std::make_unique<tbb::global_control>(
        tbb::global_control::max_allowed_parallelism,
        static_cast<std::size_t>(cfg.threads));
  }
#endif

  auto regs = build_registry_t<T>();
  std::vector<PluginHandle> plugin_handles;
  if (!cfg.plugin_paths.empty())
    load_plugins_t<T>(cfg.plugin_paths, regs, plugin_handles);
  // Phase 1: no plugin loading here (will be added later)

  std::mt19937_64 rng(cfg.seed.value_or(default_seed()));
  std::vector<T> original = make_data<T>(
      cfg.N, cfg.dist, rng, cfg.partial_shuffle_pct, cfg.dup_values);
  std::vector<T> work;

  if (cfg.verify) {
    auto ref = original;
    std::sort(ref.begin(), ref.end());
    for (const auto &algo : regs) {
      if (!name_selected(cfg.algos, cfg.algo_regex, algo.name))
        continue;
      work = original;
      algo.run(work);
      if (!std::is_sorted(work.begin(), work.end()))
        throw std::runtime_error(
            std::string("Verification failed (not sorted): ") + algo.name);
      if (work != ref)
        throw std::runtime_error(
            std::string("Verification mismatch vs std::sort: ") + algo.name);
    }
  }

  struct RowTmp {
    std::string algo;
    double med;
    double tmin;
    double tmax;
    double mean;
    double sdev;
  };
  std::vector<RowTmp> tmp;

  for (const auto &algo : regs) {
    if (!name_selected(cfg.algos, cfg.algo_regex, algo.name))
      continue;
    for (int w = 0; w < cfg.warmup; ++w) {
      (void)benchmark_once_t<T>(algo.run, original, work, cfg.assert_sorted,
                                algo.name.c_str());
    }
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(std::max(1, cfg.repeats)));
    for (int rep = 0; rep < std::max(1, cfg.repeats); ++rep) {
      times.push_back(benchmark_once_t<T>(
          algo.run, original, work, cfg.assert_sorted, algo.name.c_str()));
    }
    double med = median(times);
    auto mm = std::minmax_element(times.begin(), times.end());
    double tmin = (mm.first != times.end() ? *mm.first : med);
    double tmax = (mm.second != times.end() ? *mm.second : med);
    double sum = 0.0;
    for (double x : times)
      sum += x;
    double mean =
        (times.empty() ? med : sum / static_cast<double>(times.size()));
    double var = 0.0;
    if (times.size() >= 2) {
      for (double x : times) {
        double d = x - mean;
        var += d * d;
      }
      var /= static_cast<double>(times.size());
    }
    double sdev = (times.size() >= 2 ? std::sqrt(var) : 0.0);
    tmp.push_back(RowTmp{algo.name, med, tmin, tmax, mean, sdev});
  }

  // compute baseline speedup
  double baseline_med = 0.0;
  std::string baseline_name;
  if (cfg.baseline.has_value()) {
    baseline_name = to_lower(*cfg.baseline);
    for (const auto &r : tmp) {
      if (to_lower(r.algo) == baseline_name) {
        baseline_med = r.med;
        break;
      }
    }
  }

  RunResult out;
  out.type = cfg.type;
  out.N = cfg.N;
  out.dist = std::string(dist_name(cfg.dist));
  out.repeats = std::max(1, cfg.repeats);
  out.seed = cfg.seed;
  out.baseline = cfg.baseline;
  out.rows.reserve(tmp.size());
  for (const auto &r : tmp) {
    ResultRow rr;
    rr.algo = r.algo;
    rr.N = cfg.N;
    rr.dist = out.dist;
    rr.stats = TimingStats{r.med, r.mean, r.tmin, r.tmax, r.sdev};
    rr.speedup_vs_baseline =
        (baseline_med > 0.0 ? (baseline_med / std::max(1e-12, r.med)) : 1.0);
    out.rows.push_back(std::move(rr));
  }

  // close plugin handles before return
  for (void* h : plugin_handles) { if (h) dlclose(h); }
  return out;
}

RunResult run_benchmark(const CoreConfig &cfg) {
  switch (cfg.type) {
  case ElemType::i32:
    return run_for_type_core<int>(cfg);
  case ElemType::u32:
    return run_for_type_core<unsigned int>(cfg);
  case ElemType::i64:
    return run_for_type_core<long long>(cfg);
  case ElemType::u64:
    return run_for_type_core<unsigned long long>(cfg);
  case ElemType::f32:
    return run_for_type_core<float>(cfg);
  case ElemType::f64:
    return run_for_type_core<double>(cfg);
  case ElemType::str:
    return run_for_type_core<std::string>(cfg);
  }
  throw std::runtime_error("invalid element type");
}

std::vector<std::string> list_algorithms(ElemType t) {
  switch (t) {
  case ElemType::i32: {
    std::vector<std::string> v;
    for (auto &a : build_registry_t<int>())
      v.push_back(a.name);
    return v;
  }
  case ElemType::u32: {
    std::vector<std::string> v;
    for (auto &a : build_registry_t<unsigned int>())
      v.push_back(a.name);
    return v;
  }
  case ElemType::i64: {
    std::vector<std::string> v;
    for (auto &a : build_registry_t<long long>())
      v.push_back(a.name);
    return v;
  }
  case ElemType::u64: {
    std::vector<std::string> v;
    for (auto &a : build_registry_t<unsigned long long>())
      v.push_back(a.name);
    return v;
  }
  case ElemType::f32: {
    std::vector<std::string> v;
    for (auto &a : build_registry_t<float>())
      v.push_back(a.name);
    return v;
  }
  case ElemType::f64: {
    std::vector<std::string> v;
    for (auto &a : build_registry_t<double>())
      v.push_back(a.name);
    return v;
  }
  case ElemType::str: {
    std::vector<std::string> v;
    for (auto &a : build_registry_t<std::string>())
      v.push_back(a.name);
    return v;
  }
  }
  return {};
}

std::vector<std::string> list_algorithms(
    ElemType t, const std::vector<std::string>& plugin_paths) {
  std::vector<std::string> out;
  std::vector<PluginHandle> handles;
  switch (t) {
  case ElemType::i32: {
    auto regs = build_registry_t<int>();
    if (!plugin_paths.empty())
      load_plugins_t<int>(plugin_paths, regs, handles);
    for (auto &a : regs)
      out.push_back(a.name);
    break;
  }
  case ElemType::u32: {
    auto regs = build_registry_t<unsigned int>();
    if (!plugin_paths.empty())
      load_plugins_t<unsigned int>(plugin_paths, regs, handles);
    for (auto &a : regs)
      out.push_back(a.name);
    break;
  }
  case ElemType::i64: {
    auto regs = build_registry_t<long long>();
    if (!plugin_paths.empty())
      load_plugins_t<long long>(plugin_paths, regs, handles);
    for (auto &a : regs)
      out.push_back(a.name);
    break;
  }
  case ElemType::u64: {
    auto regs = build_registry_t<unsigned long long>();
    if (!plugin_paths.empty())
      load_plugins_t<unsigned long long>(plugin_paths, regs, handles);
    for (auto &a : regs)
      out.push_back(a.name);
    break;
  }
  case ElemType::f32: {
    auto regs = build_registry_t<float>();
    if (!plugin_paths.empty())
      load_plugins_t<float>(plugin_paths, regs, handles);
    for (auto &a : regs)
      out.push_back(a.name);
    break;
  }
  case ElemType::f64: {
    auto regs = build_registry_t<double>();
    if (!plugin_paths.empty())
      load_plugins_t<double>(plugin_paths, regs, handles);
    for (auto &a : regs)
      out.push_back(a.name);
    break;
  }
  case ElemType::str: {
    auto regs = build_registry_t<std::string>();
    // No plugin support for string
    for (auto &a : regs)
      out.push_back(a.name);
    break;
  }
  }
  for (void *h : handles)
    if (h)
      dlclose(h);
  return out;
}

std::vector<ElemType> supported_types() {
  return {ElemType::i32, ElemType::u32, ElemType::i64, ElemType::u64,
          ElemType::f32, ElemType::f64, ElemType::str};
}

} // namespace sortbench
