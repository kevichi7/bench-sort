#include <algorithm>
#include <vector>
#include "../sortbench_plugin.h"

static void plugin_std_sort(int* data, int n) {
    std::sort(data, data + n);
}

static void plugin_heap_sort(int* data, int n) {
    std::vector<int> v(data, data + n);
    std::make_heap(v.begin(), v.end());
    std::sort_heap(v.begin(), v.end());
    std::copy(v.begin(), v.end(), data);
}

static const sortbench_algo_v1 k_algos[] = {
    {"plugin_std_sort", &plugin_std_sort},
    {"plugin_heap_sort", &plugin_heap_sort},
};

extern "C" int sortbench_get_algorithms_v1(const sortbench_algo_v1** out_algos, int* out_count) {
    if (!out_algos || !out_count) return 0;
    *out_algos = k_algos;
    *out_count = (int)(sizeof(k_algos) / sizeof(k_algos[0]));
    return 1;
}

