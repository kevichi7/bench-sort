// Dual-Pivot QuickSort variant as a sortbench v2 plugin
// Provides implementations for i32/u32/i64/u64/f32/f64.
// Name: "minmax_quicksort"

#include "../sortbench_plugin.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace {

// Insertion sort for small ranges
template <class T>
inline void insertion_sort(T* a, int n) {
  for (int i = 1; i < n; ++i) {
    T key = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > key) {
      a[j + 1] = a[j];
      --j;
    }
    a[j + 1] = key;
  }
}

// Dual-pivot quicksort (Yaroslavskiy-style), small tweaks
// Partitions into < p, between [p..q], and > q
template <class T>
inline void dual_pivot_qs(T* a, int left, int right) {
  while (left < right) {
    const int n = right - left + 1;
    if (n <= 24) { // insertion sort cutoff
      insertion_sort(a + left, n);
      return;
    }

    // Choose pivots: first and last; ensure p <= q
    T p = a[left];
    T q = a[right];
    if (p > q) { std::swap(p, q); std::swap(a[left], a[right]); }

    int lt = left + 1;   // next position for < p
    int gt = right - 1;  // next position for > q
    int i  = lt;         // current

    while (i <= gt) {
      if (a[i] < p) {
        std::swap(a[i], a[lt]);
        ++lt;
        ++i;
      } else if (a[i] > q) {
        std::swap(a[i], a[gt]);
        --gt;
      } else {
        ++i;
      }
    }

    // Place pivots into final positions
    --lt; ++gt;
    std::swap(a[left], a[lt]);
    std::swap(a[right], a[gt]);

    // Recurse on three partitions; tail-recurse on the largest to limit depth
    const int left_len   = lt - left;
    const int middle_len = gt - lt - 1;
    const int right_len  = right - gt;

    // Order recursive calls from smallest to largest; convert the largest into tail recursion
    if (left_len < middle_len) {
      if (left_len < right_len) {
        // left smallest
        dual_pivot_qs(a, left, lt - 1);
        if (middle_len < right_len) {
          dual_pivot_qs(a, lt + 1, gt - 1);
          left = gt + 1; // tail on right
        } else {
          dual_pivot_qs(a, gt + 1, right);
          left = lt + 1; right = gt - 1; // tail on middle
        }
      } else {
        // right smallest
        dual_pivot_qs(a, gt + 1, right);
        if (left_len < middle_len) {
          dual_pivot_qs(a, left, lt - 1);
          left = lt + 1; right = gt - 1; // tail on middle
        } else {
          dual_pivot_qs(a, lt + 1, gt - 1);
          right = lt - 1; // tail on left
        }
      }
    } else {
      if (middle_len < right_len) {
        // middle smallest
        dual_pivot_qs(a, lt + 1, gt - 1);
        if (left_len < right_len) {
          dual_pivot_qs(a, left, lt - 1);
          left = gt + 1; // tail on right
        } else {
          dual_pivot_qs(a, gt + 1, right);
          right = lt - 1; // tail on left
        }
      } else {
        // right smallest
        dual_pivot_qs(a, gt + 1, right);
        if (left_len < middle_len) {
          dual_pivot_qs(a, left, lt - 1);
          left = lt + 1; right = gt - 1; // tail on middle
        } else {
          dual_pivot_qs(a, lt + 1, gt - 1);
          right = lt - 1; // tail on left
        }
      }
    }
  }
}

template <class T>
inline void minmax_quicksort(T* data, int n) {
  if (n <= 1) return;
  dual_pivot_qs(data, 0, n - 1);
}

// Wrappers for each supported type
static void run_i32(int* data, int n) { minmax_quicksort<int>(data, n); }
static void run_u32(unsigned int* data, int n) { minmax_quicksort<unsigned int>(data, n); }
static void run_i64(long long* data, int n) { minmax_quicksort<long long>(data, n); }
static void run_u64(unsigned long long* data, int n) { minmax_quicksort<unsigned long long>(data, n); }
static void run_f32(float* data, int n) { minmax_quicksort<float>(data, n); }
static void run_f64(double* data, int n) { minmax_quicksort<double>(data, n); }

static const sortbench_algo_v2 ALGOS[] = {
  {"dualpivot_quicksort", &run_i32, &run_u32, &run_i64, &run_u64, &run_f32, &run_f64},
};

} // namespace

extern "C" int sortbench_get_algorithms_v2(const sortbench_algo_v2** out_algos, int* out_count) {
  if (!out_algos || !out_count) return 0;
  *out_algos = ALGOS;
  *out_count = (int)(sizeof(ALGOS) / sizeof(ALGOS[0]));
  return 1;
}
