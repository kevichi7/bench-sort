#define main CppTest25orig5_main_disabled
#include "../../CppTest25orig5.cpp"
#include "../sortbench_plugin.h"

extern "C" void custom_hybrid_sort_int(int* data, int n) {
  // Use the auto-dispatching hybrid from your implementation
  hybrid_sort_auto(data, n);
}

extern "C" void custom_vqsort_int(int* data, int n) {
  vqsort_sort(data, n);
}

extern "C" void custom_pdqsort_int(int* data, int n) {
  pdqsort_sort(data, n);
}

static const sortbench_algo_v1 k_algos[] = {
    {"custom", &custom_hybrid_sort_int},
    {"custom_vqsort", &custom_vqsort_int},
    {"custom_pdqsort", &custom_pdqsort_int},
};

extern "C" int sortbench_get_algorithms_v1(const sortbench_algo_v1** out_algos, int* out_count) {
    if (!out_algos || !out_count) return 0;
    *out_algos = k_algos;
    *out_count = (int)(sizeof(k_algos)/sizeof(k_algos[0]));
    return 1;
}
